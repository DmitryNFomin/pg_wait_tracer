#!/usr/bin/env python3
"""Execute check.sh's actual stamp writer without running the full suite."""
import os
from pathlib import Path
import subprocess
import tempfile


def main():
    source = (Path(__file__).resolve().parent.parent / "scripts/check.sh").read_text()
    writer = source.split("# Stamp the exact tree (tracked + untracked, minus ignored) that passed.\n")[1]
    with tempfile.TemporaryDirectory(prefix="pgwt-check-stamp-") as directory:
        root = Path(directory)
        (root / "scripts").mkdir()
        hasher = root / "scripts/tree-hash.sh"
        for fast, mode in [(1, "fast"), (0, "full")]:
            hasher.write_text("#!/bin/sh\necho test-tree-hash\n")
            hasher.chmod(0o755)
            result = subprocess.run(["bash", "-uo", "pipefail", "-c", writer], cwd=root,
                                    env={**os.environ, "FAST": str(fast)}, capture_output=True, text=True)
            assert result.returncode == 0, result
            assert (root / ".pgwt-check.stamp").read_text() == f"test-tree-hash {mode}\n"
            assert f"CHECK PASSED ({mode})" in result.stdout, result
            print(f"stamp writer {mode}: PASS (hash, mode and own verdict)")
        hasher.write_text("#!/bin/sh\necho partial-hash\nexit 9\n")
        result = subprocess.run(["bash", "-uo", "pipefail", "-c", writer], cwd=root,
                                env={**os.environ, "FAST": "0"}, capture_output=True, text=True)
        assert result.returncode == 1 and not (root / ".pgwt-check.stamp").exists(), result
        print("stamp writer hash failure: PASS (red, stale stamp removed)")


if __name__ == "__main__":
    main()
