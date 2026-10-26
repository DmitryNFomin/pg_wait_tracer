#!/usr/bin/env python3
"""diag_537_waterfall.py -- issue #537 DIAGNOSTIC. NOT A GATE.

Measures how often `#waterfall-chart canvas` fails to become a visible,
non-zero-area canvas after navigating to the Waterfall tab on a LIVE bridge,
separately for the two navigation paths #537 names:

  hash  -- page.goto(url + '#tab=waterfall&live=1&span=900'), one canvas wait
  click -- page.goto(url), wait #status.connected, wait the Overview table's
           first real row, click .tab[data-tab='waterfall'], one canvas wait

One fixed wait budget, identical on both paths. NO retries, NO fallback, NO
widened timeout for one path only -- #537's brief forbids all three, and a
retry here would destroy the very number this script exists to produce.

Every attempt that does not produce a canvas also dumps WHY: which branch of
views/waterfall.js's mount()/renderDetail() painted instead (each writes a
distinct string), the executions row count, whether an ECharts instance
exists, the host's box, the pane-error card if any, and -- the part no
existing harness records -- the per-request WebSocket timings for this
attempt (cmd, ms, bytes, rows, error), captured by instrumenting
window.WebSocket before any app code runs. views/waterfall.js's requests()
awaits three commands SEQUENTIALLY (executions -> execution_detail ->
session_timeline) and lib/transport.js rejects any single one at 30 s, so
"the canvas never appeared" and "the third round trip had not returned yet"
are indistinguishable without these timings.

FAIL-SAFE, by design (#537: "do not trust the smoke test's success as
evidence of absence"). A sweep that cannot SEE refuses rather than reporting
zero failures:
  * an attempt that never reached #status.connected is VOID, not a failure --
    a broken harness (including this script's own WebSocket instrumentation)
    must not be scored as a product defect;
  * an attempt whose bridge was not answering at the END of it is VOID;
  * a sweep in which no attempt ever saw a single execution row is VOID --
    an empty capture renders no canvas legitimately;
  * a sweep with fewer than the requested attempts on either path is VOID;
  * a missing dependency (exit 126/127, no playwright) REFUSES with a
    non-zero exit and no verdict at all.

Usage:
    tests/diag_537_waterfall.py --url http://localhost:8384/ \
        [--attempts 10] [--phase early] [--out FILE]
"""
import argparse
import json
import os
import sys
import time
import urllib.error
import urllib.request

# One fixed budget for BOTH paths. 30000 ms is deliberate, not arbitrary: it
# is lib/transport.js's own this.timeoutMs, so a canvas that needs longer
# than this cannot be waiting on a single round trip that will still arrive.
CANVAS_TIMEOUT_MS = 30_000
CONNECT_TIMEOUT_MS = 30_000
REPRODUCE_THRESHOLD = 3     # #537's own "reproduces >= 3/10" bar

PATHS = ("hash", "click")

VIEWPORT = {"width": 1710, "height": 981}
DEVICE_SCALE_FACTOR = 2


class DiagRefusal(Exception):
    """The sweep could not run at all. Never a verdict -- always a refusal."""


# ── pure core (unit-tested in tests/test_diag_537_waterfall.py) ──────────────

def classify_attempt(attempt):
    """'pass' | 'fail' | 'void' for one attempt record.

    void BEFORE fail, always: every way the harness could have been blind is
    checked first, so a blind sweep can never report a clean 0 failures nor a
    confident 10 failures. Order matters and is asserted by the unit tests.
    """
    if not isinstance(attempt, dict):
        raise DiagRefusal("attempt record is not a dict: %r" % (attempt,))
    if attempt.get("harness_error"):
        return "void"
    if not attempt.get("connected"):
        return "void"            # never reached #status.connected
    if not attempt.get("bridge_alive_after"):
        return "void"            # bridge/daemon died under us
    snap = attempt.get("snapshot")
    if not isinstance(snap, dict):
        return "void"            # unparseable/absent diagnostic snapshot
    canvas = snap.get("canvas")
    if canvas is None:
        return "fail"            # the thing checked is ABSENT, not wrong
    if not isinstance(canvas, dict):
        return "void"            # unparseable field
    width = canvas.get("client_width")
    height = canvas.get("client_height")
    if not isinstance(width, (int, float)) or not isinstance(height, (int, float)):
        return "void"
    if width <= 0 or height <= 0:
        return "fail"            # present but zero-area: a blank tab
    return "pass"


