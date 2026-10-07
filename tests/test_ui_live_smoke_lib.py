#!/usr/bin/env python3
"""test_ui_live_smoke_lib.py -- unit tests for tests/ui_live_smoke_lib.py
(issue #93). Pure Python + PIL/numpy (already required deps, see CLAUDE.md
"Local setup"); no Playwright, no browser, no network -- runs on the Mac as
part of `make check`.

Usage: python3 tests/test_ui_live_smoke_lib.py
"""
import base64
import inspect
import io
import os
import sys
import tempfile
from contextlib import contextmanager

import numpy as np
from PIL import Image

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import ui_live_smoke_lib as lib


@contextmanager
def known_failing_tabs_override(fake):
    """Temporarily replaces lib.KNOWN_FAILING_TABS so the known-failing
    MECHANISM (known_failing_issue/known_failing_report_line/
    _apply_known_failing/build_summary's exemption) has its own test
    coverage independent of which real tabs happen to be listed right now.
    Both #101 and #100 (review round 5) were delisted; a test that keeps
    reaching into the REAL dict for an example entry breaks every time the
    dict legitimately changes, which is exactly the kind of coupling that
    made this delisting round more churn than it needed to be."""
    original = lib.KNOWN_FAILING_TABS
    lib.KNOWN_FAILING_TABS = fake
    try:
        yield
    finally:
        lib.KNOWN_FAILING_TABS = original

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


# ── frame_diff_signature (issue #100) ───────────────────────────────────────

def test_frame_diff_signature_window_pan_pattern():
    # Synthetic stand-in for timeline's own window-advance repaint: the
    # BOTTOM band (axis labels + axis line) changes heavily, the rest of
    # the panel is untouched, and only a few COLUMNS (thin vertical
    # gridlines) differ.
    h, w = 200, 400
    a = np.zeros((h, w, 3), dtype=np.uint8)
    b = a.copy()
    band_h = int(round(h * 0.15))
    b[h - band_h:, 50:120] = 255      # bottom-band label text, ~18% of width
    b[:, 10] = 255                    # one gridline shifted (whole column)
    b[:, 200] = 255                   # a second gridline shifted
    sig = lib.frame_diff_signature(a, b)
    check(sig["note"] is None, f"matching shapes -> no note ({sig})")
    check(sig["bottom_band_pixel_frac"] > sig["rest_pixel_frac"],
          f"bottom band differs far more than the rest ({sig})")
    check(sig["columns_touched_frac"] < 0.35,
          f"only a modest fraction of columns touched, not most of them ({sig})")


def test_frame_diff_signature_uniform_repaint_pattern():
    # A generic full repaint (e.g. genuinely new content): diff spread
    # roughly evenly, most columns touched -- the shape frame_diff_signature
    # must be able to tell apart from the window-pan pattern above.
    h, w = 200, 400
    a = np.zeros((h, w, 3), dtype=np.uint8)
    b = np.full((h, w, 3), 255, dtype=np.uint8)  # everything differs
    sig = lib.frame_diff_signature(a, b)
    check(abs(sig["bottom_band_pixel_frac"] - sig["rest_pixel_frac"]) < 1e-9,
          f"a uniform repaint diffs the bottom band and the rest equally ({sig})")
    check(sig["columns_touched_frac"] == 1.0,
          f"a uniform repaint touches every column ({sig})")


def test_frame_diff_signature_identical_frames():
    a = np.zeros((50, 50, 3), dtype=np.uint8)
    sig = lib.frame_diff_signature(a, a.copy())
    check(sig == {"diff_pixel_frac": 0.0, "bottom_band_pixel_frac": 0.0,
                   "rest_pixel_frac": 0.0, "columns_touched_frac": 0.0, "note": None},
          f"identical frames -> every fraction 0.0, no note ({sig})")


def test_frame_diff_signature_shape_mismatch_never_raises():
    a = np.zeros((10, 10, 3), dtype=np.uint8)
    b = np.zeros((12, 10, 3), dtype=np.uint8)
    sig = lib.frame_diff_signature(a, b)
    check(sig["note"] is not None and "resized" in sig["note"],
          f"a shape mismatch is reported via note, never a raised exception ({sig})")
    check(sig["diff_pixel_frac"] is None and sig["bottom_band_pixel_frac"] is None,
          f"every numeric field is None on a shape mismatch -- never a fabricated 0.0 ({sig})")


# ── pre_mount_diagnostic_verdict (issue #100, review round 5) ──────────────
#
# Round 4's bug, reproduced here: it asserted the pre-mount capture
# preceded the tick's own mount instead of proving it, and a real run
# proved that assumption false on two ticks (5, 6) -- this probe read
# diff_pixel_frac 0.0 on both while a plain consecutive-frame diff of the
# SAME run's own saved PNGs showed a genuine ~1.2% window-advance repaint.
# These tests pin the fix: the verdict must refuse to report a real
# signature unless precedence is actually established.

def test_pre_mount_diagnostic_verdict_precedence_established_computes_signature():
    a = np.zeros((20, 20, 3), dtype=np.uint8)
    b = a.copy()
    b[15:, :] = 255  # a real change in the bottom band
    sig = lib.pre_mount_diagnostic_verdict(a, b, pre_mount_seq_after=4, mount_seq=5)
    check(sig["note"] is None and sig["diff_pixel_frac"] is not None,
          f"seq_after(4) < mount_seq(5) -- precedence established, a real signature is computed ({sig})")


def test_pre_mount_diagnostic_verdict_no_mount_observed_yet_precedes():
    a = np.zeros((20, 20, 3), dtype=np.uint8)
    sig = lib.pre_mount_diagnostic_verdict(a, a.copy(), pre_mount_seq_after=None, mount_seq=1)
    check(sig["note"] is None and sig["diff_pixel_frac"] == 0.0,
          f"no mount observed anywhere yet trivially precedes the first one ({sig})")


def test_pre_mount_diagnostic_verdict_reproduces_the_round_4_bug_and_is_now_caught():
    # The exact shape of the round-4 failure: the "pre-mount" screenshot
    # was actually taken AFTER this tick's own mount already landed
    # (pre_mount_seq_after == mount_seq, the tick's mount already fired by
    # the time of the after-read) -- must now be NOT MEASURED, never a
    # fabricated 0.0, even though the two frames handed in are pixel-
    # identical (the case round 4's code would have silently accepted).
    a = np.zeros((20, 20, 3), dtype=np.uint8)
    sig = lib.pre_mount_diagnostic_verdict(a, a.copy(), pre_mount_seq_after=5, mount_seq=5)
    check(sig["diff_pixel_frac"] is None,
          f"seq_after(5) >= mount_seq(5) -- precedence NOT established, no signature computed ({sig})")
    check(sig["note"] is not None and "did not precede" in sig["note"],
          f"...with an explanatory note ({sig!r})")


def test_pre_mount_diagnostic_verdict_seq_after_ahead_of_mount_is_also_not_measured():
    # A later mount than the one we're testing against already happened by
    # the after-read -- even further past precedence than the exact-match
    # case above, must also be NOT MEASURED.
    a = np.zeros((20, 20, 3), dtype=np.uint8)
    sig = lib.pre_mount_diagnostic_verdict(a, a.copy(), pre_mount_seq_after=9, mount_seq=5)
    check(sig["diff_pixel_frac"] is None,
          f"seq_after(9) > mount_seq(5) -- also not measured ({sig})")


def test_pre_mount_diagnostic_verdict_missing_frame_is_not_measured_not_a_crash():
    sig = lib.pre_mount_diagnostic_verdict(None, None, pre_mount_seq_after=1, mount_seq=5)
    check(sig["diff_pixel_frac"] is None and sig["note"] is not None,
          f"a missing frame is not measured, never a crash ({sig})")


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


# ── sweep-vs-cadence coverage-floor guard (issue #193 review round 3) ──────
#
# blink_sweep_gate_verdict correctly reports NOT MEASURED (never a false
# red) when a second tick's mount lands mid-sweep. But if the sweep's own
# worst-case completion time ever gets as long as the gap between two
# consecutive mounts, ticks start being discarded for a CADENCE reason, and
# MIN_MEASURED_FRACTION starts failing tabs for nothing wrong with the
# product. These two numbers are the tightest margin actually observed
# (tests/results/ui_live/summary.json, one ephemeral box-check run, issue
# #193, timeline tab -- the worst of the 11):
#   MIN_OBSERVED_MOUNT_GAP_MS = 4982   -- the smallest gap between two
#     consecutive mounts (derived from consecutive pair_offsets_ms deltas
#     against the nominal 5000ms tick interval: tick 6's offset (48ms) was
#     18ms less than tick 5's (66ms), i.e. tick 6's mount landed 4982ms,
#     not the nominal 5000ms, after tick 5's).
#   MAX_OBSERVED_SWEEP_OFFSET_MS = 3738 -- tick 4's own sweep, its slowest
#     capture: SWEEP_OFFSETS_MS's last target (2000ms) landed at 3738ms
#     under real render-check-retry-loop contention.
# Slack that run: 4982 - 3738 = 1244ms.
MIN_OBSERVED_MOUNT_GAP_MS = 4982
MAX_OBSERVED_SWEEP_OFFSET_MS = 3738

# A conservative stand-in for one capture's own screenshot+PNG-decode
# overhead, NOT itself a live measurement -- multiplied by the sweep's
# actual capture count, this is what makes the guard below fail on a
# FUTURE SWEEP_OFFSETS_MS widening or TICK_INTERVAL_S reduction before it
# ever reaches a real gate, rather than only after a live run happens to
# reproduce contention as bad as the 3738ms above.
PER_CAPTURE_BUDGET_MS = 400


def test_sweep_slack_observed_this_run_is_positive():
    slack_ms = MIN_OBSERVED_MOUNT_GAP_MS - MAX_OBSERVED_SWEEP_OFFSET_MS
    check(slack_ms == 1244,
          f"observed slack (min mount gap {MIN_OBSERVED_MOUNT_GAP_MS}ms - max "
          f"achieved sweep offset {MAX_OBSERVED_SWEEP_OFFSET_MS}ms) is 1244ms "
          f"(got {slack_ms}ms) -- pins the number itself, not just its sign")
    check(slack_ms > 0,
          "the sweep did not, in fact, run into the next tick's mount this run")


def test_sweep_worst_case_completion_has_margin_below_the_observed_mount_gap():
    """The forward-looking guard: reads SWEEP_OFFSETS_MS and TICK_INTERVAL_S
    LIVE from the module (so a future change to either is exercised here,
    in make check-fast), against the PINNED worst-case cadence jitter
    actually observed (MIN_OBSERVED_MOUNT_GAP_MS above). A future
    SWEEP_OFFSETS_MS widening or TICK_INTERVAL_S reduction that erodes this
    margin must fail HERE, not on the gate at demo time."""
    tick_interval_ms = lib.TICK_INTERVAL_S * 1000
    worst_observed_mount_delay_ms = tick_interval_ms - MIN_OBSERVED_MOUNT_GAP_MS
    worst_case_sweep_completion_ms = (
        max(lib.SWEEP_OFFSETS_MS) +
        len(lib.SWEEP_OFFSETS_MS) * PER_CAPTURE_BUDGET_MS)
    limit_ms = tick_interval_ms - worst_observed_mount_delay_ms
    margin_ms = limit_ms - worst_case_sweep_completion_ms
    check(worst_case_sweep_completion_ms < limit_ms,
          f"sweep's worst-case completion ({worst_case_sweep_completion_ms}ms = "
          f"max(SWEEP_OFFSETS_MS)={max(lib.SWEEP_OFFSETS_MS)}ms + "
          f"{len(lib.SWEEP_OFFSETS_MS)}x{PER_CAPTURE_BUDGET_MS}ms budget) stays "
          f"below the tick interval ({tick_interval_ms}ms) minus the worst "
          f"observed mount delay ({worst_observed_mount_delay_ms}ms) = "
          f"{limit_ms}ms -- margin {margin_ms}ms")


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


