#!/usr/bin/env python3
"""test_web_ui.py -- Playwright test suite for pgwt web UI.

Starts mock_server.py, launches headless Chromium, and exercises:
  1. Page load and WebSocket connection
  2. Tab navigation
  3. Summary bar content
  4. Table rendering and data display
  5. Column sorting
  6. Drill-down flow and breadcrumb navigation
  7. Filter persistence across tabs
  8. Histogram and Timeline tabs
  9. Time picker
 10. Reconnection behavior

Usage: python3 tests/test_web_ui.py
  (No root, no PG, no SSH needed — uses mock_server.py)
"""
import collections
import os
import re
import socket
import sys
import subprocess
import time
import signal

# Playwright import
try:
    from playwright.sync_api import sync_playwright
except ImportError:
    print("ERROR: pip install playwright && playwright install chromium",
          file=sys.stderr)
    sys.exit(1)

MOCK_PORT = int(os.environ.get("PGWT_TEST_PORT", "18799"))
MOCK_SCRIPT = os.path.join(os.path.dirname(os.path.abspath(__file__)), "mock_server.py")


def app_url(http_port):
    """Page URL pointing the client's WebSocket at the mock's WS port.

    The mock serves HTTP on `http_port` and WS on `http_port + 1`; the client
    honors `?ws=<full ws url>` verbatim (app.js connect(), Track U U0), so no
    WebSocket monkey-patch is needed."""
    return f"http://127.0.0.1:{http_port}/?ws=ws://127.0.0.1:{http_port + 1}/ws"


MOCK_URL = app_url(MOCK_PORT)

tests_run = 0
tests_passed = 0
tests_failed = 0

# ── Failure artifacts (Track U U0 suite hygiene) ──────────────────────────────
# On any check() failure: a full-page screenshot + the recent console tail are
# written here (CI uploads the directory as the `web-ui-failures` artifact) so
# a red run is diagnosable post-mortem instead of being a bare FAIL line.
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
    base = os.path.join(FAIL_DIR, f"web_ui_{_capture['n']:02d}_{safe}")
    try:
        os.makedirs(FAIL_DIR, exist_ok=True)
        page.screenshot(path=base + ".png", full_page=True)
        with open(base + "-console.txt", "w") as f:
            f.write("\n".join(guard.tail if guard else ()) + "\n")
        print(f"    (failure artifacts: {base}.png / -console.txt)")
    except Exception as e:
        print(f"    (failure-artifact capture failed: {e})")


def check(cond, msg):
    global tests_run, tests_passed, tests_failed
    tests_run += 1
    if cond:
        tests_passed += 1
        print(f"  PASS: {msg}")
    else:
        tests_failed += 1
        print(f"  FAIL: {msg}")
        _capture_failure(msg)


# ── Console error guard (Phase B1) ───────────────────────────────────────────
# Any browser console error, uncaught page exception, or unhandled promise
# rejection fails the test during which it occurred.  Tests that *expect*
# errors (e.g. the reconnection test kills the server) declare an explicit
# allowlist of substrings — the check is never weakened globally.
#
# U0/P1: the app now reports every previously-swallowed failure itself via
# console.error('[pgwt] ...'). Those are the EXPECTED error surface, not a
# crash: the guard records them (and asserts their PRESENCE where a test
# deliberately provokes a failure, via expect_pgwt) while still failing hard
# on everything else — pageerrors, unhandled rejections, non-[pgwt] console
# errors.

# Injected into every page before app code runs: turns unhandled promise
# rejections into console errors so they are captured uniformly.
UNHANDLED_REJECTION_HOOK = """
window.addEventListener('unhandledrejection', e => {
    const r = e.reason;
    console.error('Unhandled rejection: ' +
        ((r && (r.stack || r.message)) || String(r)));
});
"""


class ConsoleErrorGuard:
    """Collects console errors and page errors emitted by the page."""

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
    """The app's own error reporting: console.error starting with '[pgwt]'.
    A pageerror can never qualify — an uncaught crash is always fatal."""
    return e.startswith("console: [pgwt]")


def assert_no_console_errors(page, guard, name, allow=(), expect_pgwt=False):
    """Fail the named test if it produced any non-allowlisted console error.

    '[pgwt]'-prefixed console errors never fail the guard (they are the app
    REPORTING a failure — the P1 contract); they are printed for the record,
    and with expect_pgwt=True their presence is asserted: a test that provokes
    a failure must see the app report it."""
    page.wait_for_timeout(250)  # let async errors land
    errors = guard.drain()
    pgwt = [e for e in errors if _is_pgwt_error(e)]
    for e in pgwt:
        print(f"  note: [pgwt] error surface: {e[:100]}")
    unexpected = [e for e in errors
                  if not _is_pgwt_error(e) and not any(p in e for p in allow)]
    check(len(unexpected) == 0,
          f"[{name}] no console errors "
          f"(got {len(unexpected)}: {unexpected[:3]})")
    if expect_pgwt:
        check(len(pgwt) > 0,
              f"[{name}] provoked failure reported via '[pgwt]' console.error")


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


def start_mock_server(extra_env=None, port=None):
    """Start mock_server.py as a subprocess.

    extra_env: optional dict of env vars (e.g. PGWT_MOCK_FIDELITY,
    PGWT_MOCK_DAEMON) so a test can launch the mock in a fidelity/daemon mode
    without a new protocol surface.
    port: override the HTTP port (WS = port+1) for a second concurrent mock.
    """
    env = dict(os.environ)
    if extra_env:
        env.update(extra_env)
    http_port = port or MOCK_PORT
    proc = subprocess.Popen(
        [sys.executable, MOCK_SCRIPT, "--port", str(http_port)],
        stdout=subprocess.PIPE, stderr=subprocess.PIPE, env=env,
    )
    # Wait until both listeners (HTTP :port, WS :port+1) accept connections.
    if not _wait_ports(proc, (http_port, http_port + 1)):
        if proc.poll() is None:
            proc.kill()
        out, err = proc.communicate()
        print(f"mock_server failed to start:\n{out.decode()}\n{err.decode()}")
        sys.exit(1)
    return proc


def stop_mock_server(proc):
    """Stop mock_server.py."""
    proc.send_signal(signal.SIGTERM)
    try:
        proc.wait(timeout=5)
    except subprocess.TimeoutExpired:
        proc.kill()
        proc.wait()


# ── Tests ─────────────────────────────────────────────────────────────────────

def test_page_load(page):
    """1. Page loads, WebSocket connects, status shows connected."""
    print("--- Test 1: Page Load ---")

    page.goto(MOCK_URL)
    page.wait_for_selector("#status.connected", timeout=10000)
    page.wait_for_timeout(1000)  # let the first aas response enrich the status

    # Status format: "4 CPUs · 15.0m window · peak 4.5 AAS"
    status = page.text_content("#status")
    check("CPUs" in status and "AAS" in status,
          f"Status shows connection info: '{status}'")

    title = page.title()
    check(title == "pgwt", f"Page title is 'pgwt' (got '{title}')")

    # Header exists
    h1 = page.text_content("h1")
    check(h1 == "pgwt", f"Header is 'pgwt'")


def test_tabs(page):
    """2. All tabs exist and are clickable."""
    print("--- Test 2: Tab Navigation ---")

    tabs = page.query_selector_all(".tab")
    tab_names = [t.text_content() for t in tabs]
    expected = ["Overview", "Events", "Sessions", "Queries", "Histogram", "Timeline", "Transitions", "Concurrency"]
    check(tab_names == expected,
          f"Tabs: {tab_names}")

    # Overview is active by default
    active = page.query_selector(".tab.active")
    check(active and active.text_content() == "Overview",
          "Overview tab active by default")

    # Click Events tab
    page.click(".tab[data-tab='events']")
    page.wait_for_timeout(500)
    active = page.query_selector(".tab.active")
    check(active and active.text_content() == "Events",
          "Events tab becomes active after click")

    # Click Sessions tab
    page.click(".tab[data-tab='sessions']")
    page.wait_for_timeout(500)
    active = page.query_selector(".tab.active")
    check(active and active.text_content() == "Sessions",
          "Sessions tab becomes active after click")

    # Go back to Overview
    page.click(".tab[data-tab='overview']")
    page.wait_for_timeout(500)


def test_summary_bar(page):
    """3. Summary bar shows DB Time, Wall, AAS, Idle, CPUs."""
    print("--- Test 3: Summary Bar ---")

    page.goto(MOCK_URL)
    page.wait_for_selector("#status.connected", timeout=10000)
    page.wait_for_timeout(1000)

    summary = page.text_content("#summary-bar")
    check("DB Time" in summary, f"Summary has 'DB Time'")
    check("AAS" in summary, f"Summary has 'AAS'")
    check("CPUs" in summary, f"Summary has 'CPUs'")

    # Verify specific values from canned data
    metrics = page.query_selector_all(".metric")
    metric_texts = {m.query_selector(".metric-label").text_content():
                    m.query_selector(".metric-value").text_content()
                    for m in metrics}

    check("DB Time" in metric_texts, f"DB Time metric present")
    check("CPUs" in metric_texts and metric_texts["CPUs"] == "4",
          f"CPUs = 4 (got '{metric_texts.get('CPUs', 'N/A')}')")
    check("AAS" in metric_texts and "3.47" in metric_texts["AAS"],
          f"AAS = 3.47 (got '{metric_texts.get('AAS', 'N/A')}')")


def test_overview_table(page):
    """4. Overview table shows time model rows."""
    print("--- Test 4: Overview Table ---")

    page.goto(MOCK_URL)
    page.wait_for_selector("#status.connected", timeout=10000)
    page.wait_for_timeout(1000)

    # Table should have rows
    rows = page.query_selector_all("#table-container table tbody tr")
    check(len(rows) > 0, f"Overview table has {len(rows)} rows")

    # First row should be DB Time
    first_row = page.text_content("#table-container table tbody tr:first-child")
    check("DB Time" in first_row, f"First row is 'DB Time'")

    # Should have CPU* row
    table_text = page.text_content("#table-container")
    check("CPU*" in table_text, "Table has CPU* row")
    check("IO" in table_text, "Table has IO row")
    check("Lock" in table_text, "Table has Lock row")