def fail_counts(attempts, paths=PATHS):
    """{path: {'pass': n, 'fail': n, 'void': n}} over classified attempts."""
    out = {p: {"pass": 0, "fail": 0, "void": 0} for p in paths}
    for a in attempts:
        if not isinstance(a, dict):
            raise DiagRefusal("attempt record is not a dict: %r" % (a,))
        path = a.get("path")
        if path not in out:
            raise DiagRefusal("attempt names an unknown path: %r" % (path,))
        out[path][classify_attempt(a)] += 1
    return out


def sweep_verdict(attempts, attempts_per_path, paths=PATHS,
                  threshold=REPRODUCE_THRESHOLD):
    """VOID | REPRODUCED | INTERMITTENT | NOT_REPRODUCED, plus the reason.

    VOID wins over everything. In particular VOID, never NOT_REPRODUCED, for
    an empty sweep, a short sweep, a sweep with any void attempt, and a sweep
    that never once saw an execution row -- the four ways a reader could be
    told "it does not reproduce" by a run that never looked.
    """
    if attempts_per_path < 1:
        raise DiagRefusal("attempts_per_path must be >= 1")
    counts = fail_counts(attempts, paths)
    for path in paths:
        c = counts[path]
        if c["void"]:
            return "VOID", "%d void attempt(s) on the %s path" % (c["void"], path), counts
        got = c["pass"] + c["fail"]
        if got < attempts_per_path:
            return ("VOID", "only %d of %d attempts completed on the %s path"
                    % (got, attempts_per_path, path), counts)
    if not any((a.get("snapshot") or {}).get("execution_rows") for a in attempts):
        return ("VOID", "no attempt ever saw a single execution row -- an empty "
                "capture renders no canvas legitimately", counts)
    worst = max(counts[p]["fail"] for p in paths)
    if worst >= threshold:
        return "REPRODUCED", "%d/%d failures on the worst path" % (
            worst, attempts_per_path), counts
    if worst == 0:
        return "NOT_REPRODUCED", "0 failures on both paths", counts
    return "INTERMITTENT", "%d/%d failures on the worst path, below the %d bar" % (
        worst, attempts_per_path, threshold), counts


# ── browser side ─────────────────────────────────────────────────────────────

# Instruments window.WebSocket BEFORE any app module runs, so every request
# the app issues is timed. Read-only: it forwards send() and never alters a
# frame. If this ever broke the app, the page would not reach
# #status.connected and classify_attempt() would score the attempt VOID --
# the instrumentation cannot manufacture a product failure.
_WS_PROBE_JS = r"""
(() => {
  window.__diag537 = { reqs: [], console: [] };
  const Orig = window.WebSocket;
  const Patched = function (url, protocols) {
    const ws = protocols === undefined ? new Orig(url) : new Orig(url, protocols);
    const origSend = ws.send.bind(ws);
    ws.send = function (data) {
      try {
        const m = JSON.parse(data);
        if (m && m.id != null)
          window.__diag537.reqs.push({ id: m.id, cmd: m.cmd, sent: Date.now() });
      } catch (e) { /* non-JSON frame: not ours to time */ }
      return origSend(data);
    };
    ws.addEventListener('message', (e) => {
      try {
        const m = JSON.parse(e.data);
        const r = window.__diag537.reqs.find(x => x.id === m.id && x.recv == null);
        if (!r) return;
        r.recv = Date.now();
        r.ms = r.recv - r.sent;
        r.bytes = (e.data || '').length;
        r.error = typeof m.error === 'string' && m.error ? m.error : null;
        if (m && Array.isArray(m.rows)) r.rows = m.rows.length;
        if (m && m.truncated != null) r.truncated = !!m.truncated;
        if (m && m.leader && Array.isArray(m.leader.events))
          r.leader_events = m.leader.events.length;
      } catch (e) { /* ignore */ }
    });
    return ws;
  };
  Patched.prototype = Orig.prototype;
  Patched.CONNECTING = 0; Patched.OPEN = 1; Patched.CLOSING = 2; Patched.CLOSED = 3;
  window.WebSocket = Patched;
})();
"""

