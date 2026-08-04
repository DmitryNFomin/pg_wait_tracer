#!/usr/bin/env python3
"""U3/B6 execution list, waterfall, worker lanes, and scatter honesty."""
import os
import sys

sys.path.insert(0, os.path.dirname(__file__))
from server_harness import (
    ServerHarness, generate_traces, cleanup_traces, TestRunner,
    CPU, IO_DATA_FILE_READ, LWLOCK_WAL_WRITE,
)

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
    test_detail_window_bound_and_bounded_context(t)
    test_clustered_scatter_fills_budget(t)
    test_detail_cap(t)
    test_sampled_refusal(t)
    return 0 if t.summary() else 1


if __name__ == "__main__":
    sys.exit(main())