def test_events_table(page):
    """5. Events tab shows event rows with all columns."""
    print("--- Test 5: Events Table ---")

    page.goto(MOCK_URL)
    page.wait_for_selector("#status.connected", timeout=10000)
    page.click(".tab[data-tab='events']")
    page.wait_for_timeout(1000)

    # Column headers
    headers = [th.text_content().strip() for th in
               page.query_selector_all("#table-container table thead th")]
    check("Wait Event" in headers, f"Has 'Wait Event' column")
    check("Count" in headers, f"Has 'Count' column")
    check("P50" in headers, f"Has 'P50' column")
    check("P95" in headers, f"Has 'P95' column")
    check("AAS" in headers, f"Has 'AAS' column")

    # Data rows
    rows = page.query_selector_all("#table-container table tbody tr")
    check(len(rows) >= 5, f"Events table has {len(rows)} rows (expected >= 5)")

    # First event should be CPU* (highest pct)
    first_row = page.text_content("#table-container table tbody tr:first-child")
    check("CPU*" in first_row, f"Top event is CPU*")


def test_column_sorting(page):
    """6. Click column header to sort."""
    print("--- Test 6: Column Sorting ---")

    page.goto(MOCK_URL)
    page.wait_for_selector("#status.connected", timeout=10000)
    page.click(".tab[data-tab='events']")
    page.wait_for_timeout(1000)

    # Click Count header to sort by count descending
    page.click("th[data-sort='count']")
    page.wait_for_timeout(500)

    # Verify sort indicator
    count_th = page.text_content("th[data-sort='count']")
    check("\u25bc" in count_th or "\u25b2" in count_th,
          f"Sort indicator shown in Count header")

    # Click again to toggle direction
    page.click("th[data-sort='count']")
    page.wait_for_timeout(500)

    count_th = page.text_content("th[data-sort='count']")
    check("\u25b2" in count_th,
          f"Sort direction toggled (ascending)")


def test_drill_down(page):
    """7. Click event row -> drill down to queries, breadcrumb shown."""
    print("--- Test 7: Drill-down Flow ---")

    page.goto(MOCK_URL)
    page.wait_for_selector("#status.connected", timeout=10000)

    # Start at Overview, click IO class
    page.wait_for_timeout(1000)
    # Click on an indent-1 row (clickable wait class)
    io_row = page.query_selector("tr.indent-1.clickable")
    if io_row:
        io_row.click()
        page.wait_for_timeout(1000)

        # Should switch to Events tab
        active = page.query_selector(".tab.active")
        check(active and active.text_content() == "Events",
              "Drill from Overview -> Events tab")

        # Breadcrumb should appear
        breadcrumb = page.text_content("#breadcrumb")
        check(len(breadcrumb.strip()) > 0,
              f"Breadcrumb shown: '{breadcrumb.strip()[:60]}'")

        # Should have a filter indicator
        check("class=" in breadcrumb.lower() or "cpu" in breadcrumb.lower(),
              f"Filter shown in breadcrumb")

        # Click an event row -> drill to Queries
        event_row = page.query_selector("#table-container table tbody tr.clickable")
        if event_row:
            event_row.click()
            page.wait_for_timeout(1000)

            active = page.query_selector(".tab.active")
            check(active and active.text_content() == "Queries",
                  "Drill from Events -> Queries tab")

            # Breadcrumb should have 2 entries now
            crumbs = page.query_selector_all("#breadcrumb .crumb")
            check(len(crumbs) >= 1,
                  f"Breadcrumb has {len(crumbs)} entries after 2 drills")
        else:
            check(False, "No clickable event row found for drill-down")
    else:
        check(False, "No clickable class row found in Overview")
        check(False, "(skipped event drill)")
        check(False, "(skipped breadcrumb check)")


def test_breadcrumb_navigation(page):
    """8. Click breadcrumb to go back."""
    print("--- Test 8: Breadcrumb Navigation ---")

    page.goto(MOCK_URL)
    page.wait_for_selector("#status.connected", timeout=10000)
    page.wait_for_timeout(1000)

    # Drill into a class from Overview
    io_row = page.query_selector("tr.indent-1.clickable")
    if io_row:
        io_row.click()
        page.wait_for_timeout(1000)

        # Should have breadcrumb with clear button
        clear_btn = page.query_selector(".crumb-clear")
        check(clear_btn is not None, "Clear filter button (X) exists")

        if clear_btn:
            clear_btn.click()
            page.wait_for_timeout(1000)

            # Should be back at Overview with no filters
            active = page.query_selector(".tab.active")
            check(active and active.text_content() == "Overview",
                  "Clear filters returns to Overview")

            breadcrumb = page.text_content("#breadcrumb")
            check(breadcrumb.strip() == "",
                  "Breadcrumb cleared after Clear")
    else:
        check(False, "No clickable row for breadcrumb test")
        check(False, "(skipped clear)")


def test_sessions_table(page):
    """9. Sessions tab shows session data."""
    print("--- Test 9: Sessions Table ---")

    page.goto(MOCK_URL)
    page.wait_for_selector("#status.connected", timeout=10000)
    page.click(".tab[data-tab='sessions']")
    page.wait_for_timeout(1000)

    headers = [th.text_content().strip() for th in
               page.query_selector_all("#table-container table thead th")]
    check("PID" in headers, "Sessions has 'PID' column")
    check("DB Time" in headers, "Sessions has 'DB Time' column")
    check("Top Wait" in headers, "Sessions has 'Top Wait' column")

    rows = page.query_selector_all("#table-container table tbody tr")
    check(len(rows) >= 4, f"Sessions has {len(rows)} rows (expected >= 4)")

    # Click a session -> should drill to Timeline
    rows[0].click()
    page.wait_for_timeout(1000)
    active = page.query_selector(".tab.active")
    check(active and active.text_content() == "Timeline",
          "Click session -> Timeline tab")


def test_queries_table(page):
    """10. Queries tab shows query data with stacked bars."""
    print("--- Test 10: Queries Table ---")

    page.goto(MOCK_URL)
    page.wait_for_selector("#status.connected", timeout=10000)
    page.click(".tab[data-tab='queries']")
    page.wait_for_timeout(1000)

    headers = [th.text_content().strip() for th in
               page.query_selector_all("#table-container table thead th")]
    check("Query ID" in headers, "Queries has 'Query ID' column")
    check("Query Text" in headers, "Queries has 'Query Text' column")
    check("Wait Profile" in headers, "Queries has 'Wait Profile' column")

    rows = page.query_selector_all("#table-container table tbody tr")
    check(len(rows) >= 3, f"Queries has {len(rows)} rows (expected >= 3)")

    # Stacked bars should exist
    bars = page.query_selector_all(".stacked-bar")
    check(len(bars) > 0, f"Stacked wait profile bars rendered ({len(bars)})")


def test_histogram_tab(page):
    """11. Histogram tab shows class/event selectors and heatmap."""
    print("--- Test 11: Histogram Tab ---")

    page.goto(MOCK_URL)
    page.wait_for_selector("#status.connected", timeout=10000)
    page.click(".tab[data-tab='histogram']")
    page.wait_for_timeout(1500)

    # Selectors should exist
    class_select = page.query_selector("#hm-class")
    event_select = page.query_selector("#hm-event")
    check(class_select is not None, "Class selector exists")
    check(event_select is not None, "Event selector exists")

    # Heatmap container should exist
    heatmap = page.query_selector("#heatmap-container")
    check(heatmap is not None, "Heatmap container exists")

    # ECharts canvas should be rendered
    canvas = page.query_selector("#heatmap-container canvas")
    check(canvas is not None, "Heatmap canvas rendered")

    # B4: read the ECharts option, not just canvas-exists — assert a heatmap
    # series carrying data cells (catches "renders empty" that a canvas check
    # cannot see).
    hm_status = page.evaluate("""() => {
        const el = document.getElementById('heatmap-container');
        if (!el) return 'no container';
        const c = echarts.getInstanceByDom(el);
        if (!c) return 'no echarts instance';
        const opt = c.getOption();
        const s0 = opt.series && opt.series[0];
        if (!s0) return 'no series';
        if (s0.type !== 'heatmap') return 'not a heatmap: ' + s0.type;
        if (!s0.data || s0.data.length === 0) return 'no data';
        return 'ok:' + s0.data.length;
    }""")
    check(hm_status.startswith("ok"),
          f"Heatmap series has data cells ({hm_status})")


def test_timeline_tab(page):
    """12. Timeline tab requires PID filter, shows events."""
    print("--- Test 12: Timeline Tab ---")

    page.goto(MOCK_URL)
    page.wait_for_selector("#status.connected", timeout=10000)
    page.click(".tab[data-tab='timeline']")
    page.wait_for_timeout(500)

    # Without PID filter, should show prompt
    content = page.text_content("#table-container")
    check("Select a session" in content,
          "Timeline shows 'Select a session' prompt without filter")

    # Drill into a session from Sessions tab
    page.click(".tab[data-tab='sessions']")
    page.wait_for_timeout(1000)
    first_row = page.query_selector("#table-container table tbody tr.clickable")
    if first_row:
        first_row.click()
        page.wait_for_timeout(1500)

        # Should be on Timeline now with chart
        active = page.query_selector(".tab.active")
        check(active and active.text_content() == "Timeline",
              "Drill from Sessions -> Timeline")

        canvas = page.query_selector("#timeline-chart canvas")
        check(canvas is not None, "Timeline chart canvas rendered")

        # B4: read the ECharts option — the timeline is a custom series whose
        # data is one entry per wait span; assert it carries spans.
        tl_status = page.evaluate("""() => {
            const el = document.getElementById('timeline-chart');
            if (!el) return 'no container';
            const c = echarts.getInstanceByDom(el);
            if (!c) return 'no echarts instance';
            const opt = c.getOption();
            const s0 = opt.series && opt.series[0];
            if (!s0) return 'no series';
            if (s0.type !== 'custom') return 'not custom: ' + s0.type;
            if (!s0.data || s0.data.length === 0) return 'no data';
            return 'ok:' + s0.data.length;
        }""")
        check(tl_status.startswith("ok"),
              f"Timeline custom series has spans ({tl_status})")
    else:
        check(False, "No session row to drill into")
        check(False, "(skipped timeline chart)")
        check(False, "(skipped timeline getOption)")


