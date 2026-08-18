#!/usr/bin/env python3
"""test_data_window_bound.py — T5/DUR-9: raw-load memory bound + pid pushdown.

A pid-filtered long-window query used to force the raw path to load EVERY
event in range into one unbounded doubling array. The fix:

  1. pid pushdown — with a pid filter, only that pid's events enter the
     working array;
  2. a hard bound (load_max_events, PGWT_LOAD_MAX_EVENTS env override for
     tests) — exceeding it yields a structured "window too large" error,
     never an OOM and never a silently partial result.

Scenario: 10 pids × 2,000 events = 20,000 events. Bound set to 5,000:
  - unfiltered query  → structured error (20,000 > 5,000);
  - pid-filtered query → succeeds (2,000 < 5,000) and its numbers are
    correct — proof the filter is applied DURING load, not after.
"""
import os
import sys

sys.path.insert(0, os.path.dirname(__file__))
from server_harness import (
    ServerHarness, generate_traces, cleanup_traces, TestRunner,
    CPU, IO_DATA_FILE_READ,
)

BASE_TS = 10_000_000_000_000
NUM_PIDS = 10
EVENTS_PER_PID = 2_000
DUR_NS = 1_000_000  # 1 ms per event


def build_scenario():
    events = []
    for p in range(NUM_PIDS):
        pid = 1000 + p
        ts = BASE_TS + p * 1_000
        for i in range(EVENTS_PER_PID):
            ts += DUR_NS
            events.append({"pid": pid, "ts": ts, "dur": DUR_NS,
                           "old": IO_DATA_FILE_READ, "new": CPU, "qid": 42})
    events.sort(key=lambda e: e["ts"])
    return {
        "backends": [{"pid": 1000 + p, "type": "client", "user": "u",
                      "db": "d"} for p in range(NUM_PIDS)],
        "queries": [{"id": 42, "text": "SELECT 1"}],
        "events": events,
    }


def main():
    t = TestRunner("window_bound")
    trace_dir = generate_traces(build_scenario())
    try:
        with ServerHarness(trace_dir,
                           env={"PGWT_LOAD_MAX_EVENTS": 5_000}) as srv:
            # Unfiltered: 20k events > 5k bound → structured error.
            resp = srv.query("time_model")
            t.check("error" in resp, "unfiltered long window returns error")
            t.check_eq(resp.get("code"), "window_too_large",
                       "error carries the window_too_large code")
            t.check("max_events" in resp, "error carries the bound")
            t.check("rows" not in resp,
                    "no partial data rendered alongside the error")

            # Same window, pid filter: pushdown loads only 2k events.
            resp = srv.query("time_model", filters={"pid": 1001})
            t.check("error" not in resp,
                    "pid-filtered query under the bound succeeds "
                    "(pushdown, not post-filter)")
            expected_ms = EVENTS_PER_PID * DUR_NS / 1e6
            rows = {r["name"]: r for r in resp.get("rows", [])}
            t.check_approx(rows.get("IO", {}).get("ms", -1), expected_ms,
                           0.01, "pid-filtered IO time exact")

            # Every raw-path view must reject, not truncate.
            for cmd in ("aas", "top_events", "top_sessions", "top_queries",
                        "heatmap", "session_timeline", "transitions",
                        "fingerprints", "lock_chains", "interference",
                        "concurrency", "variants"):
                resp = srv.query(cmd)
                t.check(resp.get("code") == "window_too_large",
                        f"{cmd}: structured error, not partial data")

        # Sanity: with the default (RAM-derived) bound the same trace loads.
        with ServerHarness(trace_dir) as srv:
            resp = srv.query("time_model")
            t.check("error" not in resp,
                    "default bound loads the full window")

        with ServerHarness(
                trace_dir,
                env={"PGWT_TEST_LOAD_ALLOC_FAIL": "merge_initial"}) as srv:
            resp = srv.query("time_model")
            t.check_eq(resp.get("code"), "allocation_failed",
                       "merge malloc failure has a distinct error code")
            t.check("max_events" not in resp and "rows" not in resp,
                    "allocation failure is neither a range refusal nor partial data")
    finally:
        cleanup_traces(trace_dir)

    return 0 if t.summary() else 1


if __name__ == "__main__":
    sys.exit(main())
