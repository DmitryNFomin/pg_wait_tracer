#!/usr/bin/env python3
"""U3/B6 execution list, waterfall, worker lanes, and scatter honesty."""
import os
import sys
import time

sys.path.insert(0, os.path.dirname(__file__))
from server_harness import (
    ServerHarness, generate_traces, cleanup_traces, TestRunner,
    CPU, IO_DATA_FILE_READ, LWLOCK_WAL_WRITE,
)
from demo_rehearsal_lib import WATERFALL_QUERY_THRESHOLD_S

EXEC_START = 0xFFFFFFF0
EXEC_END = 0xFFFFFFF1
PLAN_START = 0xFFFFFFF2
PLAN_END = 0xFFFFFFF3

BASE = 10_000_000_000_000_000  # > 2^53: every JSON ns value must be a string
MS = 1_000_000


def marker(pid, ts, kind, qid):
    return {"pid": pid, "ts": ts, "dur": 0,
            "old": kind, "new": kind, "qid": qid}


def lifecycle_scenario():
    events = [
        marker(1000, BASE - 5 * MS, PLAN_START, 100),
        marker(1000, BASE - 3 * MS, PLAN_END, 100),
        marker(1000, BASE, EXEC_START, 100),
        # Raw interval starts before EXEC_START but overlaps the detail window.
        {"pid": 1000, "ts": BASE + 2 * MS, "dur": 4 * MS,
         "old": IO_DATA_FILE_READ, "new": CPU, "qid": 100, "cpu": 0},
        # Parallel worker lane; cpu omitted intentionally => UNKNOWN/null.
        {"pid": 1100, "ts": BASE + 3 * MS, "dur": 2 * MS,
         "old": LWLOCK_WAL_WRITE, "new": CPU, "qid": 100},
        {"pid": 1000, "ts": BASE + 10 * MS, "dur": 5 * MS,
         "old": CPU, "new": IO_DATA_FILE_READ, "qid": 100, "cpu": 3 * MS},
        marker(1000, BASE + 20 * MS, EXEC_END, 100),

        # Complete execution without a planning phase.
        marker(1000, BASE + 30 * MS, EXEC_START, 100),
        {"pid": 1000, "ts": BASE + 35 * MS, "dur": 5 * MS,
         "old": IO_DATA_FILE_READ, "new": CPU, "qid": 100, "cpu": 0},
        marker(1000, BASE + 40 * MS, EXEC_END, 100),

        # Unpaired start: must remain open, never get the window edge as an end.
        marker(1000, BASE + 50 * MS, EXEC_START, 200),
        {"pid": 1000, "ts": BASE + 55 * MS, "dur": 5 * MS,
         "old": IO_DATA_FILE_READ, "new": CPU, "qid": 200, "cpu": 0},
    ]
    events.sort(key=lambda e: e["ts"])
    return {
        "cpu_measured": 1,
        "backends": [
            {"pid": 1000, "type": "client", "user": "u", "db": "d"},
            {"pid": 1100, "type": "parallel_worker", "leader_pid": 1000},
        ],
        "queries": [
            {"id": 100, "text": "SELECT parallel_work()"},
            {"id": 200, "text": "SELECT still_running()"},
        ],
        "events": events,
    }


def test_lifecycle_and_detail(t):
    print("\n### marker pairing, plan, raw clipping, and worker lane ###")
    trace_dir = generate_traces(lifecycle_scenario())
    try:
        with ServerHarness(trace_dir) as srv:
            data = srv.query("executions")
            t.check_eq(data.get("fidelity"), "exact",
                       "executions carries exact fidelity")
            t.check_eq(data.get("total_count"), 3,
                       "three EXEC_START markers become three rows")
            t.check(not data.get("truncated"), "unlimited table is not truncated")
            rows = data.get("rows", [])
            starts = [int(r["start_ns"]) for r in rows]
            t.check(starts == sorted(starts, reverse=True),
                    "execution rows are latest-first")
            t.check(all(isinstance(r.get("query_id"), str) and
                        isinstance(r.get("start_ns"), str) for r in rows),
                    "execution query IDs and starts are strings")
            t.check(all(r.get("end_ns") is None or
                        isinstance(r.get("end_ns"), str) for r in rows),
                    "every execution end is a string or explicit null")

            by_start = {r["start_ns"]: r for r in rows}
            first = by_start[str(BASE)]
            second = by_start[str(BASE + 30 * MS)]
            opened = by_start[str(BASE + 50 * MS)]
            t.check_eq(first.get("end_ns"), str(BASE + 20 * MS),
                       "EXEC_END pairs with the matching start")
            t.check_eq(first.get("plan_ms"), 2.0,
                       "preceding PLAN_START/END supplies plan_ms")
            t.check_eq(first.get("n_events"), 2,
                       "leader event count excludes markers and worker events")
            t.check_eq(first.get("n_workers"), 1,
                       "leader_pid metadata attaches the active worker")
            t.check(second.get("plan_ms") is None,
                    "execution without plan markers reports plan_ms null")
            t.check(opened.get("in_progress") is True and
                    opened.get("end_ns") is None and
                    opened.get("duration_ms") is None,
                    "unpaired start is in-progress with no invented end/duration")

            limited = srv.query("executions", limit=1)
            t.check_eq(len(limited.get("rows", [])), 1, "limit bounds rows")
            t.check(limited.get("truncated") is True and
                    limited.get("total_count") == 3,
                    "table truncation reports the full filtered count")

            filtered = srv.query("executions", query_id="200")
            t.check_eq(filtered.get("total_count"), 1,
                       "top-level decimal-string query_id filter works")
            t.check_eq(filtered.get("rows", [{}])[0].get("query_id"), "200",
                       "query filter returns the requested execution")

            open_scatter = srv.query("exec_scatter", max_points=100)
            open_point = next((p for p in open_scatter.get("points", [])
                               if p.get("query_id") == "200"), {})
            t.check_eq(open_scatter.get("total_count"), 3,
                       "scatter includes every marker-started execution")
            t.check(open_point.get("duration_ms") is None,
                    "open scatter point has null, never an invented duration")

            detail = srv.query("execution_detail", pid=1000,
                               start_ns=str(BASE),
                               end_ns=str(BASE + 20 * MS))
            t.check_eq(detail.get("fidelity"), "exact",
                       "execution_detail carries exact fidelity")
            t.check_eq(detail.get("query_id"), "100",
                       "root query_id is a decimal string")
            leader = detail.get("leader", {})
            t.check_eq(leader.get("query_id"), "100",
                       "leader query_id is a decimal string")
            leader_events = leader.get("events", [])
            t.check_eq(len(leader_events), 2, "leader waterfall has two events")
            clipped = next((e for e in leader_events
                            if e.get("we") == IO_DATA_FILE_READ), {})
            t.check_eq(clipped.get("start_ns"), str(BASE - 2 * MS),
                       "overlapping event preserves its pre-window raw start")
            t.check_eq(clipped.get("dur_ns"), str(4 * MS),
                       "waterfall duration is a nanosecond string")
            t.check(isinstance(clipped.get("cpu_ns"), str),
                    "measured cpu_ns is a string")
            t.check(all(isinstance(e.get("start_ns"), str) and
                        isinstance(e.get("dur_ns"), str) for e in leader_events),
                    "all leader event ns fields are strings")

            workers = detail.get("workers", [])
            t.check_eq([w.get("pid") for w in workers], [1100],
                       "parallel worker is attached as its own lane")
            worker_event = workers[0].get("events", [{}])[0] if workers else {}
            t.check(worker_event.get("cpu_ns") is None,
                    "unmeasured worker cpu_ns is explicit null")
            t.check(isinstance(worker_event.get("start_ns"), str) and
                    isinstance(worker_event.get("dur_ns"), str),
                    "worker event ns values are strings")
            plan = detail.get("plan", {})
            t.check_eq(plan.get("start_ns"), str(BASE - 5 * MS),
                       "detail carries the plan start before execution")
            t.check_eq(plan.get("end_ns"), str(BASE - 3 * MS),
                       "detail plan end is a string")
            t.check(detail.get("truncated") is False and
                    detail.get("kept_count") == detail.get("total_count"),
                    "detail reports honest aggregate event retention")

            no_plan = srv.query("execution_detail", pid=1000,
                                start_ns=str(BASE + 30 * MS),
                                end_ns=str(BASE + 40 * MS))
            t.check(no_plan.get("plan") is None,
                    "detail without a planning phase reports plan null")
    finally:
        cleanup_traces(trace_dir)