def test_transitions_tab(page):
    """12b. Transitions tab shows the directly-follows graph with data."""
    print("--- Test 12b: Transitions Tab ---")

    page.goto(MOCK_URL)
    page.wait_for_selector("#status.connected", timeout=10000)
    page.click(".tab[data-tab='transitions']")
    page.wait_for_timeout(3000)

    active = page.query_selector(".tab.active")
    check(active and active.text_content() == "Transitions",
          "Transitions tab becomes active")

    # DFG container and simplify slider must exist
    container = page.query_selector("#dfg-container")
    check(container is not None, "Transitions DFG container exists")
    slider = page.query_selector("#dfg-slider")
    check(slider is not None, "Simplify slider exists")

    # Must NOT show "No transitions found" or error
    table_container = page.query_selector("#table-container")
    container_text = table_container.text_content() if table_container else ""
    check("No transitions" not in container_text,
          f"No 'No transitions' message (got: '{container_text[:60]}')")
    check("timeout" not in container_text.lower(),
          "No timeout error")
    check("transitions" in container_text,
          "Transition count shown")

    # Verify the ECharts graph has nodes and rendered
    chart_status = page.evaluate("""
        () => {
            const el = document.getElementById('dfg-container');
            if (!el) return 'no container';
            const c = echarts.getInstanceByDom(el);
            if (!c) return 'no echarts instance';
            const opt = c.getOption();
            if (!opt.series || !opt.series[0]) return 'no series';
            if (opt.series[0].type !== 'graph') return 'not a graph';
            const data = opt.series[0].data;
            if (!data || data.length === 0) return 'no data';
            if (!el.querySelector('canvas') && !el.querySelector('svg')) return 'no visual';
            return 'ok:' + data.length;
        }
    """)
    check(chart_status.startswith("ok"),
          f"Transitions DFG rendered with data ({chart_status})")

    # Variant sections (served by the mock's `variants` command) render below
    check("Execution" in container_text or "Variant" in container_text,
          "Variants section rendered below the DFG")


def test_time_picker(page):
    """13. Time picker opens and quick range buttons work."""
    print("--- Test 13: Time Picker ---")

    page.goto(MOCK_URL)
    page.wait_for_selector("#status.connected", timeout=10000)
    page.wait_for_timeout(1000)

    # Time range button should show a time range
    time_range = page.text_content("#time-range")
    check(len(time_range) > 5, f"Time range displayed: '{time_range[:50]}'")

    # Click to open picker
    page.click("#time-range")
    page.wait_for_timeout(300)

    picker = page.query_selector("#time-picker")
    visible = picker and page.evaluate("el => el.style.display !== 'none'",
                                       picker)
    check(visible, "Time picker opens on click")

    # Quick range buttons exist
    quick_btns = page.query_selector_all(".tp-quick button")
    check(len(quick_btns) >= 8,
          f"Quick range buttons: {len(quick_btns)} (expected >= 8)")

    # Click 'All' button
    all_btn = page.query_selector(".tp-quick button[data-range='0']")
    if all_btn:
        all_btn.click()
        page.wait_for_timeout(500)

        # Picker should close
        visible = page.evaluate("el => el.style.display !== 'none'",
                                page.query_selector("#time-picker"))
        check(not visible, "Picker closes after quick range click")

    # Custom range inputs exist
    from_input = page.query_selector("#tp-from")
    to_input = page.query_selector("#tp-to")
    check(from_input is not None and to_input is not None,
          "Custom range inputs exist")


def test_zoom_out(page):
    """14. Zoom out button works."""
    print("--- Test 14: Zoom Out ---")

    page.goto(MOCK_URL)
    page.wait_for_selector("#status.connected", timeout=10000)
    page.wait_for_timeout(1000)

    # Get initial time range
    initial_range = page.text_content("#time-range")

    # Click zoom out
    page.click("#zoom-out-btn")
    page.wait_for_timeout(1000)

    # Time range should change (zoom out doubles the view span, so the
    # readout's From moves back — a real assertion, not `or True` (U0 hygiene:
    # the old tautology could never fail).
    new_range = page.text_content("#time-range")
    check(new_range != initial_range,
          f"Zoom out changes time range ('{initial_range}' -> '{new_range}')")

    # Button should exist and be clickable
    btn = page.query_selector("#zoom-out-btn")
    check(btn is not None, "Zoom out button exists")


def test_auto_refresh(page):
    """14b. Quick range enables auto-refresh, custom range stops it."""
    print("--- Test 14b: Auto-Refresh ---")

    page.goto(MOCK_URL)
    page.wait_for_selector("#status.connected", timeout=10000)
    page.wait_for_timeout(1000)

    # Live button exists
    live_btn = page.query_selector("#live-btn")
    check(live_btn is not None, "Live button exists")

    # Initially active (starts in live mode with 15m auto-refresh)
    is_active = page.evaluate("el => el.classList.contains('active')",
                              live_btn)
    check(is_active, "Live button active initially (live mode default)")

    # Click "5m" quick range → should activate auto-refresh
    page.click("#time-range")
    page.wait_for_timeout(300)
    btn_5m = page.query_selector(".tp-quick button[data-range='300']")
    if btn_5m:
        btn_5m.click()
        page.wait_for_timeout(500)

        is_active = page.evaluate(
            "el => el.classList.contains('active')",
            page.query_selector("#live-btn"))
        check(is_active, "Live indicator active after selecting '5m'")

    # Click "All" → should stop auto-refresh
    page.click("#time-range")
    page.wait_for_timeout(300)
    btn_all = page.query_selector(".tp-quick button[data-range='0']")
    if btn_all:
        btn_all.click()
        page.wait_for_timeout(500)

        is_active = page.evaluate(
            "el => el.classList.contains('active')",
            page.query_selector("#live-btn"))
        check(not is_active, "Live indicator off after selecting 'All'")

    # Click Live button → should toggle on
    page.click("#live-btn")
    page.wait_for_timeout(1000)
    is_active = page.evaluate(
        "document.getElementById('live-btn').classList.contains('active')")
    check(is_active, "Live button toggles on")

    # Click Live button again → should toggle off
    page.click("#live-btn")
    page.wait_for_timeout(500)
    is_active = page.evaluate(
        "el => el.classList.contains('active')",
        page.query_selector("#live-btn"))
    check(not is_active, "Live button toggles off")


def test_chart_rendering(page):
    """15. AAS chart is rendered — uPlot by default (U2b renderer swap), with
    the ECharts A/B baseline still mountable via ?renderer=echarts.

    B3 moved the AAS chart to ECharts; U2b swapped the default renderer to
    uPlot (the measured ECharts gesture floor tripped the
    INSTRUMENT_ARCHITECTURE §5 gate RED). The default-path assertions now go
    through the view's read-only debug surface (window.__pgwt.aasDebug) plus
    the uPlot canvas; the echarts leg pins that the rollback renderer still
    paints series data."""
    print("--- Test 15: Chart Rendering ---")

    page.goto(MOCK_URL)
    page.wait_for_selector("#status.connected", timeout=10000)
    page.wait_for_timeout(1500)

    # Default renderer is uPlot.
    renderer = page.evaluate("window.__pgwt.aasRenderer")
    check(renderer == "uplot", f"default AAS renderer is uplot (got {renderer})")

    # uPlot renders into a canvas inside the host div.
    canvas = page.query_selector("#aas-chart-container .aas-uplot-host canvas")
    check(canvas is not None, "AAS uPlot canvas rendered")

    # The painted spec must hold series with actual data buckets, and the
    # viewport (x scale) must be the camera window.
    dbg = page.evaluate("window.__pgwt.aasDebug()")
    check(bool(dbg and dbg.get("mounted")), "uPlot instance mounted")
    check(bool(dbg and dbg.get("bucketCount", 0) > 0 and
               len(dbg.get("seriesNames", [])) > 0),
          f"AAS spec has series with data (buckets={dbg and dbg.get('bucketCount')}, "
          f"series={dbg and len(dbg.get('seriesNames', []))})")
    cam = page.evaluate(
        "() => [window.__pgwt.camera.fromNs, window.__pgwt.camera.toNs]")
    xs = dbg.get("xScale") if dbg else None
    check(bool(xs) and abs(xs["min"] * 1e6 - cam[0]) < 1e4 and
          abs(xs["max"] * 1e6 - cam[1]) < 1e4,
          f"x scale is exactly the camera window (scale={xs}, cam={cam})")

    # Chart container should have reasonable dimensions
    height = page.evaluate("""() => {
        const el = document.getElementById('aas-chart-container');
        return el ? el.offsetHeight : -1;
    }""")
    check(height > 100, f"Chart height = {height}px (expected > 100)")

    # A/B baseline: ?renderer=echarts still mounts the ECharts path with
    # stacked series data (the documented rollback for the U2b swap).
    page.goto(MOCK_URL + "&renderer=echarts")
    page.wait_for_selector("#status.connected", timeout=10000)
    page.wait_for_timeout(1500)
    check(page.evaluate("window.__pgwt.aasRenderer") == "echarts",
          "?renderer=echarts selects the ECharts baseline")
    chart_status = page.evaluate("""() => {
        const el = document.getElementById('aas-chart-container');
        if (!el) return 'no container';
        const c = echarts.getInstanceByDom(el);
        if (!c) return 'no echarts instance';
        const opt = c.getOption();
        if (!opt.series || !opt.series.length) return 'no series';
        const withData = opt.series.filter(s => s.data && s.data.length > 0);
        if (withData.length === 0) return 'no series data';
        return 'ok:' + withData.length;
    }""")
    check(chart_status.startswith("ok"),
          f"ECharts baseline has series with data ({chart_status})")


def test_concurrency_tab(page):
    """15b. Concurrency tab shows peak chart and burst table."""
    print("--- Test 15b: Concurrency Tab ---")

    page.goto(MOCK_URL)
    page.wait_for_selector("#status.connected", timeout=10000)

    # Stop auto-refresh so concurrency loads fully
    page.click("#live-btn")
    page.wait_for_timeout(500)

    page.click(".tab[data-tab='concurrency']")
    page.wait_for_timeout(3000)

    active = page.query_selector(".tab.active")
    check(active and active.text_content() == "Concurrency",
          "Concurrency tab active")

    # Peak chart should render
    chart_el = page.query_selector("#concurrency-chart")
    check(chart_el is not None, "Peak concurrency chart container exists")

    has_chart = page.evaluate("""
        () => {
            const el = document.getElementById('concurrency-chart');
            return el && echarts.getInstanceByDom(el) != null;
        }
    """)
    check(has_chart, "Peak concurrency ECharts instance rendered")

    # B4: read the option — assert the peak line series carries per-bucket data.
    cc_status = page.evaluate("""() => {
        const el = document.getElementById('concurrency-chart');
        if (!el) return 'no container';
        const c = echarts.getInstanceByDom(el);
        if (!c) return 'no echarts instance';
        const opt = c.getOption();
        const s0 = opt.series && opt.series[0];
        if (!s0) return 'no series';
        if (s0.type !== 'line') return 'not line: ' + s0.type;
        if (!s0.data || s0.data.length === 0) return 'no data';
        return 'ok:' + s0.data.length;
    }""")
    check(cc_status.startswith("ok"),
          f"Concurrency peak line has data ({cc_status})")

    # Burst table should exist
    burst_el = page.query_selector("#burst-table")
    check(burst_el is not None, "Burst table container exists")

    burst_text = burst_el.text_content() if burst_el else ""
    check("Burst Events" in burst_text or "No burst" in burst_text,
          "Burst section has content")


