#!/usr/bin/env python3
"""test_web_ui_chaos.py -- Adversarial / chaos UI tests (Phase B2).

The web UI's instability is async races that NEVER fire against the
zero-latency mock (tests/mock_server.py) — which is why CI is green while
manual testing keeps finding bugs. This suite runs the mock in CHAOS mode
(latency jitter, out-of-order delivery, late-after-navigation responses,
see mock_server.py) and replicates the manual-testing actions that surface
those races:

  - rapid tab switching (switch faster than responses arrive)
  - clicking a drill-down row before its data has loaded
  - toggling Live mode on/off mid-refresh
  - drag-zoom on the AAS chart during an in-flight refresh

Every test asserts ZERO console errors / pageerrors / unhandled rejections
(reusing B1's console guard) AND a correct, self-consistent FINAL state.

══════════════════════════════════════════════════════════════════════════
XFAIL infrastructure + the B3 acceptance list
══════════════════════════════════════════════════════════════════════════
Phase B2's premise is that these tests expose races that the Phase B3
restructure (view-manager response chokepoint + transport single-flight)
will fix. To keep CI green regardless of outcome, every chaos test is
classified at registration time as either:

  GATING  — must pass; a failure fails the suite (CI red).
  XFAIL   — a known B3 target: expected to fail today; its failure is
            reported as "XFAIL (B3 will fix)" and does NOT fail the suite.
            An XFAIL test that unexpectedly PASSES is reported as XPASS
            (surfaced loudly, non-fatal) so it can be promoted to GATING.

The XFAIL machinery is retained as required B2 infrastructure so B3 (or any
future view) can register a genuinely-failing race here without turning CI
red. It also self-corrects: an XPASS print is the signal to promote.

FINDING (2026-06-14, this PR): against the CURRENT app.js, all four
race-exposing tests PASS under genuine chaos (verified out-of-order + late
delivery + a 40-action soak — zero console errors). app.js's global
generation counter (_refreshGen, bumped on every user-initiated view change
and re-checked before every heavy renderer dispatches) correctly supersedes
stale in-flight responses at the OBSERVABLE level. So these are registered
as GATING: they lock in that behavior as a regression guard B3 must keep
green while it replaces the ad-hoc generation counters with the structural
view-manager/transport chokepoint. The B3 acceptance list is therefore
"these four chaos tests stay green through the restructure."

The documented races (symptom → root cause → what B3 changes) live next to
each test below.

Usage: python3 tests/test_web_ui_chaos.py
  (No root, no PG, no SSH needed — uses mock_server.py with PGWT_CHAOS=1)
"""
import collections
import os
import re
import socket
import sys
import subprocess
import time
import signal

try:
    from playwright.sync_api import sync_playwright
except ImportError:
    # Match test_web_ui.py: a skip locally, a hard failure in CI.
    if os.environ.get("CI"):
        print("ERROR: playwright not installed — required in CI", file=sys.stderr)
        sys.exit(1)
    print("SKIP: pip install playwright && playwright install chromium",
          file=sys.stderr)
    sys.exit(0)

# Reuse a distinct port so this can run alongside test_web_ui.py.
MOCK_PORT = int(os.environ.get("PGWT_TEST_PORT", "18811"))
MOCK_SCRIPT = os.path.join(os.path.dirname(os.path.abspath(__file__)), "mock_server.py")


def app_url(http_port):
    """Page URL pointing the client's WebSocket at the mock's WS port (+1).
    The client honors `?ws=<full ws url>` verbatim (app.js connect(), U0) —
    no WebSocket monkey-patch needed."""
    return f"http://127.0.0.1:{http_port}/?ws=ws://127.0.0.1:{http_port + 1}/ws"


MOCK_URL = app_url(MOCK_PORT)

# ── Result accounting ─────────────────────────────────────────────────────────
# Gating tests fail the suite. XFAIL tests (the B3 targets) report their
# status but never fail the suite, so CI stays green.

gating_checks = []   # (ok, msg)
xfail_results = {}    # test_name -> {"failed": bool, "msgs": [...]}

_current_xfail = None  # set while an xfail test runs