def scatter_scenario(n=30):
    events = []
    for i in range(n):
        start = BASE + i * 200 * MS
        duration = 100 * MS if i == 17 else (2 + i % 7) * MS
        events.extend([
            marker(2000, start, EXEC_START, 300 + i % 2),
            {"pid": 2000, "ts": start + MS, "dur": MS,
             "old": IO_DATA_FILE_READ, "new": CPU, "qid": 300 + i % 2},
            marker(2000, start + duration, EXEC_END, 300 + i % 2),
        ])
    events.sort(key=lambda e: e["ts"])
    return {
        "backends": [{"pid": 2000, "type": "client", "user": "u", "db": "d"}],
        "queries": [{"id": 300, "text": "SELECT fast()"},
                    {"id": 301, "text": "SELECT variable()"}],
        "events": events,
    }


def test_scatter(t):
    print("\n### scatter reservoir keeps per-bucket maxima ###")
    trace_dir = generate_traces(scatter_scenario())
    try:
        with ServerHarness(trace_dir) as srv:
            data = srv.query("exec_scatter", max_points=8)
            points = data.get("points", [])
            t.check_eq(data.get("fidelity"), "exact",
                       "scatter carries exact fidelity")
            t.check_eq(data.get("total_count"), 30,
                       "scatter reports raw execution count")
            t.check(data.get("downsampled") is True and
                    data.get("kept_count") == len(points) and
                    len(points) <= 8,
                    "scatter reports honest bounded downsampling")
            t.check(any(abs(p.get("duration_ms", 0) - 100.0) < 0.001
                        for p in points),
                    "100ms outlier survives server-side downsampling")
            t.check(all(isinstance(p.get("t"), str) and
                        isinstance(p.get("query_id"), str) for p in points),
                    "scatter timestamp and query IDs are strings")
            t.check([int(p["t"]) for p in points] ==
                    sorted(int(p["t"]) for p in points),
                    "scatter points are chronological")
    finally:
        cleanup_traces(trace_dir)


def test_straddling_and_marker_identity(t):
    print("\n### window-straddling rows and exact detail identity ###")
    trace_dir = generate_traces(lifecycle_scenario())
    try:
        with ServerHarness(trace_dir) as srv:
            from_ns = BASE + 5 * MS
            to_ns = BASE + 45 * MS
            rows = srv.query("executions", from_=from_ns, to_=to_ns)
            by_start = {r["start_ns"]: r for r in rows.get("rows", [])}
            t.check_eq(rows.get("total_count"), 2,
                       "window includes the execution that started before it")
            t.check(str(BASE) in by_start and
                    by_start[str(BASE)].get("started_before_window") is True,
                    "straddling row preserves its real start and labels it")
            t.check(by_start[str(BASE + 30 * MS)].get(
                        "started_before_window") is False,
                    "in-window start is explicitly not a straddling row")

            scatter = srv.query("exec_scatter", from_=from_ns, to_=to_ns,
                                max_points=100)
            t.check_eq(scatter.get("total_count"), 2,
                       "scatter includes the same straddling execution")
            t.check(any(p.get("t") == str(BASE)
                        for p in scatter.get("points", [])),
                    "scatter retains the pre-window execution identity")

            missing = srv.query("execution_detail", pid=1000,
                                start_ns=str(BASE + 1 * MS),
                                end_ns=str(BASE + 20 * MS))
            t.check_eq(missing.get("code"), "not_found",
                       "unmatched start_ns is a structured not_found refusal")
            t.check("error" in missing and "leader" not in missing and
                    "query_id" not in missing,
                    "unmatched start_ns never fabricates a waterfall payload")
    finally:
        cleanup_traces(trace_dir)


