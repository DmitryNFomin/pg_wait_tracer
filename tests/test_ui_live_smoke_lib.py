#!/usr/bin/env python3
"""test_ui_live_smoke_lib.py -- unit tests for tests/ui_live_smoke_lib.py
(issue #93). Pure Python + PIL/numpy (already required deps, see CLAUDE.md
"Local setup"); no Playwright, no browser, no network -- runs on the Mac as
part of `make check`.

Usage: python3 tests/test_ui_live_smoke_lib.py
"""
import inspect
import io
import os
import sys
import tempfile

import numpy as np
from PIL import Image

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import ui_live_smoke_lib as lib

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


# ── frame_diff_ratio / compare_png_files ────────────────────────────────────

def test_frame_diff_ratio_identical():
    a = np.zeros((10, 10, 3), dtype=np.uint8)
    b = a.copy()
    check(lib.frame_diff_ratio(a, b) == 0.0,
          "identical frames diff ratio == 0.0")


def test_frame_diff_ratio_partial():
    a = np.zeros((10, 10, 3), dtype=np.uint8)
    b = a.copy()
    # 5 of 100 pixels differ -> ratio 0.05
    b[0, 0:5] = 255
    ratio = lib.frame_diff_ratio(a, b)
    check(abs(ratio - 0.05) < 1e-9, f"5/100 differing pixels -> ratio 0.05 (got {ratio})")


def test_frame_diff_ratio_below_threshold():
    # 1 pixel out of 100*100 = 0.0001 = 0.01% < 0.1% threshold.
    a = np.zeros((100, 100, 3), dtype=np.uint8)
    b = a.copy()
    b[0, 0] = [1, 2, 3]
    ratio = lib.frame_diff_ratio(a, b)
    check(lib.no_blink_ok(ratio),
          f"single-pixel antialiasing diff ({ratio}) passes the 0.1% blink threshold")


def test_frame_diff_ratio_above_threshold():
    # 200 pixels out of 100*100 = 2% >> 0.1% threshold -- a real re-render.
    a = np.zeros((100, 100, 3), dtype=np.uint8)
    b = a.copy()
    b[0:2, :] = 255
    ratio = lib.frame_diff_ratio(a, b)
    check(not lib.no_blink_ok(ratio),
          f"2% differing pixels ({ratio}) fails the 0.1% blink threshold (blink detected)")


def test_frame_diff_ratio_shape_mismatch():
    a = np.zeros((10, 10, 3), dtype=np.uint8)
    b = np.zeros((10, 11, 3), dtype=np.uint8)
    try:
        lib.frame_diff_ratio(a, b)
        check(False, "shape mismatch raises ValueError")
    except ValueError:
        check(True, "shape mismatch raises ValueError")


def test_blink_check_matching_shapes():
    a = np.zeros((10, 10, 3), dtype=np.uint8)
    b = a.copy()
    b[0, 0] = 255
    ratio, note = lib.blink_check(a, b)
    check(abs(ratio - 0.01) < 1e-9 and note is None,
          f"blink_check on matching shapes behaves like frame_diff_ratio ({ratio}, {note})")


def test_blink_check_resized_panel_is_maximal_not_a_crash():
    # A real bug found against a real daemon: a table still growing rows
    # between two "steady state" screenshots resizes the panel element.
    # This must be reported as the worst possible blink ratio, never raise.
    a = np.zeros((10, 10, 3), dtype=np.uint8)
    b = np.zeros((12, 10, 3), dtype=np.uint8)
    ratio, note = lib.blink_check(a, b)
    check(ratio == 1.0 and note is not None and "resized" in note,
          f"a panel resize is reported as ratio=1.0 with an explanatory note ({ratio}, {note!r})")
    check(not lib.no_blink_ok(ratio),
          "a resized-panel ratio of 1.0 always fails the no_blink threshold")


# ── sweep_consecutive_diff_ratios / build_sweep_tick_record (issue #119) ────

