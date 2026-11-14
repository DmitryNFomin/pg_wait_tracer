#!/usr/bin/env python3
"""Migration differential, deterministic retained-history work, and timing report.

This checks compatibility, not numerical truth; frozen merge-v1 rows remain the
authority. All build products (including the base source) live outside the tree.
CC/CFLAGS select the compiler and sanitizers for both implementations.
"""
import hashlib
import os
from pathlib import Path
import re
import shlex
import subprocess
import sys
import tempfile

sys.dont_write_bytecode = True
ROOT = Path(__file__).resolve().parents[1]
BASE = ROOT / "tests/fixtures/merge-a95a214c.c"
BASE_SHA256 = "4edd042846c195dd32681a70880730ec49d3228e36304e5d68959dedc3dfb14c"
EXPORTS = ("create", "destroy", "feed", "get_report", "get_epoch_report", "reason_name")


def run(command, **kwargs):
    env = dict(os.environ)
    env.setdefault("ASAN_OPTIONS", "halt_on_error=1:exitcode=86")
    env.setdefault("UBSAN_OPTIONS", "halt_on_error=1:exitcode=87:print_stacktrace=1")
    kwargs.setdefault("env", env)
    result = subprocess.run(command, cwd=ROOT, text=True, capture_output=True,
                            timeout=120, **kwargs)
    if result.returncode or re.search(
            r"AddressSanitizer|UndefinedBehaviorSanitizer|runtime error:", result.stdout + result.stderr):
        raise RuntimeError(f"{' '.join(map(str, command))}: exit {result.returncode}\n"
                           + result.stdout + result.stderr)
    return result.stdout


def flags():
    return (shlex.split(os.environ.get("CC", "cc")) +
            ["-std=c11", "-O2", "-g", "-Wall", "-Wextra", "-Werror",
             "-I" + str(ROOT / "src/core")] + shlex.split(os.environ.get("CFLAGS", "")))


def build_base(work):
    # Byte-for-byte `git show a95a214c:src/core/merge.c`, retained so shallow
    # checkouts can run the same migration check without fetching history.
    if hashlib.sha256(BASE.read_bytes()).hexdigest() != BASE_SHA256:
        raise RuntimeError("pinned base implementation checksum mismatch")
    obj = work / "base.o"
    run(flags() + [f"-Dpgwt_merge_{name}=old_pgwt_merge_{name}" for name in EXPORTS] +
        ["-c", str(BASE), "-o", str(obj)])
    return obj


def build(work, base, source, *, instrument=False):
    binary = work / "index-check"
    instrumentation = (["-DPGWT_MERGE_TEST_WORK", "-I" + str(ROOT / "tests")]
                       if instrument else [])
    run(flags() + instrumentation + [str(ROOT / "tests/core_merge_index_cases.c"), str(source), str(base),
                   "-o", str(binary)])
    return binary


def verify_instrumentation(work):
    obj = work / "merge-production.o"
    run(flags() + ["-c", str(ROOT / "src/core/merge.c"), "-o", str(obj)])
    symbols = run(["nm", str(obj)])
    if "pgwt_merge_test_work" in symbols:
        raise RuntimeError("production object contains the work hook")
    # A positive control prevents a broken symbol probe from passing vacuously.
    run(flags() + ["-DPGWT_MERGE_TEST_WORK", "-I" + str(ROOT / "tests"),
                   "-c", str(ROOT / "src/core/merge.c"), "-o", str(obj)])
    symbols = run(["nm", str(obj)])
    if not re.search(r"\bU _?pgwt_merge_test_work$", symbols, re.M):
        raise RuntimeError("instrumented object lacks the work hook")
    print("nm: production object has no pgwt_merge_test_work; "
          "instrumented object has U pgwt_merge_test_work", flush=True)


def main():
    with tempfile.TemporaryDirectory(prefix="pgwt-merge-index-") as directory:
        work = Path(directory)
        verify_instrumentation(work)
        base = build_base(work)
        binary = build(work, base, ROOT / "src/core/merge.c")
        print(run([str(binary)]), end="", flush=True)
        print(run([str(binary), "--cost"]), end="", flush=True)
        binary = build(work, base, ROOT / "src/core/merge.c", instrument=True)
        print(run([str(binary), "--work"]), end="", flush=True)


if __name__ == "__main__":
    try:
        main()
    except (OSError, RuntimeError, subprocess.TimeoutExpired) as error:
        sys.exit(str(error))