# ── Failure artifacts (Track U U0 suite hygiene) ──────────────────────────────
# On any GATING check() failure: a full-page screenshot + the recent console
# tail land here (CI uploads the directory as the `web-ui-failures` artifact).
# Shared with test_web_ui.py — file names are prefixed per suite.
FAIL_DIR = os.path.join(os.path.dirname(os.path.abspath(__file__)),
                        "results", "web_ui_failures")

_capture = {"page": None, "guard": None, "n": 0}


def set_failure_capture(page, guard):
    """Point check()'s failure capture at the currently driven page."""
    _capture["page"], _capture["guard"] = page, guard


def _capture_failure(msg):
    page, guard = _capture["page"], _capture["guard"]
    if page is None:
        return
    _capture["n"] += 1
    safe = re.sub(r"[^A-Za-z0-9._-]+", "_", msg)[:60]
    base = os.path.join(FAIL_DIR, f"chaos_{_capture['n']:02d}_{safe}")
    try:
        os.makedirs(FAIL_DIR, exist_ok=True)
        page.screenshot(path=base + ".png", full_page=True)
        with open(base + "-console.txt", "w") as f:
            f.write("\n".join(guard.tail if guard else ()) + "\n")
        print(f"    (failure artifacts: {base}.png / -console.txt)")
    except Exception as e:
        print(f"    (failure-artifact capture failed: {e})")


def check(cond, msg):
    """Record a check. Inside an xfail test it feeds that test's tally;
    otherwise it is a gating check that can fail the suite."""
    if _current_xfail is not None:
        rec = xfail_results[_current_xfail]
        if not cond:
            rec["failed"] = True
            rec["msgs"].append(msg)
            print(f"  xfail-check FAIL: {msg}")
        else:
            print(f"  xfail-check ok:   {msg}")
    else:
        gating_checks.append((cond, msg))
        print(f"  {'PASS' if cond else 'FAIL'}: {msg}")
        if not cond:
            _capture_failure(msg)


# ── Console error guard (Phase B1, reused) ────────────────────────────────────

UNHANDLED_REJECTION_HOOK = """
window.addEventListener('unhandledrejection', e => {
    const r = e.reason;
    console.error('Unhandled rejection: ' +
        ((r && (r.stack || r.message)) || String(r)));
});
"""


class ConsoleErrorGuard:
    def __init__(self, page):
        self.errors = []
        # Rolling tail of ALL console traffic, for check()'s failure artifacts.
        self.tail = collections.deque(maxlen=50)
        page.on("console", self._on_console)
        page.on("pageerror", self._on_pageerror)

    def _on_console(self, msg):
        self.tail.append(f"[{msg.type}] {msg.text}")
        if msg.type == "error":
            self.errors.append(f"console: {msg.text}")

    def _on_pageerror(self, err):
        self.tail.append(f"[pageerror] {err}")
        self.errors.append(f"pageerror: {err}")

    def drain(self):
        errors = self.errors
        self.errors = []
        return errors


def _is_pgwt_error(e):
    """U0/P1: console.error('[pgwt] ...') is the app REPORTING a failure —
    the expected error surface, never a crash. Recorded, not fatal. A
    pageerror can never qualify."""
    return e.startswith("console: [pgwt]")


def assert_no_console_errors(page, guard, name, allow=()):
    page.wait_for_timeout(1200)  # chaos delays are long; let late errors land
    errors = guard.drain()
    for e in errors:
        if _is_pgwt_error(e):
            print(f"  note: [pgwt] error surface: {e[:100]}")
    unexpected = [e for e in errors
                  if not _is_pgwt_error(e) and not any(p in e for p in allow)]
    check(len(unexpected) == 0,
          f"[{name}] no console errors "
          f"(got {len(unexpected)}: {unexpected[:3]})")


# ── Mock server (chaos mode) ──────────────────────────────────────────────────

def _wait_ports(proc, ports, timeout=10.0):
    """Poll until every port accepts a TCP connection or the process dies.
    Replaces the old blind 1.5 s startup sleep (both slow and racy)."""
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
    # A stale FOREIGN listener on one of the ports can answer the probe while
    # OUR process dies on the bind conflict moments later — give it a beat and
    # confirm it survived, so the conflict is reported here, not as a baffling
    # connection-refused mid-suite.
    time.sleep(0.2)
    return proc.poll() is None