def test_build_sweep_tick_record_records_capture_ms_and_mount_seq():
    a = np.zeros((4, 4, 3), dtype=np.uint8)
    frames = [a, a.copy()]
    rec = lib.build_sweep_tick_record([201, 503], frames,
                                      target_offsets_ms=(200, 500),
                                      capture_ms=[42.5, 39.1], mount_seq=7)
    check(rec["capture_ms"] == [42.5, 39.1],
          f"per-frame capture wall-clock time is carried through (issue #197 evidence) ({rec['capture_ms']})")
    check(rec["mount_seq"] == 7,
          f"the mount seq this tick's sweep anchored to is recorded ({rec['mount_seq']})")


def test_build_sweep_tick_record_capture_ms_and_mount_seq_default_empty():
    a = np.zeros((4, 4, 3), dtype=np.uint8)
    rec = lib.build_sweep_tick_record([201, 503], [a, a.copy()],
                                      target_offsets_ms=(200, 500))
    check(rec["capture_ms"] == [],
          f"a caller that doesn't measure capture time gets [] not a missing key ({rec['capture_ms']})")
    check(rec["mount_seq"] is None,
          "a caller that doesn't pass mount_seq gets None not a missing key")


# ── capture_ms_total_ms / panel_dims (issue #252 evidence) ─────────────────
#
# #252's secondary finding: capture_ms existed in the raw per-frame list but
# nothing summed it for a reader, and no field at all recorded the panel
# size a frame's cost should be attributed to. These pin both additions so
# a regression that silently drops either (e.g. the field reverting to a
# hardcoded 0.0/{} regardless of input -- the exact shape of the original
# bug, just moved) is caught here, not rediscovered from a 40-minute
# rehearsal.
def test_build_sweep_tick_record_capture_ms_total_is_the_sum():
    a = np.zeros((4, 4, 3), dtype=np.uint8)
    rec = lib.build_sweep_tick_record([201, 503, 1010], [a, a.copy(), a.copy()],
                                      target_offsets_ms=(200, 500, 1000),
                                      capture_ms=[120.5, 95.25, 88.0])
    check(rec["capture_ms_total_ms"] == 303.75,
          f"capture_ms_total_ms is the plain sum, not a placeholder (got "
          f"{rec['capture_ms_total_ms']}) -- this is what a reviewer reads "
          f"first against the ~300ms/frame acceptance bound, not the raw "
          f"list")


def test_build_sweep_tick_record_capture_ms_total_zero_when_not_measured():
    a = np.zeros((4, 4, 3), dtype=np.uint8)
    rec = lib.build_sweep_tick_record([201, 503], [a, a.copy()],
                                      target_offsets_ms=(200, 500))
    check(rec["capture_ms_total_ms"] == 0.0,
          "no capture_ms measured sums to 0.0, not None -- the empty "
          "capture_ms list itself is what signals 'not measured'")


def test_build_sweep_tick_record_records_panel_dims():
    a = np.zeros((4, 4, 3), dtype=np.uint8)
    dims = {"box_width": 1698.0, "box_height": 2340.5,
            "clip_width": 1698.0, "clip_height": 700.0}
    rec = lib.build_sweep_tick_record([201, 503], [a, a.copy()],
                                      target_offsets_ms=(200, 500),
                                      panel_dims=dims)
    check(rec["panel_dims"] == dims,
          f"the panel size actually captured this tick is carried through "
          f"unchanged (issue #252 evidence) ({rec['panel_dims']})")


def test_build_sweep_tick_record_panel_dims_default_empty_dict():
    a = np.zeros((4, 4, 3), dtype=np.uint8)
    rec = lib.build_sweep_tick_record([201, 503], [a, a.copy()],
                                      target_offsets_ms=(200, 500))
    check(rec["panel_dims"] == {},
          f"a caller that doesn't measure panel dims gets {{}} not a "
          f"missing key ({rec['panel_dims']})")


# ── capture_budget_ok (issue #252 review round 1 finding 1; demoted to
# reporting-only by the issue that added frame_dims_ok/frame_spacing_ok
# below -- CAPTURE_MS_BOUND_MS's own comment has the full history: it was
# calibrated against the wrong regime and was red on unmodified, healthy
# master within a day of merging) ─────────────────────────────────────────
#
# capture_budget_ok itself is unchanged -- still computable, still reported
# in summary.json's capture_budget key -- only its WIRING into
# build_tab_result's `ok` was removed. These pin the standalone function.

def test_capture_budget_ok_under_bound_passes():
    ticks = [{"capture_ms": [60.0, 55.0], "capture_ms_total_ms": 115.0},
            {"capture_ms": [70.0, 65.0], "capture_ms_total_ms": 135.0}]
    ok, detail = lib.capture_budget_ok(ticks)
    check(ok is True, f"well under CAPTURE_MS_BOUND_MS (800) passes (detail={detail!r})")
    check(detail is None, "no offending tick -> no detail text")


def test_capture_budget_ok_over_bound_fails():
    # A regression roughly at the OLD ~600ms/sweep regime (pre-#252, per-
    # frame elementHandle.screenshot() at full device scale) -- still
    # comfortably inside a 5s tick, so nothing else in build_tab_result
    # would ever flag it. Checked against a tighter bound (400ms) than the
    # real 800ms default so this test does not depend on CAPTURE_MS_BOUND_MS
    # itself ever changing -- the DEFAULT-bound case is covered separately
    # (test_build_tab_result_capture_budget_over_bound_fails_tab, at the
    # real production default of 800ms with a total picked to exceed it).
    ticks = [{"capture_ms": [120.0, 115.0, 130.0, 118.0, 122.0],
              "capture_ms_total_ms": 605.0}]
    ok, detail = lib.capture_budget_ok(ticks, bound_ms=400.0)
    check(ok is False, f"605ms > 400ms bound must fail (detail={detail!r})")
    check(detail is not None and "tick 1" in detail and "605.0" in detail,
          f"the offending tick and its own total are named, not just 'something failed' ({detail!r})")


def test_capture_budget_ok_unmeasured_tick_skipped_not_violation():
    # A tick whose capture_ms was never measured (empty list -- the "not
    # measured" contract build_sweep_tick_record already uses) must not be
    # treated as a budget violation just because capture_ms_total_ms
    # defaults to 0.0 for it -- 0.0 there means "nothing to sum", not "zero
    # cost measured". Mixed with a genuinely measured, well-under-bound
    # tick so the skip is the reason it passes, not a vacuous empty list.
    ticks = [{"capture_ms": [], "capture_ms_total_ms": 0.0},
            {"capture_ms": [50.0], "capture_ms_total_ms": 50.0}]
    ok, detail = lib.capture_budget_ok(ticks, bound_ms=800.0)
    check(ok is True, f"an unmeasured tick is skipped, not scored (detail={detail!r})")


def test_capture_budget_ok_no_ticks_at_all_trivially_passes():
    ok, detail = lib.capture_budget_ok([])
    check(ok is True, "no ticks at all -> nothing to violate the bound")
    check(detail is None, "no detail text for the trivial case")


def test_build_tab_result_capture_budget_reported_but_does_not_gate_ok():
    # An otherwise-clean tab with in-budget capture cost: capture_budget
    # still reports True, tab still passes. frame_dims_px/panel_dims are
    # filled in (matching, well within tolerance) so frame_dims_ok isn't
    # ALSO failing this tick for an unrelated reason (frame_dims_ok, this
    # issue, fails loudly on a tick with no recorded dims -- see its own
    # tests) -- this test isolates capture_budget's own demotion.
    tick_pass = {"target_offsets_ms": [200], "achieved_offsets_ms": [205],
                "capture_ms": [50.0], "capture_ms_total_ms": 50.0,
                "panel_dims": {"clip_width": 100.0, "clip_height": 50.0},
                "frame_dims_px": [{"width": 100.0, "height": 50.0}],
                "ratios": [], "notes": []}
    result = lib.build_tab_result(
        "sessions", True, "6 rows", 6, [], 0.0, [],
        {"charts": 0, "uplots": 0, "pending": 0},
        {"charts": 0, "uplots": 0, "pending": 0}, {},
        blink_pair_offsets_ms=[10] * 6, blink_sweep_ticks=[tick_pass] * 6)
    check(result["capture_budget"]["ok"] is True,
          f"under-bound capture cost reports True ({result['capture_budget']})")
    check(result["ok"] is True,
          "an otherwise-clean tab with in-budget capture cost still passes overall")


def test_build_tab_result_capture_budget_over_bound_no_longer_fails_tab():
    # THE demotion proof (this issue): capture_ms regressing past the bound
    # must still be VISIBLE (capture_budget.ok False, detail naming the
    # offending ticks) but must NOT fail the tab by itself any more --
    # CAPTURE_MS_BOUND_MS's own comment has the evidence this bound was
    # mis-specified (red on unmodified, healthy master) and frame_dims_ok/
    # frame_spacing_ok are what actually gate the regression this bound was
    # trying to catch. Same tick shape as the old (pre-demotion) "fails
    # tab" case -- small, in-bound drift -- plus matching frame_dims_px/
    # panel_dims (frame_dims_ok, this issue, fails loudly on a tick with
    # no recorded dims) so only capture_budget itself is over bound.
    tick_over = {"target_offsets_ms": [200, 500, 1000, 1500, 2000],
                "achieved_offsets_ms": [205, 510, 1010, 1515, 2020],
                "capture_ms": [120.0, 115.0, 130.0, 118.0, 122.0],
                "capture_ms_total_ms": 605.0,
                "panel_dims": {"clip_width": 100.0, "clip_height": 50.0},
                "frame_dims_px": [{"width": 100.0, "height": 50.0}] * 5,
                "ratios": [0.0, 0.0, 0.0, 0.0], "notes": []}
    result = lib.build_tab_result(
        "sessions", True, "6 rows", 6, [], 0.0, [],
        {"charts": 0, "uplots": 0, "pending": 0},
        {"charts": 0, "uplots": 0, "pending": 0}, {},
        blink_pair_offsets_ms=[10] * 6, blink_sweep_ticks=[tick_over] * 6,
        capture_ms_bound_ms=400.0)
    check(result["capture_budget"]["ok"] is False,
          f"605ms > 400ms bound is still FLAGGED in the report ({result['capture_budget']})")
    check(result["frame_dims"]["ok"] is True and result["frame_spacing"]["ok"] is True,
          f"isolating the claim: dims/spacing are clean here, only capture_budget is over "
          f"({result['frame_dims']}, {result['frame_spacing']})")
    check(result["ok"] is True,
          "a capture-budget-only violation no longer fails the tab -- demoted to reporting "
          f"(got ok={result['ok']!r})")


# ── frame_dims_ok / build_sweep_tick_record's frame_dims_px (this issue) ───
#
# The dimension assertion: catches the capture-cost regression's CAUSE
# directly (decoded frame size vs the CSS-px clip that was requested)
# instead of inferring it from wall-clock time. See frame_dims_ok's own
# docstring for why a missing frame/missing clip dims is a VIOLATION here,
# unlike capture_budget_ok's skip.

