#!/usr/bin/env python3
"""Reproduce the AAS-1 anomaly-escalation miss on the 2-vCPU CI runner."""

import argparse
from contextlib import redirect_stderr, redirect_stdout
import io
import os
from pathlib import Path
import subprocess
import sys
import traceback


REPO = Path(__file__).resolve().parents[2]
TESTS = REPO / "tests"
sys.path.insert(0, str(TESTS))

import test_capture_smoke as t  # noqa: E402


CI_SMOKE = TESTS / "ci_smoke.sh"
STRIKE_LOG = REPO / "aas1-strike.log"
SUMMARY_LOG = REPO / "aas1-confirm-summary.txt"
FULL_STDERR = REPO / ".aas1-storm-stderr.tmp"
STRIKE_PREFIX = "CPU storm triggered anomaly escalation"
STRAY_PROCESSES = ("pg_wait_tracer", "pgwt-server")


def checks_contain_strike(checks):
    """True only for the AAS-1 miss, not for another phase assertion."""
    return any(
        not passed
        and message.startswith(STRIKE_PREFIX)
        and "anomaly_fires_total=0" in message
        for passed, message in checks
    )


def output_contains_strike(output):
    """Recognize the same failed check in a full ci_smoke.sh transcript."""
    for raw_line in output.splitlines():
        line = raw_line.decode("utf-8", errors="replace")
        if ("FAIL: " + STRIKE_PREFIX) in line \
                and "anomaly_fires_total=0" in line:
            return True
    return False


def first_failed_check(checks):
    for passed, message in checks:
        if not passed:
            return message
    return "driver error before any check"


def first_fail_line(output):
    for raw_line in output.splitlines():
        line = raw_line.decode("utf-8", errors="replace").strip()
        if "FAIL" in line:
            return line
    return "no FAIL line found"


def write_strike(stderr, verdict):
    """Persist and print the first strike's complete tracer stderr."""
    if isinstance(stderr, str):
        stderr = stderr.encode("utf-8", errors="replace")
    STRIKE_LOG.write_bytes(stderr)
    SUMMARY_LOG.write_text(verdict + "\n", encoding="utf-8")
    print(verdict, flush=True)
    print("=== complete tracer stderr ===", flush=True)
    sys.stdout.buffer.write(stderr)
    if stderr and not stderr.endswith(b"\n"):
        sys.stdout.buffer.write(b"\n")
    sys.stdout.buffer.flush()


def write_not_reproduced(iterations):
    verdict = f"not reproduced in {iterations}"
    detail = (
        f"{verdict}\n"
        f"standalone phase: {iterations} iterations, no strike\n"
        f"full ci_smoke.sh fallback: {iterations} iterations, no strike\n"
    )
    STRIKE_LOG.write_text(detail, encoding="utf-8")
    SUMMARY_LOG.write_text(detail, encoding="utf-8")
    print(detail, end="")


def kill_stray_processes():
    """Remove test helpers without disturbing the PostgreSQL cluster."""
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


def run_standalone(pm_pid):
    """Run one imported storm phase and capture its checks + tracer stderr."""
    captured = {"stderr": b""}
    checks = []
    output = io.StringIO()
    original_popen = subprocess.Popen
    original_check = t.check

    class CapturingPopen(original_popen):
        def __init__(self, *popen_args, **popen_kwargs):
            command = (popen_args[0] if popen_args
                       else popen_kwargs.get("args", []))
            executable = (command[0]
                          if isinstance(command, (list, tuple)) and command
                          else command)
            self._aas1_tracer = (
                executable is not None
                and os.path.abspath(os.fspath(executable)) == t.TRACER)
            super().__init__(*popen_args, **popen_kwargs)

        def communicate(self, *comm_args, **comm_kwargs):
            stdout, stderr = super().communicate(*comm_args, **comm_kwargs)
            if self._aas1_tracer:
                captured["stderr"] = stderr or b""
            return stdout, stderr

    def capture_check(condition, message):
        checks.append((bool(condition), str(message)))
        original_check(condition, message)

    t.subprocess.Popen = CapturingPopen
    t.check = capture_check
    error = None
    try:
        with redirect_stdout(output), redirect_stderr(output):
            t.phase_cpu_storm_escalation(pm_pid)
    except Exception:
        error = traceback.format_exc()
    finally:
        t.subprocess.Popen = original_popen
        t.check = original_check

    return checks, captured["stderr"], output.getvalue(), error