def test_execution_filters(t):
    print("\n### class/event filters reach all execution commands ###")
    trace_dir = generate_traces(lifecycle_scenario())
    try:
        with ServerHarness(trace_dir) as srv:
            for filters, expected, label in [
                ({"class": "LWLock"}, 1, "class"),
                ({"event_id": IO_DATA_FILE_READ}, 3, "event_id"),
            ]:
                rows = srv.query("executions", filters=filters)
                t.check_eq(rows.get("total_count"), expected,
                           f"executions applies {label} through event matching")
                scatter = srv.query("exec_scatter", filters=filters,
                                    max_points=100)
                t.check_eq(scatter.get("total_count"), expected,
                           f"exec_scatter applies {label} through event matching")

            lock_detail = srv.query(
                "execution_detail", pid=1000, filters={"class": "LWLock"},
                start_ns=str(BASE), end_ns=str(BASE + 20 * MS))
            t.check_eq(lock_detail.get("total_count"), 1,
                       "execution_detail applies class to leader+worker lanes")
            t.check_eq(lock_detail.get("leader", {}).get("events"), [],
                       "class filter removes nonmatching leader events")
            t.check_eq(lock_detail.get("workers", [{}])[0].get("events", [{}])[0].get("we"),
                       LWLOCK_WAL_WRITE,
                       "class filter retains the matching worker event")

            io_detail = srv.query(
                "execution_detail", pid=1000,
                filters={"event_id": IO_DATA_FILE_READ},
                start_ns=str(BASE), end_ns=str(BASE + 20 * MS))
            t.check_eq(io_detail.get("total_count"), 1,
                       "execution_detail applies event_id")
            t.check_eq(io_detail.get("leader", {}).get("events", [{}])[0].get("we"),
                       IO_DATA_FILE_READ,
                       "event_id detail contains only the requested event")
            t.check_eq(io_detail.get("workers"), [],
                       "event_id detail omits nonmatching worker lanes")
    finally:
        cleanup_traces(trace_dir)


def test_sort_order_and_fallback(t):
    print("\n### #222: sort=duration_desc orders by duration (in_progress "
          "ranked by elapsed-so-far, never last-by-default); "
          "omitted/unrecognized sort falls back to start_desc ###")
    # Six single-pid executions: three closed with deliberately distinct
    # durations, and three never-closed (in_progress) ones -- a test that
    # happened to pick durations already in start order, or in_progress
    # rows that all trail every closed row, could pass without the
    # comparator actually consulting elapsed-so-far.
    TO = BASE + 1000 * MS   # the request's own window bound, not wall-clock
    events = [
        marker(2000, BASE, EXEC_START, 700),
        marker(2000, BASE + 50 * MS, EXEC_END, 700),        # 50ms, closed
        marker(2001, BASE + 1 * MS, EXEC_START, 700),
        marker(2001, BASE + 11 * MS, EXEC_END, 700),        # 10ms, closed
        marker(2002, BASE + 2 * MS, EXEC_START, 700),
        marker(2002, BASE + 202 * MS, EXEC_END, 700),       # 200ms, closed
        # #222 review: a currently-running execution must not sort last on
        # a tab titled "slowest first" -- that defeats the tab's purpose in
        # exactly the case an operator needs it (an incident). 2003/2004
        # have been running since near BASE, so at TO they are the two
        # longest-running things in the trace and must rank ABOVE every
        # closed row, not below all of them.
        marker(2003, BASE + 3 * MS, EXEC_START, 700),       # never closes: elapsed 997ms at TO
        marker(2004, BASE + 4 * MS, EXEC_START, 700),       # never closes: elapsed 996ms at TO
        # 2005's elapsed-so-far at TO is exactly 200ms -- ties pid 2002's
        # closed duration. The tie-break (start_ns desc, then pid desc)
        # must still pick a single deterministic winner between an
        # in_progress row and a completed row of equal effective duration.
        marker(2005, BASE + 800 * MS, EXEC_START, 700),     # never closes: elapsed 200ms at TO
    ]
    scenario = {
        "backends": [{"pid": p, "type": "client", "user": "u", "db": "d"}
                     for p in (2000, 2001, 2002, 2003, 2004, 2005)],
        "queries": [{"id": 700, "text": "SELECT sort_fixture()"}],
        "events": sorted(events, key=lambda e: e["ts"]),
    }
    trace_dir = generate_traces(scenario)
    try:
        with ServerHarness(trace_dir) as srv:
            duration_desc = srv.query("executions", sort="duration_desc",
                                      from_=BASE, to_=TO)
            pids = [r["pid"] for r in duration_desc.get("rows", [])]
            # 2003 (elapsed 997ms) > 2004 (996ms) > [2005 (200ms) / 2002
            # (200ms), tied -- 2005 wins on start_ns desc: 800ms > 2ms] >
            # 2000 (50ms) > 2001 (10ms).
            t.check_eq(pids, [2003, 2004, 2005, 2002, 2000, 2001],
                       f"duration_desc: a long-running in_progress row "
                       f"outranks short closed ones, and an elapsed-so-far "
                       f"tie against a closed row is broken by start_ns "
                       f"desc, not left to insertion order ({pids})")

            for sort_kwargs, label in [({}, "sort omitted"),
                                        ({"sort": "bogus"}, "unrecognized sort")]:
                resp = srv.query("executions", from_=BASE, to_=TO,
                                 **sort_kwargs)
                pids = [r["pid"] for r in resp.get("rows", [])]
                t.check_eq(pids, [2005, 2004, 2003, 2002, 2001, 2000],
                           f"{label}: falls back to start_desc, not some "
                           f"other order and not a refusal ({pids})")
    finally:
        cleanup_traces(trace_dir)


# #222's measured shape: a query firing roughly every 9.5s genuinely captured
# but reliably crowded out of the default 100-row recency slice by ~125
# exec/s of pgbench traffic, order 75k executions over a ~10 minute capture.
# Scaled down here to keep this test's own runtime reasonable (a smaller
# window, coarser fast-path rate) while staying in the same "tens of
# thousands of executions" regime #101's fix had to hold its query-latency
# budget at -- this is also the scale used to demonstrate that budget below.
SLOW_QID = 918273645

