#!/usr/bin/env python3
"""Run on-disk sweep.sh and sweep-hook.sh with isolated PR/query fixtures.

Usage: python3 scripts/lead/selftest-pr-state.py
No network, live PR mutations, or writes to the lead's actual state files.
"""
import hashlib
import json
import os
from pathlib import Path
import re
import shutil
import subprocess
import tempfile


LEAD = Path(__file__).resolve().parent
ROOT = LEAD.parent.parent
SHA_A, SHA_B = "a" * 40, "b" * 40
DELTA = re.compile(r"^   FINDING: (?:NEW-PR|NEWLY-ARMED|TIP-MOVED|DISARMED|CLOSED) ", re.M)


def run(args, **kwargs):
    return subprocess.run(args, text=True, stdout=subprocess.PIPE,
                          stderr=subprocess.STDOUT, timeout=20, **kwargs)


def pr(n=711, armed=False, sha=SHA_A):
    return dict(number=n, headRefName=f"feature/{n}", headRefOid=sha,
                autoMergeRequest={"enabledAt": "2026-11-01T20:00:00Z"} if armed else None,
                closingIssuesReferences=[])


def main():
    with tempfile.TemporaryDirectory(prefix="sweep-pr-state-") as temp:
        t = Path(temp)
        bindir, outdir, hookdir = t / "bin", t / "state", t / "hook"
        for d in (bindir, outdir, hookdir):
            d.mkdir()
        fixture, calls = t / "prs.json", t / "calls.jsonl"
        baseline = outdir / ".sweep-pr-baseline.json"
        env = dict(os.environ, PATH=str(bindir) + os.pathsep + os.environ["PATH"],
                   PR_FIXTURE=str(fixture), PR_CALLS=str(calls))
        for key in ("PGWT_BOX", "PGWT_BOX_EL8"):
            env.pop(key, None)

        def stub(name, code):
            path = bindir / name
            path.write_text("#!/usr/bin/python3\n" + code)
            path.chmod(0o755)

        stub("git", '''import sys
a = sys.argv[1:]
if a[:1] == ["fetch"]: pass
elif a[:1] == ["rev-parse"]: print("12fd4e10")
elif a[:2] == ["worktree", "list"]: pass
elif a[:1] == ["branch"]: pass
else: raise SystemExit("unexpected git call: " + repr(a))
''')
        stub("gh", '''import json, os, sys
a = sys.argv[1:]
with open(os.environ["PR_CALLS"], "a") as f: f.write(json.dumps(a) + "\\n")
if a[:2] not in (["pr", "list"], ["issue", "list"]):
    raise SystemExit("unexpected API call: " + repr(a))
fields = a[a.index("--json") + 1].split(",")
state = a[a.index("--state") + 1]
if a[:2] == ["pr", "list"] and state == "open":
    data = open(os.environ["PR_FIXTURE"]).read()
    if "headRefOid" in fields and os.environ.get("PR_QUERY_FAIL"):
        print(data)  # valid stdout must still be rejected on nonzero exit
        raise SystemExit(1)
    print(data)
else: print("[]")
''')
        for name in ("security", "ssh", "pgrep", "find"):
            stub(name, "raise SystemExit(1)\n")
        # The production hook uses BSD stat; supply that contract on Linux too.
        stub("stat", '''import os, sys
assert sys.argv[1:3] == ["-f", "%m"], sys.argv
print(int(os.stat(sys.argv[3]).st_mtime))
''')
        shutil.copy2(LEAD / "sweep-hook.sh", hookdir / "sweep-hook.sh")

        def scan(rows, fail=False, raw=None):
            fixture.write_text(json.dumps(rows) if raw is None else raw)
            calls.write_text("")
            result = run(["bash", str(LEAD / "sweep.sh"), str(outdir)],
                         env=dict(env, PR_QUERY_FAIL="1" if fail else ""))
            assert result.returncode == 0, result.stdout
            queries = [json.loads(line) for line in calls.read_text().splitlines()]
            # The original sweep makes seven bulk calls, regardless of PR count.
            assert len(queries) == 7, queries
            assert all(q[:2] in (["pr", "list"], ["issue", "list"]) for q in queries)
            assert "autoMergeRequest" in queries[-1][-1], queries[-1]
            assert "-- [11] PR state changes" in result.stdout, result.stdout
            return result.stdout

        def deltas(output):
            return [line.strip() for line in output.splitlines() if DELTA.match(line)]

        # First-run control includes an already armed PR, to prove it is quiet too.
        first = scan([pr(armed=True)])
        assert "baseline recorded (1 open PRs); first scan" in first, first
        assert not deltas(first), first
        saved = json.loads(baseline.read_text())["prs"][0]
        assert saved == dict(number=711, headRefName="feature/711", headRefOid=SHA_A,
                             armed=True, state="OPEN"), saved
        baseline.unlink()
        scan([pr()])

        armed = scan([pr(armed=True)])
        assert len(deltas(armed)) == 1 and "NEWLY-ARMED #711" in armed, armed
        assert "can merge without further action" in armed, armed
        cache = hookdir / ".sweep-latest.txt"
        cache.write_text(armed)
        (hookdir / ".dispatch-queue").write_text("# fixture\nSTATUS OK\nCAP running=3 cap=3\n")

        def hook(active=False):
            return run(["bash", str(hookdir / "sweep-hook.sh")],
                       input=json.dumps(dict(stop_hook_active=active)), env=env)

        blocked = hook()
        assert blocked.returncode == 2 and "NEWLY-ARMED #711" in blocked.stdout, blocked.stdout
        assert "items lacking a disposition" in blocked.stdout, blocked.stdout
        assert hook(True).returncode == 0, "one-bounce behaviour changed"
        text = deltas(armed)[0].removeprefix("FINDING: ")
        finding_id = hashlib.sha256(text.encode()).hexdigest()[:10]
        for disposition in ("ACTED", "MY-CHOICE", "UNKNOWN"):
            (hookdir / ".dispositions").write_text(f"{disposition} fixture [{finding_id}]\n")
            accepted = hook()
            assert accepted.returncode == 0, accepted.stdout
        print("1. " + deltas(armed)[0] + "; hook exit=2 without disposition; ACTED/MY-CHOICE/UNKNOWN exit=0")

        moved = scan([pr(armed=True, sha=SHA_B)])
        assert deltas(moved) == [f"FINDING: TIP-MOVED #711 {SHA_A} -> {SHA_B}"], moved
        print("2. " + deltas(moved)[0])
        opened = scan([pr(armed=True, sha=SHA_B), pr(716)])
        assert deltas(opened) == ["FINDING: NEW-PR #716 feature/716"], opened
        print("3. " + deltas(opened)[0])
        quiet = scan([pr(armed=True, sha=SHA_B), pr(716)])
        assert not deltas(quiet), quiet
        print("4. No changes: 0 PR delta findings")
        print("5. " + next(line.strip() for line in first.splitlines() if "baseline recorded" in line))

        good = baseline.read_bytes()
        failed = scan([], fail=True)
        assert "UNKNOWN: PR state changes unavailable" in failed, failed
        assert not deltas(failed) and baseline.read_bytes() == good, failed
        print("6. " + next(line.strip() for line in failed.splitlines()
                          if "UNKNOWN: PR state changes unavailable" in line))

        # Failure must not advance the baseline: a recovered tip change survives.
        recovered = scan([pr(), pr(716)])
        assert "DISARMED #711" in recovered and f"TIP-MOVED #711 {SHA_B} -> {SHA_A}" in recovered
        closed = scan([pr(716)])
        assert len(deltas(closed)) == 1 and "CLOSED #711" in closed, closed
        empty = scan([])
        assert len(deltas(empty)) == 1 and "CLOSED #716" in empty, empty
        assert not deltas(scan([])), "empty-to-empty snapshot must be quiet"
        both = scan([pr(armed=True)])
        assert len(deltas(both)) == 2 and "NEW-PR #711" in both and "NEWLY-ARMED #711" in both
        good = baseline.read_bytes()
        missing_armed = pr()
        del missing_armed["autoMergeRequest"]
        for rows, raw in (([], "{"), ([], "null"), ([missing_armed], None),
                          ([pr(), pr()], None), ([pr(n) for n in range(1, 101)], None)):
            output = scan(rows, raw=raw)
            assert "UNKNOWN: PR state changes unavailable" in output, output
            assert not deltas(output) and baseline.read_bytes() == good, output
        baseline.write_text("{")
        corrupt = scan([])
        assert "UNKNOWN: PR state baseline unreadable" in corrupt, corrupt
        assert not deltas(corrupt) and baseline.read_text() == "{", corrupt
        baseline.unlink()
        baseline.mkdir()  # exists but cannot be read as a file; never reset silently
        unreadable = scan([])
        assert "UNKNOWN: PR state baseline unreadable" in unreadable, unreadable
        baseline.rmdir()
        (outdir / ".sweep-pr-baseline.json.tmp").mkdir()
        unwritable = scan([])
        assert "UNKNOWN: PR state baseline could not be recorded" in unwritable, unwritable
        assert "OK: PR state baseline recorded" not in unwritable, unwritable
        print("ok: DISARMED, CLOSED, armed arrival, empty sets, failure recovery, malformed/missing/duplicate/capped data, baseline read/write failures")

        # Exercise the real tree-hash algorithm with the shipped ignore rules.
        mini = t / "hash-repo"
        (mini / "scripts/lead").mkdir(parents=True)
        shutil.copy2(LEAD / ".gitignore", mini / "scripts/lead/.gitignore")
        shutil.copy2(ROOT / "scripts/tree-hash.sh", mini / "scripts/tree-hash.sh")
        for args in (["git", "init", "-q", str(mini)], ["git", "-C", str(mini), "add", "."]):
            result = run(args)
            assert result.returncode == 0, result.stdout
        before = run(["bash", str(mini / "scripts/tree-hash.sh")])
        assert before.returncode == 0, before.stdout
        for suffix in ("", ".tmp"):
            (mini / ("scripts/lead/.sweep-pr-baseline.json" + suffix)).write_text("{}\n")
        after = run(["bash", str(mini / "scripts/tree-hash.sh")])
        assert after.returncode == 0 and before.stdout == after.stdout, after.stdout
        print("ok: baseline and atomic-write temporary file leave tree-hash unchanged; exactly seven bulk API calls per scan")


if __name__ == "__main__":
    main()