def start_mock_server():
    env = dict(os.environ)
    env["PGWT_CHAOS"] = "1"  # belt-and-suspenders alongside --chaos
    proc = subprocess.Popen(
        [sys.executable, MOCK_SCRIPT, "--port", str(MOCK_PORT), "--chaos"],
        stdout=subprocess.PIPE, stderr=subprocess.PIPE, env=env,
    )
    # Wait until both listeners (HTTP :port, WS :port+1) accept connections.
    if not _wait_ports(proc, (MOCK_PORT, MOCK_PORT + 1)):
        if proc.poll() is None:
            proc.kill()
        out, err = proc.communicate()
        print(f"mock_server failed to start:\n{out.decode()}\n{err.decode()}")
        sys.exit(1)
    return proc


def stop_mock_server(proc):
    proc.send_signal(signal.SIGTERM)
    try:
        proc.wait(timeout=5)
    except subprocess.TimeoutExpired:
        proc.kill()
        proc.wait()


def goto_ready(page):
    """Load the page and wait until the first data is on screen."""
    page.goto(MOCK_URL)
    page.wait_for_selector("#status.connected", timeout=15000)
    # Under chaos the first table/chart can take a while.
    page.wait_for_selector("#table-container table tbody tr, .loading",
                           timeout=15000)


def active_tab(page):
    el = page.query_selector(".tab.active")
    return el.text_content() if el else None


# ── Tests ─────────────────────────────────────────────────────────────────────
# Each test fixes the FINAL action, lets chaos settle, then asserts the UI
# reflects that final action — not a stale in-flight response.

def test_rapid_tab_switching(page):
    """RACE: stale tab response overwrites the current tab's content.

    Symptom (manual): switch rapidly *through* the heavy analysis tabs
    (Histogram / Timeline / Transitions) and land on Events faster than the
    chaos-delayed responses arrive. A late response for an earlier tab paints
    its own widget into #table-container while Events is the active tab — e.g.
    a heatmap container or a "Select a session" timeline prompt shows up under
    the Events tab header, or the Events table never appears.

    Root cause: refreshTable()'s plain-table branch checks `_refreshGen`, but
    the histogram/timeline/transitions/concurrency branches (refreshHistogram,
    refreshTimeline, refreshTransitions, refreshConcurrency) write into
    #table-container with NO generation re-check after their awaits — so a
    superseded heavy-tab response still renders over the active tab. B3's
    view-manager makes "drop any response addressed to a non-active view"
    structural.
    """
    goto_ready(page)
    # Stop live so auto-refresh ticks don't muddy the final-state assertion.
    page.click("#live-btn")
    page.wait_for_timeout(200)

    # Go through the heavy tabs (each kicks an async render into the shared
    # #table-container), then land on Events — all faster than chaos latency.
    order = ["histogram", "timeline", "transitions", "events"]
    for tab in order:
        page.click(f".tab[data-tab='{tab}']")
        page.wait_for_timeout(50)  # faster than the 50-300ms chaos latency

    # Let every in-flight (and late) response drain.
    page.wait_for_timeout(4000)

    check(active_tab(page) == "Events",
          f"final active tab is Events (got {active_tab(page)})")

    # Under the race a stale heavy-tab renderer wins #table-container.
    # The Events tab must own the container exclusively: its table present,
    # and none of the other tabs' signature widgets leaking in.
    headers = [th.text_content().strip() for th in
               page.query_selector_all("#table-container table thead th")]
    check("Wait Event" in headers,
          f"final container holds the Events table (headers={headers[:4]})")

    leaked = page.evaluate("""() => {
        const c = document.getElementById('table-container');
        const t = c ? c.textContent : '';
        return {
            heatmap: !!(c && c.querySelector('#heatmap-container')),
            timelinePrompt: t.includes('Select a session'),
            dfg: !!(c && c.querySelector('#dfg-container')),
        };
    }""")
    check(not leaked["heatmap"],
          "no stale heatmap container leaked under Events tab")
    check(not leaked["timelinePrompt"],
          "no stale timeline prompt leaked under Events tab")
    check(not leaked["dfg"],
          "no stale transitions DFG leaked under Events tab")