def crowded_scenario(window_s=300, fast_period_ms=12, slow_period_s=9.5):
    events = []
    backends = set()
    fast_pids = list(range(5000, 5020))  # 20 concurrent-ish fast backends
    t_ns = 0
    i = 0
    window_ns = window_s * 1_000_000_000
    while t_ns < window_ns:
        pid = fast_pids[i % len(fast_pids)]
        qid = 100 + (i % 5)
        backends.add(pid)
        start = BASE + t_ns
        events.append(marker(pid, start, EXEC_START, qid))
        events.append({"pid": pid, "ts": start + 1000, "dur": 200_000,
                       "old": CPU, "new": CPU, "qid": qid})
        events.append(marker(pid, start + 210_000, EXEC_END, qid))
        t_ns += fast_period_ms * 1_000_000
        i += 1
    n_fast = i

    slow_pid = 6000
    backends.add(slow_pid)
    t_ns = 0
    j = 0
    slow_period_ns = int(slow_period_s * 1_000_000_000)
    while t_ns < window_ns:
        start = BASE + t_ns
        events.append(marker(slow_pid, start, EXEC_START, SLOW_QID))
        events.append({"pid": slow_pid, "ts": start + MS, "dur": 400 * MS,
                       "old": IO_DATA_FILE_READ, "new": CPU,
                       "qid": SLOW_QID, "cpu": 0})
        events.append(marker(slow_pid, start + 401 * MS, EXEC_END, SLOW_QID))
        t_ns += slow_period_ns
        j += 1
    n_slow = j

    events.sort(key=lambda e: e["ts"])
    scenario = {
        "backends": [{"pid": p, "type": "client", "user": "u", "db": "d"}
                     for p in sorted(backends)],
        "queries": [{"id": 100 + k, "text": f"SELECT fast_{k}()"}
                    for k in range(5)] +
                   [{"id": SLOW_QID, "text": "SELECT pg_report_slow()"}],
        "events": events,
    }
    return scenario, n_fast, n_slow


def test_crowding_check_and_query_latency_bound(t):
    print("\n### #222 cheap check + #101 budget at tens-of-thousands scale ###")
    scenario, n_fast, n_slow = crowded_scenario()
    trace_dir = generate_traces(scenario)
    try:
        t.check(n_fast >= 20_000,
                f"fixture reaches tens-of-thousands of executions ({n_fast} fast + {n_slow} slow)")
        with ServerHarness(trace_dir) as srv:
            # ── The issue's own "cheap check, before any code" ──────────
            # Filtering to the slow query's id bypasses the recency
            # truncation entirely (execution_matches_request runs BEFORE
            # the sort+limit), so a present row means the execution was
            # genuinely captured -- "crowded out of the default view", not
            # "lost". An absent row here would mean the opposite: a real,
            # more serious capture defect, not this issue.
            probe = srv.query("executions", limit=1,
                              filters={"query_id": SLOW_QID},
                              timeout=WATERFALL_QUERY_THRESHOLD_S)
            t.check_eq(len(probe.get("rows", [])), 1,
                       "cheap check: the slow query is genuinely captured "
                       "(present under a query_id filter)")

            # ── The regression this issue is actually about ─────────────
            default_page = srv.query("executions", limit=100,
                                     timeout=WATERFALL_QUERY_THRESHOLD_S)
            default_pids = {r["pid"] for r in default_page.get("rows", [])}
            t.check(6000 not in default_pids,
                    "characterizes the bug: the unsorted default 100-row "
                    "page is the fast-only recency tail, crowding the slow "
                    "query out (pid 6000 absent)")

            # ── The fix: ask for the slowest instead ─────────────────────
            slow_start = time.monotonic()
            slowest_page = srv.query("executions", limit=100,
                                     sort="duration_desc",
                                     timeout=WATERFALL_QUERY_THRESHOLD_S)
            slow_elapsed = time.monotonic() - slow_start
            slowest_pids = {r["pid"] for r in slowest_page.get("rows", [])}
            t.check(6000 in slowest_pids,
                    "sort=duration_desc surfaces the slow query in the "
                    "default-sized page instead of the recency tail")

            # ── #101's bound, demonstrated at this scale for BOTH sorts:
            # sorting the full matching window by duration is the same
            # qsort over the same already-materialized row array as the
            # pre-existing start_desc sort -- no new pass over the
            # underlying trace events, so the budget that closed #101 is
            # unaffected by which comparator runs. ─────────────────────
            recent_start = time.monotonic()
            srv.query("executions", limit=100,
                     timeout=WATERFALL_QUERY_THRESHOLD_S)
            recent_elapsed = time.monotonic() - recent_start
            t.check(recent_elapsed < WATERFALL_QUERY_THRESHOLD_S,
                    f"start_desc (default) executions query: "
                    f"{recent_elapsed:.2f}s over ~{n_fast + n_slow} executions "
                    f"(threshold {WATERFALL_QUERY_THRESHOLD_S}s)")
            t.check(slow_elapsed < WATERFALL_QUERY_THRESHOLD_S,
                    f"duration_desc executions query: {slow_elapsed:.2f}s "
                    f"over ~{n_fast + n_slow} executions "
                    f"(threshold {WATERFALL_QUERY_THRESHOLD_S}s)")
    finally:
        cleanup_traces(trace_dir)


# #222 review (round 2 blocker): ranking an open row by elapsed-so-far
# fixed the original defect (a long-running open execution no longer sorts
# last) but risked the same defect with the sign flipped -- a flood of
# open executions, each merely OLDER than a genuinely slow CLOSED query,
# dominating duration_desc's truncated page exactly the way the old
# recency slice dominated start_desc. The flood this actually happens
# with is not 100 concurrent backends: it is ~100 CUMULATIVE
# errored/cancelled statements over a capture. Each one never reaches
# src/daemon.c's query__execute__done probe (an ERROR longjmp, a cancel,
# statement_timeout, or a client disconnect mid-query all skip it, same
# as an EXEC_END lost to a ringbuf drop) -- but the pid's NEXT CMD_END,
# emitted on the pgstat_report_activity gate flip to IDLE, DOES fire
# after all of those, and src/compute.c's pgwt_compute_executions now
# closes the stale row there with a REAL measured duration instead of
# leaving it in_progress to accumulate an ever-growing fabricated one.
ZOMBIE_QID = 314159265
CMD_END = 0xFFFFFFF7   # PGWT_MARKER_CMD_END, src/pg_wait_tracer.h