def test_legend_hover_survives_tick(page):
    """15c. U1: a legend-chip hover-solo survives a live-tick legend rebuild.

    Regression pin for the U1 legend rework (active.js renderLegend): the
    selection is keyed by series NAME and an in-progress hover-solo
    (leg.hovered) is RE-APPLIED after a live tick replaces the chips — the
    pre-U1 trailing apply(selected) popped the chart back to all-selected
    under a stationary cursor. And since the DETACHED chip can never fire
    mouseleave, leaving the legend itself (legDiv.onmouseleave) must unstick.

    U2b: PINNED TO ?renderer=echarts on purpose — this test's wire-tap idiom
    (hooking chart.setOption to record every legend.selected apply) is
    ECharts-specific AAS internals, and the ECharts path must keep exactly
    these semantics while it exists as the A/B baseline. The shared
    renderLegend semantics under the DEFAULT uPlot renderer are covered by
    test_uplot_legend (same hover-solo-survives-tick pin, via the series
    show flags).
    """
    print("--- Test 15c: Legend Hover Survives Live Tick (echarts baseline) ---")

    page.goto(MOCK_URL + "&renderer=echarts")
    page.wait_for_selector("#status.connected", timeout=10000)
    # Live mode is on by default (5 s tick cadence, app.js startAutoRefresh);
    # wait for the external legend chips instead of a fixed sleep.
    page.wait_for_selector("#aas-legend .aleg", timeout=10000)

    # Reads the hidden ECharts legend selection the external chips drive.
    read_selected = """() => {
        const c = echarts.getInstanceByDom(
            document.getElementById('aas-chart-container'));
        if (!c) return null;
        const leg = c.getOption().legend;
        return (leg && leg[0] && leg[0].selected) || null;
    }"""

    # Hover the IO chip and LEAVE the mouse there. Exact-text engine on
    # purpose: :has-text('IO') would also match 'Extension' (case-insensitive
    # substring). IO is stable — class mode always renders the fixed
    # WAIT_CLASSES set, so the same name exists on every tick.
    io_sel = "#aas-legend .aleg:text-is('IO')"
    check(page.query_selector(io_sel) is not None, "IO legend chip rendered")
    page.hover(io_sel)

    # mouseenter → ONE batched setOption({legend:{selected}}) soloing IO.
    page.wait_for_function("""() => {
        const c = echarts.getInstanceByDom(
            document.getElementById('aas-chart-container'));
        if (!c) return false;
        const leg = c.getOption().legend;
        const sel = leg && leg[0] && leg[0].selected;
        if (!sel) return false;
        const on = Object.keys(sel).filter(n => sel[n]);
        return on.length === 1 && on[0] === 'IO';
    }""", timeout=5000)
    sel = page.evaluate(read_selected)
    on = sorted(n for n, v in (sel or {}).items() if v)
    check(on == ["IO"], f"hover solos the IO series (visible={on})")

    # Record every legend apply from here on (same wire-tap idiom as the
    # ws.send hooks): the FINAL state alone is too weak — after the rebuild
    # Chromium delivers a boundary mouseenter to the new chip under the
    # stationary cursor, which would quietly re-solo and mask the pre-U1 bug.
    # The bug is the rebuild's own trailing apply(selected): a synchronous
    # all-on setOption that cancels (at least flickers away) the solo.
    page.evaluate("""() => {
        const c = echarts.getInstanceByDom(
            document.getElementById('aas-chart-container'));
        window.__legendApplies = [];
        const orig = c.setOption.bind(c);
        c.setOption = (opt, ...rest) => {
            if (opt && opt.legend && opt.legend.selected)
                window.__legendApplies.push({...opt.legend.selected});
            return orig(opt, ...rest);
        };
    }""")

    # While STILL hovering, let a live tick rebuild the legend: every mount
    # replaces legDiv.innerHTML, detaching the chip under the cursor. Wait on
    # that detachment (tick cadence 5 s) — a condition, not a sleep.
    chip = page.query_selector(io_sel)
    page.wait_for_function("el => !el.isConnected", arg=chip, timeout=15000)

    # U1 regression: the rebuild must RE-APPLY the in-progress hover-solo —
    # every legend apply across the rebuild keeps the IO solo. The pre-U1
    # trailing apply(sel) shows up here as an all-on apply.
    applies = page.evaluate("window.__legendApplies")
    bad = [sorted(n for n, v in a.items() if v)
           for a in applies if sorted(n for n, v in a.items() if v) != ["IO"]]
    check(len(applies) > 0 and not bad,
          f"hover-solo survived the live-tick rebuild "
          f"({len(applies)} applies, non-solo={bad[:2]})")
    sel = page.evaluate(read_selected)
    on = sorted(n for n, v in (sel or {}).items() if v)
    check(on == ["IO"],
          f"chart still solos IO after the rebuild (visible={on})")

    # Leave the legend: the original chip is detached and can never fire
    # mouseleave, so the persistent legDiv.onmouseleave must clear the stuck
    # hover and restore the all-on selection.
    page.mouse.move(640, 8)
    page.wait_for_function("""() => {
        const c = echarts.getInstanceByDom(
            document.getElementById('aas-chart-container'));
        if (!c) return false;
        const leg = c.getOption().legend;
        const sel = leg && leg[0] && leg[0].selected;
        if (!sel) return false;
        const names = Object.keys(sel);
        return names.length > 0 && names.every(n => sel[n]);
    }""", timeout=5000)
    sel = page.evaluate(read_selected)
    off = sorted(n for n, v in (sel or {}).items() if not v)
    check(bool(sel) and not off,
          f"selection returns to all-on after leaving the legend (off={off})")


def test_uplot_legend(page):
    """15d. U2b: the U1 legend semantics under the DEFAULT uPlot renderer.

    renderLegend is shared code; the renderer hook is applyVisible (uPlot:
    the stack.js-recipe restack + setSeries/bands/setData swap). Pins:
    hover-solo shows exactly one series; the solo SURVIVES a live-tick mount
    rebuild (the U1 re-apply rule, same as 15c's echarts pin); leaving the
    legend restores the persistent all-on selection. Series visibility is
    read from the view's debug surface (uPlot series show flags)."""
    print("--- Test 15d: uPlot Legend Semantics ---")

    page.goto(MOCK_URL)
    page.wait_for_selector("#status.connected", timeout=10000)
    page.wait_for_selector("#aas-legend .aleg", timeout=10000)

    def visible_names():
        d = page.evaluate("window.__pgwt.aasDebug()")
        if not d or not d.get("mounted"):
            return None
        return sorted(n for n, show in
                      zip(d["seriesNames"], d["seriesShow"]) if show)

    # Hover the IO chip and LEAVE the mouse there (class mode: the fixed
    # WAIT_CLASSES set, so 'IO' exists on every tick).
    io_sel = "#aas-legend .aleg:text-is('IO')"
    check(page.query_selector(io_sel) is not None, "IO legend chip rendered")
    page.hover(io_sel)
    page.wait_for_function("""() => {
        const d = window.__pgwt.aasDebug();
        if (!d || !d.mounted) return false;
        const on = d.seriesNames.filter((n, i) => d.seriesShow[i]);
        return on.length === 1 && on[0] === 'IO';
    }""", timeout=5000)
    check(visible_names() == ["IO"], "hover solos the IO series (uPlot show flags)")

    # While STILL hovering, let a live tick rebuild the chips (5 s cadence);
    # the detached-chip wait is a condition, not a sleep.
    chip = page.query_selector(io_sel)
    page.wait_for_function("el => !el.isConnected", arg=chip, timeout=15000)
    page.wait_for_timeout(300)   # trailing applyVisible after the rebuild
    check(visible_names() == ["IO"],
          f"hover-solo survived the live-tick rebuild (visible={visible_names()})")

    # Leave the legend: the persistent selection (all-on) must come back.
    page.mouse.move(640, 8)
    page.wait_for_function("""() => {
        const d = window.__pgwt.aasDebug();
        if (!d || !d.mounted) return false;
        return d.seriesShow.length > 0 && d.seriesShow.every(s => s);
    }""", timeout=5000)
    d = page.evaluate("window.__pgwt.aasDebug()")
    off = [n for n, s in zip(d["seriesNames"], d["seriesShow"]) if not s]
    check(not off, f"all series visible after leaving the legend (off={off})")


