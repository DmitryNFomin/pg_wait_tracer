#!/usr/bin/env python3
"""test_data_filters.py — 0B.4: all filters independently and combined.

Verifies class, event_id, pid, and query_id filters work correctly.
"""
import os
import sys

sys.path.insert(0, os.path.dirname(__file__))
from server_harness import (
    ServerHarness, generate_traces, cleanup_traces, TestRunner,
    CPU, IO_DATA_FILE_READ, IO_WAL_SYNC, LOCK_RELATION,
)

BASE_TS = 10_000_000_000_000


def build_scenario():
    """2 PIDs × 2 queries × mixed events.

    PID 1000, QID 100: IO:Read 3ms + CPU 2ms
    PID 1000, QID 200: Lock 4ms
    PID 1001, QID 100: IO:WalSync 5ms
    PID 1001, QID 200: CPU 1ms + IO:Read 6ms
    """
    events = []
    ts = BASE_TS

    # PID 1000, QID 100
    ts += 3_000_000
    events.append({"pid": 1000, "ts": ts, "dur": 3_000_000,
                    "old": IO_DATA_FILE_READ, "new": CPU, "qid": 100})
    ts += 2_000_000
    events.append({"pid": 1000, "ts": ts, "dur": 2_000_000,
                    "old": CPU, "new": LOCK_RELATION, "qid": 100})

    # PID 1000, QID 200
    ts += 4_000_000
    events.append({"pid": 1000, "ts": ts, "dur": 4_000_000,
                    "old": LOCK_RELATION, "new": CPU, "qid": 200})

    # PID 1001, QID 100
    ts2 = BASE_TS + 100_000
    ts2 += 5_000_000
    events.append({"pid": 1001, "ts": ts2, "dur": 5_000_000,
                    "old": IO_WAL_SYNC, "new": CPU, "qid": 100})

    # PID 1001, QID 200
    ts2 += 1_000_000
    events.append({"pid": 1001, "ts": ts2, "dur": 1_000_000,
                    "old": CPU, "new": IO_DATA_FILE_READ, "qid": 200})
    ts2 += 6_000_000
    events.append({"pid": 1001, "ts": ts2, "dur": 6_000_000,
                    "old": IO_DATA_FILE_READ, "new": CPU, "qid": 200})

    return {
        "backends": [
            {"pid": 1000, "type": "client", "user": "test", "db": "testdb"},
            {"pid": 1001, "type": "client", "user": "test", "db": "testdb"},
        ],
        "queries": [
            {"id": 100, "text": "SELECT 1"},
            {"id": 200, "text": "UPDATE t SET x=1"},
        ],
        "events": events,
    }


def get_db_time(resp):
    """Extract DB Time ms from time_model response."""
    for r in resp.get("rows", []):
        if r["name"] == "DB Time":
            return r["ms"]
    return -1


def main():
    t = TestRunner("test_data_filters")
    print(f"=== {t.name} ===")

    scenario = build_scenario()
    trace_dir = generate_traces(scenario)

    try:
        with ServerHarness(trace_dir) as srv:
            # Unfiltered total: 3+2+4+5+1+6 = 21ms
            resp = srv.query("time_model")
            total = get_db_time(resp)
            print("--- Unfiltered ---")
            t.check_approx(total, 21.0, 0.001, "Total DB Time = 21ms")

            # #345: an explicit zero is CPU, not an absent event filter.
            print("--- Filter: event_id=CPU (0) ---")
            for extra, expected in (({}, 3.0), ({"pid": 1000}, 2.0),
                                    ({"query_id": "200"}, 1.0)):
                resp = srv.query("time_model", filters={"event_id": CPU, **extra})
                t.check_eq(get_db_time(resp), expected,
                           f"CPU DB Time with {extra} = {expected}ms")
            resp = srv.query("top_events", filters={"event_id": CPU})
            t.check_eq([r["event_id"] for r in resp.get("rows", [])], [CPU],
                       "CPU filter returns only the CPU event")
            # Wide windows exercise routing away from summaries that lack
            # the requested dimension; this fixture contains no summaries.
            for cmd in ("top_sessions", "top_queries"):
                resp = srv.query(cmd, from_=BASE_TS - 60_000_000_000,
                                 to_=BASE_TS + 180_000_000_000,
                                 filters={"event_id": CPU})
                db_time = (sum(r["db_time_ms"] for r in resp.get("rows", []))
                           if cmd == "top_sessions" else resp.get("db_time_ms"))
                t.check_eq(db_time, 3.0,
                           f"{cmd} routes wide CPU requests to raw data")
            resp = srv.query("time_model")
            t.check_eq(get_db_time(resp), total,
                       "absent event filter still returns the original total")

            # Filter by class: IO = 3+5+6 = 14ms
            print("--- Filter: class=io ---")
            resp = srv.query("time_model", filters={"class": "io"})
            t.check_approx(get_db_time(resp), 14.0, 0.001,
                           "IO class DB Time = 14ms")

            # Filter by class: Lock = 4ms
            print("--- Filter: class=lock ---")
            resp = srv.query("time_model", filters={"class": "lock"})
            t.check_approx(get_db_time(resp), 4.0, 0.001,
                           "Lock class DB Time = 4ms")

            # Filter by event_id: IO:DataFileRead = 3+6 = 9ms
            print("--- Filter: event_id=IO:DataFileRead ---")
            resp = srv.query("time_model",
                             filters={"event_id": IO_DATA_FILE_READ})
            t.check_approx(get_db_time(resp), 9.0, 0.001,
                           "IO:DataFileRead DB Time = 9ms")

            # Filter by PID: 1000 = 3+2+4 = 9ms
            print("--- Filter: pid=1000 ---")
            resp = srv.query("time_model", filters={"pid": 1000})
            t.check_approx(get_db_time(resp), 9.0, 0.001,
                           "PID 1000 DB Time = 9ms")

            # Filter by PID: 1001 = 5+1+6 = 12ms
            print("--- Filter: pid=1001 ---")
            resp = srv.query("time_model", filters={"pid": 1001})
            t.check_approx(get_db_time(resp), 12.0, 0.001,
                           "PID 1001 DB Time = 12ms")

            # Filter by query_id: 100 = 3+2+5 = 10ms
            print("--- Filter: query_id=100 ---")
            resp = srv.query("time_model",
                             filters={"query_id": "100"})
            t.check_approx(get_db_time(resp), 10.0, 0.001,
                           "QID 100 DB Time = 10ms")

            # Combined: pid=1001 + class=io = 5+6 = 11ms
            print("--- Filter: pid=1001 + class=io ---")
            resp = srv.query("time_model",
                             filters={"pid": 1001, "class": "io"})
            t.check_approx(get_db_time(resp), 11.0, 0.001,
                           "PID 1001 + IO = 11ms")

            # Combined: query_id=200 + pid=1000 = Lock 4ms
            print("--- Filter: query_id=200 + pid=1000 ---")
            resp = srv.query("time_model",
                             filters={"query_id": "200", "pid": 1000})
            t.check_approx(get_db_time(resp), 4.0, 0.001,
                           "QID 200 + PID 1000 = 4ms")

    finally:
        cleanup_traces(trace_dir)

    sys.exit(0 if t.summary() else 1)


if __name__ == "__main__":
    main()
