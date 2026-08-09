#!/usr/bin/env python3
"""Repeat the full-mode pure-CPU straddle phase until its live view misses."""

import argparse
import os
from pathlib import Path
import re
import subprocess
import sys
import traceback


REPO = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(REPO / "tests"))

import test_capture_smoke as t  # noqa: E402


PROC_CPU_RE = re.compile(r"/proc on-CPU ([0-9]+(?:\.[0-9]+)?)ms")


def write_failure(stderr, summary):
    strike_path = REPO / "mode4-strike.log"
    summary_path = REPO / "mode4-hunt-summary.txt"
    if isinstance(stderr, str):
        stderr = stderr.encode("utf-8", errors="replace")
    strike_path.write_bytes(stderr)
    summary_path.write_text(summary + "\n", encoding="utf-8")
    print(summary)
    print("=== complete tracer stderr ===")
    sys.stdout.write(stderr.decode("utf-8", errors="replace"))
    if stderr and not stderr.endswith(b"\n"):
        print()


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--iterations", type=int, required=True)
    parser.add_argument("--postmaster-pid-file", type=Path, required=True)
    args = parser.parse_args()
    if args.iterations <= 0:
        parser.error("--iterations must be positive")

    pm_pid = int(args.postmaster_pid_file.read_text(encoding="utf-8")
                 .splitlines()[0])
    t.TRACER = str(REPO / "pg_wait_tracer")
    t.SERVER = str(REPO / "pgwt-server")
    os.environ["PGWT_DEBUG_DUMP_STATE"] = "1"

    current = {"stderr": b""}
    iteration_checks = []
    original_popen = subprocess.Popen
    original_check = t.check

    class CapturingPopen(original_popen):
        def __init__(self, *popen_args, **popen_kwargs):
            command = (popen_args[0] if popen_args
                       else popen_kwargs.get("args", []))
            executable = command[0] if isinstance(command, (list, tuple)) \
                and command else command
            self._mode4_tracer = (
                executable is not None
                and os.path.abspath(os.fspath(executable)) == t.TRACER)
            super().__init__(*popen_args, **popen_kwargs)

        def communicate(self, *comm_args, **comm_kwargs):
            stdout, stderr = super().communicate(*comm_args, **comm_kwargs)
            if self._mode4_tracer:
                current["stderr"] = stderr or b""
            return stdout, stderr

    def capture_check(condition, message):
        iteration_checks.append((bool(condition), str(message)))
        original_check(condition, message)

    t.subprocess.Popen = CapturingPopen
    t.check = capture_check

    try:
        for iteration in range(1, args.iterations + 1):
            current["stderr"] = b""
            iteration_checks.clear()
            try:
                t.phase_pure_cpu_straddle(pm_pid, "full")
            except Exception:
                detail = traceback.format_exc()
                stderr = current["stderr"] + detail.encode("utf-8")
                write_failure(
                    stderr,
                    f"iteration {iteration}/{args.iterations}: driver error")
                return 1

            live_failed = any(
                not passed
                and message.startswith("pure-CPU straddle live view")
                for passed, message in iteration_checks)
            proc_cpu_ms = [
                float(match.group(1))
                for _, message in iteration_checks
                for match in [PROC_CPU_RE.search(message)]
                if match
            ]
            proc_burned = any(value > 0.0 for value in proc_cpu_ms)
            failed_checks = [
                message for passed, message in iteration_checks if not passed]

            if live_failed and proc_burned:
                summary = (
                    f"iteration {iteration}/{args.iterations}: REPRODUCED; "
                    f"live CPU*=0 while /proc CPU delta was "
                    f"{max(proc_cpu_ms):.0f}ms")
                write_failure(current["stderr"], summary)
                return 1

            if failed_checks:
                summary = (
                    f"iteration {iteration}/{args.iterations}: unexpected "
                    f"phase failure: {'; '.join(failed_checks)}")
                write_failure(current["stderr"], summary)
                return 1

            print(f"iteration {iteration}/{args.iterations}: passed")
    finally:
        t.subprocess.Popen = original_popen
        t.check = original_check

    summary = f"not reproduced in {args.iterations}"
    (REPO / "mode4-strike.log").write_text(summary + "\n", encoding="utf-8")
    (REPO / "mode4-hunt-summary.txt").write_text(
        summary + "\n", encoding="utf-8")
    print(summary)
    return 0


if __name__ == "__main__":
    sys.exit(main())