def test_sweep_consecutive_diff_ratios_all_identical():
    frame = np.zeros((10, 10, 3), dtype=np.uint8)
    frames = [frame, frame.copy(), frame.copy(), frame.copy(), frame.copy()]
    pairs = lib.sweep_consecutive_diff_ratios(frames)
    check(len(pairs) == 4, f"5 frames -> 4 consecutive pairs (got {len(pairs)})")
    check(all(ratio == 0.0 and note is None for ratio, note in pairs),
          f"identical frames throughout the sweep -> every pair ratio 0.0 ({pairs})")


def test_sweep_consecutive_diff_ratios_detects_a_relayout_mid_sweep():
    a = np.zeros((10, 10, 3), dtype=np.uint8)
    b = a.copy()
    b[0, 0:5] = 255  # a real change between the 2nd and 3rd sweep frame
    frames = [a, a.copy(), a.copy(), b, b.copy()]
    pairs = lib.sweep_consecutive_diff_ratios(frames)
    ratios = [r for r, _ in pairs]
    check(ratios == [0.0, 0.0, 0.05, 0.0],
          f"a re-layout landing between two sweep offsets shows up exactly there ({ratios})")


def test_sweep_consecutive_diff_ratios_missing_frame_is_maximal_not_a_crash():
    a = np.zeros((10, 10, 3), dtype=np.uint8)
    frames = [a, None, a.copy()]
    pairs = lib.sweep_consecutive_diff_ratios(frames)
    check(len(pairs) == 2, "a missing frame does not raise or drop a pair")
    check(pairs[0] == (1.0, "frame missing from the sweep"),
          f"the pair touching the missing frame is ratio=1.0 with a note ({pairs[0]})")
    check(pairs[1] == (1.0, "frame missing from the sweep"),
          f"BOTH pairs touching a missing frame are flagged, not just one ({pairs[1]})")


def test_sweep_consecutive_diff_ratios_shape_mismatch_is_maximal():
    a = np.zeros((10, 10, 3), dtype=np.uint8)
    b = np.zeros((12, 10, 3), dtype=np.uint8)  # a panel resize mid-sweep
    pairs = lib.sweep_consecutive_diff_ratios([a, b])
    check(pairs[0][0] == 1.0 and "resized" in pairs[0][1],
          f"a panel resize mid-sweep is reported as ratio=1.0 with a note ({pairs[0]})")


def test_sweep_offsets_pinned():
    # CLAUDE.md: never widen/narrow what the issue specifies without saying so.
    check(lib.SWEEP_OFFSETS_MS == (200, 500, 1000, 1500, 2000),
          f"SWEEP_OFFSETS_MS matches issue #119's stated offsets exactly "
          f"(got {lib.SWEEP_OFFSETS_MS})")


def test_build_sweep_tick_record_shape():
    a = np.zeros((10, 10, 3), dtype=np.uint8)
    frames = [a, a.copy(), a.copy(), a.copy(), None]
    achieved = [201, 503, 998, 1501, 2010]
    rec = lib.build_sweep_tick_record(achieved, frames)
    check(rec["target_offsets_ms"] == [200, 500, 1000, 1500, 2000],
          f"target offsets carried through unchanged ({rec['target_offsets_ms']})")
    check(rec["achieved_offsets_ms"] == achieved,
          f"achieved offsets carried through unchanged ({rec['achieved_offsets_ms']})")
    check(len(rec["ratios"]) == 4, f"4 ratios for 5 frames ({rec['ratios']})")
    check(rec["ratios"][:3] == [0.0, 0.0, 0.0],
          f"identical frames give ratio 0.0 ({rec['ratios']})")
    check(rec["notes"] == ["frame missing from the sweep"],
          f"only the pair touching the missing 5th frame gets a note ({rec['notes']})")


