#!/usr/bin/env python3
"""ui_live_smoke_lib.py -- pure logic for tests/ui_live_smoke.py (issue #93).

Kept separate from the Playwright driver so the per-tab verdict logic (frame
diff ratio, legend-colour stability, leak-probe interpretation, summary
assembly) has a fast, browser-free unit test (tests/test_ui_live_smoke_lib.py)
instead of being buried inside async page-driving code.

Nothing in this module touches a browser, a socket, or the filesystem beyond
optionally decoding a PNG already on disk (load_png_array / compare_png_files)
-- everything else is plain data in, plain data out.
"""
import base64
import io
import json
import os
import shutil

import numpy as np
from PIL import Image

# The 11 tabs in the order they appear in web/static/index.html.
TABS = [
    "overview", "events", "sessions", "queries", "histogram", "timeline",
    "transitions", "concurrency", "waterfall", "scatter", "matrix",
]

# Tabs whose panel is a table (rendered check = "has rows").
TABLE_TABS = {"overview", "events", "sessions", "queries"}

# Live tick cadence the real app uses (app.js startAutoRefresh: 5000ms). The
# orchestration waits for at least MIN_TICKS ticks per tab using this as the
# per-tick timeout budget, never a blind sleep multiple.
TICK_INTERVAL_S = 5
MIN_TICKS = 6

# No-data fail-safe (issue #93 acceptance item 6): the UI must show real data
# within this many seconds of connecting, or the run FAILS (never a skip).
FIRST_DATA_TIMEOUT_S = 60

# STABILITY threshold (issue #93 acceptance item 3 / docs/VISUAL_CHECKLIST.md):
# two frames captured inside the same data window must differ by < 0.1% of
# pixels. The only source of legitimate diff between two same-tick frames is
# antialiasing of the live cursor/axis-pointer; a real re-render, redraw, or
# data change produces far more than 0.1%. This is the ONLY threshold in this
# module -- CLAUDE.md forbids widening it, so nothing here reads an env var
# to relax it; the --blink-threshold CLI flag on ui_live_smoke.py exists only
# for the deliberate-failure demo in the issue's validation step and is never
# passed by tests/ui_live_smoke.sh.
BLINK_THRESHOLD = 0.001  # 0.1%

# Offset sweep (issue #119): offsets in ms after the sweep's own anchor at
# which the driver grabs an EXTRA frame. Originally anchored on each tick's
# AAS-request timestamp and reporting-only; issue #193 (review round 2) made
# this the GATING instrument instead (blink_sweep_gate_verdict, graded on
# the WORST consecutive-pair ratio) and re-anchored it on the ViewManager
# mount event for the tab instead of the tick's AAS send -- a single
# anchored pair (round 1's own first attempt) was proven blind to a
# transient narrower than its own two-frame window (an injected 400ms
# overlay: ratio 0.0 on the pair, 1.0 on every sweep offset); these values
# are unchanged from the issue #119 gate-box evidence that picked them
# (timeline's blink, #100, was never measured this early with the OLD
# anchor: a single pair alone only ever sampled ~1.2-1.3s after the tick,
# drifting to 1.8-3.0s under load) -- still the right spread now that the
# zero point is honest, still BLINK_THRESHOLD, still no widening.
SWEEP_OFFSETS_MS = (200, 500, 1000, 1500, 2000)

# issue #193 (review round 2, SHOULD-FIX): a tab that discarded most of its
# ticks as NOT MEASURED (blink_sweep_gate_verdict's ratio=None case) must
# not report `ok` on whatever fraction it did manage to measure -- found in
# review: a tab that measured 1 of 6 ticks at ratio 0.0 and discarded the
# other 5 as not-measured passed anyway, on essentially no signal. At least
# this fraction of a tab's ATTEMPTED ticks (see build_tab_result) must
# actually be measured for the tab to pass at all; this is a coverage
# floor, not the blink ratio threshold above -- BLINK_THRESHOLD is
# untouched.
MIN_MEASURED_FRACTION = 0.5

# issue #252 review round 1 finding 1 introduced CAPTURE_MS_BOUND_MS/
# capture_budget_ok as a GATE on raw capture_ms_total_ms wall-clock time
# (merged 44812c9). It was mis-specified: its own comment cited "~600ms/
# sweep" as the regression regime, but that number is actually the OLD
# DPR1 walk's HEALTHY cost (scale="css" changes nothing at DPR1;
# .claude/worktrees/aas-agreement-and-retention/tests/results/ui_live/
# summary.json: 302-545ms/sweep, 55-135ms/frame). The bound (800ms) was
# red on an unmodified, healthy master within a day of merging: gate-1
# measured 374-1156ms/sweep (window-clip-aggregates worktree's own
# summary.json, same path pattern), closest miss "overview" at 802.7ms.
#
# Demoted to reporting-only (this issue): capture_ms/capture_ms_total_ms
# stay in the artifact as evidence, and CAPTURE_MS_BOUND_MS stays as the
# number a human skims against, but capture_budget_ok's result is no
# longer read into build_tab_result's `ok`. What raw capture time was
# actually trying to protect -- "did the sweep stop landing where
# SWEEP_OFFSETS_MS says it should" -- is now gated directly by
# frame_spacing_ok (achieved-vs-target drift) and frame_dims_ok (decoded
# frame size vs the clip's own CSS dims), below. Do not re-add this to
# `ok`: a slow-but-correctly-sized, correctly-spaced capture is not the
# defect either of those two exists to catch, and this bound's own
# calibration history (above) is why guessing a wall-clock number instead
# of measuring the actual failure mode doesn't work.
CAPTURE_MS_BOUND_MS = 800.0

