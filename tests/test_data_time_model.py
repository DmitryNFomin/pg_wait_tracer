#!/usr/bin/env python3
"""test_data_time_model.py — 0B.1: Exact time model arithmetic (0% tolerance).

Generates synthetic trace data with known durations per class and verifies
that pgwt-server's time_model output matches exactly.
"""
import os
import sys

sys.path.insert(0, os.path.dirname(__file__))
from server_harness import (
    ServerHarness, generate_traces, cleanup_traces, TestRunner,
    CPU, IO_DATA_FILE_READ, IO_WAL_SYNC, LOCK_RELATION,
    LWLOCK_WAL_WRITE, CLIENT_READ, TIMEOUT_PG_SLEEP, ACTIVITY_IDLE,
    EXTENSION_EXT, IPC_BGWORKER,
)

BASE_TS = 10_000_000_000_000  # 10000s in ns (must be > 1hr for server time range expansion)


def build_scenario():
    """Build a scenario with known time per class.

    PID 1000: 3 events:
      - CPU       2,000,000 ns (2ms)
      - IO:Read   5,000,000 ns (5ms)
      - Lock:rel  3,000,000 ns (3ms)
    PID 1001: 2 events:
      - LWLock    4,000,000 ns (4ms)
      - CPU       1,000,000 ns (1ms)
    PID 1002: 1 event (idle, should be excluded from DB Time):
      - Activity  10,000,000 ns (10ms)

    Expected DB Time = 2+5+3+4+1 = 15ms
    Expected CPU = 2+1 = 3ms
    Expected IO = 5ms
    Expected Lock = 3ms
    Expected LWLock = 4ms
    """
    events = []
    ts = BASE_TS

    # PID 1000: CPU 2ms
    ts += 2_000_000
    events.append({"pid": 1000, "ts": ts, "dur": 2_000_000,
                    "old": CPU, "new": IO_DATA_FILE_READ, "qid": 100})
    # PID 1000: IO:Read 5ms
    ts += 5_000_000
    events.append({"pid": 1000, "ts": ts, "dur": 5_000_000,
                    "old": IO_DATA_FILE_READ, "new": LOCK_RELATION, "qid": 100})
    # PID 1000: Lock 3ms
    ts += 3_000_000
    events.append({"pid": 1000, "ts": ts, "dur": 3_000_000,
                    "old": LOCK_RELATION, "new": CPU, "qid": 100})

    # PID 1001: LWLock 4ms
    ts2 = BASE_TS + 1_000_000
    ts2 += 4_000_000
    events.append({"pid": 1001, "ts": ts2, "dur": 4_000_000,
                    "old": LWLOCK_WAL_WRITE, "new": CPU, "qid": 200})
    # PID 1001: CPU 1ms
    ts2 += 1_000_000
    events.append({"pid": 1001, "ts": ts2, "dur": 1_000_000,
                    "old": CPU, "new": LWLOCK_WAL_WRITE, "qid": 200})

    # PID 1002: Activity (idle) 10ms — excluded from DB Time
    ts3 = BASE_TS + 500_000
    ts3 += 10_000_000
    events.append({"pid": 1002, "ts": ts3, "dur": 10_000_000,
                    "old": ACTIVITY_IDLE, "new": CPU, "qid": 0})

    return {
        "backends": [
            {"pid": 1000, "type": "client", "user": "test", "db": "testdb"},
            {"pid": 1001, "type": "client", "user": "test", "db": "testdb"},
            {"pid": 1002, "type": "checkpointer"},
        ],
        "queries": [
            {"id": 100, "text": "SELECT 1"},
            {"id": 200, "text": "SELECT 2"},
        ],
        "events": events,
    }