def test_click_drilldown_before_load(page):
    """RACE: a second drill-down click before the first one's data loaded.

    Symptom (manual): in Sessions, click a row to drill into Timeline for PID
    A; the chaos-delayed session_timeline response for A is still in flight.
    Before it lands, drill back and click PID B. The late timeline render for
    A then paints over PID B's timeline — the chart shows the wrong session,
    while the breadcrumb says PID B.

    Root cause: refreshTimeline() awaits session_timeline then calls
    renderTimeline() with NO generation re-check, so a superseded drill's
    response still renders. drillDown() bumps the generation for the table
    path but the timeline branch ignores it. B3's single-flight transport +
    view-manager supersede the pending request on the second drill.
    """
    goto_ready(page)
    page.click("#live-btn")
    page.wait_for_timeout(200)

    page.click(".tab[data-tab='sessions']")
    page.wait_for_selector("#table-container table tbody tr.clickable",
                           timeout=15000)
    page.wait_for_timeout(2500)  # let sessions settle so rows are stable

    rows = page.query_selector_all("#table-container table tbody tr.clickable")
    check(len(rows) >= 2, f"have >=2 session rows to drill ({len(rows)})")
    pid_a = page.text_content(
        "#table-container table tbody tr:nth-child(1) td:first-child").strip()
    pid_b = page.text_content(
        "#table-container table tbody tr:nth-child(2) td:first-child").strip()

    # Drill into PID A, then — before its timeline can load — go back and
    # drill into PID B.
    rows[0].click()                       # -> Timeline, pid A in flight
    page.wait_for_timeout(40)             # < chaos latency: A not back yet
    page.click(".tab[data-tab='sessions']")
    page.wait_for_selector("#table-container table tbody tr.clickable",
                           timeout=15000)
    rows = page.query_selector_all("#table-container table tbody tr.clickable")
    rows[1].click()                       # -> Timeline, pid B

    page.wait_for_timeout(4000)           # drain the late PID-A response

    check(active_tab(page) == "Timeline",
          f"drill lands on Timeline (got {active_tab(page)})")

    breadcrumb = page.text_content("#breadcrumb") or ""
    check(pid_b in breadcrumb,
          f"breadcrumb shows the final drill (PID {pid_b}); got "
          f"'{breadcrumb.strip()[:50]}'")

    # The breadcrumb (final drill = PID B) must agree with the timeline the
    # chart actually rendered. The mock reflects the requested pid into the
    # response's `pids` (→ y-axis category), so a stale PID-A render is visible
    # as a "PID A" axis label under a "PID B" breadcrumb. That is the race.
    axis_pids = page.evaluate("""() => {
        const el = document.getElementById('timeline-chart');
        if (!el || typeof echarts === 'undefined') return null;
        const c = echarts.getInstanceByDom(el);
        if (!c) return null;
        const opt = c.getOption();
        const yAxis = opt && opt.yAxis && opt.yAxis[0];
        return (yAxis && yAxis.data) ? yAxis.data : null;
    }""")
    check(axis_pids is not None and any(pid_b in s for s in axis_pids),
          f"timeline chart renders the final drill PID {pid_b} "
          f"(y-axis={axis_pids})")
    check(axis_pids is not None and not any(pid_a in s for s in axis_pids),
          f"timeline chart does NOT show the stale PID {pid_a} "
          f"(y-axis={axis_pids})")