# KNOWN_FAILING_TABS: tab name -> tracking issue number. ONLY for a tab that
# reproduces a real, filed product bug (issue #100, #101) -- never for timing
# or runner noise; a noisy tab is investigated, never silenced here (see also
# CLAUDE.md's Rules / tests/run_all.sh's KNOWN_FAILING -- the same mechanism,
# and now the same semantics too: neither a real failure nor an unexpected
# pass fails the gate. A listed tab still runs every check and keeps its
# artifacts; both outcomes are reported loudly (`known_failing`/`xpass` on
# the tab, `KNOWN-FAILING (issue #N)` / `UNEXPECTED PASS (issue #N) --
# intermittent or fixed; check the issue` in the driver's output) so a human
# decides when to delist a tab, not a single run's outcome either way.
KNOWN_FAILING_TABS = {
    # #100 (timeline) is DELISTED (review round 5). The issue's literal
    # claim is that a redraw with UNCHANGED DATA moves ~4.5% of pixels.
    #
    # Round 4 first tried to test this with a NEW instrument
    # (ui_live_smoke.py's pre_mount_diagnostic: diff the panel immediately
    # before this tick's own mount against the sweep's first frame after
    # it) on a live persistent-box run (root@2.28.45.47, run.id
    # 1790542465). It read diff_pixel_frac 0.0 on all 6 ticks -- WRONGLY
    # read as "no window-advance repaint occurred". It was invalid: the
    # probe captured its "pre-mount" frame with no proof it fired before
    # the tick's own mount (timeline's mount can land ~150ms after the
    # tick), so a null reading from it is not evidence of anything. That
    # probe has since been fixed (see its own docstring) to report NOT
    # MEASURED rather than a misleading 0.0 when it cannot establish it
    # fired first.
    #
    # The actual evidence, from the SAME run's saved per-tick frames
    # (tests/results/ui_live/timeline/tick-*.png), diffing consecutive
    # ticks against each other instead:
    #   tick1->2, 2->3, 3->4: EXACTLY 0.00% each -- four consecutive
    #     UNCHANGED-DATA redraws, zero pixels moved. Directly refutes the
    #     issue's literal claim.
    #   tick4->5: 1.22%, tick5->6: 1.26%, both concentrated in cols
    #     134-1219 / rows 30-165 -- the axis-label/gridline band. The DATA
    #     (the displayed window) genuinely changed there; this is not a
    #     same-window blink.
    # The original 4.46% has a named mechanism, not just an alternative
    # explanation: the OLD fixed-delay pair sampled ~1.2s after the tick,
    # while timeline's own mount ran 0.6-1.8s late under load -- a mount
    # landing inside that pair's ~120ms window paints the tick's
    # ALREADY-ADVANCED window and gets scored as a same-window blink. And
    # the issue's own 2026-09-17 08:12 diff mask already showed this,
    # independent of any later run: axis labels moving 04:58:18 ->
    # 04:58:29 IS the data changing, refuting "unchanged data" from the
    # original artifact itself. No product change ever fixed this either:
    # timeline.js's last change is bdf07ed (#106); #171/11fdf0b touched
    # only exec-scatter, matrix and waterfall.
    #
    # Residual, tracked separately (a freshness defect, not a stability
    # one, and out of this issue's scope): the same run shows timeline's
    # window sitting still for ticks 1-4 (~20s) before advancing 5s per
    # tick from tick 5 on.
    #
    # #101 (waterfall) is DELISTED. Both symptoms filed under it -- "no
    # echarts instance" and "#waterfall-chart canvas never appeared within
    # 60s" -- were ONE root cause, and it was never the executions query.
    # Measured on a real --mode full capture (gate box, PG18, pgbench 4
    # clients --rate=25): executions answers in 63 ms over 49,964
    # executions, but the tab defaulted to rows[0] -- the NEWEST execution,
    # which was undrawable (no events, no workers, no plan) in 40 of 40
    # simulated live ticks. Its execution_detail is {leader:{events:[]},
    # workers:[], plan:null}, buildWaterfallOption returns hasData:false,
    # and the view mounts no chart, so both the per-tick check and the
    # ready-selector wait fail. The default selection now picks the newest
    # execution that actually has a waterfall (~45-52 of the 100 returned
    # rows qualified at every one of those ticks).
}


def png_bytes_to_array(data):
    """Decode in-memory PNG bytes (e.g. Playwright's page.screenshot()
    return value) to an (H, W, 3) uint8 RGB array."""
    with Image.open(io.BytesIO(data)) as im:
        return np.array(im.convert("RGB"))


def load_png_array(path):
    """Decode a PNG file to an (H, W, 3) uint8 RGB array."""
    with open(path, "rb") as f:
        return png_bytes_to_array(f.read())


def data_url_to_array(data_url):
    """Decode a `canvas.toDataURL('image/png')` string (issue #142's
    ui_live_smoke.py _ATOMIC_PANEL_SNAPSHOT_JS) to an (H, W, 3) uint8 RGB
    array, reusing png_bytes_to_array. Raises ValueError on anything that
    isn't a 'data:image/png;base64,...' URL -- never silently returns a
    placeholder for a malformed capture."""
    prefix = "data:image/png;base64,"
    if not data_url.startswith(prefix):
        raise ValueError(f"not a PNG data URL: {data_url[:32]!r}...")
    return png_bytes_to_array(base64.b64decode(data_url[len(prefix):]))


def frame_diff_ratio(frame_a, frame_b):
    """Fraction of pixels that differ (any channel) between two same-shape
    (H, W, C) uint8 arrays. 0.0 = pixel-identical."""
    if frame_a.shape != frame_b.shape:
        raise ValueError(
            f"frame shape mismatch: {frame_a.shape} vs {frame_b.shape}")
    if frame_a.size == 0:
        raise ValueError("empty frame")
    diff = np.any(frame_a != frame_b, axis=-1)
    return float(np.count_nonzero(diff)) / diff.size


def clip_rect_to_viewport(x, y, width, height, viewport_width, viewport_height):
    """Intersects a (x, y, width, height) rect (CSS px, viewport-relative --
    exactly what Playwright's elementHandle.boundingBox() returns) with the
    current viewport (0, 0, viewport_width, viewport_height). Returns a
    {"x", "y", "width", "height"} dict usable as page.screenshot(clip=...),
    or None if the rect has no positive-area intersection with the viewport
    at all (panel scrolled fully out of view) -- a zero/negative-size clip
    is a Playwright error, not a legitimate empty capture, so the caller
    must treat this the same as "panel gone" (see _safe_panel_screenshot).

    Pure/testable: this is the arithmetic issue #197's fix hangs on --
    capturing only the viewport-visible slice of a panel instead of
    Playwright's own element screenshot, which silently expands the capture
    region (and the render cost with it) to the FULL element even when most
    of it is scrolled off-screen. Measured on the gate box: the Sessions
    panel at 205 rows is ~5793px tall; a whole-element capture cost ~1.4s,
    so the blink sweep's 5 frames (SWEEP_OFFSETS_MS spans 200-2000ms) took
    longer than the 5s live tick they were meant to sample inside of --
    every sweep straddled a mount by construction, reporting a red the UI
    never earned. Clipping to the viewport-visible slice makes one frame's
    cost roughly constant regardless of row count."""
    x0 = max(0.0, x)
    y0 = max(0.0, y)
    x1 = min(float(viewport_width), x + width)
    y1 = min(float(viewport_height), y + height)
    if x1 <= x0 or y1 <= y0:
        return None
    return {"x": x0, "y": y0, "width": x1 - x0, "height": y1 - y0}


def panel_capture_clip(box, viewport):
    """Decides _safe_panel_screenshot's clip rect, or None if a safe clip
    cannot be determined -- the caller must treat None here EXACTLY like
    every other "cannot capture" case (missing panel, detached element),
    NEVER fall back to an unclipped capture.

    issue #197 review (fail-safe finding): the first version of this fix
    fell back to panel.screenshot() -- the full-element, unbounded-cost
    capture this issue exists to remove -- whenever `viewport` was None.
    Unreachable with today's call site (run_tab's context always sets an
    explicit viewport), but the SHAPE was the problem: "unreachable today"
    is a property of the call site, not of this function, and the next
    refactor that adds mobile emulation or calls set_viewport_size() would
    silently reintroduce the exact cost regression, with nothing in
    summary.json to show it happened. So: no viewport -> None, same as no
    box (the panel's own elementHandle.boundingBox() came back empty) --
    both are "cannot safely capture", not "capture unboundedly instead".

    box: the panel's boundingBox() dict ({"x","y","width","height"}) or
    None. viewport: page.viewport_size ({"width","height"}) or None."""
    if box is None or viewport is None:
        return None
    return clip_rect_to_viewport(box["x"], box["y"], box["width"], box["height"],
                                 viewport["width"], viewport["height"])