def test_is_blank_frame_solid_colour():
    solid = np.full((20, 20, 3), 30, dtype=np.uint8)  # e.g. the dark theme bg
    check(lib.is_blank_frame(solid),
          "a perfectly solid-colour frame is blank")


def test_is_blank_frame_real_content():
    rng = np.zeros((20, 20, 3), dtype=np.uint8)
    # A few "content" pixels (text/grid/chart-line stand-in) is enough to
    # push real variance well above the blank threshold.
    rng[2:18, 2:4] = 200
    rng[5, :] = 255
    check(not lib.is_blank_frame(rng),
          "a frame with real content (not just background) is not blank")


def test_is_blank_frame_near_solid_antialiasing_noise_still_blank():
    # +/-1 antialiasing jitter on an otherwise flat background must not
    # trip the blank check -- it is not "real content".
    frame = np.full((20, 20, 3), 30, dtype=np.uint8)
    frame[0, 0] = 31
    frame[1, 1] = 29
    check(lib.is_blank_frame(frame),
          "tiny antialiasing-level noise on a flat background still reads as blank")


def test_compare_png_files_roundtrip():
    with tempfile.TemporaryDirectory() as d:
        a = np.zeros((20, 20, 3), dtype=np.uint8)
        b = a.copy()
        b[5, 5] = [10, 20, 30]  # 1 of 400 pixels = 0.25%
        pa, pb = os.path.join(d, "a.png"), os.path.join(d, "b.png")
        Image.fromarray(a).save(pa)
        Image.fromarray(b).save(pb)
        ratio = lib.compare_png_files(pa, pb)
        check(abs(ratio - 1 / 400) < 1e-9,
              f"PNG round-trip diff ratio matches the source arrays (got {ratio})")


def test_png_bytes_to_array_roundtrip():
    arr = np.zeros((8, 8, 3), dtype=np.uint8)
    arr[2, 2] = [200, 100, 50]
    buf = io.BytesIO()
    Image.fromarray(arr).save(buf, format="PNG")
    decoded = lib.png_bytes_to_array(buf.getvalue())
    check(np.array_equal(decoded, arr),
          "png_bytes_to_array decodes in-memory PNG bytes losslessly")


def test_blink_threshold_is_pinned_at_0_1_pct():
    # CLAUDE.md: never widen this beyond what the issue states.
    check(lib.BLINK_THRESHOLD == 0.001,
          f"BLINK_THRESHOLD stays pinned at 0.1% (got {lib.BLINK_THRESHOLD})")


# ── color_stability_violations ──────────────────────────────────────────────

def test_color_stability_stable():
    ticks = [
        {"IO": "rgb(1,2,3)", "Lock": "rgb(4,5,6)"},
        {"IO": "rgb(1,2,3)", "Lock": "rgb(4,5,6)", "CPU*": "rgb(7,8,9)"},
        {"IO": "rgb(1,2,3)"},
    ]
    check(lib.color_stability_violations(ticks) == [],
          "no violations when every name keeps its first-seen colour")


def test_color_stability_violation_detected():
    ticks = [
        {"IO": "rgb(1,2,3)"},
        {"IO": "rgb(9,9,9)"},  # IO changed color -> violation
    ]
    v = lib.color_stability_violations(ticks)
    check(len(v) == 1 and "IO" in v[0],
          f"a colour change for the same name is reported ({v})")


def test_color_stability_reappearance_after_rerank():
    # An event that leaves the top-N and comes back must still match its
    # ORIGINAL colour (docs/VISUAL_CHECKLIST.md STABILITY: rerank must not
    # reshuffle colours).
    ticks = [
        {"IO": "rgb(1,2,3)", "Lock": "rgb(4,5,6)"},
        {"Lock": "rgb(4,5,6)"},               # IO dropped out of top-N
        {"IO": "rgb(1,2,3)", "Lock": "rgb(4,5,6)"},  # IO back, same colour
    ]
    check(lib.color_stability_violations(ticks) == [],
          "an event that leaves and re-enters keeps its original colour")


