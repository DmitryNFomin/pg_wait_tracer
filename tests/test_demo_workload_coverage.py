#!/usr/bin/env python3
"""test_demo_workload_coverage.py -- unit tests for
tests/demo_workload_coverage.py (issue #214). Pure Python, no browser, no
network, no subprocess, no PG -- runs on the Mac
(python3 tests/test_demo_workload_coverage.py).

Every *_populated() checker gets both a PASS case (the shape a real capture
from the extended tests/live_loop_workload.py produces) and a FAIL case (the
shape the ORIGINAL two-wait-class workload produced before issue #214 --
this is the demonstrated "red": these are exactly the inputs that made
Waterfall/Scatter empty on the 2026-09-28 Mac walk this issue is about).

This file is in tests/unit_tests.list, so it runs in CI's `build-and-unit`
job, which has NO Playwright installed -- unlike this Mac (`make check`) or
the gate box (`make box-check`), both of which have it, and so cannot catch
a module-scope Playwright import here (issue #205's own finding, applied
here as a standing precaution: `demo_workload_coverage.py` imports
`server_harness` lazily, inside `main()`, never at module scope, and
`ui_live_smoke_lib` only needs numpy/PIL). `_PLAYWRIGHT_ENTERED_ON_IMPORT`
below is sampled immediately before/after those two imports, with nothing
in between, so a future change that pulls Playwright in at module scope
fails loudly here -- on the machines that already have it, which are
exactly the ones a CI-only failure would otherwise hide from.

Usage: python3 tests/test_demo_workload_coverage.py
"""
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
_playwright_before_import = "playwright" in sys.modules
import demo_workload_coverage as cov
import ui_live_smoke_lib as ui_lib
_PLAYWRIGHT_ENTERED_ON_IMPORT = (
    not _playwright_before_import and "playwright" in sys.modules)

tests_run = 0
tests_passed = 0
tests_failed = 0


def check(cond, msg):
    global tests_run, tests_passed, tests_failed
    tests_run += 1
    if cond:
        tests_passed += 1
        print(f"  PASS: {msg}")
    else:
        tests_failed += 1
        print(f"  FAIL: {msg}")


# ── import must need only the stdlib (+ numpy/PIL via ui_live_smoke_lib) ──

def test_import_needs_no_playwright():
    """RED (issue #205's own finding, on a sibling file): a module-scope
    Playwright import here would be invisible to `make check` and
    `make box-check` (both have Playwright) and only fail CI's
    `build-and-unit` job -- exactly the gap that cost #220 a red CI round.
    This can only ever pass on a machine that HAS Playwright (this Mac, the
    gate box); on a Playwright-free machine there is nothing to catch, which
    is why it is not the only line of defense -- verified separately in a
    real Playwright-free venv (tests/results/, this branch's report)."""
    check(not _PLAYWRIGHT_ENTERED_ON_IMPORT,
          "importing demo_workload_coverage/ui_live_smoke_lib stays out of "
          "playwright (module-scope import reachable from "
          "tests/unit_tests.list must never pull it in)")


# ── TAB_ORDER agrees with the live-smoke driver's own tab list ────────────

def test_tab_order_matches_ui_live_smoke_lib():
    check(list(cov.TAB_ORDER) == list(ui_lib.TABS),
          "TAB_ORDER matches ui_live_smoke_lib.TABS exactly (same 11, same order)")
    check(set(cov.TAB_QUERIES.keys()) == set(cov.TAB_ORDER),
          "TAB_QUERIES has exactly one entry per tab in TAB_ORDER")


def test_waterfall_query_requests_slowest_first_not_recency():
    """#222 review: on a real 40-minute capture this check queried
    `executions` with no `sort` -- the default recency (start_desc) slice,
    the exact 100-row-of-the-last-~0.8s tail #222 exists to fix -- and
    reported the rehearsal's "slowest completed execution" as 0.326ms
    against the 500ms floor while the window's own genuinely slow
    executions (pg_sleep(1.3)/pg_sleep(0.4)/a 3M-row count(*),
    tests/live_loop_workload.py) sat outside that tail the whole time.
    TAB_QUERIES["waterfall"] must ask for the SAME slice the tab itself
    defaults to (web/static/views/waterfall.js's own sortMode default,
    EXECUTIONS_SORT_DEFAULT) -- not just any slice that happens to be
    "sorted somehow"."""
    cmd, extra, _fn = cov.TAB_QUERIES["waterfall"]
    check(cmd == "executions",
          "waterfall coverage still queries the executions endpoint")
    check(extra.get("sort") == "duration_desc",
          f"waterfall coverage requests sort=duration_desc, the same "
          f"slice the tab itself defaults to (extra={extra})")


# ── overview_populated ─────────────────────────────────────────────────────

def test_overview_populated_two_classes_passes():
    resp = {"rows": [
        {"name": "DB Time", "indent": 0, "ms": 800.0},
        {"name": "Lock", "indent": 1, "ms": 500.0},
        {"name": "Timeout", "indent": 1, "ms": 300.0},
        {"name": "Lock:relation", "indent": 2, "ms": 500.0},
    ]}
    ok, detail = cov.overview_populated(resp)
    check(ok, f"overview: two class-level (indent 1) rows with ms>0 passes ({detail})")


def test_overview_populated_one_class_fails():
    """RED: only Lock:relation/Timeout:PgSleep's own single class present
    (e.g. Timeout ms==0 this window) -- the input that made this checker
    exist."""
    resp = {"rows": [
        {"name": "DB Time", "indent": 0, "ms": 500.0},
        {"name": "Lock", "indent": 1, "ms": 500.0},
        {"name": "Timeout", "indent": 1, "ms": 0.0},
    ]}
    ok, detail = cov.overview_populated(resp)
    check(not ok, f"overview: one nonzero class-level row FAILS ({detail})")


def test_overview_populated_only_grand_total_row_fails():
    """RED (found on a real box-check capture, issue #214, 2026-09-28): a
    time_model response's indent==0 row is the single "DB Time" grand
    total, never a wait class -- a checker that filtered indent==0 saw
    exactly this row and could NEVER pass, on any capture. This is the
    input that caught it: two indent==0-only rows, real nonzero ms, must
    still fail because neither is a class."""
    resp = {"rows": [{"name": "DB Time", "indent": 0, "ms": 900000.0}]}
    ok, detail = cov.overview_populated(resp)
    check(not ok, f"overview: only the indent==0 grand-total row FAILS, "
                   f"is not mistaken for a class ({detail})")