def zombie_row_scenario(n_zombies=100):
    events = []
    # No backends.jsonl entries here: pgwt_compute_executions derives every
    # row from EXEC_START/END/CMD_END markers alone (backend metadata only
    # feeds parallel-worker attribution and other views' user/db labels,
    # neither of which these single-pid leader executions touch), and
    # tests/gen_test_traces.c's fixed-size backend array
    # (MAX_TEST_BACKENDS 64) cannot hold 100+ entries anyway.
    zombie_pids = []
    for i in range(n_zombies):
        pid = 20000 + i
        zombie_pids.append(pid)
        start = BASE + i * MS
        events.append(marker(pid, start, EXEC_START, ZOMBIE_QID))
        # ERROR/cancel/timeout: no EXEC_END -- only the command closing,
        # 2ms later, ever reaches this pid's stream again.
        events.append(marker(pid, start + 2 * MS, CMD_END, ZOMBIE_QID))
    # One genuinely slow CLOSED execution -- 500ms, two orders of magnitude
    # past any zombie's real (post-CMD_END) duration -- that a "longest
    # running first" tab exists to surface.
    slow_pid = 30000
    slow_start = BASE + (n_zombies + 5) * MS
    events.append(marker(slow_pid, slow_start, EXEC_START, ZOMBIE_QID))
    events.append(marker(slow_pid, slow_start + 500 * MS, EXEC_END, ZOMBIE_QID))
    events.sort(key=lambda e: e["ts"])
    scenario = {
        "queries": [{"id": ZOMBIE_QID, "text": "SELECT zombie_row_fixture()"}],
        "events": events,
    }
    return scenario, zombie_pids, slow_pid


def test_open_row_crowding_characterized(t):
    print("\n### #222 review: an errored/cancelled statement's row does not "
          "survive its pid's next CMD_END -- closes with a real measured "
          "duration, so 100 cumulative zombies no longer crowd a "
          "genuinely slow execution off the duration_desc page ###")
    scenario, zombie_pids, slow_pid = zombie_row_scenario()
    trace_dir = generate_traces(scenario)
    to_ns = BASE + 100_000 * MS   # far past every zombie's start
    try:
        with ServerHarness(trace_dir) as srv:
            page = srv.query("executions", limit=100, sort="duration_desc",
                             from_=BASE, to_=to_ns,
                             timeout=WATERFALL_QUERY_THRESHOLD_S)
            t.check_eq(page.get("total_count"), len(zombie_pids) + 1,
                       "fixture carries 100 zombies + 1 genuinely slow "
                       "execution")
            t.check_eq(page.get("open_count"), 0,
                       "every zombie row closed at its own CMD_END -- none "
                       "left in_progress (open_count=0, not 100)")
            t.check_eq(page.get("completed_count"), len(zombie_pids) + 1,
                       "every zombie plus the slow execution is now a "
                       "closed row")

            rows_by_pid = {r["pid"]: r for r in page.get("rows", [])}
            # Only the last 5 (latest-started) zombies are guaranteed to
            # survive the limit=100 truncation of 101 total rows -- they
            # all tie at ~2ms, tie-broken by start_ns desc, so the single
            # dropped row is the EARLIEST zombie, never one of these.
            for pid in zombie_pids[-5:]:
                row = rows_by_pid.get(pid)
                t.check(row is not None and row.get("in_progress") is False,
                        f"zombie pid {pid} is closed, not in_progress ({row})")
                t.check(row is not None and row.get("duration_ms") is not None
                        and row["duration_ms"] < 10.0,
                        f"zombie pid {pid} carries its REAL ~2ms duration, "
                        f"not one fabricated from the ~100s window bound "
                        f"({row})")
                t.check(row is not None and row.get("end_inferred") is True,
                        f"zombie pid {pid} is flagged end_inferred -- closed "
                        f"at CMD_END, never a real EXEC_END ({row})")

            t.check(slow_pid in rows_by_pid,
                    "FIXED (would have been crowded out before this "
                    "round's fix): the genuinely slow 500ms execution is "
                    "on the duration_desc page, not pushed off by 100 "
                    "zombie rows each now carrying a real ~2ms duration "
                    "instead of a fabricated one")
            t.check(rows_by_pid.get(slow_pid, {}).get("end_inferred") is False,
                    "the genuinely slow execution's real EXEC_END is NOT "
                    "flagged end_inferred -- a measured completion still "
                    "reads as measured")
            pids_in_order = [r["pid"] for r in page.get("rows", [])]
            t.check(pids_in_order and pids_in_order[0] == slow_pid,
                    f"the genuinely slow execution sorts FIRST, ahead of "
                    f"every closed zombie ({pids_in_order[:5]}...)")
    finally:
        cleanup_traces(trace_dir)


# #222 review, adviser-found gap: EXEC_START unconditionally used to
# overwrite the single active_row slot, so a pid that errors and then runs
# ANOTHER statement on the SAME connection orphaned the first row beyond
# the reach of a "close the active row at CMD_END" fix -- the common case,
# not an edge case (a client that hits an error usually goes on to run
# something else). The real-world hazard this covers: a single presenter
# Ctrl-C on an ad-hoc query during a demo, then continuing to work on the
# same connection -- n=1 is enough to pin one "In progress" row at the top
# of the tab for the rest of the session.
SEQ_QID = 271828182

