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
