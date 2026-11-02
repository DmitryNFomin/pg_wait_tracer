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
# bound (src/server.c report_load_overload). Every arm used to append " or
# add a pid/query filter" when pid == 0; no loader there takes a filter
# argument at all, so a query filter is never pushed into any of them.
#
# The arms are NOT interchangeable, which is the point of round 2: each names
# the reduction that is true for ITS load, and the test asserts per arm.
#   WINDOW  [req.from, req.to] -- narrowing the range reduces it.
#   PREFIX  [trace start, window left edge - 1]: takes only an upper bound
#           and both callers pass `from - 1`, so the count IGNORES `to` and
#           GROWS as `from` rises. Narrowing is inert or counterproductive;
#           an earlier start and (when still unscoped) a pid filter are the
#           reductions that work, and the fixture below measures both.
#   CONTEXT bounds derived from a selected execution or a fixed look-back, so
#           the requested range does not set them at all.
REFUSAL_PREFIX = "ERROR: window too large:"
WINDOW_TAIL = "— narrow the time range"
S = 1_000_000_000
MS = 1_000_000
MARKER_EXEC_START = 0xFFFFFFF0
MARKER_EXEC_END = 0xFFFFFFF1
# 2 * PFX_PAIRS markers before the window vs PFX_BOUND: the prefix pass
# overflows while the in-window load (5 events) stays far under the same
# bound, so the refusal under test comes from the PREFIX loader and not from
# the window loader the case above already covers. PFX_PID_BOUND sits between
# ONE pid's prefix (2 * PFX_PAIRS / PFX_NPIDS = 120 markers) and the whole
# fixture's, which is what makes the pid-scoped prefix refusal reachable
# without the window load refusing first.
PFX_PAIRS = 1_200
PFX_NPIDS = 20
PFX_BOUND = 1_500
PFX_PID_BOUND = 100
PFX_WIN_FROM = BASE_TS + 60 * S
PFX_WIN_TO = PFX_WIN_FROM + 1 * S
# An earlier start for the SAME trace: the prefix is then only the markers
# before BASE_TS + 2 s, and the window stays small enough that its own
# separate bound is not the thing being measured.
PFX_EARLY_FROM = BASE_TS + 2 * S
PFX_EARLY_TO = BASE_TS + 3 * S


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


def grade_refusal(t, label, err, must_have, must_not_have, ends_with=None):
    """Grade every "window too large:" line pgwt-server printed.

    The FIRST assertion is the one that keeps the rest honest: at least one
    such line must exist. Without it every `all(...)` below is true of an
    empty list, which is how a deleted, unreached or silent message passes as
    "no bad advice found" — the false negative, not the true positive, is the
    failure mode this file has to survive.

    There is deliberately no blanket "the word filter never appears": the
    PREFIX arm's correct wording NAMES a pid filter, because that is the
    reduction that works there. Expectations are per arm, passed in.
    """
    lines = [ln.rstrip() for ln in err.splitlines()
             if ln.startswith(REFUSAL_PREFIX)]
    t.check(len(lines) >= 1,
            f"{label}: pgwt-server printed its own refusal line "
            f"(stderr holds {len(err)} bytes)")
    for s in must_have:
        t.check(all(s in ln for ln in lines),
                f"{label}: every refusal line says '{s}' (got {lines})")
    for s in must_not_have:
        t.check(all(s not in ln for ln in lines),
                f"{label}: no refusal line says '{s}' (got {lines})")
    if ends_with is not None:
        t.check(all(ln.endswith(ends_with) for ln in lines),
                f"{label}: every refusal line ends at '{ends_with}' "
                f"(got {lines})")


def prefix_passes(err):
    """(path, markers) for each prefix pass pgwt-server reported.

    PGWT_EXEC_PREFIX_DEBUG=1 makes the server name the implementation it
    chose and the marker count it produced. Read it rather than assuming:
    the two implementations are selected at run time, and a case that
    silently exercised the other one proves nothing about the one it claims.
    """
    out = []
    for ln in err.splitlines():
        if not ln.startswith("pgwt-prefix: path="):
            continue
        path = ln.split("path=", 1)[1].split()[0]
        markers = int(ln.split("markers=", 1)[1].split()[0])
        out.append((path, markers))
    return out