def sequential_errors_scenario(n_errors=5):
    """One pid: N EXEC_START/CMD_END cycles with no EXEC_END (errors),
    THEN one genuinely slow but SUCCESSFUL execution on the SAME
    connection. Includes the n=1 case as errors[0] on its own is already a
    complete reproduction of the single-cancel hazard; n_errors=5 pins
    that repeated errors don't accumulate either."""
    pid = 60000
    events = []
    for i in range(n_errors):
        start = BASE + i * MS
        events.append(marker(pid, start, EXEC_START, SEQ_QID))
        events.append(marker(pid, start + 2 * MS, CMD_END, SEQ_QID))
    slow_start = BASE + (n_errors + 1) * MS
    events.append(marker(pid, slow_start, EXEC_START, SEQ_QID))
    events.append(marker(pid, slow_start + 500 * MS, EXEC_END, SEQ_QID))
    events.sort(key=lambda e: e["ts"])
    scenario = {
        "queries": [{"id": SEQ_QID, "text": "SELECT sequential_errors()"}],
        "events": events,
    }
    return scenario, pid


def test_sequential_errors_on_one_pid_do_not_accumulate(t):
    print("\n### #222 review (adviser gap): sequential errors on ONE pid "
          "each close at their OWN CMD_END -- a second EXEC_START does not "
          "orphan the first row beyond CMD_END's reach ###")
    scenario, pid = sequential_errors_scenario(n_errors=5)
    trace_dir = generate_traces(scenario)
    to_ns = BASE + 100_000 * MS
    try:
        with ServerHarness(trace_dir) as srv:
            page = srv.query("executions", sort="duration_desc",
                             from_=BASE, to_=to_ns,
                             timeout=WATERFALL_QUERY_THRESHOLD_S)
            rows = page.get("rows", [])
            t.check_eq(len(rows), 6,
                       f"5 errored + 1 successful execution, all on one "
                       f"pid, all present ({len(rows)})")
            t.check_eq(page.get("open_count"), 0,
                       "RED before the adviser's fix: a second EXEC_START "
                       "used to orphan the FIRST error's row beyond "
                       "CMD_END's reach, leaving it permanently "
                       "in_progress -- none are, now (open_count=0)")
            errored = [r for r in rows if r["duration_ms"] is not None
                      and r["duration_ms"] < 10.0]
            t.check_eq(len(errored), 5,
                       f"all 5 errors closed with their real ~2ms span, "
                       f"none left open ({[r['duration_ms'] for r in errored]})")
            t.check(all(r.get("end_inferred") is True for r in errored),
                    "every errored row is flagged end_inferred")
            t.check(all(r.get("in_progress") is False for r in rows),
                    f"NOTHING on this pid is in_progress -- the n=1 case "
                    f"alone (any single one of these 5) already proves a "
                    f"single presenter cancel does not pin a row "
                    f"({[r['in_progress'] for r in rows]})")
            slow = next((r for r in rows if r["duration_ms"] and
                        r["duration_ms"] > 100.0), None)
            t.check(slow is not None and slow.get("end_inferred") is False,
                    f"the final successful 500ms execution closed via its "
                    f"own real EXEC_END, not inferred ({slow})")
            t.check(rows[0] is slow if slow else False,
                    f"the successful execution sorts FIRST, ahead of "
                    f"every errored one on the same pid ({rows[0]})")
    finally:
        cleanup_traces(trace_dir)


# #222 review: CMD_END is the correct closing point precisely because it
# only fires once the backend has left RUNNING (src/bpf/pg_wait_tracer.bpf.c:
# the pgstat_report_activity gate flip to IDLE happens after every nested
# portal for this command has itself finished or errored) -- so it cannot
# truncate a still-running statement. This must be PROVEN, not assumed:
# genuine nesting (SQL-level EXECUTE of a prepared statement from inside
# plpgsql) pushes a second EXEC_START onto the same pid's stack while the
# outer is still legitimately open; both must close normally via their own
# EXEC_END, neither early nor truncated by the other.
NEST_QID_OUTER = 100
NEST_QID_INNER = 200

def nested_execution_scenario():
    pid = 80000
    outer_start = BASE
    inner_start = BASE + 5 * MS
    inner_end = BASE + 15 * MS
    outer_end = BASE + 30 * MS
    events = [
        marker(pid, outer_start, EXEC_START, NEST_QID_OUTER),
        marker(pid, inner_start, EXEC_START, NEST_QID_INNER),
        marker(pid, inner_end, EXEC_END, NEST_QID_INNER),
        marker(pid, outer_end, EXEC_END, NEST_QID_OUTER),
    ]
    scenario = {
        "queries": [{"id": NEST_QID_OUTER, "text": "SELECT outer_call()"},
                    {"id": NEST_QID_INNER, "text": "SELECT inner_call()"}],
        "events": events,
    }
    return scenario, pid, outer_start, outer_end, inner_start, inner_end


def test_nested_execution_not_truncated(t):
    print("\n### #222 review: a legitimately nested execution is not cut "
          "short or dropped -- CMD_END closes ONLY what is still open, "
          "and only once the backend has actually gone idle ###")
    (scenario, pid, outer_start, outer_end,
     inner_start, inner_end) = nested_execution_scenario()
    trace_dir = generate_traces(scenario)
    try:
        with ServerHarness(trace_dir) as srv:
            page = srv.query("executions", sort="start_desc",
                             timeout=WATERFALL_QUERY_THRESHOLD_S)
            rows = {r["query_id"]: r for r in page.get("rows", [])}
            t.check_eq(len(page.get("rows", [])), 2,
                       f"both outer and inner rows are present, neither "
                       f"dropped ({page.get('rows')})")
            outer = rows.get(str(NEST_QID_OUTER))
            inner = rows.get(str(NEST_QID_INNER))
            t.check(outer is not None and outer.get("in_progress") is False
                    and outer.get("end_inferred") is False,
                    f"outer execution closed normally via its own real "
                    f"EXEC_END, not truncated by the inner one ({outer})")
            t.check(inner is not None and inner.get("in_progress") is False
                    and inner.get("end_inferred") is False,
                    f"inner execution closed normally via its own real "
                    f"EXEC_END ({inner})")
            t.check(outer is not None and
                    abs(outer["duration_ms"] -
                        (outer_end - outer_start) / MS) < 1e-6,
                    f"outer duration is its OWN full span "
                    f"({outer and outer['duration_ms']}ms, expected "
                    f"{(outer_end - outer_start) / MS}ms) -- not clipped to "
                    f"the inner's span")
            t.check(inner is not None and
                    abs(inner["duration_ms"] -
                        (inner_end - inner_start) / MS) < 1e-6,
                    f"inner duration is its OWN span "
                    f"({inner and inner['duration_ms']}ms, expected "
                    f"{(inner_end - inner_start) / MS}ms) -- not stretched "
                    f"to the outer's span")
    finally:
        cleanup_traces(trace_dir)


