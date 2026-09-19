#!/usr/bin/env python3
"""test_query_event.py — Verify query-level wait event attribution.

Tests:
  1. Basic: pgbench workload produces query_ids in query_event view,
     cross-referenced against pg_stat_statements.
  2. Specific: a known query's query_id from pg_stat_statements
     appears in tracer output.
  3. Mode B: --event filter shows only matching events with % Event column
  4. Mode C: --query-id filter shows wait profile with % Query column

Requires: root, running PostgreSQL 18, pg_wait_tracer built, pgbench initialized,
          compute_query_id = on/auto.
Usage: sudo python3 tests/test_query_event.py [--pid POSTMASTER_PID]
"""
import subprocess
import sys
import os
import re
import time
import argparse

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from testutil import find_postmaster
import test_capture_smoke

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


def parse_query_events(output):
    """Parse query_event view output."""
    events = []
    for line in output.split('\n'):
        line = line.strip()
        m = re.match(
            r'^(-?\d+)\s+'           # query_id (signed int64)
            r'(\S+(?::\S+)?)\s+'     # event name
            r'(\d+)\s+'              # total waits
            r'([\d.]+)\s+'           # total (ms)
            r'([\d.]+)\s+'           # avg (us)
            r'([\d.]+)\s+'           # max (us)
            r'([\d.]+)%',            # % DB
            line
        )
        if m:
            events.append({
                'query_id': int(m.group(1)),
                'name': m.group(2),
                'count': int(m.group(3)),
                'total_ms': float(m.group(4)),
                'avg_us': float(m.group(5)),
                'max_us': float(m.group(6)),
                'pct': float(m.group(7)),
            })
    return events


def cleanup():
    try:
        psql("SELECT pg_terminate_backend(pid) FROM pg_stat_activity "
             "WHERE pid != pg_backend_pid() AND query LIKE '%pg_sleep%'")
    except (subprocess.TimeoutExpired, Exception):
        pass
    try:
        psql("SELECT pg_terminate_backend(pid) FROM pg_stat_activity "
             "WHERE pid != pg_backend_pid() AND query LIKE '%pgbench_accounts%'")
    except (subprocess.TimeoutExpired, Exception):
        pass
    time.sleep(1)


def test_query_event_basic(pm_pid):
    """Run pgbench, verify query_ids appear and match pg_stat_statements."""
    print("--- Test 1: Query Event Basic ---")

    # Ensure pg_stat_statements is available
    psql("CREATE EXTENSION IF NOT EXISTS pg_stat_statements")
    psql("SELECT pg_stat_statements_reset()")

    # Start tracer
    tracer = subprocess.Popen(
        [TRACER, "--mode", "full", "--pid", str(pm_pid),
         "--interval", "12", "--duration", "16",
         "--view", "query_event"],
        stdout=subprocess.PIPE, stderr=subprocess.PIPE
    )

    time.sleep(2)  # let tracer attach

    # Run pgbench workload
    pgbench = subprocess.Popen(
        ["pgbench", "-U", "postgres", "-d", "postgres",
         "-c", "4", "-T", "10"],
        stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL
    )

    pgbench.wait()

    try:
        stdout, stderr = tracer.communicate(timeout=25)
    except subprocess.TimeoutExpired:
        tracer.kill()
        stdout, _ = tracer.communicate()

    output = STRIP_ANSI.sub('', stdout.decode('utf-8', errors='replace'))
    events = parse_query_events(output)

    # Query events should exist
    check(len(events) > 0,
          f"Query events found: {len(events)} entries")

    # Non-zero query_ids
    nonzero_qids = [e for e in events if e['query_id'] != 0]
    check(len(nonzero_qids) > 0,
          f"Non-zero query_ids present: {len(nonzero_qids)}")

    # All events have count > 0
    if events:
        all_valid = all(e['count'] > 0 for e in events)
        check(all_valid,
              f"All events have count > 0")

    # Cross-check with pg_stat_statements
    pss_raw = psql(
        "SELECT queryid FROM pg_stat_statements "
        "WHERE dbid = (SELECT oid FROM pg_database WHERE datname = 'postgres') "
        "AND queryid IS NOT NULL AND queryid != 0"
    )
    pss_qids = set()
    for line in pss_raw.strip().split('\n'):
        line = line.strip()
        if line:
            try:
                pss_qids.add(int(line))
            except ValueError:
                pass

    tracer_qids = set(e['query_id'] for e in events if e['query_id'] != 0)

    if pss_qids and tracer_qids:
        overlap = tracer_qids & pss_qids
        check(len(overlap) > 0,
              f"query_id overlap with pg_stat_statements: "
              f"{len(overlap)} shared out of "
              f"tracer={len(tracer_qids)}, pss={len(pss_qids)}")
    else:
        check(False,
              f"Cross-check: tracer_qids={len(tracer_qids)}, "
              f"pss_qids={len(pss_qids)} (need both non-empty)")


