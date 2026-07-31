#!/usr/bin/env python3
"""test_web_ui_snapshots.py -- visual-regression snapshots for the pgwt web UI.

Phase B4 of docs/ROADMAP_AND_STATUS.md. DOM/getOption() assertions (test_web_ui.py)
prove "it renders and the data is present"; they cannot see the
"renders without errors but looks WRONG" class — a dropped series color, a
broken layout, a heatmap that lost its legend. This suite catches that by
screenshotting each view against the FIXED mock dataset and diffing against a
committed baseline PNG.

Why a hand-rolled comparator instead of Playwright's `toHaveScreenshot`:
the whole web-UI suite uses the Playwright *sync API* directly (no
@playwright/test runner, no pytest-playwright fixture), so `toHaveScreenshot`
isn't available. We reproduce its essentials — fixed viewport + device scale,
disabled animations, masked volatile regions, and a small pixel-diff tolerance
to absorb antialiasing — with `page.screenshot()` + a numpy/PIL pixel compare.

DETERMINISM (the hard part — see the plan's B4 "CRITICAL" section):
Playwright screenshots are environment-sensitive (font rendering, antialiasing,
chromium build). Baselines MUST be generated in the SAME environment that
compares them. We therefore:
  - DO NOT generate baselines locally. The CI `snapshots` job creates them with
    `--update-snapshots` and uploads them as an artifact; they are committed
    and the gating compare runs in the identical environment.
  - Pin viewport 1280x800, device_scale_factor=1, reduced_motion='reduce',
    color_scheme='dark', forced animations: 'disabled' on every screenshot.
  - The mock dataset uses a FIXED time base (mock_server.py `_TO_NS`), and the
    UI anchors its window to the server clock (`now_ns`), not wall-clock — so
    the AAS x-axis, the time-range text, and all data are deterministic. We
    still turn live mode OFF before snapshotting to remove any refresh churn,
    and mask the few inherently volatile nodes (status text, time-range button)
    as a belt-and-braces measure.
  - Tolerance: per-pixel channel threshold + a max-different-pixel-ratio
    (~0.02) so subpixel antialiasing never churns a diff, while a real color /
    layout regression (thousands of pixels) fails.

Usage:
  python3 tests/test_web_ui_snapshots.py                 # COMPARE (gating)
  python3 tests/test_web_ui_snapshots.py --update-snapshots   # write baselines
  # Regenerate/compare a SUBSET (fnmatch patterns, comma-separated) — for an
  # intentional single-baseline change, so unrelated baselines cannot churn:
  python3 tests/test_web_ui_snapshots.py --update-snapshots --only=table_queries
  PGWT_SNAP_ONLY='gallery/*' python3 tests/test_web_ui_snapshots.py

Baselines live in tests/web_snapshots/<name>.png (gallery cells under
tests/web_snapshots/gallery/ — see snap_gallery_suite).

No root, no PG, no SSH — uses mock_server.py, exactly like test_web_ui.py.
"""
import collections
import fnmatch
import os
import socket
import sys
import subprocess
import time
import signal

try:
    from playwright.sync_api import sync_playwright
except ImportError:
    # Match test_web_ui.py: a hard failure in CI (env CI set), skip locally.
    msg = "playwright not installed (pip install playwright && playwright install chromium)"
    if os.environ.get("CI"):
        print(f"ERROR: {msg}", file=sys.stderr)
        sys.exit(1)
    print(f"SKIP: {msg}")
    sys.exit(0)

try:
    from PIL import Image
    import numpy as np
except ImportError:
    msg = "Pillow + numpy required for snapshot comparison (pip install pillow numpy)"
    if os.environ.get("CI"):
        print(f"ERROR: {msg}", file=sys.stderr)
        sys.exit(1)
    print(f"SKIP: {msg}")
    sys.exit(0)


# ── Config ────────────────────────────────────────────────────────────────────

# A separate port from test_web_ui.py so the two suites can run back to back
# without colliding (test_web_ui uses 18799/+1 and B5 on +10).
MOCK_PORT = int(os.environ.get("PGWT_SNAP_PORT", "18820"))


