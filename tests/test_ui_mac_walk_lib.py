#!/usr/bin/env python3
"""test_ui_mac_walk_lib.py -- unit tests for tests/ui_mac_walk_lib.py.
Pure Python, no subprocess, no network, no Playwright -- runs anywhere:

    python3 tests/test_ui_mac_walk_lib.py

Covers the parsers (sysctl / memory_pressure output -> int-or-None, never a
silent wrong default) and the attribution/void logic the criteria doc
requires (a tick under non-normal Mac pressure must be void, not "failed" or
"passed" -- and an UNMEASURED tick must fail the same way a bad one does,
never default to "fine").
"""
import os
import shutil
import sys
import tempfile

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import ui_mac_walk_lib as lib
# prepare_run_dir/update_latest_pointer are pure filesystem helpers (no
# subprocess, no network, no browser) -- imported here rather than
# duplicated, so the run-directory-retention tests below exercise the
# ACTUAL function ui_mac_walk_safari.py's main() calls, not a copy that
# could drift from it.
import ui_mac_walk_safari as walk

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


# ── parse_pressure_level ─────────────────────────────────────────────────

def test_parse_pressure_level_normal():
    check(lib.parse_pressure_level("kern.memorystatus_vm_pressure_level: 1") == 1,
          "parse_pressure_level: normal (1)")


def test_parse_pressure_level_warn():
    check(lib.parse_pressure_level("kern.memorystatus_vm_pressure_level: 2") == 2,
          "parse_pressure_level: warn (2)")


def test_parse_pressure_level_empty():
    # RED-DEMONSTRATED input: empty stdout, e.g. sysctl exited 126/127 and
    # the caller passed through its (empty) stdout rather than raising --
    # this is the exact "dependency exiting 126/127" bypass case. Must be
    # None, never a fabricated 0/1.
    check(lib.parse_pressure_level("") is None,
          "parse_pressure_level: empty input -> None, not a guessed level")


def test_parse_pressure_level_garbage():
    check(lib.parse_pressure_level("command not found") is None,
          "parse_pressure_level: unparseable text -> None")


def test_parse_pressure_level_none_input():
    check(lib.parse_pressure_level(None) is None,
          "parse_pressure_level: None input -> None (no crash)")


# ── parse_free_percent ───────────────────────────────────────────────────

def test_parse_free_percent_ok():
    text = ("The system has 34359738368 ...\n"
            "System-wide memory free percentage: 46%\n")
    check(lib.parse_free_percent(text) == 46, "parse_free_percent: 46%")


def test_parse_free_percent_missing():
    check(lib.parse_free_percent("some unrelated output") is None,
          "parse_free_percent: no matching line -> None")


# ── pressure_level_name ──────────────────────────────────────────────────

def test_pressure_level_name_known():
    check(lib.pressure_level_name(1) == "normal", "pressure_level_name(1)")
    check(lib.pressure_level_name(2) == "warn", "pressure_level_name(2)")
    check(lib.pressure_level_name(4) == "critical", "pressure_level_name(4)")


def test_pressure_level_name_unknown():
    check(lib.pressure_level_name(99) == "unknown(99)",
          "pressure_level_name: unrecognised numeric level is labelled, not silently 'normal'")


def test_pressure_level_name_none():
    check(lib.pressure_level_name(None) == "unmeasured",
          "pressure_level_name(None) -> 'unmeasured', distinct from any real level")


# ── sample_is_normal: the fail-closed contract ───────────────────────────

def test_sample_is_normal_true():
    check(lib.sample_is_normal({"pressure_level": 1}) is True,
          "sample_is_normal: level 1 -> True")


def test_sample_is_normal_warn_false():
    # This is the RED case for the "product broken" bypass: if this ever
    # returned True, a warn-level tick would be silently treated as
    # attributable-normal, and a real Mac-pressure artefact would pass
    # through as if the product had been measured cleanly.
    check(lib.sample_is_normal({"pressure_level": 2}) is False,
          "sample_is_normal: level 2 (warn) -> False")


def test_sample_is_normal_missing_key_false():
    # Bypass case: an unmeasured sample (missing key entirely, e.g. the
    # sysctl subprocess itself failed) must NOT default to True/normal --
    # that would let an entirely unattributable run report as clean.
    check(lib.sample_is_normal({}) is False,
          "sample_is_normal: missing pressure_level -> False (fail closed, not defaulted to normal)")


def test_sample_is_normal_none_level_false():
    check(lib.sample_is_normal({"pressure_level": None}) is False,
          "sample_is_normal: pressure_level=None -> False")


# ── bracketing_samples / window_void_reason: the attribution gate ────────
# BRACKETING semantics (changed 2026-09-28): a window is judged by the most
# recent sample at/before its start and the earliest sample at/after its
# end -- not by requiring a sample strictly inside it. See
# ui_mac_walk_lib.window_void_reason's own docstring for why (a sub-second
# tab window can legitimately have zero samples land inside it on a
# perfectly quiet machine).

def test_bracketing_samples_finds_both_sides():
    samples = [{"ts": 5, "pressure_level": 1}, {"ts": 15, "pressure_level": 1}]
    before, after = lib.bracketing_samples(samples, 9, 13)
    check(before["ts"] == 5, f"bracketing_samples: before is the ts=5 sample (got {before})")
    check(after["ts"] == 15, f"bracketing_samples: after is the ts=15 sample (got {after})")


def test_bracketing_samples_picks_nearest():
    samples = [{"ts": 1, "pressure_level": 1}, {"ts": 8, "pressure_level": 1},
               {"ts": 15, "pressure_level": 1}, {"ts": 30, "pressure_level": 1}]
    before, after = lib.bracketing_samples(samples, 9, 13)
    check(before["ts"] == 8, f"bracketing_samples: nearest before (8, not 1) (got {before})")
    check(after["ts"] == 15, f"bracketing_samples: nearest after (15, not 30) (got {after})")


def test_bracketing_samples_missing_before():
    samples = [{"ts": 15, "pressure_level": 1}]
    before, after = lib.bracketing_samples(samples, 9, 13)
    check(before is None, "bracketing_samples: no sample <= start -> before is None")
    check(after is not None, "bracketing_samples: after still found")


