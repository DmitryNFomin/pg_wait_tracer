#!/usr/bin/env python3
"""Exercise the on-disk audit harness with a PATH stub, never a real auditor.

Run directly, or pass --output-dir DIR to keep drafts, raw auditor output, and
console transcripts for review. No GitHub/Hetzner credentials or packages needed.
Only the final turn, delimited by a blank line followed by a codex role header,
contributes verdicts and tallies. Same-turn conflicting verdicts must fail
closed; cross-turn convergence must not. Without boundaries, use the whole file.
"""
import argparse
import hashlib
import os
from pathlib import Path
import subprocess
import tempfile


HARNESS = Path(__file__).resolve().with_name("audit-report.sh")
STUB = '''#!/usr/bin/env python3
import os
from pathlib import Path
import sys
assert len(sys.argv) == 3 and sys.argv[1] == "exec", sys.argv
assert sys.stdin.read() == ""
Path(os.environ["AUDIT_TEST_PROMPT"]).write_text(sys.argv[2])
sys.stdout.buffer.write(Path(os.environ["AUDIT_TEST_FIXTURE"]).read_bytes())
sys.stdout.buffer.flush()
sys.stderr.write(os.environ.get("AUDIT_TEST_STDERR", ""))
if os.environ.get("AUDIT_TEST_MUTATE"):
    Path(os.environ["AUDIT_TEST_DRAFT"]).write_text("Draft changed during audit.\\n")
sys.exit(int(os.environ["AUDIT_TEST_RC"]))
'''


