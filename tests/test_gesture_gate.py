#!/usr/bin/env python3
"""test_gesture_gate.py -- the U2b trigger-1 gesture-to-paint measurement.

THE WRITTEN GATE (pre-registered in docs/INSTRUMENT_ARCHITECTURE.md §5,
trigger 1; docs/ROADMAP_AND_STATUS.md Phase U2b):

    p95 gesture-to-paint <= 16.7 ms         PASS (green — 60 fps bar holds)
    16.7 ms < p95 <= 33 ms                  AMBER (timeboxed <=3 day tuning
                                            pass, then schedule path (b))
    p95 > 33 ms                             RED  (begin path (b) immediately)

measured at instrument-grade preload: >= 1200 cached strip buckets x
16-18 series, in a REAL browser (not the Node-SSR floor, which is
pre-registered at 20.4/32.8 ms p50/p95 for 1200x18, action-only, §4a).

This is a MEASUREMENT HARNESS, not a pass/fail CI test: it is skipped unless
PGWT_RUN_GESTURE_GATE=1, and a RED verdict does NOT fail the run — the number
is the deliverable (it decides U2b scheduling). What DOES fail the run is the
mechanics: steps must actually dispatch, 'datazoom' actions and paint events
must be observed for every step, and the cached strip must reach the target
scale — a measurement of the wrong thing is worse than no measurement.

WHAT IT DRIVES: the real app (web/static/) against tests/mock_server.py,
exactly like test_web_ui.py, with one harness-owned addition — the mock
returns a FIXED 60 canned AAS buckets regardless of the requested `buckets`
count, while the real pgwt-server serves `{from,to,buckets}` at any
resolution with no cap (src/server.c:298-308, src/compute.c:358-375 — the
"tile server" the instrument model relies on). To measure at instrument-grade
scale the harness restores that real-server contract at the transport
boundary: an init-script WebSocket shim expands every `aas` response to
exactly the REQUESTED bucket count (and, in event-breakdown mode, to 16
event series), fully deterministically — pure functions of the bucket index,
no Date.now, no Math.random. Nothing else is touched: the app's camera /
strip-cache / dataZoom / settle-refine pipeline runs verbatim.

SEQUENCE
  1. boot -> connected; drill Overview "IO" row -> class filter -> the AAS
     poll and strip fetches flip to detail:'events' (16 series + the silent
     pgwt-annotations series). The drill pauses live -> camera detached.
  2. wait for the camera's quantized 3x strip (~1258 buckets at the 15 min
     boot window) to land in the strip cache and paint into the chart.
  3. 40 wheel-zoom steps at the chart center (alternating in/out, CDP input
     via Playwright mouse.wheel) + 20 shift+drag pan steps (constraint C:
     plain drag is brush-select), in sub-bursts of 10 with a settle pause
     between bursts (lets the ~150 ms settle-refine re-center the strip
     skirt, exactly as a real gesture pause would).
  4. per step: gesture-dispatch (zrender input event arrival, i.e. wheel /
     mousemove hitting the chart) -> the chart's 'finished' event, measured
     entirely with in-page performance.now(). Fallback if 'finished'
     misbehaves: 'rendered' + double-rAF (recorded per step; the JSON says
     which source was used). Secondary metric: 'datazoom' action ->
     'finished' (excludes ECharts' input throttle; comparable to the SSR
     action-only floor plus paint). Steps are spaced ~30 ms apart so the
     inside-zoom 20 ms throttle contributes ~0 — the number measures the
     pipeline, not the harness cadence.
  5. p50/p95 for zoom and pan separately + combined ->
     tests/results/gesture_gate.json + a human-readable verdict.

Usage:
    PGWT_RUN_GESTURE_GATE=1 python3 tests/test_gesture_gate.py
    PGWT_GATE_PORT=18850  # isolated mock port (HTTP; WS = port+1)

No root, no PG, no SSH — mock_server.py only, like the other web suites.
"""
import json
import math
import os
import platform
import socket
import subprocess
import sys
import time
import signal
from datetime import datetime, timezone

# ── CI-default skip: a measurement, not a gate ────────────────────────────────
if os.environ.get("PGWT_RUN_GESTURE_GATE") != "1":
    print("SKIP: gesture-gate measurement (set PGWT_RUN_GESTURE_GATE=1 to run)")
    sys.exit(0)

try:
    from playwright.sync_api import sync_playwright
    from playwright.sync_api import TimeoutError as PWTimeoutError
