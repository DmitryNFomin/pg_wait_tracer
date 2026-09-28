#!/usr/bin/env python3
"""timeline_window_probe.py -- issue #205 MEASUREMENT (not a gate).

Question it answers, and nothing else: across N consecutive live ticks on the
timeline tab, does the tab's REQUEST window (`from`/`to` on the outgoing
`session_timeline` frame) advance, and does the PAINTED x-axis (the mounted
ECharts option's xAxis min/max) follow it?

Two different failures look identical in a screenshot diff:
  (a) the window never moved            -> request from/to frozen
  (b) the window moved, the paint didn't -> request from/to advancing while
                                            the painted axis lags or repeats
so the probe records BOTH, per tick, plus the ViewManager mount sequence
(web/static/lib/view-manager.js lastMount.seq) that says a paint actually
happened.

REFUSES rather than reports when it cannot see (each of these is a hard
failure, never a quiet zero):
  - window.__pgwt absent                    (app.js did not boot)
  - timeline mounted its prompt/no-data state, so no chart and no
    session_timeline request exists to measure
  - fewer than --ticks session_timeline requests observed
  - no `info` request observed (the live loop is not running)

Usage: timeline_window_probe.py --url http://localhost:8384/ [--ticks 8]
                                [--out DIR]
"""
import argparse
import json
import os
import sys
import time

from playwright.sync_api import sync_playwright
from playwright.sync_api import TimeoutError as PWTimeoutError

TICK_MS = 5000

# Records every outgoing request frame and every inbound response frame on the
# app's own WebSocket, in the app's own order. Installed after the WS is up.
WS_PROBE_JS = """() => {
    if (!window.__pgwt || !window.__pgwt.transport || !window.__pgwt.transport.ws) {
        return false;
    }
    window.__probe = { sent: [], recv: [] };
    const ws = window.__pgwt.transport.ws;
    const send = ws.send.bind(ws);
    ws.send = data => {
        try {
            const m = JSON.parse(data);
            window.__probe.sent.push({
                at: Date.now(), id: m.id, cmd: m.cmd,
                from: m.from == null ? null : String(m.from),
                to: m.to == null ? null : String(m.to),
            });
        } catch (e) {}
        return send(data);
    };
    ws.addEventListener('message', e => {
        try {
            const m = JSON.parse(e.data);
            if (m && m.now_ns != null) {
                window.__probe.recv.push({ at: Date.now(), id: m.id,
                                           now_ns: String(m.now_ns) });
            }
        } catch (e2) {}
    });
    return true;
}"""

# One sample: the app's live window, the last successful mount, and the axis
# the timeline chart is ACTUALLY painting right now (read off the ECharts
# instance, so a stale paint cannot hide behind a fresh request).
SAMPLE_JS = """() => {
    const p = window.__pgwt;
    if (!p) return { missing: '__pgwt' };
    const mount = p.viewMount ? p.viewMount() : null;
    let axis = null;
    try {
        const host = document.getElementById('timeline-chart');
        const ec = (window.echarts && host)
            ? window.echarts.getInstanceByDom(host) : null;
        if (ec) {
            const x = ec.getOption().xAxis[0];
            axis = { min: String(x.min), max: String(x.max) };
        }
    } catch (e) { axis = { error: String(e) }; }
    return {
        at: Date.now(),
        tab: p.activeTab,
        from: String(p.timeRange.from),
        to: String(p.timeRange.to),
        nowNs: String(p.server.nowNs),
        degraded: !!(p.degraded && p.degraded.active),
        mount: mount ? { id: mount.id, seq: mount.seq, at: mount.at } : null,
        axis: axis,
        hasCanvas: !!document.querySelector('#timeline-chart canvas'),
    };
}"""

LIVE_ACTIVE_JS = ("() => { const b = document.getElementById('live-btn'); "
                  "return !!b && b.classList.contains('active'); }")


class ProbeFailure(RuntimeError):
    pass


def _drill_to_timeline(page, timeout_s):
    page.click(".tab[data-tab='sessions']")
    page.wait_for_selector("#table-container table tbody tr", timeout=timeout_s * 1000)
    row = page.query_selector("#table-container table tbody tr.clickable")
    if row is None:
        raise ProbeFailure("no clickable session row to drill into for Timeline")
    row.click()
    try:
        page.wait_for_selector("#timeline-chart canvas", timeout=timeout_s * 1000)
    except PWTimeoutError:
        raise ProbeFailure(
            "timeline painted no chart canvas -- prompt/no-data state, "
            "nothing to measure")
    if not page.evaluate(LIVE_ACTIVE_JS):
        btn = page.query_selector("#live-btn")
        if btn is None:
            raise ProbeFailure("no #live-btn to resume live mode")
        btn.click()
        page.wait_for_timeout(500)
        if not page.evaluate(LIVE_ACTIVE_JS):
            raise ProbeFailure("clicking #live-btn did not resume live mode")