def test_bracketing_samples_missing_after():
    samples = [{"ts": 5, "pressure_level": 1}]
    before, after = lib.bracketing_samples(samples, 9, 13)
    check(before is not None, "bracketing_samples: before still found")
    check(after is None, "bracketing_samples: no sample >= end -> after is None")


# max_gap_s=100 in the tests below: generously large, deliberately out of
# scope for what those specific tests exercise (normal/bad/missing-side
# logic, not the distance bound) -- see the dedicated max_gap_s tests
# further down for the bound itself.
_WIDE_BOUND = 100.0


def test_window_void_reason_bracketed_by_two_normal_counts():
    # The exact case this change exists for: a sub-second window with NO
    # sample landing inside it, bracketed by two normal samples just
    # outside it -- must count, not void.
    samples = [{"ts": 9.0, "pressure_level": 1}, {"ts": 9.9, "pressure_level": 1}]
    reason = lib.window_void_reason(samples, 9.2, 9.5, _WIDE_BOUND)
    check(reason is None,
          f"window_void_reason: bracketed by two normal samples (none inside "
          f"the window itself) -> not void (got: {reason!r})")


def test_window_void_reason_one_bracket_bad_voids():
    samples = [{"ts": 5, "pressure_level": 1}, {"ts": 15, "pressure_level": 2}]
    reason = lib.window_void_reason(samples, 9, 13, _WIDE_BOUND)
    check(reason is not None and "1/2" in reason,
          f"window_void_reason: one non-normal bracketing sample voids the window (got: {reason!r})")


def test_window_void_reason_no_sample_before_is_void():
    # RED-demonstrated bypass case: no sample on ONE side (e.g. the walk
    # started before sampling began) -- the real "cannot attribute" case;
    # a gate that only checked the "after" side would silently certify
    # this as clean.
    samples = [{"ts": 15, "pressure_level": 1}]
    reason = lib.window_void_reason(samples, 9, 13, _WIDE_BOUND)
    check(reason is not None and "before" in reason,
          f"window_void_reason: no sample before window start -> void (got: {reason!r})")


def test_window_void_reason_no_sample_after_is_void():
    samples = [{"ts": 5, "pressure_level": 1}]
    reason = lib.window_void_reason(samples, 9, 13, _WIDE_BOUND)
    check(reason is not None and "after" in reason,
          f"window_void_reason: no sample after window end -> void (got: {reason!r})")


def test_window_void_reason_empty_samples_is_void():
    reason = lib.window_void_reason([], 9, 13, _WIDE_BOUND)
    check(reason is not None,
          f"window_void_reason: no samples at all -> void (got: {reason!r})")


def test_window_void_reason_unmeasured_bracketing_sample_voids():
    samples = [{"ts": 5, "pressure_level": None}, {"ts": 15, "pressure_level": 1}]
    reason = lib.window_void_reason(samples, 9, 13, _WIDE_BOUND)
    check(reason is not None and "unmeasured" in reason,
          f"window_void_reason: an unmeasured bracketing sample voids too (got: {reason!r})")


def test_window_void_reason_boundary_inclusive():
    # A sample exactly AT the window's start/end counts as its own bracket
    # (ts <= start, ts >= end are both inclusive).
    samples = [{"ts": 5, "pressure_level": 1}, {"ts": 10, "pressure_level": 1}]
    check(lib.window_void_reason(samples, 5, 10, _WIDE_BOUND) is None,
          "window_void_reason: samples exactly on the window boundary bracket it")


# ── max_gap_s: the distance bound (review blocker, 2026-09-28) ───────────
# Without a bound, two normal samples arbitrarily far apart certify
# everything between them as quiet -- reproduced by the reviewer: samples
# at ts=1000 and ts=1120 (120s apart) both normal "certify" a window at
# 1059-1061 with nothing sampled anywhere near it, including two full
# minutes of possible real load in between.

def test_window_void_reason_stale_bracket_voids_even_when_normal():
    # RED-demonstrated bypass case: this is EXACTLY the reviewer's
    # reproduction. Both bracketing samples are normal; without a distance
    # bound this would wrongly pass.
    samples = [{"ts": 1000, "pressure_level": 1}, {"ts": 1120, "pressure_level": 1}]
    reason = lib.window_void_reason(samples, 1059, 1061, max_gap_s=5.0)
    check(reason is not None and "too far" in reason,
          f"window_void_reason: bracketing samples 120s apart (bound 5s) "
          f"void even though both are normal (got: {reason!r})")


def test_window_void_reason_within_bound_counts():
    samples = [{"ts": 1000, "pressure_level": 1}, {"ts": 1004, "pressure_level": 1}]
    reason = lib.window_void_reason(samples, 1001, 1003, max_gap_s=5.0)
    check(reason is None,
          f"window_void_reason: bracketing gap (1s each side) within the "
          f"5s bound -> not void (got: {reason!r})")


def test_window_void_reason_exactly_at_bound_counts():
    samples = [{"ts": 995, "pressure_level": 1}, {"ts": 1006, "pressure_level": 1}]
    reason = lib.window_void_reason(samples, 1000, 1001, max_gap_s=5.0)
    check(reason is None,
          f"window_void_reason: gap exactly AT the bound (5.0s) counts, "
          f"not just strictly under it (got: {reason!r})")


# ── sample_cadence_stats ─────────────────────────────────────────────────

def test_sample_cadence_stats_basic():
    samples = [{"ts": 0}, {"ts": 1}, {"ts": 3}]
    mean_gap, max_gap = lib.sample_cadence_stats(samples)
    check(abs(mean_gap - 1.5) < 1e-9, f"sample_cadence_stats: mean of [1,2] -> 1.5 (got {mean_gap})")
    check(max_gap == 2, f"sample_cadence_stats: max of [1,2] -> 2 (got {max_gap})")


def test_sample_cadence_stats_unsorted_input():
    samples = [{"ts": 3}, {"ts": 0}, {"ts": 1}]
    mean_gap, max_gap = lib.sample_cadence_stats(samples)
    check(max_gap == 2, f"sample_cadence_stats: sorts input before computing gaps (got {max_gap})")