def test_uplot_gestures(page):
    """15e. U2b: the camera-owned gesture language on the uPlot pane.

    The payoff of the renderer swap: gestures mutate the CAMERA and the
    renderer draws FROM camera state (a setScale viewport transform — no
    fetch, no rebuild). Language parity pins (unchanged wording): wheel =
    cursor-anchored zoom; shift+drag = pan; plain drag = brush-select;
    dblclick = zoom-out. Every gesture detaches the camera and pauses live
    via the existing U0/U2a machinery."""
    print("--- Test 15e: uPlot Camera Gestures ---")

    page.goto(MOCK_URL)
    page.wait_for_selector("#status.connected", timeout=10000)
    page.wait_for_timeout(1500)

    dbg = page.evaluate("window.__pgwt.aasDebug()")
    check(bool(dbg and dbg.get("mounted")), "uPlot pane mounted before gestures")
    check(page.evaluate("window.__pgwt.camera.mode") == "follow",
          "camera follows (live) before any gesture")

    box = page.evaluate("""() => {
        const el = document.querySelector('.aas-uplot-host .u-over');
        const r = el.getBoundingClientRect();
        return { x: r.left, y: r.top, w: r.width, h: r.height };
    }""")
    cx = box["x"] + box["w"] * 0.5
    cy = box["y"] + box["h"] * 0.5

    # Wheel = cursor-anchored zoom IN: span shrinks, camera detaches, live
    # pauses, and the x scale lands EXACTLY on the camera window.
    span0 = page.evaluate("window.__pgwt.camera.toNs - window.__pgwt.camera.fromNs")
    page.mouse.move(cx, cy)
    page.mouse.wheel(0, -100)
    page.wait_for_timeout(400)   # rAF paint + settle headroom
    span1 = page.evaluate("window.__pgwt.camera.toNs - window.__pgwt.camera.fromNs")
    check(span1 < span0, f"wheel zooms in (span {span0} -> {span1})")
    check(page.evaluate("window.__pgwt.camera.mode") == "detached",
          "wheel gesture detaches the camera")
    live_on = page.evaluate(
        "document.getElementById('live-btn').classList.contains('active')")
    check(not live_on, "gesture paused live mode (U0 machinery)")
    state = page.evaluate("""() => {
        const d = window.__pgwt.aasDebug();
        return { xs: d && d.xScale,
                 from: window.__pgwt.camera.fromNs,
                 to: window.__pgwt.camera.toNs };
    }""")
    xs = state["xs"]
    check(bool(xs) and abs(xs["min"] * 1e6 - state["from"]) < 1e4 and
          abs(xs["max"] * 1e6 - state["to"]) < 1e4,
          f"viewport = camera window after zoom (scale={xs})")

    # Shift+drag = pan: dragging the canvas LEFT moves the window FORWARD.
    from_before = page.evaluate("window.__pgwt.camera.fromNs")
    page.keyboard.down("Shift")
    page.mouse.move(cx, cy)
    page.mouse.down()
    page.mouse.move(cx - 120, cy, steps=6)
    page.mouse.up()
    page.keyboard.up("Shift")
    page.wait_for_timeout(400)
    from_after = page.evaluate("window.__pgwt.camera.fromNs")
    check(from_after > from_before,
          f"shift+drag pans forward (from {from_before} -> {from_after})")
    # No brush zoom happened: the selection overlay must ignore shift-drags.
    span2 = page.evaluate("window.__pgwt.camera.toNs - window.__pgwt.camera.fromNs")
    check(abs(span2 - span1) < span1 * 1e-6,
          f"pan preserved the span (span {span1} -> {span2})")

    # Plain drag = brush-select: the window narrows to the dragged sub-range.
    win_before = page.evaluate(
        "() => [window.__pgwt.timeRange.from, window.__pgwt.timeRange.to]")
    x1 = box["x"] + box["w"] * 0.30
    x2 = box["x"] + box["w"] * 0.60
    page.mouse.move(x1, cy)
    page.mouse.down()
    page.mouse.move(x2, cy, steps=5)
    page.mouse.up()
    page.wait_for_timeout(600)
    win_after = page.evaluate(
        "() => [window.__pgwt.timeRange.from, window.__pgwt.timeRange.to]")
    check(win_after[1] - win_after[0] < win_before[1] - win_before[0] and
          win_after[0] >= win_before[0] - 1e6 and
          win_after[1] <= win_before[1] + 1e6,
          f"plain drag brush-selects a sub-window ({win_before} -> {win_after})")

    # dblclick = zoom-out (the app's handler; uPlot's own dblclick autoscale
    # is unbound so it can never race this).
    span3 = page.evaluate("window.__pgwt.timeRange.to - window.__pgwt.timeRange.from")
    page.mouse.dblclick(cx, cy)
    page.wait_for_timeout(600)
    span4 = page.evaluate("window.__pgwt.timeRange.to - window.__pgwt.timeRange.from")
    check(span4 > span3, f"dblclick zooms out (span {span3} -> {span4})")

    # Honesty overlays exist under this zoomed camera state: the N-CPUs line
    # is y-space geometry and must be present in EVERY camera state.
    dbg = page.evaluate("window.__pgwt.aasDebug()")
    hl = (dbg or {}).get("overlays", {}) or {}
    kinds = [l.get("kind") for l in hl.get("hlines", [])]
    check("ncpus" in kinds,
          f"N-CPUs overlay line present under the gestured camera (hlines={kinds})")


def test_reconnection(page, mock_proc):
    """16. WebSocket reconnects after disconnect."""
    print("--- Test 16: Reconnection ---")

    page.goto(MOCK_URL)
    page.wait_for_selector("#status.connected", timeout=10000)

    # Kill and restart mock server
    stop_mock_server(mock_proc)
    page.wait_for_timeout(1000)

    # Status should show reconnecting/error
    status = page.text_content("#status")
    check("Reconnect" in status or "error" in status.lower() or
          "Connect" in status,
          f"Status shows reconnecting after disconnect: '{status}'")

    # Restart server
    new_proc = start_mock_server()
    page.wait_for_timeout(5000)  # Wait for reconnect (2s backoff)

    # Should reconnect
    status = page.text_content("#status")
    check("CPUs" in status or "connected" in status.lower(),
          f"Reconnected after server restart: '{status}'")

    return new_proc


def test_session_drill_to_timeline(page):
    """17. Full drill flow: Sessions -> Timeline shows events."""
    print("--- Test 17: Session -> Timeline Flow ---")

    page.goto(MOCK_URL)
    page.wait_for_selector("#status.connected", timeout=10000)
    page.click(".tab[data-tab='sessions']")
    page.wait_for_timeout(1000)

    # Get PID from first row
    first_cell = page.text_content(
        "#table-container table tbody tr:first-child td:first-child")
    check(first_cell and first_cell.strip().isdigit(),
          f"First session PID: {first_cell}")

    # Click to drill
    page.click("#table-container table tbody tr.clickable")
    page.wait_for_timeout(1500)

    # Breadcrumb should show pid filter (label format: "PID <n>")
    breadcrumb = page.text_content("#breadcrumb")
    check("PID" in breadcrumb and first_cell.strip() in breadcrumb,
          f"Breadcrumb shows pid filter: '{breadcrumb[:60]}'")


def test_query_drill(page):
    """18. Click query row -> drill to Events with query_id filter."""
    print("--- Test 18: Query Drill-down ---")

    page.goto(MOCK_URL)
    page.wait_for_selector("#status.connected", timeout=10000)
    page.click(".tab[data-tab='queries']")
    page.wait_for_timeout(1000)

    # Click first query
    first_row = page.query_selector("#table-container table tbody tr.clickable")
    if first_row:
        first_row.click()
        page.wait_for_timeout(1000)

        active = page.query_selector(".tab.active")
        check(active and active.text_content() == "Events",
              "Query drill -> Events tab")

        # Breadcrumb label for a query drill is the query text prefix
        breadcrumb = page.text_content("#breadcrumb")
        check("UPDATE" in breadcrumb or "SELECT" in breadcrumb
              or "Query" in breadcrumb,
              f"Breadcrumb shows query filter: '{breadcrumb[:60]}'")
    else:
        check(False, "No query row to click")
        check(False, "(skipped query drill)")


def test_exact_summary_values(page):
    """19. Summary bar shows exact values from canned data."""
    print("--- Test 19: Exact Summary Values ---")

    page.goto(MOCK_URL)
    page.wait_for_selector("#status.connected", timeout=10000)
    page.wait_for_timeout(1000)

    metrics = page.query_selector_all(".metric")
    vals = {}
    for m in metrics:
        label = m.query_selector(".metric-label").text_content()
        value = m.query_selector(".metric-value").text_content()
        vals[label] = value

    # Canned: DB Time=12500ms -> fmtMs -> "12.5s"
    check(vals.get("DB Time") == "12.5s",
          f"DB Time = 12.5s (got '{vals.get('DB Time')}')")
    # Wall=3600000ms -> "3600.0s"
    check(vals.get("Wall") == "3600.0s",
          f"Wall = 3600.0s (got '{vals.get('Wall')}')")
    # AAS=3.47
    check(vals.get("AAS") == "3.47",
          f"AAS = 3.47 (got '{vals.get('AAS')}')")
    # Idle=45000ms -> "45.0s"
    check(vals.get("Idle") == "45.0s",
          f"Idle = 45.0s (got '{vals.get('Idle')}')")
    # CPUs=4
    check(vals.get("CPUs") == "4",
          f"CPUs = 4 (got '{vals.get('CPUs')}')")


def test_exact_event_values(page):
    """20. Events table cell values match canned data."""
    print("--- Test 20: Exact Event Cell Values ---")

    page.goto(MOCK_URL)
    page.wait_for_selector("#status.connected", timeout=10000)
    page.click(".tab[data-tab='events']")
    page.wait_for_timeout(1000)

    # Read first row cells (default sort by pct desc → CPU* is first)
    cells = page.query_selector_all(
        "#table-container table tbody tr:first-child td")
    cell_texts = [c.text_content().strip() for c in cells]

    # Canned CPU*: count=250000, total_ms=4800, avg_us=19.2,
    #   p50=12, p95=45, p99=120, max=5000, pct=38.4, aas=1.33
    check(any("CPU" in t for t in cell_texts),
          f"First row is CPU* (cells: {cell_texts[:3]})")
    check(any("250" in t for t in cell_texts),
          f"CPU* count contains '250' (250.0K)")
    check(any("4800" in t or "4.8s" in t for t in cell_texts),
          f"CPU* total ~4800ms")
    check(any("38.4" in t for t in cell_texts),
          f"CPU* pct = 38.4%")
    check(any("1.33" in t for t in cell_texts),
          f"CPU* AAS = 1.33")

    # Verify second row is IO:DataFileRead (pct=16.8)
    second_cells = page.query_selector_all(
        "#table-container table tbody tr:nth-child(2) td")
    second_texts = [c.text_content().strip() for c in second_cells]
    check(any("DataFileRead" in t for t in second_texts),
          f"Second event is IO:DataFileRead")
    check(any("16.8" in t for t in second_texts),
          f"IO:DataFileRead pct = 16.8%")


def test_exact_session_values(page):
    """21. Sessions table cell values match canned data."""
    print("--- Test 21: Exact Session Cell Values ---")

    page.goto(MOCK_URL)
    page.wait_for_selector("#status.connected", timeout=10000)
    page.click(".tab[data-tab='sessions']")
    page.wait_for_timeout(1000)

    # First session row: pid=1001, user=postgres, db=testdb,
    #   db_time_ms=5200, cpu_pct=45.0, wait_pct=55.0, top_wait=IO:DataFileRead
    first_row = page.text_content(
        "#table-container table tbody tr:first-child")
    check("1001" in first_row, f"First session PID = 1001")
    check("postgres" in first_row, f"First session user = postgres")
    check("testdb" in first_row, f"First session db = testdb")
    check("DataFileRead" in first_row,
          f"First session top wait = IO:DataFileRead")

    # Verify system backend row exists (checkpointer pid=4870)
    table_text = page.text_content("#table-container")
    check("4870" in table_text, "Checkpointer PID 4870 in sessions")
    check("checkpointer" in table_text, "Checkpointer type shown")