def exercise(root):
    root = root.resolve()
    stubdir = root / "bin"
    stubdir.mkdir()
    stub = stubdir / "codex"
    stub.write_text(STUB)
    stub.chmod(0o755)
    assert os.access(HARNESS, os.X_OK), "harness must be executable"
    print(f"Stub: PATH prepends {stubdir}; codex exec emits a fixture and exit code.")

    def run(label, fixture, rc, expected, summary="0 verified, 0 wrong, 0 unverifiable",
            auditor_rc=0, stderr="", mutate=False, draft_text="The draft is ready for review.\n"):
        draft = root / f"{label}.md"
        draft.write_text(draft_text)
        digest = hashlib.sha256(draft.read_bytes()).hexdigest()[:16]
        saved = Path(f"{draft}.audit-{digest}.txt")
        fixture_path = root / f"{label}.fixture"
        fixture_path.write_text(fixture)
        prompt = root / f"{label}.prompt"
        # A stale SEND must never rescue a failing or empty fresh invocation.
        saved.write_text("VERDICT: SEND\n")
        env = dict(os.environ, PATH=f"{stubdir}{os.pathsep}{os.environ['PATH']}",
                   AUDIT_TEST_FIXTURE=str(fixture_path), AUDIT_TEST_PROMPT=str(prompt),
                   AUDIT_TEST_RC=str(auditor_rc), AUDIT_TEST_STDERR=stderr,
                   AUDIT_TEST_MUTATE="yes" if mutate else "", AUDIT_TEST_DRAFT=str(draft))
        result = subprocess.run([str(HARNESS), str(draft)], env=env,
                                capture_output=True, text=True, timeout=15)
        console = result.stdout + result.stderr
        (root / f"{label}.console").write_text(console)
        assert result.returncode == rc, (label, result.returncode, console)
        assert expected in console, (label, expected, console)
        if prompt.exists():
            assert saved.read_text() == fixture + stderr, (label, "raw output lost")
            assert f"Auditor output: {saved}" in console, (label, "path not printed")
            assert summary in console, (label, "wrong counts", console)
            assert f"draft={digest}" in console, (label, "hash not bound")
        print(f"{label} exit={result.returncode}")
        print(console.rstrip())
        return saved, console, prompt

    saved, console, prompt = run(
        "1-prompt-early-block", 'VERDICT: BLOCK   or   VERDICT: SEND\n' + 'context\n' * 30 +
        'The branch -> VERIFIED checked revision\nVERDICT: SEND\n', 0,
        "VERDICT SEND", "1 verified, 0 wrong, 0 unverifiable")
    assert "Last verdict: SEND (line 33 of " in console
    assert "VERDICT: BLOCK   or" not in console  # outside the console tail
    assert saved.read_text().splitlines()[0] == "VERDICT: BLOCK   or   VERDICT: SEND"
    print(f"saved first line: {saved.read_text().splitlines()[0]}")
    print(f"saved last line 33: {saved.read_text().splitlines()[-1]}")
    brief = prompt.read_text()
    for phrase in ("Be adversarial", "11 hours", "2-3x", "three absences",
                   "HTTP 401/403", "missing API token", "permission denial",
                   "hook firing", "process listing", "never for 'I did not look'"):
        assert phrase in brief, ("prompt contract", phrase)

    run("2-conflicting-verdicts", "VERDICT: BLOCK\nVERDICT: SEND\n", 1,
        "AUDIT AMBIGUOUS (conflicting SEND and BLOCK verdicts)")
    run("3-quoted-prompt-only", "VERDICT: SEND   or   VERDICT: BLOCK\n"
        "> VERDICT: BLOCK\n'VERDICT: SEND'\n", 2, "AUDIT MALFORMED (no VERDICT line)")
    run("4-access-only", "GitHub -> UNVERIFIABLE HTTP 401/403\n"
        "Hetzner -> UNVERIFIABLE missing API token\n"
        "Session -> UNVERIFIABLE cannot observe hook firing\nVERDICT: SEND\n",
        0, "VERDICT SEND", "0 verified, 0 wrong, 3 unverifiable")
    # SEND deliberately contradicts WRONG: the harness must enforce the block.
    run("5-wrong", "Count -> WRONG checked 4, draft says 9\nVERDICT: SEND\n", 1,
        "VERDICT BLOCK", "0 verified, 1 wrong, 0 unverifiable")
    run("6-no-verdict", "The branch -> VERIFIED checked revision\n", 2,
        "AUDIT MALFORMED (no VERDICT line)", "1 verified, 0 wrong, 0 unverifiable")
    saved, console, _ = run("7-auditor-failure", "VERDICT: SEND\n", 2,
                            "AUDIT INCONCLUSIVE (auditor exit 17)", auditor_rc=17,
                            stderr="auditor failed\n")
    assert saved.exists() and str(saved) in console
    print(f"8-output-retained: exists={saved.exists()}; printed path={saved}")

    run("9-block-alone", "VERDICT: BLOCK\n", 1, "VERDICT BLOCK")
    run("10-repeated-send", "VERDICT: SEND\nVERDICT: SEND\n", 0, "VERDICT SEND")
    run("11-empty-failed-auditor", "", 2,
        "AUDIT INCONCLUSIVE (auditor exit 9)", auditor_rc=9)
    run("12-mutated-draft", "VERDICT: SEND\n", 2,
        "AUDIT INCONCLUSIVE (draft changed during audit)", mutate=True)
    run("13-status-token-boundaries", "A -> VERIFIED says WRONG in quoted evidence\n"
        "B -> UNVERIFIABLE HTTP 403; cannot establish WRONG\n"
        "WRONGLY labelled prose\nVERDICT: SEND\n", 0, "VERDICT SEND",
        "1 verified, 0 wrong, 1 unverifiable")
    run("14-standalone-statuses", "VERIFIED checked revision\nWRONG contradicted\n"
        "UNVERIFIABLE HTTP 401\nVERDICT: SEND\n", 1, "VERDICT BLOCK",
        "1 verified, 1 wrong, 1 unverifiable")
    _, _, prompt = run("15-absence-prepass", "", 1, "VERDICT BLOCK",
                       draft_text="The feature does not exist.\n")
    assert not prompt.exists(), "absence pre-pass must block before codex"
    _, console, _ = run("16-cross-turn-convergence",
                        "A -> WRONG early finding, later corrected\nVERDICT: BLOCK\n"
                        "\ncodex\n"
                        "B -> VERIFIED checked again\nVERDICT: SEND\n", 0,
                        "VERDICT SEND", "1 verified, 0 wrong, 0 unverifiable")
    assert "1 wrong" not in console, ("stale WRONG survived turn boundary", console)
    run("17-codex-without-blank-line", "A -> WRONG x\ncodex\nVERDICT: SEND\n", 1,
        "VERDICT BLOCK", "0 verified, 1 wrong, 0 unverifiable")
    print("PASS: prompt-choice control 1, controls 2-8, and regression controls 9-17.")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--output-dir", type=Path, help="new directory to retain evidence")
    args = parser.parse_args()
    if args.output_dir:
        args.output_dir.mkdir(parents=True, exist_ok=False)
        exercise(args.output_dir)
    else:
        with tempfile.TemporaryDirectory(prefix="audit-report-selftest-") as temp:
            exercise(Path(temp))


if __name__ == "__main__":
    main()