def mount_is_fresh(mount, tab_id, tick_ts_ms, min_seq):
    """True if `mount` ({"id", "seq", "at"} or None) is usable as THIS
    tick's blink-sweep anchor.

    issue #197's second defect: a mount whose own refresh cycle takes
    longer than the 5s live tick interval can land with `at` already past
    the NEXT tick's own AAS-send timestamp -- so a caller that only checks
    `mount.at >= tick_ts_ms` (the original issue #193 round-2 contract)
    can be handed the SAME mount, already consumed by the PREVIOUS tick's
    own sweep, for this tick too. _capture_at_offset computes its sleep
    from that mount's (already old) `at`, so every target offset is
    already in the past and all 5 frames fire back-to-back with no
    inter-frame spacing -- exactly the spread that lets the sweep see a
    sub-second transient at all is gone on that iteration, and
    MIN_MEASURED_FRACTION's denominator still silently counts it as
    measured (evidence: achieved first offsets of 2.5-4.5s instead of the
    ~200ms target, one live mount sampled multiple times -- same tick_ts,
    same seq, same pixels).

    Adds a THIRD condition on top of id/timestamp: `mount.seq` must be
    STRICTLY GREATER than `min_seq`, the seq the previous tick's own sweep
    already anchored to (None for the first tick of a tab's walk, which has
    no previous seq to be newer than -- same reasoning _wait_for_mount_at_or_after
    already uses for why tick 1 has no timestamp baseline to carry either).
    A mount reused across two ticks fails this by construction; the caller
    (_wait_for_mount_at_or_after) keeps polling until a genuinely new one
    lands, rather than sweeping the stale one."""
    if mount is None or mount["id"] != tab_id:
        return False
    if mount["at"] < tick_ts_ms:
        return False
    if min_seq is not None and mount["seq"] <= min_seq:
        return False
    return True


def blink_check(frame_a, frame_b):
    """frame_diff_ratio(), except a PANEL RESIZE between the two frames
    (e.g. a table still growing rows a few hundred ms after its first row
    appeared -- observed for real against a real daemon under real load;
    tests/mock_server.py's fixed row counts never exercise this) is treated
    as the worst possible instability (ratio 1.0) instead of raising --
    the panel visibly changing shape mid-"steady state" IS a no_blink
    failure, never a crash that drops the rest of the tab's ticks.

    Returns (ratio, note); note is None when the shapes matched."""
    if frame_a.shape != frame_b.shape:
        return 1.0, f"panel resized between frames: {frame_a.shape} -> {frame_b.shape}"
    return frame_diff_ratio(frame_a, frame_b), None


def frame_diff_signature(frame_a, frame_b, edge_band_frac=0.15):
    """issue #100 (timeline): characterizes WHERE two same-shape frames
    differ, to test the issue's premise against a NEW, honest instrument
    instead of re-running the artifact-producing one. #100 claims timeline
    redraws ~4.5% of pixels "with unchanged data"; the issue's own
    2026-09-17 08:12 diff mask instead shows axis labels advancing and
    gridlines shifting -- i.e. the tick's own legitimate window-advance
    repaint (web/static/lib/builders/timeline.js's xAxis sits at the
    BOTTOM of the grid: axisLabel + axisLine), not a bug, caught mid-flight
    by the OLD 1200ms-after-tick anchor. This signature exists to tell that
    shape apart from a generic/uniform repaint (a real new render) or a
    teardown-to-blank flash (touches nearly everything):

      - a window-advance repaint: heavy diff in the BOTTOM edge band
        (labels + axis line), light diff above it, and only a FEW columns
        touched overall (a handful of gridlines shifting a few px, not a
        wholesale re-render).
      - a generic full repaint: diff roughly even between the bottom band
        and the rest, most columns touched.

    Returns a dict (never raises on a shape mismatch -- same worst-case
    idiom as blink_check, but there is no single "ratio" to cap at 1.0
    here, so the note says so and every OTHER field is None):
      {"diff_pixel_frac", "bottom_band_pixel_frac", "rest_pixel_frac",
       "columns_touched_frac", "note"}
    diff_pixel_frac/bottom_band_pixel_frac/rest_pixel_frac are each "of the
    pixels IN THAT REGION, what fraction differ" (comparable across panels
    of different sizes); columns_touched_frac is "of all columns, what
    fraction contain at least one differing pixel"."""
    if frame_a.shape != frame_b.shape:
        return {"diff_pixel_frac": None, "bottom_band_pixel_frac": None,
                "rest_pixel_frac": None, "columns_touched_frac": None,
                "note": f"panel resized between frames: {frame_a.shape} -> {frame_b.shape}"}
    h = frame_a.shape[0]
    band_h = max(1, int(round(h * edge_band_frac)))
    diff = np.any(frame_a != frame_b, axis=-1)  # (H, W) bool
    bottom = diff[h - band_h:, :]
    rest = diff[:h - band_h, :]
    columns_touched = np.any(diff, axis=0)  # (W,) bool -- any row differs in this column
    return {
        "diff_pixel_frac": float(np.count_nonzero(diff)) / diff.size,
        "bottom_band_pixel_frac": (float(np.count_nonzero(bottom)) / bottom.size
                                    if bottom.size else 0.0),
        "rest_pixel_frac": (float(np.count_nonzero(rest)) / rest.size
                             if rest.size else 0.0),
        "columns_touched_frac": float(np.count_nonzero(columns_touched)) / columns_touched.size,
        "note": None,
    }


_PRE_MOUNT_NOT_MEASURED = {
    "diff_pixel_frac": None, "bottom_band_pixel_frac": None,
    "rest_pixel_frac": None, "columns_touched_frac": None,
}


def pre_mount_diagnostic_verdict(pre_mount_frame, sweep_first_frame,
                                  pre_mount_seq_after, mount_seq):
    """issue #100 (review round 4, corrected round 5), TIMELINE ONLY:
    verdict for one tick's pre-mount-vs-sweep-first diagnostic (see
    tests/ui_live_smoke.py's capture site for the full rationale).

    pre_mount_frame/sweep_first_frame: (H, W, 3) uint8 arrays or None (a
    capture failure).
    pre_mount_seq_after: the ViewManager mount seq read IMMEDIATELY AFTER
    the pre-mount screenshot was taken (None if no mount has ever been
    observed on this page yet -- trivially precedes any mount that will
    ever land, since none exists yet).
    mount_seq: this tick's own mount seq (web/static/lib/view-manager.js's
    lastMount.seq, once _wait_for_mount_at_or_after has returned it).

    Round 4's mistake: it captured "the panel right after the tick" and
    trusted it as "before this tick's mount" on the strength of "nothing
    repaints containerEl between mounts" (true, but says nothing about
    whether OUR OWN capture -- a real round trip, real wall-clock time --
    finishes before the NEXT mount, which is a live race against timeline's
    own mount latency, ~150ms in one run). It asserted precedence instead
    of proving it. Round 5, on a real run: it read diff_pixel_frac 0.0 on
    two ticks that a plain consecutive-frame diff of the SAME run's own
    saved PNGs showed genuinely repainted -- the "before" frame had
    actually been taken after.

    The fix: only trust the capture as "before" if pre_mount_seq_after is
    STILL strictly behind mount_seq -- i.e. no new mount had landed, by the
    time we finished capturing, that could have painted the content we
    just captured. Otherwise this tick is NOT MEASURED, never a fabricated
    0.0 (the same discipline as the gating sweep's own not-measured case:
    an instrument that cannot see cannot approve).

    Returns the same shape as frame_diff_signature (every numeric field
    None + an explanatory note when precedence can't be established, or
    when either frame is simply missing), so the caller never has to
    special-case "not measured" vs "frame missing" vs a real signature."""
    if not (pre_mount_seq_after is None or pre_mount_seq_after < mount_seq):
        return {**_PRE_MOUNT_NOT_MEASURED,
                "note": (f"pre-mount capture did not precede this tick's mount "
                         f"(seq {pre_mount_seq_after!r} >= {mount_seq!r}) -- not measured")}
    if pre_mount_frame is None or sweep_first_frame is None:
        return {**_PRE_MOUNT_NOT_MEASURED, "note": "pre-mount or sweep-first frame missing"}
    return frame_diff_signature(pre_mount_frame, sweep_first_frame)


