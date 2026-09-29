#!/usr/bin/env python3
"""ui_live_smoke.py -- issue #93: live UI smoke walk of all 11 tabs.

Proves that every panel works against a REAL daemon with live data (not
tests/mock_server.py's canned replies): rendered, no console errors, no
blinking, stable identity colours, no resource leak. Designed to run on the
gate box inside tests/run_all.sh's live section (via tests/ui_live_smoke.sh),
but has two front-ends so the walk logic itself is testable off the box:

    --mock             starts tests/mock_server.py itself and walks against
                        it -- validates the walk/artifact/verdict machinery
                        on a plain Mac, no daemon, no BPF, no box.
    --url <page URL>    walks against an already-running UI. Phase 2 points
                        this at the real Go bridge (tests/ui_live_smoke.sh).

Per tab (Overview, Events, Sessions, Queries, Histogram, Timeline,
Transitions, Concurrency, Waterfall, Scatter, Matrix), live mode on
(the app's default), at least MIN_TICKS ticks:
  1. rendered    -- panel-specific non-empty check (chart has data / table
                     has rows / graph has nodes; see PANEL_CHECKS).
  2. clean       -- zero console errors / page errors / unhandled rejections
                     for the whole tab visit ('[pgwt]'-prefixed console
                     errors, the app's OWN failure reporting, never fail
                     this by themselves but are recorded and printed, never
                     silently dropped).
  3. no_blink    -- a frame ~100ms after each tick is asserted non-blank
                     (a CONTINUITY teardown-to-blank flash must not pass);
                     the GATING check is the issue #119 offset sweep itself
                     (SWEEP_OFFSETS_MS, 200-2000ms), now anchored on the
                     ViewManager mount chokepoint's own event for this tab
                     (issue #193 round 2) instead of a guess from the tick's
                     AAS-request send -- graded on the WORST of its
                     consecutive-pair diff ratios against
                     ui_live_smoke_lib.BLINK_THRESHOLD (0.1%). A single
                     anchored pair (issue #193's first round) was found by
                     review to be blind to a transient that fell wholly
                     inside or wholly outside its own two-frame window (an
                     injected 400ms overlay measured ratio 0.0 on the pair,
                     1.0 on every sweep offset) -- the sweep's multiple
                     samples across the tick are what actually catch that.
                     If the mount sequence itself advances while the sweep
                     is being captured, the tick is reported NOT MEASURED
                     instead of a fabricated ratio
                     (ui_live_smoke_lib.blink_sweep_gate_verdict), and a tab
                     needs a minimum FRACTION of its ticks actually measured
                     (ui_live_smoke_lib.MIN_MEASURED_FRACTION) to pass at
                     all -- discarding most ticks as unmeasured must not
                     read as "ok, ratio 0.0". The AAS legend keeps each
                     event name's colour stable across ticks.
  4. no_leak     -- chart/uplot/pending-request counts (the chaos suite's
                     probe, test_web_ui_chaos.py's _LEAK_PROBE) are stable
                     across the visit; how long the probe took to settle is
                     recorded too (a slow settle is a latency regression
                     worth a trace even when it does not fail the check).

Five of these tabs (Transitions/Concurrency/Waterfall/Scatter/Matrix)
pause live mode on entry or on any drill (app.js), and the driver resumes
it via #live-btn (see _navigate_to_tab) -- deliberately: the walk grades
the state a real user reaches by pressing Live on one of these "heavy"
views, which the app permits and therefore must render as well as any
other live state.

Fail-safe: if the UI shows no data within FIRST_DATA_TIMEOUT_S (60s), the tab
FAILS loudly -- never a skip (mirrors run_all.sh --require-live). Same for
the controlled workload (pgbench + the lock/sleep sessions,
tests/ui_live_smoke.sh): if --pgbench-pid/--workload-pid are given and either
process is gone at a tab boundary, the whole run FAILS loudly rather than
grading the remaining tabs against an idle system.

Artifacts: tests/results/ui_live/<tab>/tick-N.png, one .webm per tab,
tests/results/ui_live/summary.json.

Usage:
    python3 tests/ui_live_smoke.py --mock
    python3 tests/ui_live_smoke.py --url http://localhost:8384/
"""
import argparse
import os
import socket
import subprocess
import sys
import time
import traceback

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import ui_live_smoke_lib as lib

try:
    from playwright.sync_api import (sync_playwright, TimeoutError as PWTimeoutError,
                                     Error as PWError)
except ImportError:
    print("ERROR: pip install playwright && playwright install chromium",
          file=sys.stderr)
    sys.exit(1)

RESULTS_DIR = os.path.join(os.path.dirname(os.path.abspath(__file__)),
                           "results", "ui_live")
# --mock's own default out-dir, deliberately NOT RESULTS_DIR: a developer
# running `--mock` on their own Mac after pulling real box evidence via
# `make box-check` must never have reset_output_dir() wipe that evidence.
MOCK_RESULTS_DIR = os.path.join(os.path.dirname(os.path.abspath(__file__)),
                                "results", "ui_live_mock")
MOCK_SCRIPT = os.path.join(os.path.dirname(os.path.abspath(__file__)),
                           "mock_server.py")
MOCK_PORT = int(os.environ.get("PGWT_UI_LIVE_MOCK_PORT", "18830"))

# Turns unhandled promise rejections into console errors, same idiom as
# test_web_ui.py / test_web_ui_chaos.py, so the console guard catches them.
UNHANDLED_REJECTION_HOOK = """
window.addEventListener('unhandledrejection', e => {
    const r = e.reason;
    console.error('Unhandled rejection: ' +
        ((r && (r.stack || r.message)) || String(r)));
});
"""

# Reused verbatim from tests/test_web_ui_chaos.py's _LEAK_PROBE (the B3
# soak-test acceptance probe): live ECharts instances + uPlot roots + pending
# transport requests.
LEAK_PROBE_JS = """() => {
    let charts = 0;
    if (typeof echarts !== 'undefined') {
        for (const el of document.querySelectorAll('*')) {
            try { if (echarts.getInstanceByDom(el)) charts++; } catch (e) {}
        }
    }
    const uplots = document.querySelectorAll('.uplot').length;
    const t = window.__pgwt && window.__pgwt.transport;
    const pending = t ? Object.keys(t.pending).length : -1;
    return { charts, uplots, pending };
}"""

# Installed once per page, right after the WS connects: counts periodic
# 'aas' requests (the persistent AAS pane refreshes on every live tick
# regardless of which tab is active -- app.js startAutoRefresh, 5s cadence)
# so the driver can wait for real ticks instead of a blind sleep multiple.
TICK_HOOK_JS = """() => {
    window.__uiLiveTicks = [];
    const ws = window.__pgwt.transport.ws;
    const send = ws.send.bind(ws);
    ws.send = data => {
        try {
            const msg = JSON.parse(data);
            if (msg && msg.cmd === 'aas') window.__uiLiveTicks.push(Date.now());
        } catch (e) {}
        return send(data);
    };
}"""