def test_build_sweep_tick_record_records_frame_dims_px():
    a = np.zeros((20, 30, 3), dtype=np.uint8)  # (H=20, W=30, 3)
    rec = lib.build_sweep_tick_record([201, 503], [a, a.copy()],
                                      target_offsets_ms=(200, 500))
    check(rec["frame_dims_px"] == [{"width": 30.0, "height": 20.0}] * 2,
          f"frame dims are derived from the decoded array's own (H, W, 3) "
          f"shape, width/height not swapped ({rec['frame_dims_px']})")


def test_build_sweep_tick_record_frame_dims_px_none_for_missing_frame():
    a = np.zeros((20, 30, 3), dtype=np.uint8)
    rec = lib.build_sweep_tick_record([201, 503], [a, None],
                                      target_offsets_ms=(200, 500))
    check(rec["frame_dims_px"] == [{"width": 30.0, "height": 20.0}, None],
          f"a missing (None) frame gets None, not a fabricated size "
          f"({rec['frame_dims_px']})")


def _dims_tick(frame_wh, clip_wh):
    """Builds a one-frame tick dict with frame dims frame_wh=(w,h) and
    requested clip dims clip_wh=(w,h), the minimal shape frame_dims_ok
    reads."""
    w, h = frame_wh
    cw, ch = clip_wh
    return {"frame_dims_px": [{"width": float(w), "height": float(h)}],
            "panel_dims": {"clip_width": float(cw), "clip_height": float(ch)}}


def test_frame_dims_ok_within_tolerance_passes():
    ticks = [_dims_tick((1698, 700), (1698.4, 700.6))]  # < 1px off each axis
    ok, detail = lib.frame_dims_ok(ticks)
    check(ok is True, f"sub-pixel rounding noise passes ({detail!r})")
    check(detail is None, "no violation -> no detail text")


def test_frame_dims_ok_2x_dims_fails():
    # THE regression case (contract (c)): removing scale="css" at DPR2
    # doubles both axes relative to the CSS-px clip -- deterministic, not
    # a timing inference.
    ticks = [_dims_tick((3396, 1400), (1698, 700))]
    ok, detail = lib.frame_dims_ok(ticks)
    check(ok is False, f"a 2x-dims frame (scale='css' removed) must fail ({detail!r})")
    check(detail is not None and "3396" in detail and "1698" in detail,
          f"both the wrong frame size and the requested clip are named ({detail!r})")


def test_frame_dims_ok_missing_frame_fails_loudly_not_skipped():
    # Contrast capture_budget_ok's skip semantics: a missing frame gives
    # this check zero power to see a dims regression on that tick, so it
    # must fail, not silently pass an empty check.
    ticks = [{"frame_dims_px": [None],
             "panel_dims": {"clip_width": 1698.0, "clip_height": 700.0}}]
    ok, detail = lib.frame_dims_ok(ticks)
    check(ok is False, f"a missing frame is a violation, not a skip ({detail!r})")
    check(detail is not None and "missing frame" in detail, f"({detail!r})")


def test_frame_dims_ok_missing_clip_dims_fails_loudly_not_skipped():
    ticks = [{"frame_dims_px": [{"width": 1698.0, "height": 700.0}],
             "panel_dims": {}}]
    ok, detail = lib.frame_dims_ok(ticks)
    check(ok is False, f"missing clip dims to compare against is a violation ({detail!r})")
    check(detail is not None and "missing clip dims" in detail, f"({detail!r})")


def test_frame_dims_ok_no_ticks_at_all_trivially_passes():
    ok, detail = lib.frame_dims_ok([])
    check(ok is True, "no ticks at all -> nothing to have gotten wrong")
    check(detail is None, "no detail text for the trivial case")


def test_frame_dims_ok_tick_with_missing_frame_dims_px_fails_loudly():
    # Review finding: an earlier version treated a tick with no
    # frame_dims_px key at all as vacuous, reasoning that ticks_ok/
    # measured_ok already fail a tab that never captured anything -- true
    # today, but it made THIS gate's correctness depend on ANOTHER gate's
    # behaviour, which the contract explicitly ruled out ("fail loudly on
    # a missing frame or missing dimensions rather than skipping" --
    # applies to the tick's OWN frame_dims_px being missing, not just an
    # individual entry within a non-empty list). Only the empty-LIST case
    # (frame_dims_ok([]) -- no ticks existed at all) stays vacuous; a
    # single tick with nothing recorded does not.
    ticks = [{"panel_dims": {}}]
    ok, detail = lib.frame_dims_ok(ticks)
    check(ok is False, f"a tick missing frame_dims_px entirely is a violation, not a skip ({detail!r})")
    check(detail is not None and "tick 1" in detail and "frame_dims_px" in detail,
          f"names the tick and what's missing ({detail!r})")


def test_frame_dims_ok_tick_with_empty_frame_dims_px_list_fails_loudly():
    # Same as above but an explicit empty list rather than a missing key
    # -- both are "nothing recorded", both must fail the same way.
    ticks = [{"frame_dims_px": [], "panel_dims": {"clip_width": 100.0, "clip_height": 50.0}}]
    ok, detail = lib.frame_dims_ok(ticks)
    check(ok is False, f"an explicit empty frame_dims_px list is also a violation ({detail!r})")


def test_build_tab_result_frame_dims_gates_ok():
    a = np.zeros((20, 30, 3), dtype=np.uint8)  # H=20, W=30
    tick = lib.build_sweep_tick_record(
        [205], [a], target_offsets_ms=(200,),
        panel_dims={"clip_width": 30.0, "clip_height": 20.0})
    result = lib.build_tab_result(
        "sessions", True, "6 rows", 6, [], 0.0, [],
        {"charts": 0, "uplots": 0, "pending": 0},
        {"charts": 0, "uplots": 0, "pending": 0}, {},
        blink_pair_offsets_ms=[10] * 6, blink_sweep_ticks=[tick] * 6)
    check(result["frame_dims"]["ok"] is True,
          f"matching decoded dims pass ({result['frame_dims']})")
    check(result["ok"] is True, "a clean tab with matching frame dims passes overall")


def test_build_tab_result_frame_dims_2x_regression_fails_tab():
    # Bypass-suite pairing with the PASS case above, end-to-end through
    # build_sweep_tick_record (real decoded-array shapes, not a hand-built
    # dict) -- proves the wiring is real: a tab that is clean on every
    # OTHER check must still fail overall once its decoded frames come
    # back at 2x the requested clip (scale="css" removed at DPR2).
    a_2x = np.zeros((40, 60, 3), dtype=np.uint8)  # 2x (20,30): the DPR2 regression
    tick = lib.build_sweep_tick_record(
        [205], [a_2x], target_offsets_ms=(200,),
        panel_dims={"clip_width": 30.0, "clip_height": 20.0})
    result = lib.build_tab_result(
        "sessions", True, "6 rows", 6, [], 0.0, [],
        {"charts": 0, "uplots": 0, "pending": 0},
        {"charts": 0, "uplots": 0, "pending": 0}, {},
        blink_pair_offsets_ms=[10] * 6, blink_sweep_ticks=[tick] * 6)
    check(result["frame_dims"]["ok"] is False,
          f"2x-dims frames must be flagged ({result['frame_dims']})")
    check(result["ok"] is False,
          "a dimension-doubling regression fails the WHOLE tab, even though "
          "rendered/clean/no_blink/measured/no_leak/ticks/capture_budget are all clean")


# ── frame_spacing_ok / frame_spacing_drift_ms (this issue) ─────────────────
#
# The frame-spacing (drift) bound: what CAPTURE_MS_BOUND_MS was actually
# trying to protect. See FRAME_SPACING_DRIFT_BOUND_MS's own comment for the
# full derivation (healthy <=30ms excluding tick 1, regression >=350ms).

def test_frame_spacing_drift_ms_basic():
    tick = {"target_offsets_ms": [200, 500, 1000], "achieved_offsets_ms": [205, 520, 990]}
    check(lib.frame_spacing_drift_ms(tick) == [5, 20, -10],
          f"drift is achieved-minus-target, signed ({lib.frame_spacing_drift_ms(tick)})")


def test_frame_spacing_drift_ms_mismatched_lengths_truncates_not_crashes():
    tick = {"target_offsets_ms": [200, 500, 1000], "achieved_offsets_ms": [205, 520]}
    check(lib.frame_spacing_drift_ms(tick) == [5, 20],
          "a short achieved_offsets_ms truncates to the shorter length, never raises")


def _spacing_tick(tick_i, drifts_from_200_500_1000_1500_2000, with_dims=True):
    targets = [200, 500, 1000, 1500, 2000]
    achieved = [t + d for t, d in zip(targets, drifts_from_200_500_1000_1500_2000)]
    tick = {"target_offsets_ms": targets, "achieved_offsets_ms": achieved}
    if with_dims:
        # Matching, in-tolerance dims by default so a test built on this
        # helper and fed through build_tab_result isolates frame_spacing
        # -- frame_dims_ok (this issue) fails loudly on a tick with no
        # recorded dims, so a caller that only cares about spacing still
        # needs SOMETHING here to keep dims green.
        tick["panel_dims"] = {"clip_width": 100.0, "clip_height": 50.0}
        tick["frame_dims_px"] = [{"width": 100.0, "height": 50.0}] * 5
    return tick


def test_frame_spacing_ok_within_bound_passes():
    # Healthy shape, POST-tick_baseline-fix: tick 1 looks like every other
    # tick, because its anchor is now a genuinely new live tick. These are the
    # real recorded offsets of scatter TICK 2 from gate-1 run.id 1791385783
    # (A=207ms, frames 2-5 drifting 11/14/18/10ms) -- the steady per-tick
    # shape that 203-226ms of floor produces, and what tick 1 is expected to
    # join. Nothing here relies on an exemption.
    ticks = [_spacing_tick(i, [7, 11, 14, 18, 10]) for i in range(1, 7)]
    ok, detail = lib.frame_spacing_ok(ticks)
    check(ok is True, f"healthy drift passes with NO tick-1 exemption ({detail!r})")


def test_frame_spacing_ok_over_bound_fails():
    # THE regression case (contract (c)): a 400ms drift at frame 2 --
    # well past the 150ms bound (this branch's own bypass evidence puts
    # the real regression floor at 154-480ms on gate-1 at the shipped
    # bound; 400 sits inside that, not a borderline value picked to just
    # clear the bound).
    ticks = [_spacing_tick(1, [30, 5, 10, 15, 20]),
            _spacing_tick(2, [30, 400, 10, 15, 20])]
    ok, detail = lib.frame_spacing_ok(ticks)
    check(ok is False, f"400ms drift at frame 2 (tick 2) must fail ({detail!r})")
    check(detail is not None and "tick 2 frame 2" in detail and "400" in detail,
          f"the offending tick and frame are named ({detail!r})")


