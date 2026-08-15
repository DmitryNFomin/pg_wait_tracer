#!/usr/bin/env python3
"""Stages 3/4 exact-probe lifecycle integration test.

The old version of this test required the query-id and activity uprobes to
fire continuously in tiered mode. Stage 3 intentionally reverses that policy
on validated layouts: PG14-18 sample the coherent PgBackendStatus query id,
while PG13 samples normalized activity text. Both per-query uprobes must be
detached and their firing counters must stay exactly zero until exact capture.

This test proves both sides of the contract:

  * validated PG13-18: detached/zero in sampled operation, atomically attached
    during each exact generation, and detached again after de-escalation;
  * a degraded layout promoted during startup warmup may have historical fire
    counts, but reconciliation detaches both probes before sampled operation;
  * degraded layouts retain the legacy pinned pair fail-safe;
  * commands already running at escalation and backends that fork/exit during
    the window do not break generation seeding or lifecycle cleanup;
  * repeated windows do not leak links or preseeds;
  * a forced second-link attach failure rolls the first link back and denies
    the window without ever advertising partial exact capture.

Usage: sudo python3 tests/test_uprobe_fired.py [--pid PM_PID]
           [--pg-version N]
"""
import argparse
import json
import os
import shutil
import socket
import subprocess
import sys
import tempfile
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from testutil import find_postmaster

PROJECT_DIR = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
TRACER = os.path.join(PROJECT_DIR, "pg_wait_tracer")
ATTR_MASK = 0x3

tests_run = 0
tests_failed = 0


def check(cond, msg):
    global tests_run, tests_failed
    tests_run += 1
    if cond:
        print(f"  PASS: {msg}")
    else:
        tests_failed += 1
        print(f"  FAIL: {msg}")


def psql(sql, timeout=20):
    return subprocess.run(
        ["psql", "-U", "postgres", "-d", "postgres", "-tAc", sql],
        stdout=subprocess.DEVNULL, stderr=subprocess.PIPE, text=True,
        timeout=timeout)


def workload(n=8):
    for i in range(n):
        result = psql(
            f"SELECT {i} + count(*) FROM generate_series(1, 1000)")
        if result.returncode != 0:
            return False
    return True


def ctl(trace_dir, request):
    path = os.path.join(trace_dir, "pgwt.sock")
    client = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
    client.settimeout(5)
    try:
        client.connect(path)
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


def wait_for_socket(proc, trace_dir, timeout=8):
    path = os.path.join(trace_dir, "pgwt.sock")
    deadline = time.time() + timeout
    while time.time() < deadline:
        if os.path.exists(path):
            return True
        if proc.poll() is not None:
            return False
        time.sleep(0.05)
    return False


def start_tracer(pm_pid, fail_at=None):
    trace_dir = tempfile.mkdtemp(prefix="pgwt_exact_lifecycle_")
    os.chmod(trace_dir, 0o755)
    env = os.environ.copy()
    if fail_at is not None:
        env["PGWT_TEST_EXACT_PROBE_FAIL_AT"] = str(fail_at)
    proc = subprocess.Popen(
        [TRACER, "--mode", "tiered", "--pid", str(pm_pid),
         "--trace-dir", trace_dir, "--duration", "45", "--quiet",
         "--interval", "1", "--escalation-budget", "120",
         "--anomaly-aas-factor", "1000000"],
        stdout=subprocess.DEVNULL, stderr=subprocess.PIPE, env=env)
    return proc, trace_dir


def stop_tracer(proc, trace_dir):
    if proc.poll() is None:
        proc.terminate()
    try:
        _, stderr = proc.communicate(timeout=8)
    except subprocess.TimeoutExpired:
        proc.kill()
        _, stderr = proc.communicate(timeout=5)
    shutil.rmtree(trace_dir, ignore_errors=True)
    return stderr.decode("utf-8", errors="replace")