except ImportError:
    print("ERROR: pip install playwright && playwright install chromium",
          file=sys.stderr)
    sys.exit(1)

# ── Config ────────────────────────────────────────────────────────────────────

# Own port so the suite can run standalone next to the other web suites
# (test_web_ui 18799, B5 +10; snapshots 18820 +10; chaos has its own).
MOCK_PORT = int(os.environ.get("PGWT_GATE_PORT", "18850"))
MOCK_SCRIPT = os.path.join(os.path.dirname(os.path.abspath(__file__)), "mock_server.py")
RESULTS_PATH = os.path.join(os.path.dirname(os.path.abspath(__file__)),
                            "results", "gesture_gate.json")

MOCK_URL = (f"http://127.0.0.1:{MOCK_PORT}/"
            f"?ws=ws://127.0.0.1:{MOCK_PORT + 1}/ws")

VIEWPORT = {"width": 1280, "height": 800}

ZOOM_STEPS = 40
PAN_STEPS = 20
BURST_LEN = 10          # steps per sub-burst; settle pause between bursts
INTER_STEP_MS = 30      # > the 20 ms inside-zoom throttle: measure the
                        # pipeline, not the harness cadence
SETTLE_PAUSE_MS = 500   # > CAMERA_SETTLE_MS (150) + strip fetch + repaint

# Instrument-grade scale (the gate's pre-registered corner). The 15 min boot
# window quantizes to a 2^31 ns resolution ladder rung whose 3x strip is
# ~1258 buckets; 16 event series + the silent annotation series = 17.
STRIP_MIN_BUCKETS = 1200
SERIES_MIN = 16
SERIES_MAX = 18

GATE_GREEN_MS = 16.7
GATE_AMBER_MS = 33.0

# Pre-registered comparison floor (INSTRUMENT_ARCHITECTURE.md §4a): Node SSR
# on the vendored bundle, 1200 buckets x 18 series, action-only (no paint).
SSR_FLOOR = {"scale": "1200x18", "p50_ms": 20.4, "p95_ms": 32.8,
             "note": "Node-SSR action-only pipeline floor (no browser paint); "
                     "pre-registered in docs/INSTRUMENT_ARCHITECTURE.md §4a"}