def test_exact_query_values(page):
    """22. Queries table cell values match canned data."""
    print("--- Test 22: Exact Query Cell Values ---")

    page.goto(MOCK_URL)
    page.wait_for_selector("#status.connected", timeout=10000)
    page.click(".tab[data-tab='queries']")
    page.wait_for_timeout(1000)

    first_row = page.text_content(
        "#table-container table tbody tr:first-child")
    # Canned: query_id=3886912043147135675, text starts with "UPDATE pgbench_accounts"
    check("UPDATE" in first_row or "pgbench_accounts" in first_row,
          f"First query text contains UPDATE pgbench_accounts")
    check("33.6" in first_row,
          f"First query pct = 33.6%")

    # Second query
    second_row = page.text_content(
        "#table-container table tbody tr:nth-child(2)")
    check("SELECT" in second_row, "Second query is SELECT")


def test_timeline_bar_positions(page):
    """23. Timeline bars: correct start positions (Bug 1) + window clamp (P6).

    U0/P6 changed the bar contract (lib/builders/timeline.js): d[0]/d[1] are
    the DRAWN interval, clamped to the view window so long waits no longer
    bleed left across the PID axis labels; the raw duration keeps riding at
    d[6] and the raw (unclamped) start at d[7] for the tooltip. This test used
    to pin the unclamped geometry (raw start<end, raw 50 s span on d[0]/d[1]);
    flipped here to pin the clamped contract instead (the flip-the-pin rule —
    the old assertions pinned exactly the P6 bleed).
    """
    print("--- Test 23: Timeline Bar Positions ---")

    page.goto(MOCK_URL)
    page.wait_for_selector("#status.connected", timeout=10000)

    # Drill to timeline via session click
    page.click(".tab[data-tab='sessions']")
    page.wait_for_timeout(1000)
    page.click("#table-container table tbody tr.clickable")
    page.wait_for_timeout(1500)

    # Extract bars + the view window via the ECharts API. Bar layout:
    # [drawnStartNs, drawnEndNs, pidIdx, name, classIdx, query, rawDurNs,
    #  rawStartNs]
    probe = page.evaluate("""() => {
        const chart = echarts.getInstanceByDom(
            document.getElementById('timeline-chart'));
        if (!chart) return null;
        const opt = chart.getOption();
        if (!opt || !opt.series || !opt.series[0]) return null;
        const data = opt.series[0].data;
        return {
            bars: data.slice(0, 4).map(d => [d[0], d[1], d[6], d[7]]),
            from: window.__pgwt.timeRange.from,
            to: window.__pgwt.timeRange.to,
        };
    }""")

    if probe and probe.get("bars"):
        bars = probe["bars"]
        vfrom, vto = int(probe["from"]), int(probe["to"])
        check(len(bars) > 0, f"Timeline has {len(bars)} bars")

        # P6 ALIGNMENT: every DRAWN coordinate stays inside the view window —
        # no bar bleeds left over the PID labels or right past the axis.
        inside = all(b[0] >= vfrom and b[1] <= vto for b in bars)
        check(inside,
              f"all drawn bar coords clamped to the window "
              f"(bars={[(b[0], b[1]) for b in bars]}, window=[{vfrom},{vto}])")

        # Raw durations survive the clamp (tooltip truth). Canned first event
        # for pid 1001: s = _FROM_NS + 100 s, d = 50 s.
        check(all(b[2] > 0 for b in bars),
              "all bars keep a positive raw duration (d[6])")
        check(abs(bars[0][2] - 50_000_000_000) < 1_000_000_000,
              f"first bar raw duration ≈ 50s (got {bars[0][2]/1e9:.1f}s)")

        # Bug 1 regression, now on the raw start (d[7]): start = s, never s+d.
        check(bars[0][3] <= bars[0][0],
              f"raw start <= drawn start (raw={bars[0][3]}, drawn={bars[0][0]})")
        if bars[0][3] < vfrom:
            # The canned first wait begins before the live window: its drawn
            # start must sit exactly on the window edge (the P6 clamp).
            check(bars[0][0] == vfrom,
                  f"pre-window wait drawn from the window edge "
                  f"(drawn={bars[0][0]}, from={vfrom})")
        else:
            check(bars[0][0] == bars[0][3],
                  "in-window wait drawn at its raw start")
    else:
        check(False, "Could not extract timeline chart data")
        for _ in range(5):
            check(False, "(skipped bar position/clamp checks)")


def test_no_double_refresh(page):
    """24. Drill-down sends exactly one refresh, not two (Bug 13 regression)."""
    print("--- Test 24: No Double Refresh ---")

    page.goto(MOCK_URL)
    page.wait_for_selector("#status.connected", timeout=10000)
    page.wait_for_timeout(1000)

    # Install WS message counter. The UI no longer has a global `state`; the
    # WebSocket lives on the transport module, exposed for tests via __pgwt.
    page.evaluate("""() => {
        window.__wsMsgLog = [];
        const ws = window.__pgwt.transport.ws;
        const origSend = ws.send.bind(ws);
        ws.send = function(data) {
            window.__wsMsgLog.push(JSON.parse(data));
            return origSend(data);
        };
    }""")

    # Clear the log, then drill down from Overview
    page.evaluate("window.__wsMsgLog = []")
    io_row = page.query_selector("tr.indent-1.clickable")
    if io_row:
        io_row.click()
        page.wait_for_timeout(1500)

        msgs = page.evaluate("window.__wsMsgLog")
        # A single refresh() calls refreshChart() + refreshTable()
        # = 1 aas + 1 top_events = 2 messages. Double refresh = 4.
        #
        # UPDATED in U2a (camera-lite): the AAS mount now also PRELOADS the
        # strip cache — one background `aas` fetch over the 3x-skirt window
        # via the transport's non-superseding send() (the drill invalidated
        # the cache, so exactly one strip refill is the INTENDED behavior,
        # not a double refresh). Strips are distinguishable on the wire by
        # their bucket count: the poll asks for <= 300 window buckets, a
        # strip for [3x, 6x) that target. Pin BOTH: exactly one poll aas
        # (the original no-double-refresh regression) and at most one strip
        # (the cache's in-flight dedup).
        cmds = [m.get("cmd") for m in msgs]
        aas_polls = [m for m in msgs if m.get("cmd") == "aas"
                     and (m.get("buckets") or 0) <= 300]
        aas_strips = [m for m in msgs if m.get("cmd") == "aas"
                      and (m.get("buckets") or 0) > 300]
        table_count = sum(1 for c in cmds if c in
                          ("top_events", "top_sessions", "top_queries",
                           "time_model", "heatmap", "session_timeline"))

        check(len(aas_polls) == 1,
              f"Drill-down sent {len(aas_polls)} window aas request(s) (expected 1)")
        check(len(aas_strips) <= 1,
              f"Drill-down sent {len(aas_strips)} strip prefetch(es) (expected <= 1)")
        check(table_count == 1,
              f"Drill-down sent {table_count} table request(s) (expected 1)")
    else:
        check(False, "No clickable row for double-refresh test")
        check(False, "(skipped)")
        check(False, "(skipped)")


def test_filter_persists_across_tabs(page):
    """25. Manually switching tabs preserves active filter."""
    print("--- Test 25: Filter Persistence Across Tabs ---")

    page.goto(MOCK_URL)
    page.wait_for_selector("#status.connected", timeout=10000)
    page.wait_for_timeout(1000)

    # Drill into a class from Overview to set a filter
    io_row = page.query_selector("tr.indent-1.clickable")
    if io_row:
        io_row.click()
        page.wait_for_timeout(1000)

        # Should be on Events with class filter
        breadcrumb_before = page.text_content("#breadcrumb")
        check(len(breadcrumb_before.strip()) > 0,
              f"Filter active: '{breadcrumb_before.strip()[:40]}'")

        # Manually switch to Sessions tab
        page.click(".tab[data-tab='sessions']")
        page.wait_for_timeout(1000)

        # Breadcrumb should still show the filter
        breadcrumb_after = page.text_content("#breadcrumb")
        check(breadcrumb_after.strip() == breadcrumb_before.strip(),
              f"Filter preserved after tab switch")

        # Switch to Queries tab
        page.click(".tab[data-tab='queries']")
        page.wait_for_timeout(1000)

        breadcrumb_queries = page.text_content("#breadcrumb")
        check(breadcrumb_queries.strip() == breadcrumb_before.strip(),
              f"Filter still preserved on Queries tab")

        # Switch back to Overview
        page.click(".tab[data-tab='overview']")
        page.wait_for_timeout(1000)

        breadcrumb_overview = page.text_content("#breadcrumb")
        check(breadcrumb_overview.strip() == breadcrumb_before.strip(),
              f"Filter preserved back on Overview tab")
    else:
        check(False, "No clickable row for filter persistence test")
        check(False, "(skipped)")
        check(False, "(skipped)")
        check(False, "(skipped)")


# ── Trust Milestone T6: transport trust ───────────────────────────────────────

def test_degraded_transport(page):
    """T6/UI-1: a dead upstream (SSH/pgwt-server behind a live bridge) shows a
    DEGRADED state — red pulsing pill + connection-lost overlay — never
    "No data" under a green pill; live mode freezes; recovery needs no reload."""
    print("--- Test T6.1: Degraded Transport ---")

    page.goto(MOCK_URL)
    page.wait_for_selector("#status.connected", timeout=10000)
    page.wait_for_timeout(1000)

    # Sanity: data on screen, live mode on, overlay hidden.
    check(page.query_selector("#table-container table tbody tr") is not None,
          "table has data before the outage")
    overlay_visible = page.evaluate(
        "document.getElementById('degraded-overlay').style.display !== 'none'")
    check(not overlay_visible, "degraded overlay hidden while healthy")

    # Kill the upstream: the WS STAYS OPEN (mirrors the Go bridge whose SSH
    # pipe died) and every request now gets a transport error envelope.
    page.evaluate(
        "window.__pgwt.transport.send('test_upstream_down', {}).catch(() => {})")
    page.wait_for_timeout(300)

    # A user action fails immediately → degraded state, within one action.
    page.click(".tab[data-tab='events']")
    page.wait_for_timeout(1000)

    status_cls = page.get_attribute("#status", "class") or ""
    check("degraded" in status_cls,
          f"status pill shows degraded (class='{status_cls}')")
    status_txt = page.text_content("#status") or ""
    check("lost" in status_txt.lower() or "retry" in status_txt.lower(),
          f"status text explains the outage: '{status_txt}'")
    overlay_visible = page.evaluate(
        "document.getElementById('degraded-overlay').style.display !== 'none'")
    check(overlay_visible, "connection-lost overlay is visible")
    container = page.text_content("#table-container") or ""
    check("No data" not in container,
          "tables do NOT claim 'No data' during a transport outage")
    check(page.evaluate("window.__pgwt.degraded.active") is True,
          "degraded state exposed to tests")

    # Live mode must stop pretending to tick: the window stays frozen across
    # a live tick (5 s cadence) while degraded.
    t0 = page.evaluate(
        "() => [window.__pgwt.timeRange.from, window.__pgwt.timeRange.to]")
    page.wait_for_timeout(6000)
    t1 = page.evaluate(
        "() => [window.__pgwt.timeRange.from, window.__pgwt.timeRange.to]")
    check(t0 == t1, f"window frozen while degraded (was {t0}, now {t1})")
    check(page.evaluate("window.__pgwt.degraded.active") is True,
          "still degraded while upstream stays dead")

    # Revive the upstream: the recovery probe (2.5 s cadence) must resync —
    # connected pill, overlay gone, data repainted — WITHOUT a page reload.
    page.evaluate(
        "window.__pgwt.transport.send('test_upstream_up', {}).catch(() => {})")
    page.wait_for_selector("#status.connected", timeout=15000)
    page.wait_for_timeout(500)
    check(page.evaluate("window.__pgwt.degraded.active") is False,
          "degraded state cleared after recovery")
    overlay_visible = page.evaluate(
        "document.getElementById('degraded-overlay').style.display !== 'none'")
    check(not overlay_visible, "overlay hidden after recovery")
    check(page.query_selector("#table-container table tbody tr") is not None,
          "table repainted with data after recovery")


