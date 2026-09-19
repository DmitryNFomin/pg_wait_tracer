#!/usr/bin/env python3
"""test_data_events.py — 0B.3: count, total, avg, max, percentiles (exact).

Generates events with known counts and durations, verifies top_events output.
"""
import os
import sys

sys.path.insert(0, os.path.dirname(__file__))
from server_harness import (
    ServerHarness, generate_traces, cleanup_traces, TestRunner,
    CPU, IO_DATA_FILE_READ, LOCK_RELATION, LWLOCK_WAL_WRITE,
)

BASE_TS = 10_000_000_000_000


def build_scenario():
    """Generate events with precise counts and durations.

    IO:DataFileRead: 10 events, each 1ms = total 10ms, avg 1ms, max 1ms
    Lock:relation:   5 events, durations 1ms,2ms,3ms,4ms,5ms = total 15ms, avg 3ms, max 5ms
    CPU:             20 events, each 500us = total 10ms, avg 500us, max 500us
    """
    events = []
    ts = BASE_TS

    # IO:DataFileRead — 10 events × 1ms
    for i in range(10):
        ts += 1_000_000
        events.append({"pid": 1000, "ts": ts, "dur": 1_000_000,
                        "old": IO_DATA_FILE_READ, "new": CPU, "qid": 100})

    # Lock:relation — 5 events with varying durations
    for i, dur_ms in enumerate([1, 2, 3, 4, 5]):
        dur_ns = dur_ms * 1_000_000
        ts += dur_ns
        events.append({"pid": 1000, "ts": ts, "dur": dur_ns,
                        "old": LOCK_RELATION, "new": CPU, "qid": 100})

    # CPU — 20 events × 500us
    for i in range(20):
        ts += 500_000
        events.append({"pid": 1000, "ts": ts, "dur": 500_000,
                        "old": CPU, "new": IO_DATA_FILE_READ, "qid": 100})

    return {
        "backends": [{"pid": 1000, "type": "client", "user": "test", "db": "testdb"}],
        "queries": [{"id": 100, "text": "SELECT 1"}],
        "events": events,
    }


def build_overflow_scenario():
    """#103: percentiles that land in the histogram's OPEN-ENDED top bucket.

    The latency histogram's last bucket holds everything above 16384 us, so a
    percentile there is only a lower bound. The server must say so per
    percentile — it cannot be inferred downstream, because bucket 14
    (8192..16383 us) reports the same 16384 as the open-ended bucket 15.

      Lock:relation     5 x 100 ms   -> every percentile in the top bucket
      LWLock:WALWrite   5 x 16383 us -> bucket 14: SAME 16384, NOT overflow
      IO:DataFileRead  10 x 1 ms     -> ordinary bucket, nowhere near the top
    """
    events = []
    ts = BASE_TS

    for _ in range(5):
        ts += 100_000_000
        events.append({"pid": 1000, "ts": ts, "dur": 100_000_000,
                       "old": LOCK_RELATION, "new": CPU, "qid": 100})

    for _ in range(5):
        ts += 16_383_000
        events.append({"pid": 1000, "ts": ts, "dur": 16_383_000,
                       "old": LWLOCK_WAL_WRITE, "new": CPU, "qid": 100})

    for _ in range(10):
        ts += 1_000_000
        events.append({"pid": 1000, "ts": ts, "dur": 1_000_000,
                       "old": IO_DATA_FILE_READ, "new": CPU, "qid": 100})

    return {
        "backends": [{"pid": 1000, "type": "client", "user": "test", "db": "testdb"}],
        "queries": [{"id": 100, "text": "SELECT 1"}],
        "events": events,
    }


def check_percentile_overflow(t):
    """top_events flags percentiles that saturated the top bucket (#103)."""
    trace_dir = generate_traces(build_overflow_scenario())
    try:
        with ServerHarness(trace_dir) as srv:
            rows = {r["name"]: r for r in srv.query("top_events").get("rows", [])}

            print("--- #103 overflow flags ---")
            lock = rows.get("Lock:relation", {})
            t.check_approx(lock.get("max_us", -1), 100000.0, 0.01,
                           "Lock max = 100ms (well past the top bucket)")
            for pctl in ("p50", "p95", "p99"):
                t.check_approx(lock.get(pctl + "_us", -1), 16384.0, 0.01,
                               f"Lock {pctl} = 16384us (top-bucket edge)")
                t.check_eq(lock.get(pctl + "_overflow"), True,
                           f"Lock {pctl}_overflow = true")

            # The ambiguity the flag exists for: identical 16384 us value,
            # but these waits are INSIDE the histogram (bucket 14).
            lw = rows.get("LWLock:WALWrite", {})
            t.check_approx(lw.get("max_us", -1), 16383.0, 0.01,
                           "LWLock max = 16383us (last in-range bucket)")
            for pctl in ("p50", "p95", "p99"):
                t.check_approx(lw.get(pctl + "_us", -1), 16384.0, 0.01,
                               f"LWLock {pctl} = 16384us (same number...)")
                t.check_eq(lw.get(pctl + "_overflow"), False,
                           f"LWLock {pctl}_overflow = false (...different meaning)")

            io = rows.get("IO:DataFileRead", {})
            for pctl in ("p50", "p95", "p99"):
                t.check_eq(io.get(pctl + "_overflow"), False,
                           f"IO:Read {pctl}_overflow = false")
    finally:
        cleanup_traces(trace_dir)