# ── Deterministic WebSocket densifier (see module docstring) ──────────────────
#
# Installed via add_init_script BEFORE any app code runs. Wraps
# window.WebSocket; tracks outgoing `aas` requests by id; rewrites their
# responses to carry exactly the requested bucket count (the real server's
# {from,to,buckets} contract), with 16 deterministic event series in
# detail:'events' mode. Envelope fields the mock added (id, fidelity, ...)
# are preserved; error envelopes pass through untouched.
DENSIFIER_INIT = """
(() => {
  const NativeWS = window.WebSocket;
  const CLASS_KEYS = ['cpu','io','lock','lwlock','ipc','client','timeout',
                      'bufferpin','activity','extension','unknown'];
  // 16 IO events (coherent with the harness's IO-class drill); identity
  // colors come from the app's own eventColor() service.
  const EVENTS = [
    'IO:BufFileRead','IO:BufFileWrite','IO:ControlFileSyncUpdate',
    'IO:DataFileExtend','IO:DataFileFlush','IO:DataFilePrefetch',
    'IO:DataFileRead','IO:DataFileSync','IO:DataFileTruncate',
    'IO:DataFileWrite','IO:RelationMapRead','IO:SLRURead','IO:SLRUSync',
    'IO:SLRUWrite','IO:WalRead','IO:WalSync',
  ].map((name, j) => ({ name, event_id: 0x01000001 + j }));

  window.__gateWire = { transformed: 0, maxBuckets: 0, requests: [] };

  function densify(req, resp) {
    const n = Math.floor(req.buckets || 0);
    const from = +req.from, to = +req.to;
    if (!(n > 0) || !(to > from)) return null;
    const bns = (to - from) / n;
    const out = Object.assign({}, resp);
    out.bucket_ns = Math.max(1, Math.round(bns));
    const buckets = new Array(n);
    if (req.detail === 'events') {
      out.breakdown = 'events';
      out.series = EVENTS.map(e => ({ name: e.name, event_id: e.event_id }));
      for (let i = 0; i < n; i++) {
        const aas = new Array(EVENTS.length);
        for (let j = 0; j < EVENTS.length; j++) {
          // Deterministic per-(bucket, series) value: a busy stacked profile
          // with per-series phase. Pure function of (i, j) — no clock, no RNG.
          aas[j] = +(0.06 + 0.07 * (1 + Math.sin(i * 0.11 + j * 1.7)) +
                     0.02 * ((i + j) % 5)).toFixed(4);
        }
        buckets[i] = { t: from + i * bns, aas };
      }
      out.max_aas = 6.0;
    } else {
      delete out.breakdown;
      delete out.series;
      for (let i = 0; i < n; i++) {
        const b = { t: from + i * bns };
        for (let j = 0; j < CLASS_KEYS.length; j++) {
          b[CLASS_KEYS[j]] = +(0.04 + 0.05 * (1 + Math.sin(i * 0.13 + j * 2.1)) +
                               0.01 * ((i + j) % 7)).toFixed(4);
        }
        buckets[i] = b;
      }
      out.max_aas = 4.5;
    }
    out.buckets = buckets;
    const w = window.__gateWire;
    w.transformed++;
    w.maxBuckets = Math.max(w.maxBuckets, n);
    w.requests.push({ buckets: n, detail: req.detail || null });
    return out;
  }

  window.WebSocket = function (url, protocols) {
    const ws = protocols !== undefined ? new NativeWS(url, protocols)
                                       : new NativeWS(url);
    const aasReqs = new Map();
    const origSend = ws.send.bind(ws);
    ws.send = function (data) {
      try {
        const m = JSON.parse(data);
        if (m && m.cmd === 'aas') aasReqs.set(m.id, m);
      } catch (e) { /* non-JSON frame: pass through */ }
      return origSend(data);
    };
    // The transport consumes messages via addEventListener('message', ...)
    // (lib/transport.js attach()); wrap listeners so aas responses arrive
    // densified. Non-aas ids and error envelopes pass through untouched.
    const origAdd = ws.addEventListener.bind(ws);
    ws.addEventListener = function (type, listener, opts) {
      if (type !== 'message') return origAdd(type, listener, opts);
      return origAdd('message', function (ev) {
        let out = ev;
        try {
          const m = JSON.parse(ev.data);
          if (m && m.id != null && aasReqs.has(m.id)) {
            const req = aasReqs.get(m.id);
            aasReqs.delete(m.id);
            if (typeof m.error !== 'string' || m.error === '') {
              const t = densify(req, m);
              if (t) out = { data: JSON.stringify(t) };
            }
          }
        } catch (e) { /* pass through */ }
        listener.call(this, out);
      }, opts);
    };
    return ws;
  };
  window.WebSocket.prototype = NativeWS.prototype;
  for (const k of ['CONNECTING', 'OPEN', 'CLOSING', 'CLOSED']) {
    window.WebSocket[k] = NativeWS[k];
  }
})();
"""

# ── In-page gesture instrumentation ───────────────────────────────────────────
#
# Installed AFTER the dense strip is resident (the chart instance persists —
# views/active.js enter() runs once at boot). All timestamps are in-page
# performance.now(): t0 = the zrender input event reaching the chart (wheel /
# drag mousemove — includes ECharts' input throttle), t_action = the
# 'datazoom' action, paint = 'finished' (fallback 'rendered' + double-rAF).
INSTRUMENT_INSTALL = """
() => {
  const el = document.getElementById('aas-chart-container');
  const chart = echarts.getInstanceByDom(el);
  const zr = chart.getZr();
  const g = window.__gate = { cur: null };
  const now = () => performance.now();
  zr.on('mousewheel', () => {
    const c = g.cur;
    if (c && c.kind === 'zoom' && c.input == null) c.input = now();
  });
  zr.on('mousemove', () => {
    const c = g.cur;
    if (c && c.kind === 'pan' && c.input == null) c.input = now();
  });
  chart.on('datazoom', () => {
    const c = g.cur;
    if (c && c.action == null) c.action = now();
  });
  chart.on('rendered', () => {
    const c = g.cur;
    if (!c || c.action == null) return;
    if (c.rendered == null) c.rendered = now();
    requestAnimationFrame(() => requestAnimationFrame(() => {
      if (c.raf2 == null) c.raf2 = now();
    }));
  });
  chart.on('finished', () => {
    const c = g.cur;
    if (c && c.action != null && c.finished == null) c.finished = now();
  });
  window.__gateArm = (kind) => {
    g.cur = { kind, armed: now(), input: null, action: null,
              rendered: null, raf2: null, finished: null };
    return true;
  };
  window.__gateCollect = () => { const c = g.cur; g.cur = null; return c; };
  return true;
}
"""