def ask(trace_dir, env, err_path, cmd, **kwargs):
    """One request to a fresh server, returning (response, stderr text)."""
    with ServerHarness(trace_dir, env=env, stderr_path=err_path) as srv:
        resp = srv.query(cmd, **kwargs)
    return resp, read_stderr(err_path)


def check_window_arm(t, err_dir):
    """The request's own range: narrowing it IS the reduction."""
    trace_dir = generate_traces(build_scenario())
    try:
        # pid == 0: the arm that carried the impossible pid/query advice.
        resp, err = ask(trace_dir, {"PGWT_LOAD_MAX_EVENTS": 5_000},
                        os.path.join(err_dir, "window-unfiltered.err"),
                        "time_model")
        t.check_eq(resp.get("code"), "window_too_large",
                   "window/pid==0: the load really did refuse, so the "
                   "assertions below are not vacuous")
        grade_refusal(t, "window/pid==0", err,
                      must_have=["the requested range holds",
                                 "across all pids"],
                      # No filter of any kind is named on this arm: five
                      # callers hardcode pid 0 and drop the request's pid
                      # filter, and no loader here sees a query filter at all.
                      must_not_have=["filter", "for this pid",
                                     "prefix", "context load"],
                      ends_with=WINDOW_TAIL)

        # pid != 0: one pid's 2,000 events over a 1,000 bound. This arm never
        # carried the advice; it is here so a change that collapses the arms
        # into one sentence cannot pass.
        resp, err = ask(trace_dir, {"PGWT_LOAD_MAX_EVENTS": 1_000},
                        os.path.join(err_dir, "window-pid.err"),
                        "time_model", filters={"pid": 1001})
        t.check_eq(resp.get("code"), "window_too_large",
                   "window/pid!=0: the pid-scoped load really did refuse")
        grade_refusal(t, "window/pid!=0", err,
                      must_have=["the requested range holds", "for this pid"],
                      must_not_have=["filter", "across all pids", "prefix"],
                      ends_with=WINDOW_TAIL)

        # CONTEXT: handle_execution_detail's load is bounded by the SELECTED
        # EXECUTION (req.start_ns/end_ns), not req.from/to, and it passes
        # pid 0 on purpose so worker lanes stay visible. So neither narrowing
        # the range nor a pid filter reduces it, and the refusal must say so
        # instead of repeating the window arm's advice.
        resp, err = ask(trace_dir, {"PGWT_LOAD_MAX_EVENTS": 1_000},
                        os.path.join(err_dir, "context.err"),
                        "execution_detail", filters={"pid": 1001},
                        start_ns=str(BASE_TS), end_ns=str(BASE_TS + 2 * S))
        t.check_eq(resp.get("code"), "window_too_large",
                   "context: the execution-bounded load really did refuse")
        grade_refusal(t, "context", err,
                      must_have=["a bounded context load",
                                 "narrowing the range does not reduce it",
                                 "PGWT_LOAD_MAX_EVENTS"],
                      must_not_have=[WINDOW_TAIL, "start the window earlier",
                                     "add a pid filter"])
    finally:
        cleanup_traces(trace_dir)