def test_sample_cadence_stats_fewer_than_two_is_none():
    check(lib.sample_cadence_stats([{"ts": 0}]) == (None, None),
          "sample_cadence_stats: fewer than 2 samples -> (None, None)")
    check(lib.sample_cadence_stats([]) == (None, None),
          "sample_cadence_stats: no samples -> (None, None)")


# ── freshness_ok ──────────────────────────────────────────────────────────

def test_freshness_ok_within_bound():
    ok, detail = lib.freshness_ok(3.0)
    check(ok is True, f"freshness_ok(3.0s): within {lib.FRESHNESS_MAX_AGE_S}s bound ({detail})")


def test_freshness_ok_over_bound():
    # RED case: this is exactly the "frozen chart" the criteria doc warns
    # about -- neither blank nor throwing, just stale.
    ok, detail = lib.freshness_ok(60.0)
    check(ok is False, f"freshness_ok(60.0s): exceeds bound ({detail})")


def test_freshness_ok_none_is_not_ok():
    ok, detail = lib.freshness_ok(None)
    check(ok is False and "not measured" in detail,
          "freshness_ok(None): unmeasured freshness must not read as passing")


def test_freshness_ok_negative_is_not_ok():
    ok, detail = lib.freshness_ok(-1.0)
    check(ok is False, "freshness_ok: negative age (clock skew) is not ok, not silently clamped")


def test_freshness_ok_exact_bound():
    ok, _ = lib.freshness_ok(lib.FRESHNESS_MAX_AGE_S)
    check(ok is True, "freshness_ok: exactly at the bound is ok (<=, not <)")


# ── build_tab_entry ───────────────────────────────────────────────────────

def test_build_tab_entry_clean():
    entry = lib.build_tab_entry("overview", True, "5 rows", 1.2, [], [], 2.0, None)
    check(entry["ok"] is True, "build_tab_entry: clean tab is ok")
    check(entry["void_reason"] is None, "build_tab_entry: clean tab has no void_reason")


def test_build_tab_entry_render_state_empty_still_ok():
    # 2026-09-28 review correction: waterfall/scatter's documented empty
    # state ("No executions for selected range") is a legitimate, working
    # panel -- rendered=True, render_state='empty' -- distinct from a real
    # render failure, and it must not fail the tab by itself.
    entry = lib.build_tab_entry("waterfall", True, "empty: No executions for selected range",
                                 0.5, [], [], 2.0, None, render_state="empty")
    check(entry["render_state"] == "empty",
          "build_tab_entry: render_state 'empty' persisted verbatim")
    check(entry["ok"] is True,
          "build_tab_entry: a legitimately empty panel is still ok=True")


def test_build_tab_entry_render_state_defaults_to_none():
    entry = lib.build_tab_entry("overview", True, "5 rows", 1.2, [], [], 2.0, None)
    check(entry["render_state"] is None,
          "build_tab_entry: render_state defaults to None for callers that don't set it")


# ── freshness_status / verdict_status: the aggregation fix (review
# blocker, 2026-09-28) ────────────────────────────────────────────────────
# Found live: tests/results/ui_mac_walk_safari/summary.json recorded
# `counted: true, ok: true` while every one of its 11 tabs carried
# freshness age_s: null ("freshness not measured") -- an unmeasured
# sub-check silently passed through into a verdict that said pass.

def test_freshness_status_unmeasured():
    check(lib.freshness_status(None) == "unmeasured",
          "freshness_status: age_s=None -> 'unmeasured', not 'ok' or 'failed'")


def test_freshness_status_ok():
    check(lib.freshness_status(3.0) == "ok",
          "freshness_status: within bound -> 'ok'")


def test_freshness_status_failed():
    check(lib.freshness_status(60.0) == "failed",
          "freshness_status: over bound -> 'failed'")
    check(lib.freshness_status(-1.0) == "failed",
          "freshness_status: negative age -> 'failed'")


def test_build_tab_entry_unmeasured_freshness_fails_the_tab():
    # THE bypass this fix closes: rendered=True, no console errors, not
    # void -- everything else about this tab looks clean -- but
    # freshness_age_s=None must still make ok False.
    entry = lib.build_tab_entry("overview", True, "5 rows", 1.2, [], [],
                                 freshness_age_s=None, mac_void_reason=None)
    check(entry["freshness"]["status"] == "unmeasured",
          "build_tab_entry: unmeasured freshness recorded as such")
    check(entry["ok"] is False,
          "build_tab_entry: unmeasured freshness makes ok False -- an "
          "unmeasured criterion is never a satisfied one")


def test_build_tab_entry_failed_freshness_fails_the_tab():
    entry = lib.build_tab_entry("overview", True, "5 rows", 1.2, [], [],
                                 freshness_age_s=60.0, mac_void_reason=None)
    check(entry["freshness"]["status"] == "failed",
          "build_tab_entry: stale freshness recorded as 'failed'")
    check(entry["ok"] is False,
          "build_tab_entry: a real freshness failure makes ok False")


def test_build_verdict_one_unmeasured_freshness_sub_check_fails_the_whole_run():
    # The exact reproduction of the review's finding: 11 tabs, all
    # rendered/clean/non-void, but freshness unmeasured on all of them --
    # must NOT aggregate to ok: true.
    tabs = [lib.build_tab_entry(f"tab{i}", True, "ok", 1.0, [], [],
                                 freshness_age_s=None, mac_void_reason=None)
            for i in range(11)]
    v = lib.build_verdict("run-x", tabs, [{"ts": 1, "pressure_level": 1}])
    check(v["ok"] is False,
          "build_verdict: unmeasured freshness on every tab -> ok False, "
          "not the vacuous pass found live")
    check(v["status"] == "failed",
          f"build_verdict: status is 'failed' (counted but not satisfied), "
          f"got {v['status']!r}")


def test_verdict_status_unmeasured():
    check(lib.verdict_status(counted=False, ok=True) == "unmeasured",
          "verdict_status: not counted -> 'unmeasured' regardless of ok")


def test_verdict_status_failed():
    check(lib.verdict_status(counted=True, ok=False) == "failed",
          "verdict_status: counted but not ok -> 'failed'")