def test_frame_spacing_ok_tick_1_frame_2_is_gated_like_every_other_tick():
    # THE BUG THIS ISSUE FIXES, as its own red/green pair.
    #
    # These are the REAL recorded offsets of scatter tick 1 from gate-1 run.id
    # 1791385783 (achieved [772, 1004, 1183, 1511, 2013] against the standard
    # [200, 500, 1000, 1500, 2000] targets -- see
    # tests/results/ui_live_anchor_evidence/). On a HEALTHY tree: the sweep
    # anchored on a mount that predated _navigate_to_tab's 500ms live-resume
    # settle, so frame 2 landed 504ms after its target.
    #
    # Under the old tick-1 frames-1-2 exemption this whole tick PASSED. It must
    # not: 504ms of drift is 3.4x the bound, and the tick_baseline fix means
    # tick 1's anchor is now as fresh as any other tick's, so there is nothing
    # left for the exemption to excuse.
    ticks = [_spacing_tick(1, [572, 504, 183, 11, 13])]
    ok, detail = lib.frame_spacing_ok(ticks)
    check(ok is False,
          f"tick 1's frame 2 is gated like every other tick's frame 2 ({detail!r})")
    check(detail is not None and "tick 1 frame 2" in detail and "504" in detail,
          f"names tick 1 frame 2 explicitly ({detail!r})")
    check(detail is not None and "tick 1 frame 3" in detail,
          f"and still names frame 3 alongside it ({detail!r})")


def test_frame_spacing_ok_tick_1_frame_2_ISOLATED_is_enough_to_fail():
    # The same bug with frame 2 as the ONLY breach, so the verdict cannot be
    # reached via some other frame. [266, 227, 14, 10, 9] is matrix tick 1
    # verbatim from the committed artifact
    # tests/results/ui_live_gate1_healthy_run1/summary.json (run.id
    # 1790796035): frame 2 drifts 227ms, and frame 3 recovered to 14ms because
    # A+capture(f1) had not yet pushed past the 1000ms target.
    #
    # On the pre-fix tree this tick passed COMPLETELY -- 227ms of drift inside
    # the tick-1 frames-1-2 exemption, in one of the five runs the bound was
    # calibrated on. That is the false negative the exemption was buying, in a
    # retained artifact anyone can open.
    ticks = [_spacing_tick(1, [266, 227, 14, 10, 9])]
    ok, detail = lib.frame_spacing_ok(ticks)
    check(ok is False,
          f"227ms at tick 1 frame 2 alone fails the tab ({detail!r})")
    check(detail is not None and "tick 1 frame 2" in detail and "227" in detail,
          f"and frame 2 is the frame named ({detail!r})")
    check(detail is not None and "frame 3" not in detail and "frame 4" not in detail,
          f"no other frame of this tick is implicated ({detail!r})")


def test_frame_spacing_ok_tick_1_frame_1_still_exempt_like_every_tick():
    # The exemption that REMAINS, and is unchanged: frame 1 (the 200ms
    # target) on every tick alike. The per-tick floor beneath it measured
    # 203-226ms across all 31 retained runs, so a few ms of drift there is
    # structural. A tick whose ONLY elevated frame is frame 1 still passes --
    # this is the boundary that proves the remaining exemption was not
    # widened to absorb the frames the fix is meant to expose.
    ticks = [_spacing_tick(1, [900, 5, 10, 15, 20])]
    ok, detail = lib.frame_spacing_ok(ticks)
    check(ok is True,
          f"frame 1's own drift never gates, on tick 1 either ({detail!r})")


def test_frame_spacing_ok_tick_1_frames_3_to_5_still_gated():
    # Review blocker from an earlier round: a version that excluded tick 1
    # WHOLE let a regression confined to its LATER frames (3-5) pass
    # silently. Still guarded, and now ISOLATED to frame 3 -- every other
    # frame of this tick is healthy, so the only thing that can fail it is
    # frame 3 itself. (Before this issue the same isolation was impossible
    # to express: frames 1-2 had to be elevated to be realistic.)
    ticks = [_spacing_tick(1, [30, 5, 300, 17, 14])]
    ok, detail = lib.frame_spacing_ok(ticks)
    check(ok is False,
          f"a regression in tick 1's frame 3 (outside the frames-1-2 exemption) must fail ({detail!r})")
    check(detail is not None and "tick 1 frame 3" in detail and "300" in detail,
          f"names tick 1 explicitly, not just 'some tick' ({detail!r})")


def test_frame_spacing_ok_excludes_frame_1_on_every_tick():
    # Frame 1 (the 200ms target, drift index 0) is excluded on EVERY tick,
    # not just tick 1 -- only frames 2-5 (indices 1-4) are bounded on a
    # non-tick-1 tick.
    ticks = [_spacing_tick(2, [900, 5, 10, 15, 20])]  # huge frame-1 drift only
    ok, detail = lib.frame_spacing_ok(ticks)
    check(ok is True, f"frame 1's own drift never gates, on any tick ({detail!r})")


# ── tick_hook_index (this issue) ───────────────────────────────────────────
#
# The pure arithmetic behind the tick_baseline fix. Every number the sweep
# later produces -- the mount wait, achieved_offsets_ms, the drift bound -- is
# computed FROM the timestamp this index selects, so a silently-wrong index is
# undetectable downstream: it just re-anchors the tick and keeps looking like a
# measurement. Hence these tests are mostly about REFUSING, not computing.

def test_tick_hook_index_is_one_based_i_into_a_zero_based_array():
    check(lib.tick_hook_index(0, 1) == 0, "no baseline, first tick -> index 0")
    check(lib.tick_hook_index(2, 1) == 2,
          "2 navigation sends already recorded -> tick 1 is index 2")
    check(lib.tick_hook_index(2, 6) == 7, "...and tick 6 is index 7")


def test_tick_hook_index_refuses_rather_than_clamping():
    # A gate that cannot address the right entry must refuse, never return
    # something plausible. i=0 is the off-by-one a future edit is most likely
    # to introduce (treating the loop as 0-based); a negative baseline means
    # the page.evaluate that produced it returned nonsense.
    for baseline, i, why in ((0, 0, "i=0 (loop is 1-based)"),
                             (0, -1, "negative i"),
                             (-1, 1, "negative baseline")):
        try:
            got = lib.tick_hook_index(baseline, i)
        except ValueError:
            continue
        check(False, f"tick_hook_index({baseline}, {i}) must raise for {why}, got {got!r}")


def test_tick_hook_index_refuses_non_int_and_bool():
    # window.__uiLiveTicks.length arriving as None/str/float (a hook that was
    # replaced or reset, or a changed evaluate) must not silently become an
    # index. bool is an int subclass in Python, so True would otherwise pass
    # as i=1 and quietly anchor every tick on the same entry.
    for baseline, i, why in ((None, 1, "baseline None"),
                             ("3", 1, "baseline str"),
                             (3.0, 1, "baseline float"),
                             (0, True, "i as bool True"),
                             (True, 1, "baseline as bool True"),
                             (0, None, "i None")):
        try:
            got = lib.tick_hook_index(baseline, i)
        except ValueError:
            continue
        check(False, f"tick_hook_index({baseline!r}, {i!r}) must raise for {why}, got {got!r}")


def test_frame_spacing_ok_no_ticks_at_all_trivially_passes():
    ok, detail = lib.frame_spacing_ok([])
    check(ok is True, "no ticks at all -> nothing to have gotten wrong")
    check(detail is None, "no detail text for the trivial case")


def test_frame_spacing_vacuous_pass_is_a_KNOWN_unclosed_blind_spot():
    """PINS a false negative rather than claiming it is fixed.

    frame_spacing_ok([]) passes vacuously (above), and so do frame_dims_ok([])
    and measured_ok at zero attempted ticks. So a tab that claims
    ticks_observed=6 while recording NO sweep records at all is reported
    ok=True -- the gate is not wrong, it is BLIND, which is the worse of the
    two. This issue makes frame_spacing carry more weight (tick 1's frame 2 is
    now gated), so the hole is worth naming precisely.

    NOT closed here, for a checked reason rather than an assumed one: the
    obvious guard ("ticks_observed >= MIN_TICKS implies blink_sweep_ticks is
    non-empty") would turn tests/test_demo_rehearsal_lib.py's
    test_blink_gate_clean_trace_still_passes red -- it calls build_tab_result
    with blink_pair_offsets_ms=[100]*6 and no blink_sweep_ticks and asserts
    ok is True. That file and tests/demo_rehearsal_lib.py belong to another
    in-flight branch, so the guard is filed as its own issue instead of being
    smuggled in here.

    This test exists so the hole cannot be closed BY ACCIDENT without someone
    reading this comment: if a future change makes the no-sweep-records case
    fail, this test goes red and points at the issue."""
    r = lib.build_tab_result(
        "scatter", True, "rendered", 6, [], 0.0, [],
        {"charts": 1, "uplots": 1, "pending": 0},
        {"charts": 1, "uplots": 1, "pending": 0}, {},
        blink_pair_offsets_ms=[], blink_sweep_ticks=[])
    check(r["frame_spacing"]["ok"] is True,
          "frame_spacing passes vacuously with no sweep records (documented)")
    check(r["ok"] is True,
          "KNOWN GAP: ticks_observed=6 with zero sweep records is still ok=True "
          "-- if this went False, the blind spot was closed; update this test "
          "and its issue rather than deleting it")


def test_build_tab_result_frame_spacing_reported_but_does_not_gate_ok():
    # Renamed with the demotion: frame_spacing no longer gates, so the old
    # name ("gates_ok") asserted something that is no longer true. The
    # assertions are unchanged -- a clean tab with well-spaced frames reports
    # True and passes; the demotion's own proof is the test below.
    ticks = [_spacing_tick(i, [30, 5, 10, 15, 20]) for i in range(1, 7)]
    result = lib.build_tab_result(
        "sessions", True, "6 rows", 6, [], 0.0, [],
        {"charts": 0, "uplots": 0, "pending": 0},
        {"charts": 0, "uplots": 0, "pending": 0}, {},
        blink_pair_offsets_ms=[10] * 6, blink_sweep_ticks=ticks)
    check(result["frame_spacing"]["ok"] is True,
          f"well-spaced frames pass ({result['frame_spacing']})")
    check(result["frame_dims"]["ok"] is True,
          f"dims stay clean too (_spacing_tick's default matching dims) ({result['frame_dims']})")
    check(result["ok"] is True, "a clean tab with well-spaced frames passes overall")


def test_build_tab_result_frame_spacing_over_bound_no_longer_fails_tab():
    # THE demotion proof (this issue), same shape as capture_budget's own
    # demotion test above. A drift regression must still be VISIBLE
    # (frame_spacing.ok False, detail naming the offending tick+frame) but
    # must NOT fail the tab by itself any more.
    #
    # This is a DEMOTION of a non-discriminating check, not a widening: the
    # bound is still 150.0 (asserted below, so a future "fix" that quietly
    # raises it instead fails here), and frame_dims_ok still gates. See
    # FRAME_SPACING_DRIFT_BOUND_MS's own comment: the deliberate bypass
    # regression scores f3=158ms while a healthy loaded run scores 297ms,
    # i.e. the defect scores BETTER than health, and frame_dims_ok catches
    # that same regression on 11/11 and 10/11 tabs of the two bypass runs.
    ticks = [_spacing_tick(1, [30, 5, 11, 17, 14])] + \
            [_spacing_tick(2, [30, 400, 10, 15, 20])] + \
            [_spacing_tick(i, [30, 5, 10, 15, 20]) for i in range(3, 7)]
    result = lib.build_tab_result(
        "sessions", True, "6 rows", 6, [], 0.0, [],
        {"charts": 0, "uplots": 0, "pending": 0},
        {"charts": 0, "uplots": 0, "pending": 0}, {},
        blink_pair_offsets_ms=[10] * 6, blink_sweep_ticks=ticks)
    check(result["frame_spacing"]["ok"] is False,
          f"400ms drift at tick 2 frame 2 is still FLAGGED ({result['frame_spacing']})")
    check(result["frame_spacing"]["detail"] is not None
          and "tick 2 frame 2" in result["frame_spacing"]["detail"],
          f"...and still names the offending tick+frame ({result['frame_spacing']})")
    check(result["frame_spacing"]["bound_ms"] == 150.0,
          f"the bound is UNCHANGED at 150ms -- this is a demotion, not a widening "
          f"({result['frame_spacing']['bound_ms']})")
    check(result["frame_dims"]["ok"] is True and result["capture_budget"]["ok"] is True,
          "isolating the claim: dims/capture_budget are clean here, only spacing is over")
    check(result["ok"] is True,
          "a frame-spacing-only violation no longer fails the tab -- demoted to "
          f"reporting (got ok={result['ok']!r})")