# ── leak_probe_ok ────────────────────────────────────────────────────────────

def test_leak_probe_ok_settled():
    check(lib.leak_probe_ok({"charts": 1, "uplots": 1, "pending": 0}),
          "settled probe (1 chart, 1 uplot, 0 pending) is OK")


def test_leak_probe_ok_table_tab():
    check(lib.leak_probe_ok({"charts": 0, "uplots": 1, "pending": 0}),
          "no-chart table tab probe (0 charts, 1 uplot) is OK")


def test_leak_probe_detects_chart_leak():
    check(not lib.leak_probe_ok({"charts": 12, "uplots": 1, "pending": 0}),
          "a growing chart count is detected as a leak")


def test_leak_probe_detects_pending_leak():
    check(not lib.leak_probe_ok({"charts": 1, "uplots": 1, "pending": 3}),
          "pending transport requests left after settle is detected as a leak")


# ── render_check_ok ──────────────────────────────────────────────────────────

def test_render_check_ok_string_success():
    ok, detail = lib.render_check_ok("ok:42")
    check(ok is True and detail == "ok:42", "'ok:N' string parses as success")


def test_render_check_ok_string_failure():
    ok, detail = lib.render_check_ok("no data")
    check(ok is False and detail == "no data", "'no ...' string parses as failure")


def test_render_check_ok_dict():
    ok, detail = lib.render_check_ok({"ok": True, "detail": "3 rows"})
    check(ok is True and detail == "3 rows", "dict result parses ok+detail")


def test_render_check_ok_unexpected():
    ok, detail = lib.render_check_ok(None)
    check(ok is False and "unexpected" in detail,
          "an unexpected (None) result is treated as a failure, not a crash")


# ── build_tab_result / build_summary ────────────────────────────────────────

def test_build_tab_result_all_green():
    r = lib.build_tab_result(
        "overview", True, "ok:8", ticks_observed=6, console_errors=[],
        blink_ratio=0.0001, color_violations=[],
        leak_before={"charts": 1, "uplots": 1, "pending": 0},
        leak_after={"charts": 1, "uplots": 1, "pending": 0},
        artifacts={"frames": ["tick-1.png"], "video": "overview.webm"})
    check(r["ok"] is True, "all-green tab result is ok=True")
    check(r["tab"] == "overview", "tab id carried through")


def test_build_tab_result_flags_blink():
    r = lib.build_tab_result(
        "events", True, "ok:8", ticks_observed=6, console_errors=[],
        blink_ratio=0.05, color_violations=[],
        leak_before={"charts": 1, "uplots": 1, "pending": 0},
        leak_after={"charts": 1, "uplots": 1, "pending": 0},
        artifacts={})
    check(r["ok"] is False and r["no_blink"]["ok"] is False,
          "a 5% blink ratio fails the tab even though everything else is clean")


def test_build_tab_result_flags_insufficient_ticks():
    r = lib.build_tab_result(
        "events", True, "ok:8", ticks_observed=2, console_errors=[],
        blink_ratio=0.0, color_violations=[],
        leak_before={"charts": 1, "uplots": 1, "pending": 0},
        leak_after={"charts": 1, "uplots": 1, "pending": 0},
        artifacts={})
    check(r["ok"] is False,
          "fewer than MIN_TICKS ticks observed fails the tab (never silently short)")


def test_build_tab_result_records_pgwt_errors_without_failing():
    # '[pgwt]'-prefixed console errors are the app's OWN failure reporting --
    # they must be visible in summary.json, but never fail the tab by
    # themselves (issue #93 fail-safe/correctness review item 3).
    r = lib.build_tab_result(
        "overview", True, "ok:8", ticks_observed=6, console_errors=[],
        blink_ratio=0.0, color_violations=[],
        leak_before={"charts": 1, "uplots": 1, "pending": 0},
        leak_after={"charts": 1, "uplots": 1, "pending": 0},
        artifacts={}, pgwt_console_errors=["console: [pgwt] view build failed: x"])
    check(r["ok"] is True and r["clean"]["ok"] is True,
          f"a [pgwt] error alone does not fail the tab ({r['clean']})")
    check(r["clean"]["pgwt_errors"] == ["console: [pgwt] view build failed: x"],
          f"the [pgwt] error is recorded, not dropped ({r['clean']['pgwt_errors']})")