def test_verdict_status_satisfied():
    check(lib.verdict_status(counted=True, ok=True) == "satisfied",
          "verdict_status: counted and ok -> 'satisfied'")


def test_build_tab_entry_defaults_wall_times_to_none():
    # Existing callers (tests/ui_mac_walk.py's Chrome walk) never pass
    # these -- must not break, and must not fabricate a timestamp.
    entry = lib.build_tab_entry("overview", True, "5 rows", 1.2, [], [], 2.0, None)
    check(entry["wall_start"] is None and entry["wall_end"] is None,
          "build_tab_entry: wall_start/wall_end default to None when not given")


def test_build_tab_entry_persists_wall_times_and_bracket():
    # The exact gap that made run8 un-re-derivable: these four fields must
    # round-trip into the verdict so a future run can be re-judged from
    # stored data alone.
    entry = lib.build_tab_entry(
        "overview", True, "5 rows", 1.2, [], [], 2.0, None,
        wall_start=100.0, wall_end=100.5,
        bracket_before={"ts": 99.0, "pressure_level": 1},
        bracket_after={"ts": 101.0, "pressure_level": 1})
    check(entry["wall_start"] == 100.0 and entry["wall_end"] == 100.5,
          "build_tab_entry: wall_start/wall_end persisted verbatim")
    check(entry["bracket_before"]["ts"] == 99.0 and entry["bracket_after"]["ts"] == 101.0,
          "build_tab_entry: the actual bracketing samples are persisted, not just the verdict")


def test_build_tab_entry_console_error_fails():
    entry = lib.build_tab_entry("events", True, "3 rows", 1.0, ["console: boom"], [], 1.0, None)
    check(entry["ok"] is False,
          "build_tab_entry: a real console error fails the tab even though it rendered")


def test_build_tab_entry_not_rendered_fails():
    entry = lib.build_tab_entry("events", False, "no rows", None, [], [], None, None)
    check(entry["ok"] is False, "build_tab_entry: not rendered -> not ok")


def test_build_tab_entry_void_fails_ok_but_is_distinguishable():
    entry = lib.build_tab_entry("events", True, "3 rows", 1.0, [], [], 1.0,
                                 "1/1 mac samples non-normal during this window (warn)")
    check(entry["ok"] is False,
          "build_tab_entry: a void window makes ok False (never silently passed)")
    check(entry["void_reason"] is not None,
          "build_tab_entry: void_reason is reported so a reader can separate infra noise from a real bug")


# ── run_id_matches ────────────────────────────────────────────────────────

def test_run_id_matches_true():
    check(lib.run_id_matches({"run": {"id": "abc"}}, "abc") is True,
          "run_id_matches: matching id -> True")


def test_run_id_matches_stale():
    # RED case: a stale verdict dict from a PREVIOUS invocation must not
    # satisfy a caller checking against THIS run's id.
    check(lib.run_id_matches({"run": {"id": "old-run"}}, "new-run") is False,
          "run_id_matches: stale run.id does not match -- staleness must be caught")


def test_run_id_matches_missing():
    check(lib.run_id_matches({}, "abc") is False,
          "run_id_matches: missing run.id -> False, not a crash")


# ── build_verdict ─────────────────────────────────────────────────────────

def test_build_verdict_all_ok():
    tabs = [lib.build_tab_entry("overview", True, "ok", 1.0, [], [], 1.0, None)]
    v = lib.build_verdict("run-1", tabs, [{"ts": 1, "pressure_level": 1}])
    check(v["ok"] is True, "build_verdict: single clean tab -> ok True")
    check(v["run"]["id"] == "run-1", "build_verdict: run.id carried through")


def test_build_verdict_one_bad_tab_fails_overall():
    tabs = [
        lib.build_tab_entry("overview", True, "ok", 1.0, [], [], 1.0, None),
        lib.build_tab_entry("events", False, "blank", None, [], [], None, None),
    ]
    v = lib.build_verdict("run-2", tabs, [])
    check(v["ok"] is False, "build_verdict: one broken tab fails the overall verdict")


def test_build_verdict_empty_tabs_not_ok():
    # RED case: an empty tab list (the walk crashed before visiting
    # anything) must not report ok=True by vacuous truth over an empty list
    # -- a gate that can't see must refuse, never approve.
    v = lib.build_verdict("run-3", [], [])
    check(v["ok"] is False, "build_verdict: zero tabs -> ok False, not vacuously True")


def test_build_verdict_all_void_not_ok():
    # RED case: every tab void (Mac was under pressure the whole run) must
    # not report ok=True either -- "nothing failed" is not the same claim
    # as "everything was measured clean".
    tabs = [
        lib.build_tab_entry("overview", True, "ok", 1.0, [], [], 1.0, "no mac samples recorded"),
        lib.build_tab_entry("events", True, "ok", 1.0, [], [], 1.0, "no mac samples recorded"),
    ]
    v = lib.build_verdict("run-4", tabs, [])
    check(v["ok"] is False,
          "build_verdict: all-void run is not ok (measuring nothing is not 'clean')")
    check(set(v["void_tabs"]) == {"overview", "events"},
          "build_verdict: void_tabs lists every void tab")


# ── classify_status / is_connected_state ─────────────────────────────────

def test_classify_status_connected():
    check(lib.classify_status("status connected") == "connected",
          "classify_status: 'status connected' -> connected")


def test_classify_status_error():
    check(lib.classify_status("status error") == "error",
          "classify_status: 'status error' -> error (the reconnect-loop class)")


def test_classify_status_degraded():
    check(lib.classify_status("status degraded") == "degraded",
          "classify_status: 'status degraded' -> degraded (WS open, ssh dead)")


def test_classify_status_connecting():
    check(lib.classify_status("status connecting") == "connecting",
          "classify_status: 'status connecting' -> connecting")


def test_classify_status_empty_is_none():
    # RED case: an empty class string (element not found / not yet
    # rendered) must not be silently read as any real state.
    check(lib.classify_status("") is None, "classify_status: empty string -> None")
    check(lib.classify_status(None) is None, "classify_status: None -> None")


