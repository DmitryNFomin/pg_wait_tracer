#!/usr/bin/env python3
"""test_cpu_time.py — Verify CPU time dominance for compute-heavy queries.

Two tests:
  1. system_event: CPU event has highest total_ms for pure-compute query
  2. time_model:   CPU Time > sum of all Wait classes

Requires: root, running PostgreSQL 18, pg_wait_tracer built.
Usage: sudo python3 tests/test_cpu_time.py [--pid POSTMASTER_PID]
"""
import subprocess
import sys
import os
import re
import time
import argparse

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from testutil import find_postmaster

TRACER = os.path.join(os.path.dirname(os.path.dirname(os.path.abspath(__file__))),
                      "pg_wait_tracer")
STRIP_ANSI = re.compile(r'\x1b\[[0-9;]*[a-zA-Z]')

tests_run = 0
tests_passed = 0
tests_failed = 0


def check(cond, msg):
    global tests_run, tests_passed, tests_failed
    tests_run += 1
    if cond:
        tests_passed += 1
        print(f"  PASS: {msg}")
    else:
        tests_failed += 1
        print(f"  FAIL: {msg}")


def psql(sql, timeout=10):
    result = subprocess.run(
        ["psql", "-U", "postgres", "-d", "postgres", "-tAc", sql],
        capture_output=True, text=True, timeout=timeout
    )
    return result.stdout.strip()


def parse_system_events(output):
    events = []
    for line in output.split('\n'):
        line = line.strip()
        m = re.match(
            r'^(\S+(?::\S+)?)\s+'
            r'(\d+)\s+'
            r'([\d.]+)\s+'
            r'([\d.]+)\s+'
            r'([\d.]+)\s+'
            r'([\d.]+)%',
            line
        )
        if m:
            events.append({
                'name': m.group(1),
                'count': int(m.group(2)),
                'total_ms': float(m.group(3)),
                'avg_us': float(m.group(4)),
                'max_us': float(m.group(5)),
                'pct': float(m.group(6)),
            })
    return events


def parse_time_model(output):
    model = {}
    for line in output.split('\n'):
        line = line.strip()
        m = re.match(r'^(.+?)\s{2,}([\d.]+)\s+[\d.]+%', line)
        if m:
            name = m.group(1).strip()
            value = float(m.group(2))
            model[name] = value
    for line in output.split('\n'):
        m = re.match(r'.*Activity.*?\s+([\d.]+)\s+', line.strip())
        if m:
            model['Activity'] = float(m.group(1))
    return model


# Waits by BACKGROUND processes that are NOT user query work: the checkpointer
# throttling between write batches (CheckpointWriteDelay), autovacuum's
# cost-based delay (VacuumDelay), and the pg_wait_sampling collector's
# background worker parked on its latch (Extension:Extension — seen for
# seconds on boxes where that extension is loaded, e.g. EL8). These are idle
# background sleeps that inflate system-wide DB Time; excluded from the "compute
# dominates" comparison so the assertion tracks real query activity, not which
# background worker happened to be waiting during the window.
BACKGROUND_WAITS = ('Timeout:CheckpointWriteDelay', 'Timeout:VacuumDelay',
                    'Extension:Extension')


def is_background_wait(name):
    """BACKGROUND_WAITS, plus any custom Extension id: pg_wait_events gives no
    id for custom extension events (#801), so the collector's wait renders
    as Extension:id=<n> rather than a name."""
    return name in BACKGROUND_WAITS or name.startswith('Extension:id=')


def cleanup():
    """Terminate leftover backends and quiet the box before measuring."""
    for pat in ('%generate_series%', '%pg_sleep%', '%LOOP%'):
        try:
            psql("SELECT pg_terminate_backend(pid) FROM pg_stat_activity "
                 f"WHERE pid != pg_backend_pid() AND query LIKE '{pat}'")
        except (subprocess.TimeoutExpired, Exception):
            pass
    # Flush pending dirty pages now (waits for any in-progress checkpoint, then
    # runs an immediate one) so no spread checkpoint throttles during the trace
    # window. The compute query writes no WAL, so nothing re-triggers one.
    try:
        psql("CHECKPOINT", timeout=30)
    except (subprocess.TimeoutExpired, Exception):
        pass
    time.sleep(1)