# Records which mount()/renderDetail() branch painted. Each of the four
# "no canvas" branches in web/static/views/waterfall.js writes a DIFFERENT
# string, so the text below names the branch without guessing.
_SNAPSHOT_JS = r"""
() => {
  const host = document.getElementById('waterfall-chart');
  const cv = host ? host.querySelector('canvas') : null;
  const inst = (host && window.echarts) ? window.echarts.getInstanceByDom(host) : null;
  let lanes = null;
  try {
    const y = inst && inst.getOption().yAxis && inst.getOption().yAxis[0];
    lanes = (y && y.data) ? y.data.length : null;
  } catch (e) { lanes = null; }
  const pane = document.querySelector('.waterfall-pane');
  const container = document.getElementById('table-container');
  const err = container ? container.querySelector('.pane-error') : null;
  const status = document.getElementById('status');
  const activeTab = document.querySelector('.tab.active');
  const trange = (window.__pgwt && window.__pgwt.timeRange) || null;
  return {
    host_present: !!host,
    host_text: host ? (host.innerText || '').slice(0, 200) : null,
    host_box: host ? { w: host.clientWidth, h: host.clientHeight } : null,
    canvas: cv ? { client_width: cv.clientWidth, client_height: cv.clientHeight,
                   attr_width: cv.width, attr_height: cv.height } : null,
    echarts_instance: !!inst,
    lanes: lanes,
    execution_rows: document.querySelectorAll('#executions-table tbody tr').length,
    execution_list_present: !!document.querySelector('.execution-list'),
    pane_display: pane ? (pane.style.display || '') : null,
    container_text: container ? (container.innerText || '').slice(0, 300) : null,
    pane_error: err ? (err.innerText || '').slice(0, 300) : null,
    status_text: status ? status.textContent : null,
    status_class: status ? status.className : null,
    active_tab: activeTab ? activeTab.dataset.tab : null,
    live_active: (() => { const b = document.getElementById('live-btn');
                          return !!b && b.classList.contains('active'); })(),
    truncation: (document.getElementById('waterfall-truncation') || {}).innerText || null,
    window_from: trange ? String(trange.from) : null,
    window_to: trange ? String(trange.to) : null,
    requests: (window.__diag537 && window.__diag537.reqs) || [],
    console: (window.__diag537 && window.__diag537.console) || [],
  };
}
"""


def bridge_alive(url, timeout=10):
    """True iff the bridge still answers <url>session with 200. The liveness
    guard: without it, a bridge that died mid-sweep makes every remaining
    attempt time out and the sweep reports a confident false reproduction."""
    probe = url.rstrip("/") + "/session"
    try:
        with urllib.request.urlopen(probe, timeout=timeout) as r:
            return r.status == 200
    except (urllib.error.URLError, OSError, ValueError):
        return False


def _waterfall_hash_url(url):
    return url.rstrip("/") + "/#tab=waterfall&live=1&span=900"