def test_classify_status_unrecognised_is_none():
    # RED case: a future/unknown class word must not default to
    # 'connected' -- that would report an actual drop as healthy.
    check(lib.classify_status("status mystery-future-state") is None,
          "classify_status: unrecognised class word -> None, not guessed")


def test_is_connected_state():
    check(lib.is_connected_state("status connected") is True,
          "is_connected_state: true only for the connected class")
    check(lib.is_connected_state("status error") is False,
          "is_connected_state: error class -> False")
    check(lib.is_connected_state("") is False,
          "is_connected_state: empty class -> False (fail closed, not assumed connected)")


# ── version_skew_from_snapshot ────────────────────────────────────────────

def test_version_skew_absent_element():
    check(lib.version_skew_from_snapshot({"present": False}) is None,
          "version_skew_from_snapshot: element absent -> None")


def test_version_skew_present_but_hidden():
    # RED case: app.js's no-skew path sets style.display='none' but leaves
    # the element (and any stale text/class) in the DOM -- a reader that
    # only checked "present" would wrongly report skew on a clean run.
    snap = {"present": True, "visible": False, "text": "", "cls": "version-skew"}
    check(lib.version_skew_from_snapshot(snap) is None,
          "version_skew_from_snapshot: present but hidden -> None (not a false positive)")


def test_version_skew_warn():
    snap = {"present": True, "visible": True, "text": "⚠ version skew",
            "cls": "version-skew warn"}
    got = lib.version_skew_from_snapshot(snap)
    check(got is not None and got["level"] == "warn",
          f"version_skew_from_snapshot: visible warn badge -> level=warn (got {got})")


def test_version_skew_error():
    snap = {"present": True, "visible": True, "text": "⚠ protocol mismatch",
            "cls": "version-skew error"}
    got = lib.version_skew_from_snapshot(snap)
    check(got is not None and got["level"] == "error",
          f"version_skew_from_snapshot: visible error badge -> level=error (got {got})")


def test_version_skew_none_snapshot():
    check(lib.version_skew_from_snapshot(None) is None,
          "version_skew_from_snapshot: None snapshot -> None, not a crash")


# ── majority_void ─────────────────────────────────────────────────────────

def _entry(void):
    return lib.build_tab_entry("x", True, "ok", 1.0, [], [], 1.0,
                                "void" if void else None)


def test_majority_void_all_clean():
    tabs = [_entry(False) for _ in range(11)]
    check(lib.majority_void(tabs) is False, "majority_void: 0/11 void -> False")


def test_majority_void_majority():
    tabs = [_entry(True)] * 6 + [_entry(False)] * 5
    check(lib.majority_void(tabs) is True, "majority_void: 6/11 void -> True")


def test_majority_void_exact_half_not_majority():
    # RED case: a tie must NOT count as majority -- "more than half", not
    # "at least half".
    tabs = [_entry(True)] * 5 + [_entry(False)] * 5
    check(lib.majority_void(tabs) is False,
          "majority_void: 5/10 (exact half) -> False, not majority")


def test_majority_void_empty_counts_as_void():
    # RED case: an empty tab list (walk crashed before visiting anything)
    # must not read as "0 void out of 0, so fine" -- nothing measured does
    # not count.
    check(lib.majority_void([]) is True,
          "majority_void: empty list -> True (nothing measured doesn't count)")


def test_build_verdict_counted_field():
    clean_tabs = [_entry(False) for _ in range(11)]
    v = lib.build_verdict("run-5", clean_tabs, [])
    check(v["counted"] is True, "build_verdict: mostly-clean run has counted=True")
    check(v["void_count"] == 0, "build_verdict: void_count reflects actual voids")

    mostly_void_tabs = [_entry(True)] * 7 + [_entry(False)] * 4
    v2 = lib.build_verdict("run-6", mostly_void_tabs, [])
    check(v2["counted"] is False,
          "build_verdict: majority-void run has counted=False")
    check(v2["void_count"] == 7, "build_verdict: void_count == 7")


# ── screen_locked_from_ioreg / display_asleep_from_system_profiler /
#    walk_precondition_reason ──────────────────────────────────────────────

def test_screen_locked_true():
    d = {"IOConsoleUsers": [{"CGSSessionScreenIsLocked": True}]}
    check(lib.screen_locked_from_ioreg(d) is True,
          "screen_locked_from_ioreg: locked -> True")


def test_screen_locked_false():
    d = {"IOConsoleUsers": [{"CGSSessionScreenIsLocked": False}]}
    check(lib.screen_locked_from_ioreg(d) is False,
          "screen_locked_from_ioreg: unlocked -> False")


def test_screen_locked_missing_key_defaults_false():
    # The real plist only carries CGSSessionScreenIsLocked when there IS a
    # session; absence of the key on a present session dict is macOS's own
    # "not locked" shape.
    d = {"IOConsoleUsers": [{}]}
    check(lib.screen_locked_from_ioreg(d) is False,
          "screen_locked_from_ioreg: key absent on a real session dict -> False")


def test_screen_locked_empty_shape_is_none():
    # RED case: no IOConsoleUsers at all (ioreg exited 126/127, or its own
    # output shape changed) must be None, never silently "unlocked".
    check(lib.screen_locked_from_ioreg({}) is None,
          "screen_locked_from_ioreg: no IOConsoleUsers -> None, not assumed unlocked")
    check(lib.screen_locked_from_ioreg({"IOConsoleUsers": []}) is None,
          "screen_locked_from_ioreg: empty IOConsoleUsers -> None")


def test_display_asleep_true():
    d = {"SPDisplaysDataType": [{"spdisplays_ndrvs": [
        {"spdisplays_asleep": "spdisplays_yes"}]}]}
    check(lib.display_asleep_from_system_profiler(d) is True,
          "display_asleep_from_system_profiler: asleep -> True")


def test_display_asleep_false():
    d = {"SPDisplaysDataType": [{"spdisplays_ndrvs": [
        {"spdisplays_online": "spdisplays_yes"}]}]}
    check(lib.display_asleep_from_system_profiler(d) is False,
          "display_asleep_from_system_profiler: display present, no asleep key -> False")


