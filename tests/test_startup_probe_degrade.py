#!/usr/bin/env python3
"""PG13/no-pgss sampled-synthetic and exact-probe degradation regression.

Run against a PostgreSQL 13 postmaster whose shared_preload_libraries does not
contain pg_stat_statements. Tiered mode must use its validated synthetic path
with no sampled attribution probes. Full mode drops the unavailable numeric
query probe but keeps wait capture; a forced activity-link failure also remains
wait-only instead of aborting the daemon.

Usage: sudo PGPORT=55413 python3 tests/test_startup_probe_degrade.py \
           --pid POSTMASTER_PID --port 55413
"""
import argparse
import json
import os
import re
import shutil
import socket
import subprocess
import sys
import tempfile
import time


PROJECT_DIR = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
TRACER = os.path.join(PROJECT_DIR, "pg_wait_tracer")
ACTIVITY_MASK = 1 << 1
STRIP_ANSI = re.compile(r"\x1b\[[0-9;]*[a-zA-Z]")

tests_run = 0
tests_failed = 0


def check(cond, message):
    global tests_run, tests_failed
    tests_run += 1
    if cond:
        print(f"  PASS: {message}")
    else:
        tests_failed += 1
        print(f"  FAIL: {message}")


def psql_argv(port):
    return ["psql", "-p", str(port), "-U", "postgres", "-d", "postgres"]


def psql(port, sql, timeout=15):
    return subprocess.run(
        psql_argv(port) + ["-tAc", sql], capture_output=True, text=True,
        timeout=timeout)


def wait_for_socket(proc, trace_dir, timeout=10):
    path = os.path.join(trace_dir, "pgwt.sock")
    deadline = time.time() + timeout
    while time.time() < deadline:
        if os.path.exists(path):
            return True
        if proc.poll() is not None:
            return False
        time.sleep(0.05)
    return False


def ctl(trace_dir, request):
    client = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
    client.settimeout(5)
    try:
        client.connect(os.path.join(trace_dir, "pgwt.sock"))
        client.sendall((json.dumps(request) + "\n").encode())
        data = b""
        while b"\n" not in data:
            chunk = client.recv(65536)
            if not chunk:
                break
            data += chunk
        return json.loads(data.splitlines()[0])
    finally:
        client.close()


def run_case(pm_pid, port, mode, fail_activity=False):
    label = f"{mode}{'/forced-activity-failure' if fail_activity else ''}"
    print(f"--- {label} ---")
    trace_dir = tempfile.mkdtemp(prefix="pgwt_startup_degrade_")
    os.chmod(trace_dir, 0o755)
    session = subprocess.Popen(
        psql_argv(port), stdin=subprocess.PIPE, stdout=subprocess.DEVNULL,
        stderr=subprocess.PIPE, text=True)
    env = os.environ.copy()
    if fail_activity:
        env["PGWT_TEST_EXACT_PROBE_FAIL_AT"] = "1"
    argv = [
        TRACER, "--mode", mode, "--pid", str(pm_pid),
        "--trace-dir", trace_dir, "--duration", "9", "--interval", "2",
    ]
    if mode == "tiered":
        argv += ["--anomaly-aas-factor", "1000000"]
    tracer = subprocess.Popen(
        argv, stdout=subprocess.PIPE, stderr=subprocess.PIPE, env=env)
    status = {}
    stdout = stderr = b""
    try:
        ready = wait_for_socket(tracer, trace_dir)
        check(ready, f"{label}: daemon reaches the control socket")
        if ready:
            status = ctl(trace_dir, {"cmd": "status"})
            expected_mask = (0 if mode == "tiered" or fail_activity
                             else ACTIVITY_MASK)
            check(status.get("exact_probe_attached_mask") == expected_mask,
                  f"{label}: unavailable startup probes are dropped "
                  f"(mask={expected_mask})")

            session.stdin.write("SELECT pg_sleep(3);\n\\q\n")
            session.stdin.flush()
            session.wait(timeout=10)
        stdout, stderr = tracer.communicate(timeout=20)
    except subprocess.TimeoutExpired:
        tracer.kill()
        stdout, stderr = tracer.communicate(timeout=5)
        check(False, f"{label}: workload and tracer finish within timeout")
    finally:
        if session.poll() is None:
            session.terminate()
            try:
                session.wait(timeout=5)
            except subprocess.TimeoutExpired:
                session.kill()
        shutil.rmtree(trace_dir, ignore_errors=True)

    out = STRIP_ANSI.sub("", stdout.decode("utf-8", errors="replace"))
    err = stderr.decode("utf-8", errors="replace")
    check(tracer.returncode == 0,
          f"{label}: daemon exits normally (rc={tracer.returncode})")
    if mode == "full":
        check("startup exact query-id probe unavailable" in err and
              "query attribution unavailable" in err,
              f"{label}: missing PG13 exact query-id source is warned explicitly")
    else:
        check("sampled exact probes detached" in err,
              f"{label}: validated synthetic sampled route detaches probes")
    if fail_activity:
        check("forcing exact-link attach failure" in err and
              "startup exact activity probe unavailable" in err,
              f"{label}: forced startup attach failure is warned and dropped")
    check("pg_wait_tracer — Time Model" in out,
          f"{label}: default Time Model view runs")
    check("Timeout:PgSleep" in out,
          f"{label}: wait-only capture records Timeout:PgSleep")


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--pid", type=int, required=True)
    parser.add_argument("--port", type=int,
                        default=int(os.environ.get("PGPORT", "5432")))
    args = parser.parse_args()

    if os.geteuid() != 0:
        print("ERROR: must run as root")
        return 1
    exe = os.path.realpath(f"/proc/{args.pid}/exe")
    version = subprocess.run(
        [exe, "--version"], capture_output=True, text=True).stdout
    if " 13." not in version:
        print(f"ERROR: regression requires PostgreSQL 13 (got {version!r})")
        return 1
    preload_result = psql(args.port, "SHOW shared_preload_libraries")
    preload = preload_result.stdout.strip()
    if preload_result.returncode != 0:
        print(f"ERROR: cannot query PG13 cluster: {preload_result.stderr}")
        return 1
    if "pg_stat_statements" in preload:
        print("ERROR: regression requires pg_stat_statements absent from "
              f"shared_preload_libraries (got {preload!r})")
        return 1

    print(f"=== PG13 no-pgss startup degradation (pid={args.pid}, "
          f"port={args.port}, preload={preload!r}) ===")
    run_case(args.pid, args.port, "tiered")
    run_case(args.pid, args.port, "full")
    run_case(args.pid, args.port, "full", fail_activity=True)

    print(f"\n{tests_run - tests_failed}/{tests_run} checks passed")
    return 0 if tests_failed == 0 else 1


if __name__ == "__main__":
    sys.exit(main())