def exact_counts(status):
    return (int(status.get("exact_query_uprobe_fires_total", -1)),
            int(status.get("exact_activity_uprobe_fires_total", -1)))


def run_cycles(pm_pid, pg_major):
    proc, trace_dir = start_tracer(pm_pid)
    stderr = ""
    try:
        ready = wait_for_socket(proc, trace_dir)
        check(ready, "tiered tracer reaches its control socket")
        if not ready:
            return False, "", False

        ready_status = ctl(trace_dir, {"cmd": "status"})
        ready_counts = exact_counts(ready_status)
        promotion_path = bool(
            ready_status.get("exact_probe_warmup_reconciled", False))
        check(workload(), "baseline query workload completed")
        baseline = ctl(trace_dir, {"cmd": "status"})
        validation = baseline.get("pgbackend_layout_validation")
        validated_tick_layout = pg_major >= 13 and validation == "validated"
        expected_mask = 0 if validated_tick_layout else ATTR_MASK

        check(baseline.get("exact_probe_attached_mask") == expected_mask,
              f"baseline exact-probe policy is mask {expected_mask} "
              f"(PG{pg_major}, layout={validation})")
        check(baseline.get("exact_probe_generation") == 0,
              "sampled baseline has no exact generation")
        check(baseline.get("exact_cpu_active") is False,
              "sampled baseline bypasses exact sched_switch accounting")
        qfires, afires = exact_counts(baseline)
        if validated_tick_layout:
            if promotion_path:
                check((qfires, afires) == ready_counts,
                      "degraded-to-validated promotion fully reconciles "
                      "probes before sampled workload "
                      f"({ready_counts} -> {(qfires, afires)})")
            else:
                check(ready_counts == (0, 0) and
                      (qfires, afires) == (0, 0),
                      "validated sampled baseline has ZERO "
                      "query/activity uprobe firings")
            check(baseline.get("sampled_attr_source") == "pgbackend_status",
                  "validated sampled attribution remains on PgBackendStatus")
        else:
            check(qfires > 0 and afires > 0,
                  f"degraded fallback uprobes still fire "
                  f"(query={qfires}, activity={afires})")

        seen_generations = set()
        previous = (qfires, afires)
        for cycle in range(1, 4):
            # The command is in flight before attach, exercising the coherent
            # straddler seed and synthetic command-open boundary.
            straddler = subprocess.Popen(
                ["psql", "-U", "postgres", "-d", "postgres", "-tAc",
                 "SELECT pg_sleep(1.0), count(*) "
                 "FROM generate_series(1, 1000)"],
                stdout=subprocess.DEVNULL, stderr=subprocess.PIPE, text=True)
            time.sleep(0.20)

            response = ctl(trace_dir, {
                "cmd": "escalate", "duration_s": 8, "reason": "manual"})
            check(response.get("ok") is True,
                  f"cycle {cycle}: exact escalation granted")
            active = ctl(trace_dir, {"cmd": "metrics"})
            generation = int(active.get("exact_probe_generation", 0))
            check(active.get("tier") == "escalated" and
                  active.get("exact_probe_attached_mask") == ATTR_MASK and
                  active.get("exact_cpu_active") is True,
                  f"cycle {cycle}: both exact links attached before publish")
            check(generation > 0 and generation not in seen_generations,
                  f"cycle {cycle}: generation {generation} is fresh")
            seen_generations.add(generation)
            check(active.get("exact_preseed_entries", 0) > 0,
                  f"cycle {cycle}: coherent generation preseed is populated")

            # New psql connections cause backend fork/exit while the exact
            # generation is open. Both probes must fire in this interval.
            check(workload(4),
                  f"cycle {cycle}: fork/exit workload completed in window")
            straddler.wait(timeout=10)
            check(straddler.returncode == 0,
                  f"cycle {cycle}: mid-query straddler completed")
            active = ctl(trace_dir, {"cmd": "status"})
            current = exact_counts(active)
            check(current[0] > previous[0] and current[1] > previous[1],
                  f"cycle {cycle}: exact query/activity edges fired "
                  f"({previous} -> {current})")

            response = ctl(trace_dir, {"cmd": "deescalate"})
            check(response.get("ok") is True,
                  f"cycle {cycle}: de-escalation acknowledged")
            sampled = ctl(trace_dir, {"cmd": "metrics"})
            check(sampled.get("tier") == "sampled" and
                  sampled.get("exact_probe_attached_mask") == expected_mask and
                  sampled.get("exact_probe_generation") == 0 and
                  sampled.get("exact_cpu_active") is False,
                  f"cycle {cycle}: sampled link policy restored")
            check(sampled.get("exact_preseed_entries") == 0,
                  f"cycle {cycle}: generation preseed fully cleared")

            after_detach = exact_counts(sampled)
            check(workload(3),
                  f"cycle {cycle}: post-window sampled workload completed")
            time.sleep(0.15)
            after_work = exact_counts(ctl(trace_dir, {"cmd": "status"}))
            if validated_tick_layout:
                check(after_work == after_detach,
                      f"cycle {cycle}: detached probes do not fire in sampled "
                      f"operation ({after_work})")
            else:
                check(after_work[0] > after_detach[0] and
                      after_work[1] > after_detach[1],
                      f"cycle {cycle}: degraded pinned probes keep firing")
            previous = after_work

        return validated_tick_layout, stderr, True
    finally:
        stderr = stop_tracer(proc, trace_dir)