def test_display_asleep_one_of_two_asleep():
    d = {"SPDisplaysDataType": [{"spdisplays_ndrvs": [
        {"spdisplays_asleep": "spdisplays_no"},
        {"spdisplays_asleep": "spdisplays_yes"},
    ]}]}
    check(lib.display_asleep_from_system_profiler(d) is True,
          "display_asleep_from_system_profiler: ANY asleep display -> True")


def test_display_asleep_no_displays_is_none():
    # RED case: no displays listed at all (system_profiler exited non-zero
    # upstream, or its shape changed) must be None, not "not asleep".
    check(lib.display_asleep_from_system_profiler({"SPDisplaysDataType": []}) is None,
          "display_asleep_from_system_profiler: no GPUs -> None")
    check(lib.display_asleep_from_system_profiler(
        {"SPDisplaysDataType": [{"spdisplays_ndrvs": []}]}) is None,
        "display_asleep_from_system_profiler: GPU with no displays -> None")


# ── empty_state_matches: the bypass-trap fix (review blocker, 2026-09-28) ──
# app.js's switchTab() puts a GENERIC ".loading">Loading...</div> into the
# SAME container (#table-container) a tab's own genuine empty-result state
# uses -- a check that only asked "is there a .loading element" would go
# green on a hung fetch that never actually returned.

def test_empty_state_matches_exact_text():
    snap = {"present": True, "text": "No data for selected range"}
    check(lib.empty_state_matches(snap, "No data for selected range") is True,
          "empty_state_matches: exact text match -> True")


def test_empty_state_matches_mocked_hung_request_is_red():
    # THE bypass case, demonstrated directly: a hung fetch leaves the
    # GENERIC transient "Loading..." text in the exact same container/class
    # the genuine empty state uses. A naive "is .loading present" check
    # would wrongly call this "empty" and pass -- this must stay False.
    snap = {"present": True, "text": "Loading..."}
    check(lib.empty_state_matches(snap, "No data for selected range") is False,
          "empty_state_matches: generic 'Loading...' text (a hung/in-flight "
          "fetch, not a genuine empty result) must NOT match -- red case "
          "for the exact bypass review found")


def test_empty_state_matches_not_present():
    check(lib.empty_state_matches({"present": False, "text": None},
                                   "No data for selected range") is False,
          "empty_state_matches: element not present -> False")


def test_empty_state_matches_whitespace_tolerant():
    snap = {"present": True, "text": "  No data for selected range  \n"}
    check(lib.empty_state_matches(snap, "No data for selected range") is True,
          "empty_state_matches: surrounding whitespace from textContent is stripped")


def test_empty_state_matches_none_snapshot():
    check(lib.empty_state_matches(None, "No data for selected range") is False,
          "empty_state_matches: None snapshot -> False, not a crash")


# ── capture_age_reason ────────────────────────────────────────────────────

def test_capture_age_reason_old_enough():
    check(lib.capture_age_reason(300.0, 120.0) is None,
          "capture_age_reason: 300s of capture >= 120s minimum -> None (proceed)")


def test_capture_age_reason_too_young():
    # RED case: this is the reviewer's exact reproduction -- a run whose
    # AAS chart showed activity only in the last ~30s of a 15-minute
    # window.
    reason = lib.capture_age_reason(30.0, 120.0)
    check(reason is not None and "30s" in reason,
          f"capture_age_reason: 30s of capture < 120s minimum -> refuses (got {reason!r})")


def test_capture_age_reason_unmeasured_refuses():
    check(lib.capture_age_reason(None, 120.0) is not None,
          "capture_age_reason: capture start time not provided -> refuses, "
          "never assumed old enough")


def test_capture_age_reason_exactly_at_bound():
    check(lib.capture_age_reason(120.0, 120.0) is None,
          "capture_age_reason: exactly at the minimum counts (>=, not >)")


def test_filter_leak_reason_before_timeline_never_flagged():
    check(lib.filter_leak_reason(False, "›PID 22530 ✕") is None,
          "filter_leak_reason: a tab before/at Timeline is never flagged, "
          "even with filters showing (Timeline's own drill is intentional)")


def test_filter_leak_reason_clean_after_timeline():
    check(lib.filter_leak_reason(True, "") is None,
          "filter_leak_reason: empty breadcrumb after Timeline -> no leak")


def test_filter_leak_reason_whitespace_only_is_clean():
    check(lib.filter_leak_reason(True, "   \n") is None,
          "filter_leak_reason: whitespace-only breadcrumb text -> no leak")


def test_filter_leak_reason_detects_leak():
    reason = lib.filter_leak_reason(True, "›PID 22530 ✕")
    check(reason is not None and "PID 22530" in reason,
          "filter_leak_reason: non-empty breadcrumb after Timeline is a leak "
          f"(got {reason!r})")


def test_filter_leak_reason_unmeasured_refuses():
    reason = lib.filter_leak_reason(True, None)
    check(reason is not None,
          "filter_leak_reason: could-not-read breadcrumb after Timeline "
          "fails closed, never assumed clean "
          f"(got {reason!r})")


def test_navigate_to_tab_does_not_clear_drill_filter_inline():
    """Structural regression guard for a bug found on a real live run
    (pgwt-gate, 2026-09-28, round 3): navigate_to_tab() used to call
    clear_drill_filter() for the Timeline tab as its OWN last step --
    before its caller's render_check()/panel screenshot ran (both happen
    AFTER navigate_to_tab() returns). clearFilters() calls
    setTabView('overview'), so both ended up reading Overview's DOM
    instead of Timeline's: a real run showed render_detail: "no
    container" and no timeline/ttfp.png, despite navigate_to_tab itself
    having legitimately observed Timeline render (render_state: 'data',
    a measured ttfp_s) moments earlier in the SAME call. Demonstrated red
    against the pre-fix source (a local backup of the function body
    including the inline clear call) before this guard existed --
    restoring the inline call reproduces the failure this test exists to
    catch."""
    src_path = os.path.join(os.path.dirname(__file__), "ui_mac_walk_safari.py")
    src = open(src_path).read()
    start = src.index("def navigate_to_tab(")
    end = src.index("\ndef ", start + 1)
    body = src[start:end]
    check("clear_drill_filter(" not in body,
          "navigate_to_tab() must never call clear_drill_filter() itself -- "
          "only its caller may, once it is done observing Timeline "
          "(render_check/screenshot both run after navigate_to_tab returns)")


