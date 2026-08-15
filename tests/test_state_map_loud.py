#!/usr/bin/env python3
"""test_state_map_loud.py — CAP-1 (T4): a full BPF state_map must be LOUD.

Before T4, state_map had max_entries=512 (< MAX_BACKENDS < common
max_connections) and every insert past capacity failed with an UNCHECKED
return: backends beyond the map size never recorded a single event, with no
error anywhere. T4 sizes the map to MAX_BACKENDS, checks every insert, and
surfaces failures as `state_map_full_total` on the control socket plus a
loud ERROR log.

Proving the loud path must not require >1024 real connections on a CI
runner, so the daemon honors PGWT_STATE_MAP_ENTRIES (test hook) to shrink
the map before load. This test:

  1. runs the tracer in --mode tiered with PGWT_STATE_MAP_ENTRIES=4 and
     opens ~10 sessions -> asserts metrics.state_map_full_total > 0 and the
     ERROR log line fired (the loud path works);
  2. runs the tracer at DEFAULT sizing with the same sessions -> asserts
     state_map_full_total == 0 (no false positives at real capacity).

Also asserts the T4 health/observability fields exist in `status` and
`metrics` (sampler_healthy, invalid_wait_reads_total, ...) — the daemon side
of what tests/mock_server.py mirrors.

Usage: sudo python3 tests/test_state_map_loud.py [--pid POSTMASTER_PID]
"""
import argparse
import json
import os
import socket
import subprocess
import sys
import tempfile
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from testutil import find_postmaster

PROJECT_DIR = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
TRACER = os.path.join(PROJECT_DIR, "pg_wait_tracer")

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


def ctl(sock_path, cmd, timeout=5):
    """One JSON-line request against the daemon control socket."""
    s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
    s.settimeout(timeout)
    try:
        s.connect(sock_path)
        s.sendall((json.dumps(cmd) + "\n").encode())
        data = b""
        while not data.endswith(b"\n"):
            chunk = s.recv(65536)
            if not chunk:
                break
            data += chunk
    finally:
        s.close()
    return json.loads(data)


def open_sessions(n):
    procs = []
    for _ in range(n):
        procs.append(subprocess.Popen(
            ["psql", "-U", "postgres", "-d", "postgres"],
            stdin=subprocess.PIPE, stdout=subprocess.DEVNULL,
            stderr=subprocess.DEVNULL, text=True))
    return procs


def close_sessions(procs):
    for p in procs:
        p.terminate()
    for p in procs:
        try:
            p.wait(timeout=5)
        except subprocess.TimeoutExpired:
            p.kill()


def run_case(pm_pid, sessions, shrink_env, duration=20):
    """Run the tracer against `sessions` open backends; return
    (metrics, status, stderr)."""
    trace_dir = tempfile.mkdtemp(prefix="pgwt_smf_")
    os.chmod(trace_dir, 0o755)
    env = dict(os.environ)
    if shrink_env:
        env["PGWT_STATE_MAP_ENTRIES"] = str(shrink_env)

    tracer = subprocess.Popen(
        [TRACER, "--mode", "tiered", "--pid", str(pm_pid),
         "-T", trace_dir, "--duration", str(duration),
         "--interval", "5", "--quiet"],
        stdout=subprocess.DEVNULL, stderr=subprocess.PIPE, env=env)
    metrics = status = None
    try:
        sock = os.path.join(trace_dir, "pgwt.sock")
        deadline = time.time() + 10
        while time.time() < deadline and not os.path.exists(sock):
            time.sleep(0.25)
        # Give the sampler time to lazily resolve + seed every backend.
        time.sleep(6)
        metrics = ctl(sock, {"cmd": "metrics"})
        status = ctl(sock, {"cmd": "status"})
    finally:
        tracer.terminate()
        try:
            _, stderr = tracer.communicate(timeout=15)
        except subprocess.TimeoutExpired:
            tracer.kill()
            _, stderr = tracer.communicate()
        subprocess.run(["rm", "-rf", trace_dir])
    return metrics, status, stderr.decode("utf-8", errors="replace")


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--pid', type=int, help='Postmaster PID')
    args = parser.parse_args()

    if os.geteuid() != 0:
        print("ERROR: must run as root (sudo)")
        sys.exit(1)
    if not os.path.exists(TRACER):
        print(f"ERROR: {TRACER} not built")
        sys.exit(1)
    pm_pid = args.pid or find_postmaster()
    if not pm_pid:
        print("ERROR: cannot find PostgreSQL postmaster PID")
        sys.exit(1)

    print(f"=== test_state_map_loud (postmaster PID {pm_pid}) ===")

    sessions = open_sessions(10)
    time.sleep(2)   # backends connected (idle -> ClientRead)
    try:
        # ── Case 1: shrunken map (4 entries) — the loud path must fire ──
        print("--- shrunken state_map (PGWT_STATE_MAP_ENTRIES=4) ---")
        metrics, status, err = run_case(pm_pid, sessions, shrink_env=4)
        check(metrics is not None and metrics.get("ok") is True,
              "metrics reachable over the control socket")
        full = int(metrics.get("state_map_full_total", 0)) if metrics else 0
        check(full > 0,
              f"state_map_full_total > 0 with 10+ backends vs 4 slots "
              f"(got {full}) [CAP-1 loud path]")
        check("state_map is FULL" in err,
              "daemon stderr carries the loud state_map-full ERROR")
        check("PGWT_STATE_MAP_ENTRIES" in err,
              "test-hook shrink is itself announced on stderr")

        # T4 observability fields must exist on the daemon side (the mock
        # mirrors them; protocol drift between the two is caught here).
        for key in ("state_map_full_total", "seen_query_ids_full_total",
                    "invalid_wait_reads_total", "sampler_ticks_missed_total",
                    "sampled_attr_tick_read_failures_total",
                    "sampled_attr_shadow_total",
                    "sampled_attr_shadow_mismatch_total",
                    "sampled_attr_shadow_active_total",
                    "sampled_attr_shadow_active_mismatch_total",
                    "sampled_attr_shadow_cmd_open_mismatch_total",
                    "sampled_attr_shadow_query_id_mismatch_total",
                    "sampled_text_pending", "sampled_text_resolved_total",
                    "sampled_text_absent_total", "sampled_text_evicted_total",
                    "sampled_text_error_total",
                    "sampled_text_retry_exhausted_total",
                    "sampler_healthy"):
            check(metrics is not None and key in metrics,
                  f"metrics carries '{key}'")
        for key in ("sampler_healthy", "sampler_unhealthy_reason"):
            check(status is not None and key in status,
                  f"status carries '{key}'")
        check(status is not None and status.get("sampler_healthy") is True,
              "sampler_healthy=true on a working system")
        inv = int(metrics.get("invalid_wait_reads_total", -1)) if metrics else -1
        check(inv == 0, f"invalid_wait_reads_total == 0 on a healthy system "
              f"(got {inv})")

        # ── Case 2: default sizing — no false positives ──────────────────
        print("--- default state_map sizing (no shrink) ---")
        metrics2, _, err2 = run_case(pm_pid, sessions, shrink_env=None,
                                     duration=14)
        full2 = int(metrics2.get("state_map_full_total", -1)) if metrics2 else -1
        check(full2 == 0,
              f"state_map_full_total == 0 at default sizing (got {full2})")
        check("state_map is FULL" not in err2,
              "no state_map-full ERROR at default sizing")
    finally:
        close_sessions(sessions)

    print(f"\n{tests_run - tests_failed}/{tests_run} checks passed")
    sys.exit(0 if tests_failed == 0 else 1)


if __name__ == '__main__':
    main()