def test_toggle_live_mid_refresh(page):
    """RACE: Live toggled off mid-refresh, then a stale tick moves the window.

    Symptom (manual): zoom into a specific historical window, toggle Live on
    (auto-refresh starts) then immediately off, then zoom into another window.
    A stale auto-refresh tick from the just-stopped loop — whose refresh() is
    NOT user-initiated and whose response can still be in flight — repaints
    the chart and, worse, the loop has already overwritten state.viewFrom/To
    with the live "last N min" window, so the user's chosen window jumps.

    Root cause: stopAutoRefresh() invalidates the loop by id, but a tick that
    has already passed its id-check and is awaiting send('info')/refresh() will
    still run to completion; refresh() inside the loop is not user-initiated so
    it does not bump the generation and is not discarded. The final view
    window must be the user's last explicit zoom, not the live window.
    """
    goto_ready(page)
    # Start from a stopped, explicit custom window so "live" vs "frozen" is
    # unambiguous.
    page.click("#live-btn")          # stop the default live loop
    page.wait_for_timeout(200)

    # Pick a fixed historical window via the time picker "All" range.
    page.click("#time-range")
    page.wait_for_timeout(150)
    page.click(".tp-quick button[data-range='0']")  # All -> frozen
    page.wait_for_timeout(2500)

    target = page.evaluate("() => [window.__pgwt.timeRange.from, window.__pgwt.timeRange.to]")

    # Toggle Live on then off rapidly so an auto-refresh tick is mid-flight
    # when we stop, then re-assert the frozen window.
    page.click("#live-btn")          # live ON  (loop starts, ticks every 5s
                                     #           but its first refresh runs now)
    page.wait_for_timeout(80)        # < chaos latency: refresh in flight
    page.click("#live-btn")          # live OFF (mid-flight)
    page.wait_for_timeout(150)
    # Reassert the user's frozen window explicitly.
    page.click("#time-range")
    page.wait_for_timeout(150)
    page.click(".tp-quick button[data-range='0']")
    page.wait_for_timeout(4000)      # let any stale tick land

    is_active = page.evaluate(
        "document.getElementById('live-btn').classList.contains('active')")
    check(is_active is False,
          f"Live button is off after final toggle (active={is_active})")

    label = page.text_content("#live-btn") or ""
    check(("●" in label) == bool(is_active),
          f"Live label/class consistent (label='{label}', active={is_active})")

    final = page.evaluate("() => [window.__pgwt.timeRange.from, window.__pgwt.timeRange.to]")
    # A stale live tick would shove the window to (serverNow - N, serverNow).
    # The frozen "All" window must survive.
    check(final == target,
          f"view window stayed frozen (target={target}, final={final})")


def test_dragzoom_during_refresh(page):
    """RACE: drag-zoom on the AAS chart while a refresh is in flight.

    Symptom (manual): start a drag-select zoom on the AAS chart while a
    chaos-delayed aas response is still pending. The pending response repaints
    the chart out from under the active selection, the zoom maps to the wrong
    time range, or the chart instance is mutated mid-render.

    B3: the AAS chart is now an ECharts instance owned by the "active" view
    (created in enter, disposed in leave — no module-level chart global), and
    drag-select is a custom overlay (lib/selection.js: pointer events + a band
    div + convertFromPixel). The aas fetch is single-flight on the active.aas
    transport channel, so a new refresh supersedes the pending one structurally.
    """
    goto_ready(page)
    page.click("#live-btn")
    page.wait_for_timeout(200)

    # Kick a refresh (zoom out) so an aas request is in flight, then
    # immediately drag-select across the chart via the custom overlay.
    box = page.query_selector("#aas-chart-container").bounding_box()
    page.click("#zoom-out-btn")
    page.wait_for_timeout(40)  # response not back yet (chaos >= 50ms)

    y = box["y"] + box["height"] / 2
    x1 = box["x"] + box["width"] * 0.30
    x2 = box["x"] + box["width"] * 0.70
    page.mouse.move(x1, y)
    page.mouse.down()
    page.mouse.move(x1 + 20, y)
    page.mouse.move(x2, y)
    page.mouse.up()

    page.wait_for_timeout(3500)  # let zoom + late responses settle

    # The chart must still be a live, rendered ECharts instance with series.
    canvas = page.query_selector("#aas-chart-container canvas")
    check(canvas is not None, "AAS chart canvas still present after drag-zoom")
    has_series = page.evaluate("""() => {
        const el = document.getElementById('aas-chart-container');
        if (!el || typeof echarts === 'undefined') return false;
        const c = echarts.getInstanceByDom(el);
        if (!c) return false;
        const opt = c.getOption();
        return !!(opt.series && opt.series.some(s => s.data && s.data.length));
    }""")
    check(has_series is True,
          "AAS chart still has ECharts series with data after drag-zoom")

    # Time range and view window must be self-consistent (from < to). State is
    # now the explicit TimeRange module, exposed for tests as window.__pgwt.
    consistent = page.evaluate(
        "() => !!(window.__pgwt) && window.__pgwt.timeRange.from < window.__pgwt.timeRange.to")
    check(consistent is True,
          "view window self-consistent (from < to) after drag-zoom")


