#!/usr/bin/env python3
"""test_idle_exclusion.py — Activity/idle time is excluded from DB Time.

Spec 0A.6: Verify that system backends in Activity:* wait states
(checkpointer idle, autovac launcher idle, etc.) contribute to
"Activity" time but NOT to DB Time.

The tracer classifies Activity:* events as idle and excludes them from
DB Time. This test verifies that exclusion works correctly by running a
light workload where system backends dominate idle time.

Requires: root, running PostgreSQL 18.
Usage: sudo python3 tests/test_idle_exclusion.py [--pid POSTMASTER_PID]
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


def cleanup_stale_backends():
    try:
        psql("SELECT pg_terminate_backend(pid) FROM pg_stat_activity "
             "WHERE pid != pg_backend_pid() AND datname = 'postgres' "
             "AND backend_type = 'client backend' AND state != 'active'")
        psql("SELECT pg_terminate_backend(pid) FROM pg_stat_activity "
             "WHERE pid != pg_backend_pid() AND query LIKE '%generate_series%'")
    except (subprocess.TimeoutExpired, Exception):
        pass
    time.sleep(1)


def parse_time_model(output):
    model = {}
    for line in output.split('\n'):
        line = line.strip()
        m = re.match(r'^(.+?)\s{2,}([\d.]+)\s+[\d.]+%', line)
        if m:
            model[m.group(1).strip()] = float(m.group(2))
    for line in output.split('\n'):
        m = re.match(r'.*Activity.*?\s+([\d.]+)\s+', line.strip())
        if m:
            model['Activity'] = float(m.group(1))
    return model


def parse_system_events(output):
    events = []
    for line in output.split('\n'):
        line = line.strip()
        m = re.match(
            r'^(\S+(?::\S+)?)\s+(\d+)\s+([\d.]+)\s+([\d.]+)\s+([\d.]+)\s+([\d.]+)%',
            line
        )
        if m:
            events.append({
                'name': m.group(1),
                'count': int(m.group(2)),
                'total_ms': float(m.group(3)),
            })
    return events


def test_idle_exclusion(pm_pid):
    """Verify Activity time is excluded from DB Time.

    With just 1 active backend, system backends (checkpointer, bgwriter,
    walwriter, archiver, logical_launcher) are in Activity:* states.
    Their combined idle time should be reported as Activity but excluded
    from DB Time.
    """
    print("--- Test 1: Activity excluded from DB Time ---")

    INTERVAL = 8

    # 1 active backend: CPU burn
    active_proc = subprocess.Popen(
        ["psql", "-U", "postgres", "-d", "postgres",
         "-c", "SELECT count(*) FROM generate_series(1, 500000000)",
         "-c", "SELECT pg_sleep(60)"],
        stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL
    )

    time.sleep(1)

    # Start tracer with time_model to see Activity line
    tracer = subprocess.Popen(
        [TRACER, "--mode", "full", "--pid", str(pm_pid),
         "--interval", str(INTERVAL), "--duration", str(INTERVAL + 4),
         "--view", "time_model"],
        stdout=subprocess.PIPE, stderr=subprocess.PIPE
    )

    try:
        stdout, _ = tracer.communicate(timeout=INTERVAL + 20)
    except subprocess.TimeoutExpired:
        tracer.kill()
        stdout, _ = tracer.communicate()

    output = STRIP_ANSI.sub('', stdout.decode('utf-8', errors='replace'))

    # Cleanup
    active_proc.terminate()
    try:
        active_proc.wait(timeout=5)
    except subprocess.TimeoutExpired:
        active_proc.kill()
    try:
        psql("SELECT pg_terminate_backend(pid) FROM pg_stat_activity "
             "WHERE pid != pg_backend_pid() AND query LIKE '%generate_series%'")
    except (subprocess.TimeoutExpired, Exception):
        pass
    time.sleep(1)

    model = parse_time_model(output)

    check('DB Time' in model,
          f"DB Time found (keys: {list(model.keys())})")
    if 'DB Time' not in model:
        return

    db_time = model['DB Time']
    activity_time = model.get('Activity', 0)
    cpu_time = model.get('CPU*', 0)

    # Activity time should be reported (system backends are idle)
    check(activity_time > 0,
          f"Activity time = {activity_time:.0f}ms (system backends idle time)")

    # Activity time should be substantial — multiple system backends ×
    # INTERVAL seconds each
    check(activity_time > INTERVAL * 1000,
          f"Activity = {activity_time:.0f}ms > {INTERVAL * 1000}ms "
          f"(multiple idle system backends)")

    # DB Time should NOT include Activity
    # Verify: CPU + all wait classes = DB Time (without Activity)
    WAIT_CLASSES = {'IO', 'LWLock', 'Lock', 'Client', 'IPC',
                    'BufferPin', 'Timeout', 'Extension'}
    wait_sum = sum(model.get(c, 0) for c in WAIT_CLASSES)
    # The product's identity is DB Time = CPU* + Off-CPU* + sum(waits); the
    # runqueue row is reported separately and must be counted here too.
    offcpu_time = model.get('Off-CPU*', 0)
    reconstructed = cpu_time + offcpu_time + wait_sum

    if db_time > 0:
        error_pct = abs(reconstructed - db_time) / db_time * 100
        check(error_pct < 1.0,
              f"DB Time = CPU + Waits (no Activity): "
              f"CPU({cpu_time:.0f}) + OffCPU({offcpu_time:.0f}) + "
              f"Waits({wait_sum:.0f}) = {reconstructed:.0f} "
              f"vs DB Time {db_time:.0f} (error {error_pct:.1f}%)")

    # DB Time + Activity should be more than DB Time alone
    total_with_activity = db_time + activity_time
    check(total_with_activity > db_time * 1.5,
          f"DB Time + Activity = {total_with_activity:.0f}ms >> "
          f"DB Time = {db_time:.0f}ms (Activity adds significant time)")

    # Also check system_event: Activity events should NOT appear
    tracer2 = subprocess.Popen(
        [TRACER, "--mode", "full", "--pid", str(pm_pid),
         "--interval", str(INTERVAL), "--duration", str(INTERVAL + 4),
         "--view", "system_event"],
        stdout=subprocess.PIPE, stderr=subprocess.PIPE
    )

    # Restart active backend for second measurement
    active_proc2 = subprocess.Popen(
        ["psql", "-U", "postgres", "-d", "postgres",
         "-c", "SELECT count(*) FROM generate_series(1, 500000000)",
         "-c", "SELECT pg_sleep(60)"],
        stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL
    )

    time.sleep(1)

    tracer2 = subprocess.Popen(
        [TRACER, "--mode", "full", "--pid", str(pm_pid),
         "--interval", str(INTERVAL), "--duration", str(INTERVAL + 4),
         "--view", "system_event"],
        stdout=subprocess.PIPE, stderr=subprocess.PIPE
    )

    try:
        stdout2, _ = tracer2.communicate(timeout=INTERVAL + 20)
    except subprocess.TimeoutExpired:
        tracer2.kill()
        stdout2, _ = tracer2.communicate()

    output2 = STRIP_ANSI.sub('', stdout2.decode('utf-8', errors='replace'))
    events = parse_system_events(output2)

    active_proc2.terminate()
    try:
        active_proc2.wait(timeout=5)
    except subprocess.TimeoutExpired:
        active_proc2.kill()

    # No Activity events should appear in system_event (they're excluded)
    activity_events = [e for e in events if e['name'].startswith('Activity:')]
    check(len(activity_events) == 0,
          f"No Activity events in system_event "
          f"(found {len(activity_events)}: {[e['name'] for e in activity_events]})")


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

    print(f"=== test_idle_exclusion (postmaster PID {pm_pid}) ===")

    cleanup_stale_backends()
    test_idle_exclusion(pm_pid)

    print(f"\n{tests_passed}/{tests_run} tests passed")
    sys.exit(0 if tests_failed == 0 else 1)


if __name__ == '__main__':
    main()