def test_frame_spacing_demotion_did_not_de_tooth_the_product_checks():
    """The other half of the demotion: everything product-facing KEEPS its
    teeth. If a future edit demotes one of these by accident -- or copies the
    demotion pattern one line too far -- this goes red.

    Each case is a tab that is clean except for ONE check, and each must still
    fail the tab. The sweep ticks are deliberately well-spaced and
    well-dimensioned throughout, so frame_spacing and frame_dims are green and
    the only thing under test is the named check."""
    clean = [_spacing_tick(i, [30, 5, 10, 15, 20]) for i in range(1, 7)]
    good_leak = {"charts": 0, "uplots": 0, "pending": 0}
    base = dict(ticks_observed=6, console_errors=[], blink_ratio=0.0,
                color_violations=[], leak_before=good_leak,
                leak_after=good_leak, artifacts={},
                blink_pair_offsets_ms=[10] * 6, blink_sweep_ticks=clean)

    def build(**over):
        kw = dict(base)
        kw.update(over)
        kw.setdefault("rendered_ok", True)
        kw.setdefault("rendered_detail", "6 rows")
        return lib.build_tab_result("sessions", kw.pop("rendered_ok"),
                                    kw.pop("rendered_detail"), **kw)

    cases = [
        ("rendered", dict(rendered_ok=False, rendered_detail="no rows")),
        ("clean (console errors)", dict(console_errors=["TypeError: x"])),
        ("no_blink", dict(blink_ratio=0.5)),
        ("color_stability", dict(color_violations=["IO changed colour"])),
        ("no_leak", dict(leak_after={"charts": 9, "uplots": 9, "pending": 3})),
        ("ticks_observed", dict(ticks_observed=1)),
        # measured-fraction floor (MIN_MEASURED_FRACTION): 2 of 6 ticks
        # actually measured is 0.33, under the 0.5 floor. Added after a
        # mutation run showed this guard caught 7 of 8 dropped checks and was
        # BLIND to measured_ok -- see this branch's report for the harness.
        ("measured fraction", dict(blink_not_measured=[3, 4, 5, 6])),
    ]
    for name, over in cases:
        r = build(**over)
        check(r["ok"] is False,
              f"{name} must still GATE the tab after frame_spacing's demotion "
              f"(got ok={r['ok']!r})")
    # frame_dims is the one that REPLACES frame_spacing's stated purpose, so
    # it gets its own explicit case rather than riding along above.
    bad_dims = dict(clean[0])
    bad_dims["frame_dims_px"] = [{"width": 200.0, "height": 100.0}] * 5
    r = build(blink_sweep_ticks=[bad_dims] + clean[1:])
    check(r["frame_dims"]["ok"] is False and r["ok"] is False,
          f"frame_dims must still GATE -- it is the detector frame_spacing was "
          f"demoted in favour of ({r['frame_dims']}, ok={r['ok']!r})")


# ── clip_rect_to_viewport (issue #197) ──────────────────────────────────────
#
# The Sessions panel (#table-container) has no vertical overflow/height cap,
# so it grows to its full row count -- ~5793px at 205 rows on the gate box.
# Playwright's own elementHandle.screenshot() captures such an element IN
# FULL (expanding the capture region beyond the viewport), which measured
# ~1.4s for that panel: the 5-frame blink sweep then cost more than the 5s
# live tick it needs to fit inside of, so every sweep straddled a mount by
# construction. clip_rect_to_viewport is the arithmetic behind capturing
# only the viewport-visible slice instead, so one frame's cost stays roughly
# constant regardless of row count.

def test_clip_rect_to_viewport_panel_fits_entirely():
    # A short panel (e.g. a chart tab) entirely inside the viewport: the
    # clip is just the panel's own rect, unchanged.
    clip = lib.clip_rect_to_viewport(10, 20, 300, 200, 1280, 900)
    check(clip == {"x": 10.0, "y": 20.0, "width": 300.0, "height": 200.0},
          f"a panel that fits is clipped to itself unchanged ({clip})")


def test_clip_rect_to_viewport_tall_panel_clips_to_viewport_height():
    # The Sessions-panel case: a 5793px-tall panel starting at y=0 (scrolled
    # to the top), viewport 900px tall -- MUST clip to 900, never capture
    # the whole 5793px (that's the bug this function exists to fix: a clip
    # this function fails to produce leaves _safe_panel_screenshot calling
    # panel.screenshot() on the full element instead, which is the exact
    # cost regression issue #197 fixes).
    clip = lib.clip_rect_to_viewport(0, 0, 1200, 5793, 1280, 900)
    check(clip == {"x": 0.0, "y": 0.0, "width": 1200.0, "height": 900.0},
          f"a too-tall panel clips to the viewport height, not its own full height ({clip})")


def test_clip_rect_to_viewport_scrolled_past_top_negative_y():
    # scroll_into_view_if_needed() aligning the panel can still leave a
    # negative bounding-box y in principle (over-scroll); the clip must
    # still land inside [0, viewport_height), never a negative origin.
    clip = lib.clip_rect_to_viewport(0, -50, 800, 5793, 1280, 900)
    check(clip == {"x": 0.0, "y": 0.0, "width": 800.0, "height": 900.0},
          f"negative y clamps to the viewport top ({clip})")


def test_clip_rect_to_viewport_panel_scrolled_fully_out_of_view_is_none():
    clip = lib.clip_rect_to_viewport(0, 950, 800, 200, 1280, 900)
    check(clip is None,
          f"a panel entirely below the viewport has no intersection -- None, not a "
          f"zero/negative-size clip ({clip})")


def test_clip_rect_to_viewport_zero_height_is_none():
    clip = lib.clip_rect_to_viewport(0, 900, 800, 200, 1280, 900)
    check(clip is None,
          f"a panel starting exactly at the viewport bottom edge has zero-area "
          f"intersection -- None, never a 0-height clip Playwright would reject ({clip})")


# ── panel_capture_clip (issue #197 review: the fail-safe finding) ──────────
#
# The FIRST version of this fix fell back to an unclipped, unbounded-cost
# capture whenever page.viewport_size was None -- unreachable at today's
# call site, but the shape (a silent fallback to exactly the cost regression
# this issue removes, with nothing in summary.json to show it happened) is
# the problem a fail-safe branch must never have. panel_capture_clip pins
# the fix: no viewport (or no box) means "cannot safely capture", treated
# identically to every other such case -- None, never an unclipped capture.

def test_panel_capture_clip_no_viewport_is_none():
    box = {"x": 0, "y": 0, "width": 1200, "height": 5793}
    check(lib.panel_capture_clip(box, None) is None,
          "no viewport -- cannot safely determine a clip -- None, NEVER a "
          "fallback to an unclipped capture (the exact regression this issue fixes)")


def test_panel_capture_clip_no_box_is_none():
    viewport = {"width": 1280, "height": 900}
    check(lib.panel_capture_clip(None, viewport) is None,
          "no box (panel gone/detached) -- None, same as every other "
          "cannot-capture case")


def test_panel_capture_clip_no_box_and_no_viewport_is_none():
    check(lib.panel_capture_clip(None, None) is None,
          "neither box nor viewport available -- still None")


def test_panel_capture_clip_normal_case_matches_clip_rect_to_viewport():
    box = {"x": 0, "y": 0, "width": 1200, "height": 5793}
    viewport = {"width": 1280, "height": 900}
    got = lib.panel_capture_clip(box, viewport)
    want = lib.clip_rect_to_viewport(0, 0, 1200, 5793, 1280, 900)
    check(got == want == {"x": 0.0, "y": 0.0, "width": 1200.0, "height": 900.0},
          f"the normal case delegates to clip_rect_to_viewport unchanged ({got})")


# ── mount_is_fresh (issue #197: the stale-mount re-match defect) ───────────
#
# _wait_for_mount_at_or_after's ORIGINAL (issue #193 round 2) contract only
# checked id + timestamp. A mount whose refresh cycle outran the 5s tick
# interval can land with `at` already past the NEXT tick's own tick_ts, so
# that check alone hands the SAME already-swept mount to two ticks in a row
# -- the second sweep then fires all 5 frames back-to-back against a
# already-stale mount_at_ms, with none of the spread across the tick that
# lets the sweep see a sub-second transient at all. min_seq is the fix: the
# THIS input that makes the OLD id+timestamp-only check pass while the
# product is broken is exactly the case these tests name explicitly.

def test_mount_is_fresh_accepts_a_genuinely_new_mount():
    mount = {"id": "sessions", "seq": 5, "at": 1000}
    check(lib.mount_is_fresh(mount, "sessions", tick_ts_ms=900, min_seq=4) is True,
          "a mount newer than min_seq, at/after the tick, for the right tab: fresh")


def test_mount_is_fresh_first_tick_has_no_min_seq_to_violate():
    mount = {"id": "sessions", "seq": 1, "at": 1000}
    check(lib.mount_is_fresh(mount, "sessions", tick_ts_ms=900, min_seq=None) is True,
          "tick 1 has no previous tick's seq to be newer than -- min_seq=None never blocks it")


def test_mount_is_fresh_rejects_the_stale_reused_mount():
    # THE defect this fixes: the previous tick already anchored its sweep to
    # seq=5, and that same mount's `at` still satisfies THIS tick's (later)
    # timestamp threshold purely because its refresh cycle ran long. The old
    # id+timestamp-only check (mount is not None and mount["id"] == tab_id
    # and mount["at"] >= tick_ts_ms) would ACCEPT this and re-sweep it --
    # demonstrated red below (test_mount_is_fresh_would_pass_without_the_seq_check).
    mount = {"id": "sessions", "seq": 5, "at": 3200}
    check(lib.mount_is_fresh(mount, "sessions", tick_ts_ms=3000, min_seq=5) is False,
          "a mount whose seq == the already-consumed min_seq is stale, not fresh, "
          "even though its timestamp alone would satisfy the old check")


def test_mount_is_fresh_would_pass_without_the_seq_check():
    """Demonstrates the exact input that makes the OLD (issue #193 round 2)
    id+timestamp-only check pass while the product/harness is actually
    broken -- the stale-mount re-match this issue fixes. Reproduces the OLD
    predicate inline (not by calling mount_is_fresh) so this test documents
    what a regression back to that predicate would look like, and stays red
    against mount_is_fresh itself since the two are asserted to disagree."""
    mount = {"id": "sessions", "seq": 5, "at": 3200}
    tab_id, tick_ts_ms, min_seq = "sessions", 3000, 5
    old_predicate_result = (mount is not None and mount["id"] == tab_id
                            and mount["at"] >= tick_ts_ms)
    check(old_predicate_result is True,
          "the old id+timestamp-only predicate wrongly accepts the stale mount")
    check(lib.mount_is_fresh(mount, tab_id, tick_ts_ms, min_seq) is False,
          "mount_is_fresh correctly rejects the same input -- the two predicates "
          "disagree on exactly the case this issue fixes")


