#!/usr/bin/env python3
"""test_event_classes.py — Verify all wait event classes are detected.

Covers the 4 classes not tested elsewhere:
  1. Activity (0x05): background processes always generate idle events
  2. IPC (0x08):      parallel query generates IPC:ExecuteGather
  3. Extension (0x07): pg_wait_sampling generates Extension:Extension
  4. BufferPin (0x04): concurrent VACUUM + scan on same table

Requires: root, running PostgreSQL 18, pg_wait_tracer built, pgbench initialized.
Usage: sudo python3 tests/test_event_classes.py [--pid POSTMASTER_PID]
"""
import subprocess
import sys
import os
import re
import time
import argparse

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from testutil import find_postmaster, pg_wait_sampling_available

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


def psql(sql, timeout=30):
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


def cleanup():
    try:
        psql("SELECT pg_terminate_backend(pid) FROM pg_stat_activity "
             "WHERE pid != pg_backend_pid() AND query LIKE '%pg_sleep%'")
    except (subprocess.TimeoutExpired, Exception):
        pass
    try:
        psql("SELECT pg_terminate_backend(pid) FROM pg_stat_activity "
             "WHERE pid != pg_backend_pid() "
             "AND query LIKE '%pgbench_accounts%' AND state != 'idle'")
    except (subprocess.TimeoutExpired, Exception):
        pass
    time.sleep(1)


# ── Test 1: Activity events ──────────────────────────────────

def test_activity(pm_pid):
    """Verify Activity (idle) events from background processes are detected
    and correctly excluded from DB Time."""
    print("--- Test 1: Activity Events ---")

    # Run tracer with time_model view during pgbench to ensure some DB Time
    pgbench = subprocess.Popen(
        ["pgbench", "-U", "postgres", "-d", "postgres",
         "-c", "4", "-T", "15"],
        stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL
    )

    time.sleep(3)

    tracer = subprocess.Popen(
        [TRACER, "--mode", "full", "--pid", str(pm_pid),
         "--interval", "8", "--duration", "12",
         "--view", "time_model"],
        stdout=subprocess.PIPE, stderr=subprocess.PIPE
    )

    try:
        stdout, stderr = tracer.communicate(timeout=25)
    except subprocess.TimeoutExpired:
        tracer.kill()
        stdout, _ = tracer.communicate()

    pgbench.wait()

    output = STRIP_ANSI.sub('', stdout.decode('utf-8', errors='replace'))
    model = parse_time_model(output)

    # Activity should be present (background processes are always idle)
    check('Activity' in model,
          f"Activity time reported (keys: {list(model.keys())})")

    if 'Activity' in model:
        check(model['Activity'] > 0,
              f"Activity time = {model['Activity']:.1f}ms > 0")

    # Activity must NOT be included in DB Time (verify via consistency)
    db_time = model.get('DB Time', 0)
    cpu_time = model.get('CPU*', 0)
    WAIT_CLASSES = {'IO', 'LWLock', 'Lock', 'Client', 'IPC',
                    'BufferPin', 'Timeout', 'Extension'}
    wait_sum = sum(v for k, v in model.items() if k in WAIT_CLASSES)
    # Off-CPU* (issue #202) is CPU*'s measured sibling and a first-class
    # component of DB Time; absent (0) on the legacy/sampled tiers.
    offcpu = model.get('Off-CPU*', 0)
    reconstructed = cpu_time + offcpu + wait_sum
    if db_time > 0:
        error_pct = abs(reconstructed - db_time) / db_time * 100
        check(error_pct < 2.0,
              f"Activity excluded from DB Time: "
              f"CPU({cpu_time:.0f}) + OffCPU({offcpu:.0f}) + "
              f"Waits({wait_sum:.0f}) = "
              f"{reconstructed:.0f}ms vs DB Time {db_time:.0f}ms "
              f"(error {error_pct:.1f}%)")


# ── Test 2: IPC events via parallel query ─────────────────────

