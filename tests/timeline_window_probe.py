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

# Byte-for-byte the tick hook tests/ui_live_smoke.py installs (TICK_HOOK_JS):
# an entry per outgoing `aas` request. The smoke installs it BEFORE it
# navigates to the tab, and `--emulate-smoke` does the same, because where it
# is installed relative to the navigation is exactly what is under test.
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


def emulate_smoke(url, ticks, out_dir, timeout_s):
    """Replays tests/ui_live_smoke.py's OWN per-tick capture loop -- the
    instrument that produced #205's `tick-*.png` evidence -- while recording,
    at each capture, the app's live window and the ViewManager mount seq that
    the smoke does not save.

    What it is testing: the smoke's `_wait_for_tick(page, i)` waits for
    `window.__uiLiveTicks.length >= i`, and that array counts EVERY outgoing
    `aas` request from the moment the hook is installed -- which is BEFORE
    the tab navigation. The navigation (tab click, session drill, live-btn
    resume) issues its own `aas` requests. If it issues L of them, iterations
    1..L of the loop are satisfied with no live tick having occurred, so they
    re-capture one and the same mount.

    Reports, per iteration: the frame's md5, the mount seq, and timeRange.to.
    """
    import hashlib
    os.makedirs(out_dir, exist_ok=True)
    recs = []
    with sync_playwright() as p:
        browser = p.chromium.launch()
        context = browser.new_context(viewport={"width": 1280, "height": 900})
        page = context.new_page()
        page.goto(url)
        page.wait_for_selector("#status.connected", timeout=timeout_s * 1000)
        deadline = time.time() + timeout_s
        while page.query_selector("#table-container table tbody tr") is None:
            if time.time() > deadline:
                raise ProbeFailure("UI showed no data within %ds" % timeout_s)
            page.wait_for_timeout(200)

        # Same install point as the smoke: after first data, before navigation.
        page.evaluate(TICK_HOOK_JS)
        _drill_to_timeline(page, timeout_s)
        pre_loop_ticks = page.evaluate("() => window.__uiLiveTicks.length")
        print("  aas requests counted as 'ticks' by the navigation alone "
              "(before the loop runs): %d" % pre_loop_ticks)

        for i in range(1, ticks + 1):
            try:
                page.wait_for_function(
                    "window.__uiLiveTicks && window.__uiLiveTicks.length >= %d" % i,
                    timeout=30000)
            except PWTimeoutError:
                raise ProbeFailure("tick %d/%d did not arrive within 30s" % (i, ticks))
            tick_ts = page.evaluate(
                "window.__uiLiveTicks[window.__uiLiveTicks.length - 1]")
            # _wait_for_mount_at_or_after: the first lastMount for this tab
            # whose own `at` is >= this tick's timestamp.
            mount = None
            mdl = time.time() + 30
            while time.time() < mdl:
                m = page.evaluate("() => { const f = window.__pgwt && window.__pgwt.viewMount;"
                                  " return f ? f() : {missing: true}; }")
                if m and m.get("missing"):
                    raise ProbeFailure("window.__pgwt.viewMount() is missing")
                if m and m.get("id") == "timeline" and m.get("at") >= tick_ts:
                    mount = m
                    break
                page.wait_for_timeout(50)
            if mount is None:
                raise ProbeFailure("no timeline mount at/after tick %d within 30s" % i)

            target = mount["at"] + 200
            now_ms = page.evaluate("Date.now()")
            if target > now_ms:
                page.wait_for_timeout(target - now_ms)
            el = page.query_selector("#timeline-chart")
            if el is None:
                raise ProbeFailure("#timeline-chart vanished at tick %d" % i)
            png = el.screenshot()
            state = page.evaluate(SAMPLE_JS)
            path = os.path.join(out_dir, "tick-%d.png" % i)
            with open(path, "wb") as fh:
                fh.write(png)
            recs.append({
                "tick": i, "md5": hashlib.md5(png).hexdigest(),
                "tick_ts": tick_ts, "mount_seq": mount["seq"], "mount_at": mount["at"],
                "to": state["to"], "axis_max": (state.get("axis") or {}).get("max"),
                "wall": state["at"],
            })
        browser.close()

    with open(os.path.join(out_dir, "emulate.json"), "w") as fh:
        json.dump({"pre_loop_ticks": pre_loop_ticks, "records": recs}, fh, indent=1)

    print("\n-- smoke-shaped capture loop --")
    print("  %-5s %-10s %-12s %-22s %s" % ("iter", "mount_seq", "wall_delta", "to_ns", "md5[:8]"))
    prev = None
    for r in recs:
        dw = "" if prev is None else str(r["wall"] - prev["wall"])
        print("  %-5d %-10d %-12s %-22s %s"
              % (r["tick"], r["mount_seq"], dw, r["to"], r["md5"][:8]))
        prev = r
    dupe_runs = []
    for r in recs:
        if dupe_runs and dupe_runs[-1][0] == r["md5"]:
            dupe_runs[-1][1].append(r["tick"])
        else:
            dupe_runs.append([r["md5"], [r["tick"]]])
    print("  identical-frame runs: %s"
          % [g[1] for g in dupe_runs if len(g[1]) > 1] or "none")
    seqs = [r["mount_seq"] for r in recs]
    print("  distinct mount seqs across %d iterations: %d"
          % (len(recs), len(set(seqs))))
    print("  pre-loop aas count (L): %d -> iterations 1..%d need no live tick"
          % (pre_loop_ticks, pre_loop_ticks))
    return 0


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
    ap.add_argument("--emulate-smoke", action="store_true",
                    help="replay tests/ui_live_smoke.py's per-tick capture loop")
    a = ap.parse_args(argv)
    try:
        if a.emulate_smoke:
            return emulate_smoke(a.url, a.ticks, a.out, a.timeout)
        return run(a.url, a.ticks, a.out, a.timeout)
    except ProbeFailure as e:
        print("PROBE REFUSED: %s" % e, file=sys.stderr)
        return 2


if __name__ == "__main__":
    sys.exit(main())
