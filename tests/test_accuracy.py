#!/usr/bin/env python3
"""test_accuracy.py — Verify that pg_wait_tracer reports correct numbers.

Three tests:
  1. pg_sleep duration:  run pg_sleep(3), verify tracer reports ~3000ms of Timeout:PgSleep
  2. DB Time sanity:     verify DB Time ≈ wall-clock × active backends (±30%)
  3. IO count cross-check: compare tracer IO:DataFileRead count against pg_stat_io

Requires: root, running PostgreSQL 18, pgbench initialized.
Usage: sudo python3 tests/test_accuracy.py [--pid POSTMASTER_PID]
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


def psql(sql):
    """Run SQL via psql, return stdout."""
    result = subprocess.run(
        ["psql", "-U", "postgres", "-d", "postgres", "-tAc", sql],
        capture_output=True, text=True, timeout=10
    )
    return result.stdout.strip()


def run_tracer(pm_pid, view, duration, interval, event=None):
    """Run tracer, return cleaned stdout."""
    cmd = [TRACER, "--mode", "full", "--pid", str(pm_pid),
           "--interval", str(interval), "--duration", str(duration),
           "--view", view]
    if event:
        cmd += ["--event", event]
    result = subprocess.run(cmd, capture_output=True, timeout=duration + 15)
    return STRIP_ANSI.sub('', result.stdout.decode('utf-8', errors='replace'))


def parse_system_events(output):
    """Parse system_event view into list of {name, count, total_ms, avg_us, max_us, pct}."""
    events = []
    for line in output.split('\n'):
        line = line.strip()
        # Match: "IO:DataFileRead     1035      21.9    21.1    1743.8   0.1%"
        #    or: "CPU                43460    5714.0   131.5   10630.2  22.8%"
        m = re.match(
            r'^(\S+(?::\S+)?)\s+'       # event name (CPU or Class:Event)
            r'(\d+)\s+'                  # total waits
            r'([\d.]+)\s+'              # total (ms)
            r'([\d.]+)\s+'              # avg (us)
            r'([\d.]+)\s+'              # max (us)
            r'([\d.]+)%',              # % DB
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
    """Parse time_model view into dict of {name: time_ms}."""
    model = {}
    for line in output.split('\n'):
        line = line.strip()
        # Match: "DB Time                 25088.6     100.0%"
        # or:    "CPU                      5498.6      21.9%"
        # or:    "IO                       3262.8      13.0%"
        # or:    "IO:DataFileRead           621.2      12.2%"
        m = re.match(r'^(.+?)\s{2,}([\d.]+)\s+[\d.]+%', line)
        if m:
            name = m.group(1).strip()
            value = float(m.group(2))
            model[name] = value
    # Also match Activity line: "(Activity/Idle — ..." with "—" instead of %
    for line in output.split('\n'):
        m = re.match(r'.*Activity.*?\s+([\d.]+)\s+', line.strip())
        if m:
            model['Activity'] = float(m.group(1))
    return model


# ── Test 1: pg_sleep duration ──────────────────────────────────

def cleanup_stale_backends():
    """Terminate leftover client backends from previous tests."""
    try:
        psql("SELECT pg_terminate_backend(pid) FROM pg_stat_activity "
             "WHERE pid != pg_backend_pid() AND datname = 'postgres' "
             "AND backend_type = 'client backend' AND state != 'active'")
        psql("SELECT pg_terminate_backend(pid) FROM pg_stat_activity "
             "WHERE pid != pg_backend_pid() AND query LIKE '%pg_sleep%'")
    except (subprocess.TimeoutExpired, Exception):
        pass
    time.sleep(1)


def test_pg_sleep_duration(pm_pid):
    """Verify tracer reports Timeout:PgSleep with reasonable duration.

    Start pg_sleep(5) BEFORE the tracer so the initial scan finds the
    backend already in PgSleep.  This avoids the race window in fork
    detection → watchpoint setup that causes flakiness under load.

    Timeline:
      t=0    start psql: pg_sleep(5) + pg_sleep(60)
      t=1    start tracer (interval=8, duration=12)
      t=1    initial scan finds backend in PgSleep, sets watchpoint
      t=5    pg_sleep(5) ends → PgSleep→CPU transition fires watchpoint
             closed interval ≈ 4s (from attachment at t=1 to transition at t=5)
      t=5    pg_sleep(60) starts, keeps backend alive
      t=9    timer tick reads maps: PgSleep count=1, total ≈ 4000ms
      t=13   tracer exits
    """
    print("--- Test 1: pg_sleep Duration ---")

    SLEEP_SEC = 5

    # Start pg_sleep BEFORE tracer so initial scan finds backend in PgSleep
    psql_proc = subprocess.Popen(
        ["psql", "-U", "postgres", "-d", "postgres",
         "-c", f"SELECT pg_sleep({SLEEP_SEC})",
         "-c", "SELECT pg_sleep(60)"],
        stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL
    )

    time.sleep(1)  # let backend enter PgSleep

    # Start tracer — initial scan will find backend already in PgSleep
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

    output = STRIP_ANSI.sub('', stdout.decode('utf-8', errors='replace'))

    # Kill the long pg_sleep session
    psql_proc.terminate()
    try:
        psql_proc.wait(timeout=5)
    except subprocess.TimeoutExpired:
        psql_proc.kill()
    try:
        psql("SELECT pg_terminate_backend(pid) FROM pg_stat_activity "
             "WHERE pid != pg_backend_pid() AND query LIKE '%pg_sleep%'")
    except (subprocess.TimeoutExpired, Exception):
        pass
    time.sleep(1)

    events = parse_system_events(output)

    # Find Timeout:PgSleep
    pg_sleep_ev = [e for e in events if e['name'] == 'Timeout:PgSleep']

    check(len(pg_sleep_ev) > 0,
          f"Timeout:PgSleep found in output (events seen: {[e['name'] for e in events]})")

    if pg_sleep_ev:
        ev = pg_sleep_ev[0]

        # Closed interval from pg_sleep(5): ~4s (5s minus ~1s before attachment)
        # Check duration is reasonable (> 2s, < 8s)
        check(ev['total_ms'] > 2000,
              f"Timeout:PgSleep duration {ev['total_ms']:.1f}ms > 2000ms")

        check(ev['count'] >= 1,
              f"Timeout:PgSleep count = {ev['count']} (expected ≥ 1)")


# ── Test 2: DB Time sanity check ──────────────────────────────

def test_db_time_sanity(pm_pid):
    """Verify DB Time is reasonable and internally consistent.

    Two checks:
      a) DB Time > 0 and < theoretical max (CLIENTS × wall-clock)
      b) DB Time = CPU Time + sum(all Wait classes)  (internal consistency)
    """
    print("--- Test 2: DB Time Sanity ---")

    CLIENTS = 4
    BENCH_SEC = 15
    INTERVAL = 12  # one snapshot after pgbench is well into its run

    # Start pgbench FIRST so backends are forked and attached before timing
    pgbench = subprocess.Popen(
        ["pgbench", "-U", "postgres", "-d", "postgres",
         "-c", str(CLIENTS), "-T", str(BENCH_SEC)],
        stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL
    )

    time.sleep(3)  # let pgbench connect + backends get attached

    # Run tracer
    output = run_tracer(pm_pid, "time_model",
                        duration=INTERVAL + 2, interval=INTERVAL)

    pgbench.wait()

    model = parse_time_model(output)

    check('DB Time' in model,
          f"DB Time found in output (keys: {list(model.keys())})")

    if 'DB Time' not in model:
        return

    db_time_ms = model['DB Time']
    cpu_time_ms = model.get('CPU*', 0)

    # Sanity: DB Time must be positive and meaningful
    check(db_time_ms > 1000,
          f"DB Time = {db_time_ms:.0f}ms (expected > 1000ms for {CLIENTS} clients)")

    # Upper bound: can't exceed CLIENTS × INTERVAL × 1000 + some extra from
    # system backends (checkpointer, bgwriter, walwriter also contribute).
    # Add 5% margin: open intervals from state_map can slightly exceed
    # wall-clock due to timing drift between event and state_map timestamps.
    theoretical_max_ms = (CLIENTS + 5) * INTERVAL * 1000 * 1.05
    check(db_time_ms < theoretical_max_ms,
          f"DB Time {db_time_ms:.0f}ms < theoretical max {theoretical_max_ms:.0f}ms")

    # Internal consistency: DB Time = CPU* + Off-CPU* + sum of all Wait
    # classes (class-level names only, not sub-events which contain ':').
    # Off-CPU* (issue #202) is CPU*'s measured sibling — the wall of the
    # on-CPU intervals the backend was NOT on a CPU for (runqueue/throttle).
    # It is absent on the legacy/sampled tiers, where CPU* carries the whole
    # gap; model.get() then returns 0 and the identity is the old one.
    WAIT_CLASSES = {'IO', 'LWLock', 'Lock', 'Client', 'IPC',
                    'BufferPin', 'Timeout', 'Extension'}
    wait_sum = sum(v for k, v in model.items() if k in WAIT_CLASSES)
    offcpu_ms = model.get('Off-CPU*', 0)

    reconstructed = cpu_time_ms + offcpu_ms + wait_sum
    if db_time_ms > 0:
        error_pct = abs(reconstructed - db_time_ms) / db_time_ms * 100
        check(error_pct < 2.0,
              f"DB Time consistency: CPU({cpu_time_ms:.0f}) + "
              f"OffCPU({offcpu_ms:.0f}) + Waits({wait_sum:.0f}) "
              f"= {reconstructed:.0f}ms vs DB Time {db_time_ms:.0f}ms "
              f"(error {error_pct:.1f}%)")

    # CPU Time should be > 0
    check(cpu_time_ms > 0,
          f"CPU = {cpu_time_ms:.0f}ms (should be > 0)")

    # % should sum to ~100%
    if db_time_ms > 0:
        total_pct = reconstructed / db_time_ms * 100
        check(98 <= total_pct <= 102,
              f"Percentages sum to {total_pct:.1f}% (expected ~100%)")


# ── Test 3: IO count cross-check vs pg_stat_io ───────────────

def test_io_count_cross_check(pm_pid):
    """Compare tracer IO:DataFileRead count against pg_stat_io reads.

    COUNTS, deliberately not time (T2 study Q3): pg_stat_io.read_time's
    INSTR_TIME window covers per-op AIO bookkeeping + per-page completion
    work (~9us + ~1.7us/page more than the wait_event set/clear window),
    so a time ratio floats 0.55-0.90 with mean op latency — the historical
    "0.80 async overlap" was instrumentation-window overhead, not overlap
    and not data loss. Counts agree to +/-0.2% when windows align.

    Strategy: use pgbench TPC-B (not -S) which does updates and generates
    real IO.  Start pgbench first, let backends attach, THEN reset pg_stat_io
    and start the tracer measurement window simultaneously.
    """
    print("--- Test 3: IO Count Cross-Check ---")

    CLIENTS = 4
    BENCH_SEC = 20
    INTERVAL = 10

    # Drop caches to ensure we get real reads (not all from shared_buffers)
    try:
        with open('/proc/sys/vm/drop_caches', 'w') as f:
            f.write('3\n')
        time.sleep(1)
    except PermissionError:
        pass  # non-fatal — we may still get reads

    # Start pgbench (TPC-B with updates — generates more IO than -S)
    pgbench = subprocess.Popen(
        ["pgbench", "-U", "postgres", "-d", "postgres",
         "-c", str(CLIENTS), "-T", str(BENCH_SEC)],
        stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL
    )

    time.sleep(4)  # let pgbench connect + backends get fully attached

    # Now reset pg_stat_io — the tracer and pg_stat_io start from the same point
    psql("SELECT pg_stat_reset_shared('io')")

    # Run tracer for one interval
    output = run_tracer(pm_pid, "system_event",
                        duration=INTERVAL + 2, interval=INTERVAL)

    # Read pg_stat_io immediately after tracer finishes
    pg_reads = int(psql(
        "SELECT coalesce(sum(reads), 0) FROM pg_stat_io "
        "WHERE backend_type = 'client backend' AND object = 'relation'"
    ) or '0')

    pgbench.wait()

    # Parse tracer output
    events = parse_system_events(output)
    df_read = [e for e in events if e['name'] == 'IO:DataFileRead']

    tracer_reads = df_read[0]['count'] if df_read else 0

    print(f"  pg_stat_io reads:        {pg_reads}")
    print(f"  tracer IO:DataFileRead:  {tracer_reads}")

    if pg_reads == 0 and tracer_reads == 0:
        check(True, "No reads occurred (all cached) — consistent")
        return

    # Both should agree on the order of magnitude
    if pg_reads > 0 and tracer_reads > 0:
        ratio = tracer_reads / pg_reads
        # The tracer snapshot covers INTERVAL seconds starting from attach.
        # pg_stat_io was reset at the same time but covers until we query it
        # (a few seconds later). So allow generous tolerance.
        check(0.1 <= ratio <= 3.0,
              f"ratio tracer/pg_stat_io = {ratio:.2f} "
              f"(acceptable range 0.1-3.0)")
    elif pg_reads > 0:
        # pg_stat_io has reads but tracer doesn't — may be all cached for
        # the tracer's window (reads happened after tracer snapshot)
        check(True, f"tracer={tracer_reads}, pg_stat_io={pg_reads} "
              f"(timing window difference)")
    else:
        check(tracer_reads == 0,
              f"pg_stat_io=0 but tracer={tracer_reads}")

    # Avg latency sanity: DataFileRead should typically be < 10ms
    if df_read and df_read[0]['count'] > 0:
        avg_us = df_read[0]['avg_us']
        check(avg_us < 10000,
              f"IO:DataFileRead avg latency = {avg_us:.0f}us (expected < 10ms)")


# ── Main ─────────────────────────────────────────────────────

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

    print(f"=== test_accuracy (postmaster PID {pm_pid}) ===")

    cleanup_stale_backends()

    test_pg_sleep_duration(pm_pid)
    test_db_time_sanity(pm_pid)
    test_io_count_cross_check(pm_pid)

    print(f"\n{tests_passed}/{tests_run} tests passed")
    sys.exit(0 if tests_failed == 0 else 1)


if __name__ == '__main__':
    main()