# ── Soak test (B3 acceptance) ───────────────────────────────────────────────────
# ~50 random tab/filter transitions under chaos. Asserts no console errors AND
# that resources do not leak across the churn: the live ECharts-instance count
# settles to a small bound (the persistent AAS chart + at most the one active
# tab's chart), and nothing is left pending in the transport. This is the B3
# "stable listener/handler counts (no leak)" acceptance item — the restructure's
# enter()/leave() chart ownership must dispose every per-tab chart it created.

# Count of live ECharts instances + pending transport requests. Each per-tab
# chart view disposes its instance in leave(); a leak would show as a growing
# instance count after many switches. We scan every element ECharts could have
# attached to (it stamps a private id on the host node).
_LEAK_PROBE = """() => {
    let charts = 0;
    if (typeof echarts !== 'undefined') {
        for (const el of document.querySelectorAll('*')) {
            try { if (echarts.getInstanceByDom(el)) charts++; } catch (e) {}
        }
    }
    const t = window.__pgwt && window.__pgwt.transport;
    const pending = t ? Object.keys(t.pending).length : -1;
    return { charts, pending };
}"""

# A pseudo-random but deterministic action stream (no wall-clock RNG) so the
# soak is reproducible. Mixes tab switches across ALL tabs (including the heavy
# chart tabs migrated in B3 p3), row-drills, and breadcrumb clears.
_TABS = ["overview", "events", "sessions", "queries",
         "histogram", "timeline", "transitions", "concurrency"]