def test_build_tab_result_records_leak_settle_duration():
    r = lib.build_tab_result(
        "overview", True, "ok:8", ticks_observed=6, console_errors=[],
        blink_ratio=0.0, color_violations=[],
        leak_before={"charts": 1, "uplots": 1, "pending": 0},
        leak_after={"charts": 1, "uplots": 1, "pending": 0},
        artifacts={}, leak_before_settle_s=0.4, leak_after_settle_s=25.3)
    check(r["no_leak"]["settle_s"] == {"before": 0.4, "after": 25.3},
          f"leak-probe settle durations are recorded per tab ({r['no_leak']['settle_s']})")


def test_build_tab_result_records_blink_pair_offsets():
    # issue #93 review item 3: the achieved blink-pair capture offset
    # (now_ms - tick_ts_ms) per tick must be visible in summary.json, so a
    # future overrun of the 1200ms tick-anchored settle is not silent.
    r = lib.build_tab_result(
        "timeline", True, "ok:8", ticks_observed=6, console_errors=[],
        blink_ratio=0.0, color_violations=[],
        leak_before={"charts": 1, "uplots": 1, "pending": 0},
        leak_after={"charts": 1, "uplots": 1, "pending": 0},
        artifacts={}, blink_pair_offsets_ms=[1201, 1198, 1350, 1199, 1205, 1200])
    check(r["no_blink"]["pair_offsets_ms"] == [1201, 1198, 1350, 1199, 1205, 1200],
          f"per-tick achieved blink-pair offsets recorded ({r['no_blink']['pair_offsets_ms']})")
    check(r["ok"] is True,
          "an occasional overrun (1350ms here) is visible, not itself a failure")


def test_build_tab_result_blink_pair_offsets_default_empty():
    r = lib.build_tab_result(
        "overview", True, "ok:8", ticks_observed=6, console_errors=[],
        blink_ratio=0.0, color_violations=[],
        leak_before={"charts": 1, "uplots": 1, "pending": 0},
        leak_after={"charts": 1, "uplots": 1, "pending": 0},
        artifacts={})
    check(r["no_blink"]["pair_offsets_ms"] == [],
          "blink_pair_offsets_ms defaults to an empty list, not missing/None")


def test_build_failed_tab_result_pair_offsets_empty():
    r = lib.build_failed_tab_result("waterfall", "panel did not render within 60s")
    check(r["no_blink"]["pair_offsets_ms"] == [],
          "a tab that never reached the tick loop has no offsets to report")


def test_build_tab_result_records_blink_sweep():
    a = np.zeros((10, 10, 3), dtype=np.uint8)
    tick_rec = lib.build_sweep_tick_record(
        [201, 503, 998, 1501, 2010], [a, a.copy(), a.copy(), a.copy(), a.copy()])
    r = lib.build_tab_result(
        "timeline", True, "ok:8", ticks_observed=6, console_errors=[],
        blink_ratio=0.0, color_violations=[],
        leak_before={"charts": 1, "uplots": 1, "pending": 0},
        leak_after={"charts": 1, "uplots": 1, "pending": 0},
        artifacts={}, blink_sweep_ticks=[tick_rec])
    check(r["blink_sweep"]["offsets_ms"] == [200, 500, 1000, 1500, 2000],
          f"blink_sweep records the nominal target offsets ({r['blink_sweep']})")
    check(r["blink_sweep"]["ticks"] == [tick_rec],
          f"blink_sweep records one entry per tick, unmodified ({r['blink_sweep']})")
    check(r["ok"] is True,
          "the sweep never affects the gating verdict (only the anchored pair does)")