def test_ipc(pm_pid):
    """Verify IPC events via parallel query on pgbench_accounts."""
    print("--- Test 2: IPC Events (Parallel Query) ---")

    # PL/pgSQL PERFORM doesn't use parallel execution, so we must run
    # direct SQL queries.  Write a temp SQL file with many repeated
    # parallel-eligible queries.
    sql_file = "/tmp/_pgwt_test_ipc.sql"
    with open(sql_file, "w") as f:
        # Force parallel execution even on small pgbench_accounts tables
        f.write("SET max_parallel_workers_per_gather = 4;\n")
        f.write("SET parallel_setup_cost = 0;\n")
        f.write("SET parallel_tuple_cost = 0;\n")
        f.write("SET min_parallel_table_scan_size = '1kB';\n")
        for _ in range(200):
            f.write("SELECT count(*) FROM pgbench_accounts WHERE aid > 0;\n")

    # Launch several concurrent sessions to maximize IPC:ExecuteGather time
    psql_procs = []
    for _ in range(4):
        p = subprocess.Popen(
            ["psql", "-U", "postgres", "-d", "postgres", "-f", sql_file],
            stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL
        )
        psql_procs.append(p)

    time.sleep(3)  # let parallel queries start

    tracer = subprocess.Popen(
        [TRACER, "--mode", "full", "--pid", str(pm_pid),
         "--interval", "10", "--duration", "14",
         "--view", "system_event"],
        stdout=subprocess.PIPE, stderr=subprocess.PIPE
    )

    try:
        stdout, stderr = tracer.communicate(timeout=30)
    except subprocess.TimeoutExpired:
        tracer.kill()
        stdout, _ = tracer.communicate()

    for p in psql_procs:
        p.terminate()
    for p in psql_procs:
        try:
            p.wait(timeout=5)
        except subprocess.TimeoutExpired:
            p.kill()
    cleanup()
    try:
        os.unlink(sql_file)
    except OSError:
        pass

    output = STRIP_ANSI.sub('', stdout.decode('utf-8', errors='replace'))
    events = parse_system_events(output)

    ipc_events = [e for e in events if e['name'].startswith('IPC:')]

    check(len(ipc_events) > 0,
          f"IPC events detected: {[e['name'] for e in ipc_events]} "
          f"(all: {[e['name'] for e in events]})")

    if ipc_events:
        top_ipc = max(ipc_events, key=lambda e: e['total_ms'])
        check(top_ipc['count'] > 0,
              f"Top IPC {top_ipc['name']} count = {top_ipc['count']}")


# ── Test 3: Extension events ─────────────────────────────────

def test_extension(pm_pid):
    """Verify Extension events are detected (pg_wait_sampling generates them)."""
    print("--- Test 3: Extension Events ---")

    # Check pg_wait_sampling is loaded. The local psql() returns "" on
    # error instead of raising, so a try/except here never fires — probe
    # via the process return code instead (see testutil).
    if not pg_wait_sampling_available():
        print("  SKIP: pg_wait_sampling not loaded")
        return

    # pgbench generates activity; pg_wait_sampling's background worker
    # produces Extension:Extension events while sampling
    pgbench = subprocess.Popen(
        ["pgbench", "-U", "postgres", "-d", "postgres",
         "-c", "4", "-T", "15"],
        stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL
    )

    time.sleep(3)

    tracer = subprocess.Popen(
        [TRACER, "--mode", "full", "--pid", str(pm_pid),
         "--interval", "8", "--duration", "12",
         "--view", "system_event"],
        stdout=subprocess.PIPE, stderr=subprocess.PIPE
    )

    try:
        stdout, stderr = tracer.communicate(timeout=25)
    except subprocess.TimeoutExpired:
        tracer.kill()
        stdout, _ = tracer.communicate()

    pgbench.wait()

    output = STRIP_ANSI.sub('', stdout.decode('utf-8', errors='replace'))
    events = parse_system_events(output)

    ext_events = [e for e in events if e['name'].startswith('Extension:')]

    check(len(ext_events) > 0,
          f"Extension events detected: {[e['name'] for e in ext_events]} "
          f"(all: {[e['name'] for e in events]})")

    if ext_events:
        ev = ext_events[0]
        check(ev['count'] > 0,
              f"Extension:{ev['name']} count = {ev['count']}")

    # Also verify Extension appears in time_model
    tracer2 = subprocess.Popen(
        [TRACER, "--mode", "full", "--pid", str(pm_pid),
         "--interval", "5", "--duration", "8",
         "--view", "time_model"],
        stdout=subprocess.PIPE, stderr=subprocess.PIPE
    )

    # Generate some activity while time_model runs
    pgbench2 = subprocess.Popen(
        ["pgbench", "-U", "postgres", "-d", "postgres",
         "-c", "2", "-T", "8"],
        stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL
    )

    try:
        stdout2, _ = tracer2.communicate(timeout=20)
    except subprocess.TimeoutExpired:
        tracer2.kill()
        stdout2, _ = tracer2.communicate()

    pgbench2.wait()

    output2 = STRIP_ANSI.sub('', stdout2.decode('utf-8', errors='replace'))
    model = parse_time_model(output2)

    check('Extension' in model,
          f"Extension in time_model (keys: {list(model.keys())})")