def sweep_consecutive_diff_ratios(frames):
    """Companion to blink_check() for the offset sweep (issue #119): frames
    is a list of (H, W, 3) uint8 arrays, or None where a capture failed
    (e.g. the panel element was gone at that offset), one per
    SWEEP_OFFSETS_MS entry in order.

    Returns a list of (ratio, note) pairs, one per CONSECUTIVE pair -- i.e.
    len(frames) - 1 entries -- using blink_check's own semantics (a shape
    mismatch is the worst possible ratio with a note, never a raised
    exception) plus the same treatment for a missing frame, so a re-layout
    or a torn-down panel anywhere in the sweep is visible in summary.json
    instead of crashing the tick.

    issue #193 (review round 2): this IS now the gating instrument (via
    blink_sweep_gate_verdict's max() over these ratios), not merely a
    reporting diagnostic -- a single anchored pair (round 1's design) was
    proven blind to a transient that fell wholly inside or wholly outside
    its own two-frame window; only multiple samples spread across the tick
    catch that."""
    ratios = []
    for a, b in zip(frames, frames[1:]):
        if a is None or b is None:
            ratios.append((1.0, "frame missing from the sweep"))
            continue
        ratios.append(blink_check(a, b))
    return ratios


def blink_sweep_gate_verdict(frames, seq_before_sweep, seq_after_sweep):
    """GATING verdict for one tick (issue #193, review round 2), built on
    the offset sweep (sweep_consecutive_diff_ratios) instead of a single
    anchored pair.

    Round 1 anchored a single two-frame pair on the ViewManager mount event
    instead of a fixed delay from the tick's AAS-request send. Review then
    injected a 400ms blank overlay into a view (the PR #188 shape) and ran
    the check both ways: OLD fixed-delay pair -- ratio 0.0. Round 1's
    mount-anchored pair -- ALSO ratio 0.0. Both compare two frames that
    land wholly inside or wholly outside the transient, so neither can see
    it by construction. The offset sweep (issue #119, SWEEP_OFFSETS_MS) was
    the only instrument that caught it: ratio 1.0 on every consecutive
    pair, because its five samples spread across the tick cannot both land
    on the same side of a sub-second transient.

    frames: the sweep's own decoded arrays/None list, one per
    SWEEP_OFFSETS_MS entry, captured at increasing offsets AFTER the
    ViewManager mount event (round 1's genuine insight, kept: NOT the
    tick's AAS send, which undershoots under load -- see
    tests/ui_live_smoke.py's _wait_for_mount_at_or_after).

    seq_before_sweep/seq_after_sweep are the ViewManager mount chokepoint's
    own sequence number (web/static/lib/view-manager.js's lastMount.seq),
    sampled immediately before the sweep's first capture and immediately
    after its last, i.e. bracketing the WHOLE sweep -- same not-measured
    contract as round 1's pair verdict:

      - seq_before_sweep != seq_after_sweep: a real mount landed WHILE the
        sweep was being captured -- the sweep straddles a genuine content
        boundary, not several samples of the same steady state. Returns
        (None, note): NOT MEASURED, never a fabricated ratio.
      - otherwise: the WORST (max) of the sweep's own consecutive-pair
        ratios, INCLUDING when it is high because two samples genuinely
        differ. A steady sequence number is exactly the claim "no content
        changed here"; if any consecutive pair disagrees anyway, that is a
        real blink and must still fail like any other.

    Returns (ratio: float | None, note: str | None). ratio is None only for
    the not-measured case; every other outcome (including a genuine
    detected blink) returns a numeric ratio, so a caller can always tell a
    real 0.0 apart from "we couldn't tell". `note` joins every non-None
    per-pair note (e.g. a missing/resized frame anywhere in the sweep) so
    none of them are silently dropped just because the worst ratio came
    from a different pair."""
    if seq_before_sweep != seq_after_sweep:
        return None, (
            f"mount sequence advanced mid-sweep ({seq_before_sweep!r} -> "
            f"{seq_after_sweep!r}): sweep straddles a real content boundary, "
            "not measured")
    pairs = sweep_consecutive_diff_ratios(frames)
    if not pairs:
        return 1.0, "fewer than 2 sweep frames captured -- treated as maximal instability"
    ratios = [r for r, _n in pairs]
    notes = [n for _r, n in pairs if n]
    return max(ratios), ("; ".join(notes) if notes else None)


def build_sweep_tick_record(achieved_offsets_ms, frames,
                             target_offsets_ms=SWEEP_OFFSETS_MS,
                             capture_ms=None, mount_seq=None,
                             panel_dims=None):
    """One tick's offset-sweep record for summary.json (issue #119 item 2).

    achieved_offsets_ms: `now_ms - mount_at_ms` actually measured at each
    capture (issue #193: the sweep's own base is the mount event, not the
    tick's AAS send) -- the target is mount-anchored but preceding work can
    still push the real capture later.
    frames: the decoded arrays (or None) captured at those offsets, same
    order, fed straight to sweep_consecutive_diff_ratios().
    capture_ms: issue #197 evidence -- wall-clock milliseconds the SCREENSHOT
    ITSELF took at each offset (page.screenshot(clip=...) start to return),
    one entry per frame; None/empty for a caller that doesn't measure it.
    Directly answers "is a frame's capture cost bounded" without inferring
    it from achieved_offsets_ms drift. capture_ms_total_ms (issue #252
    evidence) is the plain sum of that list -- how much of THIS tick's
    whole sweep was spent inside the screenshot call itself, the number a
    reader actually wants without hand-summing five floats; 0.0 (not None)
    when capture_ms is empty, since "no frames measured" sums to zero cost,
    not unknown cost -- the list itself (empty) is what signals "not
    measured", not this field.
    mount_seq: issue #197 evidence -- the ViewManager mount seq this tick's
    sweep is anchored to (mount_is_fresh's own accepted mount). Recorded so
    a run's summary.json can be checked for distinct, strictly-increasing
    seqs across ticks -- a repeated seq is exactly the stale-mount-reuse bug
    this issue fixes.
    panel_dims: issue #252 evidence -- ONE dims dict for the whole tick
    ({"box_width", "box_height", "clip_width", "clip_height"}, CSS px --
    _panel_clip_and_dims' own shape), the clip computed once and reused for
    every frame of this tick's sweep (issue #252 performance change: the
    clip is no longer recomputed per frame, so there is only one
    measurement per tick to report, not one per frame). {} (not a missing
    key) for a caller that doesn't measure it.

    frame_dims_px (this issue): derived from `frames` itself, not a
    caller-supplied argument -- one {"width", "height"} dict (CSS px, from
    the decoded array's own (H, W, 3) shape) or None per entry of `frames`,
    same order. This is the DIRECT evidence frame_dims_ok gates on: the
    clip requested (panel_dims.clip_width/clip_height) vs what the
    screenshot actually decoded to. Derived here rather than passed in
    because `frames` (the decoded arrays) is already this function's own
    input -- a caller has no extra measurement to take.

    Pure: no page access. Kept here (not inline in ui_live_smoke.py) so the
    achieved-offsets-in, ratios-out shape has its own unit test."""
    pairs = sweep_consecutive_diff_ratios(frames)
    capture_ms_list = list(capture_ms) if capture_ms else []
    frame_dims_px = [
        {"width": float(f.shape[1]), "height": float(f.shape[0])}
        if f is not None else None
        for f in frames
    ]
    return {
        "target_offsets_ms": list(target_offsets_ms),
        "achieved_offsets_ms": list(achieved_offsets_ms),
        "capture_ms": capture_ms_list,
        "capture_ms_total_ms": sum(capture_ms_list),
        "mount_seq": mount_seq,
        "panel_dims": dict(panel_dims) if panel_dims else {},
        "frame_dims_px": frame_dims_px,
        "ratios": [ratio for ratio, _note in pairs],
        "notes": [note for _ratio, note in pairs if note],
    }