def test_specific_query_id(pm_pid):
    """Run a specific identifiable query, verify its query_id appears."""
    print("--- Test 2: Specific Query Identification ---")

    psql("CREATE EXTENSION IF NOT EXISTS pg_stat_statements")
    psql("SELECT pg_stat_statements_reset()")

    # Start tracer
    tracer = subprocess.Popen(
        [TRACER, "--mode", "full", "--pid", str(pm_pid),
         "--interval", "8", "--duration", "12",
         "--view", "query_event"],
        stdout=subprocess.PIPE, stderr=subprocess.PIPE
    )

    time.sleep(2)

    # Run a distinctive query that generates IO events on pgbench_accounts
    # The sequential scan on 138MB table produces IO:DataFileRead
    psql_proc = subprocess.Popen(
        ["psql", "-U", "postgres", "-d", "postgres",
         "-c", "SELECT count(*) FROM pgbench_accounts WHERE aid > 0",
         "-c", "SELECT pg_sleep(60)"],
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
    events = parse_query_events(output)

    # Get the query_id from pg_stat_statements
    specific_raw = psql(
        "SELECT queryid FROM pg_stat_statements "
        "WHERE query LIKE '%pgbench_accounts%aid%' "
        "AND queryid IS NOT NULL LIMIT 1"
    )
    specific_qid = None
    if specific_raw.strip():
        try:
            specific_qid = int(specific_raw.strip())
        except ValueError:
            pass

    if specific_qid:
        tracer_qids = set(e['query_id'] for e in events)
        check(specific_qid in tracer_qids,
              f"Specific query_id {specific_qid} found in tracer output "
              f"(tracer has {len(tracer_qids)} unique query_ids)")
    else:
        check(False,
              f"Could not find query_id in pg_stat_statements "
              f"(raw: '{specific_raw}')")


def parse_mode_b_events(output):
    """Parse Mode B output (--event filter): query_id | Waits | Total | Avg | Max | % Event | % DB."""
    events = []
    for line in output.split('\n'):
        line = line.strip()
        m = re.match(
            r'^(-?\d+)\s+'           # query_id
            r'(\d+)\s+'              # waits
            r'([\d.]+)\s+'           # total (ms)
            r'([\d.]+)\s+'           # avg (us)
            r'([\d.]+)\s+'           # max (us)
            r'([\d.]+)%\s+'          # % Event
            r'([\d.]+)%',            # % DB
            line
        )
        if m:
            events.append({
                'query_id': int(m.group(1)),
                'count': int(m.group(2)),
                'total_ms': float(m.group(3)),
                'pct_event': float(m.group(6)),
                'pct_db': float(m.group(7)),
            })
    return events


def parse_mode_c_events(output):
    """Parse Mode C output (--query-id filter): Wait Event | Waits | Total | Avg | Max | % Query | % DB."""
    events = []
    for line in output.split('\n'):
        line = line.strip()
        m = re.match(
            r'^(\S+(?::\S+)?)\s+'    # event name
            r'(\d+)\s+'              # waits
            r'([\d.]+)\s+'           # total (ms)
            r'([\d.]+)\s+'           # avg (us)
            r'([\d.]+)\s+'           # max (us)
            r'([\d.]+)%\s+'          # % Query
            r'([\d.]+)%',            # % DB
            line
        )
        if m:
            events.append({
                'name': m.group(1),
                'count': int(m.group(2)),
                'total_ms': float(m.group(3)),
                'pct_query': float(m.group(6)),
                'pct_db': float(m.group(7)),
            })
    return events


def test_mode_b(pm_pid):
    """Verify --event filter shows only matching events with % Event column.

    Uses the capture-smoke Workload's sleeper instead of a pgbench workload
    filtered on IO:DataFileRead (issue #113): on a loaded gate box,
    pgbench's scale-10 data is fully cached, so DataFileRead is rare to
    absent in a short window even though attribution itself works (isolated
    reruns parse events fine) — a workload-density problem, not a product
    bug. A pg_sleep() is deterministic every run regardless of cache state
    or box load; unlike Lock:relation (confirmed on the gate box during
    #113's investigation to not currently correlate to a query_id in
    query_event at all — a separate, unfixed gap), Timeout:PgSleep does.
    Two extra sessions running structurally distinct pg_sleep-shaped SQL
    (the query-id jumbler normalizes literals but not query shape) give
    >= 3 distinct query_id rows, not just >= 1.
    """
    print("--- Test 3: Mode B (--event filter) ---")

    # Route the shared Workload's own precondition checks (e.g. "holder
    # acquired the lock") into this file's counters, so a broken workload
    # precondition fails this test's exit code too.
    test_capture_smoke.check = check
    wl = test_capture_smoke.Workload()
    wl.open_sessions()

    tracer = subprocess.Popen(
        [TRACER, "--mode", "full", "--pid", str(pm_pid),
         "--interval", "8", "--duration", "12",
         "--view", "query_event", "--event", "Timeout:PgSleep"],
        stdout=subprocess.PIPE, stderr=subprocess.PIPE
    )

    time.sleep(2)  # let tracer attach

    extra_sleepers = []
    try:
        wl.fire(sleep_s=3)
        extra_sleepers = [
            wl.open_extra_session("sleeper2", "SELECT pg_sleep(2), 1"),
            wl.open_extra_session("sleeper3", "SELECT 1, pg_sleep(2)"),
        ]
        wl.release()

        try:
            stdout, stderr = tracer.communicate(timeout=25)
        except subprocess.TimeoutExpired:
            tracer.kill()
            stdout, _ = tracer.communicate()
    finally:
        for p in extra_sleepers:
            p.terminate()
            try:
                p.wait(timeout=5)
            except subprocess.TimeoutExpired:
                p.kill()
        wl.stop()

    output = STRIP_ANSI.sub('', stdout.decode('utf-8', errors='replace'))

    # Header should mention the event
    check("Top Queries for Timeout:PgSleep" in output,
          "Mode B header contains event name")

    # % Event column present
    check("% Event" in output,
          "Mode B output contains '% Event' column")

    # Parse and validate
    events = parse_mode_b_events(output)
    check(len(events) > 0,
          f"Mode B parsed {len(events)} entries")

    if events:
        # % Event should sum to ~100%
        pct_sum = sum(e['pct_event'] for e in events)
        check(80 < pct_sum < 120,
              f"Mode B % Event sums to {pct_sum:.1f}% (expected ~100%)")

        # All entries should have count > 0
        all_positive = all(e['count'] > 0 for e in events)
        check(all_positive,
              "Mode B all entries have count > 0")


def test_mode_c(pm_pid):
    """Verify --query-id filter shows wait profile with % Query column."""
    print("--- Test 4: Mode C (--query-id filter) ---")

    psql("CREATE EXTENSION IF NOT EXISTS pg_stat_statements")
    psql("SELECT pg_stat_statements_reset()")

    # Run pgbench to generate query_ids
    pgbench = subprocess.Popen(
        ["pgbench", "-U", "postgres", "-d", "postgres",
         "-c", "4", "-T", "15"],
        stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL
    )

    time.sleep(8)

    # Get a known query_id from pg_stat_statements
    qid_raw = psql(
        "SELECT queryid FROM pg_stat_statements "
        "WHERE dbid = (SELECT oid FROM pg_database WHERE datname = 'postgres') "
        "AND queryid IS NOT NULL AND queryid != 0 "
        "ORDER BY total_exec_time DESC LIMIT 1"
    )

    query_id = None
    if qid_raw.strip():
        try:
            query_id = int(qid_raw.strip())
        except ValueError:
            pass

    if not query_id:
        pgbench.wait()
        check(False, "Could not find query_id in pg_stat_statements")
        return

    tracer = subprocess.Popen(
        [TRACER, "--mode", "full", "--pid", str(pm_pid),
         "--interval", "8", "--duration", "12",
         "--view", "query_event", "--query-id", str(query_id)],
        stdout=subprocess.PIPE, stderr=subprocess.PIPE
    )

    try:
        stdout, stderr = tracer.communicate(timeout=25)
    except subprocess.TimeoutExpired:
        tracer.kill()
        stdout, _ = tracer.communicate()

    pgbench.wait()

    output = STRIP_ANSI.sub('', stdout.decode('utf-8', errors='replace'))

    # Header should mention the query_id
    check(f"Wait Profile for query_id {query_id}" in output,
          f"Mode C header contains query_id {query_id}")

    # % Query column present
    check("% Query" in output,
          "Mode C output contains '% Query' column")

    # Parse and validate
    events = parse_mode_c_events(output)
    check(len(events) > 0,
          f"Mode C parsed {len(events)} events for query_id {query_id}")

    if events:
        # % Query should sum to ~100%
        pct_sum = sum(e['pct_query'] for e in events)
        check(80 < pct_sum < 120,
              f"Mode C % Query sums to {pct_sum:.1f}% (expected ~100%)")

        # CPU* should be present (every query has some CPU)
        has_cpu = any(e['name'] == 'CPU*' for e in events)
        check(has_cpu,
              "Mode C includes CPU* event")


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

    # Check compute_query_id is enabled
    cqid = psql("SHOW compute_query_id")
    if cqid not in ('on', 'auto'):
        print(f"ERROR: compute_query_id = '{cqid}' (need 'on' or 'auto')")
        sys.exit(1)

    print(f"=== test_query_event (postmaster PID {pm_pid}) ===")

    test_query_event_basic(pm_pid)
    test_specific_query_id(pm_pid)
    test_mode_b(pm_pid)
    test_mode_c(pm_pid)

    print(f"\n{tests_passed}/{tests_run} tests passed")
    sys.exit(0 if tests_failed == 0 else 1)


if __name__ == '__main__':
    main()