def run(url, ticks, out_dir, timeout_s):
    os.makedirs(out_dir, exist_ok=True)
    with sync_playwright() as p:
        browser = p.chromium.launch()
        context = browser.new_context(viewport={"width": 1280, "height": 900})
        page = context.new_page()
        page.goto(url)
        page.wait_for_selector("#status.connected", timeout=timeout_s * 1000)
        if page.evaluate(WS_PROBE_JS) is not True:
            raise ProbeFailure("window.__pgwt.transport.ws absent -- app.js did not boot")
        _drill_to_timeline(page, timeout_s)

        samples = []
        deadline = time.time() + (ticks + 2) * (TICK_MS / 1000.0)
        while time.time() < deadline:
            s = page.evaluate(SAMPLE_JS)
            if s.get("missing"):
                raise ProbeFailure("window.%s disappeared mid-run" % s["missing"])
            samples.append(s)
            page.wait_for_timeout(250)

        frames = page.evaluate("() => window.__probe")
        browser.close()

    sent = frames["sent"]
    tl = [f for f in sent if f["cmd"] == "session_timeline"]
    info = [f for f in sent if f["cmd"] == "info"]
    if not info:
        raise ProbeFailure("no `info` request observed -- the live loop never ticked")
    if len(tl) < ticks:
        raise ProbeFailure(
            "observed only %d session_timeline requests, needed %d -- "
            "the timeline tab was not fetching" % (len(tl), ticks))

    result = {
        "url": url, "ticks_requested": ticks,
        "session_timeline_requests": tl,
        "info_requests": info,
        "info_now_ns": frames["recv"],
        "samples": samples,
    }
    path = os.path.join(out_dir, "probe.json")
    with open(path, "w") as fh:
        json.dump(result, fh, indent=1)
    summarise(result)
    print("probe: wrote %s" % path)
    return 0


def _deltas(vals):
    return [int(vals[i + 1]) - int(vals[i]) for i in range(len(vals) - 1)]


def summarise(result):
    tl = result["session_timeline_requests"]
    print("\n-- session_timeline request window, per request --")
    print("  %-4s %-14s %-22s %s" % ("n", "wall_delta_ms", "to_ns", "to_delta_ms"))
    prev_at = prev_to = None
    for i, f in enumerate(tl):
        d_at = "" if prev_at is None else str(f["at"] - prev_at)
        d_to = "" if prev_to is None else str((int(f["to"]) - int(prev_to)) // 1000000)
        print("  %-4d %-14s %-22s %s" % (i + 1, d_at, f["to"], d_to))
        prev_at, prev_to = f["at"], f["to"]
    tos = [f["to"] for f in tl]
    dts = _deltas(tos)
    frozen = sum(1 for d in dts if d == 0)
    print("\n  request-window advances: %d of %d consecutive pairs moved; "
          "%d frozen" % (len(dts) - frozen, len(dts), frozen))

    now_ns = [r["now_ns"] for r in result["info_now_ns"]]
    if len(now_ns) > 1:
        print("  server now_ns steps (ms): %s"
              % [d // 1000000 for d in _deltas(now_ns)])

    axes = [s["axis"]["max"] for s in result["samples"]
            if s.get("axis") and s["axis"].get("max")]
    uniq = []
    for a in axes:
        if not uniq or uniq[-1] != a:
            uniq.append(a)
    print("  distinct painted xAxis.max values over the run: %d "
          "(samples: %d)" % (len(uniq), len(axes)))
    if len(uniq) > 1:
        print("  painted xAxis.max steps (ms): %s"
              % [d // 1000000 for d in _deltas(uniq)])
    seqs = sorted({s["mount"]["seq"] for s in result["samples"] if s.get("mount")})
    print("  mount seqs observed: %d distinct (%s..%s)"
          % (len(seqs), seqs[0] if seqs else "-", seqs[-1] if seqs else "-"))


def main(argv=None):
    ap = argparse.ArgumentParser()
    ap.add_argument("--url", required=True)
    ap.add_argument("--ticks", type=int, default=8)
    ap.add_argument("--out", default="tests/results/timeline_probe")
    ap.add_argument("--timeout", type=int, default=60)
    a = ap.parse_args(argv)
    try:
        return run(a.url, a.ticks, a.out, a.timeout)
    except ProbeFailure as e:
        print("PROBE REFUSED: %s" % e, file=sys.stderr)
        return 2


if __name__ == "__main__":
    sys.exit(main())