def run_attempt(browser, url, path, index, out_dir, pw_timeout_error):
    """One attempt. Returns the record classify_attempt() grades."""
    rec = {"path": path, "index": index, "connected": False,
           "bridge_alive_after": False, "snapshot": None,
           "harness_error": None, "canvas_wait_ms": None}
    ctx = None
    try:
        ctx = browser.new_context(viewport=dict(VIEWPORT),
                                  device_scale_factor=DEVICE_SCALE_FACTOR)
        page = ctx.new_page()
        page.add_init_script(_WS_PROBE_JS)
        console = []
        page.on("console", lambda m: console.append(
            {"type": m.type, "text": m.text[:300]}))
        page.on("pageerror", lambda e: console.append(
            {"type": "pageerror", "text": str(e)[:300]}))

        if path == "hash":
            page.goto(_waterfall_hash_url(url))
        else:
            page.goto(url)

        try:
            page.wait_for_selector("#status.connected", timeout=CONNECT_TIMEOUT_MS)
            rec["connected"] = True
        except pw_timeout_error:
            rec["connected"] = False

        if rec["connected"] and path == "click":
            # The live walk's own sequence (ui_live_smoke._navigate_to_tab):
            # the default Overview table's first real row, then the tab click.
            try:
                page.wait_for_selector("#table-container table tbody tr",
                                       timeout=CONNECT_TIMEOUT_MS)
            except pw_timeout_error:
                rec["harness_error"] = "overview table never rendered a row"
            if not rec["harness_error"]:
                page.click(".tab[data-tab='waterfall']")

        if rec["connected"] and not rec["harness_error"]:
            t0 = time.monotonic()
            try:
                page.wait_for_selector("#waterfall-chart canvas",
                                       timeout=CANVAS_TIMEOUT_MS)
            except pw_timeout_error:
                pass             # a real negative: snapshot below records why
            rec["canvas_wait_ms"] = int((time.monotonic() - t0) * 1000)

        snap = page.evaluate(_SNAPSHOT_JS)
        snap["console"] = console
        rec["snapshot"] = snap
        verdict = classify_attempt(dict(rec, bridge_alive_after=True))
        if verdict != "pass" and out_dir:
            shot = os.path.join(out_dir, "fail-%s-%02d.png" % (path, index))
            try:
                page.screenshot(path=shot)
                rec["screenshot"] = shot
            except Exception as exc:          # noqa: BLE001 -- best effort
                rec["screenshot_error"] = str(exc)[:200]
    except Exception as exc:                  # noqa: BLE001
        rec["harness_error"] = "%s: %s" % (type(exc).__name__, str(exc)[:300])
    finally:
        if ctx is not None:
            try:
                ctx.close()
            except Exception:                 # noqa: BLE001
                pass
    rec["bridge_alive_after"] = bridge_alive(url)
    rec["status"] = classify_attempt(rec)
    return rec


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--url", required=True, help="live bridge base URL")
    ap.add_argument("--attempts", type=int, default=10,
                    help="attempts PER PATH (default 10)")
    ap.add_argument("--phase", default="",
                    help="label recorded with the result (e.g. early/late)")
    ap.add_argument("--out", default=None)
    ap.add_argument("--out-dir", default=None,
                    help="directory for failure screenshots")
    args = ap.parse_args()

    try:
        from playwright.sync_api import sync_playwright
        from playwright.sync_api import TimeoutError as PWTimeoutError
    except ImportError as exc:
        print("REFUSE: playwright is not importable (%s) -- a missing "
              "dependency must never read as zero failures" % exc,
              file=sys.stderr)
        sys.exit(2)

    if not bridge_alive(args.url):
        print("REFUSE: %s/session did not answer 200 before the sweep started"
              % args.url.rstrip("/"), file=sys.stderr)
        sys.exit(2)

    if args.out_dir:
        os.makedirs(args.out_dir, exist_ok=True)

    attempts = []
    started = time.time()
    with sync_playwright() as p:
        browser = p.chromium.launch()
        try:
            # Interleaved hash,click,hash,click...: both paths see the same
            # capture age and the same box-load drift, so a difference between
            # them cannot be an artefact of when each ran.
            for i in range(args.attempts):
                for path in PATHS:
                    rec = run_attempt(browser, args.url, path, i, args.out_dir,
                                      PWTimeoutError)
                    attempts.append(rec)
                    snap = rec.get("snapshot") or {}
                    print("[%s] attempt %2d %-5s -> %-4s  canvas_wait=%sms "
                          "rows=%s lanes=%s echarts=%s host_text=%r"
                          % (args.phase or "-", i, path, rec["status"],
                             rec["canvas_wait_ms"], snap.get("execution_rows"),
                             snap.get("lanes"), snap.get("echarts_instance"),
                             (snap.get("host_text") or "")[:60]), flush=True)
        finally:
            browser.close()

    try:
        verdict, reason, counts = sweep_verdict(attempts, args.attempts)
    except DiagRefusal as exc:
        print("REFUSE: %s" % exc, file=sys.stderr)
        sys.exit(2)

    result = {"phase": args.phase, "url": args.url,
              "attempts_per_path": args.attempts,
              "canvas_timeout_ms": CANVAS_TIMEOUT_MS,
              "viewport": VIEWPORT, "device_scale_factor": DEVICE_SCALE_FACTOR,
              "verdict": verdict, "reason": reason, "counts": counts,
              "elapsed_s": round(time.time() - started, 1),
              "attempts": attempts}
    print(json.dumps({k: v for k, v in result.items() if k != "attempts"},
                     indent=2))
    if args.out:
        with open(args.out, "w") as f:
            json.dump(result, f, indent=2)
        print("wrote %s" % args.out)
    # Exit 0 for any REAL verdict (this is a measurement, not a gate); 2 only
    # when the sweep could not see.
    sys.exit(2 if verdict == "VOID" else 0)


if __name__ == "__main__":
    main()