def test_overview_populated_empty_fails():
    ok, detail = cov.overview_populated({"rows": []})
    check(not ok, f"overview: no rows at all FAILS ({detail})")
    ok, detail = cov.overview_populated(None)
    check(not ok, f"overview: None response FAILS, does not crash ({detail})")


# ── events_populated ────────────────────────────────────────────────────────

def test_events_populated_two_classes_passes():
    resp = {"rows": [
        {"name": "Lock:relation", "class": "Lock", "total_ms": 100.0},
        {"name": "Timeout:PgSleep", "class": "Timeout", "total_ms": 200.0},
    ]}
    ok, detail = cov.events_populated(resp)
    check(ok, f"events: two classes passes ({detail})")


def test_events_populated_one_class_fails():
    resp = {"rows": [{"name": "Lock:relation", "class": "Lock", "total_ms": 100.0}]}
    ok, detail = cov.events_populated(resp)
    check(not ok, f"events: one class FAILS ({detail})")


def test_events_populated_ms_field_not_total_ms_fails():
    """RED (found on a real box-check capture, issue #214, 2026-09-28): a
    top_events row's field is "total_ms" -- "ms" is time_model's name for
    the same quantity on a DIFFERENT endpoint. A checker reading "ms" here
    always reads 0/missing and can never pass, on any capture."""
    resp = {"rows": [
        {"name": "Lock:relation", "class": "Lock", "ms": 100.0},
        {"name": "Timeout:PgSleep", "class": "Timeout", "ms": 200.0},
    ]}
    ok, detail = cov.events_populated(resp)
    check(not ok, f"events: rows carrying only 'ms' (wrong field) FAIL, "
                   f"not silently treated as zero-and-fine ({detail})")


# ── sessions_populated ──────────────────────────────────────────────────────

def test_sessions_populated_several_passes():
    ok, detail = cov.sessions_populated({"rows": [{"pid": 1}, {"pid": 2}]})
    check(ok, f"sessions: 2 rows passes ({detail})")


def test_sessions_populated_single_fails():
    """RED: a single backend visible (e.g. only the sleeper mid-wait) --
    'several concurrent backends' is what the tab needs to look alive."""
    ok, detail = cov.sessions_populated({"rows": [{"pid": 1}]})
    check(not ok, f"sessions: 1 row FAILS ({detail})")


# ── queries_populated ────────────────────────────────────────────────────────

def test_queries_populated_distinct_passes():
    resp = {"rows": [
        {"query_id": "1", "text": "SELECT pg_sleep(3)", "total_ms": 3000.0},
        {"query_id": "2", "text": "SELECT count(*) FROM pg_class", "total_ms": 5.0},
    ]}
    ok, detail = cov.queries_populated(resp)
    check(ok, f"queries: two distinct texts/totals passes ({detail})")


def test_queries_populated_single_statement_fails():
    """RED: the original workload's holder/waiter/sleeper trio alone
    produces exactly one visibly distinct statement shape."""
    resp = {"rows": [{"query_id": "1", "text": "SELECT pg_sleep(3)", "total_ms": 3000.0}]}
    ok, detail = cov.queries_populated(resp)
    check(not ok, f"queries: one statement FAILS ({detail})")


# ── histogram_populated ─────────────────────────────────────────────────────

def test_histogram_populated_spread_passes():
    resp = {"cells": [[0, 2, 5], [0, 9, 1]]}   # two distinct latency bands
    ok, detail = cov.histogram_populated(resp)
    check(ok, f"histogram: two nonzero bands passes ({detail})")


def test_histogram_populated_one_mode_fails():
    """RED: every event lands in the same latency band -- 'one mode', not a
    spread, exactly the case issue #214 calls out."""
    resp = {"cells": [[0, 2, 5], [1, 2, 3], [2, 2, 9]]}   # all band index 2
    ok, detail = cov.histogram_populated(resp)
    check(not ok, f"histogram: one band only FAILS ({detail})")


def test_histogram_populated_empty_fails():
    ok, detail = cov.histogram_populated({"cells": []})
    check(not ok, f"histogram: no cells FAILS ({detail})")


# ── timeline_populated ───────────────────────────────────────────────────────

def test_timeline_populated_changing_pid_passes():
    resp = {"events": [
        {"p": 100, "n": "Lock:relation"}, {"p": 100, "n": "CPU"},
        {"p": 200, "n": "CPU"},
    ]}
    ok, detail = cov.timeline_populated(resp)
    check(ok, f"timeline: pid 100 has 2 distinct wait names, passes ({detail})")


def test_timeline_populated_static_pids_fail():
    """RED: every pid shows exactly one wait name for the whole window --
    'a backend whose waits change over time' is what this tab needs."""
    resp = {"events": [{"p": 100, "n": "CPU"}, {"p": 200, "n": "CPU"}]}
    ok, detail = cov.timeline_populated(resp)
    check(not ok, f"timeline: no pid changes wait FAILS ({detail})")


def test_timeline_populated_empty_fails():
    ok, detail = cov.timeline_populated({"events": []})
    check(not ok, f"timeline: no events FAILS ({detail})")


# ── transitions_populated / matrix_populated ────────────────────────────────

def test_transitions_populated_real_transition_passes():
    resp = {"links": [
        {"source": "CPU*", "target": "Lock:relation", "value": 10},
        {"source": "Client:ClientRead", "target": "CPU*", "value": 500},
    ]}
    ok, detail = cov.transitions_populated(resp)
    check(ok, f"transitions: one non-idle transition passes ({detail})")


def test_transitions_populated_idle_only_fails():
    """RED: only the ever-present Client:ClientRead<->CPU* idle loop --
    the exact 'a tab that is non-empty but shows nothing workload-driven'
    trap this checker exists to catch."""
    resp = {"links": [
        {"source": "Client:ClientRead", "target": "CPU*", "value": 500},
        {"source": "CPU*", "target": "Client:ClientRead", "value": 480},
    ]}
    ok, detail = cov.transitions_populated(resp)
    check(not ok, f"transitions: idle-only links FAIL ({detail})")


def test_matrix_populated_two_pairs_passes():
    resp = {"links": [
        {"source": "CPU*", "target": "Lock:relation", "value": 10},
        {"source": "CPU*", "target": "Timeout:PgSleep", "value": 8},
    ]}
    ok, detail = cov.matrix_populated(resp)
    check(ok, f"matrix: two non-idle pairs passes ({detail})")