def capture_budget_ok(blink_sweep_ticks, bound_ms=CAPTURE_MS_BOUND_MS):
    """Reporting-only (this issue demoted it out of build_tab_result's
    `ok` -- see CAPTURE_MS_BOUND_MS's own comment for why: the bound was
    calibrated against the wrong regime and was red on unmodified, healthy
    master). Still computed and still carried into summary.json's
    capture_budget.ok/detail so the raw number stays visible to a human
    skimming the artifact; frame_dims_ok/frame_spacing_ok below are what
    actually gate now. blink_sweep_ticks: a tab's blink_sweep.ticks list
    (one build_sweep_tick_record() dict per tick).

    Every tick whose capture_ms_total_ms was actually measured (its
    capture_ms list is non-empty -- build_sweep_tick_record's own "[]
    means not measured" contract) must stay at/under bound_ms. A tick that
    never measured capture_ms is SKIPPED, not counted as a violation --
    this check has power only where there is something to check, same
    idiom as measured_ok's own "zero attempted ticks trivially passes"
    above. Zero MEASURED ticks (every caller/test before #252, or a tab
    that discarded every tick as not-measured -- already failing via
    measured_ok, never given a second, unrelated reason to look wrong)
    trivially passes for the same reason.

    Returns (ok, detail) -- detail is None when ok, else names every
    offending tick's own total so a real regression is diagnosable from
    summary.json alone, not just "something, somewhere, was slow"."""
    over = []
    for i, tick in enumerate(blink_sweep_ticks, start=1):
        if not tick.get("capture_ms"):
            continue
        total = tick.get("capture_ms_total_ms")
        if total is not None and total > bound_ms:
            over.append((i, total))
    if not over:
        return True, None
    detail = "; ".join(
        f"tick {i}: capture_ms_total_ms={total:.1f}ms > {bound_ms:.0f}ms bound"
        for i, total in over)
    return False, detail


# issue (this branch): dimension assertion -- catches the capture-cost
# regression's CAUSE directly instead of inferring it from wall-clock time.
# Removing scale="css" at DPR2 (demo_rehearsal.py's pinned viewport;
# _capture_with_clip's own docstring) doubles every decoded screenshot's
# pixel dimensions relative to the CSS-px clip that was requested --
# deterministic, not a timing inference.
FRAME_DIMS_TOLERANCE_PX = 1.0


def frame_dims_ok(blink_sweep_ticks, tolerance_px=FRAME_DIMS_TOLERANCE_PX):
    """Each decoded frame's dimensions (build_sweep_tick_record's own
    frame_dims_px, derived from the actually-decoded array) must be within
    +-tolerance_px of the SAME tick's requested clip dimensions
    (panel_dims.clip_width/clip_height, CSS px) -- never box_width/
    box_height, which is the panel's full (pre-viewport-clip) size, not
    what was actually captured.

    Unlike capture_budget_ok, a frame this check cannot evaluate --
    frame_dims_px entry is None (capture failed/missing), or the tick's
    panel_dims has no clip_width/clip_height to compare against -- is a
    VIOLATION, not a skip. capture_budget_ok's skip is safe because an
    unmeasured capture_ms is optional instrumentation with an independent
    "not measured" signal elsewhere (MIN_MEASURED_FRACTION); this check IS
    the instrument for the dimension regression, so a tick it cannot see
    into gives it zero power to catch that regression on that tick -- a
    gate that cannot see must refuse, never silently approve. (A tick with
    genuinely zero attempted frames -- frame_dims_px itself absent/empty --
    is still vacuous: there is nothing to have gotten wrong, and ticks_ok/
    measured_ok already fail a tab that never captured anything.)

    Returns (ok, detail) -- detail names every offending tick+frame so a
    real regression (or a missing-measurement bug) is diagnosable from
    summary.json alone."""
    violations = []
    for i, tick in enumerate(blink_sweep_ticks, start=1):
        frame_dims = tick.get("frame_dims_px") or []
        panel_dims = tick.get("panel_dims") or {}
        clip_w = panel_dims.get("clip_width")
        clip_h = panel_dims.get("clip_height")
        for j, fd in enumerate(frame_dims, start=1):
            if fd is None:
                violations.append(f"tick {i} frame {j}: missing frame")
                continue
            if clip_w is None or clip_h is None:
                violations.append(
                    f"tick {i} frame {j}: missing clip dims to compare against")
                continue
            dw = abs(fd["width"] - clip_w)
            dh = abs(fd["height"] - clip_h)
            if dw > tolerance_px or dh > tolerance_px:
                violations.append(
                    f"tick {i} frame {j}: frame {fd['width']:.0f}x{fd['height']:.0f}px "
                    f"vs clip {clip_w:.0f}x{clip_h:.0f}px (tolerance {tolerance_px:.0f}px)")
    if not violations:
        return True, None
    return False, "; ".join(violations)


def frame_spacing_drift_ms(tick):
    """Per-frame achieved-minus-target drift (ms) for one tick: same
    two-line formula as tests/demo_rehearsal_lib.py's sweep_offset_drift
    (which is reporting-only there, by owner instruction 2026-09-28 --
    "report the drift; do not try to fix the scheduling in this branch",
    a decision about THAT branch's coverage-loss diagnosis, not a
    standing ban on ever gating on drift). Duplicated here as the two-line
    core rather than imported: ui_live_smoke_lib.py has no other
    dependency on demo_rehearsal_lib.py, and pulling in a whole sibling
    harness module for one list comprehension is not worth the coupling.

    tick: a build_sweep_tick_record() dict. Returns a list, one entry per
    frame present in both target_offsets_ms and achieved_offsets_ms
    (mismatched/short lists truncate to the shorter, never raise)."""
    targets = tick.get("target_offsets_ms") or []
    achieved = tick.get("achieved_offsets_ms") or []
    n = min(len(targets), len(achieved))
    return [achieved[i] - targets[i] for i in range(n)]


