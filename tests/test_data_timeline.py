#!/usr/bin/env python3
"""test_data_timeline.py — 0B.6: timestamp correctness (Bug 1 regression).

Verifies that session_timeline events have correct start/end timestamps:
  start = timestamp_ns - duration_ns
  end = timestamp_ns
  Adjacent events for the same PID are contiguous (no gaps/overlaps).

#278/#360: query-ID drill-downs keep every PID, including after the old
256-PID boundary and beyond the old 1024-slot hash. Allocation failure
refuses the whole response rather than returning a successful prefix.
"""
import os
import sys

sys.path.insert(0, os.path.dirname(__file__))
from server_harness import (
    ServerHarness, generate_traces, cleanup_traces, TestRunner,
    CPU, IO_DATA_FILE_READ, LOCK_RELATION,
)

BASE_TS = 10_000_000_000_000
MS = 1_000_000
WINDOW_END = BASE_TS + 900_000 * MS


def build_many_pids(n):
    # These collide in the former 1024-slot hash. Two interleaved passes
    # exercise both lookup and pending-state preservation across growth.
    pids = [1000 + i * 1024 for i in range(n)]
    events = []
    for round_, event in enumerate((IO_DATA_FILE_READ, LOCK_RELATION)):
        for i, pid in enumerate(pids):
            events.append({"pid": pid, "ts": BASE_TS + (round_ * n + i + 1) * MS,
                           "dur": MS, "old": event, "new": CPU, "qid": 100})
    # A different statement must not acquire a PID slot in this drill-down.
    events.append({"pid": 999, "ts": BASE_TS + (2 * n + 1) * MS,
                   "dur": MS, "old": CPU, "new": CPU, "qid": 200})
    return {"events": events}, pids


def check_many_pids(t, n):
    scenario, pids = build_many_pids(n)
    trace_dir = generate_traces(scenario)
    try:
        with ServerHarness(trace_dir) as srv:
            resp = srv.query("session_timeline", timeout=5,
                             from_=BASE_TS, to_=WINDOW_END,
                             filters={"query_id": "100"})
            t.check("error" not in resp, f"{n} PIDs: succeeds")
            t.check(resp.get("pids") == pids, f"{n} PIDs: every PID in arrival order")
            bars = resp.get("events", [])
            expected = {(e["pid"], e["ts"] - e["dur"], e["dur"], e["old"], "100")
                        for e in scenario["events"] if e["qid"] == 100}
            actual = {(e["p"], e["s"], e["d"], e["e"], e["q"]) for e in bars}
            t.check(actual == expected, f"{n} PIDs: every wait survives both passes")
            t.check_eq(len(bars), 2 * n, f"{n} PIDs: no duplicate or missing bars")
            t.check_eq(resp.get("total_count"), 2 * n, f"{n} PIDs: exact total")
            t.check_eq(resp.get("truncated"), False, f"{n} PIDs: complete response")

            last = srv.query("session_timeline", timeout=5,
                             from_=BASE_TS, to_=WINDOW_END,
                             filters={"pid": pids[-1]})
            t.check_eq(last.get("pids"), [pids[-1]], f"{n} PIDs: last session drill-down")
            t.check_eq(len(last.get("events", [])), 2, f"{n} PIDs: last session waits")

            empty = srv.query("session_timeline", timeout=5,
                              filters={"query_id": "300"})
            t.check_eq(empty.get("pids"), [], f"{n} PIDs: empty selection has no PIDs")
            t.check_eq(empty.get("events"), [], f"{n} PIDs: empty selection has no bars")
            t.check_eq(empty.get("total_count"), 0, f"{n} PIDs: empty selection total")
    finally:
        cleanup_traces(trace_dir)


def check_alloc_failure(t):
    scenario, _ = build_many_pids(257)
    trace_dir = generate_traces(scenario)
    try:
        for point in ("pid_index_grow", "timeline_pending_grow", "timeline_pending_regrow"):
            with ServerHarness(trace_dir, env={"PGWT_TEST_ALLOC_FAIL": point}) as srv:
                resp = srv.query("session_timeline", timeout=5,
                                 filters={"query_id": "100"})
                t.check_eq(resp.get("code"), "allocation_failed", f"{point}: failure flag")
                t.check(bool(resp.get("error")), f"{point}: explicit refusal")
                t.check(all(k not in resp for k in ("pids", "events", "total_count")),
                        f"{point}: no successful partial arrays or counts")
                # A refusal must leave the server alive for subsequent requests.
                t.check("error" not in srv.query("info", timeout=5),
                        f"{point}: server still responds")
    finally:
        cleanup_traces(trace_dir)