def test_matrix_populated_one_pair_fails():
    resp = {"links": [{"source": "CPU*", "target": "Lock:relation", "value": 10}]}
    ok, detail = cov.matrix_populated(resp)
    check(not ok, f"matrix: one pair FAILS ({detail})")


# ── concurrency_populated ────────────────────────────────────────────────────

def test_concurrency_populated_peak_at_or_above_cpu_line_passes():
    resp = {"peaks": [{"t": 1, "max": 2.5}, {"t": 2, "max": 4.2}]}
    ok, detail = cov.concurrency_populated(resp, num_cpus=4)
    check(ok, f"concurrency: max AAS 4.2 >= num_cpus 4 passes ({detail})")


def test_concurrency_populated_below_cpu_line_fails():
    """RED: ordinary background concurrency (AAS 2.1) that would have
    passed the old fixed-1.5 proxy but never actually saturates a 4-CPU
    box -- exactly the gap the review round asked to close."""
    resp = {"peaks": [{"t": 1, "max": 0.5}, {"t": 2, "max": 2.1}]}
    ok, detail = cov.concurrency_populated(resp, num_cpus=4)
    check(not ok, f"concurrency: max AAS 2.1 < num_cpus 4 FAILS "
                   f"(would have passed the old 1.5 proxy) ({detail})")


def test_concurrency_populated_no_peaks_fails():
    ok, detail = cov.concurrency_populated({"peaks": []}, num_cpus=4)
    check(not ok, f"concurrency: no peaks FAILS ({detail})")


def test_concurrency_populated_missing_num_cpus_refuses():
    """RED: num_cpus not supplied (e.g. an `info` response predating the
    field, or a caller that forgot to thread it through) must REFUSE, not
    silently grade against a weaker floor -- even with a peak that would
    have passed any fixed proxy."""
    resp = {"peaks": [{"t": 1, "max": 99.0}]}
    ok, detail = cov.concurrency_populated(resp, num_cpus=None)
    check(not ok, f"concurrency: missing num_cpus REFUSES even with a huge "
                   f"peak, never treated as fine ({detail})")


def test_concurrency_populated_invalid_num_cpus_refuses():
    for bad in (0, -1, "4", True):
        ok, detail = cov.concurrency_populated(
            {"peaks": [{"t": 1, "max": 99.0}]}, num_cpus=bad)
        check(not ok, f"concurrency: num_cpus={bad!r} REFUSES ({detail})")


# ── waterfall_populated ──────────────────────────────────────────────────────

def test_waterfall_populated_slow_execution_passes():
    resp = {"rows": [
        {"query_id": "1", "duration_ms": 12.0},
        {"query_id": "2", "duration_ms": 1300.0},
    ]}
    ok, detail = cov.waterfall_populated(resp)
    check(ok, f"waterfall: one slow completed execution passes ({detail})")


def test_waterfall_populated_no_slow_execution_fails():
    """RED: this issue's own reported symptom -- executions present but
    none slow enough to be worth pointing at (or none at all)."""
    resp = {"rows": [{"query_id": "1", "duration_ms": 5.0}]}
    ok, detail = cov.waterfall_populated(resp)
    check(not ok, f"waterfall: only fast executions FAIL ({detail})")


def test_waterfall_populated_empty_fails():
    ok, detail = cov.waterfall_populated({"rows": []})
    check(not ok, f"waterfall: no rows FAILS ({detail})")


def test_waterfall_populated_in_progress_excluded():
    """An in-progress execution has no settled duration -- must not count
    toward the 'slow enough' floor."""
    resp = {"rows": [{"query_id": "1", "duration_ms": 9999.0, "in_progress": True}]}
    ok, detail = cov.waterfall_populated(resp)
    check(not ok, f"waterfall: in-progress-only execution FAILS ({detail})")


# ── waterfall_floor_excludes_inferred (#222 review round 2, addition 2) ────

def test_waterfall_floor_excludes_inferred_measured_row_passes():
    resp = {"rows": [{"query_id": "1", "duration_ms": 700.0,
                       "end_inferred": False}]}
    ok, detail = cov.waterfall_floor_excludes_inferred(resp)
    check(ok, f"a measured (end_inferred=False) slow row passes ({detail})")


def test_waterfall_floor_excludes_inferred_only_inferred_row_fails():
    """RED: this IS the hole addition 2 closes -- a cancelled/errored
    statement (end_inferred=True) must not alone satisfy the floor even
    though it clears the 500ms threshold and waterfall_populated (which
    does not look at end_inferred) would pass it."""
    resp = {"rows": [{"query_id": "1", "duration_ms": 700.0,
                       "end_inferred": True}]}
    populated_ok, _ = cov.waterfall_populated(resp)
    check(populated_ok,
          "waterfall_populated ALONE would pass this inferred-only row "
          "(confirms addition 2 is the thing doing the excluding)")
    ok, detail = cov.waterfall_floor_excludes_inferred(resp)
    check(not ok, f"an inferred-only slow row FAILS the stricter floor ({detail})")


def test_waterfall_floor_excludes_inferred_mixed_passes_on_the_measured_one():
    resp = {"rows": [{"query_id": "1", "duration_ms": 700.0, "end_inferred": True},
                      {"query_id": "2", "duration_ms": 600.0, "end_inferred": False}]}
    ok, detail = cov.waterfall_floor_excludes_inferred(resp)
    check(ok, f"a genuinely measured slow row among inferred ones still "
              f"passes ({detail})")


def test_waterfall_floor_excludes_inferred_missing_key_fails_closed():
    """#222 review round 3, item 1. RED input: exactly this response --
    a 700ms completed row with NO `end_inferred` key, which is what a
    pgwt-server built before this branch returns. `not
    r.get("end_inferred")` reads the missing key as "measured" and
    PASSES, so the whole inferred-exclusion silently no-ops against an
    older server instead of refusing. The key must be present AND
    false."""
    resp = {"rows": [{"query_id": "1", "duration_ms": 700.0}]}
    populated_ok, _ = cov.waterfall_populated(resp)
    check(populated_ok,
          "waterfall_populated alone still passes this row (so the "
          "fail-closed behaviour is this function's, not inherited)")
    ok, detail = cov.waterfall_floor_excludes_inferred(resp)
    check(not ok,
          f"a slow row with NO end_inferred field does NOT satisfy the "
          f"floor -- version drift refuses instead of approving ({detail})")
    check("FAIL-CLOSED" in detail and "end_inferred" in detail,
          f"and the detail NAMES version drift, so a future red is not "
          f"mistaken for 'the capture had no slow query' ({detail})")