def test_custom_range_utc_and_aas_empty_state(page):
    """T6/UI-11 + UI-5: the custom-range inputs are UTC even in a UTC+3
    browser, and an out-of-range (empty) window CLEARS the AAS chart instead
    of leaving the previous window's paint under 'No data' tables."""
    print("--- Test T6.2: UTC Custom Range + AAS Empty State ---")

    # A dedicated UTC+3 context (Europe/Moscow) — the UI-11 repro condition.
    # (MOCK_URL's ?ws= override reaches the mock's WS port; no monkey-patch.)
    browser = page.context.browser
    ctx = browser.new_context(viewport={"width": 1280, "height": 900},
                              timezone_id="Europe/Moscow")
    ctx.add_init_script(UNHANDLED_REJECTION_HOOK)
    p = ctx.new_page()
    guard = ConsoleErrorGuard(p)
    prev_capture = (_capture["page"], _capture["guard"])
    set_failure_capture(p, guard)
    try:
        p.goto(MOCK_URL)
        p.wait_for_selector("#status.connected", timeout=10000)
        p.wait_for_timeout(1200)

        # The window readout is labeled UTC (UI-11).
        check(" UTC" in (p.text_content("#time-range") or ""),
              "time-range readout labeled UTC")

        # AAS chart has series data initially (default uPlot renderer; the
        # debug surface reads the painted spec — U2b).
        d = p.evaluate("window.__pgwt.aasDebug()")
        check(bool(d and d.get("mounted") and d.get("bucketCount", 0) > 0),
              "AAS chart painted for the live window")

        # Apply a custom range far outside the data. Typed values are UTC:
        # 2039-01-01 00:00-01:00 UTC, regardless of the browser's UTC+3 zone.
        p.click("#live-btn")            # stop live mode
        p.wait_for_timeout(200)
        p.click("#time-range")
        p.wait_for_timeout(300)
        # Set the values directly (Chromium's fill() rejects seconds in
        # datetime-local); the Apply handler reads .value, no events needed.
        p.eval_on_selector("#tp-from", "el => el.value = '2039-01-01T00:00:00'")
        p.eval_on_selector("#tp-to", "el => el.value = '2039-01-01T01:00:00'")
        p.click("#tp-apply")
        p.wait_for_timeout(1200)

        # UI-11: parsed as UTC — no 3-hour shift in a Moscow browser.
        import calendar
        want_from = calendar.timegm((2039, 1, 1, 0, 0, 0)) * 1_000_000_000
        got_from = p.evaluate("window.__pgwt.timeRange.from")
        check(int(got_from) == want_from,
              f"custom From parsed as UTC (want {want_from}, got {int(got_from)})")

        # UI-5: the empty window CLEARED the chart (no stale series paint).
        # uPlot form (U2b): the instance is destroyed and an explicit
        # empty-state card takes the pane — same contract as the old ECharts
        # clear + 'No data' graphic.
        state = p.evaluate("""() => {
            const d = window.__pgwt.aasDebug();
            const card = document.querySelector(
                '#aas-chart-container .aas-empty');
            return { renderer: d && d.renderer, mounted: !!(d && d.mounted),
                     hasData: !!(d && d.hasData),
                     cardText: card ? card.textContent : null };
        }""")
        check(not state.get("mounted") and not state.get("hasData"),
              f"AAS chart cleared on empty window ({state})")
        check(state.get("cardText") == "No data in selected range",
              f"AAS chart shows an explicit 'No data' placeholder "
              f"(got {state.get('cardText')!r})")

        assert_no_console_errors(p, guard, "utc/empty-state")
    finally:
        set_failure_capture(*prev_capture)
        ctx.close()


def test_reconnect_idempotent_no_leak(page):
    """T6/UI-4: N WS reconnects run the RESYNC path, not a fresh init — no
    duplicated chart instances and no duplicated tab/resize listeners."""
    print("--- Test T6.3: Reconnect Idempotence ---")

    page.goto(MOCK_URL)
    page.wait_for_selector("#status.connected", timeout=10000)
    page.wait_for_timeout(1000)
    page.click("#live-btn")   # stop live so ticks don't muddy request counts
    page.wait_for_timeout(300)

    # Force three full WS reconnect cycles (close → 2s backoff → reconnect).
    for i in range(3):
        page.evaluate("window.__pgwt.transport.ws.close()")
        page.wait_for_selector("#status.connected", timeout=15000)
        page.wait_for_timeout(400)

    # Chart instances bounded: the persistent AAS chart only (overview tab has
    # no chart of its own). A leaking init() would add one per reconnect.
    charts = page.evaluate("""() => {
        let n = 0;
        for (const el of document.querySelectorAll('*')) {
            try { if (echarts.getInstanceByDom(el)) n++; } catch (e) {}
        }
        return n;
    }""")
    check(charts <= 2,
          f"no ECharts instance leak after 3 reconnects (live instances={charts})")

    # U2b: the AAS pane is uPlot by default — exactly ONE uPlot root must
    # exist (resync mounts reuse the instance; a leaking init would add one
    # .uplot per reconnect).
    uplot_roots = page.evaluate(
        "document.querySelectorAll('#aas-chart-container .uplot').length")
    check(uplot_roots == 1,
          f"no uPlot instance leak after 3 reconnects (roots={uplot_roots})")

    # Tab listeners not duplicated: one tab click must produce exactly one
    # table request + one aas request (N+1 listeners would send N+1 each).
    page.evaluate("""() => {
        window.__wsMsgLog = [];
        const ws = window.__pgwt.transport.ws;
        const origSend = ws.send.bind(ws);
        ws.send = function(data) {
            window.__wsMsgLog.push(JSON.parse(data));
            return origSend(data);
        };
    }""")
    page.click(".tab[data-tab='events']")
    page.wait_for_timeout(1500)
    cmds = [m.get("cmd") for m in page.evaluate("window.__wsMsgLog")]
    check(cmds.count("top_events") == 1,
          f"one tab click sends exactly 1 top_events after reconnects (got {cmds})")
    check(cmds.count("aas") == 1,
          f"one tab click sends exactly 1 aas after reconnects (got {cmds})")

    page.click(".tab[data-tab='overview']")
    page.wait_for_timeout(500)


# ── Track U Phase U0: error visibility (P1) ───────────────────────────────────

def test_command_error_card(page):
    """U0/P1: a command-level error envelope (transport:false) paints a
    per-pane error card with a Retry button and logs a '[pgwt]'-prefixed
    console error — never a silent blank pane. Provoked by rewriting the next
    top_events request ON THE WIRE to a command the mock answers with a plain
    error envelope ("unknown command: ..."), so the real client path runs
    end-to-end. Registered with expect_pgwt: the guard asserts the [pgwt]
    console.error actually fired."""
    print("--- Test U0.1: Command Error Card ---")

    page.goto(MOCK_URL)
    page.wait_for_selector("#status.connected", timeout=10000)
    page.wait_for_timeout(1000)
    page.click("#live-btn")   # stop live so the provoked refresh is the only one
    page.wait_for_timeout(300)

    # One-shot wire rewrite: the next top_events request goes out as an
    # unknown command; the mock replies {"id": N, "error": "unknown command:
    # ..."} with no transport flag — exactly a healthy-server refusal.
    page.evaluate("""() => {
        const ws = window.__pgwt.transport.ws;
        const orig = ws.send.bind(ws);
        ws.send = (data) => {
            const msg = JSON.parse(data);
            if (msg.cmd === 'top_events') {
                msg.cmd = 'top_events_broken';
                ws.send = orig;   // one-shot: the Retry goes through clean
            }
            return orig(JSON.stringify(msg));
        };
    }""")

    page.click(".tab[data-tab='events']")
    page.wait_for_timeout(1000)

    card = page.query_selector("#table-container > .pane-error")
    check(card is not None, "pane error card painted on a command error")
    if card:
        txt = card.text_content() or ""
        check("unknown command" in txt,
              f"card carries the server's error text ('{txt.strip()[:80]}')")
        detail_el = page.query_selector(
            "#table-container > .pane-error .pane-error-detail")
        detail = detail_el.text_content() if detail_el else ""
        check("view: events" in detail and "command: top_events" in detail,
              f"card names the failing view + command ('{detail[:80]}')")
        check(page.query_selector("#table-container > .loading") is None,
              "no eternal 'Loading...' placeholder behind the error card")

        # Retry (the wire rewrite is gone) repaints Events and clears the card.
        btn = page.query_selector(
            "#table-container > .pane-error .pane-error-retry")
        check(btn is not None, "error card offers a Retry button")
        if btn:
            btn.click()
            page.wait_for_timeout(1000)
            check(page.query_selector("#table-container > .pane-error") is None,
                  "error card cleared after a successful retry")
            headers = [th.text_content().strip() for th in
                       page.query_selector_all("#table-container table thead th")]
            check("Wait Event" in headers,
                  f"Events table repainted after retry (headers={headers[:3]})")
        else:
            check(False, "(skipped: no Retry button)")
            check(False, "(skipped retry repaint check)")
    else:
        for skipped in ("error text", "view/command detail", "loading removal",
                        "Retry button", "retry clears card", "retry repaint"):
            check(False, f"(skipped: no error card — {skipped})")


