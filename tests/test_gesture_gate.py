#!/usr/bin/env python3
"""test_gesture_gate.py -- gesture-to-paint measurement, A/B across renderers.

THE WRITTEN GATE (pre-registered in docs/INSTRUMENT_ARCHITECTURE.md §5,
trigger 1; docs/ROADMAP_AND_STATUS.md Phase U2b):

    p95 gesture-to-paint <= 16.7 ms         PASS (green — 60 fps bar holds)
    16.7 ms < p95 <= 33 ms                  AMBER (timeboxed <=3 day tuning
                                            pass, then schedule path (b))
    p95 > 33 ms                             RED  (begin path (b) immediately)

measured at instrument-grade preload: >= 1200 cached strip buckets x
16-18 series, in a REAL browser (not the Node-SSR floor, which is
pre-registered at 20.4/32.8 ms p50/p95 for 1200x18, action-only, §4a).

HISTORY: the U2a run of this harness (2026-07-31, ECharts-only) measured
combined p95 = 61.2/59.4/67.6 ms over three runs — RED, so path (b) began:
U2b replaced the AAS pane's default renderer with uPlot (lib/uplot-aas.js;
a camera gesture is ONE u.setScale viewport transform over resident data).
This harness now measures BOTH renderers in one invocation, same mock, same
dense strip, same steps:

    uplot     the default path (no ?renderer= param — this also pins that
              uplot IS the default); gate verdict is judged on this series.
    echarts   ?renderer=echarts — the retained rollback path, re-measured as
              the A/B baseline so the two numbers come from one machine, one
              chromium, one sitting.

This is a MEASUREMENT HARNESS, not a pass/fail CI test: it is skipped unless
PGWT_RUN_GESTURE_GATE=1, and a RED verdict does NOT fail the run — the number
is the deliverable. What DOES fail the run is the mechanics: steps must
actually dispatch, camera commits and paint completions must be observed for
every step, and the cached strip must reach the target scale — a measurement
of the wrong thing is worse than no measurement.

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
strip-cache / settle-refine pipeline runs verbatim under BOTH renderers.

PER-STEP PROBES (all timestamps are in-page performance.now(); both probes
end at the same instant class — "the renderer's canvas 2D commands for the
new viewport are fully written", pre-compositor):

  echarts   t0 = zrender input event arrival (wheel / drag mousemove — the
            renderer's own DOM handler, includes ECharts' input throttle);
            action = the 'datazoom' action; paint = the chart's 'finished'
            event, which zrender fires after flushing its canvas painting
            (fallback 'rendered' + double-rAF, recorded per step).
  uplot     t0 = capture-phase DOM listener on the pane for the same input
            event (wheel / pointermove) — capture fires before the app's own
            handler on the same node, so t0 is the moment the input reaches
            the page, like the zrender hook; action = camera.onChange commit
            (the analog of 'datazoom': the gesture's state mutation);
            paint = completion of the rAF callback in which the painted x
            scale moved to the armed gesture's target. WHY THIS IS THE
            EQUIVALENT OF 'finished': the view's gesture repaint is one
            rAF-coalesced u.setScale('x', …) (views/active.js
            scheduleViewportPaint/paintViewport). uPlot 1.6.32 commits
            via queueMicrotask outside batch(): setScale returns with the
            scale UNCHANGED, and the microtask then mutates the scale and
            synchronously writes the canvas (draw hooks included) before
            returning — pending scales are invisible until that commit, so
            a wrapped rAF callback can only observe the moved x scale
            AFTER the canvas is written. The probe wraps
            window.requestAnimationFrame (resolved from the global at each
            schedule site, so a post-boot wrap is seen by the app) and
            timestamps at the first callback whose completed frame shows
            the x scale moved — guaranteed same-frame by the double-rAF
            chain queued in the camera dispatch (load-bearing, not merely
            a fallback), microseconds after the canvas write: the same
            pre-compositor instant ECharts' 'finished' reports. Fallback
            source='raf2' is recorded per step, mirroring ECharts.

SEQUENCE (per renderer, per run)
  1. boot -> connected; drill Overview "IO" row -> class filter -> the AAS
     poll and strip fetches flip to detail:'events' (16 series). The drill
     pauses live -> camera detached.
  2. wait for the camera's quantized 3x strip (~1258 buckets at the 15 min
     boot window) to land in the strip cache and paint into the chart.
  3. 40 wheel-zoom steps at the chart center (alternating in/out, CDP input
     via Playwright mouse.wheel) + 20 shift+drag pan steps (constraint C:
     plain drag is brush-select), in sub-bursts of 10 with a settle pause
     between bursts (lets the ~150 ms settle-refine re-center the strip
     skirt, exactly as a real gesture pause would). Steps are spaced ~30 ms
     apart (> ECharts' 20 ms inside-zoom throttle; kept identical for uplot
     so the cadence is a constant, not a variable).
  4. p50/p95 for zoom and pan separately + combined. PGWT_GATE_RUNS runs
     (default 3) per renderer, each in a fresh browser context.
  5. Everything -> tests/results/gesture_gate.json (the previous record is
     preserved under "history") + a human-readable A/B verdict. The gate is
     judged on the WORST uplot run's combined p95 (conservative).

Usage:
    PGWT_RUN_GESTURE_GATE=1 python3 tests/test_gesture_gate.py
    PGWT_GATE_PORT=18850  # isolated mock port (HTTP; WS = port+1)
    PGWT_GATE_RUNS=3      # runs per renderer

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

# Measured A/B: uplot first (the default path + the gate subject), then the
# retained echarts rollback path as the baseline.
RENDERERS = ("uplot", "echarts")
RUNS = int(os.environ.get("PGWT_GATE_RUNS", "3"))

ZOOM_STEPS = 40
PAN_STEPS = 20
BURST_LEN = 10          # steps per sub-burst; settle pause between bursts
INTER_STEP_MS = 30      # > the 20 ms ECharts inside-zoom throttle: measure
                        # the pipeline, not the harness cadence (identical
                        # for uplot so cadence is a constant across the A/B)
SETTLE_PAUSE_MS = 500   # > CAMERA_SETTLE_MS (150) + strip fetch + repaint

# Instrument-grade scale (the gate's pre-registered corner). The 15 min boot
# window quantizes to a 2^31 ns resolution ladder rung whose 3x strip is
# ~1258 buckets; 16 event series (+ the silent annotation series on the
# echarts path only — uplot's honesty overlays are draw-hook geometry, not a
# series).
STRIP_MIN_BUCKETS = 1200
SERIES_MIN = 16
SERIES_MAX = 18

GATE_GREEN_MS = 16.7
GATE_AMBER_MS = 33.0

# Pre-registered comparison floor (INSTRUMENT_ARCHITECTURE.md §4a): Node SSR
# on the vendored ECharts bundle, 1200 buckets x 18 series, action-only.
SSR_FLOOR = {"scale": "1200x18", "p50_ms": 20.4, "p95_ms": 32.8,
             "note": "Node-SSR action-only ECharts pipeline floor (no browser "
                     "paint); pre-registered in docs/INSTRUMENT_ARCHITECTURE.md §4a"}

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

# ── In-page gesture instrumentation: ECharts ─────────────────────────────────
#
# Installed AFTER the dense strip is resident (the chart instance persists —
# views/active.js enter() runs once at boot). All timestamps are in-page
# performance.now(): t0 = the zrender input event reaching the chart (wheel /
# drag mousemove — includes ECharts' input throttle), t_action = the
# 'datazoom' action, paint = 'finished' (fallback 'rendered' + double-rAF).
INSTRUMENT_INSTALL_ECHARTS = """
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

