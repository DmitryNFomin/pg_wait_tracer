#!/usr/bin/env python3
"""Exercise check.sh's stamp tail and deployed-hook migration in scratch repos."""
import os
from pathlib import Path
import shutil
import subprocess
import tempfile

REPO = Path(__file__).resolve().parent.parent


def main():
    source = Path(os.environ.get("PGWT_TEST_CHECK_SCRIPT", REPO / "scripts/check.sh")).read_text()
    tail = "if [[ $fail -ne 0 ]]; then\n" + source.rsplit("if [[ $fail -ne 0 ]]; then\n", 1)[1]
    # Use the deployed revision verbatim: reconstructing its stamp comparison
    # here would let this migration test agree with the same regression.
    old_hook = subprocess.check_output(
        ["git", "show", "4059fcde:scripts/git-hooks/pre-push"], cwd=REPO, text=True)
    env = {key: value for key, value in os.environ.items()
           if not key.startswith("GIT_") and key != "PGWT_SKIP_PUSH_GUARD"}
    env.update(GIT_CONFIG_NOSYSTEM="1", GIT_CONFIG_GLOBAL=os.devnull,
               GIT_AUTHOR_NAME="Test", GIT_AUTHOR_EMAIL="test@example.invalid",
               GIT_COMMITTER_NAME="Test", GIT_COMMITTER_EMAIL="test@example.invalid")
    with tempfile.TemporaryDirectory(prefix="pgwt-check-stamp-") as directory:
        root = Path(directory) / "tree"
        (root / "scripts").mkdir(parents=True)
        shutil.copyfile(REPO / ".gitignore", root / ".gitignore")
        hasher = root / "scripts/tree-hash.sh"
        shutil.copyfile(REPO / "scripts/tree-hash.sh", hasher)
        hasher.chmod(0o755)
        hooks = {}
        for name, text in [("deployed", old_hook),
                           ("new", (REPO / "scripts/git-hooks/pre-push").read_text())]:
            hook = Path(directory) / f"{name}-hook"
            hook.write_text(text)
            hooks[name] = hook

        def run(*args, **extra):
            return subprocess.run(args, cwd=root, capture_output=True,
                                  text=True, **{"env": env, **extra})

        def git(*args):
            result = run("git", *args)
            assert result.returncode == 0, result
            return result.stdout

        def write_stamp(fast=0, fail=0):
            return run("bash", "-uo", "pipefail", "-c", tail,
                       env={**env, "FAST": str(fast), "fail": str(fail)})

        git("init", "-q", "-b", "main")
        git("add", ".")
        git("commit", "-qm", "fixture")
        tree_hash = run("bash", "scripts/tree-hash.sh").stdout.strip()
        assert len(tree_hash) == 40, tree_hash
        stamp = root / ".pgwt-check.stamp"
        mode_file = root / ".pgwt-check.mode"
        for fast, mode in [(1, "fast"), (0, "full")]:
            result = write_stamp(fast)
            assert result.returncode == 0, result
            assert stamp.read_text() == f"{tree_hash}\n", "stamp must contain only tree hash"
            assert mode_file.read_text() == f"{mode}\n", "mode must be stored separately"
            assert f"CHECK PASSED ({mode})" in result.stdout, result
            for name, hook in hooks.items():
                result = run("bash", str(hook))
                assert result.returncode == 0, (name, mode, result)
            assert git("status", "--porcelain") == "", "check artifacts must be ignored"
            assert run("bash", "scripts/tree-hash.sh").stdout.strip() == tree_hash
            print(f"stamp writer {mode}: PASS (hash alone, separate mode, both hooks, clean status, unchanged hash)")

        mode_file.unlink()
        result = run("bash", str(hooks["new"]))
        assert result.returncode == 0, result
        print("new hook with legacy stamp and no mode: PASS")

        for failure in ("suite", "hash", "empty-hash", "mode-write", "stamp-write"):
            stamp.write_text("stale\n")
            mode_file.write_text("full\n")
            if failure == "hash":
                hasher.write_text("#!/bin/sh\necho partial-hash\nexit 9\n")
            elif failure == "empty-hash":
                hasher.write_text("#!/bin/sh\nexit 0\n")
            elif failure in ("mode-write", "stamp-write"):
                path = mode_file if failure == "mode-write" else stamp
                path.unlink()
                path.mkdir()
            result = write_stamp(fail=int(failure == "suite"))
            assert result.returncode == 1, (failure, result)
            assert not stamp.is_file() and not mode_file.is_file(), (failure, result)
            assert "CHECK PASSED" not in result.stdout, result
            for path in (stamp, mode_file):
                if path.is_dir():
                    path.rmdir()
            shutil.copyfile(REPO / "scripts/tree-hash.sh", hasher)
            print(f"stamp writer {failure} failure: PASS (red, stale stamp and mode removed)")


if __name__ == "__main__":
    main()