def app_url(http_port):
    """Page URL pointing the client's WebSocket at the mock's WS port (+1).
    The client honors `?ws=<full ws url>` verbatim (app.js connect(), U0) —
    no WebSocket monkey-patch needed."""
    return f"http://127.0.0.1:{http_port}/?ws=ws://127.0.0.1:{http_port + 1}/ws"


MOCK_URL = app_url(MOCK_PORT)
# Sampled+daemon mock for the B5 fidelity-state snapshots.
SAMPLED_PORT = MOCK_PORT + 10
SAMPLED_URL = app_url(SAMPLED_PORT)

# U1 fixture gallery — static page under the mock's HTTP root (no WebSocket;
# the fixtures are fully deterministic, see web/static/dev/fixtures/).
GALLERY_URL = f"http://127.0.0.1:{MOCK_PORT}/dev/gallery.html"

# Gallery cells snapshotted at the tight GALLERY_* tolerance. Cell DOM ids are
# the manifest's stable 'gallery-<builder>-<state>' contract
# (web/static/dev/fixtures/manifest.mjs); baseline name = gallery/<builder>-<state>.
# Coverage: dense/degenerate/fidelity/i18n/injection/empty per the U1 plan.
GALLERY_STATIC_CELLS = [
    "gallery-aas-dense",                        # 300-bucket class-mode stack
    "gallery-aas-dense-events",                 # event-mode: P2 identity colors
    "gallery-aas-sampled",                      # fidelity: amber sampled band
    "gallery-aas-mixed-escalation",             # fidelity: mixed sub-ranges
    "gallery-aas-escalated-live-edge",          # fidelity: escalation edge line
    "gallery-aas-unicode-names",                # i18n event names
    "gallery-aas-empty",                        # empty state (placeholder card)
    "gallery-timeline-dense-50pids",            # dense timeline (480px cap)
    "gallery-timeline-single-point",            # degenerate timeline
    "gallery-histogram-dense",                  # dense heatmap
    "gallery-concurrency-dense-bursts",         # overlay chart + burst tables
    "gallery-table-configs-queries-hostile-sql",  # hostile SQL + event tints
]
# The recorded live-replay cell, captured at ONE fixed tick (stepped there via
# the deterministic ⏭ button — never the ▶ 1 s timer).
GALLERY_TICK_CELL = "gallery-aas-live-ticks"
GALLERY_TICK_STEP = 4


def _gallery_name(cell_id):
    """'gallery-aas-dense' -> baseline name 'gallery/aas-dense'."""
    return "gallery/" + cell_id[len("gallery-"):]

MOCK_SCRIPT = os.path.join(os.path.dirname(os.path.abspath(__file__)), "mock_server.py")
SNAP_DIR = os.path.join(os.path.dirname(os.path.abspath(__file__)), "web_snapshots")

VIEWPORT = {"width": 1280, "height": 800}
DEVICE_SCALE = 1

# Pixel-diff tolerance. A channel difference <= PIXEL_THRESHOLD (0-255) is not
# counted as a differing pixel; the snapshot passes if the fraction of
# differing pixels is <= MAX_DIFF_RATIO. These absorb antialiasing while still
# catching a real regression (a dropped series color repaints a large area).
PIXEL_THRESHOLD = 28
MAX_DIFF_RATIO = 0.02

# Gallery cells (U1) gate MUCH tighter than the 28/0.02 full-pane budget: each
# cell is a single chart at fixed CSS pixels + devicePixelRatio 1 on fully
# deterministic fixture data, so there is no layout/volatile-text slack to
# absorb. 8/0.002 lets subpixel antialiasing through while a one-series
# recolor or a dropped annotation (hundreds of pixels) fails.
GALLERY_PIXEL_THRESHOLD = 8
GALLERY_MAX_DIFF_RATIO = 0.002

UPDATE = "--update-snapshots" in sys.argv or os.environ.get("PGWT_UPDATE_SNAPSHOTS") == "1"


def _parse_only():
    """Snapshot-name filter from PGWT_SNAP_ONLY and/or --only= (comma-separated
    fnmatch patterns). Empty = run everything."""
    pats = [p.strip() for p in os.environ.get("PGWT_SNAP_ONLY", "").split(",")
            if p.strip()]
    for arg in sys.argv[1:]:
        if arg.startswith("--only="):
            pats += [p.strip() for p in arg[len("--only="):].split(",")
                     if p.strip()]
    return pats