# ── In-page gesture instrumentation: uPlot (the camera-owned pipeline) ───────
#
# Same record shape as the ECharts probe so run_step()/complete() are shared.
# See the module docstring for the probe-equivalence argument. Notes on the
# mechanics, verified against the sources this measures:
#   - t0: capture-phase listeners fire before the app's own wheel handler on
#     #aas-chart-container / pointermove handler on window (capture precedes
#     target/bubble on the same node), so t0 is input-arrival like zr.on().
#   - action: ctx.camera is the app's Camera singleton (window.__pgwt.camera);
#     zoomAt/panByFrac fire onChange synchronously — the gesture's state
#     commit, the analog of the 'datazoom' action.
#   - paint: views/active.js scheduleViewportPaint() calls the GLOBAL
#     requestAnimationFrame at each schedule site (not a captured binding), so
#     a post-boot wrap is what the app calls. The wrapper returns the native
#     id (cancelAnimationFrame in leaveUplot stays valid). uPlot 1.6.32
#     commits via queueMicrotask outside batch(): setScale returns with the
#     scale unchanged, then the microtask mutates the scale and synchronously
#     writes the canvas (draw hooks included) in the same task. Pending
#     scales are invisible until the commit, so "callback returned AND the
#     painted x scale moved" can never precede the canvas write; the
#     double-rAF chain queued in the camera dispatch guarantees a same-frame
#     wrapped callback exists (load-bearing, not merely a fallback). The
#     scale compare is against the value captured at arm time; the paint is
#     credited only after the camera commit (c.action != null), like the
#     ECharts probe.
#   - fallback: action + double-rAF (recorded per step, source='raf2') — the
#     view's paint rAF is queued inside the camera dispatch BEFORE this one,
#     so double-rAF is a conservative (+1 frame) upper bound, mirroring the
#     ECharts 'rendered'+double-rAF fallback.
INSTRUMENT_INSTALL_UPLOT = """
() => {
  const el = document.getElementById('aas-chart-container');
  const g = window.__gate = { cur: null };
  const now = () => performance.now();
  const xScale = () => {
    const d = window.__pgwt.aasDebug();
    return d && d.xScale ? d.xScale : null;
  };
  el.addEventListener('wheel', () => {
    const c = g.cur;
    if (c && c.kind === 'zoom' && c.input == null) c.input = now();
  }, { capture: true, passive: true });
  window.addEventListener('pointermove', () => {
    const c = g.cur;
    if (c && c.kind === 'pan' && c.input == null) c.input = now();
  }, true);
  window.__pgwt.camera.onChange(() => {
    const c = g.cur;
    if (!c || c.action != null) return;
    c.action = now();
    // Fallback paint bound: the view's coalesced paint rAF was queued inside
    // this same camera dispatch (its subscriber runs before this one), so it
    // precedes these in the frame queue.
    requestAnimationFrame(() => requestAnimationFrame(() => {
      if (c.raf2 == null) c.raf2 = now();
    }));
  });
  const origRaf = window.requestAnimationFrame.bind(window);
  window.requestAnimationFrame = (cb) => origRaf((ts) => {
    try { cb(ts); } finally {
      const c = g.cur;
      if (c && c.action != null && c.finished == null) {
        const xs = xScale();
        if (xs && (!c.x0 || xs.min !== c.x0.min || xs.max !== c.x0.max)) {
          c.finished = now();
        }
      }
    }
  });
  window.__gateArm = (kind) => {
    g.cur = { kind, armed: now(), input: null, action: null,
              rendered: null, raf2: null, finished: null, x0: xScale() };
    return true;
  };
  window.__gateCollect = () => { const c = g.cur; g.cur = null; return c; };
  return true;
}
"""

