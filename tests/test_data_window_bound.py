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
import shutil
import sys
import tempfile

sys.path.insert(0, os.path.dirname(__file__))
from server_harness import (
    ServerHarness, generate_traces, cleanup_traces, TestRunner,
    CPU, IO_DATA_FILE_READ,
)

BASE_TS = 10_000_000_000_000
NUM_PIDS = 10
EVENTS_PER_PID = 2_000
DUR_NS = 1_000_000  # 1 ms per event

# ── #728 ───────────────────────────────────────────────────────────────────
# The refusal pgwt-server prints to its OWN stderr when a raw load hits the
# bound. Both loaders that can hit it (the fidelity merge and the
# retained-marker prefix) used to append " or add a pid/query filter" on the
# pid == 0 arm. Neither loader takes a filter argument at all, so a query
# filter is never pushed into the load and cannot change the count being
# reported; the line now names only a narrower time range.
REFUSAL_PREFIX = "ERROR: window too large:"
REFUSAL_TAIL = "— narrow the time range"
S = 1_000_000_000
MS = 1_000_000
MARKER_EXEC_START = 0xFFFFFFF0
MARKER_EXEC_END = 0xFFFFFFF1
# 2 * PFX_PAIRS markers before the window vs PFX_BOUND: the prefix pass
# overflows while the in-window load (5 events) stays far under the same
# bound, so the refusal under test comes from the PREFIX loader and not from
# the window loader the case above already covers.
PFX_PAIRS = 1_200
PFX_BOUND = 1_500
PFX_WIN_FROM = BASE_TS + 60 * S
PFX_WIN_TO = PFX_WIN_FROM + 1 * S


def build_prefix_scenario():
    """Markers before the window, a handful of events inside it."""
    ev = []
    for i in range(PFX_PAIRS):
        pid = 3000 + (i % 20)
        qid = 700 + (i % 5)
        ts = BASE_TS + i * 10 * MS          # 12 s of prefix, all before the window
        ev.append({"pid": pid, "ts": ts, "dur": 0,
                   "old": MARKER_EXEC_START, "new": MARKER_EXEC_START,
                   "qid": qid})
        ev.append({"pid": pid, "ts": ts + 5 * MS, "dur": 0,
                   "old": MARKER_EXEC_END, "new": MARKER_EXEC_END,
                   "qid": qid})
    for j in range(5):
        ev.append({"pid": 3000, "ts": PFX_WIN_FROM + (j + 1) * 10 * MS,
                   "dur": MS, "old": IO_DATA_FILE_READ, "new": CPU,
                   "qid": 700})
    ev.sort(key=lambda e: e["ts"])
    return {
        "backends": [{"pid": 3000 + p, "type": "client", "user": "u",
                      "db": "d"} for p in range(20)],
        "queries": [{"id": 700 + q, "text": "SELECT %d" % q}
                    for q in range(5)],
        "events": ev,
    }


def read_stderr(path):
    """Everything pgwt-server has written to stderr so far.

    No sleep and no retry: stderr is unbuffered in C and the server prints
    the refusal inside the loader, BEFORE the JSON response the caller has
    already read, so the line is on disk by the time this runs. A missing
    file returns "" — and then the POSITIVE assertion below fails, which is
    the refusal we want from a check that cannot see its input.
    """
    if not os.path.exists(path):
        return ""
    with open(path, errors="replace") as f:
        return f.read()


def check_refusal(t, label, err, scope, forbidden_scope):
    """Grade every "window too large:" line pgwt-server printed.

    Three assertions, and all three are needed:
      * at least one line exists. Without it the two below are vacuous on an
        empty stderr — the exact false negative that lets a deleted or
        unreached message pass as "no bad advice found";
      * every such line ENDS at "narrow the time range". This is the
        discriminating one: the pre-#728 line also contained that phrase, in
        the middle, followed by " or add a pid/query filter", so only the
        line's END separates the two texts;
      * no such line mentions a filter in ANY phrasing, so the advice cannot
        return reworded.
    """
    lines = [ln.rstrip() for ln in err.splitlines()
             if ln.startswith(REFUSAL_PREFIX)]
    t.check(len(lines) >= 1,
            f"{label}: pgwt-server printed its own refusal line "
            f"(stderr holds {len(err)} bytes)")
    t.check(all(ln.endswith(REFUSAL_TAIL) for ln in lines),
            f"{label}: every refusal line ends at '{REFUSAL_TAIL}' "
            f"(got {lines})")
    t.check(all("filter" not in ln for ln in lines),
            f"{label}: no refusal line advises a filter the load never "
            f"receives (got {lines})")
    t.check(all(scope in ln for ln in lines),
            f"{label}: every refusal line names the scope that overflowed "
            f"('{scope}') (got {lines})")
    t.check(all(forbidden_scope not in ln for ln in lines),
            f"{label}: no refusal line claims the other scope "
            f"('{forbidden_scope}') (got {lines})")