# issue #193: reads window.__pgwt.viewMount() -- the ViewManager mount
# chokepoint's own record of the last successful view.mount() ({id, seq,
# at}, `at` = Date.now() in this SAME browser context at mount time),
# exposed by web/static/app.js from web/static/lib/view-manager.js's
# lastMount. Returns {missing: true} rather than throwing when the hook
# itself is absent (an old/broken app.js), so the Python side can tell
# "nothing mounted yet" (a legitimate wait) apart from "this build has no
# such hook" (a loud failure, never a hang -- see
# _view_mount/_wait_for_mount_at_or_after).
VIEW_MOUNT_JS = """() => {
    const fn = window.__pgwt && window.__pgwt.viewMount;
    if (typeof fn !== 'function') return {missing: true};
    return {missing: false, mount: fn()};
}"""

# AAS legend chip colours keyed by event/class name (active.js renderLegend:
# `.aleg` chips, each with a coloured swatch <span> child).
LEGEND_COLORS_JS = """() => {
    const out = {};
    document.querySelectorAll('#aas-legend .aleg').forEach(el => {
        const name = el.textContent.trim();
        const swatch = el.querySelector('span');
        if (swatch) out[name] = getComputedStyle(swatch).backgroundColor;
    });
    return out;
}"""

# Panel-specific "rendered" checks -- same idiom test_web_ui.py already uses
# (read the ECharts option, not just canvas-exists, so an empty series can't
# pass). Table tabs are checked with plain DOM row counts.
_TABLE_CHECK = """() => {
    const rows = document.querySelectorAll('#table-container table tbody tr');
    if (!rows || rows.length === 0) return {ok:false, detail:'no rows'};
    return {ok:true, detail: rows.length + ' rows'};
}"""

PANEL_CHECKS = {
    "overview": _TABLE_CHECK,
    "events": _TABLE_CHECK,
    "sessions": _TABLE_CHECK,
    "queries": _TABLE_CHECK,
    "histogram": """() => {
        const el = document.getElementById('heatmap-container');
        if (!el) return {ok:false, detail:'no container'};
        const c = echarts.getInstanceByDom(el);
        if (!c) return {ok:false, detail:'no echarts instance'};
        const s0 = c.getOption().series && c.getOption().series[0];
        if (!s0) return {ok:false, detail:'no series'};
        if (s0.type !== 'heatmap') return {ok:false, detail:'not a heatmap: ' + s0.type};
        if (!s0.data || s0.data.length === 0) return {ok:false, detail:'no data'};
        return {ok:true, detail: s0.data.length + ' cells'};
    }""",
    # The series length alone cannot say WHICH render path drew the tab: above
    # ~2 spans per painted pixel the timeline draws class-share SEGMENTS (one
    # or more per pixel column) instead of one rect per wait (#106), and
    # "N spans" reads identically either way. The banner the user sees carries
    # the density sentence, so report it — box evidence then states which path
    # the live data took, and the item count is labelled for what it is.
    "timeline": """() => {
        const el = document.getElementById('timeline-chart');
        if (!el) return {ok:false, detail:'no container'};
        const c = echarts.getInstanceByDom(el);
        if (!c) return {ok:false, detail:'no echarts instance'};
        const s0 = c.getOption().series && c.getOption().series[0];
        if (!s0) return {ok:false, detail:'no series'};
        if (s0.type !== 'custom') return {ok:false, detail:'not custom: ' + s0.type};
        if (!s0.data || s0.data.length === 0) return {ok:false, detail:'no spans'};
        const b = document.getElementById('timeline-banner');
        const note = b ? b.textContent.trim() : '';
        // "aggregated at " is the load-bearing substring of the density
        // banner (lib/builders/timeline.js timelineBannerNote says so).
        const aggregated = note.indexOf('aggregated at ') >= 0;
        return {ok:true, detail: s0.data.length +
            (aggregated ? ' segments' : ' spans') +
            (note ? ' | ' + note : '')};
    }""",
    "transitions": """() => {
        const el = document.getElementById('dfg-container');
        if (!el) return {ok:false, detail:'no container'};
        const c = echarts.getInstanceByDom(el);
        if (!c) return {ok:false, detail:'no echarts instance'};
        const s0 = c.getOption().series && c.getOption().series[0];
        if (!s0) return {ok:false, detail:'no series'};
        if (s0.type !== 'graph') return {ok:false, detail:'not a graph: ' + s0.type};
        if (!s0.data || s0.data.length === 0) return {ok:false, detail:'no nodes'};
        return {ok:true, detail: s0.data.length + ' nodes'};
    }""",
    "concurrency": """() => {
        const el = document.getElementById('concurrency-chart');
        if (!el) return {ok:false, detail:'no container'};
        const c = echarts.getInstanceByDom(el);
        if (!c) return {ok:false, detail:'no echarts instance'};
        const s0 = c.getOption().series && c.getOption().series[0];
        if (!s0) return {ok:false, detail:'no series'};
        if (s0.type !== 'line') return {ok:false, detail:'not line: ' + s0.type};
        if (!s0.data || s0.data.length === 0) return {ok:false, detail:'no peak data'};
        return {ok:true, detail: s0.data.length + ' buckets'};
    }""",
    "waterfall": """() => {
        const rows = document.querySelectorAll('#executions-table tbody tr');
        if (!rows || rows.length === 0) return {ok:false, detail:'no executions'};
        const el = document.getElementById('waterfall-chart');
        if (!el) return {ok:false, detail:'no chart container'};
        const c = echarts.getInstanceByDom(el);
        if (!c) return {ok:false, detail:'no echarts instance'};
        const y = c.getOption().yAxis && c.getOption().yAxis[0];
        if (!y || !y.data || y.data.length === 0) return {ok:false, detail:'no lanes'};
        return {ok:true, detail: rows.length + ' executions, ' + y.data.length + ' lanes'};
    }""",
    "scatter": """() => {
        const el = document.getElementById('scatter-chart');
        if (!el) return {ok:false, detail:'no container'};
        const c = echarts.getInstanceByDom(el);
        if (!c) return {ok:false, detail:'no echarts instance'};
        const s0 = c.getOption().series && c.getOption().series[0];
        if (!s0) return {ok:false, detail:'no series'};
        if (!s0.data || s0.data.length === 0) return {ok:false, detail:'no points'};
        return {ok:true, detail: s0.data.length + ' points'};
    }""",
    "matrix": """() => {
        const el = document.getElementById('matrix-chart');
        if (!el) return {ok:false, detail:'no container'};
        const c = echarts.getInstanceByDom(el);
        if (!c) return {ok:false, detail:'no echarts instance'};
        const s0 = c.getOption().series && c.getOption().series[0];
        if (!s0) return {ok:false, detail:'no series'};
        if (s0.type !== 'heatmap') return {ok:false, detail:'not a heatmap: ' + s0.type};
        if (!s0.data || s0.data.length === 0) return {ok:false, detail:'no cells'};
        return {ok:true, detail: s0.data.length + ' cells'};
    }""",
}

# Ready selector waited on right after navigating to a tab, before the
# per-tick render check is trusted (avoids racing the first setOption).
_READY_SELECTOR = {
    "overview": "#table-container table tbody tr",
    "events": "#table-container table tbody tr",
    "sessions": "#table-container table tbody tr",
    "queries": "#table-container table tbody tr",
    "histogram": "#heatmap-container canvas",
    "timeline": "#timeline-chart canvas",
    "transitions": "#dfg-container canvas, #dfg-container svg",
    "concurrency": "#concurrency-chart canvas",
    "waterfall": "#waterfall-chart canvas",
    "scatter": "#scatter-chart canvas",
    "matrix": "#matrix-chart canvas",
}