def test_waterfall_floor_excludes_inferred_missing_key_is_not_a_blanket_refusal():
    """Anti-bypass for the fix above: failing closed must not degrade into
    failing ALWAYS. A page that carries one properly-serialised measured
    row still passes even when some OTHER row lacks the field."""
    resp = {"rows": [{"query_id": "1", "duration_ms": 700.0},
                      {"query_id": "2", "duration_ms": 600.0,
                       "end_inferred": False}]}
    ok, detail = cov.waterfall_floor_excludes_inferred(resp)
    check(ok, f"one correctly-serialised measured slow row still passes "
              f"({detail})")
    check("FAIL-CLOSED" in detail,
          f"and the field-less row is still reported, not hidden by the "
          f"pass ({detail})")


def test_waterfall_floor_excludes_inferred_missing_key_only_counts_slow_rows():
    """The fail-closed notice must not fire on rows the floor never
    considered anyway -- a fast row with no end_inferred field is not
    version drift worth shouting about, and a notice that fires on every
    page is a notice nobody reads."""
    resp = {"rows": [{"query_id": "1", "duration_ms": 0.3},
                      {"query_id": "2", "duration_ms": 600.0,
                       "end_inferred": False}]}
    ok, detail = cov.waterfall_floor_excludes_inferred(resp)
    check(ok and "FAIL-CLOSED" not in detail,
          f"a sub-floor row without the field raises no notice ({detail})")


# ── waterfall_recency_diagnostic (diagnostic only, never gates) ───────────

def test_waterfall_recency_diagnostic_shape():
    resp = {"rows": [
        {"query_id": "1", "duration_ms": 10.0, "start_ns": "100"},
        {"query_id": "2", "duration_ms": 20.0, "start_ns": "200"},
        {"query_id": "3", "duration_ms": None, "in_progress": True,
         "start_ns": "300"},
    ]}
    diag = cov.waterfall_recency_diagnostic(resp)
    check(diag["rows"] == 3, f"row count recorded ({diag})")
    check(diag["completed"] == 2, f"in-progress row excluded from completed ({diag})")
    check(diag["max_completed_duration_ms"] == 20.0, f"max completed duration ({diag})")
    check(diag["span_ns"] == 200, f"wall-clock start span ({diag})")


def test_waterfall_recency_diagnostic_empty():
    diag = cov.waterfall_recency_diagnostic({"rows": []})
    check(diag == {"rows": 0, "completed": 0,
                    "max_completed_duration_ms": None, "span_ns": 0},
          f"empty recency slice reports zeros, not an exception ({diag})")


def test_run_coverage_recency_slice_never_gates_waterfall():
    """The adviser-corrected version of addition 1: a recency slice with
    NOTHING slow in it (the exact timing accident #222 removed from the
    gate) must NOT fail waterfall as long as the duration_desc slice
    clears the floor with a measured row -- proving the diagnostic really
    is diagnostic-only, not a second gate wearing a different name."""
    good = {
        "time_model": {"rows": [{"name": "Lock", "indent": 1, "ms": 5},
                                 {"name": "Timeout", "indent": 1, "ms": 5}]},
        "top_events": {"rows": [{"name": "Lock:relation", "class": "Lock", "total_ms": 5},
                                 {"name": "Timeout:PgSleep", "class": "Timeout", "total_ms": 5}]},
        "top_sessions": {"rows": [{"pid": 1}, {"pid": 2}]},
        "top_queries": {"rows": [
            {"text": "A", "total_ms": 1}, {"text": "B", "total_ms": 2}]},
        "heatmap": {"cells": [[0, 1, 5], [0, 2, 5]]},
        "session_timeline": {"events": [{"p": 1, "n": "A"}, {"p": 1, "n": "B"}]},
        "transitions": {"links": [{"source": "CPU*", "target": "Lock:relation", "value": 1},
                                   {"source": "CPU*", "target": "Timeout:PgSleep", "value": 1}]},
        "concurrency": {"peaks": [{"t": 1, "max": 5.0}]},
        "executions": {"rows": [{"query_id": "1", "duration_ms": 0.05,
                                  "end_inferred": False}]},
        "exec_scatter": {"points": [{"duration_ms": d} for d in [1, 2, 5, 50, 400]]},
    }
    srv = _FakeServer(good)
    results = cov.run_coverage(srv, 0, 1, num_cpus=4)
    check(results["waterfall"]["ok"] is False,
          f"waterfall still FAILS on its own merits (no row clears the "
          f"500ms floor) -- this scenario does not exercise the "
          f"diagnostic's non-gating behavior by accident "
          f"({results['waterfall']['detail']})")
    check(results["waterfall"]["recency_diagnostic"]["max_completed_duration_ms"] == 0.05,
          f"the diagnostic recorded the (unhelpfully fast) recency slice "
          f"regardless ({results['waterfall']['recency_diagnostic']})")


def test_run_coverage_waterfall_ok_survives_an_empty_recency_slice():
    """RED without addition 1's non-gating design: a recency slice with
    NOTHING slow in it (all sub-ms pgbench noise, the exact shape a real
    busy capture's last 100 starts actually has -- see waterfall_populated's
    own docstring) must not drag down a waterfall tab whose duration_desc
    slice genuinely clears the floor with a measured row. The gating and
    diagnostic queries are BOTH `executions` with the same kwargs shape
    except `sort`, so the fake server must actually distinguish them by
    kwargs (via a callable response) -- a dict-per-cmd fake could not tell
    the two queries apart and would make this test unable to fail for the
    right reason."""
    duration_desc_resp = {"rows": [{"query_id": "1", "duration_ms": 1300.0,
                                    "end_inferred": False}]}
    recency_resp = {"rows": [{"query_id": "2", "duration_ms": 0.3,
                              "start_ns": "1"},
                             {"query_id": "3", "duration_ms": 0.2,
                              "start_ns": "2"}]}

    def executions_responder(kwargs):
        return (duration_desc_resp if kwargs.get("sort") == "duration_desc"
                else recency_resp)

    good = {
        "time_model": {"rows": [{"name": "Lock", "indent": 1, "ms": 5},
                                 {"name": "Timeout", "indent": 1, "ms": 5}]},
        "top_events": {"rows": [{"name": "Lock:relation", "class": "Lock", "total_ms": 5},
                                 {"name": "Timeout:PgSleep", "class": "Timeout", "total_ms": 5}]},
        "top_sessions": {"rows": [{"pid": 1}, {"pid": 2}]},
        "top_queries": {"rows": [
            {"text": "A", "total_ms": 1}, {"text": "B", "total_ms": 2}]},
        "heatmap": {"cells": [[0, 1, 5], [0, 2, 5]]},
        "session_timeline": {"events": [{"p": 1, "n": "A"}, {"p": 1, "n": "B"}]},
        "transitions": {"links": [{"source": "CPU*", "target": "Lock:relation", "value": 1},
                                   {"source": "CPU*", "target": "Timeout:PgSleep", "value": 1}]},
        "concurrency": {"peaks": [{"t": 1, "max": 5.0}]},
        "executions": executions_responder,
        "exec_scatter": {"points": [{"duration_ms": d} for d in [1, 2, 5, 50, 400]]},
    }
    srv = _FakeServer(good)
    results = cov.run_coverage(srv, 0, 1, num_cpus=4)
    check(results["waterfall"]["ok"] is True,
          f"waterfall PASSES on the duration_desc slice's real measured "
          f"slow row, even though the recency slice has nothing slow at "
          f"all ({results['waterfall']['detail']})")
    diag = results["waterfall"]["recency_diagnostic"]
    check(diag["max_completed_duration_ms"] == 0.3,
          f"the diagnostic still faithfully recorded the (unhelpful) "
          f"recency slice's own data ({diag})")