# Derivation, verified against artifacts (not asserted -- CLAUDE.md
# evidence rule):
#
#   Healthy, gate-1 (root@2.28.45.47), 11 tabs x 6 ticks, frames 2-5
#   (drift indices 1-4, i.e. achieved/target offsets 500/1000/1500/2000ms)
#   of ticks 2..N:
#     .claude/worktrees/window-clip-aggregates/tests/results/ui_live/
#     summary.json -- max drift 27ms (histogram, tick 5, frame 4).
#
#   Tick 1 is excluded WHOLE, not merely its frame 1 (the 200ms target).
#   The SAME artifact shows tick 1's own frame 2 (index 1, not just index
#   0) elevated on every one of the 11 tabs -- from 3ms (waterfall) to
#   254ms (timeline) -- because the sweep's mount-anchor clock starts late
#   on the run's first tick (77-500ms of pre-sweep work: page navigation,
#   first AAS fetch, first ViewManager mount) and that fixed offset still
#   shows up at the SECOND target before later, larger inter-offset gaps
#   absorb it. Example per-tab tick-1 drift (ms, frames 1-5): timeline
#   [492, 254, 11, 17, 14], transitions [267, 192, 17, 14, 12], matrix
#   [242, 166, 9, 10, 10]. An earlier reading of this bound excluded only
#   frame 1 of every tick, leaving tick 1's 254ms frame-2 drift in scope --
#   only 1.4x below the regression floor below, nowhere near the >10x
#   separation actually available. Excluding the whole first tick is what
#   reproduces the <=30ms healthy number.
#
#   Regression floor, issue #252's own run 1790716019 (DPR2, viewport clip
#   already in place, scale="css" NOT yet added): capture cost
#   648-1086ms/frame against SWEEP_OFFSETS_MS's 300/500ms inter-frame
#   gaps -- drift >=350ms at frame 2, accumulating to 1.5-4s by frame 5.
#
#   27ms healthy vs 350ms regression floor: ~13x separation. Bound set at
#   100ms -- >3.5x clear of the healthy max, >3.5x below the regression
#   floor, deliberately centered rather than tight (contract: do not set
#   it tight).
FRAME_SPACING_DRIFT_BOUND_MS = 100.0


def frame_spacing_ok(blink_sweep_ticks, bound_ms=FRAME_SPACING_DRIFT_BOUND_MS):
    """Bounds frames 2-5's achieved-vs-target offset drift (frame_spacing_
    drift_ms indices 1-4) for every tick EXCEPT the first -- see
    FRAME_SPACING_DRIFT_BOUND_MS's own comment for why tick 1 and frame 1
    are both excluded, with the artifact numbers that justify it. This is
    what CAPTURE_MS_BOUND_MS was actually trying to protect: once a
    frame's own capture cost exceeds the gap to the NEXT sweep offset,
    achieved_offsets_ms drifts away from target_offsets_ms and the sweep
    stops sampling where SWEEP_OFFSETS_MS says it should -- bounding that
    directly, rather than inferring it from a raw wall-clock budget.

    Like frame_dims_ok (and unlike capture_budget_ok): a tick within scope
    (i > 1) that has fewer than 5 achieved offsets is not silently
    skipped -- frame_spacing_drift_ms's own truncate-to-shorter behavior
    means a tick record with no offsets at all (frame_spacing_drift_ms
    returns []) is vacuous (nothing to have gotten wrong, same as
    frame_dims_ok's zero-attempt case), but any offset that WAS recorded
    is compared.

    Returns (ok, detail) -- detail names every offending tick+frame's own
    drift so a real regression is diagnosable from summary.json alone."""
    violations = []
    for i, tick in enumerate(blink_sweep_ticks, start=1):
        if i == 1:
            continue  # tick 1 excluded -- see FRAME_SPACING_DRIFT_BOUND_MS
        drift = frame_spacing_drift_ms(tick)
        for idx, d in enumerate(drift[1:5], start=2):  # frames 2-5
            if abs(d) > bound_ms:
                violations.append(
                    f"tick {i} frame {idx}: drift={d}ms > {bound_ms:.0f}ms bound")
    if not violations:
        return True, None
    return False, "; ".join(violations)


def is_blank_frame(frame, std_threshold=1.0):
    """True if frame is (near-)solid-colour -- a CONTINUITY teardown-to-blank
    flash, not real rendered content. Real content (text, grid lines, chart
    series, table rows) has per-pixel variance far above a flat background
    repaint; std_threshold (0..255 terms, computed over the whole array) is
    deliberately tiny -- this is a "did the panel go completely blank" check,
    not a content-richness heuristic, so it never second-guesses a
    genuinely sparse-but-real panel."""
    return float(np.std(frame)) < std_threshold


def compare_png_files(path_a, path_b):
    """frame_diff_ratio() for two PNGs already on disk."""
    return frame_diff_ratio(load_png_array(path_a), load_png_array(path_b))


def blind_window_ok(snapshot, blank_std_threshold=1.0):
    """issue #142: verdict for ui_live_smoke.py's blind-window CONTINUITY
    check, from ui_live_smoke.py's _ATOMIC_PANEL_SNAPSHOT_JS single-
    page.evaluate() result (a plain dict, no Playwright object) -- kept pure/
    testable here, same idiom as every other verdict function in this module.

    snapshot shapes (see _ATOMIC_PANEL_SNAPSHOT_JS):
      {"present": False}                                      -- element gone
      {"present": True, "canvas": True, "dataURL": "data:..."} -- chart tabs
      {"present": True, "canvas": False, "hasContent": bool}   -- table tabs

    Returns (ok: bool, reason: str); reason is only meaningful when
    ok is False (fed straight into the SmokeFailure message)."""
    if not snapshot.get("present"):
        return False, "panel element gone"
    if snapshot.get("canvas"):
        frame = data_url_to_array(snapshot["dataURL"])
        if is_blank_frame(frame, std_threshold=blank_std_threshold):
            return False, "panel went blank"
        return True, ""
    if not snapshot.get("hasContent"):
        return False, "panel has no content"
    return True, ""


def no_blink_ok(ratio, threshold=BLINK_THRESHOLD):
    return ratio is not None and ratio < threshold


def color_stability_violations(tick_legends):
    """tick_legends: list of {event_name: css_color_string} dicts, one per
    tick (or per frame) in chronological order.

    Returns a list of human-readable violation strings; empty = every name
    kept the same colour across every tick it appeared in (issue #93
    acceptance item 3: "every legend chip keeps the same colour for the
    same event name")."""
    violations = []
    first_seen = {}  # name -> (tick_idx, color)
    for tick_idx, legend in enumerate(tick_legends):
        for name, color in legend.items():
            if name not in first_seen:
                first_seen[name] = (tick_idx, color)
                continue
            base_idx, base_color = first_seen[name]
            if color != base_color:
                violations.append(
                    f"{name}: {base_color!r} at tick {base_idx} -> "
                    f"{color!r} at tick {tick_idx}")
    return violations


def leak_probe_ok(probe, chart_max=2, uplot_max=1):
    """Interprets the resource probe reused from
    tests/test_web_ui_chaos.py's _LEAK_PROBE ({"charts", "uplots",
    "pending"}). Same bounds as test_soak_random_navigation: a settled UI
    keeps at most the persistent AAS pane + one just-disposed/just-created
    per-tab transient; a real leak grows without bound."""
    return (probe.get("charts", 0) <= chart_max and
            probe.get("uplots", 0) <= uplot_max and
            probe.get("pending", 0) == 0)


def render_check_ok(result):
    """Normalizes a panel-specific JS render-check's return value.

    The per-tab JS snippets (PANEL_CHECKS in ui_live_smoke.py) either return
    a string ('ok:<n>' on success, 'no ...'/'not a ...' on failure -- the
    idiom test_web_ui.py already uses for ECharts option probes) or a dict
    {'ok': bool, 'detail': str}. Returns (ok: bool, detail: str)."""
    if isinstance(result, dict):
        return bool(result.get("ok")), str(result.get("detail", ""))
    if isinstance(result, str):
        return result.startswith("ok"), result
    return False, f"unexpected render-check result: {result!r}"


