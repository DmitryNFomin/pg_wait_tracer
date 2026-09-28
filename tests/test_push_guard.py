#!/usr/bin/env python3
"""Exercise the PreToolUse hook with real JSON and disposable git worktrees."""

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


def run(command, cwd, skip=False):
    env = os.environ.copy()
    env.pop("PGWT_SKIP_PUSH_GUARD", None)
    if skip:
        env["PGWT_SKIP_PUSH_GUARD"] = "1"
    result = subprocess.run(
        ["bash", str(GUARD)], cwd=cwd, env=env, text=True,
        input=json.dumps({"tool_input": {"command": command}}),
        capture_output=True,
    )
    return result.returncode, result.stderr


def cases(main, worktree):
    q = shlex.quote(str(worktree))
    yield "bare clean push", "git push origin main", main, 0, False
    (main / "data.txt").write_text("changed\n")
    yield "bare stale push", "git push origin main", main, 2, False
    yield "clean worktree, dirty main", f"cd {q} && git push origin agent/test", main, 0, True
    stamp(main)
    (worktree / ".pgwt-check.stamp").unlink()
    yield "unstamped worktree, clean main", f"cd {q} && git push origin agent/test", main, 2, True
    stamp(worktree)
    (worktree / "data.txt").write_text("changed in worktree\n")
    yield "stale worktree, clean main", f"cd {q} && git push origin agent/test", main, 2, True
    yield "git -C worktree with options", f"git --no-pager -C {q} -c core.quotePath=false push origin agent/test", main, 2, True
    yield "unknown target", "cd /this/path/does/not/exist && git push origin main", main, 2, True
    yield "skip escape hatch", f"cd {q} && git push origin agent/test", main, 0, False
    yield "quoted phrase", "printf '%s' 'git push'", worktree, 0, True
    yield "heredoc phrase", "cat <<'EOF'\ngit push\nEOF", worktree, 0, True
    yield "substitution push", "echo \"$(git push)\"", worktree, 2, False
    yield "heredoc header push", "cat <<EOF && git push\ntext\nEOF", worktree, 2, False
    yield "unknown wrapper push", f"sudo git -C {q} push origin agent/test", main, 2, True
    yield "Git directory override", f"GIT_DIR={q}/.git git push origin main", main, 2, True
    yield "unrelated command", "printf hello", worktree, 0, False


def main():
    old = os.environ.get("PGWT_TEST_PUSH_GUARD") is not None
    failed = 0
    with tempfile.TemporaryDirectory(prefix="pgwt-push-guard-") as tmp:
        repo, worktree = fixture(Path(tmp))
        stamp(repo)
        stamp(worktree)
        for name, command, cwd, expected, should_red in cases(repo, worktree):
            code, error = run(command, cwd, name == "skip escape hatch")
            want = (code != expected) if old and should_red else (code == expected)
            print(f"  {'PASS' if want else 'FAIL'}: {name} (exit {code}, fixed expectation {expected})")
            if not want:
                print(f"    stderr: {error.strip()}")
                failed += 1
    mode = "original hook red cases" if old else "fixed hook"
    print(f"push guard {mode}: {15 - failed}/15 passed")
    return 1 if failed else 0


if __name__ == "__main__":
    raise SystemExit(main())