# The element the no_blink two-frame diff is scoped to. Deliberately just the
# tab's OWN panel, not the full viewport: the persistent AAS pane (shown on
# every tab, see test_web_ui_chaos.py's leak-probe comment) draws its own
# live cursor/axis-pointer machinery that is expected to shimmer a little
# between two frames of the SAME data (docs/VISUAL_CHECKLIST.md STABILITY
# note) -- that is a known, accepted source, not something every OTHER
# panel's blink check should also have to absorb.
_PANEL_ELEMENT_SELECTOR = {
    "overview": "#table-container",
    "events": "#table-container",
    "sessions": "#table-container",
    "queries": "#table-container",
    "histogram": "#heatmap-container",
    "timeline": "#timeline-chart",
    "transitions": "#dfg-container",
    "concurrency": "#concurrency-chart",
    "waterfall": "#waterfall-chart",
    "scatter": "#scatter-chart",
    "matrix": "#matrix-chart",
}

TICK_TIMEOUT_S = 30  # generous multiple of the 5s tick cadence

# issue #193 (round 1): the gating check used to be a single pair of frames
# anchored on a fixed delay after the tick's own AAS-request timestamp
# (BLINK_PAIR_ANCHOR_MS, 500ms -- see git history for the issue #119
# gate-box evidence that picked that value). That anchor was provably wrong
# under real load: app.js's refresh() awaits the summary pane's own round
# trip BEFORE calling ViewManager.refresh() (web/static/app.js), so the
# ACTIVE TAB's data lands later than any guess made from the AAS send time.
#
# issue #193 (round 2, review): round 1's fix kept a SINGLE anchored pair,
# just re-anchored on the ViewManager mount chokepoint's own event instead
# of a fixed delay. Review then PROVED that a single pair -- anchored on
# ANYTHING -- is blind to a transient narrower than its own capture window:
# injecting a 400ms blank overlay into a view (the PR #188 shape) and
# running the live check both ways measured ratio 0.0 on the old fixed pair
# AND 0.0 on the mount-anchored pair, because both compare two frames that
# land wholly inside or wholly outside the transient. The ONLY instrument
# that caught it was the issue #119 offset sweep (SWEEP_OFFSETS_MS) at
# ratio 1.0 on every consecutive pair -- multiple samples spread across the
# tick, not two samples chosen up front, are what actually detect a
# transient of unknown width and unknown position.
#
# The gate is now BUILT ON the sweep instead of a separate pair: the sweep
# itself is anchored on the mount event (round 1's genuine insight -- the
# AAS-send-time anchor undershoots under load, proven by the rehearsal's own
# pair_offsets_ms drifting to 1-10s) via _wait_for_mount_at_or_after, then
# graded on its WORST (max) consecutive-pair ratio
# (ui_live_smoke_lib.blink_sweep_gate_verdict) instead of on one pair.
# SWEEP_OFFSETS_MS (ui_live_smoke_lib.py) is unchanged -- the same 5 values
# issue #119 picked from real gate-box evidence, just measured from the
# mount event now instead of the tick's AAS send.
#
# The sequence is re-checked immediately before the sweep's first capture
# and immediately after its last; if it advanced anywhere in that bracket
# (a second tick's mount landing mid-sweep), the WHOLE sweep straddled a
# real content boundary and the tick is reported NOT MEASURED (never a
# fabricated ratio) -- see ui_live_smoke_lib.blink_sweep_gate_verdict. A tab
# also needs at least ui_live_smoke_lib.MIN_MEASURED_FRACTION of its ticks
# actually measured to pass at all (review SHOULD-FIX: a tab that discarded
# most of its ticks as not-measured must not report ok on the remainder
# alone).


class SmokeFailure(Exception):
    """A tab could not even be checked (fail loudly, never skip)."""


class ConsoleErrorGuard:
    """Same idiom as test_web_ui.py / test_web_ui_chaos.py: collects
    console errors and page errors. '[pgwt]'-prefixed console.error calls
    are the app's OWN failure reporting, not a crash -- they are recorded
    verbatim but do not fail the guard by themselves."""

    def __init__(self, page):
        self.errors = []
        page.on("console", self._on_console)
        page.on("pageerror", self._on_pageerror)

    def _on_console(self, msg):
        if msg.type == "error":
            self.errors.append(f"console: {msg.text}")

    def _on_pageerror(self, err):
        self.errors.append(f"pageerror: {err}")

    def drain(self):
        errors = self.errors
        self.errors = []
        return errors


def _is_pgwt_error(e):
    return e.startswith("console: [pgwt]")


# ── mock front-end ───────────────────────────────────────────────────────────

def _wait_ports(proc, ports, timeout=10.0):
    deadline = time.time() + timeout
    for p in ports:
        while True:
            if proc.poll() is not None:
                return False
            try:
                with socket.create_connection(("127.0.0.1", p), timeout=0.25):
                    break
            except OSError:
                if time.time() >= deadline:
                    return False
                time.sleep(0.05)
    return True


def start_mock_server(port):
    proc = subprocess.Popen(
        [sys.executable, MOCK_SCRIPT, "--port", str(port)],
        stdout=subprocess.PIPE, stderr=subprocess.PIPE)
    if not _wait_ports(proc, (port, port + 1)):
        if proc.poll() is None:
            proc.kill()
        out, err = proc.communicate()
        print(f"mock_server failed to start on port {port} "
              f"(WS {port + 1}) — port busy, or the mock crashed:\n"
              f"{out.decode()}\n{err.decode()}",
              file=sys.stderr)
        sys.exit(1)
    return proc


def stop_mock_server(proc):
    proc.terminate()
    try:
        proc.wait(timeout=5)
    except subprocess.TimeoutExpired:
        proc.kill()
        proc.wait()


def mock_app_url(port):
    return f"http://127.0.0.1:{port}/?ws=ws://127.0.0.1:{port + 1}/ws"


# Two independent app.js mechanisms pause live mode as a SIDE EFFECT of
# navigation, and both apply here:
#   - switchTab(): entering transitions/concurrency/waterfall/scatter/matrix
#     ("too heavy for the 5s live loop", views/*.js pausesLive) calls
#     stopAutoRefresh().
#   - drill(): EVERY drill gesture (P4) calls stopAutoRefresh(), including the
#     Sessions-row-click that is the only way to reach Timeline with data.
# Rather than special-case which tabs happen to trigger which mechanism (and
# silently go stale the next time either list changes), the driver checks the
# #live-btn's own state after landing on the tab and resumes live itself when
# it finds it paused -- exactly what a user parked on that tab and wanting
# live ticks would do. This deliberately exercises the "heavy" views under
# the same continuous 5s refresh the other tabs get, which is the regression
# this suite exists to catch, not a workaround for a skip.
_LIVE_ACTIVE_JS = "() => { const b = document.getElementById('live-btn'); " \
                  "return !!b && b.classList.contains('active'); }"


# ── tab navigation ───────────────────────────────────────────────────────────