ONLY = _parse_only()


def selected(name):
    """Does `name` participate in this run? The --only=/PGWT_SNAP_ONLY filter
    exists so an INTENTIONAL single-baseline change can be regenerated alone
    (`--update-snapshots --only=table_queries`) without silently rewriting —
    and thereby un-gating — every other baseline."""
    return not ONLY or any(fnmatch.fnmatch(name, p) for p in ONLY)


results = []  # list of (name, ok, detail)


# Volatile elements masked out of every screenshot (painted opaque so their
# content can never churn a diff). The mock is deterministic, but the status
# text and the time-range button are the two nodes most coupled to "now", so we
# mask them defensively. ECharts/table content is the actual subject and is not
# masked.
VOLATILE_SELECTORS = ["#status", "#time-range"]


# ── Mock server lifecycle (mirrors test_web_ui.py) ─────────────────────────────

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
    proc.send_signal(signal.SIGTERM)
    try:
        proc.wait(timeout=5)
    except subprocess.TimeoutExpired:
        proc.kill()
        proc.wait()


# ── Failure artifacts (Track U U0 suite hygiene) ──────────────────────────────
# On any snapshot failure: a full-page screenshot + the recent console tail
# land here (CI uploads the directory as the `snapshots-failures` artifact) —
# complementing the -actual/-diff PNGs, which only exist once a capture
# compared. Shared with the other web suites; names are prefixed per suite.
FAIL_DIR = os.path.join(os.path.dirname(os.path.abspath(__file__)),
                        "results", "web_ui_failures")

# Rolling tail of all console traffic (the two pages run sequentially, so one
# shared buffer is unambiguous).
_console_tail = collections.deque(maxlen=50)


def attach_console_tail(page):
    page.on("console", lambda m: _console_tail.append(f"[{m.type}] {m.text}"))
    page.on("pageerror", lambda e: _console_tail.append(f"[pageerror] {e}"))


def capture_failure(page, name, detail):
    try:
        os.makedirs(FAIL_DIR, exist_ok=True)
        # Gallery names contain '/' (subdir baselines) — flatten for artifacts.
        base = os.path.join(FAIL_DIR, f"snapshots_{name.replace('/', '_')}")
        page.screenshot(path=base + ".png", full_page=True)
        with open(base + "-console.txt", "w") as f:
            f.write(f"{name}: {detail}\n\n" + "\n".join(_console_tail) + "\n")
        print(f"    (failure artifacts: {base}.png / -console.txt)")
    except Exception as e:
        print(f"    (failure-artifact capture failed: {e})")


# ── Snapshot core ──────────────────────────────────────────────────────────────

def _mask_volatile(page):
    """Paint volatile nodes opaque so their text can't churn a diff."""
    page.evaluate(
        """(sels) => {
            for (const s of sels) {
                document.querySelectorAll(s).forEach(el => {
                    el.style.color = 'transparent';
                    el.style.background = '#222';
                });
            }
        }""",
        VOLATILE_SELECTORS,
    )