# The guard must SEE the provoked [pgwt] console error (see main()'s loop).
test_command_error_card.expect_pgwt = True


# ── Phase B5: fidelity-aware UI ───────────────────────────────────────────────
# These run against a SECOND mock launched in sampled fidelity with the daemon
# control command enabled, so the escalate flow, the sampled shading, the
# unavailable panels, and the metrics panel can be driven deterministically.

B5_PORT = MOCK_PORT + 10
B5_URL = app_url(B5_PORT)


def test_b5_sampled_shading(page):
    """B5.1: AAS chart shades the sampled window + shows the fidelity legend."""
    print("--- Test B5.1: Sampled Fidelity Shading ---")

    page.goto(B5_URL)
    page.wait_for_selector("#status.connected", timeout=10000)
    page.wait_for_timeout(1500)

    # U2b: under the default uPlot renderer the sampled-honesty band is
    # overlay GEOMETRY painted by the draw hooks (lib/uplot-aas.js
    # overlayGeometry) — assert a sampled rect exists under the CURRENT
    # viewport, and that the chip explains it. (The ECharts markArea form of
    # this pin lives on via the fidelity_sampled_shading snapshot +
    # ?renderer=echarts.)
    shading = page.evaluate("""() => {
        const d = window.__pgwt.aasDebug();
        if (!d || !d.mounted) return 'no instance';
        if (!d.overlays) return 'no overlays';
        const bands = d.overlays.rects.filter(
            r => r.kind === 'sampled' || r.kind === 'mixed');
        if (!bands.length) return 'no fidelity band';
        return 'ok:' + bands.length;
    }""")
    check(shading.startswith("ok"),
          f"AAS pane has a fidelity shading band in view ({shading})")

    chip = page.query_selector("#aas-fidelity-chip")
    check(chip is not None, "Fidelity legend chip rendered")
    if chip:
        txt = chip.text_content()
        check("sampled" in txt.lower(),
              f"Fidelity chip explains sampled shading: '{txt[:60]}'")


def test_b5_unavailable_panel(page):
    """B5.2: an EXACT-only view over a sampled window shows the escalate panel."""
    print("--- Test B5.2: Exact-only Unavailable Panel ---")

    page.goto(B5_URL)
    page.wait_for_selector("#status.connected", timeout=10000)

    # Stop auto-refresh so the panel is stable, then open Transitions (EXACT).
    page.click("#live-btn")
    page.wait_for_timeout(300)
    page.click(".tab[data-tab='transitions']")
    page.wait_for_timeout(1500)

    panel = page.query_selector(".unavailable-panel")
    check(panel is not None, "Transitions shows the unavailable panel")
    if panel:
        txt = panel.text_content()
        check("full-fidelity" in txt.lower(),
              f"Panel explains no full-fidelity data: '{txt[:60]}'")
        btn = page.query_selector(".unavailable-panel .escalate-btn")
        check(btn is not None, "Panel offers an Escalate button")

    # Concurrency (also EXACT-required) shows the same panel.
    page.click(".tab[data-tab='concurrency']")
    page.wait_for_timeout(1500)
    check(page.query_selector(".unavailable-panel") is not None,
          "Concurrency shows the unavailable panel")

    # Histogram (EXACT) shows it inside the heatmap container.
    page.click(".tab[data-tab='histogram']")
    page.wait_for_timeout(1500)
    check(page.query_selector(".unavailable-panel") is not None,
          "Histogram shows the unavailable panel")


def test_b5_escalate_flow(page):
    """B5.3: clicking Escalate transitions the daemon to escalated + budget drops."""
    print("--- Test B5.3: Escalate Flow ---")

    page.goto(B5_URL)
    page.wait_for_selector("#status.connected", timeout=10000)
    page.wait_for_timeout(1500)

    # The header escalate control is visible (daemon supports tiered escalation).
    ctrl = page.query_selector("#escalate-control")
    check(ctrl is not None and ctrl.is_visible(),
          "Escalate control visible (daemon supports escalation)")

    btn = page.query_selector("#hdr-escalate")
    check(btn is not None, "Escalate button present")
    label_before = btn.text_content() if btn else ""
    check("Escalate" in label_before, f"Button reads 'Escalate' (got '{label_before}')")

    # Read the status before escalating.
    tier_before = page.evaluate("window.__pgwt && window.__pgwt.daemonTier()")
    check(tier_before == "sampled", f"Tier sampled before escalate (got {tier_before})")

    # Click Escalate.
    btn.click()
    page.wait_for_timeout(1200)

    tier_after = page.evaluate("window.__pgwt && window.__pgwt.daemonTier()")
    check(tier_after == "escalated", f"Tier escalated after click (got {tier_after})")

    # The control now shows seconds remaining + a Stop affordance.
    btn2 = page.query_selector("#hdr-escalate")
    label_after = btn2.text_content() if btn2 else ""
    check("left" in label_after or "Escalated" in label_after,
          f"Button shows escalated state: '{label_after}'")
    check(page.query_selector("#hdr-deescalate") is not None,
          "Stop (de-escalate) button appears while escalated")


def test_b5_metrics_panel(page):
    """B5.4: the daemon self-metrics panel renders events/s, drops, overhead."""
    print("--- Test B5.4: Daemon Metrics Panel ---")

    page.goto(B5_URL)
    page.wait_for_selector("#status.connected", timeout=10000)
    page.wait_for_timeout(1200)

    mbtn = page.query_selector("#metrics-btn")
    check(mbtn is not None and mbtn.is_visible(),
          "Metrics button visible when daemon present")

    mbtn.click()
    page.wait_for_timeout(800)

    panel = page.query_selector("#daemon-metrics")
    check(panel is not None and panel.is_visible(), "Metrics panel opens")
    if panel:
        txt = panel.text_content()
        check("Events/s" in txt, "Metrics panel shows Events/s")
        check("drops" in txt.lower(), "Metrics panel shows ringbuf drops")
        check("overhead" in txt.lower(), "Metrics panel shows overhead estimate")
        check("Budget" in txt, "Metrics panel shows budget remaining")


# ── Main ─────────────────────────────────────────────────────────────────────

def main():
    print("=== test_web_ui ===")

    # Start mock server
    mock_proc = start_mock_server()

    try:
        with sync_playwright() as p:
            browser = p.chromium.launch(headless=True)
            context = browser.new_context(viewport={"width": 1280, "height": 900})

            # Mock server runs HTTP on MOCK_PORT and WS on MOCK_PORT+1; the
            # page URL's `?ws=` override (see app_url) points the client's
            # WebSocket there — no monkey-patch.

            # Capture unhandled promise rejections as console errors
            context.add_init_script(UNHANDLED_REJECTION_HOOK)

            page = context.new_page()
            guard = ConsoleErrorGuard(page)
            set_failure_capture(page, guard)

            # Every test is followed by a console-error assertion: any
            # console error / pageerror / unhandled rejection produced
            # while it ran fails that test.
            tests = [
                test_page_load,
                test_tabs,
                test_summary_bar,
                test_overview_table,
                test_events_table,
                test_column_sorting,
                test_drill_down,
                test_breadcrumb_navigation,
                test_sessions_table,
                test_queries_table,
                test_histogram_tab,
                test_timeline_tab,
                test_transitions_tab,
                test_time_picker,
                test_zoom_out,
                test_auto_refresh,
                test_chart_rendering,
                test_concurrency_tab,
                test_legend_hover_survives_tick,
                # Track U Phase U2b: uPlot default renderer
                test_uplot_legend,
                test_uplot_gestures,
                test_session_drill_to_timeline,
                test_query_drill,
                # Sprint 5.3: Exact data display tests
                test_exact_summary_values,
                test_exact_event_values,
                test_exact_session_values,
                test_exact_query_values,
                test_timeline_bar_positions,
                # Sprint 5.4: Regression tests
                test_no_double_refresh,
                test_filter_persists_across_tabs,
                # Trust Milestone T6: transport trust
                test_degraded_transport,
                test_custom_range_utc_and_aas_empty_state,
                test_reconnect_idempotent_no_leak,
                # Track U Phase U0: error visibility (P1)
                test_command_error_card,
            ]
            for fn in tests:
                fn(page)
                assert_no_console_errors(
                    page, guard, fn.__name__,
                    expect_pgwt=getattr(fn, "expect_pgwt", False))

            # ── Phase B5: fidelity-aware UI ───────────────────────────────────
            # A second mock in sampled fidelity with the daemon control command
            # enabled, in its own browser context (own WS-port redirect), so the
            # escalate flow / sampled shading / unavailable panels / metrics
            # panel are driven without disturbing the primary suite.
            b5_proc = start_mock_server(
                extra_env={"PGWT_MOCK_FIDELITY": "sampled",
                           "PGWT_MOCK_DAEMON": "1",
                           "PGWT_MOCK_TIER": "sampled",
                           "PGWT_MOCK_BUDGET_S": "300"},
                port=B5_PORT)
            try:
                # (B5_URL's ?ws= override reaches the B5 mock's WS port.)
                b5_ctx = browser.new_context(viewport={"width": 1280, "height": 900})
                b5_ctx.add_init_script(UNHANDLED_REJECTION_HOOK)
                b5_page = b5_ctx.new_page()
                b5_guard = ConsoleErrorGuard(b5_page)
                set_failure_capture(b5_page, b5_guard)
                b5_tests = [
                    test_b5_sampled_shading,
                    test_b5_unavailable_panel,
                    test_b5_escalate_flow,
                    test_b5_metrics_panel,
                ]
                for fn in b5_tests:
                    fn(b5_page)
                    assert_no_console_errors(b5_page, b5_guard, fn.__name__)
                b5_ctx.close()
            finally:
                set_failure_capture(page, guard)
                stop_mock_server(b5_proc)

            # Reconnection test (kills/restarts mock server) — connection
            # failures during the outage are expected, everything else fails.
            mock_proc = test_reconnection(page, mock_proc)
            assert_no_console_errors(
                page, guard, "test_reconnection",
                allow=("WebSocket", "disconnected", "not connected",
                       "Failed to load resource", "net::ERR"))

            browser.close()
    finally:
        stop_mock_server(mock_proc)

    print(f"\n{tests_passed}/{tests_run} tests passed")
    sys.exit(0 if tests_failed == 0 else 1)


if __name__ == "__main__":
    main()