# The camera-quantized strip as the active view sees it (same targetBuckets
# knob: min(width/4, 300)) — verifies the actual cached payload scale.
# Renderer-agnostic (camera + strip cache are shared modules).
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

# What the chart actually holds (the pipeline cost driver), per renderer.
# echarts: getOption() series/points, annotation series excluded from data.
# uplot: the read-only debug surface (views/active.js debug()) — bucketCount
# is alignedData[0].length, seriesNames is the full identity list; the
# honesty overlays are hook geometry, not a series, hence seriesTotal ==
# dataSeries.
CHART_PROBE_ECHARTS = """
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

CHART_PROBE_UPLOT = """
() => {
  const d = window.__pgwt.aasDebug();
  if (!d || !d.mounted) return { seriesTotal: 0, dataSeries: 0, points: 0 };
  return {
    seriesTotal: d.seriesNames.length,
    dataSeries: d.seriesNames.length,
    points: d.bucketCount,
  };
}
"""


def chart_probe(renderer):
    return CHART_PROBE_UPLOT if renderer == "uplot" else CHART_PROBE_ECHARTS


def instrument_install(renderer):
    return (INSTRUMENT_INSTALL_UPLOT if renderer == "uplot"
            else INSTRUMENT_INSTALL_ECHARTS)


def page_url(renderer):
    # uplot is measured WITHOUT a ?renderer= param: the measurement doubles as
    # the pin that uplot IS the default (asserted via aasRenderer below).
    return MOCK_URL if renderer == "uplot" else MOCK_URL + "&renderer=echarts"


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

    Returns (record, paint_source) — paint_source 'finished' (the renderer's
    primary paint probe) or 'raf2' (the documented double-rAF fallback)."""
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