def _navigate_to_tab(page, tab_id, timeout_s):
    """Switches to tab_id, driving the same clicks a user would. Timeline has
    no standalone entry point with data (a bare tab click shows the "select a
    session" prompt) -- it is reached by drilling into a session row, exactly
    like tests/test_web_ui.py's test_timeline_tab / test_sessions_table."""
    if tab_id == "timeline":
        page.click(".tab[data-tab='sessions']")
        page.wait_for_selector("#table-container table tbody tr", timeout=timeout_s * 1000)
        row = page.query_selector("#table-container table tbody tr.clickable")
        if row is None:
            raise SmokeFailure("no clickable session row to drill into for Timeline")
        row.click()
    else:
        page.click(f".tab[data-tab='{tab_id}']")

    try:
        page.wait_for_selector(_READY_SELECTOR[tab_id], timeout=timeout_s * 1000)
    except PWTimeoutError:
        raise SmokeFailure(
            f"panel did not render ({_READY_SELECTOR[tab_id]!r}) within {timeout_s}s")

    if not page.evaluate(_LIVE_ACTIVE_JS):
        live_btn = page.query_selector("#live-btn")
        if live_btn is None:
            raise SmokeFailure("no #live-btn to resume live mode")
        live_btn.click()
        page.wait_for_timeout(500)  # let the resumed refresh settle before ticks count
        if not page.evaluate(_LIVE_ACTIVE_JS):
            raise SmokeFailure("clicking #live-btn did not resume live mode")


def _wait_for_tick(page, target_count, timeout_s):
    try:
        page.wait_for_function(
            "window.__uiLiveTicks && window.__uiLiveTicks.length >= " + str(target_count),
            timeout=timeout_s * 1000)
        return True
    except PWTimeoutError:
        return False


def _safe_panel_screenshot(page, tab_id):
    """Re-queries the panel element fresh and screenshots ONLY the slice of
    it that intersects the current viewport, returning None (never raising)
    if the element is missing, gets detached from the DOM mid-call, or is
    scrolled entirely out of view -- observed for real (Waterfall/Matrix/
    Scatter re-mounting their host div when the underlying execution/
    transition identity rotates under continuous real load); the caller
    treats None as maximal instability, not a skip.

    issue #197: this used to be a plain elementHandle.screenshot() call,
    which -- when the element is taller than the viewport -- Playwright
    captures IN FULL by temporarily expanding the capture region beyond the
    viewport, not just the visible slice. The Sessions panel (#table-container
    has no vertical overflow/height cap -- web/static/style.css -- so it grows
    to its full row count) measured ~5793px tall at 205 rows on the gate box,
    and that single capture cost ~1.4s: the 5-frame blink sweep (SWEEP_OFFSETS_MS
    spans 200-2000ms) then took longer than the 5s live tick it was meant to
    sample inside of, so every sweep straddled a mount and the gate reported a
    red the UI never earned. Clipping the capture to the panel/viewport
    intersection (lib.panel_capture_clip / lib.clip_rect_to_viewport) makes
    one frame's cost roughly constant regardless of row count, and keeps the
    capture anchored to the panel (never a whole-page screenshot, which
    would let unrelated chrome dominate the diff -- see
    _PANEL_ELEMENT_SELECTOR's own comment).

    scroll_into_view_if_needed() first, same as Playwright's own element
    screenshot semantics, so the visible slice is deterministic (the panel's
    top edge at/near the viewport top) instead of whatever the page happened
    to be scrolled to already.

    query_selector() + .bounding_box() + .screenshot() are separate CDP round
    trips: every chart-tab view rebuilds its host div via el.innerHTML on
    EVERY refresh (not just on a data-identity change -- see
    web/static/views/exec-scatter.js mount() / matrix.js mount()),
    synchronously and atomically from the page's own single JS thread's point
    of view. If that rebuild happens to run in the gap between these round
    trips, a handle this function already holds goes stale and the next call
    throws -- a real, if narrow, Playwright-side race against a legitimate,
    instantaneous DOM swap, not evidence the panel was ever actually absent/
    blank. Fine for THIS function's own callers (the gating offset sweep --
    issue #119 -- which already treats a None result as "maximal
    instability", never a crash, exactly because of this). NOT safe for a
    single-sample hard pass/fail check -- see _atomic_panel_snapshot below,
    used for that."""
    panel = page.query_selector(_PANEL_ELEMENT_SELECTOR[tab_id])
    if panel is None:
        return None
    try:
        panel.scroll_into_view_if_needed()
        box = panel.bounding_box()
        viewport = page.viewport_size
        # lib.panel_capture_clip returns None for "cannot safely determine
        # a clip" (missing box OR missing viewport) -- treated the same as
        # every other "cannot capture" case in this function, NEVER a
        # fallback to panel.screenshot()'s unbounded, full-element capture
        # (issue #197 review: that fallback silently reintroduces the exact
        # cost regression this function exists to fix, with no signal in
        # summary.json -- see panel_capture_clip's own docstring).
        clip = lib.panel_capture_clip(box, viewport)
        if clip is None:
            return None
        return page.screenshot(clip=clip)
    except PWError:
        return None


# issue #142: single atomic page.evaluate() -- query the selector AND read its
# content in the SAME synchronous JS turn, so there is no gap for a page-side
# view's own (synchronous, atomic) el.innerHTML rebuild to straddle, unlike
# _safe_panel_screenshot's two separate CDP round trips (query_selector, then
# a LATER .screenshot() call). Reads the chart's own <canvas> pixels via
# toDataURL() when present (same JS thread, no separate screenshot API call);
# falls back to a DOM-content check for table tabs, whose panel selector
# (#table-container) is the ViewManager's own stable containerEl and is never
# replaced by a view's mount() (only its children are -- see app.js
# `vm.setContainer(tableEl)`, called once, never re-targeted per view).
_ATOMIC_PANEL_SNAPSHOT_JS = """(sel) => {
    const el = document.querySelector(sel);
    if (!el) return {present: false};
    const canvas = el.querySelector('canvas');
    if (canvas) {
        return {present: true, canvas: true, dataURL: canvas.toDataURL('image/png')};
    }
    return {present: true, canvas: false,
            hasContent: el.children.length > 0 || el.textContent.trim().length > 0};
}"""


def _atomic_panel_snapshot(page, tab_id):
    """Single-round-trip, race-free read of the panel's current state -- see
    _ATOMIC_PANEL_SNAPSHOT_JS. Returns the JS snapshot dict; ui_live_smoke_lib.
    blind_window_ok() turns it into a pass/fail verdict (kept pure/testable
    there, same idiom as every other verdict function in this module)."""
    return page.evaluate(_ATOMIC_PANEL_SNAPSHOT_JS, _PANEL_ELEMENT_SELECTOR[tab_id])


def _capture_at_offset(page, tab_id, base_ts_ms, offset_ms):
    """Sleeps only the remainder to base_ts_ms+offset_ms (never a flat sleep
    from wherever this is called) and screenshots the panel. Returns
    (raw_png_or_None, achieved_offset_ms, capture_ms) -- the achieved offset
    can exceed offset_ms if prior work already ran past the target.
    base_ts_ms is the mount event's own timestamp (issue #193 round 2), not
    the tick's AAS-request send -- see _wait_for_mount_at_or_after.
    capture_ms (issue #197 evidence) is the wall-clock time the screenshot
    call itself took -- the direct, measured answer to "is a frame's capture
    cost bounded", not inferred from achieved-offset drift."""
    target_ms = base_ts_ms + offset_ms
    now_ms = page.evaluate("Date.now()")
    if target_ms > now_ms:
        page.wait_for_timeout(target_ms - now_ms)
        now_ms = page.evaluate("Date.now()")
    t0 = time.monotonic()
    frame = _safe_panel_screenshot(page, tab_id)
    capture_ms = (time.monotonic() - t0) * 1000.0
    return frame, now_ms - base_ts_ms, capture_ms


