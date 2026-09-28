#!/usr/bin/env python3
"""demo_workload_coverage.py -- issue #214: "the demo workload must make
every tab show something, not just satisfy the conservation maths".

Companion to tests/live_loop_workload.py, not a fork of anything: this is
the machine-checkable half of the tab table in that file's own docstring
and in docs/DEMO_REHEARSAL_CRITERIA.md's workload section. It answers one
question per tab -- "does this capture have the kind of data that tab
needs to show real content, not an empty state" -- against a REAL trace
dir a real daemon capture produced (not a synthetic/mock fixture), because
`rendered: ok` on an empty panel is exactly how #214's gap was missed in
the first place (an empty Waterfall/Scatter passed every existing check).

Split in two layers on purpose:
  - the *_populated() functions below are PURE: given an already-parsed
    pgwt-server JSON response for that tab's query, they return (ok, detail)
    with no I/O, no subprocess, no server connection -- unit-tested directly
    by tests/test_demo_workload_coverage.py (python3 tests/test_demo_workload_coverage.py,
    runs on the Mac, no PG, no BPF).
  - main() below is the non-pure glue: it starts a pgwt-server subprocess
    against a real --trace-dir (server_harness.ServerHarness, the same
    harness tests/demo_rehearsal.py's own end-of-capture checks use),
    issues each tab's real query over the capture's own window, and prints
    a PASS/FAIL line per tab -- exit 0 iff every tab passes.

TAB_QUERIES maps each of ui_live_smoke_lib.TABS to the exact pgwt-server
`cmd` its own view issues (web/static/views/*.js requests()) and the pure
checker above. Kept as literal command names here rather than importing
each view's JS (there is no cheap cross-language import) -- if a view's
query command ever changes, this table and web/static/views/<tab>.js's
`requests()` will disagree, which is a real drift worth catching, not
papered over.

Usage:
    python3 tests/demo_workload_coverage.py --trace-dir /tmp/pgwt_demo_XXXXXX
    python3 tests/demo_workload_coverage.py --trace-dir DIR --window-s 300
"""
import argparse
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

# ── constants (all thresholds live here, named, not buried in a condition) ──

# Concurrency: issue #214's own table asks for "AAS above the CPU-count
# line at least once, to show saturation" -- and the CPU count IS reachable
# here, not a proxy. It is the same `num_cpus` field the UI's own "N CPUs"
# chip reads (web/static/views/active.js: ctx.server.numCpus, set from the
# `info` response's num_cpus -- src/server.c handle_info,
# cJSON_AddNumberToObject(root, "num_cpus", srv->num_cpus)). A first cut of
# this module used a fixed proxy floor (AAS >= 1.5) because it did not look
# for this field; checked concretely (grepped web/static/lib/builders/aas.js,
# web/static/views/active.js and src/server.c's `info` handler) and it is
# there, so concurrency_populated() below takes num_cpus as a required
# argument -- a missing/invalid value REFUSES rather than silently grading
# against the old proxy (a gate that cannot see the real line must say so).

# Waterfall: issue #214's acceptance criterion names this explicitly --
# "at least one slow enough to be interesting". 500ms is comfortably above
# noise (a sub-ms catalog lookup) and comfortably below live_loop_workload's
# own long pg_sleep(1.3) / pg_sleep(3), so it fails on an unlucky sample
# without the workload's slow leg, not just discriminates fast-vs-slow.
WATERFALL_INTERESTING_MS = 500.0

# Scatter: "enough completed executions to form a visible distribution".
# Both a floor on count and a floor on spread -- five identical-duration
# points are not a distribution.
SCATTER_MIN_POINTS = 5
SCATTER_MIN_SPREAD_RATIO = 3.0

# Idle/client-wait nodes the transitions/matrix graph always carries on any
# live connection regardless of workload (mirrors
# web/static/views/transitions.js IDLE_NODE_NAMES) -- excluded so "the
# Client:ClientRead<->CPU* loop exists" does not count as workload-driven
# content.
IDLE_NODE_NAMES = ("Client:ClientRead",)


# ── pure per-tab checkers: parsed JSON in, (ok, detail) out, no I/O ────────

def overview_populated(resp):
    """time_model: >=2 class-level (indent==1) wait classes with ms>0.

    indent==0 is the single "DB Time" grand-total row (src/server.c
    handle_time_model); indent==1 rows are the classes ("Lock", "Timeout",
    "CPU", ...); indent==2 rows are sub-events within a class. Confirmed
    against a real capture (issue #214 box-check, 2026-09-28): a first cut
    of this checker filtered indent==0 and always saw exactly one row
    ("DB Time" itself), so it could never pass -- caught by running against
    a real trace dir, not by reasoning about the schema."""
    rows = (resp or {}).get("rows") or []
    classes = {r.get("name") for r in rows
               if r.get("indent") == 1 and (r.get("ms") or 0) > 0}
    ok = len(classes) >= 2
    return ok, f"class-level wait classes with ms>0: {sorted(c for c in classes if c)}"