def sudo_env(stderr_path):
    assignments = [
        f"PATH={os.environ.get('PATH', '')}",
        "PGWT_DEBUG_SAMPLER_TRACE=1",
        f"PGWT_AAS1_CAPTURE_STDERR={stderr_path}",
    ]
    for name in ("BPFTOOL", "GITHUB_STEP_SUMMARY"):
        value = os.environ.get(name)
        if value:
            assignments.append(f"{name}={value}")
    return assignments


def run_full_sequence(pg_version):
    command = [
        "sudo", "env", *sudo_env(FULL_STDERR), str(CI_SMOKE),
        "--pg-version", pg_version,
    ]
    return subprocess.run(
        command,
        cwd=REPO,
        stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT,
        check=False,
    )


def main(argv=None):
    parser = argparse.ArgumentParser()
    parser.add_argument("--iterations", type=int, required=True)
    parser.add_argument("--pg-version", required=True)
    args = parser.parse_args(argv)
    if args.iterations <= 0:
        parser.error("--iterations must be positive")

    t.TRACER = str(REPO / "pg_wait_tracer")
    t.SERVER = str(REPO / "pgwt-server")
    os.environ["PGWT_DEBUG_SAMPLER_TRACE"] = "1"
    pm_pid = t.find_postmaster(pg_version=int(args.pg_version))
    if not pm_pid:
        parser.error(
            f"cannot find PostgreSQL {args.pg_version} postmaster PID")

    print(f"method: standalone phase (postmaster PID {pm_pid})", flush=True)
    for iteration in range(1, args.iterations + 1):
        checks, stderr, _output, error = run_standalone(pm_pid)
        if checks_contain_strike(checks):
            verdict = (
                f"method=standalone iteration {iteration}/{args.iterations}: "
                "STRIKE (anomaly_fires_total=0)")
            write_strike(stderr, verdict)
            return 1

        if error or any(not passed for passed, _ in checks):
            detail = error.splitlines()[-1] if error else first_failed_check(checks)
            print(
                f"standalone iteration {iteration}/{args.iterations}: "
                f"other ({detail})",
                flush=True,
            )
        else:
            print(
                f"standalone iteration {iteration}/{args.iterations}: pass",
                flush=True,
            )
        kill_stray_processes()

    print(
        f"standalone did not reproduce in {args.iterations}; "
        "method: full ci_smoke.sh fallback",
        flush=True,
    )
    for iteration in range(1, args.iterations + 1):
        FULL_STDERR.unlink(missing_ok=True)
        result = run_full_sequence(args.pg_version)
        output = result.stdout or b""
        if output_contains_strike(output):
            if FULL_STDERR.exists():
                stderr = FULL_STDERR.read_bytes()
            else:
                stderr = (
                    b"AAS-1 strike detected, but the complete tracer stderr "
                    b"capture is missing. Full ci_smoke.sh output follows:\n" + output)
            verdict = (
                f"method=full-ci-smoke iteration "
                f"{iteration}/{args.iterations}: STRIKE "
                "(anomaly_fires_total=0)")
            write_strike(stderr, verdict)
            FULL_STDERR.unlink(missing_ok=True)
            return 1

        if result.returncode:
            print(
                f"full iteration {iteration}/{args.iterations}: "
                f"other ({first_fail_line(output)})",
                flush=True,
            )
        else:
            print(
                f"full iteration {iteration}/{args.iterations}: pass",
                flush=True,
            )
        FULL_STDERR.unlink(missing_ok=True)
        kill_stray_processes()

    write_not_reproduced(args.iterations)
    return 0


if __name__ == "__main__":
    sys.exit(main())