def test_build_tab_result_blink_sweep_default_empty():
    r = lib.build_tab_result(
        "overview", True, "ok:8", ticks_observed=6, console_errors=[],
        blink_ratio=0.0, color_violations=[],
        leak_before={"charts": 1, "uplots": 1, "pending": 0},
        leak_after={"charts": 1, "uplots": 1, "pending": 0},
        artifacts={})
    check(r["blink_sweep"] == {"offsets_ms": [200, 500, 1000, 1500, 2000], "ticks": []},
          f"blink_sweep_ticks defaults to an empty tick list, not missing ({r['blink_sweep']})")


def test_build_failed_tab_result_blink_sweep_empty():
    r = lib.build_failed_tab_result("waterfall", "panel did not render within 60s")
    check(r["blink_sweep"] == {"offsets_ms": [200, 500, 1000, 1500, 2000], "ticks": []},
          f"a could-not-check tab still reports the sweep schema, with no ticks ({r['blink_sweep']})")


def test_build_failed_tab_result_records_pgwt_errors():
    r = lib.build_failed_tab_result(
        "waterfall", "panel did not render within 60s",
        pgwt_console_errors=["console: [pgwt] escalate failed"])
    check(r["clean"]["pgwt_errors"] == ["console: [pgwt] escalate failed"],
          f"a could-not-check tab still records any [pgwt] errors seen ({r['clean']})")


def test_build_summary_ok_requires_every_tab():
    good = lib.build_tab_result(
        "overview", True, "ok:1", 6, [], 0.0, [],
        {"charts": 1, "uplots": 1, "pending": 0},
        {"charts": 1, "uplots": 1, "pending": 0}, {})
    bad = lib.build_tab_result(
        "events", False, "no rows", 6, [], 0.0, [],
        {"charts": 1, "uplots": 1, "pending": 0},
        {"charts": 1, "uplots": 1, "pending": 0}, {})
    s = lib.build_summary([good, bad])
    check(s["ok"] is False and s["failed_tabs"] == ["events"],
          f"one failing tab fails the whole summary ({s['failed_tabs']})")

    s2 = lib.build_summary([good])
    check(s2["ok"] is True and s2["failed_tabs"] == [],
          "all-passing summary is ok=True with no failed tabs")


def test_build_summary_empty_is_not_ok():
    # An empty result list must never report ok=True -- that is exactly the
    # "all-skip run is green" failure mode run_all.sh guards against (TST-4).
    s = lib.build_summary([])
    check(s["ok"] is False, "an empty tab-result list is never reported ok")


def test_build_failed_tab_result():
    r = lib.build_failed_tab_result("matrix", "no data within 60s", ticks_observed=0)
    check(r["ok"] is False and r["tab"] == "matrix" and
          r["rendered"]["detail"] == "no data within 60s",
          f"a could-not-check tab is reported failed with its reason ({r})")
    s = lib.build_summary([r])
    check(s["ok"] is False and s["failed_tabs"] == ["matrix"],
          "a failed-to-check tab fails the overall summary too")


def test_known_failing_tabs_pinned():
    # Owner-filed tracking issues -- pinned exactly so nothing else quietly
    # gets added to this dict, and so DELISTING one stays a deliberate edit
    # here too. #101 (Waterfall) was delisted once its single root cause was
    # found and fixed: the tab defaulted to the newest execution, which on a
    # real capture has no events, no workers and no plan, so the panel never
    # mounted a chart (web/static/lib/builders/waterfall.js).
    check(lib.KNOWN_FAILING_TABS == {"timeline": 100},
          f"KNOWN_FAILING_TABS is exactly {{'timeline': 100}} "
          f"(got {lib.KNOWN_FAILING_TABS})")


