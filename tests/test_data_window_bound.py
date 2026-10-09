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
                           env={"PGWT_LOAD_MAX_EVENTS": 5_000,
                                # merged_blocks/decoded_blocks are opt-in
                                # provenance (they must never change WHETHER
                                # two answers compare equal), and the
                                # `transitions` exemption below asserts on
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

            # `transitions` is the ONE documented exception, and it is the
            # point of the paint-latency block aggregate: it merges per-block
            # (old_event, new_event) tables and never materialises the window,
            # so the memory bound this test sets does not apply to it. It must
            # ANSWER -- and the answer must be self-consistent, because above
            # the bound there is no raw result left to compare against (the
            # bit-exact comparison lives in tests/test_block_agg.c §6 and §9,
            # at sizes the raw path can still compute).
            # TWO requests, because the first cannot merge anything: a cold
            # cache has no aggregates yet, so request 1 decodes every block
            # and stores them (merged=0) and request 2 merges them. Asserting
            # merged>0 on the FIRST request is simply wrong, and the gate said
            # so (merged=0 decoded=5). What matters is that the cache delivers
            # on the second look, and that both answers are IDENTICAL -- a
            # cached aggregate that answered differently from a fresh decode
            # would be the silent-wrong failure this whole phase is built
            # against.
            first = srv.query("transitions")
            t.check("error" not in first,
                    "transitions: answers past the raw bound (block aggregate)")
            t.check(first.get("decoded_blocks", 0) > 0,
                    "transitions: the cold request decoded blocks "
                    "(merged=%s decoded=%s)" % (first.get("merged_blocks"),
                                                first.get("decoded_blocks")))
            resp = srv.query("transitions")
            t.check("error" not in resp, "transitions: second request answers")
            t.check(resp.get("merged_blocks", 0) > 0,
                    "transitions: the warm request MERGED blocks, so the "
                    "aggregate cache actually delivered (merged=%s "
                    "decoded=%s)" % (resp.get("merged_blocks"),
                                     resp.get("decoded_blocks")))
            t.check_eq(resp.get("total"), first.get("total"),
                       "transitions: merged answer == freshly decoded answer "
                       "(total)")
            t.check_eq(resp.get("total_link_count"),
                       first.get("total_link_count"),
                       "transitions: merged answer == freshly decoded answer "
                       "(distinct links)")
            # The two scalars above are NOT "identical answers": this fixture
            # has few enough distinct pairs that `total` and the link count can
            # both match while a per-link duration_ms or a node total differs.
            # Comparing the arrays costs nothing and is what the comment above
            # actually promises, so compare them.
            t.check_eq(resp.get("links"), first.get("links"),
                       "transitions: merged answer == freshly decoded answer "
                       "(every link, including duration_ms)")
            t.check_eq(resp.get("nodes"), first.get("nodes"),
                       "transitions: merged answer == freshly decoded answer "
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
                    "transitions: non-empty, so the exemption is not hiding "
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