def events_populated(resp):
    """top_events: >=2 distinct event classes with total_ms>0.

    top_events rows carry "total_ms" (not "ms" -- that is time_model's
    field name for the same quantity); a real-capture run (issue #214
    box-check) caught an earlier version of this checker reading "ms" here,
    which is always absent from a top_events row, so it never passed."""
    rows = (resp or {}).get("rows") or []
    classes = {r.get("class") for r in rows
               if r.get("class") and (r.get("total_ms") or 0) > 0}
    ok = len(classes) >= 2
    return ok, f"event classes with total_ms>0: {sorted(classes)}"


def sessions_populated(resp):
    """top_sessions: several concurrent backends, not just one."""
    rows = (resp or {}).get("rows") or []
    ok = len(rows) >= 2
    return ok, f"session rows: {len(rows)}"


def queries_populated(resp):
    """top_queries: more than one distinct statement, with differing totals."""
    rows = (resp or {}).get("rows") or []
    texts = {r.get("text") for r in rows if r.get("text")}
    totals = {r.get("total_ms") for r in rows if r.get("total_ms") is not None}
    ok = len(texts) >= 2 and len(totals) >= 2
    return ok, (f"distinct query texts={len(texts)}, "
                f"distinct total_ms values={len(totals)}")


def histogram_populated(resp):
    """heatmap: a spread of durations (>=2 nonzero latency bands), not one mode."""
    cells = (resp or {}).get("cells") or []
    bands = {c[1] for c in cells if len(c) >= 3 and c[2] > 0}
    ok = len(bands) >= 2
    return ok, f"nonzero latency bands: {sorted(bands)}"


def timeline_populated(resp):
    """session_timeline: a backend whose waits change over time (>=2
    distinct event names for the same pid)."""
    events = (resp or {}).get("events") or []
    by_pid = {}
    for e in events:
        by_pid.setdefault(e.get("p"), set()).add(e.get("n"))
    changing = [pid for pid, names in by_pid.items() if len(names) >= 2]
    ok = len(changing) >= 1
    return ok, f"pids with >=2 distinct wait names: {changing} (pids seen: {len(by_pid)})"


def _non_idle_links(resp):
    links = (resp or {}).get("links") or []
    return [l for l in links
            if l.get("source") not in IDLE_NODE_NAMES
            and l.get("target") not in IDLE_NODE_NAMES
            and l.get("source") != l.get("target")
            and (l.get("value") or 0) > 0]


def transitions_populated(resp):
    """transitions: actual (non-idle, non-self) transitions between wait states."""
    links = _non_idle_links(resp)
    ok = len(links) >= 1
    return ok, f"non-idle state transitions: {len(links)}"


def matrix_populated(resp):
    """transitions (matrix's own params): more than one distinct non-idle
    (source, target) combination."""
    links = _non_idle_links(resp)
    pairs = {(l.get("source"), l.get("target")) for l in links}
    ok = len(pairs) >= 2
    return ok, f"distinct non-idle transition pairs: {sorted(pairs)}"


def concurrency_populated(resp, num_cpus):
    """concurrency: AAS peaks present, with at least one instant where AAS
    reaches or exceeds the capture box's own CPU count -- the literal
    "above the CPU-count line" criterion (see the module docstring above
    for where num_cpus comes from). `num_cpus` is REQUIRED and validated
    here rather than defaulted: a caller that cannot supply it (an old
    server predating the field, a bad response) gets an explicit FAIL
    naming why, never a silent pass against a weaker floor."""
    peaks = (resp or {}).get("peaks") or []
    if not peaks:
        return False, "no AAS peaks in window"
    if (not isinstance(num_cpus, (int, float)) or isinstance(num_cpus, bool)
            or num_cpus <= 0):
        return False, (f"num_cpus unavailable/invalid ({num_cpus!r}) -- "
                        f"cannot verify AAS against the real CPU-count line")
    best = max((p.get("max") or 0) for p in peaks)
    ok = best >= num_cpus
    return ok, f"peaks={len(peaks)}, max AAS={best:.2f} (CPU-count line {num_cpus})"


def waterfall_populated(resp):
    """executions: completed executions with query text, at least one slow
    enough to be interesting."""
    rows = (resp or {}).get("rows") or []
    durations = [r.get("duration_ms") for r in rows
                 if r.get("duration_ms") is not None and not r.get("in_progress")]
    slow = max(durations) if durations else None
    ok = len(rows) >= 1 and slow is not None and slow >= WATERFALL_INTERESTING_MS
    return ok, (f"executions={len(rows)}, slowest completed_ms={slow} "
                f"(floor {WATERFALL_INTERESTING_MS})")


def scatter_populated(resp):
    """exec_scatter: enough completed executions to form a visible
    distribution (both a count floor and a spread floor)."""
    points = (resp or {}).get("points") or []
    durations = sorted(p.get("duration_ms") for p in points
                        if p.get("duration_ms") not in (None, 0))
    if len(durations) < SCATTER_MIN_POINTS:
        return False, f"completed points={len(durations)} (floor {SCATTER_MIN_POINTS})"
    spread = durations[-1] / durations[0]
    ok = spread >= SCATTER_MIN_SPREAD_RATIO
    return ok, (f"completed points={len(durations)}, "
                f"max/min duration ratio={spread:.1f} (floor {SCATTER_MIN_SPREAD_RATIO})")


