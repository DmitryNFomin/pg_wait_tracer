#!/usr/bin/env python3
"""test_data_queries.py — Per-query attribution + filtered class_ns (Bug 4 regression).

Verifies that top_queries correctly attributes time per query_id,
and that class filters only report the filtered class in class breakdown.

#459 (second half of this file): the exec/plan lifecycle columns
(exec_count, exec_total_ms, avg_exec_ms) through the REAL pgwt-server, on a
trace with a NESTED execution. pgwt_compute_query_lifecycle used to keep one
open execution timestamp and one query id per pid, so an inner EXEC_START
overwrote the outer one and the outer EXEC_END contributed nothing: the
Queries tab showed the inner statement's duration as the whole query's Avg
Exec. tests/test_pid_cap.c section 5 pins the computation; this pins the
SERVED RESPONSE, because a correct computation reached through a wrong
handler is still a wrong tab.
"""
import os
import sys

sys.path.insert(0, os.path.dirname(__file__))
from server_harness import (
    ServerHarness, generate_traces, cleanup_traces, TestRunner,
    CPU, IO_DATA_FILE_READ, IO_WAL_SYNC, LOCK_RELATION,
)

BASE_TS = 10_000_000_000_000
MS = 1_000_000

MARKER_EXEC_START = 0xFFFFFFF0
MARKER_EXEC_END = 0xFFFFFFF1
MARKER_PLAN_START = 0xFFFFFFF2
MARKER_PLAN_END = 0xFFFFFFF3
MARKER_CMD_END = 0xFFFFFFF7

# Nested query, flat control query, and a query that re-plans mid-execution.
QID_NESTED = 500
QID_FLAT = 600
QID_REPLAN = 700
QID_REPLAN_INNER = 701


def build_scenario():
    """2 queries with known wait profiles.

    Query 100: CPU 2ms + IO:Read 8ms = 10ms total
    Query 200: CPU 1ms + Lock 4ms + IO:WalSync 5ms = 10ms total
    """
    events = []
    ts = BASE_TS

    # Query 100: CPU 2ms
    ts += 2_000_000
    events.append({"pid": 1000, "ts": ts, "dur": 2_000_000,
                    "old": CPU, "new": IO_DATA_FILE_READ, "qid": 100})
    # Query 100: IO 8ms
    ts += 8_000_000
    events.append({"pid": 1000, "ts": ts, "dur": 8_000_000,
                    "old": IO_DATA_FILE_READ, "new": CPU, "qid": 100})

    # Query 200: CPU 1ms
    ts2 = BASE_TS + 500_000
    ts2 += 1_000_000
    events.append({"pid": 1001, "ts": ts2, "dur": 1_000_000,
                    "old": CPU, "new": LOCK_RELATION, "qid": 200})
    # Query 200: Lock 4ms
    ts2 += 4_000_000
    events.append({"pid": 1001, "ts": ts2, "dur": 4_000_000,
                    "old": LOCK_RELATION, "new": IO_WAL_SYNC, "qid": 200})
    # Query 200: IO:WalSync 5ms
    ts2 += 5_000_000
    events.append({"pid": 1001, "ts": ts2, "dur": 5_000_000,
                    "old": IO_WAL_SYNC, "new": CPU, "qid": 200})

    return {
        "backends": [
            {"pid": 1000, "type": "client", "user": "test", "db": "testdb"},
            {"pid": 1001, "type": "client", "user": "test", "db": "testdb"},
        ],
        "queries": [
            {"id": 100, "text": "SELECT pg_sleep(0.01)"},
            {"id": 200, "text": "UPDATE t SET x = 1"},
        ],
        "events": events,
    }


def _marker(pid, ts, mk, qid):
    return {"pid": pid, "ts": ts, "dur": 0, "old": mk, "new": mk, "qid": qid}


def _wait(pid, ts, dur, qid):
    return {"pid": pid, "ts": ts, "dur": dur,
            "old": IO_DATA_FILE_READ, "new": CPU, "qid": qid}