# ── Test 4: BufferPin events ─────────────────────────────────

def test_bufferpin(pm_pid):
    """Attempt to generate BufferPin events via concurrent VACUUM + scan.

    BufferPin waits occur when a backend tries to access a buffer page
    that is currently pinned by another backend (e.g. during VACUUM).
    This is inherently hard to trigger deterministically.
    """
    print("--- Test 4: BufferPin Events ---")

    # Create a test table with enough data to trigger concurrent access
    psql("DROP TABLE IF EXISTS _test_bufferpin")
    psql("CREATE TABLE _test_bufferpin AS "
         "SELECT i, repeat('x', 200) AS data "
         "FROM generate_series(1, 100000) i")
    psql("CREATE INDEX ON _test_bufferpin(i)")

    # Delete ~50% of rows to create dead tuples for VACUUM to process
    psql("DELETE FROM _test_bufferpin WHERE i % 2 = 0")

    # Run concurrent VACUUM and sequential scans to try to trigger BufferPin
    # VACUUM processes pages while scans try to read the same pages
    vacuum_proc = subprocess.Popen(
        ["psql", "-U", "postgres", "-d", "postgres",
         "-c", "VACUUM (VERBOSE) _test_bufferpin",
         "-c", "SELECT pg_sleep(60)"],
        stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL
    )

    time.sleep(1)

    # Multiple concurrent scan sessions
    scan_procs = []
    for _ in range(4):
        p = subprocess.Popen(
            ["psql", "-U", "postgres", "-d", "postgres",
             "-c", "DO $$ BEGIN "
                   "FOR i IN 1..50 LOOP "
                   "PERFORM count(*) FROM _test_bufferpin; "
                   "END LOOP; END $$",
             "-c", "SELECT pg_sleep(60)"],
            stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL
        )
        scan_procs.append(p)

    time.sleep(2)

    tracer = subprocess.Popen(
        [TRACER, "--mode", "full", "--pid", str(pm_pid),
         "--interval", "10", "--duration", "14",
         "--view", "system_event"],
        stdout=subprocess.PIPE, stderr=subprocess.PIPE
    )

    try:
        stdout, stderr = tracer.communicate(timeout=30)
    except subprocess.TimeoutExpired:
        tracer.kill()
        stdout, _ = tracer.communicate()

    # Cleanup
    vacuum_proc.terminate()
    for p in scan_procs:
        p.terminate()
    for p in [vacuum_proc] + scan_procs:
        try:
            p.wait(timeout=5)
        except subprocess.TimeoutExpired:
            p.kill()
    cleanup()
    psql("DROP TABLE IF EXISTS _test_bufferpin")

    output = STRIP_ANSI.sub('', stdout.decode('utf-8', errors='replace'))
    events = parse_system_events(output)

    bp_events = [e for e in events if e['name'].startswith('BufferPin:')]

    # BufferPin is very hard to trigger — don't fail the test if not seen,
    # but report it as a bonus pass if detected
    if bp_events:
        check(True,
              f"BufferPin events detected: {[e['name'] for e in bp_events]}")
        check(bp_events[0]['count'] > 0,
              f"BufferPin count = {bp_events[0]['count']}")
    else:
        # Not a failure — BufferPin is inherently rare
        print(f"  INFO: BufferPin not triggered (this is expected — very rare event)")
        print(f"        Events seen: {[e['name'] for e in events]}")


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

    print(f"=== test_event_classes (postmaster PID {pm_pid}) ===")

    test_activity(pm_pid)
    test_ipc(pm_pid)
    test_extension(pm_pid)
    test_bufferpin(pm_pid)

    print(f"\n{tests_passed}/{tests_run} tests passed")
    sys.exit(0 if tests_failed == 0 else 1)


if __name__ == '__main__':
    main()