# ── scatter_populated ────────────────────────────────────────────────────────

def test_scatter_populated_spread_passes():
    resp = {"points": [{"duration_ms": d} for d in [1.0, 2.0, 5.0, 50.0, 400.0]]}
    ok, detail = cov.scatter_populated(resp)
    check(ok, f"scatter: 5 points with wide spread passes ({detail})")


def test_scatter_populated_too_few_points_fails():
    """RED: this issue's own reported symptom -- Scatter needs 'enough
    completed executions to form a visible distribution'."""
    resp = {"points": [{"duration_ms": d} for d in [1.0, 2.0]]}
    ok, detail = cov.scatter_populated(resp)
    check(not ok, f"scatter: only 2 points FAILS ({detail})")


def test_scatter_populated_no_spread_fails():
    """RED: enough points, but all near-identical duration -- not a
    'distribution', a single dot repeated."""
    resp = {"points": [{"duration_ms": d} for d in [10.0, 10.1, 9.9, 10.2, 10.0]]}
    ok, detail = cov.scatter_populated(resp)
    check(not ok, f"scatter: no spread among 5 points FAILS ({detail})")


def test_scatter_populated_zero_duration_excluded():
    resp = {"points": [{"duration_ms": 0}, {"duration_ms": None},
                        {"duration_ms": 1.0}, {"duration_ms": 2.0}]}
    ok, detail = cov.scatter_populated(resp)
    check(not ok, f"scatter: zero/None durations excluded from the count ({detail})")


# ── run_coverage glue: absent/broken query never silently skips a tab ──────

class _FakeServer:
    """A tiny stand-in for server_harness.ServerHarness good enough for
    run_coverage: records the cmd (and, since #222 review, the full kwargs)
    it was asked and returns a scripted response or raises. A response may
    also be a callable(kwargs) -> resp (#222 review round 2) -- needed to
    give the gating duration_desc `executions` query and the diagnostic
    recency `executions` query (same cmd, different kwargs) genuinely
    different scripted responses, which a single dict-per-cmd cannot."""
    def __init__(self, responses):
        self.responses = responses   # cmd -> resp, Exception, or callable
        self.calls = []              # [(cmd, kwargs), ...] in call order

    def query(self, cmd, **kwargs):
        self.calls.append((cmd, kwargs))
        r = self.responses.get(cmd)
        if callable(r):
            r = r(kwargs)
        if isinstance(r, Exception):
            raise r
        return r


def test_run_coverage_query_exception_fails_that_tab_not_skips():
    """A dependency exiting mid-query (crash, timeout, pipe close) must
    fail that tab, never be silently treated as 'not applicable' -- a gate
    that cannot see must refuse. Every OTHER tab's response is a clean
    PASS shape so only the broken one can plausibly fail."""
    good = {
        "time_model": {"rows": [{"name": "Lock", "indent": 1, "ms": 5},
                                 {"name": "Timeout", "indent": 1, "ms": 5}]},
        "top_events": {"rows": [{"name": "Lock:relation", "class": "Lock", "total_ms": 5},
                                 {"name": "Timeout:PgSleep", "class": "Timeout", "total_ms": 5}]},
        "top_sessions": {"rows": [{"pid": 1}, {"pid": 2}]},
        "top_queries": {"rows": [
            {"text": "A", "total_ms": 1}, {"text": "B", "total_ms": 2}]},
        "heatmap": {"cells": [[0, 1, 5], [0, 2, 5]]},
        "session_timeline": {"events": [{"p": 1, "n": "A"}, {"p": 1, "n": "B"}]},
        "transitions": {"links": [{"source": "CPU*", "target": "Lock:relation", "value": 1},
                                   {"source": "CPU*", "target": "Timeout:PgSleep", "value": 1}]},
        "concurrency": {"peaks": [{"t": 1, "max": 5.0}]},
        "exec_scatter": {"points": [{"duration_ms": d} for d in [1, 2, 5, 50, 400]]},
    }
    responses = dict(good)
    responses["executions"] = RuntimeError("pgwt-server pipe closed")
    srv = _FakeServer(responses)
    results = cov.run_coverage(srv, 0, 1, num_cpus=4)
    check(results["waterfall"]["ok"] is False,
          f"waterfall FAILS (not skipped) when its query raises ({results['waterfall']['detail']})")
    others_ok = all(results[t]["ok"] for t in cov.TAB_ORDER if t != "waterfall")
    check(others_ok, "every other tab still evaluated normally from its own response")