def drive_bursts(page, kind, n_steps, dispatch_for, probe_js, chart_points_seen):
    """Drive n_steps gestures in sub-bursts of BURST_LEN with a settle pause
    between bursts. Returns (records, sources)."""
    records, sources = [], []
    for i in range(n_steps):
        if i > 0 and i % BURST_LEN == 0:
            # Let the ~150 ms settle-refine run: re-centers the strip skirt
            # (fresh pan/zoom room) and repaints from the cache — the same
            # pause a real gesture stream contains.
            page.wait_for_timeout(SETTLE_PAUSE_MS)
            chart_points_seen.append(page.evaluate(probe_js))
        rec, source = run_step(page, kind, dispatch_for(i))
        records.append(rec)
        sources.append(source)
        page.wait_for_timeout(INTER_STEP_MS)
    page.wait_for_timeout(SETTLE_PAUSE_MS)
    chart_points_seen.append(page.evaluate(probe_js))
    return records, sources


def measure_run(browser, renderer, run_no):
    """One full boot->drill->measure pass for `renderer`. Returns the per-run
    result dict; mechanics failures are recorded via require() with a
    renderer/run prefix."""
    tag = f"[{renderer} run {run_no}]"
    probe_js = chart_probe(renderer)
    console_errors = []

    ctx = browser.new_context(viewport=VIEWPORT,
                              device_scale_factor=1,
                              reduced_motion="reduce",
                              color_scheme="dark")
    try:
        page = ctx.new_page()
        page.on("console", lambda m: console_errors.append(m.text)
                if m.type == "error" else None)
        page.on("pageerror", lambda e: console_errors.append(f"pageerror: {e}"))
        page.add_init_script(DENSIFIER_INIT)

        # 1. Boot + connect.
        page.goto(page_url(renderer))
        page.wait_for_selector("#status.connected", timeout=10000)
        page.wait_for_timeout(800)

        # Renderer pin: the uplot pass runs WITHOUT ?renderer=, so this also
        # asserts uplot is the default; the echarts pass pins the rollback
        # seam actually engaging.
        active = page.evaluate("() => window.__pgwt.aasRenderer")
        require(active == renderer,
                f"{tag} active renderer is '{renderer}' (got '{active}'"
                + ("" if renderer != "uplot" else ", no ?renderer= param — "
                   "pins the default") + ")")

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
        require(io_row is not None,
                f"{tag} Overview has a drillable IO class row")
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
                const c = ({probe_js})();
                return c.dataSeries >= {SERIES_MIN} &&
                       c.points >= {STRIP_MIN_BUCKETS};
            }}""", timeout=10000)

        strip = page.evaluate(STRIP_PROBE)
        chart0 = page.evaluate(probe_js)
        extra = chart0["seriesTotal"] - chart0["dataSeries"]
        print(f"{tag} cached strip: {strip['payloadBuckets']} buckets x "
              f"{strip['payloadSeries']} series (exact={strip['exact']}, "
              f"resNs=2^{int(math.log2(strip['resNs']))}); chart holds "
              f"{chart0['points']} pts x {chart0['dataSeries']} data series"
              + (f" (+{extra} annotation)" if extra else
                 " (overlays = hook geometry, no annotation series)"))
        require(strip["payloadBuckets"] >= STRIP_MIN_BUCKETS,
                f"{tag} cached strip >= {STRIP_MIN_BUCKETS} buckets "
                f"(got {strip['payloadBuckets']})")
        require(SERIES_MIN <= chart0["dataSeries"] <= SERIES_MAX,
                f"{tag} chart data series in [{SERIES_MIN},{SERIES_MAX}] "
                f"(got {chart0['dataSeries']})")

        # 4. Install instrumentation on the persistent chart/view instance.
        page.evaluate(instrument_install(renderer))

        box = page.query_selector("#aas-chart-container").bounding_box()
        cx = box["x"] + box["width"] / 2
        cy = box["y"] + box["height"] / 2
        chart_points_seen = [chart0]

        # 5. Wheel-zoom burst: alternate in/out at the chart center.
        print(f"{tag} {ZOOM_STEPS} wheel-zoom steps (alternating in/out)")
        page.mouse.move(cx, cy)
        page.wait_for_timeout(200)

        def zoom_dispatch(i):
            delta = -120 if i % 2 == 0 else 120   # in, out, in, ...
            return lambda: page.mouse.wheel(0, delta)

        zoom_recs, zoom_sources = drive_bursts(
            page, "zoom", ZOOM_STEPS, zoom_dispatch, probe_js,
            chart_points_seen)

        # 6. Shift+drag pan bursts (constraint C: plain drag = brush).
        print(f"{tag} {PAN_STEPS} shift+drag pan steps")

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
            chart_points_seen.append(page.evaluate(probe_js))

        wire = page.evaluate("() => window.__gateWire")
    finally:
        ctx.close()

    # ── Mechanics validation ─────────────────────────────────────────────────
    def complete(recs):
        return [r for r in recs
                if r and r["input"] is not None and r["action"] is not None
                and (r["finished"] is not None or r["raf2"] is not None)]

    zoom_ok, pan_ok = complete(zoom_recs), complete(pan_recs)
    require(len(zoom_ok) == ZOOM_STEPS,
            f"{tag} all {ZOOM_STEPS} zoom steps dispatched + action + paint "
            f"observed (got {len(zoom_ok)})")
    require(len(pan_ok) == PAN_STEPS,
            f"{tag} all {PAN_STEPS} pan steps dispatched + action + paint "
            f"observed (got {len(pan_ok)})")

    fallbacks = (sum(1 for s in zoom_sources if s != "finished") +
                 sum(1 for s in pan_sources if s != "finished"))
    finished_missing = sum(1 for r in zoom_ok + pan_ok
                           if r["finished"] is None)
    paint_event = "finished" if finished_missing == 0 else \
        f"finished ({finished_missing} steps fell back to double-rAF)"
    require(fallbacks == 0 or finished_missing < len(zoom_ok + pan_ok),
            f"{tag} paint probe usable (fallback waits: {fallbacks}, "
            f"steps without primary paint probe: {finished_missing})")

    min_points = min(c["points"] for c in chart_points_seen)
    max_points = max(c["points"] for c in chart_points_seen)
    print(f"{tag} chart resident points across settles: "
          f"{min_points}..{max_points}")

    pgwt_errors = [e for e in console_errors if e.startswith("[pgwt]")]
    other_errors = [e for e in console_errors if not e.startswith("[pgwt]")]
    require(len(other_errors) == 0,
            f"{tag} no unexpected console errors (got {len(other_errors)}: "
            f"{other_errors[:3]})")
    if pgwt_errors:
        print(f"{tag} note: [pgwt] error surface: {pgwt_errors[:3]}")

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

    print(f"{tag} zoom p50 {zoom_stats['p50_ms']} / p95 {zoom_stats['p95_ms']}"
          f" · pan p50 {pan_stats['p50_ms']} / p95 {pan_stats['p95_ms']}"
          f" · combined p50 {combined_stats['p50_ms']} / "
          f"p95 {combined_stats['p95_ms']} ms")

    return {
        "run": run_no,
        "paint_event": paint_event,
        "scale": {
            "strip_payload_buckets": strip["payloadBuckets"],
            "strip_series": strip["payloadSeries"],
            "chart_data_series": chart0["dataSeries"],
            "chart_series_total": chart0["seriesTotal"],
            "chart_points_min_across_settles": min_points,
            "chart_points_max_across_settles": max_points,
            "aas_wire_requests_densified": wire["transformed"],
        },
        "zoom": zoom_stats,
        "pan": pan_stats,
        "combined": combined_stats,
    }


def main():
    print("=== gesture-to-paint gate (U2b A/B: uplot default vs echarts) ===")
    print(f"gate (applies to uplot): p95 <= {GATE_GREEN_MS} ms green | "
          f"<= {GATE_AMBER_MS} ms amber | > {GATE_AMBER_MS} ms red; "
          f"{RUNS} runs per renderer, judged on the worst uplot run")

    # Preserve the measurement history (the pre-swap ECharts RED record is the
    # evidence trail for why U2b happened; never overwrite it silently).
    history = []
    if os.path.exists(RESULTS_PATH):
        try:
            with open(RESULTS_PATH) as f:
                prev = json.load(f)
            history = prev.pop("history", [])
            history.append(prev)
        except (ValueError, OSError) as e:
            print(f"  note: could not read previous results for history: {e}")

    per_renderer = {}
    proc = start_mock_server()
    try:
        with sync_playwright() as p:
            browser = p.chromium.launch(headless=True)
            for renderer in RENDERERS:
                runs = []
                for run_no in range(1, RUNS + 1):
                    print(f"--- {renderer} run {run_no}/{RUNS} ---")
                    runs.append(measure_run(browser, renderer, run_no))
                per_renderer[renderer] = runs
            browser.close()
    finally:
        stop_mock_server(proc)

    # ── Verdict: the gate is judged on uplot (the default renderer),
    #    conservatively on the WORST run's combined p95 ────────────────────────
    def p95s(runs):
        return [r["combined"]["p95_ms"] for r in runs
                if r["combined"]["p95_ms"] is not None]

    uplot_p95s = p95s(per_renderer.get("uplot", []))
    echarts_p95s = p95s(per_renderer.get("echarts", []))
    worst = max(uplot_p95s) if uplot_p95s else None

    if worst is None:
        verdict, verdict_text = "INVALID", "no completed uplot steps"
    elif worst <= GATE_GREEN_MS:
        verdict = "GREEN"
        verdict_text = (f"uPlot worst-run p95 {worst} ms <= {GATE_GREEN_MS} ms "
                        f"— the 60 fps bar holds; U2b target met")
    elif worst <= GATE_AMBER_MS:
        verdict = "AMBER"
        verdict_text = (f"uPlot worst-run p95 {worst} ms in ({GATE_GREEN_MS}, "
                        f"{GATE_AMBER_MS}] — timeboxed (<=3 day) tuning pass "
                        f"on the uPlot path")
    else:
        verdict = "RED"
        verdict_text = (f"uPlot worst-run p95 {worst} ms > {GATE_AMBER_MS} ms "
                        f"— open the bespoke-canvas fallback conversation "
                        f"(INSTRUMENT_ARCHITECTURE.md §4b)")

    result = {
        "generated_at": datetime.now(timezone.utc).isoformat(),
        "harness": "tests/test_gesture_gate.py",
        "mode": "A/B renderer measurement (U2b: uplot default vs echarts "
                "rollback, one invocation, same mock/strip/steps)",
        "gate": {"green_p95_ms": GATE_GREEN_MS, "amber_p95_ms": GATE_AMBER_MS,
                 "source": "docs/INSTRUMENT_ARCHITECTURE.md §5 trigger 1",
                 "applies_to": "renderer 'uplot' (the AAS default since U2b); "
                               "judged on the worst run's combined p95"},
        "environment": {
            "platform": platform.platform(),
            "headless_chromium": True,
            "viewport": VIEWPORT,
            "note": "in-page performance.now() timing; CPU-raster canvas "
                    "(headless, no GPU)",
        },
        "method": {
            "runs_per_renderer": RUNS,
            "inter_step_ms": INTER_STEP_MS,
            "burst_len": BURST_LEN,
            "settle_pause_ms": SETTLE_PAUSE_MS,
            "probes": {
                "echarts": {
                    "t0": "zrender input event arrival (wheel / drag "
                          "mousemove)",
                    "action": "'datazoom' action",
                    "paint": "chart 'finished' (fallback 'rendered' + "
                             "double-rAF, recorded per step)",
                },
                "uplot": {
                    "t0": "capture-phase DOM input arrival on the pane "
                          "(wheel / pointermove; capture precedes the app's "
                          "own handlers)",
                    "action": "camera.onChange commit (zoomAt/panByFrac — "
                              "the gesture's state mutation, the 'datazoom' "
                              "analog)",
                    "paint": "completion of the first rAF callback whose "
                             "frame shows the x scale moved (rAF-coalesced "
                             "u.setScale in views/active.js; uPlot 1.6.32 "
                             "commits via queueMicrotask — scale mutation "
                             "and synchronous canvas write happen "
                             "atomically in the commit, and pending scales "
                             "are invisible before it, so the callback can "
                             "never precede the canvas write; the "
                             "double-rAF chain queued in the camera "
                             "dispatch guarantees a same-frame callback — "
                             "load-bearing, not merely a fallback; the "
                             "same pre-compositor instant ECharts' "
                             "'finished' reports)",
                },
            },
            "secondary": "action_to_paint = state-commit -> paint (excludes "
                         "input delivery; ECharts side is SSR-floor-"
                         "comparable plus paint)",
        },
        "renderers": {
            name: {
                "runs": runs,
                "combined_p95_ms_runs": p95s(runs),
                "combined_p50_ms_runs": [r["combined"]["p50_ms"] for r in runs],
            }
            for name, runs in per_renderer.items()
        },
        "verdict": verdict,
        "verdict_text": verdict_text,
        "ssr_floor_reference": SSR_FLOOR,
        "history": history,
        "mechanics_failures": list(_failures),
    }
    os.makedirs(os.path.dirname(RESULTS_PATH), exist_ok=True)
    with open(RESULTS_PATH, "w") as f:
        json.dump(result, f, indent=2)

    # ── Report ────────────────────────────────────────────────────────────────
    print("\n=== gesture-to-paint A/B results (per-run combined p50/p95, ms) ===")
    for name in RENDERERS:
        runs = per_renderer.get(name, [])
        if not runs:
            continue
        s0 = runs[0]["scale"]
        print(f"  {name} ({s0['strip_payload_buckets']} buckets x "
              f"{s0['chart_data_series']} series):")
        for r in runs:
            a2p = r["combined"]["action_to_paint"]
            print(f"    run {r['run']}: zoom {r['zoom']['p50_ms']}/"
                  f"{r['zoom']['p95_ms']} · pan {r['pan']['p50_ms']}/"
                  f"{r['pan']['p95_ms']} · combined {r['combined']['p50_ms']}/"
                  f"{r['combined']['p95_ms']} (action->paint "
                  f"{a2p['p50_ms']}/{a2p['p95_ms']}; paint: {r['paint_event']})")
    if uplot_p95s and echarts_p95s:
        print(f"  uplot   combined p95 across runs: {uplot_p95s} "
              f"(worst {max(uplot_p95s)})")
        print(f"  echarts combined p95 across runs: {echarts_p95s} "
              f"(worst {max(echarts_p95s)})")
    print(f"  SSR floor (pre-registered, {SSR_FLOOR['scale']} action-only, "
          f"echarts): p50 {SSR_FLOOR['p50_ms']} ms, p95 {SSR_FLOOR['p95_ms']} ms")
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
