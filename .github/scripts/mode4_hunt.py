#!/usr/bin/env python3
"""Repeat the full ci_smoke.sh sequence until mode 4 is reproduced."""

import argparse
import os
from pathlib import Path
import subprocess
import sys


REPO = Path(__file__).resolve().parents[2]
CI_SMOKE = REPO / "tests" / "ci_smoke.sh"
STRIKE_LOG = REPO / "mode4-strike.log"
SUMMARY_LOG = REPO / "mode4-hunt-summary.txt"
STRIKE_SIGNATURE = b"FAIL: pure-CPU straddle live view shows CPU* > 0"
STRAY_PROCESSES = ("pg_wait_tracer", "pgwt-server")


def sudo_env():
    """Return the environment assignments needed by the full smoke test."""
    assignments = [
        f"PATH={os.environ.get('PATH', '')}",
        "PGWT_DEBUG_DUMP_STATE=1",
    ]
    for name in ("BPFTOOL", "GITHUB_STEP_SUMMARY"):
        value = os.environ.get(name)
        if value:
            assignments.append(f"{name}={value}")
    return assignments


def run_full_sequence(pg_version):
    command = [
        "sudo", "env", *sudo_env(), str(CI_SMOKE),
        "--pg-version", pg_version,
    ]
    return subprocess.run(
        command,
        cwd=REPO,
        stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT,
        check=False,
    )


def kill_stray_processes():
    """Remove tracer processes without disturbing the PostgreSQL cluster."""
    for signal in ("-TERM", "-KILL"):
        for process_name in STRAY_PROCESSES:
            result = subprocess.run(
                ["sudo", "pkill", signal, "-x", process_name],
                stdout=subprocess.DEVNULL,
                stderr=subprocess.DEVNULL,
                check=False,
            )
            # pkill returns 1 when no matching process exists.
            if result.returncode not in (0, 1):
                raise RuntimeError(
                    f"could not clean up stray {process_name} process "
                    f"(pkill rc={result.returncode})")


def first_fail_line(output):
    for raw_line in output.splitlines():
        line = raw_line.decode("utf-8", errors="replace").strip()
        if "FAIL" in line:
            return line
    return "no FAIL line found"


def write_strike(output, verdict):
    STRIKE_LOG.write_bytes(output)
    SUMMARY_LOG.write_text(verdict + "\n", encoding="utf-8")
    print(verdict, flush=True)
    sys.stdout.buffer.write(output)
    if output and not output.endswith(b"\n"):
        sys.stdout.buffer.write(b"\n")
    sys.stdout.buffer.flush()


def write_not_reproduced(iterations):
    verdict = f"not reproduced in {iterations} full-sequence runs"
    STRIKE_LOG.write_text(verdict + "\n", encoding="utf-8")
    SUMMARY_LOG.write_text(verdict + "\n", encoding="utf-8")
    print(verdict)


def main(argv=None):
    parser = argparse.ArgumentParser()
    parser.add_argument("--iterations", type=int, required=True)
    parser.add_argument("--pg-version", required=True)
    args = parser.parse_args(argv)
    if args.iterations <= 0:
        parser.error("--iterations must be positive")

    for iteration in range(1, args.iterations + 1):
        result = run_full_sequence(args.pg_version)
        output = result.stdout or b""

        if STRIKE_SIGNATURE in output:
            verdict = (
                f"iteration {iteration}/{args.iterations}: STRIKE "
                "(pure-CPU straddle live CPU*=0)")
            write_strike(output, verdict)
            return 1

        if result.returncode:
            detail = first_fail_line(output)
            print(
                f"iteration {iteration}/{args.iterations}: "
                f"non-mode-4 failure ({detail})",
                flush=True,
            )
        else:
            print(f"iteration {iteration}/{args.iterations}: passed", flush=True)

        if iteration < args.iterations:
            kill_stray_processes()

    write_not_reproduced(args.iterations)
    return 0


if __name__ == "__main__":
    sys.exit(main())
