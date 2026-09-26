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

# Offset sweep (issue #119): tick-anchored offsets, in ms after each tick's
# own timestamp, at which the driver grabs an EXTRA frame purely for
# reporting -- never gating. Lets a re-layout/re-render show up at whichever
# window it actually lands in (timeline's blink, #100, was never measured
# this early: the gating pair alone only ever sampled ~1.2-1.3s after the
# tick, and drifted to 1.8-3.0s on half the ticks when the render-check
# retry loop ate the budget). The gating check itself stays exactly what it
# was -- one pair at the anchored settle offset, BLINK_THRESHOLD -- this is
# additional data, not a wider or narrower pass/fail rule.
SWEEP_OFFSETS_MS = (200, 500, 1000, 1500, 2000)

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
    # #100: no_blink ratio 4.46% in one run, but an UNPROVEN xpass in
    # another -- do not read the xpass as "fixed". The xpass run itself was
    # confounded: tests/ui_live_smoke.py's blind-window check (added the
    # same round) pushed the blink pair from ~1.3s to ~1.6s after the tick
    # (a flat 1200ms sleep from wherever the code happened to be, not
    # anchored to the tick's own timestamp -- fixed after this was found),
    # so a pass under the drifted timing proves nothing about whichever
    # window the original failure landed in. #102 (missing animation:false)
    # is CLOSED, not merely unproven: disproved at runtime -- all four
    # builders (concurrency/timeline/exec-scatter/matrix) already had
    # animation:false at the option root, confirmed by reading the actual
    # ECharts option on every non-empty gallery cell. The missing-animation
    # hypothesis for #100 is ruled out; whatever causes timeline's blink is
    # something else. Stays listed until a run with the tick-anchored
    # settle (and now the offset sweep, issue #119) is investigated either
    # way.
    "timeline": 100,
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


def sweep_consecutive_diff_ratios(frames):
    """Reporting-only companion to blink_check() for the offset sweep (issue
    #119): frames is a list of (H, W, 3) uint8 arrays, or None where a
    capture failed (e.g. the panel element was gone at that offset), one per
    SWEEP_OFFSETS_MS entry in order.

    Returns a list of (ratio, note) pairs, one per CONSECUTIVE pair -- i.e.
    len(frames) - 1 entries -- using blink_check's own semantics (a shape
    mismatch is the worst possible ratio with a note, never a raised
    exception) plus the same treatment for a missing frame, so a re-layout
    or a torn-down panel anywhere in the sweep is visible in summary.json
    instead of crashing the tick.

    Never read by build_tab_result's `ok` computation -- the gating check
    stays exactly the one pair at the anchored settle offset it always was;
    this is additional data for deciding the settle (issue #119 item 3), not
    a second pass/fail rule."""
    ratios = []
    for a, b in zip(frames, frames[1:]):
        if a is None or b is None:
            ratios.append((1.0, "frame missing from the sweep"))
            continue
        ratios.append(blink_check(a, b))
    return ratios


def build_sweep_tick_record(achieved_offsets_ms, frames,
                             target_offsets_ms=SWEEP_OFFSETS_MS):
    """One tick's offset-sweep record for summary.json (issue #119 item 2).

    achieved_offsets_ms: `now_ms - tick_ts_ms` actually measured at each
    capture, same idea as blink_pair_offsets_ms -- the target is tick-
    anchored but preceding work can still push the real capture later.
    frames: the decoded arrays (or None) captured at those offsets, same
    order, fed straight to sweep_consecutive_diff_ratios().

    Pure: no page access. Kept here (not inline in ui_live_smoke.py) so the
    achieved-offsets-in, ratios-out shape has its own unit test."""
    pairs = sweep_consecutive_diff_ratios(frames)
    return {
        "target_offsets_ms": list(target_offsets_ms),
        "achieved_offsets_ms": list(achieved_offsets_ms),
        "ratios": [ratio for ratio, _note in pairs],
        "notes": [note for _ratio, note in pairs if note],
    }


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
                      blink_pair_offsets_ms=(), blink_sweep_ticks=()):
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

    blink_pair_offsets_ms: per-tick `now_ms - tick_ts_ms` when frame_a of the
    blink pair was actually captured (issue #93 review item 3) -- the settle
    (ui_live_smoke.py's BLINK_PAIR_ANCHOR_MS) is anchored to the tick's own
    timestamp, but preceding work (the blind-window check,
    _poll_render_check's retry loop) can still push the actual capture past
    the target under load. Recorded, not enforced: makes any future overrun
    of the anchor visible in summary.json instead of silent.

    blink_sweep_ticks: one build_sweep_tick_record() dict per tick (issue
    #119 item 2) -- reporting only, never read below to compute `ok`; the
    gating check stays the one pair at the anchored settle offset it always
    was."""
    clean_ok = len(console_errors) == 0
    blink_ok = no_blink_ok(blink_ratio, blink_threshold)
    leak_ok = leak_probe_ok(leak_before) and leak_probe_ok(leak_after)
    color_ok = len(color_violations) == 0
    ticks_ok = ticks_observed >= MIN_TICKS
    ok = (rendered_ok and clean_ok and blink_ok and leak_ok and color_ok and
          ticks_ok)
    result = {
        "tab": tab_id,
        "ok": ok,
        "ticks_observed": ticks_observed,
        "rendered": {"ok": rendered_ok, "detail": rendered_detail},
        "clean": {"ok": clean_ok, "console_errors": list(console_errors)[:10],
                  "pgwt_errors": list(pgwt_console_errors)[:10]},
        "no_blink": {"ok": blink_ok, "ratio": blink_ratio,
                     "threshold": blink_threshold,
                     "pair_offsets_ms": list(blink_pair_offsets_ms)},
        "blink_sweep": {"offsets_ms": list(SWEEP_OFFSETS_MS),
                        "ticks": list(blink_sweep_ticks)},
        "color_stability": {"ok": color_ok, "violations": color_violations},
        "no_leak": {"ok": leak_ok, "before": leak_before, "after": leak_after,
                    "settle_s": {"before": leak_before_settle_s,
                                 "after": leak_after_settle_s}},
        "artifacts": artifacts,
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
                     "pair_offsets_ms": []},
        "blink_sweep": {"offsets_ms": list(SWEEP_OFFSETS_MS), "ticks": []},
        "color_stability": {"ok": None, "violations": []},
        "no_leak": {"ok": None, "before": None, "after": None,
                    "settle_s": {"before": None, "after": None}},
        "artifacts": artifacts or {},
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
