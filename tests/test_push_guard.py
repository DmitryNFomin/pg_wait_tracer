#!/usr/bin/env python3
"""Exercise the PreToolUse hook with real JSON and disposable git worktrees.

Run normally for green. Set PGWT_TEST_RED_BASE to 5233266 or d12ffee to
materialize that hook from local history and verify the named red cases.
PGWT_TEST_PUSH_GUARD can instead supply a hook path for the same red mode.
"""

import json
import os
from pathlib import Path
import shlex
import shutil
import subprocess
import tempfile


SOURCE = Path(__file__).resolve().parents[1]
GUARD = Path(os.environ.get("PGWT_TEST_PUSH_GUARD", SOURCE / "scripts/hooks/push-guard.sh"))
HASH = SOURCE / "scripts/tree-hash.sh"
RED_523 = {
    "commit and push chain", "true and push", "case without push",
    "cd then command then push", "git status then case without push",
    "git status and push", "if clean push", "for clean push",
    "unclassifiable nonpush operator",
    "malformed nonpush quote",
    "quoted punctuation path push",
}
RED_D12 = {
    "substitution adjacent semicolon", "substitution adjacent and",
    "substitution adjacent or", "substitution adjacent pipe",
    "multiple glued operators", "glued close and",
    "if clean push", "for clean push",
    "unclassifiable push operator",
    "malformed nonpush quote",
    "quoted punctuation path push",
}


def git(repo, *args):
    subprocess.run(["git", "-C", str(repo), *args], check=True, stdout=subprocess.DEVNULL)


def fixture(parent):
    main = parent / "main"
    worktree = parent / "agent worktree"
    (main / "scripts").mkdir(parents=True)
    shutil.copy2(HASH, main / "scripts/tree-hash.sh")
    (main / ".gitignore").write_text(".pgwt-check.stamp\n")
    (main / "data.txt").write_text("original\n")
    git(main, "init", "-q", "-b", "main")
    git(main, "config", "user.email", "test@example.com")
    git(main, "config", "user.name", "Test")
    git(main, "add", "-A")
    git(main, "commit", "-q", "-m", "initial")
    git(main, "worktree", "add", "-q", "-b", "agent/test", str(worktree))
    return main, worktree


def stamp(repo):
    digest = subprocess.check_output([str(repo / "scripts/tree-hash.sh")], cwd=repo, text=True)
    (repo / ".pgwt-check.stamp").write_text(digest)


def run(command, cwd, guard, skip=False):
    env = os.environ.copy()
    env.pop("PGWT_SKIP_PUSH_GUARD", None)
    if skip:
        env["PGWT_SKIP_PUSH_GUARD"] = "1"
    result = subprocess.run(
        ["bash", str(guard)], cwd=cwd, env=env, text=True,
        input=json.dumps({"tool_input": {"command": command}}),
        capture_output=True,
    )
    return result.returncode, result.stderr