def check_event_limit(t):
    # The separate bar limit already has a UI-rendered truncation flag.
    # It must stay explicit even though the PID list is now complete.
    for n in (2500, 2501):
        scenario, pids = build_many_pids(n)
        trace_dir = generate_traces(scenario)
        try:
            with ServerHarness(trace_dir) as srv:
                resp = srv.query("session_timeline", timeout=5,
                                 from_=BASE_TS, to_=WINDOW_END,
                                 filters={"query_id": "100"})
                t.check("error" not in resp, f"{2 * n} waits: succeeds")
                t.check(resp.get("pids") == pids, f"{2 * n} waits: every PID retained")
                t.check_eq(len(resp.get("events", [])), 5000,
                           f"{2 * n} waits: bar limit")
                t.check_eq(resp.get("total_count"), 2 * n,
                           f"{2 * n} waits: count before truncation")
                t.check_eq(resp.get("truncated"), n > 2500,
                           f"{2 * n} waits: explicit truncation status")
        finally:
            cleanup_traces(trace_dir)


def build_scenario():
    """3 contiguous events for PID 1000.

    Event 1: CPU     from BASE_TS to BASE_TS+2ms (dur=2ms)
    Event 2: IO:Read from BASE_TS+2ms to BASE_TS+7ms (dur=5ms)
    Event 3: Lock    from BASE_TS+7ms to BASE_TS+10ms (dur=3ms)
    """
    events = [
        {"pid": 1000, "ts": BASE_TS + 2_000_000, "dur": 2_000_000,
         "old": CPU, "new": IO_DATA_FILE_READ, "qid": 100},
        {"pid": 1000, "ts": BASE_TS + 7_000_000, "dur": 5_000_000,
         "old": IO_DATA_FILE_READ, "new": LOCK_RELATION, "qid": 100},
        {"pid": 1000, "ts": BASE_TS + 10_000_000, "dur": 3_000_000,
         "old": LOCK_RELATION, "new": CPU, "qid": 100},
    ]

    return {
        "backends": [{"pid": 1000, "type": "client", "user": "test", "db": "testdb"}],
        "queries": [{"id": 100, "text": "SELECT 1"}],
        "events": events,
    }


def main():
    t = TestRunner("test_data_timeline")
    print(f"=== {t.name} ===")

    scenario = build_scenario()
    trace_dir = generate_traces(scenario)

    try:
        with ServerHarness(trace_dir) as srv:
            resp = srv.query("session_timeline")
            evts = resp.get("events", [])

            print("--- Event count ---")
            t.check_eq(len(evts), 3, "3 timeline events")

            if len(evts) >= 3:
                print("--- Event 1: CPU 2ms ---")
                e1 = evts[0]
                expected_start = BASE_TS
                t.check_eq(e1["s"], expected_start,
                           f"Event 1 start = {expected_start}")
                t.check_eq(e1["d"], 2_000_000, "Event 1 dur = 2ms")
                t.check(e1["s"] + e1["d"] == BASE_TS + 2_000_000,
                        "Event 1 end = BASE_TS + 2ms")

                print("--- Event 2: IO:Read 5ms ---")
                e2 = evts[1]
                t.check_eq(e2["s"], BASE_TS + 2_000_000,
                           "Event 2 start = Event 1 end (contiguous)")
                t.check_eq(e2["d"], 5_000_000, "Event 2 dur = 5ms")

                print("--- Event 3: Lock 3ms ---")
                e3 = evts[2]
                t.check_eq(e3["s"], BASE_TS + 7_000_000,
                           "Event 3 start = Event 2 end (contiguous)")
                t.check_eq(e3["d"], 3_000_000, "Event 3 dur = 3ms")

                print("--- Contiguity ---")
                for i in range(len(evts) - 1):
                    a = evts[i]
                    b = evts[i + 1]
                    if a["p"] == b["p"]:
                        t.check_eq(a["s"] + a["d"], b["s"],
                                   f"Events {i} → {i+1} contiguous")

    finally:
        cleanup_traces(trace_dir)

    for n in (256, 257, 436, 1100):
        print(f"--- Query drill-down: {n} PIDs in a 900s window ---")
        check_many_pids(t, n)
    check_alloc_failure(t)
    check_event_limit(t)

    sys.exit(0 if t.summary() else 1)


if __name__ == "__main__":
    main()