# #222 review (accounting misattribution): the reviewer's own bug
# description -- a pid errors on A (row stays stacked, no EXEC_END), then
# runs and finishes B on the same connection. A wait event landing after
# B's EXEC_END but before anything else touches this pid's stack must be
# credited to NEITHER row: A did not experience it (A is not running --
# it is a dead orphan), and B is already closed. Before the
# top_attributable fix, exec_pid_peek_row(st) handed that event to
# whatever the pop exposed underneath B -- which is A -- silently
# inflating a long-dead orphan's n_events (and flipping
# matches_event_filter on it) for as long as CMD_END never arrives.
MISATTR_QID_A = 555111
MISATTR_QID_B = 555222


def misattributed_orphan_scenario():
    pid = 70000
    events = [
        marker(pid, BASE, EXEC_START, MISATTR_QID_A),
        # A's own genuine wait event, while A is legitimately on top.
        {"pid": pid, "ts": BASE + 1 * MS, "dur": 1 * MS,
         "old": IO_DATA_FILE_READ, "new": CPU, "qid": MISATTR_QID_A,
         "cpu": 0},
        # A errors (no EXEC_END); the same pid runs B next.
        marker(pid, BASE + 5 * MS, EXEC_START, MISATTR_QID_B),
        # B's own genuine wait event.
        {"pid": pid, "ts": BASE + 6 * MS, "dur": 1 * MS,
         "old": IO_DATA_FILE_READ, "new": CPU, "qid": MISATTR_QID_B,
         "cpu": 0},
        marker(pid, BASE + 8 * MS, EXEC_END, MISATTR_QID_B),
        # Strictly after B's EXEC_END, strictly before CMD_END: the row
        # this exposes on top of the stack is A, but A must NOT get it.
        {"pid": pid, "ts": BASE + 9 * MS, "dur": 1 * MS,
         "old": IO_DATA_FILE_READ, "new": CPU, "qid": MISATTR_QID_A,
         "cpu": 0},
        # Only CMD_END may safely close A (see compute.c's own comment).
        marker(pid, BASE + 20 * MS, CMD_END, MISATTR_QID_A),
    ]
    events.sort(key=lambda e: e["ts"])
    scenario = {
        "queries": [{"id": MISATTR_QID_A, "text": "SELECT a_errors()"},
                    {"id": MISATTR_QID_B, "text": "SELECT b_succeeds()"}],
        "events": events,
    }
    return scenario, pid


def test_orphan_does_not_absorb_events_after_inner_exec_end(t):
    print("\n### #222 review: a wait event after an inner EXEC_END pops "
          "the stack is credited to NEITHER the exposed orphan NOR the "
          "closed inner row -- not silently mis-attributed to whichever "
          "row the pop happened to reveal ###")
    scenario, pid = misattributed_orphan_scenario()
    trace_dir = generate_traces(scenario)
    to_ns = BASE + 100_000 * MS
    try:
        with ServerHarness(trace_dir) as srv:
            page = srv.query("executions", sort="start_desc",
                             from_=BASE, to_=to_ns,
                             timeout=WATERFALL_QUERY_THRESHOLD_S)
            rows = {r["query_id"]: r for r in page.get("rows", [])}
            row_a = rows.get(str(MISATTR_QID_A))
            row_b = rows.get(str(MISATTR_QID_B))
            t.check(row_a is not None and row_b is not None,
                    f"both A and B rows are present ({page.get('rows')})")
            t.check_eq(row_a.get("n_events"), 1,
                       "A ends with exactly its OWN one genuine event -- "
                       "the post-B-EXEC_END event must NOT land on the "
                       "reopened orphan (RED before the fix: this reads 2)")
            t.check_eq(row_b.get("n_events"), 1,
                       "B keeps exactly its own one genuine event")
            t.check(row_a.get("end_inferred") is True and
                    row_a.get("in_progress") is False,
                    f"A closes via CMD_END, inferred, not left open ({row_a})")
    finally:
        cleanup_traces(trace_dir)


def test_detail_window_bound_and_bounded_context(t):
    print("\n### detail bound is window-local and still enforced ###")
    start = BASE + 50_000 * MS
    events = []
    for i in range(40):
        events.append({"pid": 9000, "ts": BASE + i * MS, "dur": MS,
                       "old": CPU, "new": IO_DATA_FILE_READ, "qid": 900})
    events.extend([
        marker(9100, start, EXEC_START, 910),
        {"pid": 9100, "ts": start + MS, "dur": MS,
         "old": IO_DATA_FILE_READ, "new": CPU, "qid": 910},
        marker(9100, start + 2 * MS, EXEC_END, 910),
    ])
    scenario = {
        "backends": [
            {"pid": 9000, "type": "client", "user": "u", "db": "d"},
            {"pid": 9100, "type": "client", "user": "u", "db": "d"},
        ],
        "events": sorted(events, key=lambda e: e["ts"]),
    }
    trace_dir = generate_traces(scenario)
    try:
        with ServerHarness(trace_dir, env={"PGWT_LOAD_MAX_EVENTS": "10"}) as srv:
            detail = srv.query("execution_detail", pid=9100,
                               start_ns=str(start), end_ns=str(start + 2 * MS))
            t.check_eq(detail.get("total_count"), 1,
                       "unrelated full-trace volume does not refuse tiny detail")
            t.check("error" not in detail,
                    "bounded pid-pushed marker pre-scan avoids full-trace overload")
    finally:
        cleanup_traces(trace_dir)

    many = [marker(9200, start, EXEC_START, 920)]
    for i in range(20):
        many.append({"pid": 9200, "ts": start + i + 1, "dur": 1,
                     "old": CPU, "new": IO_DATA_FILE_READ, "qid": 920})
    many.append(marker(9200, start + 30, EXEC_END, 920))
    trace_dir = generate_traces({
        "backends": [{"pid": 9200, "type": "client", "user": "u", "db": "d"}],
        "events": many,
    })
    try:
        with ServerHarness(trace_dir, env={"PGWT_LOAD_MAX_EVENTS": "10"}) as srv:
            refused = srv.query("execution_detail", pid=9200,
                                start_ns=str(start), end_ns=str(start + 30))
            t.check_eq(refused.get("code"), "window_too_large",
                       "execution_detail still refuses an actually oversized window")
            t.check_eq(refused.get("max_events"), 10,
                       "detail bound reports the configured event cap")
            t.check_eq(refused.get("fidelity"), "exact",
                       "overload refusal carries freshly-derived exact fidelity")
    finally:
        cleanup_traces(trace_dir)