def test_run_coverage_missing_num_cpus_fails_concurrency_not_skips():
    """The num_cpus REFUSAL must reach through run_coverage's glue, not
    just the pure function in isolation -- a caller of run_coverage() that
    omits num_cpus (the default) must see concurrency FAIL, even with a
    concurrency response that would otherwise pass easily."""
    good = {
        "time_model": {"rows": [{"name": "Lock", "indent": 1, "ms": 5},
                                 {"name": "Timeout", "indent": 1, "ms": 5}]},
        "top_events": {"rows": [{"name": "Lock:relation", "class": "Lock", "total_ms": 5},
                                 {"name": "Timeout:PgSleep", "class": "Timeout", "total_ms": 5}]},
        "top_sessions": {"rows": [{"pid": 1}, {"pid": 2}]},
        "top_queries": {"rows": [
            {"text": "A", "total_ms": 1}, {"text": "B", "total_ms": 2}]},
        "heatmap": {"cells": [[0, 1, 5], [0, 2, 5]]},
        "session_timeline": {"events": [{"p": 1, "n": "A"}, {"p": 1, "n": "B"}]},
        "transitions": {"links": [{"source": "CPU*", "target": "Lock:relation", "value": 1},
                                   {"source": "CPU*", "target": "Timeout:PgSleep", "value": 1}]},
        "concurrency": {"peaks": [{"t": 1, "max": 99.0}]},
        "executions": {"rows": [{"duration_ms": 1300.0}]},
        "exec_scatter": {"points": [{"duration_ms": d} for d in [1, 2, 5, 50, 400]]},
    }
    srv = _FakeServer(good)
    results = cov.run_coverage(srv, 0, 1)   # num_cpus omitted -> None
    check(results["concurrency"]["ok"] is False,
          f"concurrency FAILS when run_coverage's caller omits num_cpus, "
          f"even with a huge peak ({results['concurrency']['detail']})")


def test_run_coverage_waterfall_forwards_sort_and_sees_the_window_not_the_tail():
    """#222 review, end to end through run_coverage's real glue (not just
    the TAB_QUERIES table in isolation): the response below is the shape a
    sort=duration_desc page actually looks like on a real capture -- the
    genuinely slow execution present, NOT crowded out by a 0.8s recency
    tail of pgbench noise. RED without the fix: revert
    TAB_QUERIES["waterfall"]'s extra to {} and this still passes today
    (the response is scripted to already contain the slow row), but the
    kwargs assertion below catches the regression the response shape
    alone cannot -- it fails the moment `sort` stops being forwarded to
    srv.query, independent of what any particular response happens to
    contain."""
    good = {
        "time_model": {"rows": [{"name": "Lock", "indent": 1, "ms": 5},
                                 {"name": "Timeout", "indent": 1, "ms": 5}]},
        "top_events": {"rows": [{"name": "Lock:relation", "class": "Lock", "total_ms": 5},
                                 {"name": "Timeout:PgSleep", "class": "Timeout", "total_ms": 5}]},
        "top_sessions": {"rows": [{"pid": 1}, {"pid": 2}]},
        "top_queries": {"rows": [
            {"text": "A", "total_ms": 1}, {"text": "B", "total_ms": 2}]},
        "heatmap": {"cells": [[0, 1, 5], [0, 2, 5]]},
        "session_timeline": {"events": [{"p": 1, "n": "A"}, {"p": 1, "n": "B"}]},
        "transitions": {"links": [{"source": "CPU*", "target": "Lock:relation", "value": 1},
                                   {"source": "CPU*", "target": "Timeout:PgSleep", "value": 1}]},
        "concurrency": {"peaks": [{"t": 1, "max": 5.0}]},
        # The duration_desc page: the reporter session's slow query leads,
        # the pgbench tail trails -- exactly inverted from the recency
        # page that made the real rehearsal see only 0.326ms.
        # end_inferred present on every row: that is what every
        # pgwt-server from #222 on actually serialises
        # (serialize_execution_row), and waterfall_floor_excludes_inferred
        # now FAILS CLOSED on its absence, so a fixture without it would
        # be a shape no real server produces.
        "executions": {"rows": [
            {"query_id": "1", "duration_ms": 1300.0, "end_inferred": False},
            {"query_id": "2", "duration_ms": 0.326, "end_inferred": False},
        ]},
        "exec_scatter": {"points": [{"duration_ms": d} for d in [1, 2, 5, 50, 400]]},
    }
    srv = _FakeServer(good)
    results = cov.run_coverage(srv, 0, 1, num_cpus=4)
    check(results["waterfall"]["ok"] is True,
          f"waterfall PASSES against the duration-sorted page "
          f"({results['waterfall']['detail']})")
    # #222 review round 2: run_coverage now issues TWO executions queries
    # for waterfall -- the gating duration_desc slice and a diagnostic-only
    # recency slice (waterfall_recency_diagnostic) -- exactly one of each.
    exec_calls = [kwargs for cmd, kwargs in srv.calls if cmd == "executions"]
    check(len(exec_calls) == 2,
          f"run_coverage issues exactly two executions queries for "
          f"waterfall: the gating slice and the diagnostic recency slice "
          f"(calls={exec_calls})")
    sort_calls = [k for k in exec_calls if k.get("sort") == "duration_desc"]
    recency_calls = [k for k in exec_calls if "sort" not in k]
    check(len(sort_calls) == 1,
          f"exactly one call carries sort=duration_desc, not just the "
          f"TAB_QUERIES table in the abstract (calls={exec_calls})")
    check(len(recency_calls) == 1,
          f"exactly one call carries no sort at all -- the recency slice "
          f"(calls={exec_calls})")
    check("recency_diagnostic" in results["waterfall"],
          f"the recency slice's diagnostic is recorded on the result "
          f"({results['waterfall'].keys()})")


def test_run_coverage_server_error_field_fails_not_skips():
    responses = {cmd: {"error": "boom"} for cmd, _extra, _fn in cov.TAB_QUERIES.values()}
    # give every OTHER cmd its own distinct error too -- rebuild per-cmd map
    responses = {}
    for tab in cov.TAB_ORDER:
        cmd, _extra, _fn = cov.TAB_QUERIES[tab]
        responses[cmd] = {"error": "boom"} if tab == "overview" else None
    srv = _FakeServer(responses)
    results = cov.run_coverage(srv, 0, 1)
    check(results["overview"]["ok"] is False,
          f"a response carrying an 'error' field FAILS ({results['overview']['detail']})")


# ── window_too_large / allocation_failed: a distinct "could not evaluate"
# outcome, never conflated with "tab genuinely empty" (#214 round 3) ───────