def build_single_wait_scenario():
    """#103 review (nearest-rank): ONE wait per event, nothing else.

    With the old floor() threshold a single observation asked for rank 0,
    which bucket 0 satisfied vacuously: one 100 ms wait reported P50 = 1 us
    with no overflow flag. Nearest-rank (ceil, min 1) must answer with the
    bucket the observation is actually in.

      Lock:relation    1 x 100 ms  -> open-ended top bucket (>= 16384 us)
      LWLock:WALWrite  1 x 5 ms    -> bucket 13 (4096..8191 us) -> 8192 us
    """
    ts = BASE_TS
    events = []
    ts += 100_000_000
    events.append({"pid": 1000, "ts": ts, "dur": 100_000_000,
                   "old": LOCK_RELATION, "new": CPU, "qid": 100})
    ts += 5_000_000
    events.append({"pid": 1000, "ts": ts, "dur": 5_000_000,
                   "old": LWLOCK_WAL_WRITE, "new": CPU, "qid": 100})
    return {
        "backends": [{"pid": 1000, "type": "client", "user": "test", "db": "testdb"}],
        "queries": [{"id": 100, "text": "SELECT 1"}],
        "events": events,
    }


def check_percentile_nearest_rank(t):
    """A single observation lands in its own bucket, not in bucket 0."""
    trace_dir = generate_traces(build_single_wait_scenario())
    try:
        with ServerHarness(trace_dir) as srv:
            rows = {r["name"]: r for r in srv.query("top_events").get("rows", [])}

            print("--- #103 nearest-rank (n=1) ---")
            lock = rows.get("Lock:relation", {})
            t.check_eq(lock.get("count"), 1, "Lock count = 1 (single wait)")
            for pctl in ("p50", "p95", "p99"):
                t.check_approx(lock.get(pctl + "_us", -1), 16384.0, 0.01,
                               f"one 100ms wait: {pctl} = 16384us (not 1us)")
                t.check_eq(lock.get(pctl + "_overflow"), True,
                           f"one 100ms wait: {pctl}_overflow = true")

            lw = rows.get("LWLock:WALWrite", {})
            t.check_eq(lw.get("count"), 1, "LWLock count = 1 (single wait)")
            for pctl in ("p50", "p95", "p99"):
                t.check_approx(lw.get(pctl + "_us", -1), 8192.0, 0.01,
                               f"one 5ms wait: {pctl} = 8192us (its own bucket)")
                t.check_eq(lw.get(pctl + "_overflow"), False,
                           f"one 5ms wait: {pctl}_overflow = false")
    finally:
        cleanup_traces(trace_dir)


def build_exact_300s_scenario():
    """A >= 120 s fully-EXACT window, so should_use_summaries() takes the
    summary fast path (pattern from test_data_summary_honesty.py).

    150 x IO:DataFileRead 1 ms + 150 x Lock:relation 100 ms, all under
    query 100, spread one per second over 300 s.
    """
    events = []
    ts = BASE_TS
    for i in range(150):
        ts += 1_000_000_000
        events.append({"pid": 1000, "ts": ts, "dur": 1_000_000,
                       "old": IO_DATA_FILE_READ, "new": CPU, "qid": 100})
        ts += 1_000_000_000
        events.append({"pid": 1000, "ts": ts, "dur": 100_000_000,
                       "old": LOCK_RELATION, "new": CPU, "qid": 100})
    return ({
        "backends": [{"pid": 1000, "type": "client", "user": "test", "db": "testdb"}],
        "queries": [{"id": 100, "text": "SELECT 1"}],
        "events": events,
    }, BASE_TS, ts)