def build_nested_scenario():
    """#459's server fixture.

    QID_NESTED (pid 1000): outer execution [0,100] ms with an inner one
    [20,30] ms strictly inside it — the same query, as plpgsql EXECUTE of a
    prepared statement produces. Correct answer: exec_count 2,
    exec_total_ms 110, avg_exec_ms 55 (inclusive spans, not additive DB
    Time). Pre-#459: exec_count 1, avg_exec_ms 10 — the inner statement
    reported as the whole query.

    QID_FLAT (pid 1001): two DISJOINT executions, 100 ms then 10 ms. Same
    count, same total, same average as QID_NESTED's correct answer — so it
    is the control that CANNOT see the defect, and the assertion below is
    that the two queries agree. Pre-#459 they did not.

    QID_REPLAN (pid 1002): one execution [0,100] ms with a PLAN of a
    DIFFERENT query (QID_REPLAN_INNER) nested inside it. The shared qid slot
    re-keyed the open execution, so the 100 ms was credited to the planned
    query and QID_REPLAN's row carried no exec_count key at all.

    Each execution contains a real IO wait, because top_queries' rows come
    from wait/CPU accounting: a query with markers but no waits gets no row,
    and the lifecycle columns would then have nothing to hang off.
    """
    b = BASE_TS
    events = [
        # ── QID_NESTED, pid 1000 ──
        _marker(1000, b, MARKER_EXEC_START, QID_NESTED),
        _wait(1000, b + 5 * MS, 5 * MS, QID_NESTED),
        _marker(1000, b + 20 * MS, MARKER_EXEC_START, QID_NESTED),
        _wait(1000, b + 28 * MS, 5 * MS, QID_NESTED),
        _marker(1000, b + 30 * MS, MARKER_EXEC_END, QID_NESTED),
        _wait(1000, b + 60 * MS, 5 * MS, QID_NESTED),
        _marker(1000, b + 100 * MS, MARKER_EXEC_END, QID_NESTED),
        _marker(1000, b + 101 * MS, MARKER_CMD_END, QID_NESTED),
        # ── QID_FLAT, pid 1001: 100 ms then 10 ms, disjoint ──
        _marker(1001, b + 200 * MS, MARKER_EXEC_START, QID_FLAT),
        _wait(1001, b + 205 * MS, 5 * MS, QID_FLAT),
        _marker(1001, b + 300 * MS, MARKER_EXEC_END, QID_FLAT),
        _marker(1001, b + 301 * MS, MARKER_CMD_END, QID_FLAT),
        _marker(1001, b + 320 * MS, MARKER_EXEC_START, QID_FLAT),
        _wait(1001, b + 325 * MS, 5 * MS, QID_FLAT),
        _marker(1001, b + 330 * MS, MARKER_EXEC_END, QID_FLAT),
        _marker(1001, b + 331 * MS, MARKER_CMD_END, QID_FLAT),
        # ── QID_REPLAN, pid 1002: a plan of another query inside it ──
        _marker(1002, b + 400 * MS, MARKER_EXEC_START, QID_REPLAN),
        _marker(1002, b + 410 * MS, MARKER_PLAN_START, QID_REPLAN_INNER),
        _marker(1002, b + 415 * MS, MARKER_PLAN_END, QID_REPLAN_INNER),
        _wait(1002, b + 420 * MS, 5 * MS, QID_REPLAN),
        _marker(1002, b + 500 * MS, MARKER_EXEC_END, QID_REPLAN),
        _marker(1002, b + 501 * MS, MARKER_CMD_END, QID_REPLAN),
    ]
    return {
        "backends": [
            {"pid": 1000, "type": "client", "user": "test", "db": "testdb"},
            {"pid": 1001, "type": "client", "user": "test", "db": "testdb"},
            {"pid": 1002, "type": "client", "user": "test", "db": "testdb"},
        ],
        "queries": [
            {"id": QID_NESTED, "text": "CALL nest()"},
            {"id": QID_FLAT, "text": "SELECT flat()"},
            {"id": QID_REPLAN, "text": "SELECT replanned()"},
            {"id": QID_REPLAN_INNER, "text": "SELECT inner_plan()"},
        ],
        "events": events,
    }