def test_version_skew_reason_none_is_ok():
    check(lib.version_skew_reason(None) is None,
          "version_skew_reason: no skew element measured -> not a failure "
          "(absence of the badge is not evidence of anything wrong)")


def test_version_skew_reason_warn_fails():
    reason = lib.version_skew_reason({"level": "warn", "text": "⚠ version skew"})
    check(reason is not None and "warn" in reason,
          f"version_skew_reason: a warn-level badge must be a failure (got {reason!r})")


def test_version_skew_reason_error_fails():
    reason = lib.version_skew_reason({"level": "error", "text": "boom"})
    check(reason is not None, f"version_skew_reason: error level fails too (got {reason!r})")


def test_gating_ok_all_none():
    check(lib.gating_ok(None, None, None) is True,
          "gating_ok: every reason None -> True")


def test_gating_ok_one_reason():
    check(lib.gating_ok(None, "something broke", None) is False,
          "gating_ok: any non-None reason -> False")


def test_classify_verdict_extra_fields_accepts_known():
    gating = lib.classify_verdict_extra_fields({
        "version_skew": {"level": "warn", "text": "x"},
        "branch_sha": "abc123",
    })
    check(gating == {"version_skew": {"level": "warn", "text": "x"}},
          f"classify_verdict_extra_fields: returns only the GATING subset (got {gating!r})")


def test_classify_verdict_extra_fields_rejects_unknown_field():
    """The core bypass this registry exists to close: a brand-new verdict
    field, attached without anyone deciding whether it gates `ok`, must be
    a hard error -- never silently treated as informational. This is
    exactly the shape version_skew shipped in before this fix."""
    try:
        lib.classify_verdict_extra_fields({"totally_new_field": 42})
        ok = False
    except ValueError as e:
        ok = "totally_new_field" in str(e)
    check(ok, "classify_verdict_extra_fields: an unclassified field must "
              "raise ValueError naming it, never pass through silently")


def _fake_verdict(status="satisfied", ok=True):
    return {
        "run": {"id": "x"}, "ok": ok, "note": "n", "tabs": {}, "tab_order": [],
        "void_tabs": [], "void_count": 0, "tab_count": 0, "counted": True,
        "mac_samples": [], "notes": [], "status": status,
    }


def test_finalize_verdict_gating_clean_run_stays_satisfied():
    v = _fake_verdict()
    base_keys = set(v)
    v["version_skew"] = None
    v["branch_sha"] = "abc"
    lib.finalize_verdict_gating(v, base_keys)
    check(v["ok"] is True and v["status"] == "satisfied",
          f"finalize_verdict_gating: no skew -> unchanged (got ok={v['ok']!r} "
          f"status={v['status']!r})")


def test_finalize_verdict_gating_skew_flips_ok_false():
    v = _fake_verdict()
    base_keys = set(v)
    v["version_skew"] = {"level": "warn", "text": "⚠ version skew"}
    lib.finalize_verdict_gating(v, base_keys)
    check(v["ok"] is False and v["status"] == "failed",
          "finalize_verdict_gating: version_skew warn must flip ok/status "
          f"(got ok={v['ok']!r} status={v['status']!r}) -- this is the exact "
          "live bug: status:'satisfied', ok:true alongside a warn skew badge")
    check(v["gating_reasons"]["version_skew"] is not None,
          "finalize_verdict_gating: the reason itself is recorded, not just the flip")


def test_finalize_verdict_gating_never_upgrades_unmeasured():
    v = _fake_verdict(status="unmeasured", ok=False)
    base_keys = set(v)
    v["version_skew"] = {"level": "warn", "text": "x"}
    lib.finalize_verdict_gating(v, base_keys)
    check(v["status"] == "unmeasured",
          "finalize_verdict_gating: must never turn an already-'unmeasured' "
          f"status into 'failed' (got {v['status']!r}) -- majority-void means "
          "'we could not tell', a claim finalize_verdict_gating must not "
          "overwrite with the stronger, more specific 'failed'")


def test_finalize_verdict_gating_rejects_unclassified_field():
    v = _fake_verdict()
    base_keys = set(v)
    v["mystery_field"] = "surprise"
    try:
        lib.finalize_verdict_gating(v, base_keys)
        ok = False
    except ValueError as e:
        ok = "mystery_field" in str(e)
    check(ok, "finalize_verdict_gating: an unclassified extra field on the "
              "REAL verdict dict must raise, same as calling "
              "classify_verdict_extra_fields directly -- this is what makes "
              "a future `verdict[\"x\"] = ...` line in ui_mac_walk_safari.py "
              "impossible to ship unclassified")


def test_walk_precondition_all_clear():
    check(lib.walk_precondition_reason(False, False) is None,
          "walk_precondition_reason: unlocked + awake -> None (walk may proceed)")


def test_walk_precondition_locked_refuses():
    reason = lib.walk_precondition_reason(True, False)
    check(reason is not None and "locked" in reason,
          f"walk_precondition_reason: locked -> refuses (got {reason!r})")


def test_walk_precondition_asleep_refuses():
    reason = lib.walk_precondition_reason(False, True)
    check(reason is not None and "asleep" in reason,
          f"walk_precondition_reason: asleep -> refuses (got {reason!r})")


def test_walk_precondition_unmeasured_refuses():
    # RED case: an unmeasured precondition (ioreg/system_profiler failed)
    # must refuse the walk, never silently proceed as if it were clear.
    check(lib.walk_precondition_reason(None, False) is not None,
          "walk_precondition_reason: lock state unmeasured -> refuses")
    check(lib.walk_precondition_reason(False, None) is not None,
          "walk_precondition_reason: display state unmeasured -> refuses")


# ── effective_zoom ────────────────────────────────────────────────────────

def test_effective_zoom_no_zoom():
    check(lib.effective_zoom(1710, 1710) == 1.0,
          "effective_zoom: achieved == target -> 1.0")


def test_effective_zoom_matches_measured_115_percent():
    z = lib.effective_zoom(1710, 1486)
    check(abs(z - 1.15) < 0.01,
          f"effective_zoom: 1710/1486 recovers ~1.15 (measured 115% Safari "
          f"zoom on 'localhost', 2026-09-28) (got {z})")