def test_run_coverage_window_too_large_is_could_not_evaluate_not_plain_fail():
    """RED: this is the exact review finding -- pgwt-server's real
    window_too_large response shape (src/server.c reject_overload), on a
    demo-length capture. Before this fix, run_coverage recorded it
    identically to an empty tab; a human reading the verdict could not
    tell the difference."""
    responses = {}
    for tab in cov.TAB_ORDER:
        cmd, _extra, _fn = cov.TAB_QUERIES[tab]
        responses[cmd] = (
            {"error": "window too large", "code": "window_too_large",
             "max_events": 100000, "hint": "narrow the time range or add a "
             "pid/query filter"}
            if tab == "waterfall" else None)
    srv = _FakeServer(responses)
    results = cov.run_coverage(srv, 0, 1)
    r = results["waterfall"]
    check(r["ok"] is False, f"window_too_large still FAILS the tab ({r['detail']})")
    check(r["could_not_evaluate"] is True,
          f"window_too_large is flagged could_not_evaluate, not a plain "
          f"empty-tab FAIL ({r['detail']})")
    check("window_too_large" in r["detail"] or "COULD NOT EVALUATE" in r["detail"],
          f"the detail string names it distinctly ({r['detail']})")


def test_run_coverage_allocation_failed_is_also_could_not_evaluate():
    responses = {}
    for tab in cov.TAB_ORDER:
        cmd, _extra, _fn = cov.TAB_QUERIES[tab]
        responses[cmd] = (
            {"error": "memory allocation failed", "code": "allocation_failed",
             "hint": "retry the request or reduce the time range"}
            if tab == "scatter" else None)
    srv = _FakeServer(responses)
    results = cov.run_coverage(srv, 0, 1)
    r = results["scatter"]
    check(r["ok"] is False, f"allocation_failed still FAILS the tab ({r['detail']})")
    check(r["could_not_evaluate"] is True,
          f"allocation_failed is also flagged could_not_evaluate ({r['detail']})")


def test_run_coverage_ordinary_error_is_not_could_not_evaluate():
    """RED: an ordinary error (no code, or a code outside
    COULD_NOT_EVALUATE_CODES) must NOT be mislabeled could_not_evaluate --
    that would let a genuinely broken query masquerade as 'not my fault,
    just a capacity limit'."""
    responses = {}
    for tab in cov.TAB_ORDER:
        cmd, _extra, _fn = cov.TAB_QUERIES[tab]
        responses[cmd] = {"error": "boom", "code": "invalid_request"} if tab == "events" else None
    srv = _FakeServer(responses)
    results = cov.run_coverage(srv, 0, 1)
    r = results["events"]
    check(r["ok"] is False, f"an ordinary error still FAILS ({r['detail']})")
    check(r["could_not_evaluate"] is False,
          f"an ordinary error is NOT mislabeled could_not_evaluate ({r['detail']})")


def test_run_coverage_exception_is_not_could_not_evaluate():
    """A raised exception (pipe closed, timeout) is a different failure
    mode from a structured server refusal -- not could_not_evaluate
    either, so the two are never conflated."""
    responses = dict.fromkeys(
        (cmd for cmd, _e, _f in cov.TAB_QUERIES.values()), None)
    responses[cov.TAB_QUERIES["waterfall"][0]] = RuntimeError("pipe closed")
    srv = _FakeServer(responses)
    results = cov.run_coverage(srv, 0, 1)
    r = results["waterfall"]
    check(r["ok"] is False, f"an exception still FAILS ({r['detail']})")
    check(r["could_not_evaluate"] is False,
          f"a raised exception is NOT mislabeled could_not_evaluate ({r['detail']})")


def test_run_coverage_normal_pass_has_could_not_evaluate_false():
    responses = {
        "time_model": {"rows": [{"name": "Lock", "indent": 1, "ms": 5},
                                 {"name": "Timeout", "indent": 1, "ms": 5}]},
    }
    for tab in cov.TAB_ORDER:
        cmd, _e, _f = cov.TAB_QUERIES[tab]
        responses.setdefault(cmd, None)
    srv = _FakeServer(responses)
    results = cov.run_coverage(srv, 0, 1)
    check(results["overview"]["could_not_evaluate"] is False,
          "a normal (non-error) response is could_not_evaluate=False, "
          "whether it passes or fails its own checker")


# ── #222 review item 4: the returned PAGE, and the retained artifact ───────
#
# Bypass suite. The true-positive path (a page with no slow row fails) is
# already covered above; every test below is about a way the check or its
# evidence could stop being able to SEE -- which is the direction that
# actually goes wrong: a verdict derived from a number nobody kept, an
# artifact silently absent, or an artifact left over from another run.

def _retention_server(duration_page, recency_page=None, exc=None):
    """A _FakeServer whose `executions` cmd answers the gating
    duration_desc query and the diagnostic recency query differently (or
    raises), and answers every other tab with None (those tabs then fail
    their own checkers, which is irrelevant here)."""
    def executions(kwargs):
        if exc is not None:
            raise exc
        if kwargs.get("sort") == "duration_desc":
            return duration_page
        return recency_page
    responses = {"executions": executions}
    for cmd, _e, _f in cov.TAB_QUERIES.values():
        responses.setdefault(cmd, None)
    return _FakeServer(responses)


def _read_raw(raw_dir, name):
    import json as _json
    with open(os.path.join(raw_dir, name + ".json")) as f:
        return _json.load(f)


def _tmpdir():
    import tempfile
    return tempfile.mkdtemp(prefix="pgwt_cov_raw_")


SLOW_PAGE = {"rows": [
    {"query_id": "slow", "duration_ms": 1300.0, "in_progress": False,
     "end_inferred": False},
    {"query_id": "fast", "duration_ms": 0.326, "in_progress": False,
     "end_inferred": False},
], "total_count": 2, "open_count": 0, "completed_count": 2}


def test_waterfall_floor_reads_the_returned_page_not_a_summary_field():
    """#222 review item 4: the 500ms floor must be satisfied by a row that
    is ACTUALLY ON THE RETURNED PAGE, never by a summary number the
    response advertises about rows it did not return. RED input: this
    response, whose own `max_duration_ms`/`slowest_ms`/`total_count`
    announce a 3-second execution in the window while every returned row
    is sub-millisecond. If either checker ever reached for a summary field
    instead of `rows`, this passes and the tab is still empty on stage."""
    crowded_out = {
        "rows": [{"query_id": str(i), "duration_ms": 0.18,
                  "in_progress": False, "end_inferred": False}
                 for i in range(100)],
        "total_count": 48000, "open_count": 0, "completed_count": 48000,
        "max_duration_ms": 3003.8, "slowest_ms": 3003.8, "truncated": True,
    }
    ok, detail = cov.waterfall_populated(crowded_out)
    check(ok is False,
          f"the 500ms floor FAILS when no returned row clears it, however "
          f"loudly the response's own summary fields claim otherwise "
          f"({detail})")
    ok2, detail2 = cov.waterfall_floor_excludes_inferred(crowded_out)
    check(ok2 is False,
          f"the measured-row requirement likewise reads only `rows` "
          f"({detail2})")