def _view_mount(page):
    """Reads window.__pgwt.viewMount() (issue #193's mount chokepoint hook:
    web/static/lib/view-manager.js's ViewManager.lastMount, exposed by
    web/static/app.js). Returns the raw {"id", "seq", "at"} dict, or None
    before the first mount of the page's lifetime.

    Raises SmokeFailure if the hook ITSELF is missing -- an old/broken
    app.js build must fail loudly here, never be silently read as "nothing
    has mounted yet" (which would make _wait_for_mount_at_or_after poll all
    the way to its own timeout looking exactly like a slow-but-real wait,
    hiding a missing-instrumentation bug behind a generic timeout
    message)."""
    result = page.evaluate(VIEW_MOUNT_JS)
    if result.get("missing"):
        raise SmokeFailure(
            "window.__pgwt.viewMount() is missing -- the mount-sequence "
            "instrumentation (issue #193) is not present in this build")
    return result.get("mount")


def _wait_for_mount_at_or_after(page, tab_id, tick_ts_ms, timeout_s=TICK_TIMEOUT_S,
                                 interval_ms=50, min_seq=None):
    """Polls until the ViewManager mount chokepoint (_view_mount) reports a
    mount of `tab_id` whose OWN wall-clock timestamp (`at`, Date.now() in
    the SAME browser context as tick_ts_ms) is >= tick_ts_ms -- issue #193's
    replacement for a fixed delay from the tick's AAS-request send: the
    summary pane's own round trip runs before ViewManager.refresh()
    (web/static/app.js's refresh()), so a fixed guess from the send time
    undershoots under load; this waits for the actual paint decision
    instead of guessing when it happens.

    Compares TIMESTAMPS, not a FIXED seq baseline read once at navigation
    time (review round 2 found that off by one): navigating to a tab already
    triggers its own refresh()/mount() (switchTab's refreshActive() +
    vm.switchTo(), which is also __uiLiveTicks' very first entry), so a seq
    value read right after navigation already reflects tick 1's own mount
    -- waiting for "seq > that ONE baseline, forever" there would silently
    wait for tick 2's mount instead, and every later tick would inherit the
    one-tick shift. A timestamp comparison alone has no such fixed baseline
    to get wrong: `mount.at >= tick_ts_ms` is correct for tick 1 the exact
    same way it is for every later tick.

    min_seq (issue #197, per-call, NOT a fixed baseline -- the caller passes
    the PREVIOUS TICK's own consumed mount seq each time, None for tick 1):
    timestamp alone is not sufficient. A mount whose own refresh cycle takes
    longer than the 5s live tick interval can land with `at` already past
    the NEXT tick's tick_ts, so a purely timestamp-based wait can be handed
    back the SAME mount the previous tick's sweep already anchored to and
    consumed -- that tick's whole 5-frame sweep then fires back-to-back
    against an already-stale mount_at_ms, with none of the spacing across
    the tick that lets it see a sub-second transient at all (evidence: the
    rehearsal's achieved first offsets drifted to 2.5-4.5s instead of the
    ~200ms target -- one live mount sampled multiple times, same seq, same
    pixels). min_seq requires the accepted mount's OWN seq to be strictly
    newer than the one already consumed (lib.mount_is_fresh) -- this is
    per-tick state threaded through by the caller, not the single
    navigation-time baseline round 2 rejected, so it does not reintroduce
    that bug: tick 1 passes min_seq=None (no previous tick to be newer
    than), exactly like the timestamp check already does.

    Raises SmokeFailure -- NEVER hangs, never silently returns a stale
    record -- if the hook is missing (see _view_mount) or no matching FRESH
    mount lands within timeout_s. Both are real product-visible failures:
    the first means the instrumentation itself regressed, the second means
    the tab's view never repainted a NEW mount for a whole tick's worth of
    budget (a frozen tab, or -- pre-fix -- the harness quietly re-sweeping a
    stale one forever)."""
    deadline = time.monotonic() + timeout_s
    while True:
        mount = _view_mount(page)
        if lib.mount_is_fresh(mount, tab_id, tick_ts_ms, min_seq):
            return mount
        if time.monotonic() >= deadline:
            raise SmokeFailure(
                f"no FRESH view.mount() of tab {tab_id!r} at/after this "
                f"tick's AAS send ({tick_ts_ms}) with seq > {min_seq!r} "
                f"landed within {timeout_s}s (issue #193/#197 mount-anchor "
                "wait)")
        page.wait_for_timeout(interval_ms)


def _poll_render_check(page, tab_id, timeout_s=2.0, interval_ms=150):
    """Evaluates PANEL_CHECKS[tab_id], retrying briefly on failure. A tick's
    re-render (ECharts dispose+init when the underlying data identity
    changes -- e.g. Waterfall's "latest execution" rotating under continuous
    real load, observed against a real daemon) can take a moment after the
    tick's WS response lands before the chart instance exists again. Bounded
    exactly like Playwright's own wait_for_selector: a check STILL failing
    after this budget is a real failure, reported as such, never swallowed."""
    deadline = time.monotonic() + timeout_s
    while True:
        raw = page.evaluate(PANEL_CHECKS[tab_id])
        ok, detail = lib.render_check_ok(raw)
        if ok or time.monotonic() >= deadline:
            return ok, detail
        page.wait_for_timeout(interval_ms)


def _settled_leak_probe(page, timeout_s=TICK_TIMEOUT_S, interval_ms=200):
    """Poll LEAK_PROBE_JS until `pending` settles to 0 or the budget runs
    out. A single-instant read can catch a request that is normally
    in-flight for a moment (a real server under real load, vs. the
    zero-latency mock) and wrongly call it a leak -- test_web_ui_chaos.py's
    test_soak_random_navigation re-probes the SAME probe after a settle
    window for exactly this reason; this is the same idiom, not a widened
    tolerance (a request stuck pending for the whole budget still fails).
    Budget matches TICK_TIMEOUT_S: measured on the gate box, a `transitions`/
    `exec_scatter`/`top_queries` query over several minutes of --mode full
    capture (every event, not sampled) can legitimately take a few seconds
    to answer -- the same real-server-latency reasoning that sized
    TICK_TIMEOUT_S already, not a separate ad hoc allowance.

    Returns (probe, elapsed_s): the elapsed time is recorded in summary.json
    (issue #93 review item 6) even when it does not, by itself, fail
    no_leak -- a probe that took most of the budget to settle is a latency
    regression worth a trace."""
    start = time.monotonic()
    deadline = start + timeout_s
    probe = page.evaluate(LEAK_PROBE_JS)
    while probe.get("pending", 0) != 0 and time.monotonic() < deadline:
        page.wait_for_timeout(interval_ms)
        probe = page.evaluate(LEAK_PROBE_JS)
    return probe, time.monotonic() - start


def _pid_is_live(pid):
    """True if pid exists and is not a zombie. os.kill(pid, 0) alone is not
    enough (issue #93 review nit): it succeeds for a zombie too -- the PID
    stays reserved until the parent reaps it, so a dead-but-unreaped pgbench/
    workload process would pass as "alive" and the walk would keep grading
    an idle system. /proc/<pid>/stat's state field (immediately after the
    LAST ')' -- the command name itself can contain parens) is 'Z' for a
    zombie; anything else, or an unreadable/missing /proc entry meaning the
    process is fully gone, is handled by the caller."""
    try:
        with open(f"/proc/{pid}/stat") as f:
            content = f.read()
    except OSError:
        return False
    fields_after_comm = content.rsplit(")", 1)[-1].split()
    state = fields_after_comm[0] if fields_after_comm else ""
    return state != "Z"