def test_effective_zoom_zero_achieved_is_none():
    # RED case: a genuinely broken viewport read (0) must not divide by
    # zero or report a fabricated zoom.
    check(lib.effective_zoom(1710, 0) is None,
          "effective_zoom: achieved width 0 -> None, not a crash or a lie")


def test_effective_zoom_none_achieved_is_none():
    check(lib.effective_zoom(1710, None) is None,
          "effective_zoom: achieved width None -> None")


def test_prepare_run_dir_two_runs_both_survive():
    """The core regression this round's fix closes (2026-09-28,
    coordinator's finding): a run used to wipe and reuse ONE shared
    directory, so a second run destroyed the first's verdict before
    anyone could compare them -- "two consecutive clean runs on the same
    tree" was reported to the owner and had to be retracted. Demonstrates
    the fix directly: two different run_ids must produce two directories
    that BOTH still exist and BOTH still hold their own file afterward."""
    container = tempfile.mkdtemp(prefix="pgwt-rundir-test-")
    try:
        dir1 = walk.prepare_run_dir(container, "1111111111")
        with open(os.path.join(dir1, "summary.json"), "w") as f:
            f.write('{"run": "one"}')
        dir2 = walk.prepare_run_dir(container, "2222222222")
        with open(os.path.join(dir2, "summary.json"), "w") as f:
            f.write('{"run": "two"}')
        check(dir1 != dir2, f"prepare_run_dir: different run_ids -> different "
                             f"directories (got {dir1!r} == {dir2!r})")
        check(os.path.isfile(os.path.join(dir1, "summary.json")),
              "prepare_run_dir: the FIRST run's file must still exist after "
              "the second run's prepare_run_dir call -- this is exactly the "
              "overwrite bug the fix closes")
        with open(os.path.join(dir1, "summary.json")) as f:
            check(f.read() == '{"run": "one"}',
                  "prepare_run_dir: the first run's content must be intact, "
                  "not truncated or replaced")
        check(os.path.isfile(os.path.join(dir2, "summary.json")),
              "prepare_run_dir: the second run's own file exists too")
    finally:
        shutil.rmtree(container, ignore_errors=True)


def test_prepare_run_dir_same_run_id_replaces_only_that_one():
    container = tempfile.mkdtemp(prefix="pgwt-rundir-test-")
    try:
        other_dir = walk.prepare_run_dir(container, "other-run")
        with open(os.path.join(other_dir, "marker"), "w") as f:
            f.write("untouched")
        d1 = walk.prepare_run_dir(container, "same-id")
        with open(os.path.join(d1, "stale.txt"), "w") as f:
            f.write("should be gone next call")
        d2 = walk.prepare_run_dir(container, "same-id")
        check(d1 == d2, "prepare_run_dir: same run_id -> same path")
        check(not os.path.exists(os.path.join(d2, "stale.txt")),
              "prepare_run_dir: re-preparing the SAME run_id clears that "
              "run's own stale content (an exact-marker re-run in dev)")
        check(os.path.isfile(os.path.join(other_dir, "marker")),
              "prepare_run_dir: a DIFFERENT run_id's directory is never "
              "touched by preparing this one")
    finally:
        shutil.rmtree(container, ignore_errors=True)


def test_update_latest_pointer_points_at_run_dir():
    container = tempfile.mkdtemp(prefix="pgwt-latest-test-")
    try:
        run_dir = walk.prepare_run_dir(container, "run-a")
        with open(os.path.join(run_dir, "summary.json"), "w") as f:
            f.write("a")
        ok = walk.update_latest_pointer(container, run_dir)
        latest = os.path.join(container, "latest")
        check(ok is True, "update_latest_pointer: succeeds on a normal filesystem")
        check(os.path.islink(latest), "update_latest_pointer: 'latest' is a symlink")
        check(os.path.realpath(latest) == os.path.realpath(run_dir),
              "update_latest_pointer: 'latest' resolves to the run directory "
              f"(got {os.path.realpath(latest)!r} != {os.path.realpath(run_dir)!r})")
        # A second run's own prepare+update must MOVE the pointer, never
        # leave it on the first run (which must itself remain untouched --
        # the whole point of this round's fix).
        run_dir_2 = walk.prepare_run_dir(container, "run-b")
        with open(os.path.join(run_dir_2, "summary.json"), "w") as f:
            f.write("b")
        walk.update_latest_pointer(container, run_dir_2)
        check(os.path.realpath(latest) == os.path.realpath(run_dir_2),
              "update_latest_pointer: a later run moves 'latest' to itself")
        check(os.path.isfile(os.path.join(run_dir, "summary.json")),
              "update_latest_pointer: moving 'latest' never touches the "
              "PREVIOUS run's own directory or file")
    finally:
        shutil.rmtree(container, ignore_errors=True)


def test_build_verdict_note_asserts_gate_not_measurement_only():
    """Locks in the coordinator's decision (2026-09-28): either this is a
    gate and the old 'measurement only -- not a ... gate' wording goes, or
    it stays informational and nobody may cite it as a gate. The decision
    made: it IS the gate for this walk, distinct from (not lesser than)
    docs/DEMO_REHEARSAL_CRITERIA.md's own gate, and it must explicitly
    disclaim visual correctness (issue #213 passed this walk green)."""
    v = lib.build_verdict("run-x", [], [])
    note = v["note"]
    check("measurement only" not in note,
          f"build_verdict note: the old ambiguous framing must be gone (got {note!r})")
    check("gate" in note.lower(),
          f"build_verdict note: must say plainly that this IS a gate (got {note!r})")
    check("visual correctness" in note.lower() or "visual" in note.lower(),
          f"build_verdict note: must disclaim visual correctness explicitly "
          f"(got {note!r}) -- a green run is not evidence a panel looks right")


if __name__ == "__main__":
    for name, fn in sorted(list(globals().items())):
        if name.startswith("test_") and callable(fn):
            fn()
    print(f"\n{tests_passed}/{tests_run} passed")
    sys.exit(0 if tests_failed == 0 else 1)