def run_partial_failure(pm_pid):
    # Index 1 is activity, so query-id attaches first and must be rolled back.
    proc, trace_dir = start_tracer(pm_pid, fail_at=1)
    stderr = ""
    try:
        ready = wait_for_socket(proc, trace_dir)
        check(ready, "rollback tracer reaches sampled baseline")
        if not ready:
            return
        response = ctl(trace_dir, {
            "cmd": "escalate", "duration_s": 8, "reason": "manual"})
        check(response.get("ok") is False and
              "rolled back" in response.get("error", ""),
              "forced second-link failure denies exact escalation")
        metrics = ctl(trace_dir, {"cmd": "metrics"})
        check(metrics.get("tier") == "sampled" and
              metrics.get("exact_probe_attached_mask") == 0 and
              metrics.get("exact_probe_generation") == 0,
              "partial attach rolls back to an unadvertised sampled window")
        check(metrics.get("exact_preseed_entries") == 0,
              "failed attach creates no generation preseed")
        check(metrics.get("escalation_denied_total", 0) >= 1 and
              metrics.get("escalation_windows_total", 0) == 0,
              "failed attach is counted as denied, never as an exact window")
    finally:
        stderr = stop_tracer(proc, trace_dir)
        check("forcing exact-link attach failure" in stderr,
              "integration hook reached the second exact-link attach")


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--pid", type=int)
    parser.add_argument("--pg-version", type=int, default=0)
    # Kept as a compatibility no-op for callers of the pre-Stage-3 test.
    parser.add_argument("--bpftool", default=os.environ.get("BPFTOOL",
                                                            "bpftool"))
    args = parser.parse_args()

    if os.geteuid() != 0:
        print("ERROR: must run as root (sudo)")
        sys.exit(1)
    pm_pid = args.pid or find_postmaster()
    if not pm_pid:
        print("ERROR: cannot find postmaster")
        sys.exit(1)

    print(f"=== test_uprobe_fired Stages 3/4 lifecycle (postmaster {pm_pid}, "
          f"PG{args.pg_version or 'auto'}) ===")
    validated, _, completed = run_cycles(pm_pid, args.pg_version)
    if completed and validated:
        run_partial_failure(pm_pid)

    print(f"\n{tests_run - tests_failed}/{tests_run} checks passed")
    sys.exit(0 if tests_failed == 0 else 1)


if __name__ == "__main__":
    main()