def test_mount_is_fresh_rejects_wrong_tab():
    mount = {"id": "overview", "seq": 5, "at": 1000}
    check(lib.mount_is_fresh(mount, "sessions", tick_ts_ms=900, min_seq=4) is False,
          "a mount belonging to a different tab is never fresh for this one")


def test_mount_is_fresh_rejects_none_mount():
    check(lib.mount_is_fresh(None, "sessions", tick_ts_ms=900, min_seq=None) is False,
          "no mount observed yet is never fresh")


def test_mount_is_fresh_rejects_timestamp_before_the_tick():
    mount = {"id": "sessions", "seq": 9, "at": 500}
    check(lib.mount_is_fresh(mount, "sessions", tick_ts_ms=900, min_seq=None) is False,
          "a mount that landed before this tick's own AAS send is never fresh, "
          "regardless of seq")


# ── blink_sweep_gate_verdict (issue #193, review round 2) ──────────────────
#
# Round 1 gated on a single mount-anchored PAIR. Review injected a 400ms
# blank overlay into a view (the PR #188 shape) and ran the live check both
# ways: the fixed-delay pair AND the mount-anchored pair both measured
# ratio 0.0 -- neither could see it, because both compare two frames that
# land wholly inside or wholly outside the transient. Only the offset
# sweep's consecutive pairs (ratio 1.0 on every one) caught it. These tests
# reproduce that structurally and pin the sweep-based gate's own trap: it
# must not lose the not-measured / genuine-repaint guarantees round 1 had.

def test_blink_sweep_gate_verdict_all_identical_passes():
    frame = np.zeros((10, 10, 3), dtype=np.uint8)
    frames = [frame, frame.copy(), frame.copy(), frame.copy(), frame.copy()]
    ratio, note = lib.blink_sweep_gate_verdict(frames, seq_before_sweep=4, seq_after_sweep=4)
    check(ratio == 0.0 and note is None,
          f"a steady sweep of identical frames -> ratio 0.0, no note ({ratio}, {note!r})")
    check(lib.no_blink_ok(ratio), "and it passes the no_blink gate")


def test_blink_sweep_gate_verdict_reproduces_the_injected_flash_experiment():
    # Structural reproduction of the reviewer's live experiment: a ~400ms
    # blank overlay that starts after the sweep's first sample (200ms) and
    # resolves before its third (1000ms) -- i.e. it is fully contained
    # between two INTERIOR sweep offsets, exactly the shape a single pair
    # taken at the sweep's own first and last sample would miss entirely.
    normal = np.full((10, 10, 3), 100, dtype=np.uint8)
    flash = np.zeros((10, 10, 3), dtype=np.uint8)   # the injected blank overlay
    # offsets:                 200      500     1000    1500    2000
    frames = [normal, flash, normal, normal, normal]

    # The instrument round 1 (and master, before it) used: a single pair,
    # sampled at the sweep's own first and last point -- both land OUTSIDE
    # the transient here, so it reads a clean pass. This is exactly what
    # the reviewer measured against the real product (ratio 0.0 both ways).
    single_pair_ratio, _ = lib.blink_check(frames[0], frames[-1])
    check(single_pair_ratio == 0.0,
          f"a single first/last pair is blind to a transient between two interior "
          f"samples (ratio {single_pair_ratio}) -- reproducing the reviewer's finding")

    # The sweep-based gate must NOT be blind to it: two of its own
    # consecutive pairs straddle the transient's edges.
    ratio, note = lib.blink_sweep_gate_verdict(frames, seq_before_sweep=1, seq_after_sweep=1)
    check(ratio is not None and ratio > 0.5,
          f"the sweep gate's worst consecutive-pair ratio catches the flash ({ratio})")
    check(not lib.no_blink_ok(ratio),
          "...and it fails the no_blink gate, unlike the single pair above")


def test_blink_sweep_gate_verdict_seq_advance_mid_sweep_is_never_a_pass():
    # Same not-measured contract as round 1's pair verdict, now for the
    # sweep: tested with IDENTICAL frames (the case an implementation could
    # wrongly special-case as "pixels agree, so ratio 0.0 is fine").
    frame = np.zeros((10, 10, 3), dtype=np.uint8)
    frames = [frame.copy() for _ in range(5)]
    ratio, note = lib.blink_sweep_gate_verdict(frames, seq_before_sweep=5, seq_after_sweep=6)
    check(ratio is None,
          f"a mid-sweep sequence advance yields ratio=None even with identical pixels (got {ratio})")
    check(note is not None and "not measured" in note,
          f"...with an explanatory 'not measured' note ({note!r})")
    check(not lib.no_blink_ok(ratio),
          "None can never satisfy no_blink_ok -- 'not measured' is never read as a pass")


def test_blink_sweep_gate_verdict_genuine_diff_with_steady_seq_still_fails():
    a = np.full((100, 100, 3), 50, dtype=np.uint8)
    b = a.copy()
    b[0:10, :] = 255
    frames = [a, a.copy(), b, b.copy(), b.copy()]  # a real change mid-sweep
    ratio, note = lib.blink_sweep_gate_verdict(frames, seq_before_sweep=9, seq_after_sweep=9)
    check(ratio is not None and ratio > 0.05,
          f"a genuine repaint with an UNCHANGED sequence still yields a high ratio ({ratio})")
    check(not lib.no_blink_ok(ratio), "...and still fails the gate")


def test_blink_sweep_gate_verdict_missing_frame_note_surfaces_alongside_the_max():
    a = np.full((10, 10, 3), 50, dtype=np.uint8)
    b = a.copy()
    b[0, 0:5] = 255  # a real, larger diff than the missing-frame pairs
    frames = [a, None, a.copy(), b]
    ratio, note = lib.blink_sweep_gate_verdict(frames, seq_before_sweep=0, seq_after_sweep=0)
    check(ratio == 1.0,
          f"a missing frame is still the worst-case ratio ({ratio})")
    check(note is not None and "missing" in note,
          f"the missing-frame note is not dropped just because it wasn't the sole cause ({note!r})")


def test_blink_sweep_gate_verdict_fewer_than_two_frames_is_worst_case():
    ratio, note = lib.blink_sweep_gate_verdict([None], seq_before_sweep=0, seq_after_sweep=0)
    check(ratio == 1.0 and note is not None,
          f"a degenerate sweep (< 2 frames) is worst-case, never a silent pass ({ratio}, {note!r})")
    ratio2, note2 = lib.blink_sweep_gate_verdict([], seq_before_sweep=0, seq_after_sweep=0)
    check(ratio2 == 1.0 and note2 is not None,
          f"an EMPTY sweep is worst-case too, not a crash or an implicit 0.0 ({ratio2}, {note2!r})")


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


def _array_to_data_url(arr):
    """Mirrors a browser's canvas.toDataURL('image/png') for a test-built
    array -- ui_live_smoke.py's _ATOMIC_PANEL_SNAPSHOT_JS produces the real
    thing from a live <canvas>; this is the same encoding, built here so the
    verdict logic can be exercised without a browser."""
    buf = io.BytesIO()
    Image.fromarray(arr).save(buf, format="PNG")
    return "data:image/png;base64," + base64.b64encode(buf.getvalue()).decode("ascii")


def test_data_url_to_array_roundtrip():
    arr = np.zeros((8, 8, 3), dtype=np.uint8)
    arr[3, 3] = [9, 99, 199]
    decoded = lib.data_url_to_array(_array_to_data_url(arr))
    check(np.array_equal(decoded, arr),
          "data_url_to_array decodes a canvas.toDataURL()-shaped string losslessly")


def test_data_url_to_array_rejects_non_png_data_url():
    threw = False
    try:
        lib.data_url_to_array("not a data url")
    except ValueError:
        threw = True
    check(threw, "data_url_to_array raises ValueError on a malformed data URL")


# ── blind_window_ok (issue #142) ─────────────────────────────────────────────

def test_blind_window_ok_element_gone():
    ok, reason = lib.blind_window_ok({"present": False})
    check(not ok and reason == "panel element gone",
          f"an absent panel element fails with the expected reason (got {ok!r}, {reason!r})")


def test_blind_window_ok_canvas_with_real_content_passes():
    frame = np.zeros((20, 20, 3), dtype=np.uint8)
    frame[5, :] = 255  # stand-in for a rendered chart line
    snapshot = {"present": True, "canvas": True, "dataURL": _array_to_data_url(frame)}
    ok, reason = lib.blind_window_ok(snapshot)
    check(ok, f"a canvas with real painted content passes (reason={reason!r})")


def test_blind_window_ok_catches_an_injected_blank_canvas():
    # issue #142 acceptance: prove the check still goes RED on a genuine
    # blank -- a canvas painted a single solid colour, exactly what a
    # sustained (not merely atomically-torn) teardown-to-blank would produce.
    blank = np.full((20, 20, 3), 30, dtype=np.uint8)
    snapshot = {"present": True, "canvas": True, "dataURL": _array_to_data_url(blank)}
    ok, reason = lib.blind_window_ok(snapshot)
    check(not ok and reason == "panel went blank",
          f"a solid-colour canvas still fails the check (got {ok!r}, {reason!r})")


def test_blind_window_ok_table_tab_with_rows_passes():
    ok, reason = lib.blind_window_ok(
        {"present": True, "canvas": False, "hasContent": True})
    check(ok, f"a table panel with content passes (reason={reason!r})")


def test_blind_window_ok_table_tab_empty_fails():
    ok, reason = lib.blind_window_ok(
        {"present": True, "canvas": False, "hasContent": False})
    check(not ok and reason == "panel has no content",
          f"an empty table panel fails with the expected reason (got {ok!r}, {reason!r})")


def test_blind_window_ok_never_races_a_stale_screenshot_handle():
    # The bug this whole mechanism replaces (issue #142): the OLD check used
    # two separate CDP round trips (query_selector, then a later .screenshot()
    # call) and treated ANY exception from the second call as "gone", even
    # though the element demonstrably existed a moment earlier. The new
    # atomic snapshot has no such second call to race -- a present, painted
    # canvas is read in the SAME evaluate() as the presence check, so there
    # is no representable "present but its content is unknown/stale" shape
    # for blind_window_ok to misjudge.
    frame = np.full((20, 20, 3), 200, dtype=np.uint8)
    frame[0:2, 0:2] = 0
    snapshot = {"present": True, "canvas": True, "dataURL": _array_to_data_url(frame)}
    ok, _reason = lib.blind_window_ok(snapshot)
    check(ok, "a present, painted canvas is never misjudged as gone/blank")


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


# ── blink_not_measured / all-not-measured fail-by-construction (issue #193) ─

def test_build_tab_result_records_not_measured_ticks():
    r = lib.build_tab_result(
        "sessions", True, "ok:8", ticks_observed=6, console_errors=[],
        blink_ratio=0.0, color_violations=[],
        leak_before={"charts": 1, "uplots": 1, "pending": 0},
        leak_after={"charts": 1, "uplots": 1, "pending": 0},
        artifacts={},
        blink_pair_offsets_ms=[5100, 1200, 3400, 5200, 5150, 5180],  # 6 attempted
        blink_not_measured=[{"tick": 3, "reason": "mount sequence advanced mid-capture"}])
    check(r["no_blink"]["not_measured"] ==
          [{"tick": 3, "reason": "mount sequence advanced mid-capture"}],
          f"not-measured ticks are visible in summary.json, not dropped ({r['no_blink']})")
    check(r["no_blink"]["measured"] ==
          {"ok": True, "measured_count": 5, "attempted_count": 6, "min_fraction": 0.5},
          f"5 of 6 attempted ticks measured clears the 50% floor ({r['no_blink']['measured']})")
    check(r["ok"] is True,
          "one not-measured tick among otherwise-measured ones does not by itself fail the tab")


