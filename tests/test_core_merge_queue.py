#!/usr/bin/env python3
"""Large-backlog compatibility and deterministic queue-operation cost gates."""
import subprocess
import sys
import tempfile
from pathlib import Path

sys.dont_write_bytecode = True
import test_core_merge_index as checks


def build(work, base, source, instrument=False):
    binary = work / "queue-check"
    extra = ["-DPGWT_MERGE_TEST_WORK", "-I" + str(checks.ROOT / "tests")] if instrument else []
    checks.run(checks.flags() + extra + [str(checks.ROOT / "tests/core_merge_queue_cases.c"),
               str(source), str(base), "-o", str(binary)])
    return binary


def main():
    with tempfile.TemporaryDirectory(prefix="pgwt-merge-queue-") as directory:
        work = Path(directory)
        checks.verify_instrumentation(work)
        base = checks.build_base(work)
        binary = build(work, base, checks.ROOT / "src/core/merge.c")
        print(checks.run([str(binary)]), end="", flush=True)
        print(checks.run([str(binary), "--cost"]), end="", flush=True)
        binary = build(work, base, checks.ROOT / "src/core/merge.c", True)
        print(checks.run([str(binary), "--cost"]), end="", flush=True)


if __name__ == "__main__":
    try:
        main()
    except (OSError, RuntimeError, subprocess.TimeoutExpired) as error:
        sys.exit(str(error))