def cases(main, worktree):
    q = shlex.quote(str(worktree))
    yield "bare clean push", "git push origin main", main, 0
    yield "commit and push chain", 'git add -A && git commit -m "msg" && git push origin main', main, 0
    yield "true and push", "true && git push origin main", main, 0
    yield "git status and push", "git status && git push origin main", main, 0
    yield "case without push", 'case "$x" in a) true;; esac', main, 0
    yield "git status then case without push", 'git status && case "$x" in a) true;; esac', main, 0
    yield "cd then command then push", f"cd {q} && true && git push origin agent/test", main, 0
    yield "env then push", "env X=1 git push origin main", main, 0
    yield "push after semicolon", "true; git push origin main", main, 0
    yield "if clean push", "if true; then git push origin main; fi", main, 0
    yield "for clean push", "for x in a; do git push origin main; done", main, 0
    yield "unclassifiable push operator", "git push origin main >>> output", main, 2
    yield "unclassifiable nonpush operator", "git status >>> output", main, 0
    yield "quoted punctuation path push", "git -C '&&' push origin main", main, 2
    yield "malformed nonpush quote", "printf 'unfinished", main, 0
    yield "shell c clean push", "bash -c 'git push origin main'", main, 0
    yield "eval clean push", "eval 'git push origin main'", main, 0
    (main / "data.txt").write_text("changed\n")
    yield "bare stale push", "git push origin main", main, 2
    yield "clean worktree, dirty main", f"cd {q} && git push origin agent/test", main, 0
    stamp(main)
    (worktree / ".pgwt-check.stamp").unlink()
    yield "unstamped worktree, clean main", f"cd {q} && git push origin agent/test", main, 2
    (worktree / "data.txt").write_text("dirty and unstamped\n")
    yield "cd then command then dirty push", f"cd {q} && true && git push origin agent/test", main, 2
    yield "substitution adjacent semicolon", f"x=$(pwd);cd {q} && git push origin agent/test", main, 2
    yield "substitution adjacent and", f"x=$(pwd)&&cd {q} && git push origin agent/test", main, 2
    yield "substitution adjacent or", f"x=$(pwd)||cd {q} && git push origin agent/test", main, 2
    yield "substitution adjacent pipe", f"x=$(pwd)|cd {q} && git push origin agent/test", main, 2
    yield "multiple glued operators", f"x=$(pwd);(cd {q} && git push origin agent/test)", main, 2
    yield "glued close and", f"(cd {q})&&git push origin main", main, 2
    stamp(worktree)
    (worktree / "data.txt").write_text("changed in worktree\n")
    yield "stale worktree, clean main", f"cd {q} && git push origin agent/test", main, 2
    yield "git -C worktree with options", f"git --no-pager -C {q} -c core.quotePath=false push origin agent/test", main, 2
    yield "unknown target", "cd /this/path/does/not/exist && git push origin main", main, 2
    yield "skip escape hatch", f"cd {q} && git push origin agent/test", main, 0
    yield "quoted phrase", "printf '%s' 'git push'", worktree, 0
    yield "heredoc phrase", "cat <<'EOF'\ngit push\nEOF", worktree, 0
    yield "substitution push", "echo \"$(git push)\"", worktree, 2
    yield "heredoc header push", "cat <<EOF && git push\ntext\nEOF", worktree, 2
    yield "unknown wrapper push", f"sudo git -C {q} push origin agent/test", main, 2
    yield "if push", "if true; then git push origin agent/test; fi", worktree, 2
    yield "for push", "for x in a; do git push origin agent/test; done", worktree, 2
    yield "Git directory override", f"GIT_DIR={q}/.git git push origin main", main, 2
    yield "unrelated command", "printf hello", worktree, 0


def main():
    failed = 0
    selected = os.environ.get("PGWT_TEST_CASE_FILTER")
    red_base = os.environ.get("PGWT_TEST_RED_BASE")
    if os.environ.get("PGWT_TEST_PUSH_GUARD") and not red_base:
        red_base = "5233266"
    if red_base not in {None, "5233266", "d12ffee"}:
        raise ValueError("PGWT_TEST_RED_BASE must be 5233266 or d12ffee")
    count = 0
    with tempfile.TemporaryDirectory(prefix="pgwt-push-guard-") as tmp:
        guard = GUARD
        if red_base and not os.environ.get("PGWT_TEST_PUSH_GUARD"):
            hook_dir = Path(tmp) / "hook"
            hook_dir.mkdir()
            for filename in ("push-guard.sh", "push-guard-target.py"):
                content = subprocess.check_output(
                    ["git", "show", f"{red_base}:scripts/hooks/{filename}"],
                    cwd=SOURCE,
                )
                (hook_dir / filename).write_bytes(content)
            guard = hook_dir / "push-guard.sh"
        repo, worktree = fixture(Path(tmp))
        stamp(repo)
        stamp(worktree)
        for name, command, cwd, expected in cases(repo, worktree):
            if selected and name not in selected.split(","):
                continue
            count += 1
            code, error = run(command, cwd, guard, name == "skip escape hatch")
            should_red = name in (RED_523 if red_base == "5233266" else RED_D12)
            want = code != expected if red_base and should_red else code == expected
            print(f"  {'PASS' if want else 'FAIL'}: {name} (exit {code}, fixed expectation {expected})")
            if not want:
                print(f"    stderr: {error.strip()}")
                failed += 1
    mode = f"red against {red_base}" if red_base else "fixed"
    print(f"push guard {mode}: {count - failed}/{count} passed")
    return 1 if failed else 0


if __name__ == "__main__":
    raise SystemExit(main())
