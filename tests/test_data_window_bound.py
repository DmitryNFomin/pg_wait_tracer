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

Transitions must also refuse above the bound while the untagged block
aggregate is unsafe (#357). With a sufficient bound, the tagged raw fallback
must answer non-empty on both cold and warm requests, with zero aggregate
provenance and identical links and nodes.
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
                           env={"PGWT_LOAD_MAX_EVENTS": 5_000,
                                # merged_blocks/decoded_blocks are opt-in
                                # provenance (they must never change WHETHER
                                # two answers compare equal), and the
                                # `transitions` refusal below asserts on
                                # them.
                                "PGWT_TRANSITIONS_PROVENANCE": "1"}) as srv:
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
                        "heatmap", "session_timeline",
                        "fingerprints", "lock_chains", "interference",
                        "concurrency", "variants"):
                resp = srv.query(cmd)
                t.check(resp.get("code") == "window_too_large",
                        f"{cmd}: structured error, not partial data")

            # #357: block aggregates see untagged CPU records and disagree
            # with the tagged raw loader. Refuse rather than bypass its bound,
            # on BOTH requests; a warm cache must not reopen the unsafe path.
            for phase in ("cold", "warm"):
                resp = srv.query("transitions")
                t.check("error" in resp and
                        resp.get("code") == "window_too_large",
                        f"transitions: {phase} fast path refuses past the raw bound")
                t.check_eq(resp.get("max_events"), 5_000,
                           f"transitions: {phase} refusal carries the raw bound")
                t.check("links" not in resp and "nodes" not in resp and
                        "total" not in resp,
                        f"transitions: {phase} refusal carries no partial graph")

        # Same unfiltered exact-only window, now within an explicit raw bound.
        # Do not add a filter to force fallback: this request must exercise
        # the safety refusal even where the old aggregate path could serve.
        with ServerHarness(trace_dir,
                           env={"PGWT_LOAD_MAX_EVENTS": 25_000,
                                "PGWT_TRANSITIONS_PROVENANCE": "1"}) as srv:
            first = srv.query("transitions")
            t.check("error" not in first,
                    "transitions: cold raw fallback answers within the bound")
            resp = srv.query("transitions")
            t.check("error" not in resp,
                    "transitions: warm raw fallback answers within the bound")
            for phase, answer in (("cold", first), ("warm", resp)):
                # No default of zero: absent provenance is not proof of raw.
                t.check_eq(answer.get("merged_blocks"), 0,
                           f"transitions: {phase} fast path merged no blocks")
                t.check_eq(answer.get("decoded_blocks"), 0,
                           f"transitions: {phase} fast path decoded no blocks")
                t.check_eq(answer.get("total"), NUM_PIDS * EVENTS_PER_PID,
                           f"transitions: {phase} raw fallback counts the full fixture")
            t.check_eq(resp.get("total"), first.get("total"),
                       "transitions: warm raw answer == cold raw answer "
                       "(total)")
            t.check_eq(resp.get("total_link_count"),
                       first.get("total_link_count"),
                       "transitions: warm raw answer == cold raw answer "
                       "(distinct links)")
            # The two scalars above are NOT "identical answers": this fixture
            # has few enough distinct pairs that `total` and the link count can
            # both match while a per-link duration_ms or a node total differs.
            # Comparing the arrays costs nothing and is what the comment above
            # actually promises, so compare them.
            t.check_eq(resp.get("links"), first.get("links"),
                       "transitions: warm raw answer == cold raw answer "
                       "(every link, including duration_ms)")
            t.check_eq(resp.get("nodes"), first.get("nodes"),
                       "transitions: warm raw answer == cold raw answer "
                       "(every node, including total_ms and order)")
            # ...and the arrays must be non-empty, or the two check_eq calls
            # above are [] == [] and prove nothing. This fixture carries
            # exactly ONE link and ONE node, so what the comparison adds over
            # the two scalars is every FIELD of that link and node -- notably
            # duration_ms and total_ms, which `total` and the link count
            # cannot see. It is not a many-row comparison, and asserting >1
            # here would be asserting something about the fixture that is
            # false.
            t.check(len(first.get("links") or []) >= 1,
                    "transitions: the cold answer carries at least one link, "
                    "so the array comparison is not [] == [] (%d)"
                    % len(first.get("links") or []))
            t.check(len(first.get("nodes") or []) >= 1,
                    "transitions: the cold answer carries at least one node, "
                    "so the array comparison is not [] == [] (%d)"
                    % len(first.get("nodes") or []))
            links = resp.get("links", [])
            t.check(len(links) > 0 and resp.get("total", 0) > 0,
                    "transitions: non-empty, so the raw fallback is not hiding "
                    "an empty answer (total=%s links=%d)"
                    % (resp.get("total"), len(links)))
            t.check(resp.get("total_link_count", 0) >= len(links),
                    "transitions: total_link_count >= the rows emitted")
            t.check(sum(l.get("value", 0) for l in links)
                    <= resp.get("total", 0),
                    "transitions: the emitted rows' counts cannot exceed the "
                    "declared total (truncation is a cap, not an invention)")

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