def viewport_mismatch_reason(requested_width, requested_height,
                             requested_device_scale_factor,
                             actual_inner_width, actual_inner_height,
                             actual_device_pixel_ratio):
    """None if the viewport/zoom a caller asked run_tab() to use is the one
    the page actually ended up with; else a human-readable reason naming
    every field that disagrees.

    Exists because a *requested* viewport is not evidence of anything --
    docs/DEMO_REHEARSAL_CRITERIA.md's viewport section names two traps a
    harness must not fall into: trusting the size it asked for instead of
    reading back `innerWidth`/`innerHeight`, and never checking the
    per-hostname zoom that (Chrome and Safari both) silently rescales the
    content layer even when the outer window size is right --
    `devicePixelRatio` is the only signal Playwright exposes for that (see
    docs/chrome-demo-viewport-2026-09-28.md's "Effective zoom check": at
    100% zoom on a 2x-backing-scale display DPR reads exactly 2.0, and any
    other zoom multiplies it away from that). A caller that wires a new
    device_scale_factor kwarg into run_tab() without actually plumbing it
    into browser.new_context() would otherwise report a plausible,
    stable-looking PASS at the wrong zoom -- this is what catches that.

    Width/height are compared exactly (Playwright's context viewport is a
    fixed integer, not something that settles asynchronously); DPR is
    compared with a small epsilon since it can arrive as e.g. 2 or 2.0."""
    reasons = []
    if actual_inner_width != requested_width:
        reasons.append(f"width: requested {requested_width}, "
                       f"got innerWidth={actual_inner_width}")
    if actual_inner_height != requested_height:
        reasons.append(f"height: requested {requested_height}, "
                       f"got innerHeight={actual_inner_height}")
    if abs(actual_device_pixel_ratio - requested_device_scale_factor) > 1e-6:
        reasons.append(
            f"zoom/devicePixelRatio: requested device_scale_factor="
            f"{requested_device_scale_factor}, got "
            f"devicePixelRatio={actual_device_pixel_ratio}")
    if not reasons:
        return None
    return "; ".join(reasons)


def known_failing_issue(tab_id):
    """The tracking issue number if tab_id is listed in KNOWN_FAILING_TABS,
    else None."""
    return KNOWN_FAILING_TABS.get(tab_id)


def known_failing_report_line(tab_id, raw_ok):
    """The KNOWN-FAILING / UNEXPECTED PASS line for a listed tab, given its
    RAW (unadjusted) pass/fail; None if tab_id is not listed. raw_ok=False
    (the expected case) -> "KNOWN-FAILING (issue #N)"; raw_ok=True (the run
    happened not to reproduce it) -> the UNEXPECTED PASS line -- printed
    loudly either way, per KNOWN_FAILING_TABS's doc comment, but never
    fails the run by itself (see _apply_known_failing)."""
    issue = known_failing_issue(tab_id)
    if issue is None:
        return None
    if raw_ok:
        return f"UNEXPECTED PASS (issue #{issue}) -- intermittent or fixed; check the issue"
    return f"KNOWN-FAILING (issue #{issue})"


def _apply_known_failing(result):
    """Tags a tab result dict with known_failing/xpass per KNOWN_FAILING_TABS.
    `ok` is left as the RAW (real) pass/fail -- summary.json always tells the
    truth about what actually happened; build_summary is what excuses a
    listed tab's raw failure (or unexpected pass) from the OVERALL verdict."""
    issue = known_failing_issue(result["tab"])
    raw_ok = result["ok"]
    result["known_failing"] = bool(issue is not None and not raw_ok)
    result["xpass"] = bool(issue is not None and raw_ok)
    return result


def build_tab_result(tab_id, rendered_ok, rendered_detail, ticks_observed,
                      console_errors, blink_ratio, color_violations,
                      leak_before, leak_after, artifacts,
                      blink_threshold=BLINK_THRESHOLD,
                      pgwt_console_errors=(),
                      leak_before_settle_s=None, leak_after_settle_s=None,
                      blink_pair_offsets_ms=(), blink_sweep_ticks=(),
                      blink_not_measured=(),
                      min_measured_fraction=MIN_MEASURED_FRACTION,
                      pre_mount_diagnostics=(),
                      capture_ms_bound_ms=CAPTURE_MS_BOUND_MS):
    """Assembles one tab's verdict. Pure: every input is already-collected
    data, no page access.

    ticks_observed must be the count of frames ACTUALLY captured (a tick can
    land without a usable screenshot -- e.g. a DOM-detach race -- so this is
    not simply "how many ticks did we wait for").

    pgwt_console_errors: '[pgwt]'-prefixed console.error calls (the app's OWN
    failure reporting) drained during the visit. These never fail the tab
    (clean_ok is computed from console_errors only, the UNEXPECTED ones) but
    must still be visible in summary.json, not silently dropped.

    leak_before_settle_s/leak_after_settle_s: how long the leak probe took to
    settle (issue #93 fail-safe/correctness review item 6) -- a 25s response
    is itself a latency regression worth a trace even though it does not, by
    itself, fail no_leak.

    blink_pair_offsets_ms: per-tick `mount_at_ms - tick_ts_ms` -- how long
    after the tick's AAS request send the ViewManager mount actually landed
    (issue #93 review item 3; issue #193 changed WHAT is being timed --
    the mount event, not a fixed-delay guess -- but this field keeps the
    same visibility purpose: a future drift under load stays visible in
    summary.json, never silent). One entry per ATTEMPTED tick (appended
    whether or not that tick ended up measured), so len() here is also this
    tab's attempted-tick count -- see min_measured_fraction below.

    blink_not_measured: per-tick {"tick": i, "reason": str} entries (issue
    #193) for a tick whose gating sweep straddled a real mount mid-capture
    (blink_sweep_gate_verdict's ratio=None case) -- reported for
    visibility, and ALSO why `ok` below can still end up False on an
    all-not-measured tab: blink_ratio is computed by the caller as
    max(measured ratios), which is None when every tick was unmeasured, and
    no_blink_ok(None, ...) is False by construction -- an unmeasured signal
    fails the gate rather than silently passing it.

    blink_sweep_ticks: one build_sweep_tick_record() dict per tick (issue
    #119 item 2) -- AND, since issue #193 review round 2, the gating data
    itself: blink_ratio is the caller-computed max over these ticks' own
    worst ratios (blink_sweep_gate_verdict), not a separate pair.

    min_measured_fraction (issue #193 review round 2, SHOULD-FIX): a tab
    that discarded most of its ATTEMPTED ticks (len(blink_pair_offsets_ms))
    as not-measured must not report `ok` on whatever fraction it did manage
    to measure -- found in review: a tab that measured 1 of 6 ticks at
    ratio 0.0 and discarded the other 5 passed anyway, on essentially no
    signal. measured_ok requires at least this fraction of attempted ticks
    to have produced a real ratio. Zero attempted ticks (the default
    blink_pair_offsets_ms=(), used by callers/tests that don't populate
    this field) trivially satisfies measured_ok -- ticks_ok already fails
    that tab for having no ticks at all, so this never becomes a second,
    contradictory reason a legitimately-untested result looks wrong.

    pre_mount_diagnostics (issue #100, review round 4, corrected round 5):
    timeline-tab-only, one {"tick", "diff_pixel_frac",
    "bottom_band_pixel_frac", "rest_pixel_frac", "columns_touched_frac",
    "note"} entry per tick (pre_mount_diagnostic_verdict's own return
    shape, plus "tick") -- reporting only, never read here to compute
    `ok`. Every numeric field is None with an explanatory note when the
    capture could not prove it preceded the tick's own mount (see
    pre_mount_diagnostic_verdict) -- never a fabricated 0.0. Empty for
    every other tab.

    capture_ms_bound_ms: see capture_budget_ok/CAPTURE_MS_BOUND_MS's own
    comments -- capture_budget is REPORTING ONLY now (this issue demoted
    it out of `ok`: it was mis-specified against the wrong regime and was
    red on healthy master). frame_dims_ok and frame_spacing_ok (below) are
    what now close the escape path where a capture-cost regression stays
    invisible as long as it still fits inside one live tick -- the former
    catches the regression's cause (decoded frame size vs requested clip)
    directly, the latter catches its effect on the sweep's own sampling
    schedule."""
    clean_ok = len(console_errors) == 0
    blink_ok = no_blink_ok(blink_ratio, blink_threshold)
    leak_ok = leak_probe_ok(leak_before) and leak_probe_ok(leak_after)
    color_ok = len(color_violations) == 0
    ticks_ok = ticks_observed >= MIN_TICKS
    attempted_count = len(blink_pair_offsets_ms)
    not_measured_count = len(blink_not_measured)
    measured_count = attempted_count - not_measured_count
    measured_ok = (attempted_count == 0 or
                   (measured_count / attempted_count) >= min_measured_fraction)
    # Reporting only (see capture_ms_bound_ms above) -- deliberately NOT
    # ANDed into `ok`.
    capture_ok, capture_detail = capture_budget_ok(blink_sweep_ticks,
                                                    bound_ms=capture_ms_bound_ms)
    dims_ok, dims_detail = frame_dims_ok(blink_sweep_ticks)
    spacing_ok, spacing_detail = frame_spacing_ok(blink_sweep_ticks)
    ok = (rendered_ok and clean_ok and blink_ok and measured_ok and leak_ok and
          color_ok and ticks_ok and dims_ok and spacing_ok)
    result = {
        "tab": tab_id,
        "ok": ok,
        "ticks_observed": ticks_observed,
        "rendered": {"ok": rendered_ok, "detail": rendered_detail},
        "clean": {"ok": clean_ok, "console_errors": list(console_errors)[:10],
                  "pgwt_errors": list(pgwt_console_errors)[:10]},
        "no_blink": {"ok": blink_ok, "ratio": blink_ratio,
                     "threshold": blink_threshold,
                     "pair_offsets_ms": list(blink_pair_offsets_ms),
                     "not_measured": list(blink_not_measured),
                     "measured": {"ok": measured_ok,
                                  "measured_count": measured_count,
                                  "attempted_count": attempted_count,
                                  "min_fraction": min_measured_fraction}},
        "blink_sweep": {"offsets_ms": list(SWEEP_OFFSETS_MS),
                        "ticks": list(blink_sweep_ticks)},
        "capture_budget": {"ok": capture_ok, "bound_ms": capture_ms_bound_ms,
                           "detail": capture_detail},
        "frame_dims": {"ok": dims_ok, "tolerance_px": FRAME_DIMS_TOLERANCE_PX,
                       "detail": dims_detail},
        "frame_spacing": {"ok": spacing_ok, "bound_ms": FRAME_SPACING_DRIFT_BOUND_MS,
                          "detail": spacing_detail},
        "color_stability": {"ok": color_ok, "violations": color_violations},
        "no_leak": {"ok": leak_ok, "before": leak_before, "after": leak_after,
                    "settle_s": {"before": leak_before_settle_s,
                                 "after": leak_after_settle_s}},
        "artifacts": artifacts,
        "pre_mount_diagnostic": list(pre_mount_diagnostics),
    }
    return _apply_known_failing(result)