def snapshot(page, name, selector, pixel_threshold=PIXEL_THRESHOLD,
             max_diff_ratio=MAX_DIFF_RATIO):
    """Screenshot `selector` and compare to (or write) tests/web_snapshots/<name>.png.

    Animations are disabled for the capture. On compare, a per-channel threshold
    + max-diff-ratio decides pass/fail; on a mismatch the actual + diff PNGs are
    written next to the baseline (…-actual.png / …-diff.png) for PR review.
    `name` may contain '/' (gallery baselines live in a subdirectory);
    per-snapshot thresholds let the gallery cells gate tighter than the panes.
    """
    if not selected(name):
        return
    os.makedirs(os.path.join(SNAP_DIR, os.path.dirname(name)), exist_ok=True)
    el = page.query_selector(selector)
    if el is None:
        results.append((name, False, f"selector not found: {selector}"))
        print(f"  FAIL: {name} (selector {selector} not found)")
        capture_failure(page, name, f"selector not found: {selector}")
        return

    _mask_volatile(page)
    # Settle layout/fonts before capture.
    page.wait_for_timeout(300)
    png = el.screenshot(animations="disabled")

    baseline_path = os.path.join(SNAP_DIR, f"{name}.png")

    if UPDATE:
        with open(baseline_path, "wb") as f:
            f.write(png)
        results.append((name, True, "baseline written"))
        print(f"  WROTE: {name}.png ({len(png)} bytes)")
        return

    if not os.path.exists(baseline_path):
        results.append((name, False, "no baseline (run --update-snapshots in CI)"))
        print(f"  FAIL: {name} (no baseline committed)")
        # Still write the actual so the missing baseline can be inspected.
        with open(os.path.join(SNAP_DIR, f"{name}-actual.png"), "wb") as f:
            f.write(png)
        return

    ok, detail = _compare(png, baseline_path, name, pixel_threshold,
                          max_diff_ratio)
    results.append((name, ok, detail))
    print(f"  {'PASS' if ok else 'FAIL'}: {name} ({detail})")
    if not ok:
        capture_failure(page, name, detail)