# ── measured_ok / MIN_MEASURED_FRACTION (issue #193 review round 2 SHOULD-FIX)

def test_build_tab_result_mostly_not_measured_fails_even_with_a_perfect_ratio():
    # The reviewer's literal finding: "a tab can discard five of six ticks
    # and still report ok with ratio 0.0." blink_ratio=0.0 here is exactly
    # that one measured tick's own (genuinely clean) ratio -- this must now
    # fail on COVERAGE, not on the ratio.
    r = lib.build_tab_result(
        "sessions", True, "ok:8", ticks_observed=6, console_errors=[],
        blink_ratio=0.0, color_violations=[],
        leak_before={"charts": 1, "uplots": 1, "pending": 0},
        leak_after={"charts": 1, "uplots": 1, "pending": 0},
        artifacts={},
        blink_pair_offsets_ms=[5100, 1200, 3400, 5200, 5150, 5180],  # 6 attempted
        blink_not_measured=[{"tick": i, "reason": "mid-sweep advance"} for i in (1, 3, 4, 5, 6)])
    check(r["no_blink"]["ok"] is True,
          "the ratio itself is clean (0.0 < threshold) -- this is NOT a blink failure")
    check(r["no_blink"]["measured"]["ok"] is False,
          f"but only 1 of 6 attempted ticks was measured, below the 50% floor "
          f"({r['no_blink']['measured']})")
    check(r["ok"] is False,
          "...so the tab fails overall -- a near-blind tick loop must not read as ok")


def test_build_tab_result_measured_fraction_exactly_at_the_floor_passes():
    r = lib.build_tab_result(
        "sessions", True, "ok:8", ticks_observed=6, console_errors=[],
        blink_ratio=0.0, color_violations=[],
        leak_before={"charts": 1, "uplots": 1, "pending": 0},
        leak_after={"charts": 1, "uplots": 1, "pending": 0},
        artifacts={},
        blink_pair_offsets_ms=[5100, 1200, 3400, 5200, 5150, 5180],  # 6 attempted
        blink_not_measured=[{"tick": i, "reason": "mid-sweep advance"} for i in (1, 2, 3)])
    check(r["no_blink"]["measured"] ==
          {"ok": True, "measured_count": 3, "attempted_count": 6, "min_fraction": 0.5},
          f"exactly 50% measured clears the floor (>=, not >) ({r['no_blink']['measured']})")
    check(r["ok"] is True, "and the tab passes")


def test_build_tab_result_measured_fraction_just_below_the_floor_fails():
    r = lib.build_tab_result(
        "sessions", True, "ok:8", ticks_observed=7, console_errors=[],
        blink_ratio=0.0, color_violations=[],
        leak_before={"charts": 1, "uplots": 1, "pending": 0},
        leak_after={"charts": 1, "uplots": 1, "pending": 0},
        artifacts={},
        blink_pair_offsets_ms=[5100, 1200, 3400, 5200, 5150, 5180, 5090],  # 7 attempted
        blink_not_measured=[{"tick": i, "reason": "mid-sweep advance"} for i in (1, 2, 3, 4)])
    check(r["no_blink"]["measured"]["ok"] is False,
          f"3 of 7 (~42.9%) is just below the 50% floor ({r['no_blink']['measured']})")
    check(r["ok"] is False, "and the tab fails")


def test_build_tab_result_zero_attempted_ticks_does_not_fail_on_coverage_alone():
    # A caller that doesn't populate blink_pair_offsets_ms at all (existing
    # tests, or a code path that never reached the tick loop) must not be
    # penalised by measured_ok -- ticks_ok already fails that case for
    # having no ticks, so this must not become a SECOND, contradictory
    # reason a legitimately-untested result looks wrong.
    r = lib.build_tab_result(
        "overview", True, "ok:8", ticks_observed=0, console_errors=[],
        blink_ratio=None, color_violations=[],
        leak_before={"charts": 1, "uplots": 1, "pending": 0},
        leak_after={"charts": 1, "uplots": 1, "pending": 0}, artifacts={})
    check(r["no_blink"]["measured"]["ok"] is True,
          "zero attempted ticks trivially satisfies measured_ok")
    check(r["ok"] is False, "but the tab still fails, via ticks_ok/blink_ok, not double-counted")


def test_build_tab_result_not_measured_defaults_empty():
    r = lib.build_tab_result(
        "overview", True, "ok:8", ticks_observed=6, console_errors=[],
        blink_ratio=0.0, color_violations=[],
        leak_before={"charts": 1, "uplots": 1, "pending": 0},
        leak_after={"charts": 1, "uplots": 1, "pending": 0}, artifacts={})
    check(r["no_blink"]["not_measured"] == [],
          "blink_not_measured defaults to an empty list, not missing/None")


def test_build_tab_result_all_ticks_not_measured_fails_the_gate():
    # issue #193: "empty ratios already fail by construction in the existing
    # code" -- the previous agent VERIFIED this but did not test it. This is
    # that test: the caller (run_tab) computes blink_ratio as
    # max(measured-only ratios), which is None when every tick landed as
    # not-measured -- pin that None reaches here and still fails, so a run
    # where NOTHING was ever actually measured cannot pass by omission.
    r = lib.build_tab_result(
        "sessions", True, "ok:8", ticks_observed=6, console_errors=[],
        blink_ratio=None, color_violations=[],
        leak_before={"charts": 1, "uplots": 1, "pending": 0},
        leak_after={"charts": 1, "uplots": 1, "pending": 0},
        artifacts={},
        blink_not_measured=[{"tick": i, "reason": "mid-capture advance"}
                            for i in range(1, 7)])
    check(r["no_blink"]["ok"] is False,
          "an all-not-measured tab's no_blink check is False, never an implicit pass")
    check(r["ok"] is False,
          "...and that fails the whole tab (a gate that cannot see must refuse, never approve)")


def test_build_failed_tab_result_not_measured_empty():
    r = lib.build_failed_tab_result("waterfall", "panel did not render within 60s")
    check(r["no_blink"]["not_measured"] == [],
          "a tab that never reached the tick loop has no not-measured ticks to report")


# ── pre_mount_diagnostic passthrough (issue #100, review round 4) ──────────

def test_build_tab_result_records_pre_mount_diagnostic():
    entries = [{"tick": 1, "diff_pixel_frac": 0.045, "bottom_band_pixel_frac": 0.6,
                "rest_pixel_frac": 0.01, "columns_touched_frac": 0.2, "note": None}]
    r = lib.build_tab_result(
        "timeline", True, "ok:8", ticks_observed=6, console_errors=[],
        blink_ratio=0.0, color_violations=[],
        leak_before={"charts": 1, "uplots": 1, "pending": 0},
        leak_after={"charts": 1, "uplots": 1, "pending": 0},
        artifacts={}, pre_mount_diagnostics=entries)
    check(r["pre_mount_diagnostic"] == entries,
          f"pre-mount diagnostic entries carried through unchanged ({r['pre_mount_diagnostic']})")
    check(r["ok"] is True,
          "the pre-mount diagnostic is reporting-only -- never affects the gating verdict")


def test_build_tab_result_pre_mount_diagnostic_defaults_empty():
    r = lib.build_tab_result(
        "overview", True, "ok:8", ticks_observed=6, console_errors=[],
        blink_ratio=0.0, color_violations=[],
        leak_before={"charts": 1, "uplots": 1, "pending": 0},
        leak_after={"charts": 1, "uplots": 1, "pending": 0}, artifacts={})
    check(r["pre_mount_diagnostic"] == [],
          "pre_mount_diagnostics defaults to an empty list for every non-timeline tab")


def test_build_failed_tab_result_pre_mount_diagnostic_empty():
    r = lib.build_failed_tab_result("timeline", "panel did not render within 60s")
    check(r["pre_mount_diagnostic"] == [],
          "a tab that never reached the tick loop has no pre-mount diagnostic to report")


def test_build_tab_result_records_blink_sweep():
    a = np.zeros((10, 10, 3), dtype=np.uint8)
    # panel_dims must match the frames' own 10x10 shape (frame_dims_ok,
    # this issue) -- a missing/mismatched clip would fail the tab for a
    # reason unrelated to what this test checks (that raw sweep data
    # recording doesn't ITSELF affect gating).
    tick_rec = lib.build_sweep_tick_record(
        [201, 503, 998, 1501, 2010], [a, a.copy(), a.copy(), a.copy(), a.copy()],
        panel_dims={"box_width": 10.0, "box_height": 10.0,
                    "clip_width": 10.0, "clip_height": 10.0})
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


# ── ttfp_ms (issue #245, time-to-first-paint) ───────────────────────────────

def test_build_tab_result_carries_ttfp_ms_through_unaveraged():
    """The field this branch adds: one raw per-navigation measurement,
    reaching summary.json unchanged -- never averaged, rounded, or dropped
    by build_tab_result. What would make this fail: build_tab_result
    ignoring its ttfp_ms kwarg (the field silently reading back None or the
    default) -- see this repo's report for a demonstrated-red run with the
    kwarg unwired at the caller."""
    r = lib.build_tab_result(
        "overview", True, "ok:8", ticks_observed=6, console_errors=[],
        blink_ratio=0.0001, color_violations=[],
        leak_before={"charts": 1, "uplots": 1, "pending": 0},
        leak_after={"charts": 1, "uplots": 1, "pending": 0},
        artifacts={}, ttfp_ms=842)
    check(r["ttfp_ms"] == 842,
          f"the exact measured ttfp_ms (842) reaches the tab result, got {r['ttfp_ms']!r}")


def test_build_tab_result_ttfp_ms_defaults_to_none():
    """A caller that hasn't been updated to pass ttfp_ms (or one that
    legitimately has nothing to report) must read back None, not a
    fabricated 0 -- 0 would be indistinguishable from a genuinely instant
    paint."""
    r = lib.build_tab_result(
        "overview", True, "ok:8", ticks_observed=6, console_errors=[],
        blink_ratio=0.0001, color_violations=[],
        leak_before={"charts": 1, "uplots": 1, "pending": 0},
        leak_after={"charts": 1, "uplots": 1, "pending": 0},
        artifacts={})
    check(r["ttfp_ms"] is None, "ttfp_ms defaults to None when the caller gives none")