def test_known_failing_issue():
    check(lib.known_failing_issue("timeline") == 100, "timeline -> issue #100")
    check(lib.known_failing_issue("waterfall") is None,
          "waterfall is delisted (#101 fixed) and has no known-failing issue")
    check(lib.known_failing_issue("overview") is None,
          "an unlisted tab has no known-failing issue")


def test_known_failing_report_line():
    check(lib.known_failing_report_line("overview", False) is None,
          "an unlisted tab never gets a known-failing report line")
    fail_line = lib.known_failing_report_line("timeline", False)
    check(fail_line == "KNOWN-FAILING (issue #100)",
          f"a listed tab's real failure reports KNOWN-FAILING ({fail_line!r})")
    pass_line = lib.known_failing_report_line("timeline", True)
    check(pass_line == "UNEXPECTED PASS (issue #100) -- intermittent or fixed; check the issue",
          f"a listed tab's real pass reports UNEXPECTED PASS ({pass_line!r})")


def test_build_tab_result_known_failing_does_not_fail_summary():
    # Timeline (#100) actually failing (raw ok=False): excused from the
    # overall verdict, but the raw failure and the known_failing flag are
    # both visible in the tab's own record.
    r = lib.build_tab_result(
        "timeline", True, "ok:1", 6, [], 0.05, [],  # 5% blink -> raw fail
        {"charts": 1, "uplots": 1, "pending": 0},
        {"charts": 1, "uplots": 1, "pending": 0}, {})
    check(r["ok"] is False, "the raw per-tab result still says what really happened")
    check(r["known_failing"] is True and r["xpass"] is False,
          f"a real failure on a listed tab is known_failing, not xpass ({r})")
    s = lib.build_summary([r])
    check(s["ok"] is True and s["failed_tabs"] == [] and
          s["known_failing_tabs"] == ["timeline"],
          f"a known-failing tab's real failure does not fail the summary ({s})")


def test_build_tab_result_xpass_does_not_fail_summary_either():
    # A listed tab (#100 Timeline) happening to pass this run: reported as
    # xpass, not silently absorbed, but does not fail the run -- same
    # semantics run_all.sh's own KNOWN_FAILING now uses too (an unexpected
    # pass is reported, never a gate failure by itself; a single real-daemon
    # run passing isn't proof an intermittent bug is fixed).
    r = lib.build_tab_result(
        "timeline", True, "ok:1", 6, [], 0.0, [],
        {"charts": 1, "uplots": 1, "pending": 0},
        {"charts": 1, "uplots": 1, "pending": 0}, {})
    check(r["ok"] is True and r["xpass"] is True and r["known_failing"] is False,
          f"a real pass on a listed tab is xpass, not known_failing ({r})")
    s = lib.build_summary([r])
    check(s["ok"] is True and s["xpass_tabs"] == ["timeline"],
          f"an xpass tab does not fail the summary either ({s})")


def test_build_failed_tab_result_known_failing():
    # A listed tab that could not be checked at all goes through
    # build_failed_tab_result, not build_tab_result, and must still be
    # excused by its issue number.
    r = lib.build_failed_tab_result(
        "timeline", "panel did not render ('#timeline-chart canvas') within 60s")
    check(r["known_failing"] is True and r["ok"] is False,
          f"a could-not-check known-failing tab is still known_failing ({r})")
    s = lib.build_summary([r])
    check(s["ok"] is True, "a known-failing could-not-check tab does not fail the summary")


def test_delisted_waterfall_failure_is_a_real_failure():
    # issue #101 regression guard: the waterfall tab is no longer excused, so
    # its old failure shape must fail the run outright. A return of the
    # empty-panel bug cannot slip through as "known failing" again.
    r = lib.build_failed_tab_result(
        "waterfall", "panel did not render ('#waterfall-chart canvas') within 60s")
    check(r["known_failing"] is False and r["ok"] is False,
          f"a waterfall render failure is a real failure again ({r})")
    s = lib.build_summary([r])
    check(s["ok"] is False and s["failed_tabs"] == ["waterfall"],
          f"a waterfall render failure fails the summary ({s})")


