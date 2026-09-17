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
import io
import json
import os

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

# KNOWN_FAILING_TABS: tab name -> tracking issue number. ONLY for a tab that
# reproduces a real, filed product bug (issue #100, #101) -- never for timing
# or runner noise; a noisy tab is investigated, never silenced here (see also
# CLAUDE.md's Rules / tests/run_all.sh's KNOWN_FAILING, the same mechanism).
#
# A listed tab still runs every check and keeps its artifacts. Unlike
# run_all.sh's KNOWN_FAILING (where an UNEXPECTED PASS counts as a failure,
# because a deterministic test passing once is real evidence the bug is
# fixed), a single real-daemon run passing is NOT strong evidence an
# intermittent product bug is gone -- so here neither a failure nor an
# unexpected pass fails the overall run; both are reported loudly
# (`known_failing`/`xpass` on the tab, `KNOWN-FAILING (issue #N)` /
# `UNEXPECTED PASS (issue #N) -- intermittent or fixed; check the issue` in
# the driver's output) so a human decides when to delist a tab, not a single
# green run.
KNOWN_FAILING_TABS = {
    "timeline": 100,   # #100: no_blink ratio 4.46% (missing animation:false, #102)
    "waterfall": 101,  # #101: executions query > 60s under sustained full-mode load
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


def compare_png_files(path_a, path_b):
    """frame_diff_ratio() for two PNGs already on disk."""
    return frame_diff_ratio(load_png_array(path_a), load_png_array(path_b))


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
                      blink_threshold=BLINK_THRESHOLD):
    """Assembles one tab's verdict. Pure: every input is already-collected
    data, no page access."""
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
        "clean": {"ok": clean_ok, "console_errors": list(console_errors)[:10]},
        "no_blink": {"ok": blink_ok, "ratio": blink_ratio,
                     "threshold": blink_threshold},
        "color_stability": {"ok": color_ok, "violations": color_violations},
        "no_leak": {"ok": leak_ok, "before": leak_before, "after": leak_after},
        "artifacts": artifacts,
    }
    return _apply_known_failing(result)


def build_failed_tab_result(tab_id, reason, ticks_observed=0, artifacts=None):
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
        "clean": {"ok": None, "console_errors": []},
        "no_blink": {"ok": None, "ratio": None, "threshold": BLINK_THRESHOLD},
        "color_stability": {"ok": None, "violations": []},
        "no_leak": {"ok": None, "before": None, "after": None},
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


def write_summary(path, tab_results):
    summary = build_summary(tab_results)
    os.makedirs(os.path.dirname(path), exist_ok=True)
    with open(path, "w") as f:
        json.dump(summary, f, indent=2, sort_keys=True)
        f.write("\n")
    return summary