def build_failed_tab_result(tab_id, reason, ticks_observed=0, artifacts=None,
                            pgwt_console_errors=()):
    """A tab result for a tab that never got far enough to evaluate the four
    checks (e.g. the 60s no-data fail-safe fired, or navigation raised).
    Kept separate from build_tab_result so a genuine "checked and failed"
    result is never confused with "could not even check" -- both fail the
    tab (and, unless known-failing-listed, the run), but the JSON says which
    happened."""
    result = {
        "tab": tab_id,
        "ok": False,
        "ticks_observed": ticks_observed,
        "error": reason,
        "rendered": {"ok": False, "detail": reason},
        "clean": {"ok": None, "console_errors": [],
                  "pgwt_errors": list(pgwt_console_errors)[:10]},
        "no_blink": {"ok": None, "ratio": None, "threshold": BLINK_THRESHOLD,
                     "pair_offsets_ms": [], "not_measured": [],
                     "measured": {"ok": None, "measured_count": 0,
                                  "attempted_count": 0,
                                  "min_fraction": MIN_MEASURED_FRACTION}},
        "blink_sweep": {"offsets_ms": list(SWEEP_OFFSETS_MS), "ticks": []},
        "capture_budget": {"ok": None, "bound_ms": CAPTURE_MS_BOUND_MS,
                           "detail": None},
        "frame_dims": {"ok": None, "tolerance_px": FRAME_DIMS_TOLERANCE_PX,
                       "detail": None},
        "frame_spacing": {"ok": None, "bound_ms": FRAME_SPACING_DRIFT_BOUND_MS,
                          "detail": None},
        "color_stability": {"ok": None, "violations": []},
        "no_leak": {"ok": None, "before": None, "after": None,
                    "settle_s": {"before": None, "after": None}},
        "artifacts": artifacts or {},
        "pre_mount_diagnostic": [],
    }
    return _apply_known_failing(result)


def build_summary(tab_results):
    """Assembles the top-level tests/results/ui_live/summary.json payload.
    `ok` reflects only unlisted tabs (or listed tabs that happened to pass):
    a KNOWN_FAILING_TABS tab's raw failure -- or unexpected pass -- never
    flips the overall verdict; `tabs[*].known_failing`/`xpass` (and the
    driver's printed KNOWN-FAILING/UNEXPECTED PASS lines) are how that stays
    visible instead of silent."""
    excused = [t["tab"] for t in tab_results if t.get("known_failing")]
    failed_tabs = [t["tab"] for t in tab_results
                   if not t["ok"] and not t.get("known_failing")]
    ok = len(tab_results) > 0 and len(failed_tabs) == 0
    return {
        "ok": ok,
        "tabs": {t["tab"]: t for t in tab_results},
        "failed_tabs": failed_tabs,
        "known_failing_tabs": excused,
        "xpass_tabs": [t["tab"] for t in tab_results if t.get("xpass")],
    }


def reset_output_dir(path):
    """Deletes path (if present) and recreates it empty.

    Found via a ui-reviewer blocker: tests/results/ui_live/waterfall/
    tick-1..3.png survived from an EARLIER run (2h-old mtimes) into a later
    run whose waterfall tab failed to render at all (0 frames written) --
    summary.json correctly said frames: [], but the stale PNGs on disk made
    the tab look rendered to a reviewer who just opens the frame. Every
    artifact under the output dir must belong to the run that just produced
    summary.json, never a leftover from a previous invocation (mock, real,
    or a stale rsync copy) -- so the driver calls this ONCE, before any tab
    runs, instead of each tab's os.makedirs(..., exist_ok=True) trusting an
    already-populated directory."""
    if os.path.isdir(path):
        shutil.rmtree(path)
    os.makedirs(path, exist_ok=True)


def write_summary(path, tab_results):
    summary = build_summary(tab_results)
    os.makedirs(os.path.dirname(path), exist_ok=True)
    with open(path, "w") as f:
        json.dump(summary, f, indent=2, sort_keys=True)
        f.write("\n")
    return summary