def check_nested_lifecycle(t):
    """#459: the served Queries-tab columns over a nested execution."""
    trace_dir = generate_traces(build_nested_scenario())
    try:
        with ServerHarness(trace_dir) as srv:
            resp = srv.query("top_queries")
            rows = resp.get("rows", [])
            by_qid = {int(r["query_id"]): r for r in rows}

            print("--- #459 nested execution ---")
            # The gate must not pass on an empty response.
            t.check(len(rows) >= 3,
                    f"top_queries returned the three queries (got {len(rows)} "
                    f"rows: {sorted(by_qid)})")

            nested = by_qid.get(QID_NESTED)
            t.check(nested is not None, "nested query has a row")
            if nested is not None:
                t.check_eq(nested.get("exec_count"), 2,
                           "nested: exec_count = 2 (pre-#459: 1 — the outer "
                           "EXEC_END contributed nothing)")
                t.check_approx(nested.get("exec_total_ms", -1), 110.0, 1e-6,
                               "nested: exec_total_ms = 110 (100 outer + 10 "
                               "inner; inclusive spans, not DB Time)")
                t.check_approx(nested.get("avg_exec_ms", -1), 55.0, 1e-6,
                               "nested: avg_exec_ms = 55 (pre-#459: 10, the "
                               "INNER statement's duration)")

            flat = by_qid.get(QID_FLAT)
            t.check(flat is not None, "flat control query has a row")
            if flat is not None and nested is not None:
                t.check_eq(flat.get("exec_count"), 2,
                           "flat control: exec_count = 2")
                t.check_approx(flat.get("exec_total_ms", -1), 110.0, 1e-6,
                               "flat control: exec_total_ms = 110")
                # THE differential: the flat query was already right before
                # the fix, so "nested agrees with flat" is exactly the
                # assertion the defect broke.
                t.check_eq(nested.get("exec_count"), flat.get("exec_count"),
                           "nested and flat agree on exec_count — the whole "
                           "defect was that they did not")
                t.check_approx(nested.get("avg_exec_ms", -1),
                               flat.get("avg_exec_ms", -2), 1e-6,
                               "nested and flat agree on avg_exec_ms")

            replan = by_qid.get(QID_REPLAN)
            t.check(replan is not None, "re-planning query has a row")
            if replan is not None:
                t.check_eq(replan.get("exec_count"), 1,
                           "re-plan: exec_count = 1 (pre-#459: the key was "
                           "ABSENT — the execution had been re-keyed to the "
                           "planned query)")
                t.check_approx(replan.get("avg_exec_ms", -1), 100.0, 1e-6,
                               "re-plan: avg_exec_ms = 100, the executing "
                               "query's own span")
                t.check_eq(replan.get("plan_count"), 0,
                           "re-plan: no planning charged to the executing "
                           "query")
            inner = by_qid.get(QID_REPLAN_INNER)
            if inner is not None:
                t.check_eq(inner.get("exec_count"), 0,
                           "the PLANNED query is charged no execution "
                           "(pre-#459: one, of 100 ms)")
    finally:
        cleanup_traces(trace_dir)


def main():
    t = TestRunner("test_data_queries")
    print(f"=== {t.name} ===")

    scenario = build_scenario()
    trace_dir = generate_traces(scenario)

    try:
        with ServerHarness(trace_dir) as srv:
            # Unfiltered query list
            resp = srv.query("top_queries")
            rows = resp.get("rows", [])
            by_qid = {str(r["query_id"]): r for r in rows}

            print("--- Unfiltered ---")
            q100 = by_qid.get("100", {})
            t.check_approx(q100.get("total_ms", -1), 10.0, 0.001,
                           "Query 100 total = 10ms")

            q200 = by_qid.get("200", {})
            t.check_approx(q200.get("total_ms", -1), 10.0, 0.001,
                           "Query 200 total = 10ms")

            # Bug 4 regression: filter by IO class
            print("--- Filtered by IO class ---")
            resp_io = srv.query("top_queries", filters={"class": "io"})
            rows_io = resp_io.get("rows", [])
            by_qid_io = {str(r["query_id"]): r for r in rows_io}

            q100_io = by_qid_io.get("100", {})
            t.check_approx(q100_io.get("total_ms", -1), 8.0, 0.001,
                           "Query 100 IO-only = 8ms")

            q200_io = by_qid_io.get("200", {})
            t.check_approx(q200_io.get("total_ms", -1), 5.0, 0.001,
                           "Query 200 IO-only = 5ms")

            # Verify filtered by Lock class
            print("--- Filtered by Lock class ---")
            resp_lock = srv.query("top_queries", filters={"class": "lock"})
            rows_lock = resp_lock.get("rows", [])
            by_qid_lock = {str(r["query_id"]): r for r in rows_lock}

            # Query 100 has no Lock events — should not appear
            t.check("100" not in by_qid_lock,
                    "Query 100 absent when filtered by Lock")

            q200_lock = by_qid_lock.get("200", {})
            t.check_approx(q200_lock.get("total_ms", -1), 4.0, 0.001,
                           "Query 200 Lock-only = 4ms")

    finally:
        cleanup_traces(trace_dir)

    check_nested_lifecycle(t)

    sys.exit(0 if t.summary() else 1)


if __name__ == "__main__":
    main()