def test_soak_random_navigation(page):
    """SOAK: 50 random tab/filter transitions, assert no leak + no errors."""
    goto_ready(page)
    page.click("#live-btn")          # stop auto-refresh so the churn is the only driver
    page.wait_for_timeout(200)

    # Deterministic LCG over the action space.
    seed = 12345
    def nxt():
        nonlocal seed
        seed = (seed * 1103515245 + 12345) & 0x7fffffff
        return seed

    N = 50
    for i in range(N):
        r = nxt() % 10
        if r < 6:
            # Tab switch (covers all eight tabs).
            tab = _TABS[nxt() % len(_TABS)]
            page.click(f".tab[data-tab='{tab}']")
        elif r < 8:
            # Drill into the first clickable row, if any (events/sessions/queries).
            row = page.query_selector(
                "#table-container table tbody tr.clickable")
            if row:
                try:
                    row.click()
                except Exception:
                    pass
        else:
            # Clear filters via the breadcrumb ✕, if present.
            clr = page.query_selector("#breadcrumb .crumb-clear")
            if clr:
                try:
                    clr.click()
                except Exception:
                    pass
        # Switch faster than the 50-300ms chaos latency for the first half, then
        # let things settle for the second half so late responses land mid-churn.
        page.wait_for_timeout(40 if i < N // 2 else 120)

    # Land deterministically on a table tab and let every in-flight + late
    # response drain.
    page.click(".tab[data-tab='overview']")
    page.wait_for_timeout(5000)

    check(active_tab(page) == "Overview",
          f"soak ends on Overview (got {active_tab(page)})")

    probe = page.evaluate(_LEAK_PROBE)
    # On a no-chart table tab, only the persistent AAS chart should remain.
    # Allow a small slack (>=1, <=2) to absorb a just-disposed/just-created
    # transient, but a leak (one per switch) would push this to dozens.
    check(probe["charts"] <= 2,
          f"no ECharts instance leak after {N} transitions "
          f"(live instances={probe['charts']}, expected ~1 AAS)")
    check(probe["pending"] == 0,
          f"no transport requests left pending after soak "
          f"(pending={probe['pending']})")

    # Re-run the probe after a second settle window to confirm it's stable, not
    # mid-transition.
    page.wait_for_timeout(1500)
    probe2 = page.evaluate(_LEAK_PROBE)
    check(probe2["charts"] <= 2 and probe2["pending"] == 0,
          f"resource counts stable on re-probe (charts={probe2['charts']}, "
          f"pending={probe2['pending']})")


# ── Registry ──────────────────────────────────────────────────────────────────
# Tests are registered as either gating (must pass) or xfail (B3 targets:
# expected to fail today, reported but non-fatal). If an xfail test starts
# passing, it is reported as XPASS so it can be promoted to gating.

# All four pass against current app.js (see FINDING in the module docstring),
# so they are GATING regression guards. If B3's restructure regresses any,
# move it to XFAIL_TESTS with a note rather than weakening the assertion.
GATING_TESTS = [
    test_rapid_tab_switching,
    test_click_drilldown_before_load,
    test_toggle_live_mid_refresh,
    test_dragzoom_during_refresh,
    test_soak_random_navigation,
]

# B3 acceptance list: race-exposing tests that fail against current code.
# Currently empty — every chaos test above passes today. Retained (with the
# XFAIL machinery in main()) so a genuinely-failing race can be registered
# here without turning CI red.
XFAIL_TESTS = [
    # (test_fn, "symptom — root cause (B3 will fix)"),
]


# ── Main ──────────────────────────────────────────────────────────────────────

def main():
    global _current_xfail
    print("=== test_web_ui_chaos (Phase B2 — chaos mode) ===")

    mock_proc = start_mock_server()
    xpass = []  # xfail tests that unexpectedly passed (promote to gating)

    try:
        with sync_playwright() as p:
            browser = p.chromium.launch(headless=True)
            context = browser.new_context(
                viewport={"width": 1280, "height": 900})

            # MOCK_URL's ?ws= override (see app_url) points the client's
            # WebSocket at the mock's WS port — no monkey-patch.
            context.add_init_script(UNHANDLED_REJECTION_HOOK)

            page = context.new_page()
            guard = ConsoleErrorGuard(page)
            set_failure_capture(page, guard)

            # Gating chaos tests (none yet — kept for promoted xpasses).
            for fn in GATING_TESTS:
                print(f"\n--- [gating] {fn.__name__} ---")
                fn(page)
                assert_no_console_errors(page, guard, fn.__name__)

            # XFAIL chaos tests (the B3 acceptance list).
            for fn, desc in XFAIL_TESTS:
                name = fn.__name__
                print(f"\n--- [xfail] {name} — {desc} ---")
                xfail_results[name] = {"failed": False, "msgs": []}
                _current_xfail = name
                try:
                    fn(page)
                    assert_no_console_errors(page, guard, name)
                except Exception as e:
                    xfail_results[name]["failed"] = True
                    xfail_results[name]["msgs"].append(f"exception: {e}")
                    print(f"  xfail-check FAIL: exception: {e}")
                finally:
                    _current_xfail = None
                rec = xfail_results[name]
                if rec["failed"]:
                    print(f"  -> XFAIL (expected, B3 will fix): {desc}")
                else:
                    print(f"  -> XPASS (unexpected pass — promote to gating!)")
                    xpass.append(name)

            browser.close()
    finally:
        stop_mock_server(mock_proc)

    # ── Summary ──────────────────────────────────────────────────────────────
    print("\n" + "=" * 60)
    print("  CHAOS SUITE SUMMARY")
    print("=" * 60)

    gating_failed = [m for ok, m in gating_checks if not ok]
    gating_passed = len(gating_checks) - len(gating_failed)
    print(f"  Gating checks: {gating_passed}/{len(gating_checks)} passed")
    for m in gating_failed:
        print(f"    GATING FAIL: {m}")

    xfailed = [n for n, r in xfail_results.items() if r["failed"]]
    print(f"\n  XFAIL (B3 acceptance list) — {len(xfailed)} failing as expected:")
    for fn, desc in XFAIL_TESTS:
        n = fn.__name__
        status = "XFAIL" if xfail_results[n]["failed"] else "XPASS"
        print(f"    [{status}] {n}: {desc}")

    if xpass:
        print(f"\n  NOTE: {len(xpass)} xfail test(s) PASSED unexpectedly "
              f"(XPASS) — promote to gating:")
        for n in xpass:
            print(f"    {n}")

    # CI stays green: only GATING failures are fatal. XFAIL/XPASS are not.
    print("")
    if gating_failed:
        print("RESULT: FAIL (gating checks failed)")
        sys.exit(1)
    print("RESULT: PASS (xfail races documented for B3; CI green)")
    sys.exit(0)


if __name__ == "__main__":
    main()