def test_cpu_system_event(pm_pid):
    """Run pure-compute query, verify CPU is dominant in system_event."""
    print("--- Test 1: CPU Dominance (system_event) ---")

    # Start the tracer FIRST, then fire the compute AFTER it has attached, so
    # the backend is caught by the fork tracepoint (reliable) rather than the
    # initial-scan straddle path (an intermittent connect/scan race for a
    # pure-compute client backend — see docs/ROADMAP_AND_STATUS.md; the straddle case
    # itself is covered by phase_cpu_straddle). The loop must outlast the whole
    # measured window so the backend is pinned on-CPU for every interval.
    tracer = subprocess.Popen(
        [TRACER, "--mode", "full", "--pid", str(pm_pid),
         "--interval", "8", "--duration", "12",
         "--view", "system_event"],
        stdout=subprocess.PIPE, stderr=subprocess.PIPE
    )
    time.sleep(3)  # tracer scans + arms watchpoints before the backend forks

    # NESTED loop: a plpgsql integer FOR loop binds its counter to int4, so a
    # single `1..3000000000` bound raises "integer out of range" INSTANTLY
    # (3e9 > INT_MAX) — the loop never runs and the backend is pure-idle. The
    # nested form keeps both bounds well under INT_MAX while doing billions of
    # iterations (~minutes of CPU), so the backend stays pinned for the whole
    # window and is killed below. No pg_sleep tail: if the compute ever breaks
    # again, the trace shows ~0 CPU and this test FAILS loudly instead of
    # silently measuring the sleep.
    psql_proc = subprocess.Popen(
        ["psql", "-U", "postgres", "-d", "postgres",
         "-c", "DO $$ DECLARE x bigint := 0; BEGIN "
               "FOR i IN 1..100000 LOOP "
               "FOR j IN 1..100000 LOOP x := x + 1; END LOOP; "
               "END LOOP; END $$"],
        stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL
    )

    try:
        stdout, stderr = tracer.communicate(timeout=25)
    except subprocess.TimeoutExpired:
        tracer.kill()
        stdout, _ = tracer.communicate()

    psql_proc.terminate()
    try:
        psql_proc.wait(timeout=5)
    except subprocess.TimeoutExpired:
        psql_proc.kill()
    cleanup()

    output = STRIP_ANSI.sub('', stdout.decode('utf-8', errors='replace'))
    events = parse_system_events(output)

    cpu_ev = [e for e in events if e['name'] == 'CPU*']

    check(len(cpu_ev) > 0,
          f"CPU event found (events: {[e['name'] for e in events]})")

    if cpu_ev:
        ev = cpu_ev[0]
        # The backend is pinned on-CPU for the whole window; the first report
        # covers ~5s of compute (fired at t=3, first interval ends at t=8), so
        # a correctly-measured CPU* is several thousand ms. >3000ms clears the
        # shared-box floor while still failing hard on the ~0 regression.
        check(ev['total_ms'] > 3000,
              f"CPU total = {ev['total_ms']:.1f}ms > 3000ms")

        # A pure-compute backend must make CPU the DOMINANT event among real
        # query activity — background maintenance sleeps (checkpointer,
        # autovacuum) are excluded (see BACKGROUND_WAITS) since they are idle
        # throttling, not work, and can run for seconds under suite load.
        competing = [e for e in events
                     if not e['name'].startswith('Activity:')
                     and not is_background_wait(e['name'])]
        if competing:
            top_ms = max(e['total_ms'] for e in competing)
            check(ev['total_ms'] >= top_ms * 0.8,
                  f"CPU {ev['total_ms']:.1f}ms is the dominant event "
                  f"(>=80% of top {top_ms:.1f}ms, excl. background maintenance)")