def check_loader_refusal_lines(t):
    """#728: the two loader-level refusals, on both pid arms."""
    trace_dir = generate_traces(build_scenario())
    err_dir = tempfile.mkdtemp(prefix="pgwt_stderr_")
    try:
        # pid == 0: the arm that used to carry the impossible advice.
        err = os.path.join(err_dir, "unfiltered.err")
        with ServerHarness(trace_dir, env={"PGWT_LOAD_MAX_EVENTS": 5_000},
                           stderr_path=err) as srv:
            resp = srv.query("time_model")
        t.check_eq(resp.get("code"), "window_too_large",
                   "stderr/pid==0: the load really did refuse, so the "
                   "assertions below are not vacuous")
        check_refusal(t, "stderr/pid==0", read_stderr(err),
                      "across all pids", "for this pid")

        # pid != 0: one pid's 2,000 events over a 1,000 bound. The pid arm
        # never carried the advice; it is here so a fix that collapsed both
        # arms into one sentence cannot pass.
        err = os.path.join(err_dir, "pid.err")
        with ServerHarness(trace_dir, env={"PGWT_LOAD_MAX_EVENTS": 1_000},
                           stderr_path=err) as srv:
            resp = srv.query("time_model", filters={"pid": 1001})
        t.check_eq(resp.get("code"), "window_too_large",
                   "stderr/pid!=0: the pid-scoped load really did refuse")
        check_refusal(t, "stderr/pid!=0", read_stderr(err),
                      "for this pid", "across all pids")
    finally:
        cleanup_traces(trace_dir)
        shutil.rmtree(err_dir, ignore_errors=True)

    # The retained-marker prefix is a SECOND loader with its own copy of this
    # line (src/server.c server_load_prefix_from_index). Reached through
    # `executions` with an explicit window that starts after the trace does.
    trace_dir = generate_traces(build_prefix_scenario())
    err_dir = tempfile.mkdtemp(prefix="pgwt_stderr_")
    try:
        err = os.path.join(err_dir, "prefix.err")
        with ServerHarness(trace_dir,
                           env={"PGWT_LOAD_MAX_EVENTS": PFX_BOUND,
                                "PGWT_EXEC_PREFIX_DEBUG": "1"},
                           stderr_path=err) as srv:
            resp = srv.query("executions", from_=PFX_WIN_FROM,
                             to_=PFX_WIN_TO)
        t.check_eq(resp.get("code"), "window_too_large",
                   "stderr/prefix: the prefix pass really did refuse")
        raw = read_stderr(err)
        # Which of the two prefix implementations ran is pgwt-server's own
        # choice, so READ it instead of assuming: if this fell back to the
        # legacy decode, the index copy of the message was never printed and
        # this case proves nothing about it.
        paths = [ln for ln in raw.splitlines()
                 if ln.startswith("pgwt-prefix: path=")]
        t.check(any("path=index" in ln for ln in paths),
                "stderr/prefix: the retained-index prefix is the pass that "
                f"refused, so its own copy of the line is what was graded "
                f"(got {paths})")
        check_refusal(t, "stderr/prefix", raw, "across all pids",
                      "for this pid")
    finally:
        cleanup_traces(trace_dir)
        shutil.rmtree(err_dir, ignore_errors=True)


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

    check_loader_refusal_lines(t)

    return 0 if t.summary() else 1


if __name__ == "__main__":
    sys.exit(main())