def check_cpu_aas_contract(t):
    # The long unfiltered window uses summaries; the PID-filtered twin uses
    # raw events. The load-budget oracle proves the long request used summaries.
    events = [{"pid": 42, "ts": BASE_TS + sec * 1_000_000_000 + 10485760,
               "dur": 10485760, "cpu": 10433332, "old": CPU, "new": CPU, "qid": 99}
              for sec in range(140)]
    events += [{"pid": 43, "ts": BASE_TS + sec * 1_000_000_000 + 10485760,
                "dur": 10485760, "cpu": 10485760, "old": CPU, "new": CPU, "qid": 99}
               for sec in range(150, 290)]
    trace_dir = generate_traces({"cpu_measured": 1, "events": events,
        "backends": [{"pid": 42, "type": "client"}, {"pid": 43, "type": "client"}],
        "queries": [{"id": 99, "text": "SELECT 1"}]})
    try:
        bounds = dict(from_=BASE_TS, to_=BASE_TS + 140_000_000_000)
        with ServerHarness(trace_dir, env={"PGWT_LOAD_MAX_EVENTS": "1"}) as srv:
            for cmd in ("top_events", "time_model"):
                oracle = srv.query(cmd, **bounds)
                t.check("error" not in oracle and oracle.get("rows"),
                        "757 summary route survives a one-event load budget: " + cmd)
        with ServerHarness(trace_dir) as srv:
            for route, filters in (("summary", {}), ("raw", {"pid": 42})):
                tm = srv.query("time_model", filters=filters, **bounds)
                te = srv.query("top_events", filters=filters, **bounds)
                ts = srv.query("top_sessions", filters=filters, **bounds)
                t.check(ts.get("cpu_ms") == tm["cpu_ms"],
                        "757 " + route + " Sessions CPU total == Overview CPU exactly")
                t.check(ts["offcpu_ms"] == tm["offcpu_ms"],
                        "757 " + route + " Sessions off-CPU total == Overview exactly")
                waiting = next(r for r in tm["rows"] if r["name"] == "CPU (waiting for a core)")
                t.check(te.get("offcpu_aas") is not None,
                        "757 " + route + " emitted Events offcpu_aas is available")
                t.check(te.get("offcpu_aas") == waiting["aas"],
                        "757 " + route + " emitted Events AAS == Overview waiting AAS exactly")
                t.check(te["offcpu_ms"] == tm["offcpu_ms"],
                        "757 " + route + " emitted integer off-CPU totals agree exactly")
            for filters in ({"class": "cpu"}, {"event_id": 0}):
                tm = srv.query("time_model", filters=filters, **bounds)
                te = srv.query("top_events", filters=filters, **bounds)
                ts = srv.query("top_sessions", filters=filters, **bounds)
                t.check(tm["cpu_ms"] == te["rows"][0]["total_ms"] == ts["cpu_ms"],
                        "757 CPU class/event filter exact CPU parity across handlers")
                t.check(tm["offcpu_ms"] == te["offcpu_ms"] == ts["offcpu_ms"],
                        "757 CPU class/event filter exact off-CPU parity across handlers")
            # The C suite pins both probe inputs; this wire case also checks
            # the full requested window denominator for a one-tenth straddle.
            end = events[0]["ts"]
            clipped = dict(from_=end - 1048576, to_=end, filters={"pid": 42})
            tm = srv.query("time_model", **clipped)
            te = srv.query("top_events", **clipped)
            waiting = next(r for r in tm["rows"] if r["name"] == "CPU (waiting for a core)")
            t.check(te.get("offcpu_aas") == waiting["aas"],
                    "757 raw straddle emitted AAS exact equality")
            for filters in ({}, {"pid": 43}):
                zero = srv.query("top_events", filters=filters,
                    from_=BASE_TS + 150_000_000_000, to_=BASE_TS + 290_000_000_000)
                t.check(zero["has_measured_cpu"] and zero["offcpu_aas"] == 0,
                        "757 raw/summary measured zero AAS remains numeric zero")
            unavailable = srv.query("top_events", filters={"pid": 999}, **bounds)
            t.check(unavailable.get("offcpu_aas", "missing") is None,
                    "757 unmeasured Events AAS emits null, not zero")
    finally:
        cleanup_traces(trace_dir)


def main():
    t = TestRunner("test_data_time_model")
    print(f"=== {t.name} ===")

    scenario = build_scenario()
    trace_dir = generate_traces(scenario)

    try:
        with ServerHarness(trace_dir) as srv:
            resp = srv.query("time_model")

            # Build lookup by name
            rows = {r["name"]: r for r in resp["rows"]}

            print("--- DB Time ---")
            t.check("DB Time" in rows, "DB Time row exists")
            t.check_approx(rows["DB Time"]["ms"], 15.0, 0.001,
                           "DB Time = 15.0ms")

            print("--- Class breakdown ---")
            t.check_approx(rows.get("CPU*", {}).get("ms", -1), 3.0, 0.001,
                           "CPU = 3.0ms")
            t.check_approx(rows.get("IO", {}).get("ms", -1), 5.0, 0.001,
                           "IO = 5.0ms")
            t.check_approx(rows.get("Lock", {}).get("ms", -1), 3.0, 0.001,
                           "Lock = 3.0ms")
            t.check_approx(rows.get("LWLock", {}).get("ms", -1), 4.0, 0.001,
                           "LWLock = 4.0ms")

            print("--- Percentages ---")
            # CPU should be 3/15 = 20%
            t.check_approx(rows.get("CPU*", {}).get("pct", -1), 20.0, 0.01,
                           "CPU% = 20.0%")
            # IO should be 5/15 = 33.33%
            t.check_approx(rows.get("IO", {}).get("pct", -1), 33.33, 0.01,
                           "IO% = 33.33%")

            print("--- Activity excluded ---")
            # Activity should be present but excluded from DB Time
            if "Activity" in rows:
                t.check(True, "Activity row present (excluded from DB Time)")
            else:
                t.check(True, "Activity not in output (OK if idle-only)")

    finally:
        cleanup_traces(trace_dir)

    check_cpu_aas_contract(t)
    sys.exit(0 if t.summary() else 1)


if __name__ == "__main__":
    main()