# ── tab -> (pgwt-server cmd, extra request params, pure checker) ──────────

TAB_QUERIES = {
    "overview":     ("time_model", {}, overview_populated),
    "events":       ("top_events", {}, events_populated),
    "sessions":     ("top_sessions", {}, sessions_populated),
    "queries":      ("top_queries", {}, queries_populated),
    "histogram":    ("heatmap", {"buckets": 200}, histogram_populated),
    "timeline":     ("session_timeline", {}, timeline_populated),
    "transitions":  ("transitions", {}, transitions_populated),
    "concurrency":  ("concurrency", {}, concurrency_populated),
    "waterfall":    ("executions", {}, waterfall_populated),
    "scatter":      ("exec_scatter", {"max_points": 2000}, scatter_populated),
    "matrix":       ("transitions", {"buckets": 200}, matrix_populated),
}

# Same 11, same order, as ui_live_smoke_lib.TABS -- kept as a literal tuple
# here (not imported) so this module has zero import-time dependency on the
# live-smoke driver; test_demo_workload_coverage.py separately asserts the
# two lists agree, so a drift is still caught, just not via a shared import
# that would make this module unusable on its own.
TAB_ORDER = (
    "overview", "events", "sessions", "queries", "histogram", "timeline",
    "transitions", "concurrency", "waterfall", "scatter", "matrix",
)


def run_coverage(srv, from_ns, to_ns, num_cpus=None):
    """Query every tab's endpoint over [from_ns, to_ns] and apply its pure
    checker. Returns {tab: {"ok": bool, "detail": str}}. A query that
    itself errors or times out is recorded as a FAILING tab (never
    silently skipped) -- a gate that cannot see must refuse, not approve.

    num_cpus: the capture box's CPU count (`info` response's `num_cpus`,
    the same field the UI's "N CPUs" chip reads) -- forwarded only to the
    concurrency checker, which requires it and fails loudly without it
    rather than grading against a weaker proxy."""
    results = {}
    for tab in TAB_ORDER:
        cmd, extra, checker = TAB_QUERIES[tab]
        try:
            resp = srv.query(cmd, from_=from_ns, to_=to_ns,
                              timeout=30, **extra)
        except Exception as e:
            results[tab] = {"ok": False, "detail": f"{cmd} query failed: {e!r}"}
            continue
        if isinstance(resp, dict) and resp.get("error"):
            results[tab] = {"ok": False, "detail": f"{cmd} error: {resp['error']!r}"}
            continue
        if tab == "concurrency":
            ok, detail = checker(resp, num_cpus)
        else:
            ok, detail = checker(resp)
        results[tab] = {"ok": bool(ok), "detail": detail}
    return results


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--trace-dir", required=True)
    ap.add_argument("--window-s", type=float, default=None,
                     help="restrict to the trailing N seconds of the "
                          "capture (default: the whole capture)")
    ap.add_argument("--out-json", default=None,
                     help="write the full per-tab results (plus from_ns/"
                          "to_ns/num_cpus) as JSON to this path, so a run's "
                          "numbers stay auditable under tests/results/ "
                          "instead of living only in prose (issue #214 "
                          "review)")
    args = ap.parse_args()

    from server_harness import ServerHarness

    with ServerHarness(args.trace_dir) as srv:
        info = srv.query("info", timeout=30)
        from_ns = int(info.get("from_ns", 0))
        to_ns = int(info.get("to_ns", 0))
        num_cpus = info.get("num_cpus")
        if args.window_s is not None:
            from_ns = max(from_ns, to_ns - int(args.window_s * 1_000_000_000))
        results = run_coverage(srv, from_ns, to_ns, num_cpus=num_cpus)

    failed = [t for t in TAB_ORDER if not results[t]["ok"]]
    for tab in TAB_ORDER:
        r = results[tab]
        status = "PASS" if r["ok"] else "FAIL"
        print(f"demo_workload_coverage: {tab}: {status} -- {r['detail']}")

    if args.out_json:
        import json
        os.makedirs(os.path.dirname(os.path.abspath(args.out_json)) or ".",
                    exist_ok=True)
        with open(args.out_json, "w") as f:
            json.dump({
                "trace_dir": args.trace_dir,
                "from_ns": from_ns, "to_ns": to_ns, "num_cpus": num_cpus,
                "window_s": args.window_s,
                "results": results,
                "failed": failed,
                "ok": not failed,
            }, f, indent=2, sort_keys=True)
            f.write("\n")
        print(f"demo_workload_coverage: wrote {args.out_json}")

    if failed:
        print(f"demo_workload_coverage: FAIL -- {len(failed)}/{len(TAB_ORDER)} "
              f"tab(s) not populated: {failed}")
        return 1
    print(f"demo_workload_coverage: PASS -- all {len(TAB_ORDER)} tabs populated")
    return 0


if __name__ == "__main__":
    sys.exit(main())