def check_prefix_arm(t, err_dir):
    """The execution-marker prefix: narrowing the window does NOT reduce it.

    `server_load_exec_prefix` takes only an upper bound and both callers pass
    `from - 1`, so the count ignores `to` and grows as `from` rises. Each
    case below reads the `pgwt-prefix: path=` line to pin WHICH
    implementation refused, because the two have separate copies of the
    message and a case that silently ran the other one would prove nothing.
    """
    trace_dir = generate_traces(build_prefix_scenario())
    try:
        prefix_must_have = ["the execution-marker prefix",
                            "start the window earlier"]
        prefix_must_not = [WINDOW_TAIL, "a bounded context load"]

        for label, extra_env, want_path in (
                ("prefix/index", {}, "index"),
                # The legacy full decode goes through the GENERAL loader
                # (server_load_markers_fi), so this case is what proves the
                # prefix purpose survives that fallback instead of the
                # general loader's window wording leaking back in.
                ("prefix/legacy", {"PGWT_EXEC_PREFIX_INDEX": "0"}, "legacy")):
            env = {"PGWT_LOAD_MAX_EVENTS": PFX_BOUND,
                   "PGWT_EXEC_PREFIX_DEBUG": "1"}
            env.update(extra_env)
            resp, err = ask(trace_dir, env,
                            os.path.join(err_dir, label.replace("/", "-")
                                         + ".err"),
                            "executions", from_=PFX_WIN_FROM, to_=PFX_WIN_TO)
            t.check_eq(resp.get("code"), "window_too_large",
                       f"{label}: the prefix pass really did refuse")
            passes = prefix_passes(err)
            t.check(any(p == want_path for p, _ in passes),
                    f"{label}: the refusing pass is the {want_path} "
                    f"implementation, so its own copy of the line is what "
                    f"was graded (got {passes})")
            grade_refusal(t, label, err,
                          must_have=prefix_must_have + ["across all pids",
                                                        "add a pid filter"],
                          must_not_have=prefix_must_not + ["for this pid"])

        # Already pid-scoped: the earlier start is still available, a pid
        # filter is not, so the line must not suggest one. Bound sits between
        # one pid's prefix and the window load's 5 events.
        resp, err = ask(trace_dir,
                        {"PGWT_LOAD_MAX_EVENTS": PFX_PID_BOUND,
                         "PGWT_EXEC_PREFIX_DEBUG": "1"},
                        os.path.join(err_dir, "prefix-pid.err"),
                        "executions", from_=PFX_WIN_FROM, to_=PFX_WIN_TO,
                        filters={"pid": 3000})
        t.check_eq(resp.get("code"), "window_too_large",
                   "prefix/pid!=0: the pid-scoped prefix really did refuse")
        grade_refusal(t, "prefix/pid!=0", err,
                      must_have=prefix_must_have + ["for this pid"],
                      must_not_have=prefix_must_not + ["add a pid filter",
                                                       "across all pids"])

        # ── The advice, MEASURED rather than asserted ──────────────────────
        # Same trace, same bound. The prefix marker count the server itself
        # reports must fall when the window starts earlier and when the
        # request is pid-scoped — otherwise the new text names reductions
        # that do not reduce, which is the defect this issue is about.
        env = {"PGWT_LOAD_MAX_EVENTS": PFX_BOUND,
               "PGWT_EXEC_PREFIX_DEBUG": "1"}
        _, err_late = ask(trace_dir, env,
                          os.path.join(err_dir, "reduce-late.err"),
                          "executions", from_=PFX_WIN_FROM, to_=PFX_WIN_TO)
        resp_early, err_early = ask(trace_dir, env,
                                    os.path.join(err_dir, "reduce-early.err"),
                                    "executions", from_=PFX_EARLY_FROM,
                                    to_=PFX_EARLY_TO)
        resp_pid, err_pid = ask(trace_dir, env,
                                os.path.join(err_dir, "reduce-pid.err"),
                                "executions", from_=PFX_WIN_FROM,
                                to_=PFX_WIN_TO, filters={"pid": 3000})
        late = prefix_passes(err_late)
        early = prefix_passes(err_early)
        scoped = prefix_passes(err_pid)
        t.check(len(late) == 1 and len(early) == 1 and len(scoped) == 1,
                "reduction: one prefix pass per request, so the counts "
                f"compared below are the right ones (late={late} "
                f"early={early} scoped={scoped})")
        n_late = late[0][1] if late else -1
        n_early = early[0][1] if early else -1
        n_scoped = scoped[0][1] if scoped else -1
        t.check(n_late >= PFX_BOUND,
                "reduction: the late unscoped prefix is the one that hit the "
                f"bound ({n_late} >= {PFX_BOUND})")
        t.check(0 < n_early < n_late,
                "reduction: starting the window earlier really does shrink "
                f"the prefix load ({n_early} < {n_late}, and non-empty)")
        t.check(0 < n_scoped < n_late,
                "reduction: a pid filter really does shrink the prefix load "
                f"({n_scoped} < {n_late}, and non-empty)")
        t.check(resp_early.get("code") != "window_too_large",
                "reduction: the earlier start clears the prefix refusal "
                f"(code={resp_early.get('code')})")
        t.check(resp_pid.get("code") != "window_too_large",
                "reduction: the pid scope clears the prefix refusal "
                f"(code={resp_pid.get('code')})")
    finally:
        cleanup_traces(trace_dir)


def check_loader_refusal_lines(t):
    """#728: one arm per load purpose, each naming a reduction that works."""
    err_dir = tempfile.mkdtemp(prefix="pgwt_stderr_")
    try:
        check_window_arm(t, err_dir)
        check_prefix_arm(t, err_dir)
    finally:
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
