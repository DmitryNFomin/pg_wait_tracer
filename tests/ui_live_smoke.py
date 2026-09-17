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
                     for the whole tab visit.
  3. no_blink    -- two frames captured back-to-back inside the same data
                     window differ by < ui_live_smoke_lib.BLINK_THRESHOLD
                     (0.1%) of pixels; the AAS legend keeps each event name's
                     colour stable across ticks.
  4. no_leak     -- chart/uplot/pending-request counts (the chaos suite's
                     probe, test_web_ui_chaos.py's _LEAK_PROBE) are stable
                     across the visit.

Fail-safe: if the UI shows no data within FIRST_DATA_TIMEOUT_S (60s), the tab
FAILS loudly -- never a skip (mirrors run_all.sh --require-live).

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
    "timeline": """() => {
        const el = document.getElementById('timeline-chart');
        if (!el) return {ok:false, detail:'no container'};
        const c = echarts.getInstanceByDom(el);
        if (!c) return {ok:false, detail:'no echarts instance'};
        const s0 = c.getOption().series && c.getOption().series[0];
        if (!s0) return {ok:false, detail:'no series'};
        if (s0.type !== 'custom') return {ok:false, detail:'not custom: ' + s0.type};
        if (!s0.data || s0.data.length === 0) return {ok:false, detail:'no spans'};
        return {ok:true, detail: s0.data.length + ' spans'};
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
        print(f"mock_server failed to start:\n{out.decode()}\n{err.decode()}",
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
    """Re-queries the panel element fresh and screenshots it, returning None
    (never raising) if the element is missing or gets detached from the DOM
    mid-call -- observed for real (Waterfall/Matrix/Scatter re-mounting their
    host div when the underlying execution/transition identity rotates under
    continuous real load); the caller treats None as maximal instability,
    not a skip."""
    panel = page.query_selector(_PANEL_ELEMENT_SELECTOR[tab_id])
    if panel is None:
        return None
    try:
        return panel.screenshot()
    except PWError:
        return None


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
    TICK_TIMEOUT_S already, not a separate ad hoc allowance."""
    deadline = time.monotonic() + timeout_s
    probe = page.evaluate(LEAK_PROBE_JS)
    while probe.get("pending", 0) != 0 and time.monotonic() < deadline:
        page.wait_for_timeout(interval_ms)
        probe = page.evaluate(LEAK_PROBE_JS)
    return probe


# ── one tab's walk ───────────────────────────────────────────────────────────

def run_tab(browser, tab_id, url, out_dir, ticks, first_data_timeout,
            blink_threshold):
    tab_dir = os.path.join(out_dir, tab_id)
    os.makedirs(tab_dir, exist_ok=True)
    context = browser.new_context(
        viewport={"width": 1280, "height": 900},
        record_video_dir=tab_dir,
        record_video_size={"width": 1280, "height": 900})
    context.add_init_script(UNHANDLED_REJECTION_HOOK)
    page = context.new_page()
    guard = ConsoleErrorGuard(page)
    console_errors = []
    tick_paths = []
    video_path = None
    try:
        page.goto(url)
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

        leak_before = _settled_leak_probe(page)

        legend_ticks = []
        blink_ratios = []
        render_ok, render_detail = False, "never checked"
        for i in range(1, ticks + 1):
            if not _wait_for_tick(page, i, TICK_TIMEOUT_S):
                raise SmokeFailure(
                    f"tick {i}/{ticks} did not arrive within {TICK_TIMEOUT_S}s")

            render_ok, render_detail = _poll_render_check(page, tab_id)
            if not render_ok:
                raise SmokeFailure(f"tick {i}: {render_detail}")

            # Settle past ECharts' default ~1000ms setOption() transition
            # before measuring "the same data window": AAS explicitly turned
            # this off (docs/VISUAL_CHECKLIST.md's U0 CONTINUITY fix,
            # "no replayed draw-in on refresh"), but several other builders
            # (observed: Concurrency/Timeline/Scatter/Matrix) still animate
            # on every live-tick setOption(), and catching that mid-transition
            # is measuring the WRONG window, not a real per-tick instability.
            # 1200ms is comfortably past ECharts' default animationDuration.
            page.wait_for_timeout(1200)

            frame_a = _safe_panel_screenshot(page, tab_id)
            page.wait_for_timeout(120)  # same data window, before the next tick
            frame_b = _safe_panel_screenshot(page, tab_id)
            if frame_a is None or frame_b is None:
                # A real find (heavy real load, not the mock): the panel's
                # host DOM node itself can be torn down and rebuilt (not
                # merely resized -- see blink_check's shape-mismatch case)
                # between two "steady state" frames, e.g. Waterfall/Matrix
                # re-mounting when the underlying execution/transition
                # identity rotates under continuous pgbench traffic. That
                # IS instability -- report the worst possible ratio, never
                # crash on Playwright's "Element is not attached" error.
                ratio, blink_note = 1.0, "panel element detached from the DOM between frames"
            else:
                ratio, blink_note = lib.blink_check(
                    lib.png_bytes_to_array(frame_a), lib.png_bytes_to_array(frame_b))
            if blink_note:
                print(f"  note [{tab_id}] tick {i}: {blink_note}")
            blink_ratios.append(ratio)

            if frame_a is not None:
                tick_path = os.path.join(tab_dir, f"tick-{i}.png")
                with open(tick_path, "wb") as f:
                    f.write(frame_a)
                tick_paths.append(tick_path)

            legend = page.evaluate(LEGEND_COLORS_JS)
            if legend:
                legend_ticks.append(legend)

            console_errors.extend(guard.drain())

        leak_after = _settled_leak_probe(page)
        color_violations = (lib.color_stability_violations(legend_ticks)
                            if legend_ticks else [])
        worst_blink = max(blink_ratios) if blink_ratios else None
        unexpected_errors = [e for e in console_errors if not _is_pgwt_error(e)]

        result = lib.build_tab_result(
            tab_id, render_ok, render_detail, ticks, unexpected_errors,
            worst_blink, color_violations, leak_before, leak_after,
            artifacts={}, blink_threshold=blink_threshold)
    except SmokeFailure as e:
        print(f"  FAIL [{tab_id}]: {e}", file=sys.stderr)
        result = lib.build_failed_tab_result(tab_id, str(e),
                                             ticks_observed=len(tick_paths))
    except Exception:
        traceback.print_exc()
        result = lib.build_failed_tab_result(
            tab_id, "unhandled exception (see stderr traceback)",
            ticks_observed=len(tick_paths))
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
    ap.add_argument("--out-dir", default=RESULTS_DIR,
                    help=f"artifact directory (default {RESULTS_DIR})")
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
    args = ap.parse_args()

    if bool(args.url) == bool(args.mock):
        ap.error("exactly one of --url or --mock is required")

    mock_proc = None
    if args.mock:
        mock_proc = start_mock_server(MOCK_PORT)
        url = mock_app_url(MOCK_PORT)
    else:
        url = args.url

    os.makedirs(args.out_dir, exist_ok=True)
    results = []
    try:
        with sync_playwright() as p:
            browser = p.chromium.launch()
            try:
                for tab_id in lib.TABS:
                    print(f"=== {tab_id} ===")
                    result = run_tab(browser, tab_id, url, args.out_dir,
                                     args.ticks, args.first_data_timeout,
                                     args.blink_threshold)
                    results.append(result)
                    status = "PASS" if result["ok"] else "FAIL"
                    print(f"  {status} [{tab_id}] "
                          f"ticks={result['ticks_observed']} "
                          f"rendered={result['rendered']}")
            finally:
                browser.close()
    finally:
        if mock_proc is not None:
            stop_mock_server(mock_proc)

    summary_path = os.path.join(args.out_dir, "summary.json")
    summary = lib.write_summary(summary_path, results)

    print()
    print("════════════════════════════════════════")
    print("  UI LIVE SMOKE SUMMARY")
    print("════════════════════════════════════════")
    for tab_id in lib.TABS:
        r = summary["tabs"].get(tab_id)
        print(f"  {'PASS' if r and r['ok'] else 'FAIL':4s} {tab_id}")
    print(f"  summary: {summary_path}")
    print(f"  overall: {'PASS' if summary['ok'] else 'FAIL'}"
          + (f" (failed: {', '.join(summary['failed_tabs'])})"
             if summary["failed_tabs"] else ""))

    return 0 if summary["ok"] else 1


if __name__ == "__main__":
    sys.exit(main())
