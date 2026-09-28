#!/usr/bin/env python3
"""issue191_split_repro.py — deterministic reproduction for issue #191.

NOT part of any suite (not in tests/unit_tests.list, not in tests/run_all.sh):
it needs root and a live PostgreSQL, and it exists so the #191 finding can be
re-checked in one command instead of waiting for a 2-in-16 intermittent.

What #191 reported: `tests/test_deterministic.py` Test 1 runs 5 x pg_sleep(2)
and intermittently sees SIX `Timeout:PgSleep` intervals whose total is still
exactly five sleeps' worth, and guessed at a "near-zero-duration phantom
interval" admitted by the counting path.

What actually happens (measured — see the issue for the traces): a signal
delivered to the sleeping backend ends its WaitLatch, PostgreSQL's pg_sleep
loop re-enters WAIT_EVENT_PG_SLEEP for the remaining time, and the backend
therefore performs SIX real waits.  The sixth record is the second half of a
SPLIT interval (e.g. 1059.476 ms + 941.106 ms = one sleep), never a
zero-duration record, and the six closed records are in the recorded trace
file — written from kernel watchpoint fires — so no live-view accounting
guard is involved.  In the field the signal is a SIGALRM from PostgreSQL's
own 10-second stats-flush timeout; it reproduces with no tracer running at
all.

This script forces the same thing on purpose: at t+5 s (the middle of
sleep #3) another session sets the sleeping backend's latch with
pg_log_backend_memory_contexts().  The tracer then reports the exact field
signature — count 6, total ~10010 ms, max ~2002 ms.

    sudo python3 tests/issue191_split_repro.py            # expect count 6
    sudo python3 tests/issue191_split_repro.py --control  # expect count 5

Exit 0 when the arm's expectation held on every iteration, 1 otherwise.
Requires PG14+ (pg_log_backend_memory_contexts); older majors SKIP.
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

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from testutil import find_postmaster

TRACER = os.path.join(os.path.dirname(os.path.dirname(os.path.abspath(__file__))),
                      "pg_wait_tracer")
STRIP_ANSI = re.compile(r'\x1b\[[0-9;]*[a-zA-Z]')

N_SLEEPS = 5
SLEEP_EACH_S = 2


def psql(sql, timeout=15):
    return subprocess.run(
        ["psql", "-U", "postgres", "-d", "postgres", "-tAc", sql],
        capture_output=True, text=True, timeout=timeout).stdout.strip()


def wait_for_control_socket(proc, trace_dir, timeout=30):
    """Post-attach readiness, same handshake tests/test_deterministic.py uses."""
    path = os.path.join(trace_dir, "pgwt.sock")
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        if os.path.exists(path):
            client = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
            client.settimeout(5)
            try:
                client.connect(path)
                client.sendall(b'{"cmd":"status"}\n')
                data = b""
                while b"\n" not in data:
                    chunk = client.recv(65536)
                    if not chunk:
                        break
                    data += chunk
                if json.loads(data.splitlines()[0]).get("ok") is True:
                    return True
            except (OSError, ValueError, json.JSONDecodeError, IndexError):
                pass
            finally:
                client.close()
        if proc.poll() is not None:
            return False
        time.sleep(0.05)
    return False


def wait_for_backend(application_name, timeout=30):
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        out = psql("SELECT pid FROM pg_stat_activity "
                   f"WHERE application_name = '{application_name}'")
        if re.fullmatch(r'\d+', out or ''):
            return int(out)
        time.sleep(0.05)
    return None


def parse_system_events(output):
    events = []
    for line in output.split('\n'):
        m = re.match(
            r'^(\S+(?::\S+)?)\s+(\d+)\s+([\d.]+)\s+([\d.]+)\s+([\d.]+)\s+([\d.]+)%',
            line.strip())
        if m:
            events.append({'name': m.group(1), 'count': int(m.group(2)),
                           'total_ms': float(m.group(3)),
                           'avg_us': float(m.group(4)),
                           'max_us': float(m.group(5))})
    return events


def one_run(pm_pid, perturb, at_s):
    """One tracer run over 5 x pg_sleep(2). Returns the last PgSleep row."""
    app = f"pgwt_issue191_{os.getpid()}_{time.time_ns()}"
    sql = (f"DO $$ BEGIN FOR i IN 1..{N_SLEEPS} LOOP "
           f"PERFORM pg_sleep({SLEEP_EACH_S}); END LOOP; END $$")
    backend = subprocess.Popen(
        ["psql", "-U", "postgres", "-d", "postgres"],
        stdin=subprocess.PIPE, stdout=subprocess.DEVNULL,
        stderr=subprocess.DEVNULL, text=True,
        env={**os.environ, "PGAPPNAME": app})
    trace_dir = tempfile.mkdtemp(prefix="pgwt_issue191_")
    os.chmod(trace_dir, 0o755)
    tracer = None
    stdout = b""
    try:
        bpid = wait_for_backend(app)
        if bpid is None:
            print("  ERROR: tagged backend never appeared")
            return None
        tracer = subprocess.Popen(
            [TRACER, "--mode", "full", "--pid", str(pm_pid), "-T", trace_dir,
             "--interval", "14", "--duration", "35", "--view", "system_event"],
            stdout=subprocess.PIPE, stderr=subprocess.PIPE)
        if not wait_for_control_socket(tracer, trace_dir):
            print("  ERROR: tracer never reached the post-attach boundary")
            return None
        backend.stdin.write(sql + ";\n")
        backend.stdin.flush()
        t0 = time.monotonic()
        if perturb:
            time.sleep(max(0.0, at_s - (time.monotonic() - t0)))
            psql(f"SELECT pg_log_backend_memory_contexts({bpid})")
            print(f"  latch set at t+{time.monotonic() - t0:.2f}s (pid {bpid})")
        try:
            stdout, _ = tracer.communicate(timeout=80)
        except subprocess.TimeoutExpired:
            tracer.kill()
            stdout, _ = tracer.communicate()
    finally:
        if tracer is not None and tracer.poll() is None:
            tracer.kill()
            tracer.communicate()
        if backend.poll() is None:
            backend.terminate()
            try:
                backend.wait(timeout=5)
            except subprocess.TimeoutExpired:
                backend.kill()
        shutil.rmtree(trace_dir, ignore_errors=True)

    rows = [e for e in parse_system_events(
        STRIP_ANSI.sub('', stdout.decode('utf-8', 'replace')))
        if e['name'] == 'Timeout:PgSleep']
    return rows[-1] if rows else None


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--pid', type=int, help='postmaster PID')
    ap.add_argument('--iters', type=int, default=3)
    ap.add_argument('--at', type=float, default=5.0,
                    help='seconds into the sleep loop at which to set the latch')
    ap.add_argument('--control', action='store_true',
                    help='no perturbation: the same run must report five')
    args = ap.parse_args()

    if os.geteuid() != 0:
        print("ERROR: must run as root (sudo)")
        return 1
    if not os.path.exists(TRACER):
        print(f"ERROR: tracer binary not found at {TRACER}")
        return 1
    major = psql("SHOW server_version_num")
    if major.isdigit() and int(major) < 140000:
        print(f"SKIP: pg_log_backend_memory_contexts needs PG14+ "
              f"(server_version_num={major})")
        return 0

    pm_pid = args.pid or find_postmaster()
    if not pm_pid:
        print("ERROR: cannot find PostgreSQL postmaster PID")
        return 1

    expected = N_SLEEPS if args.control else N_SLEEPS + 1
    arm = "control (no latch set)" if args.control else "latch set mid-sleep"
    print(f"=== issue #191 split reproduction — {arm}, "
          f"expecting count = {expected} ===")

    counts = []
    for i in range(args.iters):
        print(f"[run {i + 1}/{args.iters}]")
        row = one_run(pm_pid, not args.control, args.at)
        if row is None:
            print("  FAIL: no Timeout:PgSleep row")
            counts.append(-1)
            continue
        counts.append(row['count'])
        verdict = "PASS" if row['count'] == expected else "FAIL"
        print(f"  {verdict}: count={row['count']} (expected {expected}) "
              f"total={row['total_ms']:.1f}ms max={row['max_us'] / 1000:.1f}ms")

    hits = counts.count(expected)
    print(f"RESULT: {hits}/{len(counts)} runs reported {expected} "
          f"(counts={counts})")
    return 0 if hits == len(counts) else 1


if __name__ == '__main__':
    sys.exit(main())