def test_build_tab_result_ttfp_ms_never_gates_ok():
    """This branch adds measurement only (no bound, no gate, per the
    issue): an extreme ttfp_ms must never flip `ok` by itself.

    Two rebases landed new terms into `ok` right next to ttfp_ms in the
    source (issue #255/#252: capture_ok, from capture_budget_ok/
    CAPTURE_MS_BOUND_MS; issue #259 then demoted capture_ok OUT of `ok`
    and replaced it with dims_ok/spacing_ok, from frame_dims_ok/
    frame_spacing_ok) -- a single "ok is True" assertion alone would keep
    passing even if a merge had accidentally routed ttfp_ms into one of
    those terms instead of keeping it out of `ok` entirely, as long as
    that term's own trivial default (empty blink_sweep_ticks --
    frame_dims_ok([])/frame_spacing_ok([]) both trivially pass, same idiom
    as capture_budget_ok before them) still came out True. So this also
    directly compares two calls identical in EVERY OTHER argument, one
    with ttfp_ms=None and one with ttfp_ms=60000, and asserts `ok` is
    identical between them -- proof by construction that ttfp_ms's value
    cannot be what determined either result, not just that one arbitrary
    value happened to still pass. This mechanism does not name which
    fields currently gate `ok` -- it stays valid across a future rebase
    that changes them again, same as it survived #259."""
    kwargs = dict(
        tab_id="overview", rendered_ok=True, rendered_detail="ok:8",
        ticks_observed=6, console_errors=[], blink_ratio=0.0001,
        color_violations=[],
        leak_before={"charts": 1, "uplots": 1, "pending": 0},
        leak_after={"charts": 1, "uplots": 1, "pending": 0}, artifacts={})
    good = lib.build_tab_result(**kwargs, ttfp_ms=60000)
    check(good["ok"] is True,
          "a huge ttfp_ms (60000ms) does not fail an otherwise-clean tab -- "
          "this branch adds measurement, never a gate")
    baseline = lib.build_tab_result(**kwargs, ttfp_ms=None)
    check(baseline["ok"] == good["ok"],
          "an otherwise-identical call differing ONLY in ttfp_ms (None vs "
          "60000) produces the SAME `ok` -- ttfp_ms cannot be the thing "
          "deciding it either way")


def test_build_failed_tab_result_ttfp_ms_default_and_override():
    r = lib.build_failed_tab_result("timeline", "panel did not render within 60s")
    check(r["ttfp_ms"] is None,
          "a failed navigation's ttfp_ms defaults to None, never a stale/fabricated value")
    r2 = lib.build_failed_tab_result("timeline", "later step failed", ttfp_ms=310)
    check(r2["ttfp_ms"] == 310,
          "build_failed_tab_result can still carry a measured ttfp_ms when "
          "navigation itself succeeded but a later step failed")


def test_known_failing_tabs_pinned():
    # Owner-filed tracking issues -- pinned exactly so nothing else quietly
    # gets added to this dict, and so DELISTING one stays a deliberate edit
    # here too. #101 (Waterfall) was delisted once its single root cause was
    # found and fixed: the tab defaulted to the newest execution, which on a
    # real capture has no events, no workers and no plan, so the panel never
    # mounted a chart (web/static/lib/builders/waterfall.js). #100
    # (Timeline, review round 5) was delisted once its own literal claim --
    # a redraw with UNCHANGED DATA moves ~4.5% of pixels -- was refuted by a
    # real live run's own consecutive-tick frames (four straight
    # unchanged-data redraws at exactly 0.00%) with a named mechanism for
    # the original number (see KNOWN_FAILING_TABS's own comment). Empty is
    # the correct, deliberate value here, not an oversight.
    check(lib.KNOWN_FAILING_TABS == {},
          f"KNOWN_FAILING_TABS is exactly {{}} (got {lib.KNOWN_FAILING_TABS})")


def test_known_failing_issue():
    with known_failing_tabs_override({"faketab": 999}):
        check(lib.known_failing_issue("faketab") == 999, "a listed tab -> its issue number")
        check(lib.known_failing_issue("waterfall") is None,
              "waterfall is delisted (#101 fixed) and has no known-failing issue")
        check(lib.known_failing_issue("timeline") is None,
              "timeline is delisted (#100, review round 5) and has no known-failing issue")
        check(lib.known_failing_issue("overview") is None,
              "an unlisted tab has no known-failing issue")


def test_known_failing_report_line():
    with known_failing_tabs_override({"faketab": 999}):
        check(lib.known_failing_report_line("overview", False) is None,
              "an unlisted tab never gets a known-failing report line")
        fail_line = lib.known_failing_report_line("faketab", False)
        check(fail_line == "KNOWN-FAILING (issue #999)",
              f"a listed tab's real failure reports KNOWN-FAILING ({fail_line!r})")
        pass_line = lib.known_failing_report_line("faketab", True)
        check(pass_line == "UNEXPECTED PASS (issue #999) -- intermittent or fixed; check the issue",
              f"a listed tab's real pass reports UNEXPECTED PASS ({pass_line!r})")


def test_build_tab_result_known_failing_does_not_fail_summary():
    # A listed tab actually failing (raw ok=False): excused from the
    # overall verdict, but the raw failure and the known_failing flag are
    # both visible in the tab's own record.
    with known_failing_tabs_override({"faketab": 999}):
        r = lib.build_tab_result(
            "faketab", True, "ok:1", 6, [], 0.05, [],  # 5% blink -> raw fail
            {"charts": 1, "uplots": 1, "pending": 0},
            {"charts": 1, "uplots": 1, "pending": 0}, {})
        check(r["ok"] is False, "the raw per-tab result still says what really happened")
        check(r["known_failing"] is True and r["xpass"] is False,
              f"a real failure on a listed tab is known_failing, not xpass ({r})")
        s = lib.build_summary([r])
        check(s["ok"] is True and s["failed_tabs"] == [] and
              s["known_failing_tabs"] == ["faketab"],
              f"a known-failing tab's real failure does not fail the summary ({s})")


def test_build_tab_result_xpass_does_not_fail_summary_either():
    # A listed tab happening to pass this run: reported as xpass, not
    # silently absorbed, but does not fail the run -- same semantics
    # run_all.sh's own KNOWN_FAILING now uses too (an unexpected pass is
    # reported, never a gate failure by itself; a single real-daemon run
    # passing isn't proof an intermittent bug is fixed).
    with known_failing_tabs_override({"faketab": 999}):
        r = lib.build_tab_result(
            "faketab", True, "ok:1", 6, [], 0.0, [],
            {"charts": 1, "uplots": 1, "pending": 0},
            {"charts": 1, "uplots": 1, "pending": 0}, {})
        check(r["ok"] is True and r["xpass"] is True and r["known_failing"] is False,
              f"a real pass on a listed tab is xpass, not known_failing ({r})")
        s = lib.build_summary([r])
        check(s["ok"] is True and s["xpass_tabs"] == ["faketab"],
              f"an xpass tab does not fail the summary either ({s})")


def test_build_failed_tab_result_known_failing():
    # A listed tab that could not be checked at all goes through
    # build_failed_tab_result, not build_tab_result, and must still be
    # excused by its issue number.
    with known_failing_tabs_override({"faketab": 999}):
        r = lib.build_failed_tab_result(
            "faketab", "panel did not render ('#faketab-chart canvas') within 60s")
        check(r["known_failing"] is True and r["ok"] is False,
              f"a could-not-check known-failing tab is still known_failing ({r})")
        s = lib.build_summary([r])
        check(s["ok"] is True, "a known-failing could-not-check tab does not fail the summary")


def test_delisted_waterfall_failure_is_a_real_failure():
    # issue #101 regression guard: the waterfall tab is no longer excused, so
    # its old failure shape must fail the run outright. A return of the
    # empty-panel bug cannot slip through as "known failing" again. Uses
    # the REAL (un-overridden) KNOWN_FAILING_TABS deliberately.
    r = lib.build_failed_tab_result(
        "waterfall", "panel did not render ('#waterfall-chart canvas') within 60s")
    check(r["known_failing"] is False and r["ok"] is False,
          f"a waterfall render failure is a real failure again ({r})")
    s = lib.build_summary([r])
    check(s["ok"] is False and s["failed_tabs"] == ["waterfall"],
          f"a waterfall render failure fails the summary ({s})")


def test_delisted_timeline_failure_is_a_real_failure():
    # issue #100 regression guard (review round 5): timeline is no longer
    # excused, so a real blink/render failure on it must fail the run
    # outright, not disappear as "known failing" again. Uses the REAL
    # (un-overridden) KNOWN_FAILING_TABS deliberately.
    r = lib.build_failed_tab_result(
        "timeline", "panel did not render ('#timeline-chart canvas') within 60s")
    check(r["known_failing"] is False and r["ok"] is False,
          f"a timeline render failure is a real failure again ({r})")
    s = lib.build_summary([r])
    check(s["ok"] is False and s["failed_tabs"] == ["timeline"],
          f"a timeline render failure fails the summary ({s})")


def test_known_failing_does_not_affect_unlisted_tabs():
    r = lib.build_tab_result(
        "overview", True, "ok:1", 6, [], 0.0, [],
        {"charts": 1, "uplots": 1, "pending": 0},
        {"charts": 1, "uplots": 1, "pending": 0}, {})
    check(r["known_failing"] is False and r["xpass"] is False,
          "an unlisted tab always has known_failing=False, xpass=False")


def test_build_summary_mixed_known_failing_and_real_failure():
    # A KNOWN_FAILING tab failing must not mask a genuine, unlisted failure.
    with known_failing_tabs_override({"faketab": 999}):
        known = lib.build_failed_tab_result("faketab", "blink 4.46%")
        real_fail = lib.build_failed_tab_result("overview", "no rows")
        s = lib.build_summary([known, real_fail])
        check(s["ok"] is False and s["failed_tabs"] == ["overview"] and
              s["known_failing_tabs"] == ["faketab"],
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


# ── viewport_mismatch_reason (issue #223) ────────────────────────────────
# docs/DEMO_REHEARSAL_CRITERIA.md: "assert the viewport actually obtained,
# not the one requested" and "assert the effective zoom too". These cases
# are exactly the input that makes the check catch a plausible-looking but
# wrong result: a requested size/DSF that run_tab() never actually wired
# into browser.new_context(), so the page silently kept the OLD viewport
# or DPR while the caller believed it got the pinned one.

def test_viewport_mismatch_reason_matching_is_none():
    check(lib.viewport_mismatch_reason(1710, 981, 2, 1710, 981, 2) is None,
          "matching width/height/DPR: no mismatch")


def test_viewport_mismatch_reason_matching_int_vs_float_dpr():
    check(lib.viewport_mismatch_reason(1710, 981, 2, 1710, 981, 2.0) is None,
          "DPR compared with tolerance: 2 (int) vs 2.0 (float) still matches")


def test_viewport_mismatch_reason_width_drift():
    reason = lib.viewport_mismatch_reason(1710, 981, 2, 1280, 981, 2)
    check(reason is not None, "width drift is reported, not swallowed")
    check("width" in reason and "1710" in reason and "1280" in reason,
          "width reason names both the requested and actual values")


def test_viewport_mismatch_reason_height_drift():
    reason = lib.viewport_mismatch_reason(1710, 981, 2, 1710, 900, 2)
    check(reason is not None, "height drift is reported")
    check("height" in reason and "981" in reason and "900" in reason,
          "height reason names both values")


def test_viewport_mismatch_reason_zoom_drift_size_still_matches():
    # This is exactly the trap the criteria doc calls out: outer window
    # size is right (1710x981) but the effective zoom/DPR silently stayed
    # at the OLD value -- a harness that only checks width/height would
    # report a clean PASS here.
    reason = lib.viewport_mismatch_reason(1710, 981, 2, 1710, 981, 1)
    check(reason is not None,
          "zoom/DPR drift is caught even when width/height are correct")
    check("zoom" in reason or "devicePixelRatio" in reason,
          "zoom reason names the DPR field, not just 'mismatch'")


def test_viewport_mismatch_reason_reports_every_mismatched_field():
    reason = lib.viewport_mismatch_reason(1710, 981, 2, 1280, 900, 1)
    check(reason is not None, "multiple drifts still reported")
    check("width" in reason, "combined reason still names width")
    check("height" in reason, "combined reason still names height")
    check("zoom" in reason or "devicePixelRatio" in reason,
          "combined reason still names zoom/DPR")


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
