#!/usr/bin/env python3
"""test_multi_window.py — Verify multi-window time_model, system_event, and query_event output.

Tests:
  1. time_model format validation: column headers, separator, data rows present
  2. time_model progressive population: shorter windows fill first, longer show '-'
  3. time_model internal consistency: DB Time = CPU + Wait classes (first window)
  4. system_event format: vertically stacked sections with section headers
  5. system_event progressive population: shorter sections fill first
  6. system_event data sanity: events sorted, counts > 0, no Max column
  7. query_event Mode A multi-window: section headers, column headers, data rows
  8. query_event Mode B multi-window: --event filter with % Event column
  9. query_event Mode C multi-window: --query-id filter with % Query column
 10. histogram multi-window: side-by-side columns, progressive population, pct sums

Requires: root, running PostgreSQL 18, pgbench initialized,
          compute_query_id = on/auto.
Usage: sudo python3 tests/test_multi_window.py [--pid POSTMASTER_PID]
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


def run_tracer(pm_pid, interval, count, windows, view="time_model",
               duration=None, timeout_extra=15, extra_args=None):
    """Run tracer with --window, return cleaned stdout."""
    cmd = [TRACER, "--mode", "full", "--pid", str(pm_pid),
           "--interval", str(interval),
           "--window", windows,
           "--count", str(count),
           "--view", view]
    if duration:
        cmd += ["--duration", str(duration)]
    if extra_args:
        cmd += extra_args
    wall = interval * count + timeout_extra
    result = subprocess.run(cmd, capture_output=True, timeout=wall)
    return STRIP_ANSI.sub('', result.stdout.decode('utf-8', errors='replace'))


def split_ticks(output):
    """Split piped output into per-tick text blocks.

    In pipe mode each tick starts with '--- YYYY-MM-DD HH:MM:SS ---'.
    Returns only non-empty tick blocks (skips preamble before first separator).
    """
    ticks = []
    current = []
    for line in output.split('\n'):
        if re.match(r'^--- \d{4}-\d{2}-\d{2} \d{2}:\d{2}:\d{2} ---$', line.strip()):
            if current:
                text = '\n'.join(current)
                if text.strip():
                    ticks.append(text)
            current = []
        else:
            current.append(line)
    if current:
        text = '\n'.join(current)
        if text.strip():
            ticks.append(text)
    return ticks


def parse_first_window(output):
    """Parse time_model multi-window output, extracting first window values.

    Returns dict of {name: value_ms}. Works because the existing regex
    captures the first numerical value on each row — which is the first
    window's value in multi-window mode.
    """
    model = {}
    for line in output.split('\n'):
        line = line.strip()
        m = re.match(r'^(.+?)\s{2,}([\d.]+)\s+[\d.]+%', line)
        if m:
            name = m.group(1).strip()
            value = float(m.group(2))
            model[name] = value
    return model


# ── Test 1: Format Validation ────────────────────────────────

def test_format(pm_pid):
    """Verify multi-window output has correct headers and structure."""
    print("--- Test 1: Format Validation ---")

    # Start pgbench for background workload
    pgbench = subprocess.Popen(
        ["pgbench", "-U", "postgres", "-d", "postgres",
         "-c", "2", "-T", "20"],
        stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL
    )

    time.sleep(3)

    output = run_tracer(pm_pid, interval=1, count=4, windows="1s,3s")

    pgbench.wait()

    # Column headers present
    check("Last 1s" in output,
          f"Output contains 'Last 1s' header")
    check("Last 3s" in output,
          f"Output contains 'Last 3s' header")
    check("% DB" in output,
          f"Output contains '% DB' header")

    # Separator line (all dashes)
    has_separator = any(
        set(line.strip()) <= {'-', ' '} and len(line.strip()) > 30
        for line in output.split('\n')
        if line.strip()
    )
    check(has_separator, "Output contains dash separator line")

    # Data rows
    check("DB Time" in output,
          f"Output contains 'DB Time' row")
    check("CPU*" in output,
          f"Output contains 'CPU*' row")

    # At least one data row has numeric values (not just dashes)
    has_data = bool(re.search(r'DB Time\s+[\d.]+\s+[\d.]+%', output))
    check(has_data,
          "DB Time row has numeric values")


# ── Test 2: Progressive Population ───────────────────────────

def test_progressive_population(pm_pid):
    """Verify shorter windows fill before longer ones.

    With --window 1s,5s --interval 1:
    - Tick 1: "waiting for data" (ring needs >=2 pushes for 1-tick delta)
    - Tick 2: first window valid, second shows '-' (needs 5 pushes)
    - Tick 6+: both windows valid
    """
    print("--- Test 2: Progressive Population ---")

    pgbench = subprocess.Popen(
        ["pgbench", "-U", "postgres", "-d", "postgres",
         "-c", "2", "-T", "20"],
        stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL
    )

    time.sleep(3)

    output = run_tracer(pm_pid, interval=1, count=7, windows="1s,5s")

    pgbench.wait()

    ticks = split_ticks(output)

    check(len(ticks) >= 5,
          f"Got {len(ticks)} ticks (expected >= 5)")

    if len(ticks) < 2:
        return

    # First tick should show "waiting for data"
    check("waiting for data" in ticks[0],
          "First tick shows 'waiting for data'")

    # Early ticks (2-4): first window valid, second may show '-'
    # Look for a tick that has "Last 1s" data but '-' for second window
    found_partial = False
    for tick in ticks[1:4]:
        if "Last 1s" in tick:
            lines = tick.split('\n')
            for line in lines:
                if "DB Time" in line:
                    # Check if line has a '-' after first window's value
                    # Pattern: number + % + ... + '-'
                    m = re.search(r'DB Time\s+[\d.]+\s+[\d.]+%.*\s+-\s+-', line)
                    if m:
                        found_partial = True
                    break

    check(found_partial,
          "Early tick has first window data but '-' for second window")

    # Later ticks: both windows should be valid
    if len(ticks) >= 6:
        last_tick = ticks[-1]
        # Should have "Last 5s" data (not just '-')
        has_both = bool(re.search(r'DB Time\s+[\d.]+\s+[\d.]+%\s+[\d.]+\s+[\d.]+%',
                                  last_tick))
        check(has_both,
              "Last tick has data for both windows")


# ── Test 3: Internal Consistency ─────────────────────────────

def test_internal_consistency(pm_pid):
    """Verify DB Time = CPU* + sum(Wait classes) in multi-window mode.

    Uses first window values parsed from multi-window output.
    """
    print("--- Test 3: Internal Consistency ---")

    CLIENTS = 4
    BENCH_SEC = 30

    pgbench = subprocess.Popen(
        ["pgbench", "-U", "postgres", "-d", "postgres",
         "-c", str(CLIENTS), "-T", str(BENCH_SEC)],
        stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL
    )

    time.sleep(3)

    # Use a 5s window; count=3 because first tick is "waiting for data"
    # pgbench must outlive the measurement: 3 × 5s = 15s + 3s warmup = 18s < 30s
    output = run_tracer(pm_pid, interval=5, count=3,
                        windows="5s")

    pgbench.wait()

    model = parse_first_window(output)

    check('DB Time' in model,
          f"DB Time found in multi-window output (keys: {list(model.keys())})")

    if 'DB Time' not in model:
        return

    db_time_ms = model['DB Time']
    cpu_time_ms = model.get('CPU*', 0)

    check(db_time_ms > 100,
          f"DB Time = {db_time_ms:.0f}ms (expected > 100ms)")

    check(cpu_time_ms > 0,
          f"CPU* = {cpu_time_ms:.0f}ms (expected > 0)")

    # DB Time decomposes into TOP-LEVEL rows only: CPU*, Off-CPU* (measured,
    # T8), and each wait-class PARENT. A sub-event row (e.g. "Timeout:PgSleep")
    # carries ':' and is ALREADY counted inside its parent ("Timeout") — summing
    # both double-counts and overshoots DB Time (the ~120-130% %DB test bug seen
    # on EL9). "DB Time" and "Idle" are indent-0 rows, not components of DB Time.
    # Summing every ':'-free row (rather than a hardcoded class set) is robust to
    # Off-CPU*, which is a measured sibling of CPU* and part of DB Time — leaving
    # it out would make the reconstruction fall short by the off-CPU time.
    EXCLUDE = {'DB Time', 'Idle'}
    top_level = {k: v for k, v in model.items()
                 if ':' not in k and k not in EXCLUDE}
    reconstructed = sum(top_level.values())

    if db_time_ms > 0:
        error_pct = abs(reconstructed - db_time_ms) / db_time_ms * 100
        check(error_pct < 2.0,
              f"DB Time consistency: Σ(top-level rows {sorted(top_level)}) "
              f"= {reconstructed:.0f}ms vs DB Time {db_time_ms:.0f}ms "
              f"(error {error_pct:.1f}%)")


# ── Test 4: system_event Format ─────────────────────────────

def test_system_event_format(pm_pid):
    """Verify multi-window system_event has vertically stacked sections."""
    print("--- Test 4: system_event Format ---")

    pgbench = subprocess.Popen(
        ["pgbench", "-U", "postgres", "-d", "postgres",
         "-c", "2", "-T", "20"],
        stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL
    )

    time.sleep(3)

    output = run_tracer(pm_pid, interval=1, count=4, windows="1s,3s",
                        view="system_event")

    pgbench.wait()

    # Section headers present (---- Last Xs ----)
    check("Last 1s" in output,
          "system_event contains 'Last 1s' section header")
    check("Last 3s" in output,
          "system_event contains 'Last 3s' section header")

    # Verify section headers use ---- format
    has_section_header = bool(re.search(r'^---- Last \d+s -+$', output, re.MULTILINE))
    check(has_section_header,
          "system_event has '---- Last Xs ----' section header format")

    # Column headers in each section
    check("Wait Event" in output,
          "system_event contains 'Wait Event' column")
    check("Total Waits" in output,
          "system_event contains 'Total Waits' column")
    check("Avg (us)" in output,
          "system_event contains 'Avg (us)' column")
    check("% DB" in output,
          "system_event contains '% DB' column")

    # No Max column in multi-window mode (deltas don't track max)
    # Count occurrences of "Max (us)" — should be 0
    max_count = output.count("Max (us)")
    check(max_count == 0,
          f"system_event multi-window has no Max column (found {max_count})")


# ── Test 5: system_event Progressive Population ────────────

def test_system_event_progressive(pm_pid):
    """Verify shorter sections fill before longer ones."""
    print("--- Test 5: system_event Progressive Population ---")

    pgbench = subprocess.Popen(
        ["pgbench", "-U", "postgres", "-d", "postgres",
         "-c", "2", "-T", "20"],
        stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL
    )

    time.sleep(3)

    output = run_tracer(pm_pid, interval=1, count=7, windows="1s,5s",
                        view="system_event")

    pgbench.wait()

    ticks = split_ticks(output)

    check(len(ticks) >= 5,
          f"Got {len(ticks)} ticks (expected >= 5)")

    if len(ticks) < 2:
        return

    # First tick: both sections should show "waiting for data"
    check("waiting for data" in ticks[0],
          "First tick shows 'waiting for data'")

    # Early tick (2-4): first section has data, second shows "waiting for data"
    found_partial = False
    for tick in ticks[1:4]:
        sections = re.split(r'^---- Last \d+[smh] -+$', tick, flags=re.MULTILINE)
        if len(sections) >= 3:
            # sections[1] = content after "Last 1s", sections[2] = content after "Last 5s"
            first_has_data = bool(re.search(r'Wait Event', sections[1]))
            second_waiting = "waiting for data" in sections[2]
            if first_has_data and second_waiting:
                found_partial = True
                break

    check(found_partial,
          "Early tick: first section has data, second shows 'waiting for data'")

    # Later ticks: both sections should have data
    if len(ticks) >= 6:
        last_tick = ticks[-1]
        sections = re.split(r'^---- Last \d+[smh] -+$', last_tick,
                            flags=re.MULTILINE)
        if len(sections) >= 3:
            both_have_data = (
                bool(re.search(r'Wait Event', sections[1])) and
                bool(re.search(r'Wait Event', sections[2]))
            )
            check(both_have_data,
                  "Last tick has data in both sections")


# ── Test 6: system_event Data Sanity ───────────────────────

def parse_system_events_section(text):
    """Parse events from a system_event section.

    The % DB column is either a number ("12.3%") for non-idle (DB-Time)
    events, or "—" (em-dash) for idle-but-visible events (Client:ClientRead),
    which have time but no meaningful share of DB Time. We capture the raw
    token and expose 'pct' (float or None) plus 'pct_is_dash'."""
    events = []
    for line in text.split('\n'):
        line = line.strip()
        m = re.match(
            r'^(\S+(?::\S+)?)\s+'
            r'(\d+)\s+'
            r'([\d.]+)\s+'
            r'([\d.]+)\s+'
            r'(\d[\d.]*%|—)',          # % DB: number+% OR em-dash
            line
        )
        if m:
            pct_tok = m.group(5)
            is_dash = (pct_tok == '—')
            events.append({
                'name': m.group(1),
                'count': int(m.group(2)),
                'total_ms': float(m.group(3)),
                'avg_us': float(m.group(4)),
                'pct': None if is_dash else float(pct_tok.rstrip('%')),
                'pct_is_dash': is_dash,
            })
    return events


def test_system_event_data(pm_pid):
    """Verify system_event multi-window data is sane."""
    print("--- Test 6: system_event Data Sanity ---")

    CLIENTS = 4
    BENCH_SEC = 30

    pgbench = subprocess.Popen(
        ["pgbench", "-U", "postgres", "-d", "postgres",
         "-c", str(CLIENTS), "-T", str(BENCH_SEC)],
        stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL
    )

    time.sleep(3)

    output = run_tracer(pm_pid, interval=5, count=3, windows="5s",
                        view="system_event")

    pgbench.wait()

    ticks = split_ticks(output)

    # Find last tick with actual data
    last_data_tick = None
    for tick in reversed(ticks):
        if "Wait Event" in tick:
            last_data_tick = tick
            break

    check(last_data_tick is not None,
          "Found tick with event data")

    if not last_data_tick:
        return

    events = parse_system_events_section(last_data_tick)

    check(len(events) > 0,
          f"Parsed {len(events)} events from system_event section")

    if not events:
        return

    # Events should be sorted by total_ms descending
    totals = [e['total_ms'] for e in events]
    is_sorted = all(totals[i] >= totals[i + 1] for i in range(len(totals) - 1))
    check(is_sorted,
          "Events sorted by Total (ms) descending")

    # All events should have count > 0
    all_positive = all(e['count'] > 0 for e in events)
    check(all_positive,
          "All events have count > 0")

    # %DB = share of DB Time and applies ONLY to non-idle (DB-Time) events.
    # Idle-but-visible events (Client:ClientRead) are listed (they have time)
    # but render "—" for %DB, not a number — counting them would overshoot the
    # column past 100%. Sum must be ~100% across the NON-IDLE rows.
    #
    # Every row of the system_event view is a LEAF wait event (CPU* plus
    # "Class:Event" rows); unlike the time_model view (Test 3) it has no class
    # parent rows, so the rows are disjoint and Σ non-idle rows == DB Time
    # minus the off-CPU remainder of the open on-CPU stretches (the CPU* row
    # carries measured on-CPU ns, DB Time wall) plus the io_worker waits (kept
    # visible in the rows, excluded from DB Time by the T2 category contract —
    # ~1% under pgbench on PG18 io_method=worker). Issue #97: this check used
    # to sum only ':'-free rows, i.e. the CPU* row ALONE, and read 128–142%
    # because the live closed-record path filed non-command CPU under CPU*
    # while DB Time excluded it; with that fixed the CPU* row alone is the
    # in-command share (~13-17% under pgbench on the gate box — the live gate
    # is read at emission, #98), so the row selection here is the identity the
    # comment always described, not a single row.
    # Off-CPU* (measured runqueue residual) can be MOST of DB Time under
    # oversubscription (observed 44.7% visible on a 4-vCPU EL8 box under suite
    # load — the rest was off-CPU), hence the loose floor; ≤125% still fails
    # hard on double counting (a parent row atop its children would be ~150%+).
    # Exact conservation is Test 3 above.
    non_idle = [e for e in events if not e['pct_is_dash']]
    pct_sum = sum(e['pct'] for e in non_idle)
    check(15 < pct_sum <= 125,
          f"Non-idle %DB sums to {pct_sum:.1f}% "
          f"(15-125%; = 100% - Off-CPU* per snapshot ± windowed-delta noise)")

    # Idle-but-visible events must render "—" (not a number) for %DB.
    idle_rows = [e for e in events if e['name'] == 'Client:ClientRead']
    if idle_rows:
        all_dash = all(e['pct_is_dash'] for e in idle_rows)
        check(all_dash,
              "Client:ClientRead renders '—' for %DB (idle, excluded from DB Time)")
        # ...while still showing its time (it is visible, not hidden).
        has_time = all(e['total_ms'] > 0 for e in idle_rows)
        check(has_time,
              "Client:ClientRead still shows its time (visible row)")


# ── Test 7: query_event Mode A multi-window ─────────────────

def parse_query_events_section(text):
    """Parse query_event Mode A rows from a multi-window section.

    Format: query_id  Wait Event  Waits  Total (ms)  Avg (us)  % DB
    """
    events = []
    for line in text.split('\n'):
        line = line.strip()
        m = re.match(
            r'^(-?\d+)\s+'           # query_id
            r'(\S+(?::\S+)?)\s+'     # event name
            r'(\d+)\s+'              # waits
            r'([\d.]+)\s+'           # total (ms)
            r'([\d.]+)\s+'           # avg (us)
            r'([\d.]+)%',            # % DB
            line
        )
        if m:
            events.append({
                'query_id': int(m.group(1)),
                'name': m.group(2),
                'count': int(m.group(3)),
                'total_ms': float(m.group(4)),
                'pct': float(m.group(6)),
            })
    return events


def test_query_event_mode_a(pm_pid):
    """Verify multi-window query_event Mode A has section headers and data."""
    print("--- Test 7: query_event Mode A multi-window ---")

    pgbench = subprocess.Popen(
        ["pgbench", "-U", "postgres", "-d", "postgres",
         "-c", "4", "-T", "25"],
        stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL
    )

    time.sleep(3)

    output = run_tracer(pm_pid, interval=1, count=5, windows="1s,3s",
                        view="query_event")

    pgbench.wait()

    # Section headers present
    check("Last 1s" in output,
          "query_event Mode A contains 'Last 1s' section")
    check("Last 3s" in output,
          "query_event Mode A contains 'Last 3s' section")

    # Has section header format
    has_section_header = bool(re.search(r'^---- Last \d+s -+$', output,
                                        re.MULTILINE))
    check(has_section_header,
          "query_event Mode A has '---- Last Xs ----' format")

    # Column headers
    check("query_id" in output,
          "query_event Mode A contains 'query_id' column")
    check("Wait Event" in output,
          "query_event Mode A contains 'Wait Event' column")
    check("% DB" in output,
          "query_event Mode A contains '% DB' column")

    # No Max column in multi-window mode
    max_count = output.count("Max (us)")
    check(max_count == 0,
          f"query_event Mode A multi-window has no Max column (found {max_count})")

    # Parse data from last tick with data
    ticks = split_ticks(output)
    last_data_tick = None
    for tick in reversed(ticks):
        if "query_id" in tick:
            last_data_tick = tick
            break

    if last_data_tick:
        events = parse_query_events_section(last_data_tick)
        check(len(events) > 0,
              f"query_event Mode A parsed {len(events)} entries")
    else:
        check(False, "query_event Mode A: no tick with data found")


# ── Test 8: query_event Mode B multi-window ─────────────────

def parse_mode_b_section(text):
    """Parse query_event Mode B rows: query_id  Waits  Total  Avg  % Event  % DB."""
    events = []
    for line in text.split('\n'):
        line = line.strip()
        m = re.match(
            r'^(-?\d+)\s+'           # query_id
            r'(\d+)\s+'              # waits
            r'([\d.]+)\s+'           # total (ms)
            r'([\d.]+)\s+'           # avg (us)
            r'([\d.]+)%\s+'          # % Event
            r'([\d.]+)%',            # % DB
            line
        )
        if m:
            events.append({
                'query_id': int(m.group(1)),
                'count': int(m.group(2)),
                'total_ms': float(m.group(3)),
                'pct_event': float(m.group(5)),
                'pct_db': float(m.group(6)),
            })
    return events


def test_query_event_mode_b(pm_pid):
    """Verify multi-window query_event --event filter shows % Event."""
    print("--- Test 8: query_event Mode B multi-window ---")

    pgbench = subprocess.Popen(
        ["pgbench", "-U", "postgres", "-d", "postgres",
         "-c", "4", "-T", "25"],
        stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL
    )

    time.sleep(3)

    output = run_tracer(pm_pid, interval=1, count=5, windows="1s,3s",
                        view="query_event",
                        extra_args=["--event", "IO:DataFileRead"])

    pgbench.wait()

    # Header mentions the event
    check("Top Queries for IO:DataFileRead" in output,
          "query_event Mode B multi-window has correct header")

    # Section headers present
    check("Last 1s" in output,
          "query_event Mode B contains 'Last 1s' section")

    # % Event column present
    check("% Event" in output,
          "query_event Mode B multi-window contains '% Event' column")

    # No Max column
    max_count = output.count("Max (us)")
    check(max_count == 0,
          f"query_event Mode B multi-window has no Max column (found {max_count})")

    # Parse and validate data
    ticks = split_ticks(output)
    last_data_tick = None
    for tick in reversed(ticks):
        if "% Event" in tick:
            last_data_tick = tick
            break

    if last_data_tick:
        # Get first section with data
        sections = re.split(r'^---- Last \d+[smh] -+$', last_data_tick,
                            flags=re.MULTILINE)
        events = []
        for section in sections:
            events = parse_mode_b_section(section)
            if events:
                break

        check(len(events) > 0,
              f"query_event Mode B parsed {len(events)} entries")

        if events:
            pct_sum = sum(e['pct_event'] for e in events)
            check(80 < pct_sum < 120,
                  f"query_event Mode B % Event sums to {pct_sum:.1f}% "
                  f"(expected ~100%)")
    else:
        check(False, "query_event Mode B: no tick with data found")
        check(False, "query_event Mode B: cannot check % Event sum")


# ── Test 9: query_event Mode C multi-window ─────────────────

def psql(sql, timeout=10):
    result = subprocess.run(
        ["psql", "-U", "postgres", "-d", "postgres", "-tAc", sql],
        capture_output=True, text=True, timeout=timeout
    )
    return result.stdout.strip()


def parse_mode_c_section(text):
    """Parse query_event Mode C rows: Wait Event  Waits  Total  Avg  % Query  % DB."""
    events = []
    for line in text.split('\n'):
        line = line.strip()
        m = re.match(
            r'^(\S+(?::\S+)?)\s+'    # event name
            r'(\d+)\s+'              # waits
            r'([\d.]+)\s+'           # total (ms)
            r'([\d.]+)\s+'           # avg (us)
            r'([\d.]+)%\s+'          # % Query
            r'([\d.]+)%',            # % DB
            line
        )
        if m:
            events.append({
                'name': m.group(1),
                'count': int(m.group(2)),
                'total_ms': float(m.group(3)),
                'pct_query': float(m.group(5)),
                'pct_db': float(m.group(6)),
            })
    return events


def test_query_event_mode_c(pm_pid):
    """Verify multi-window query_event --query-id filter shows % Query."""
    print("--- Test 9: query_event Mode C multi-window ---")

    psql("CREATE EXTENSION IF NOT EXISTS pg_stat_statements")
    psql("SELECT pg_stat_statements_reset()")

    pgbench = subprocess.Popen(
        ["pgbench", "-U", "postgres", "-d", "postgres",
         "-c", "4", "-T", "25"],
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

    output = run_tracer(pm_pid, interval=1, count=5, windows="1s,3s",
                        view="query_event",
                        extra_args=["--query-id", str(query_id)])

    pgbench.wait()

    # Header mentions the query_id
    check(f"Wait Profile for query_id {query_id}" in output,
          f"query_event Mode C multi-window header contains query_id")

    # Section headers present
    check("Last 1s" in output,
          "query_event Mode C contains 'Last 1s' section")

    # % Query column present
    check("% Query" in output,
          "query_event Mode C multi-window contains '% Query' column")

    # No Max column
    max_count = output.count("Max (us)")
    check(max_count == 0,
          f"query_event Mode C multi-window has no Max column (found {max_count})")

    # Parse and validate data
    ticks = split_ticks(output)
    last_data_tick = None
    for tick in reversed(ticks):
        if "% Query" in tick:
            last_data_tick = tick
            break

    if last_data_tick:
        sections = re.split(r'^---- Last \d+[smh] -+$', last_data_tick,
                            flags=re.MULTILINE)
        events = []
        for section in sections:
            events = parse_mode_c_section(section)
            if events:
                break

        check(len(events) > 0,
              f"query_event Mode C parsed {len(events)} events")

        if events:
            pct_sum = sum(e['pct_query'] for e in events)
            check(80 < pct_sum < 120,
                  f"query_event Mode C % Query sums to {pct_sum:.1f}% "
                  f"(expected ~100%)")

            has_cpu = any(e['name'] == 'CPU*' for e in events)
            check(has_cpu,
                  "query_event Mode C includes CPU* event")
    else:
        check(False, "query_event Mode C: no tick with data found")
        check(False, "query_event Mode C: cannot check % Query sum")
        check(False, "query_event Mode C: cannot check CPU*")


# ── Test 10: histogram multi-window ──────────────────────────

def parse_histogram_row(line):
    """Parse a histogram row: bucket_label  count  pct%  [count  pct%  ...]

    Bucket labels are known strings like '<1', '1-  2', '>=16K'.
    We detect a histogram row by finding count/pct pairs in the line.
    """
    line = line.strip()
    # Must start with a bucket-like char (digit, <, >)
    if not line or line[0] not in '<>=0123456789':
        return None
    # Extract all numeric (count pct%) pairs from the whole line
    values = []
    for wm in re.finditer(r'(\d+)\s+([\d.]+)%', line):
        values.append({
            'count': int(wm.group(1)),
            'pct': float(wm.group(2)),
        })
    # Also check for '-' entries (not-yet-valid windows)
    has_dash = bool(re.search(r'\s+-\s+-', line))
    if not values and not has_dash:
        return None
    return {'values': values, 'has_dash': has_dash}


def test_histogram_multi(pm_pid):
    """Verify multi-window histogram has side-by-side columns."""
    print("--- Test 10: histogram multi-window ---")

    pgbench = subprocess.Popen(
        ["pgbench", "-U", "postgres", "-d", "postgres",
         "-c", "4", "-T", "25"],
        stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL
    )

    time.sleep(3)

    output = run_tracer(pm_pid, interval=1, count=5, windows="1s,3s",
                        view="histogram",
                        extra_args=["--event", "IO:WalSync"])

    pgbench.wait()

    # Column headers present
    check("Bucket(us)" in output,
          "histogram multi-window contains 'Bucket(us)' column")
    check("Last 1s" in output,
          "histogram multi-window contains 'Last 1s' column")
    check("Last 3s" in output,
          "histogram multi-window contains 'Last 3s' column")

    # Event name in header
    check("IO:WalSync" in output,
          "histogram multi-window shows event name")

    # No Cumulative column (that's only in single-window mode)
    cum_count = output.count("Cumulative")
    check(cum_count == 0,
          f"histogram multi-window has no Cumulative column (found {cum_count})")

    # No ASCII bar (only in single-window mode)
    bar_count = output.count("####")
    check(bar_count == 0,
          f"histogram multi-window has no ASCII bar (found {bar_count})")

    # Progressive population: find a tick where first window has data,
    # second shows '-'
    ticks = split_ticks(output)
    check(len(ticks) >= 3,
          f"histogram got {len(ticks)} ticks (expected >= 3)")

    # Last tick should have data in both windows
    if len(ticks) >= 4:
        last_tick = ticks[-1]
        # Look for at least one row with two count/pct pairs
        has_both = False
        for line in last_tick.split('\n'):
            row = parse_histogram_row(line)
            if row and len(row['values']) >= 2:
                has_both = True
                break
        check(has_both,
              "histogram last tick has data in both windows")

    # Percentages in each window should sum to ~100%
    if len(ticks) >= 4:
        last_tick = ticks[-1]
        # Collect all pct values for the first window
        pct_sums = [0.0, 0.0]
        for line in last_tick.split('\n'):
            row = parse_histogram_row(line)
            if row:
                for wi, v in enumerate(row['values']):
                    if wi < 2:
                        pct_sums[wi] += v['pct']

        if pct_sums[0] > 0:
            check(80 < pct_sums[0] < 120,
                  f"histogram window 1 percentages sum to {pct_sums[0]:.1f}% "
                  f"(expected ~100%)")
        if pct_sums[1] > 0:
            check(80 < pct_sums[1] < 120,
                  f"histogram window 2 percentages sum to {pct_sums[1]:.1f}% "
                  f"(expected ~100%)")


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

    print(f"=== test_multi_window (postmaster PID {pm_pid}) ===")

    test_format(pm_pid)
    test_progressive_population(pm_pid)
    test_internal_consistency(pm_pid)
    test_system_event_format(pm_pid)
    test_system_event_progressive(pm_pid)
    test_system_event_data(pm_pid)
    test_query_event_mode_a(pm_pid)
    test_query_event_mode_b(pm_pid)
    test_query_event_mode_c(pm_pid)
    test_histogram_multi(pm_pid)

    print(f"\n{tests_passed}/{tests_run} tests passed")
    sys.exit(0 if tests_failed == 0 else 1)


if __name__ == '__main__':
    main()
