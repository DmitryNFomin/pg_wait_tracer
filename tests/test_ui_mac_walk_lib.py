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
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import ui_mac_walk_lib as lib

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


# ── window_void_reason: the attribution gate ─────────────────────────────

def test_window_void_reason_all_normal():
    samples = [{"ts": 10, "pressure_level": 1}, {"ts": 12, "pressure_level": 1}]
    check(lib.window_void_reason(samples, 9, 13) is None,
          "window_void_reason: all-normal window -> None (not void)")


def test_window_void_reason_one_bad():
    samples = [{"ts": 10, "pressure_level": 1}, {"ts": 12, "pressure_level": 2}]
    reason = lib.window_void_reason(samples, 9, 13)
    check(reason is not None and "1/2" in reason,
          f"window_void_reason: one non-normal sample voids the window (got: {reason!r})")


def test_window_void_reason_empty_window_is_void():
    # RED-demonstrated bypass case: no samples landed in [start,end] at all
    # (sampler never ran, or the window was shorter than the sample
    # interval). A gate that skipped this check would silently certify an
    # unmeasured window as clean -- exactly the "check that accepted
    # evidence gathered on a stale/absent tree" failure shape.
    samples = [{"ts": 100, "pressure_level": 1}]
    reason = lib.window_void_reason(samples, 0, 5)
    check(reason is not None and "no mac samples" in reason,
          f"window_void_reason: empty window is void, not silently clean (got: {reason!r})")


def test_window_void_reason_unmeasured_sample_voids():
    samples = [{"ts": 10, "pressure_level": None}]
    reason = lib.window_void_reason(samples, 9, 11)
    check(reason is not None and "unmeasured" in reason,
          f"window_void_reason: an unmeasured sample voids the window too (got: {reason!r})")


def test_window_void_reason_boundary_inclusive():
    samples = [{"ts": 5, "pressure_level": 1}, {"ts": 10, "pressure_level": 1}]
    check(lib.window_void_reason(samples, 5, 10) is None,
          "window_void_reason: samples exactly on the window boundary count as in-window")


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


if __name__ == "__main__":
    for name, fn in sorted(list(globals().items())):
        if name.startswith("test_") and callable(fn):
            fn()
    print(f"\n{tests_passed}/{tests_run} passed")
    sys.exit(0 if tests_failed == 0 else 1)