def test_clustered_scatter_fills_budget(t):
    print("\n### clustered scatter redistributes empty-bucket quota ###")
    events = []
    for i in range(400):
        start = BASE + i * 100_000
        events.extend([
            marker(9300, start, EXEC_START, 930),
            marker(9300, start + 50_000, EXEC_END, 930),
        ])
    outlier_start = BASE + 9_000 * MS
    events.extend([
        marker(9300, outlier_start, EXEC_START, 930),
        marker(9300, outlier_start + 100 * MS, EXEC_END, 930),
    ])
    trace_dir = generate_traces({
        "backends": [{"pid": 9300, "type": "client", "user": "u", "db": "d"}],
        "events": sorted(events, key=lambda e: e["ts"]),
    })
    try:
        with ServerHarness(trace_dir) as srv:
            data = srv.query("exec_scatter", from_=BASE,
                             to_=BASE + 10_000 * MS, max_points=100)
            t.check_eq(data.get("total_count"), 401,
                       "clustered fixture exposes all executions before sampling")
            t.check_eq(data.get("kept_count"), 100,
                       "unused bucket quota is redistributed to fill the budget")
            t.check(any(p.get("t") == str(outlier_start)
                        for p in data.get("points", [])),
                    "far-bucket maximum remains guaranteed")
            again = srv.query("exec_scatter", from_=BASE,
                              to_=BASE + 10_000 * MS, max_points=100)
            t.check_eq(again.get("points"), data.get("points"),
                       "redistributed reservoir remains deterministic")
    finally:
        cleanup_traces(trace_dir)


def test_detail_cap(t):
    print("\n### per-lane waterfall cap is explicit ###")
    start = BASE + 20_000 * MS
    event_count = 2005
    events = [marker(4000, start, EXEC_START, 500)]
    for i in range(event_count):
        events.append({"pid": 4000, "ts": start + i + 1, "dur": 1,
                       "old": CPU, "new": IO_DATA_FILE_READ, "qid": 500})
    end = start + event_count + 1
    events.append(marker(4000, end, EXEC_END, 500))
    scenario = {
        "backends": [{"pid": 4000, "type": "client", "user": "u", "db": "d"}],
        "queries": [{"id": 500, "text": "SELECT many_steps()"}],
        "events": events,
    }
    trace_dir = generate_traces(scenario)
    try:
        with ServerHarness(trace_dir) as srv:
            detail = srv.query("execution_detail", pid=4000,
                               start_ns=str(start), end_ns=str(end))
            leader = detail.get("leader", {})
            t.check_eq(len(leader.get("events", [])), 2000,
                       "leader lane is capped at 2000 events")
            t.check_eq(leader.get("total_count"), event_count,
                       "lane reports its uncapped event count")
            t.check(leader.get("truncated") is True and
                    detail.get("truncated") is True,
                    "lane and root both report truncation")
            t.check_eq(detail.get("kept_count"), 2000,
                       "root reports the exact retained event count")
    finally:
        cleanup_traces(trace_dir)


def test_sampled_refusal(t):
    print("\n### exact-required commands refuse sampled-only data ###")
    scenario = {
        "sample_period_ns": 100 * MS,
        "backends": [{"pid": 3000, "type": "client", "user": "u", "db": "d"}],
        "samples": [
            {"pid": 3000, "ts": BASE, "event": IO_DATA_FILE_READ, "qid": 400},
            {"pid": 3000, "ts": BASE + 100 * MS,
             "event": IO_DATA_FILE_READ, "qid": 400},
        ],
    }
    trace_dir = generate_traces(scenario)
    try:
        with ServerHarness(trace_dir) as srv:
            requests = [
                ("executions", {}),
                ("exec_scatter", {"max_points": 10}),
                ("execution_detail", {"pid": 3000,
                                      "start_ns": str(BASE),
                                      "end_ns": str(BASE + 100 * MS)}),
            ]
            for cmd, kwargs in requests:
                data = srv.query(cmd, **kwargs)
                t.check_eq(data.get("fidelity"), "sampled",
                           f"{cmd}: refusal carries sampled fidelity")
                t.check_eq(data.get("code"), "full_fidelity_required",
                           f"{cmd}: refusal carries stable code")
                t.check_eq(data.get("unavailable"),
                           "requires full-fidelity data",
                           f"{cmd}: refusal is explicit, not empty success")
                t.check("hint" in data and "rows" not in data and
                        "points" not in data and "leader" not in data,
                        f"{cmd}: refusal has a hint and no fabricated payload")
    finally:
        cleanup_traces(trace_dir)


def main():
    t = TestRunner("test_data_executions")
    test_lifecycle_and_detail(t)
    test_scatter(t)
    test_straddling_and_marker_identity(t)
    test_execution_filters(t)
    test_sort_order_and_fallback(t)
    test_crowding_check_and_query_latency_bound(t)
    test_open_row_crowding_characterized(t)
    test_sequential_errors_on_one_pid_do_not_accumulate(t)
    test_nested_execution_not_truncated(t)
    test_orphan_does_not_absorb_events_after_inner_exec_end(t)
    test_detail_window_bound_and_bounded_context(t)
    test_clustered_scatter_fills_budget(t)
    test_detail_cap(t)
    test_sampled_refusal(t)
    return 0 if t.summary() else 1


if __name__ == "__main__":
    sys.exit(main())