# The camera-quantized strip as the active view sees it (same targetBuckets
# knob: min(width/4, 300)) — verifies the actual cached payload scale.
STRIP_PROBE = """
() => {
  const el = document.getElementById('aas-chart-container');
  const tb = Math.min(Math.floor(el.clientWidth / 4), 300);
  const q = window.__pgwt.camera.quantize(tb);
  const hit = window.__pgwt.stripCache.get(q);
  if (!hit) return null;
  const p = hit.payload || {};
  return {
    exact: hit.exact,
    resNs: hit.resNs,
    stripBuckets: hit.buckets,
    payloadBuckets: (p.buckets || []).length,
    payloadSeries: p.series ? p.series.length : null,
    breakdown: p.breakdown || null,
  };
}
"""

# What the chart actually holds (the ECharts pipeline cost driver).
CHART_PROBE = """
() => {
  const el = document.getElementById('aas-chart-container');
  const chart = echarts.getInstanceByDom(el);
  const opt = chart.getOption();
  const data = (opt.series || []).filter(s => s.name !== 'pgwt-annotations');
  return {
    seriesTotal: (opt.series || []).length,
    dataSeries: data.length,
    points: data.length ? data[0].data.length : 0,
  };
}
"""

# ── Mock lifecycle (mirrors test_web_ui.py) ───────────────────────────────────

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
    time.sleep(0.2)
    return proc.poll() is None


def start_mock_server():
    proc = subprocess.Popen(
        [sys.executable, MOCK_SCRIPT, "--port", str(MOCK_PORT)],
        stdout=subprocess.PIPE, stderr=subprocess.PIPE,
    )
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


# ── Mechanics assertions ──────────────────────────────────────────────────────

_failures = []


def require(cond, msg):
    """A mechanics requirement: the measurement is invalid if it fails."""
    if cond:
        print(f"  ok: {msg}")
    else:
        print(f"  MECHANICS FAILURE: {msg}")
        _failures.append(msg)


# ── Stats ─────────────────────────────────────────────────────────────────────

def percentile(sorted_vals, p):
    """Nearest-rank percentile on a pre-sorted list."""
    if not sorted_vals:
        return None
    k = max(1, math.ceil(p / 100.0 * len(sorted_vals)))
    return sorted_vals[k - 1]


def summarize(samples):
    s = sorted(samples)
    return {
        "steps": len(s),
        "p50_ms": round(percentile(s, 50), 2) if s else None,
        "p95_ms": round(percentile(s, 95), 2) if s else None,
        "mean_ms": round(sum(s) / len(s), 2) if s else None,
        "min_ms": round(s[0], 2) if s else None,
        "max_ms": round(s[-1], 2) if s else None,
    }


# ── Gesture driving ───────────────────────────────────────────────────────────

def run_step(page, kind, dispatch):
    """Arm -> dispatch one gesture -> wait for paint -> collect the record.

    Returns (record, paint_source) — paint_source 'finished' or 'raf2'
    (the documented rendered+double-rAF fallback)."""
    page.evaluate("kind => window.__gateArm(kind)", kind)
    dispatch()
    source = "finished"
    try:
        page.wait_for_function(
            "() => window.__gate.cur && window.__gate.cur.finished != null",
            timeout=1500)
    except PWTimeoutError:
        source = "raf2"
        try:
            page.wait_for_function(
                "() => window.__gate.cur && window.__gate.cur.raf2 != null",
                timeout=1500)
        except PWTimeoutError:
            pass  # collected record will show what is missing
    rec = page.evaluate("() => window.__gateCollect()")
    return rec, source


def drive_bursts(page, kind, n_steps, dispatch_for, chart_points_seen):
    """Drive n_steps gestures in sub-bursts of BURST_LEN with a settle pause
    between bursts. Returns (records, sources)."""
    records, sources = [], []
    for i in range(n_steps):
        if i > 0 and i % BURST_LEN == 0:
            # Let the ~150 ms settle-refine run: re-centers the strip skirt
            # (fresh pan/zoom room) and repaints from the cache — the same
            # pause a real gesture stream contains.
            page.wait_for_timeout(SETTLE_PAUSE_MS)
            chart_points_seen.append(page.evaluate(CHART_PROBE))
        rec, source = run_step(page, kind, dispatch_for(i))
        records.append(rec)
        sources.append(source)
        page.wait_for_timeout(INTER_STEP_MS)
    page.wait_for_timeout(SETTLE_PAUSE_MS)
    chart_points_seen.append(page.evaluate(CHART_PROBE))
    return records, sources


