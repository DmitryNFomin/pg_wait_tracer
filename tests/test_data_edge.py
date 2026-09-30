#!/usr/bin/env python3
"""test_data_edge.py — 0B.8: empty, single, zero duration, overflow.

Edge cases that should not crash pgwt-server.
"""
import os
import sys
import tempfile
import shutil

sys.path.insert(0, os.path.dirname(__file__))
from server_harness import (
    ServerHarness, generate_traces, cleanup_traces, TestRunner,
    CPU, IO_DATA_FILE_READ,
)

BASE_TS = 10_000_000_000_000


def test_single_event(t):
    """Single event — all commands should return valid results."""
    print("--- Single event ---")
    scenario = {
        "backends": [{"pid": 1000, "type": "client", "user": "test", "db": "testdb"}],
        "queries": [{"id": 100, "text": "SELECT 1"}],
        "events": [
            {"pid": 1000, "ts": BASE_TS + 1_000_000, "dur": 1_000_000,
             "old": CPU, "new": IO_DATA_FILE_READ, "qid": 100},
        ],
    }
    trace_dir = generate_traces(scenario)
    try:
        with ServerHarness(trace_dir) as srv:
            for cmd in ["time_model", "top_events", "top_sessions",
                        "top_queries", "session_timeline", "info"]:
                resp = srv.query(cmd)
                t.check("error" not in resp,
                        f"'{cmd}' succeeds with single event")
    finally:
        cleanup_traces(trace_dir)


def test_zero_duration(t):
    """Zero-duration event — should not cause division by zero."""
    print("--- Zero duration ---")
    scenario = {
        "backends": [{"pid": 1000, "type": "client", "user": "test", "db": "testdb"}],
        "queries": [{"id": 100, "text": "SELECT 1"}],
        "events": [
            {"pid": 1000, "ts": BASE_TS, "dur": 0,
             "old": CPU, "new": IO_DATA_FILE_READ, "qid": 100},
            {"pid": 1000, "ts": BASE_TS + 1_000_000, "dur": 1_000_000,
             "old": IO_DATA_FILE_READ, "new": CPU, "qid": 100},
        ],
    }
    trace_dir = generate_traces(scenario)
    try:
        with ServerHarness(trace_dir) as srv:
            resp = srv.query("top_events")
            t.check("error" not in resp, "top_events handles zero duration")
            resp = srv.query("time_model")
            t.check("error" not in resp, "time_model handles zero duration")
    finally:
        cleanup_traces(trace_dir)


def test_many_pids(t):
    """50 PIDs — should not crash or truncate."""
    print("--- Many PIDs ---")
    events = []
    backends = []
    for pid in range(1000, 1050):
        events.append({
            "pid": pid,
            "ts": BASE_TS + 1_000_000,
            "dur": 1_000_000,
            "old": CPU, "new": IO_DATA_FILE_READ, "qid": 100,
        })
        backends.append({"pid": pid, "type": "client", "user": "test", "db": "testdb"})

    scenario = {
        "backends": backends,
        "queries": [{"id": 100, "text": "SELECT 1"}],
        "events": events,
    }
    trace_dir = generate_traces(scenario)
    try:
        with ServerHarness(trace_dir) as srv:
            resp = srv.query("top_sessions")
            rows = resp.get("rows", [])
            t.check(len(rows) >= 50, f"50 PIDs in top_sessions (got {len(rows)})")
    finally:
        cleanup_traces(trace_dir)


def test_large_duration(t):
    """Very large duration (1 hour) — should not overflow.

    The 1-hour wait ENDS at the trace's only timestamp, so ServerHarness's
    auto-injected window ([first − 30 s, last + 30 s], 60 s wide) contains
    only the wait's last 30 s. A windowed aggregate reports time INSIDE the
    window, so DB Time is 30,000 ms, not 3,600,000 ms: an hour of DB Time in
    a 60-second window would be AAS 60 on one session, which is the exact
    straddler defect tests/test_window_clip.c pins (Overview counted a wait
    that started before `from` in full; the AAS chart did not).

    The overflow property this case exists for is asserted on BOTH sides:
    the clipped DB Time is exactly 30 s (a wrapped uint64 would be ~1.8e10 s,
    a truncated one 0), and the LATENCY column — deliberately not clipped,
    because a wait's duration is a property of the wait — still carries the
    whole 3.6e9 µs.
    """
    print("--- Large duration ---")
    one_hour = 3_600_000_000_000  # 1 hour in ns
    scenario = {
        "backends": [{"pid": 1000, "type": "client", "user": "test", "db": "testdb"}],
        "queries": [{"id": 100, "text": "SELECT 1"}],
        "events": [
            {"pid": 1000, "ts": BASE_TS + one_hour, "dur": one_hour,
             "old": IO_DATA_FILE_READ, "new": CPU, "qid": 100},
        ],
    }
    trace_dir = generate_traces(scenario)
    try:
        with ServerHarness(trace_dir) as srv:
            resp = srv.query("time_model")
            rows = {r["name"]: r for r in resp.get("rows", [])}
            db_ms = rows.get("DB Time", {}).get("ms", -1)
            # The harness expands the window 30 s each way around the single
            # event's END timestamp, so 30 s of the hour is inside it.
            t.check_approx(db_ms, 30_000.0, 0.001,
                           "1-hour event clipped to the window: DB Time = 30,000ms")

            ev = srv.query("top_events")
            row = next((r for r in ev.get("rows", [])
                        if r.get("name", "").startswith("IO:")), None)
            t.check(row is not None, "1-hour event appears in top_events")
            if row:
                t.check_approx(row.get("total_ms", -1), 30_000.0, 0.001,
                               "top_events DB-time column is clipped too")
                t.check_approx(row.get("max_us", -1), 3_600_000_000.0, 0.001,
                               "…but max latency is the full hour (no overflow)")
    finally:
        cleanup_traces(trace_dir)


def main():
    t = TestRunner("test_data_edge")
    print(f"=== {t.name} ===")

    test_single_event(t)
    test_zero_duration(t)
    test_many_pids(t)
    test_large_duration(t)

    sys.exit(0 if t.summary() else 1)


if __name__ == "__main__":
    main()