def _compare(actual_png_bytes, baseline_path, name, pixel_threshold,
             max_diff_ratio):
    import io
    actual = Image.open(io.BytesIO(actual_png_bytes)).convert("RGB")
    baseline = Image.open(baseline_path).convert("RGB")

    if actual.size != baseline.size:
        # Save the actual so the size change is reviewable.
        actual.save(os.path.join(SNAP_DIR, f"{name}-actual.png"))
        return False, f"size {actual.size} != baseline {baseline.size}"

    a = np.asarray(actual, dtype=np.int16)
    b = np.asarray(baseline, dtype=np.int16)
    # Max per-channel absolute difference per pixel.
    chan_diff = np.abs(a - b).max(axis=2)
    differing = chan_diff > pixel_threshold
    n_diff = int(differing.sum())
    total = differing.size
    ratio = n_diff / total if total else 0.0

    if ratio > max_diff_ratio:
        actual.save(os.path.join(SNAP_DIR, f"{name}-actual.png"))
        # A red diff mask over a dimmed baseline for quick PR triage.
        diff_img = (b // 3).astype(np.uint8)
        diff_img[differing] = [255, 0, 0]
        Image.fromarray(diff_img).save(os.path.join(SNAP_DIR, f"{name}-diff.png"))
        return False, f"diff ratio {ratio:.4f} > {max_diff_ratio} ({n_diff}/{total} px)"

    return True, f"diff ratio {ratio:.4f} <= {max_diff_ratio}"


# ── Navigation helpers ─────────────────────────────────────────────────────────

def open_app(page, url):
    page.goto(url)
    page.wait_for_selector("#status.connected", timeout=10000)
    page.wait_for_timeout(1200)
    # Turn live mode OFF so no auto-refresh repaints mid-capture. The Live
    # button starts active (15m live default); one click stops the loop.
    live = page.query_selector("#live-btn")
    if live and page.evaluate("el => el.classList.contains('active')", live):
        live.click()
        page.wait_for_timeout(400)


def goto_tab(page, tab):
    page.click(f".tab[data-tab='{tab}']")
    page.wait_for_timeout(1800)


# ── Snapshot scenarios ─────────────────────────────────────────────────────────

def snap_exact_suite(page):
    """Snapshots against the default (exact, no-daemon) mock dataset."""
    print("--- Snapshots: exact mock ---")

    open_app(page, MOCK_URL)

    # Overview: AAS chart + time-model table.
    snapshot(page, "aas_chart_overview", "#aas-chart-container")
    snapshot(page, "table_overview", "#table-container")

    # Events table.
    goto_tab(page, "events")
    snapshot(page, "table_events", "#table-container")

    # Sessions table.
    goto_tab(page, "sessions")
    snapshot(page, "table_sessions", "#table-container")
    # (sessions rows are clickable -> Timeline; re-open the app to reset state
    #  in case a click happened during settle.)

    # Queries table (stacked wait-profile bars).
    open_app(page, MOCK_URL)
    goto_tab(page, "queries")
    snapshot(page, "table_queries", "#table-container")

    # Histogram heatmap.
    goto_tab(page, "histogram")
    page.wait_for_timeout(800)
    snapshot(page, "histogram_heatmap", "#heatmap-container")

    # Transitions directly-follows graph (Sankey-style).
    goto_tab(page, "transitions")
    page.wait_for_timeout(1500)
    snapshot(page, "transitions_dfg", "#dfg-container")

    # Concurrency overlay chart + burst table.
    goto_tab(page, "concurrency")
    page.wait_for_timeout(1200)
    snapshot(page, "concurrency_chart", "#concurrency-chart")
    snapshot(page, "concurrency_burst_table", "#burst-table")

    # Session timeline: drill in from Sessions.
    open_app(page, MOCK_URL)
    goto_tab(page, "sessions")
    row = page.query_selector("#table-container table tbody tr.clickable")
    if row:
        row.click()
        page.wait_for_timeout(1800)
        snapshot(page, "session_timeline", "#timeline-chart")
    elif selected("session_timeline"):
        results.append(("session_timeline", False, "no session row to drill"))
        print("  FAIL: session_timeline (no session row)")
        capture_failure(page, "session_timeline", "no session row to drill")


def snap_fidelity_suite(page):
    """B5 fidelity-state snapshots against the sampled + daemon mock."""
    print("--- Snapshots: sampled/daemon mock (B5 fidelity states) ---")

    open_app(page, SAMPLED_URL)

    # Sampled shading: the AAS chart paints an amber markArea band + chip.
    snapshot(page, "fidelity_sampled_shading", "#aas-chart-container")

    # Unavailable "escalate" panel: an EXACT-only view over a sampled window.
    goto_tab(page, "transitions")
    page.wait_for_timeout(1200)
    panel = page.query_selector(".unavailable-panel")
    if panel:
        snapshot(page, "fidelity_unavailable_panel", ".unavailable-panel")
    elif selected("fidelity_unavailable_panel"):
        results.append(("fidelity_unavailable_panel", False, "no unavailable panel"))
        print("  FAIL: fidelity_unavailable_panel (panel absent)")
        capture_failure(page, "fidelity_unavailable_panel", "no unavailable panel")

    # Daemon self-metrics panel.
    open_app(page, SAMPLED_URL)
    mbtn = page.query_selector("#metrics-btn")
    if mbtn and mbtn.is_visible():
        mbtn.click()
        page.wait_for_timeout(900)
        snapshot(page, "fidelity_metrics_panel", "#daemon-metrics")
    elif selected("fidelity_metrics_panel"):
        results.append(("fidelity_metrics_panel", False, "metrics button hidden"))
        print("  FAIL: fidelity_metrics_panel (metrics button hidden)")
        capture_failure(page, "fidelity_metrics_panel", "metrics button hidden")


def snap_gallery_suite(page):
    """U1 gallery-cell snapshots (docs/VISUALIZATION_REVIEW.md §6 item 8).

    web/static/dev/gallery.html renders every (builder × state) fixture through
    the REAL pure builders at fixed CSS pixels + devicePixelRatio 1, fully
    deterministic (no wall-clock, no randomness). Each snapshot here is ONE
    cell — chart + header + facts footer — screenshotted by its stable
    #gallery-<builder>-<state> id and gated at the tight 8/0.002 tolerance
    (GALLERY_*), so a single-series recolor or a dropped annotation fails even
    though it would drown inside a 0.02 full-pane budget. Baselines live in
    tests/web_snapshots/gallery/. Vocabulary for failures:
    docs/VISUAL_CHECKLIST.md ("<WORD> violation, cell <id>, tick N").

    The gallery is fully static (ES modules over HTTP; no WebSocket), served by
    the exact mock's static root.
    """
    tick_name = f"gallery/aas-live-ticks-tick{GALLERY_TICK_STEP}"
    wanted = [_gallery_name(c) for c in GALLERY_STATIC_CELLS] + [tick_name]
    if not any(selected(n) for n in wanted):
        return  # --only filter selects no gallery cell; skip the page load

    print("--- Snapshots: fixture gallery (U1 tight-threshold cells) ---")
    page.goto(GALLERY_URL)
    # gallery.js sets data-gallery-ready="1" after every cell has rendered.
    page.wait_for_selector("body[data-gallery-ready='1']", timeout=15000)
    page.wait_for_timeout(500)  # font/canvas settle (charts animate: false)

    for cell_id in GALLERY_STATIC_CELLS:
        snapshot(page, _gallery_name(cell_id), f"#{cell_id}",
                 pixel_threshold=GALLERY_PIXEL_THRESHOLD,
                 max_diff_ratio=GALLERY_MAX_DIFF_RATIO)

    # Tick replay at a FIXED step: click the ⏭ step button GALLERY_TICK_STEP
    # times (never the ▶ interval timer — a timer capture would race the
    # screenshot) and wait for the cell's data-tick attribute to confirm the
    # deterministic position before capturing.
    if selected(tick_name):
        step_btn = page.query_selector(
            f"#{GALLERY_TICK_CELL} .tick-bar button:nth-of-type(3)")
        if step_btn is None:
            results.append((tick_name, False, "tick step button not found"))
            print(f"  FAIL: {tick_name} (tick step button not found)")
            capture_failure(page, tick_name, "tick step button not found")
        else:
            for _ in range(GALLERY_TICK_STEP):
                step_btn.click()
            page.wait_for_selector(
                f"#{GALLERY_TICK_CELL}[data-tick='{GALLERY_TICK_STEP}']",
                timeout=5000)
            snapshot(page, tick_name, f"#{GALLERY_TICK_CELL}",
                     pixel_threshold=GALLERY_PIXEL_THRESHOLD,
                     max_diff_ratio=GALLERY_MAX_DIFF_RATIO)


# ── Main ────────────────────────────────────────────────────────────────────

def main():
    print("=== test_web_ui_snapshots ===")
    print(f"mode: {'UPDATE baselines' if UPDATE else 'COMPARE (gating)'}")
    print(f"snapshot dir: {SNAP_DIR}")
    if ONLY:
        print(f"only: {','.join(ONLY)}")

    exact_proc = start_mock_server(port=MOCK_PORT)
    sampled_proc = start_mock_server(
        extra_env={"PGWT_MOCK_FIDELITY": "sampled",
                   "PGWT_MOCK_DAEMON": "1",
                   "PGWT_MOCK_TIER": "sampled",
                   "PGWT_MOCK_BUDGET_S": "300"},
        port=SAMPLED_PORT)

    try:
        with sync_playwright() as p:
            browser = p.chromium.launch(
                headless=True,
                args=["--force-color-profile=srgb",
                      "--disable-lcd-text",
                      "--font-render-hinting=none"],
            )
            ctx_kw = dict(
                viewport=VIEWPORT,
                device_scale_factor=DEVICE_SCALE,
                reduced_motion="reduce",
                color_scheme="dark",
            )

            # The page URLs carry the ?ws= override (see app_url) — the pages
            # reach each mock's WS port without any WebSocket monkey-patch.
            exact_ctx = browser.new_context(**ctx_kw)
            exact_page = exact_ctx.new_page()
            attach_console_tail(exact_page)
            snap_exact_suite(exact_page)
            exact_ctx.close()

            sampled_ctx = browser.new_context(**ctx_kw)
            sampled_page = sampled_ctx.new_page()
            attach_console_tail(sampled_page)
            snap_fidelity_suite(sampled_page)
            sampled_ctx.close()

            gallery_ctx = browser.new_context(**ctx_kw)
            gallery_page = gallery_ctx.new_page()
            attach_console_tail(gallery_page)
            snap_gallery_suite(gallery_page)
            gallery_ctx.close()

            browser.close()
    finally:
        stop_mock_server(exact_proc)
        stop_mock_server(sampled_proc)

    n = len(results)
    passed = sum(1 for _, ok, _ in results if ok)
    print(f"\n{passed}/{n} snapshots {'written' if UPDATE else 'matched'}")
    if not UPDATE and passed != n:
        print("Failed snapshots:")
        for name, ok, detail in results:
            if not ok:
                print(f"  - {name}: {detail}")
    sys.exit(0 if passed == n else 1)


if __name__ == "__main__":
    main()