def check_query_filtered_summary_gating(t):
    """#103 review BLOCKER: the summary path's per-QUERY records carry no
    histogram and no max (struct pgwt_summary_query_event = event_id, count,
    total_ns). Answering a percentile from that all-zero histogram made every
    row of a query-drilled Events table read ">= 16.4ms" with Max 0us — a
    fabricated bound. A row with no distribution must gate p50/p95/p99 AND
    max to null (count/total/avg stay, they are real)."""
    scenario, lo, hi = build_exact_300s_scenario()
    trace_dir = generate_traces(scenario)
    try:
        with ServerHarness(trace_dir) as srv:
            frm, to = lo - 10_000_000_000, hi + 10_000_000_000   # ~320 s

            print("--- #103 summary path, UNFILTERED (system histogram) ---")
            resp = srv.query("top_events", from_=frm, to_=to)
            t.check_eq(resp.get("fidelity"), "exact",
                       "long exact window answered from summaries (exact)")
            rows = {r["name"]: r for r in resp.get("rows", [])}
            lock = rows.get("Lock:relation", {})
            io = rows.get("IO:DataFileRead", {})
            t.check_approx(lock.get("p50_us", -1), 16384.0, 0.01,
                           "unfiltered: Lock p50 = 16384us from the histogram")
            t.check_eq(lock.get("p50_overflow"), True,
                       "unfiltered: Lock p50_overflow = true (100ms waits)")
            t.check_approx(lock.get("max_us", -1), 100000.0, 0.01,
                           "unfiltered: Lock max = 100ms (summaries carry max)")
            t.check_approx(io.get("p50_us", -1), 1024.0, 0.01,
                           "unfiltered: IO p50 = 1024us (in-range bucket)")
            t.check_eq(io.get("p50_overflow"), False,
                       "unfiltered: IO p50_overflow = false")

            print("--- #103 summary path, QUERY-FILTERED (no histogram) ---")
            qresp = srv.query("top_events", from_=frm, to_=to,
                              filters={"query_id": 100})
            qrows = {r["name"]: r for r in qresp.get("rows", [])}
            t.check(len(qrows) > 0,
                    f"query-filtered drill returns rows (got {len(qrows)})")
            for name, row in sorted(qrows.items()):
                t.check(row.get("count", 0) > 0,
                        f"{name}: count survives the drill ({row.get('count')})")
                t.check(row.get("avg_us") is not None,
                        f"{name}: avg_us stays (count and total are real)")
                for col in ("p50_us", "p95_us", "p99_us", "max_us"):
                    t.check(row.get(col, "missing") is None,
                            f"{name}: {col} is null (no histogram behind it)")
                for col in ("p50_overflow", "p95_overflow", "p99_overflow"):
                    t.check_eq(row.get(col), False,
                               f"{name}: {col} = false (never a fabricated bound)")
    finally:
        cleanup_traces(trace_dir)


def main():
    t = TestRunner("test_data_events")
    print(f"=== {t.name} ===")

    scenario = build_scenario()
    trace_dir = generate_traces(scenario)

    try:
        with ServerHarness(trace_dir) as srv:
            resp = srv.query("top_events")
            rows_by_name = {}
            for r in resp.get("rows", []):
                rows_by_name[r["name"]] = r

            print("--- IO:DataFileRead ---")
            io = rows_by_name.get("IO:DataFileRead", {})
            t.check_eq(io.get("count"), 10, "IO:Read count = 10")
            t.check_approx(io.get("total_ms", -1), 10.0, 0.001,
                           "IO:Read total = 10ms")
            t.check_approx(io.get("avg_us", -1), 1000.0, 0.01,
                           "IO:Read avg = 1000us")
            t.check_approx(io.get("max_us", -1), 1000.0, 0.01,
                           "IO:Read max = 1000us")

            print("--- Lock:relation ---")
            lock = rows_by_name.get("Lock:relation", {})
            t.check_eq(lock.get("count"), 5, "Lock count = 5")
            t.check_approx(lock.get("total_ms", -1), 15.0, 0.001,
                           "Lock total = 15ms")
            t.check_approx(lock.get("avg_us", -1), 3000.0, 0.01,
                           "Lock avg = 3000us")
            t.check_approx(lock.get("max_us", -1), 5000.0, 0.01,
                           "Lock max = 5000us")

            print("--- CPU ---")
            cpu = rows_by_name.get("CPU*", {})
            t.check_eq(cpu.get("count"), 20, "CPU count = 20")
            t.check_approx(cpu.get("total_ms", -1), 10.0, 0.001,
                           "CPU total = 10ms")
            t.check_approx(cpu.get("avg_us", -1), 500.0, 0.01,
                           "CPU avg = 500us")

            print("--- Percentages ---")
            # Total = 10 + 15 + 10 = 35ms
            t.check_approx(io.get("pct", -1), 10.0 / 35.0 * 100, 0.01,
                           "IO pct = 28.57%")
            t.check_approx(lock.get("pct", -1), 15.0 / 35.0 * 100, 0.01,
                           "Lock pct = 42.86%")
            t.check_approx(cpu.get("pct", -1), 10.0 / 35.0 * 100, 0.01,
                           "CPU pct = 28.57%")

    finally:
        cleanup_traces(trace_dir)

    check_percentile_overflow(t)
    check_percentile_nearest_rank(t)
    check_query_filtered_summary_gating(t)

    sys.exit(0 if t.summary() else 1)


if __name__ == "__main__":
    main()