def test_known_failing_does_not_affect_unlisted_tabs():
    r = lib.build_tab_result(
        "overview", True, "ok:1", 6, [], 0.0, [],
        {"charts": 1, "uplots": 1, "pending": 0},
        {"charts": 1, "uplots": 1, "pending": 0}, {})
    check(r["known_failing"] is False and r["xpass"] is False,
          "an unlisted tab always has known_failing=False, xpass=False")


def test_build_summary_mixed_known_failing_and_real_failure():
    # A KNOWN_FAILING tab failing must not mask a genuine, unlisted failure.
    known = lib.build_failed_tab_result("timeline", "blink 4.46%")
    real_fail = lib.build_failed_tab_result("overview", "no rows")
    s = lib.build_summary([known, real_fail])
    check(s["ok"] is False and s["failed_tabs"] == ["overview"] and
          s["known_failing_tabs"] == ["timeline"],
          f"a real failure still fails the summary alongside an excused one ({s})")


def test_reset_output_dir_removes_stale_artifacts():
    # The ui-reviewer blocker this pins: a stale tick-1.png from an earlier
    # run must never survive into a later run's output directory.
    with tempfile.TemporaryDirectory() as d:
        out_dir = os.path.join(d, "ui_live")
        stale_tab_dir = os.path.join(out_dir, "waterfall")
        os.makedirs(stale_tab_dir)
        stale_file = os.path.join(stale_tab_dir, "tick-1.png")
        with open(stale_file, "wb") as f:
            f.write(b"stale")
        check(os.path.exists(stale_file), "stale artifact exists before reset (setup)")

        lib.reset_output_dir(out_dir)

        check(os.path.isdir(out_dir), "reset_output_dir leaves the directory present")
        check(not os.path.exists(stale_file),
              "a stale artifact from an earlier run does not survive reset_output_dir")
        check(os.listdir(out_dir) == [],
              f"the directory is empty right after reset (got {os.listdir(out_dir)})")


def test_reset_output_dir_first_call_no_preexisting_dir():
    # Also works when out_dir does not exist yet (first-ever run).
    with tempfile.TemporaryDirectory() as d:
        out_dir = os.path.join(d, "never_existed", "ui_live")
        lib.reset_output_dir(out_dir)
        check(os.path.isdir(out_dir),
              "reset_output_dir creates the directory when it did not exist")


def test_write_summary_roundtrip():
    import json
    with tempfile.TemporaryDirectory() as d:
        good = lib.build_tab_result(
            "overview", True, "ok:1", 6, [], 0.0, [],
            {"charts": 1, "uplots": 1, "pending": 0},
            {"charts": 1, "uplots": 1, "pending": 0}, {})
        path = os.path.join(d, "nested", "summary.json")
        written = lib.write_summary(path, [good])
        with open(path) as f:
            on_disk = json.load(f)
        check(on_disk == written,
              "write_summary() writes exactly what build_summary() returned")


def _discover_tests():
    """Every callable named test_* defined at module level, in declaration
    order (by source line). Replaces a hand-maintained TESTS list (issue #93
    review: two tests were defined but never added to it, so they silently
    never ran) -- this whole class of drift cannot recur since a new
    test_* function is picked up automatically."""
    found = [obj for name, obj in list(globals().items())
             if name.startswith("test_") and inspect.isfunction(obj)]
    found.sort(key=lambda fn: inspect.getsourcelines(fn)[1])
    return found


TESTS = _discover_tests()


def main():
    print(f"discovered {len(TESTS)} test functions")
    for t in TESTS:
        print(f"--- {t.__name__} ---")
        t()
    print()
    print(f"Ran {tests_run}, passed {tests_passed}, failed {tests_failed} "
          f"({len(TESTS)} test functions discovered)")
    return 0 if tests_failed == 0 else 1


if __name__ == "__main__":
    sys.exit(main())