def _assert_workload_alive(pgbench_pid, workload_pid, where):
    """FAILS LOUDLY (raises SystemExit) if either the pgbench or the lock/
    sleep workload process is gone (or a zombie -- see _pid_is_live). Called
    at each tab boundary (issue #93 review item 2): a controlled-load
    process that died partway through the walk would leave the REMAINING
    tabs grading an idle system -- rendered/no_blink/no_leak could all still
    trivially "pass" against stale, frozen data, which is exactly the false
    confidence this whole test exists to prevent. None for either pid means
    the caller did not wire this check up (e.g. --mock, or a manual --url
    run) -- skipped, not failed."""
    for label, pid in (("pgbench", pgbench_pid), ("lock/sleep workload", workload_pid)):
        if pid is None:
            continue
        if not _pid_is_live(pid):
            print(f"FATAL: {label} (pid {pid}) is no longer running ({where}) -- "
                  f"refusing to grade the remaining tabs against an idle system",
                  file=sys.stderr)
            sys.exit(1)


# ── one tab's walk ───────────────────────────────────────────────────────────

def run_tab(browser, tab_id, url, out_dir, ticks, first_data_timeout,
            blink_threshold, viewport=None, device_scale_factor=None):
    """viewport / device_scale_factor (issue #223): only tests/demo_rehearsal.py
    passes these (the criteria doc's pinned demo client viewport, 1710x981 at
    DPR 2 -- see docs/DEMO_REHEARSAL_CRITERIA.md and
    docs/chrome-demo-viewport-2026-09-28.md). ui_live_smoke.py's own CI-tier
    walk (main(), below) passes neither, keeping the existing 1280x900 DSF 1
    -- a different gate with different goals; widening it would slow every PR
    for nothing.

    Whichever viewport is requested, it is never trusted on faith: right
    after the page loads, the ACTUAL innerWidth/innerHeight/devicePixelRatio
    are read back and compared against what was requested
    (lib.viewport_mismatch_reason) -- docs/DEMO_REHEARSAL_CRITERIA.md names
    exactly this trap ("assert the viewport actually obtained, not the one
    requested" / "assert the effective zoom too"): a request that never
    actually reached browser.new_context() must fail loudly here, not read
    as a silent, plausible-looking pass."""
    req_width = viewport["width"] if viewport else 1280
    req_height = viewport["height"] if viewport else 900
    req_dsf = device_scale_factor if device_scale_factor is not None else 1
    tab_dir = os.path.join(out_dir, tab_id)
    os.makedirs(tab_dir, exist_ok=True)
    context = browser.new_context(
        viewport={"width": req_width, "height": req_height},
        device_scale_factor=req_dsf,
        record_video_dir=tab_dir,
        record_video_size={"width": req_width, "height": req_height})
    context.add_init_script(UNHANDLED_REJECTION_HOOK)
    page = context.new_page()
    guard = ConsoleErrorGuard(page)
    console_errors = []
    pgwt_errors = []
    tick_paths = []
    video_path = None
    try:
        page.goto(url)
        actual = page.evaluate(
            "() => ({w: window.innerWidth, h: window.innerHeight, "
            "dpr: window.devicePixelRatio})")
        viewport_reason = lib.viewport_mismatch_reason(
            req_width, req_height, req_dsf,
            actual["w"], actual["h"], actual["dpr"])
        if viewport_reason is not None:
            raise SmokeFailure(
                f"viewport not achieved as requested: {viewport_reason}")
        try:
            page.wait_for_selector("#status.connected", timeout=first_data_timeout * 1000)
        except PWTimeoutError:
            raise SmokeFailure(
                f"WebSocket never reached #status.connected within {first_data_timeout}s")

        # No-data fail-safe (issue #93 acceptance item 6): real rows, not a
        # perpetual '.loading' placeholder, within the fixed budget.
        deadline = time.monotonic() + first_data_timeout
        while page.query_selector("#table-container table tbody tr") is None:
            if time.monotonic() > deadline:
                raise SmokeFailure(f"UI showed no data within {first_data_timeout}s")
            page.wait_for_timeout(200)

        page.evaluate(TICK_HOOK_JS)
        _navigate_to_tab(page, tab_id, first_data_timeout)

        leak_before, leak_before_settle_s = _settled_leak_probe(page)

        legend_ticks = []
        blink_ratios = []
        blink_pair_offsets_ms = []
        blink_sweep_ticks = []
        blink_not_measured = []
        # issue #197: the seq of the mount the PREVIOUS tick's sweep
        # consumed, so _wait_for_mount_at_or_after refuses to hand back the
        # same (stale) mount to this tick -- see its own docstring and
        # lib.mount_is_fresh. None for tick 1 (no previous tick to be newer
        # than).
        last_consumed_mount_seq = None
        # issue #100 (review round 4): timeline-only diagnostic, one entry
        # per tick -- see the capture site below for what it measures and
        # why. Empty for every other tab; never read by build_tab_result's
        # `ok` computation.
        pre_mount_diagnostics = []
        render_ok, render_detail = False, "never checked"
        for i in range(1, ticks + 1):
            if not _wait_for_tick(page, i, TICK_TIMEOUT_S):
                raise SmokeFailure(
                    f"tick {i}/{ticks} did not arrive within {TICK_TIMEOUT_S}s")
            tick_ts_ms = page.evaluate(
                "window.__uiLiveTicks[window.__uiLiveTicks.length - 1]")

            # issue #100 (review round 4, corrected round 5), TIMELINE
            # ONLY: try to capture the panel as the PREVIOUS tick's mount
            # left it, before THIS tick's own mount can touch it, then diff
            # it below against the sweep's own first frame (mount+200ms) to
            # test #100's premise (the reported ~4.5% "unchanged-data"
            # redraw was actually the tick's own legitimate window-advance
            # repaint -- axis labels/gridlines, timeline.js's xAxis sits at
            # the bottom of the grid -- caught mid-flight by the OLD
            # 1200ms-anchor instrument). Never gates: reporting-only, read
            # by nothing in build_tab_result.
            #
            # Round 4's mistake: it assumed "nothing repaints containerEl
            # between mounts" (true, ViewManager's own contract) meant this
            # capture was safe to trust unconditionally. It does not -- that
            # invariant says nothing about whether OUR OWN capture (a real
            # round trip: page.evaluate + a screenshot + PNG decode, real
            # wall-clock time) finishes BEFORE the next mount, which is
            # timeline's own live race (its mount can land ~150ms after the
            # tick, sometimes faster than this capture completes). Asserted,
            # never proven -- and on a real run it was proven WRONG twice
            # (ticks 5-6, review round 5): a genuine window-advance repaint
            # happened on both, confirmed by diffing the run's own saved
            # per-tick frames, while this probe read 0.0 on the exact same
            # ticks, because its "before" frame was actually taken AFTER.
            #
            # Fixed the same way the gating sweep already brackets its own
            # capture: read the mount seq immediately AFTER this screenshot;
            # only trust the frame as "before" if that seq is STILL behind
            # THIS tick's eventual mount once it lands (checked further
            # down, once `mount` is known, via
            # lib.pre_mount_diagnostic_verdict). None (no mount observed at
            # all yet) trivially precedes any mount that will ever land.
            pre_mount_frame = None
            pre_mount_seq_after = None
            if tab_id == "timeline":
                pre_mount_frame = _safe_panel_screenshot(page, tab_id)
                m_after = _view_mount(page)
                pre_mount_seq_after = m_after["seq"] if m_after is not None else None

            # Blind-window check (issue #93 review item 5): between the tick
            # landing and the render-check retry + gating settle below,
            # NOTHING is sampled -- a teardown-to-blank flash right after the
            # tick (a CONTINUITY violation) would go completely unnoticed by
            # the later, already-resettled blink pair. Grab one atomic
            # snapshot ~100ms after the tick and assert the panel is not
            # blank. Widens nothing: this is an ADDITIONAL check, not a
            # relaxed one.
            #
            # issue #142: this used to be _safe_panel_screenshot() (two
            # separate CDP round trips: query_selector, then a LATER
            # .screenshot() call), which intermittently reported scatter/
            # matrix as "gone" -- not because the panel was ever actually
            # absent or blank, but because chart-tab views rebuild their host
            # div via el.innerHTML on EVERY refresh (synchronously, so no
            # externally-observable in-between state exists -- see
            # _safe_panel_screenshot's own updated docstring), and that
            # rebuild occasionally landed in the gap between the two round
            # trips, staling the handle this function already held.
            # _atomic_panel_snapshot reads the panel in a SINGLE
            # page.evaluate() call, closing that gap by construction: the
            # panel is observed at a moment the product actually holds a
            # single, atomic DOM/paint state, never a torn one.
            page.wait_for_timeout(100)
            ok, reason = lib.blind_window_ok(_atomic_panel_snapshot(page, tab_id))
            if not ok:
                raise SmokeFailure(
                    f"tick {i}: {reason} ~100ms after the tick "
                    "(CONTINUITY teardown-to-blank)")

            render_ok, render_detail = _poll_render_check(page, tab_id)
            if not render_ok:
                raise SmokeFailure(f"tick {i}: {render_detail}")

            # issue #193 (round 2, review): the GATING check IS the offset
            # sweep (issue #119, SWEEP_OFFSETS_MS), anchored on the mount
            # chokepoint's own event for this tab instead of a fixed delay
            # from the tick's AAS-request send. Review proved a single
            # anchored pair -- however it is anchored -- is blind to a
            # transient narrower than its own capture window (an injected
            # 400ms overlay measured ratio 0.0 on the pair, 1.0 on every
            # sweep offset); multiple samples spread across the tick are
            # what actually catch a transient of unknown width/position.
            #
            # 1. Wait for a mount of THIS TAB whose own timestamp is at/
            #    after this tick's AAS send AND whose seq is strictly newer
            #    than the previous tick's own consumed mount
            #    (_wait_for_mount_at_or_after) -- the chokepoint's own signal
            #    that a fresh response was actually painted, never a guess
            #    at when that happens, and never the same mount a previous
            #    tick already swept (issue #197).
            mount = _wait_for_mount_at_or_after(page, tab_id, tick_ts_ms, TICK_TIMEOUT_S,
                                                 min_seq=last_consumed_mount_seq)
            mount_at_ms = mount["at"]
            last_consumed_mount_seq = mount["seq"]
            # issue #93 review item 3, kept: record how long after the AAS
            # send the mount landed, so a future drift under load stays
            # visible in summary.json.
            achieved_a_ms = mount_at_ms - tick_ts_ms
            blink_pair_offsets_ms.append(achieved_a_ms)

            # 2. Re-check the sequence immediately before the sweep's first
            #    capture and immediately after its last, bracketing the
            #    WHOLE sweep -- if it moved anywhere in between (a second
            #    tick's mount landing mid-sweep), the sweep straddled a real
            #    content boundary and must be reported NOT MEASURED, never
            #    a fabricated ratio.
            seq_before_sweep = mount["seq"]
            sweep_offsets = list(lib.SWEEP_OFFSETS_MS)
            sweep_raw_frames = []
            sweep_achieved_ms = []
            sweep_capture_ms = []
            for offset in sweep_offsets:
                frame, achieved, capture_ms = _capture_at_offset(page, tab_id, mount_at_ms, offset)
                sweep_raw_frames.append(frame)
                sweep_achieved_ms.append(achieved)
                sweep_capture_ms.append(capture_ms)
            after_sweep = _view_mount(page)
            seq_after_sweep = after_sweep["seq"] if after_sweep is not None else None

            sweep_arrays = [lib.png_bytes_to_array(f) if f is not None else None
                            for f in sweep_raw_frames]

            # issue #100 (review round 4, corrected round 5): pre-mount vs
            # sweep's own first frame, timeline only -- see the capture site
            # above for what precedes and why, and
            # lib.pre_mount_diagnostic_verdict's own docstring for the
            # precedence check itself.
            if tab_id == "timeline":
                pre_mount_arr = (lib.png_bytes_to_array(pre_mount_frame)
                                 if pre_mount_frame is not None else None)
                sweep_first_arr = sweep_arrays[0] if sweep_arrays else None
                sig = lib.pre_mount_diagnostic_verdict(
                    pre_mount_arr, sweep_first_arr, pre_mount_seq_after, mount["seq"])
                pre_mount_diagnostics.append({"tick": i, **sig})

            ratio, blink_note = lib.blink_sweep_gate_verdict(
                sweep_arrays, seq_before_sweep, seq_after_sweep)
            if blink_note:
                print(f"  note [{tab_id}] tick {i}: {blink_note}")
            if ratio is None:
                blink_not_measured.append({"tick": i, "reason": blink_note})
            else:
                blink_ratios.append(ratio)

            blink_sweep_ticks.append(
                lib.build_sweep_tick_record(sweep_achieved_ms, sweep_arrays,
                                            target_offsets_ms=sweep_offsets,
                                            capture_ms=sweep_capture_ms,
                                            mount_seq=mount["seq"]))

            # The sweep's smallest-offset frame doubles as this tick's
            # artifact PNG (closest analogue to the old gating pair's
            # frame_a).
            if sweep_raw_frames and sweep_raw_frames[0] is not None:
                tick_path = os.path.join(tab_dir, f"tick-{i}.png")
                with open(tick_path, "wb") as f:
                    f.write(sweep_raw_frames[0])
                tick_paths.append(tick_path)

            legend = page.evaluate(LEGEND_COLORS_JS)
            if legend:
                legend_ticks.append(legend)

            for e in guard.drain():
                if _is_pgwt_error(e):
                    # The app's own failure reporting -- never fails the tab
                    # by itself, but must be visible, not dropped silently
                    # (issue #93 review item 3).
                    print(f"  note [{tab_id}] tick {i}: {e}")
                    pgwt_errors.append(e)
                else:
                    console_errors.append(e)

        leak_after, leak_after_settle_s = _settled_leak_probe(page)
        color_violations = (lib.color_stability_violations(legend_ticks)
                            if legend_ticks else [])
        # blink_ratios holds only MEASURED ticks (blink_sweep_gate_verdict's
        # ratio=None/"not measured" ticks are excluded, tracked separately
        # in blink_not_measured) -- so an all-not-measured tab lands here as
        # an empty list, worst_blink=None, and no_blink_ok(None, ...) is
        # False by construction: an unmeasured signal fails the gate rather
        # than silently passing it (issue #193). build_tab_result ALSO
        # requires a minimum fraction of ticks to be measured at all
        # (review SHOULD-FIX) -- a tab that discarded most of its ticks as
        # not-measured must not report ok on the remainder alone.
        worst_blink = max(blink_ratios) if blink_ratios else None

        result = lib.build_tab_result(
            tab_id, render_ok, render_detail, len(tick_paths), console_errors,
            worst_blink, color_violations, leak_before, leak_after,
            artifacts={}, blink_threshold=blink_threshold,
            pgwt_console_errors=pgwt_errors,
            leak_before_settle_s=leak_before_settle_s,
            leak_after_settle_s=leak_after_settle_s,
            blink_pair_offsets_ms=blink_pair_offsets_ms,
            blink_sweep_ticks=blink_sweep_ticks,
            blink_not_measured=blink_not_measured,
            pre_mount_diagnostics=pre_mount_diagnostics)
    except SmokeFailure as e:
        print(f"  FAIL [{tab_id}]: {e}", file=sys.stderr)
        result = lib.build_failed_tab_result(tab_id, str(e),
                                             ticks_observed=len(tick_paths),
                                             pgwt_console_errors=pgwt_errors)
    except Exception:
        traceback.print_exc()
        result = lib.build_failed_tab_result(
            tab_id, "unhandled exception (see stderr traceback)",
            ticks_observed=len(tick_paths), pgwt_console_errors=pgwt_errors)
    finally:
        video = page.video
        context.close()
        if video is not None:
            try:
                src = video.path()
                dst = os.path.join(tab_dir, f"{tab_id}.webm")
                os.replace(src, dst)
                video_path = dst
            except Exception as e:
                print(f"  note: could not finalize video for {tab_id}: {e}",
                      file=sys.stderr)

    result["artifacts"] = {"frames": tick_paths, "video": video_path}
    return result