def test_retain_waterfall_raw_writes_every_slice_it_was_given():
    raw = _tmpdir()
    cov._retain_waterfall_raw(raw, {"executions_duration_desc": SLOW_PAGE,
                                    "executions_recency": {"rows": []}})
    for name in cov.WATERFALL_RAW_SLICES:
        check(os.path.exists(os.path.join(raw, name + ".json")),
              f"{name}.json was written")
    check(_read_raw(raw, "executions_duration_desc") == SLOW_PAGE,
          "the retained duration_desc page is the response VERBATIM, so "
          "the floor can be re-derived from it")


def test_retain_waterfall_raw_marks_an_uncollected_slice_never_omits_it():
    """Absent-rather-than-wrong: when a slice was never collected the file
    must still exist and say so. RED without the marker: the file is
    simply missing, and "missing" cannot be told apart from "this code
    never ran", which is how an evidence gate goes quietly blind."""
    raw = _tmpdir()
    cov._retain_waterfall_raw(raw, {"executions_duration_desc": SLOW_PAGE})
    got = _read_raw(raw, "executions_recency")
    check("pgwt_artifact_error" in got,
          f"the uncollected slice still gets a file, carrying an explicit "
          f"not-collected marker rather than being absent ({got})")


def test_retain_waterfall_raw_overwrites_a_stale_previous_run():
    """The retained-evidence failure this repo keeps finding: a reader
    picks up the PREVIOUS run's page and believes it belongs to this one.
    RED if the file were ever opened for append, or written only when the
    slice was collected."""
    raw = _tmpdir()
    cov._retain_waterfall_raw(raw, {"executions_duration_desc":
                                    {"rows": [{"stale": True}]}})
    cov._retain_waterfall_raw(raw, {})          # a later run collects nothing
    got = _read_raw(raw, "executions_duration_desc")
    check("pgwt_artifact_error" in got and "rows" not in got,
          f"the stale page is GONE, replaced by this run's marker ({got})")


def test_run_coverage_retains_both_slices_end_to_end():
    raw = _tmpdir()
    srv = _retention_server(SLOW_PAGE, {"rows": [{"duration_ms": 0.1,
                                                  "in_progress": False,
                                                  "start_ns": "5"}]})
    results = cov.run_coverage(srv, 0, 1, num_cpus=4, raw_out_dir=raw)
    check(results["waterfall"]["ok"] is True,
          f"waterfall passes on the slow page ({results['waterfall']['detail']})")
    check(_read_raw(raw, "executions_duration_desc") == SLOW_PAGE,
          "run_coverage retained the exact page its verdict came from")
    check(_read_raw(raw, "executions_recency")["rows"][0]["duration_ms"] == 0.1,
          "and the recency page beside it, from the same run and window")


def test_run_coverage_retains_raw_when_the_executions_query_raises():
    """Dependency-failure bypass: the query blows up (timeout, dead
    server, exit 127 harness). The tab must FAIL (already covered) and the
    artifacts must still exist as markers -- an unwritten directory would
    read as "nobody checked" to the next person."""
    raw = _tmpdir()
    srv = _retention_server(None, exc=RuntimeError("server gone"))
    results = cov.run_coverage(srv, 0, 1, num_cpus=4, raw_out_dir=raw)
    check(results["waterfall"]["ok"] is False,
          "waterfall fails when its query raises")
    for name in cov.WATERFALL_RAW_SLICES:
        got = _read_raw(raw, name)
        check("pgwt_artifact_error" in got,
              f"{name} exists and says it was not collected ({got})")


def test_run_coverage_retains_an_error_response_rather_than_nothing():
    raw = _tmpdir()
    srv = _retention_server({"error": "window too large", "code": "E_TOO_BIG"})
    results = cov.run_coverage(srv, 0, 1, num_cpus=4, raw_out_dir=raw)
    check(results["waterfall"]["ok"] is False, "an error response fails the tab")
    got = _read_raw(raw, "executions_duration_desc")
    check(got.get("error") == "window too large",
          f"the error response itself is retained, so the reason is "
          f"readable from the artifact directory ({got})")


def test_run_coverage_without_raw_out_dir_writes_nothing():
    """The default must stay inert for every pre-existing caller: no
    directory created, no file written, no exception. Checked by running
    with cwd inside an empty scratch tree and then walking that WHOLE
    tree -- not by re-listing one directory we happened to name. RED
    input: make the flush unconditional with a fallback destination
    (`_retain_waterfall_raw(raw_out_dir or os.getcwd(), wf_raw)`); a test
    that only re-lists its own unused tmpdir still passes that mutation,
    which is why this walks instead."""
    import tempfile
    root = tempfile.mkdtemp(prefix="pgwt_cov_noraw_")
    cwd = os.getcwd()
    try:
        os.chdir(root)
        srv = _retention_server(SLOW_PAGE, {"rows": []})
        cov.run_coverage(srv, 0, 1, num_cpus=4)
        written = [os.path.join(d, f)
                   for d, _sub, files in os.walk(root) for f in files]
    finally:
        os.chdir(cwd)
    check(written == [],
          f"raw_out_dir defaults to None and nothing is written anywhere, "
          f"including the process's own working directory ({written})")


def test_demo_rehearsal_still_passes_raw_out_dir_to_run_coverage():
    """Wiring guard: run_coverage's retention is opt-in, so the ONE caller
    whose artifacts the PR evidence depends on must keep opting in. RED
    input: drop `raw_out_dir=` from tests/demo_rehearsal.py's
    run_coverage(...) call -- every other test here still passes, because
    they call run_coverage directly."""
    here = os.path.dirname(os.path.abspath(__file__))
    with open(os.path.join(here, "demo_rehearsal.py")) as f:
        src = f.read()
    i = src.find("cov.run_coverage(")
    check(i >= 0, "demo_rehearsal.py calls cov.run_coverage")
    call = src[i:i + 400]
    check("raw_out_dir=" in call,
          f"demo_rehearsal.py's run_coverage call still passes raw_out_dir "
          f"({call[:200]!r})")



def run():
    tests = [v for k, v in sorted(globals().items()) if k.startswith("test_")]
    for t in tests:
        print(f"-- {t.__name__}")
        t()
    print(f"\n{tests_passed}/{tests_run} passed, {tests_failed} failed")
    return 1 if tests_failed else 0


if __name__ == "__main__":
    sys.exit(run())