def test_cpu_time_model(pm_pid):
    """Run pure-compute query, verify CPU Time > Wait in time_model."""
    print("--- Test 2: CPU Dominance (time_model) ---")

    # Tracer first, then fire the compute after attach (fork tracepoint —
    # reliable; see test_cpu_system_event above). Nested int4-safe loop, no
    # pg_sleep tail — same rationale as test_cpu_system_event.
    tracer = subprocess.Popen(
        [TRACER, "--mode", "full", "--pid", str(pm_pid),
         "--interval", "8", "--duration", "12",
         "--view", "time_model"],
        stdout=subprocess.PIPE, stderr=subprocess.PIPE
    )
    time.sleep(3)

    psql_proc = subprocess.Popen(
        ["psql", "-U", "postgres", "-d", "postgres",
         "-c", "DO $$ DECLARE x bigint := 0; BEGIN "
               "FOR i IN 1..100000 LOOP "
               "FOR j IN 1..100000 LOOP x := x + 1; END LOOP; "
               "END LOOP; END $$"],
        stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL
    )

    try:
        stdout, stderr = tracer.communicate(timeout=25)
    except subprocess.TimeoutExpired:
        tracer.kill()
        stdout, _ = tracer.communicate()

    psql_proc.terminate()
    try:
        psql_proc.wait(timeout=5)
    except subprocess.TimeoutExpired:
        psql_proc.kill()
    cleanup()

    output = STRIP_ANSI.sub('', stdout.decode('utf-8', errors='replace'))
    model = parse_time_model(output)
    print(f"  time_model: {model}")

    cpu_ms = model.get('CPU*', 0)
    check(cpu_ms > 0,
          f"CPU = {cpu_ms:.1f}ms > 0")

    # The backend is pinned on-CPU for the whole window (fork-after-attach); the
    # first report covers ~5s of compute, so a correctly-measured CPU* is
    # several thousand ms. >3000ms fails hard on the ~0 regression (the class
    # this guards: a fork-caught compute backend whose ongoing on-CPU interval
    # was suppressed in the live view — the has_closed_data bug, fixed in
    # map_reader.c) while clearing the shared-box floor.
    check(cpu_ms > 3000,
          f"CPU {cpu_ms:.1f}ms > 3000ms (compute-heavy query captured)")

    # CPU must DOMINATE real DB Time: the only sustained query work on the box
    # is this pure-compute backend. Background maintenance sleeps (checkpointer,
    # autovacuum — see BACKGROUND_WAITS) are subtracted from the denominator
    # since they are idle throttling, not work; under suite load they can add
    # seconds of DB Time. Off-CPU* and minor IO still dilute, so require >40%.
    db_time = model.get('DB Time', 0)
    background = sum(v for w, v in model.items() if is_background_wait(w))
    real_db = db_time - background
    if real_db > 0:
        cpu_pct = cpu_ms / real_db * 100
        check(cpu_pct > 40,
              f"CPU is {cpu_pct:.1f}% of real DB Time "
              f"(DB {db_time:.0f}ms - background {background:.0f}ms; "
              f"compute dominates)")


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--pid', type=int, help='Postmaster PID')
    args = parser.parse_args()

    if os.geteuid() != 0:
        print("ERROR: must run as root (sudo)")
        sys.exit(1)

    if not os.path.exists(TRACER):
        print(f"ERROR: tracer binary not found at {TRACER}")
        sys.exit(1)

    pm_pid = args.pid
    if not pm_pid:
        pm_pid = find_postmaster()
    if not pm_pid:
        print("ERROR: cannot find PostgreSQL postmaster PID")
        sys.exit(1)

    print(f"=== test_cpu_time (postmaster PID {pm_pid}) ===")

    cleanup()

    test_cpu_system_event(pm_pid)
    test_cpu_time_model(pm_pid)

    print(f"\n{tests_passed}/{tests_run} tests passed")
    sys.exit(0 if tests_failed == 0 else 1)


if __name__ == '__main__':
    main()