# ── main ──────────────────────────────────────────────────────────────────────

def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--url", help="page URL of an already-running UI "
                    "(phase 2: the real Go bridge)")
    ap.add_argument("--mock", action="store_true",
                    help="start tests/mock_server.py and walk against it")
    ap.add_argument("--out-dir", default=None,
                    help=f"artifact directory (default {RESULTS_DIR} for "
                    f"--url, {MOCK_RESULTS_DIR} for --mock -- kept separate "
                    "so a local --mock run can never wipe real box evidence)")
    ap.add_argument("--ticks", type=int, default=lib.MIN_TICKS,
                    help=f"live ticks to observe per tab (default {lib.MIN_TICKS}; "
                    "the issue requires >= 6 -- never lower this in a gating run)")
    ap.add_argument("--first-data-timeout", type=float,
                    default=lib.FIRST_DATA_TIMEOUT_S,
                    help="no-data fail-safe in seconds (default "
                    f"{lib.FIRST_DATA_TIMEOUT_S}; issue #93 item 6)")
    ap.add_argument("--blink-threshold", type=float, default=lib.BLINK_THRESHOLD,
                    help="TEST-ONLY override for the deliberate-failure demo "
                    "(issue #93 phase-1 validation step). Gating runs "
                    "(tests/ui_live_smoke.sh) never pass this -- the default "
                    f"is the issue's stated 0.1% ({lib.BLINK_THRESHOLD}).")
    ap.add_argument("--pgbench-pid", type=int, default=None,
                    help="PID of the background pgbench process (from "
                    "tests/ui_live_smoke.sh); checked alive at every tab "
                    "boundary, FAILS LOUDLY if it is gone (review item 2)")
    ap.add_argument("--workload-pid", type=int, default=None,
                    help="PID of the looping lock/sleep workload process "
                    "(from tests/ui_live_smoke.sh); same liveness check as "
                    "--pgbench-pid")
    args = ap.parse_args()

    if bool(args.url) == bool(args.mock):
        ap.error("exactly one of --url or --mock is required")

    mock_proc = None
    if args.mock:
        mock_proc = start_mock_server(MOCK_PORT)
        url = mock_app_url(MOCK_PORT)
    else:
        url = args.url

    out_dir = args.out_dir or (MOCK_RESULTS_DIR if args.mock else RESULTS_DIR)

    # Every artifact under out_dir must belong to THIS run -- delete and
    # recreate it fresh before any tab runs, so a stale tick-N.png from an
    # earlier invocation (mock, real, or a previous box-check) can never
    # survive into this run's summary.json (issue #93 ui-reviewer finding).
    lib.reset_output_dir(out_dir)

    # This run's marker (issue #93 review item 3): tests/run_all.sh exports
    # PGWT_RUN_MARKER (its own start timestamp) and re-emits this run's
    # verdict only if run.id matches it -- tests/results is excluded from
    # box-check.sh's up-rsync, so the box otherwise keeps the LAST run's
    # ui_live/ between invocations, and a smoke that died before the walk
    # would have run_all.sh print a stale, unrelated PASS/FAIL as if it were
    # this run's. Written IMMEDIATELY after reset_output_dir so it survives
    # even a failure on the very first tab. Falls back to the wall clock for
    # --mock/manual runs, which have no run_all.sh marker to match.
    run_marker = os.environ.get("PGWT_RUN_MARKER", str(int(time.time())))
    with open(os.path.join(out_dir, "run.id"), "w") as f:
        f.write(run_marker)

    _assert_workload_alive(args.pgbench_pid, args.workload_pid, "before the walk")
    results = []
    try:
        with sync_playwright() as p:
            browser = p.chromium.launch()
            try:
                for tab_id in lib.TABS:
                    print(f"=== {tab_id} ===")
                    result = run_tab(browser, tab_id, url, out_dir,
                                     args.ticks, args.first_data_timeout,
                                     args.blink_threshold)
                    results.append(result)
                    _assert_workload_alive(args.pgbench_pid, args.workload_pid,
                                           f"after tab {tab_id!r}")
                    kf_line = lib.known_failing_report_line(tab_id, result["ok"])
                    status = kf_line if kf_line else ("PASS" if result["ok"] else "FAIL")
                    print(f"  {status} [{tab_id}] "
                          f"ticks={result['ticks_observed']} "
                          f"rendered={result['rendered']}")
            finally:
                browser.close()
    finally:
        if mock_proc is not None:
            stop_mock_server(mock_proc)

    summary_path = os.path.join(out_dir, "summary.json")
    summary = lib.write_summary(summary_path, results)

    print()
    print("════════════════════════════════════════")
    print("  UI LIVE SMOKE SUMMARY")
    print("════════════════════════════════════════")
    for tab_id in lib.TABS:
        r = summary["tabs"].get(tab_id)
        kf_line = lib.known_failing_report_line(tab_id, r["ok"]) if r else None
        label = kf_line if kf_line else ("PASS" if r and r["ok"] else "FAIL")
        print(f"  {label} {tab_id}")
    print(f"  summary: {summary_path}")
    print(f"  overall: {'PASS' if summary['ok'] else 'FAIL'}"
          + (f" (failed: {', '.join(summary['failed_tabs'])})"
             if summary["failed_tabs"] else "")
          + (f" (known-failing: {', '.join(summary['known_failing_tabs'])})"
             if summary["known_failing_tabs"] else "")
          + (f" (xpass: {', '.join(summary['xpass_tabs'])})"
             if summary["xpass_tabs"] else ""))

    return 0 if summary["ok"] else 1


if __name__ == "__main__":
    sys.exit(main())