def main():
    print("=== gesture-to-paint gate (U2b trigger 1) ===")
    print(f"gate: p95 <= {GATE_GREEN_MS} ms green | <= {GATE_AMBER_MS} ms amber"
          f" | > {GATE_AMBER_MS} ms red")
    console_errors = []

    proc = start_mock_server()
    try:
        with sync_playwright() as p:
            browser = p.chromium.launch(headless=True)
            ctx = browser.new_context(viewport=VIEWPORT,
                                      device_scale_factor=1,
                                      reduced_motion="reduce",
                                      color_scheme="dark")
            page = ctx.new_page()
            page.on("console", lambda m: console_errors.append(m.text)
                    if m.type == "error" else None)
            page.on("pageerror", lambda e: console_errors.append(f"pageerror: {e}"))
            page.add_init_script(DENSIFIER_INIT)

            # 1. Boot + connect.
            page.goto(MOCK_URL)
            page.wait_for_selector("#status.connected", timeout=10000)
            page.wait_for_timeout(800)

            # 2. Drill Overview -> IO class: flips the AAS pipeline to event
            #    breakdown (16 series) and pauses live (camera detaches).
            page.wait_for_selector("#table-container tr.indent-1.clickable",
                                   timeout=10000)
            io_row = None
            for row in page.query_selector_all(
                    "#table-container tr.indent-1.clickable"):
                if row.inner_text().strip().startswith("IO"):
                    io_row = row
                    break
            require(io_row is not None, "Overview has a drillable IO class row")
            if io_row is None:
                raise RuntimeError("cannot drill; aborting")
            io_row.click()
            page.wait_for_function(
                "() => window.__pgwt.filters.filters.class === 'IO'",
                timeout=5000)
            page.wait_for_function(
                "() => window.__pgwt.camera.mode === 'detached'", timeout=5000)

            # 3. Wait for the dense event-mode strip to land + paint.
            page.wait_for_function(
                f"""() => {{
                    const probe = ({STRIP_PROBE})();
                    return probe && probe.exact &&
                           probe.breakdown === 'events' &&
                           probe.payloadBuckets >= {STRIP_MIN_BUCKETS};
                }}""", timeout=15000)
            page.wait_for_function(
                f"""() => {{
                    const c = ({CHART_PROBE})();
                    return c.dataSeries >= {SERIES_MIN} &&
                           c.points >= {STRIP_MIN_BUCKETS};
                }}""", timeout=10000)

            strip = page.evaluate(STRIP_PROBE)
            chart0 = page.evaluate(CHART_PROBE)
            print(f"cached strip: {strip['payloadBuckets']} buckets x "
                  f"{strip['payloadSeries']} series (exact={strip['exact']}, "
                  f"resNs=2^{int(math.log2(strip['resNs']))}); chart holds "
                  f"{chart0['points']} pts x {chart0['dataSeries']} data series "
                  f"(+{chart0['seriesTotal'] - chart0['dataSeries']} annotation)")
            require(strip["payloadBuckets"] >= STRIP_MIN_BUCKETS,
                    f"cached strip >= {STRIP_MIN_BUCKETS} buckets "
                    f"(got {strip['payloadBuckets']})")
            require(SERIES_MIN <= chart0["dataSeries"] <= SERIES_MAX,
                    f"chart data series in [{SERIES_MIN},{SERIES_MAX}] "
                    f"(got {chart0['dataSeries']})")

            # 4. Install instrumentation on the persistent chart instance.
            page.evaluate(INSTRUMENT_INSTALL)

            box = page.query_selector("#aas-chart-container").bounding_box()
            cx = box["x"] + box["width"] / 2
            cy = box["y"] + box["height"] / 2
            chart_points_seen = [chart0]

            # 5. Wheel-zoom burst: alternate in/out at the chart center.
            print(f"--- {ZOOM_STEPS} wheel-zoom steps (alternating in/out) ---")
            page.mouse.move(cx, cy)
            page.wait_for_timeout(200)

            def zoom_dispatch(i):
                delta = -120 if i % 2 == 0 else 120   # in, out, in, ...
                return lambda: page.mouse.wheel(0, delta)

            zoom_recs, zoom_sources = drive_bursts(
                page, "zoom", ZOOM_STEPS, zoom_dispatch, chart_points_seen)

            # 6. Shift+drag pan bursts (constraint C: plain drag = brush).
            print(f"--- {PAN_STEPS} shift+drag pan steps ---")

            pan_recs, pan_sources = [], []
            steps_done = 0
            while steps_done < PAN_STEPS:
                burst = min(BURST_LEN, PAN_STEPS - steps_done)
                page.mouse.move(cx, cy)
                page.keyboard.down("Shift")
                page.mouse.down()
                pos = 0
                for i in range(burst):
                    dx = 28 if i % 2 == 0 else -28    # oscillate near center
                    pos += dx
                    target_x = cx + pos

                    def pan_dispatch(_i, x=target_x):
                        return lambda: page.mouse.move(x, cy, steps=1)

                    rec, source = run_step(page, "pan", pan_dispatch(i))
                    pan_recs.append(rec)
                    pan_sources.append(source)
                    page.wait_for_timeout(INTER_STEP_MS)
                page.mouse.up()
                page.keyboard.up("Shift")
                steps_done += burst
                page.wait_for_timeout(SETTLE_PAUSE_MS)
                chart_points_seen.append(page.evaluate(CHART_PROBE))

            wire = page.evaluate("() => window.__gateWire")
            browser.close()
    finally:
        stop_mock_server(proc)

    # ── Mechanics validation ──────────────────────────────────────────────────
    print("--- mechanics ---")

    def complete(recs):
        return [r for r in recs
                if r and r["input"] is not None and r["action"] is not None
                and (r["finished"] is not None or r["raf2"] is not None)]

    zoom_ok, pan_ok = complete(zoom_recs), complete(pan_recs)
    require(len(zoom_ok) == ZOOM_STEPS,
            f"all {ZOOM_STEPS} zoom steps dispatched + datazoom + paint "
            f"observed (got {len(zoom_ok)})")
    require(len(pan_ok) == PAN_STEPS,
            f"all {PAN_STEPS} pan steps dispatched + datazoom + paint "
            f"observed (got {len(pan_ok)})")

    fallbacks = (sum(1 for s in zoom_sources if s != "finished") +
                 sum(1 for s in pan_sources if s != "finished"))
    finished_missing = sum(1 for r in zoom_ok + pan_ok if r["finished"] is None)
    paint_event = "finished" if finished_missing == 0 else \
        f"finished ({finished_missing} steps fell back to rendered+double-rAF)"
    require(fallbacks == 0 or finished_missing < len(zoom_ok + pan_ok),
            f"paint event source usable (fallback waits: {fallbacks}, "
            f"steps without 'finished': {finished_missing})")

    min_points = min(c["points"] for c in chart_points_seen)
    max_points = max(c["points"] for c in chart_points_seen)
    print(f"  chart resident points across settles: {min_points}..{max_points}")

    pgwt_errors = [e for e in console_errors if e.startswith("[pgwt]")]
    other_errors = [e for e in console_errors if not e.startswith("[pgwt]")]
    require(len(other_errors) == 0,
            f"no unexpected console errors (got {len(other_errors)}: "
            f"{other_errors[:3]})")
    if pgwt_errors:
        print(f"  note: [pgwt] error surface: {pgwt_errors[:3]}")

    # ── Numbers ───────────────────────────────────────────────────────────────
    def paint_ts(r):
        return r["finished"] if r["finished"] is not None else r["raf2"]

    zoom_g2p = [paint_ts(r) - r["input"] for r in zoom_ok]
    pan_g2p = [paint_ts(r) - r["input"] for r in pan_ok]
    zoom_a2p = [paint_ts(r) - r["action"] for r in zoom_ok]
    pan_a2p = [paint_ts(r) - r["action"] for r in pan_ok]

    zoom_stats = summarize(zoom_g2p)
    pan_stats = summarize(pan_g2p)
    combined_stats = summarize(zoom_g2p + pan_g2p)
    zoom_stats["action_to_paint"] = summarize(zoom_a2p)
    pan_stats["action_to_paint"] = summarize(pan_a2p)
    combined_stats["action_to_paint"] = summarize(zoom_a2p + pan_a2p)

    p95 = combined_stats["p95_ms"]
    if p95 is None:
        verdict, verdict_text = "INVALID", "no completed steps"
    elif p95 <= GATE_GREEN_MS:
        verdict = "GREEN"
        verdict_text = (f"p95 {p95} ms <= {GATE_GREEN_MS} ms — the 60 fps bar "
                        f"holds; path (a) stands, U2b not triggered")
    elif p95 <= GATE_AMBER_MS:
        verdict = "AMBER"
        verdict_text = (f"p95 {p95} ms in ({GATE_GREEN_MS}, {GATE_AMBER_MS}] — "
                        f"timeboxed (<=3 day) tuning pass, then schedule "
                        f"path (b) (uPlot substrate)")
    else:
        verdict = "RED"
        verdict_text = (f"p95 {p95} ms > {GATE_AMBER_MS} ms — begin path (b) "
                        f"(uPlot AAS instrument pane) immediately")

    result = {
        "generated_at": datetime.now(timezone.utc).isoformat(),
        "harness": "tests/test_gesture_gate.py",
        "gate": {"green_p95_ms": GATE_GREEN_MS, "amber_p95_ms": GATE_AMBER_MS,
                 "source": "docs/INSTRUMENT_ARCHITECTURE.md §5 trigger 1"},
        "environment": {
            "platform": platform.platform(),
            "headless_chromium": True,
            "viewport": VIEWPORT,
            "note": "in-page performance.now() timing; CPU-raster canvas "
                    "(headless, no GPU)",
        },
        "scale": {
            "strip_payload_buckets": strip["payloadBuckets"],
            "strip_series": strip["payloadSeries"],
            "chart_data_series": chart0["dataSeries"],
            "chart_series_total": chart0["seriesTotal"],
            "chart_points_min_across_settles": min_points,
            "chart_points_max_across_settles": max_points,
            "aas_wire_requests_densified": wire["transformed"],
        },
        "method": {
            "t0": "zrender input event arrival (wheel / drag mousemove)",
            "paint": paint_event,
            "inter_step_ms": INTER_STEP_MS,
            "burst_len": BURST_LEN,
            "settle_pause_ms": SETTLE_PAUSE_MS,
            "secondary": "action_to_paint = 'datazoom' action -> paint "
                         "(excludes input throttle; SSR-floor-comparable "
                         "plus paint)",
        },
        "zoom": zoom_stats,
        "pan": pan_stats,
        "combined": combined_stats,
        "verdict": verdict,
        "verdict_text": verdict_text,
        "ssr_floor_reference": SSR_FLOOR,
        "mechanics_failures": list(_failures),
    }
    os.makedirs(os.path.dirname(RESULTS_PATH), exist_ok=True)
    with open(RESULTS_PATH, "w") as f:
        json.dump(result, f, indent=2)

    # ── Report ────────────────────────────────────────────────────────────────
    print("\n=== gesture-to-paint results "
          f"({strip['payloadBuckets']} buckets x {chart0['dataSeries']} series "
          f"+ annotation) ===")
    print(f"  paint event: {paint_event}")
    print(f"  zoom     p50 {zoom_stats['p50_ms']:7.2f} ms   "
          f"p95 {zoom_stats['p95_ms']:7.2f} ms   (n={zoom_stats['steps']})")
    print(f"  pan      p50 {pan_stats['p50_ms']:7.2f} ms   "
          f"p95 {pan_stats['p95_ms']:7.2f} ms   (n={pan_stats['steps']})")
    print(f"  combined p50 {combined_stats['p50_ms']:7.2f} ms   "
          f"p95 {combined_stats['p95_ms']:7.2f} ms   (n={combined_stats['steps']})")
    a2p = combined_stats["action_to_paint"]
    print(f"  action->paint (secondary): p50 {a2p['p50_ms']} ms, "
          f"p95 {a2p['p95_ms']} ms")
    print(f"  SSR floor (pre-registered, {SSR_FLOOR['scale']} action-only): "
          f"p50 {SSR_FLOOR['p50_ms']} ms, p95 {SSR_FLOOR['p95_ms']} ms")
    print(f"\n  VERDICT: {verdict} — {verdict_text}")
    print(f"  written to {os.path.relpath(RESULTS_PATH)}")

    if _failures:
        print(f"\n{len(_failures)} MECHANICS FAILURE(S) — measurement invalid:")
        for m in _failures:
            print(f"  - {m}")
        sys.exit(1)
    sys.exit(0)


if __name__ == "__main__":
    main()
