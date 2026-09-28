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

#
# Playwright is imported LAZILY, inside the functions that actually drive a
# browser -- never at module scope. The two verdict functions below
# (select_measurable, ledger_verdict) are pure, and tests/
# test_timeline_window_probe.py must be able to import this module with
# nothing but the stdlib: it is registered in tests/unit_tests.list, so it
# runs in CI's `build-and-unit` job, which has no Playwright. A module-scope
# import made that job fail on a tree BOTH local gates passed -- `make check`
# on the Mac and `make box-check` on the gate box each have Playwright
# installed, so neither could see it. test_timeline_window_probe.py now pins
# this directly (it asserts 'playwright' never enters sys.modules on import),
# which is what lets a machine that HAS Playwright catch the regression.

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
        numEvents: p.server.numEvents,
        liveActive: (() => { const b = document.getElementById('live-btn');
                             return !!b && b.classList.contains('active'); })(),
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


def _pw():
    """Import Playwright on first use and return (sync_playwright, TimeoutError).

    Called only by the browser-driving paths. A missing Playwright is a
    REFUSAL with a name, not an ImportError traceback halfway through a run --
    and, crucially, not something that happens at module import, where it
    would take the pure verdict functions and their unit test down with it."""
    try:
        from playwright.sync_api import sync_playwright
        from playwright.sync_api import TimeoutError as PWTimeoutError
    except ImportError as e:
        raise ProbeFailure(
            "Playwright is not installed, so no browser can be driven: %s "
            "(the pure verdict functions do not need it)" % e)
    return sync_playwright, PWTimeoutError


def _open_app(page, url, timeout_s):
    """Load the UI and wait for a live WebSocket. Every way this can fail --
    nothing listening, something listening that is not pgwt, a bridge that
    serves the page but never connects -- must REFUSE by name rather than
    surface as a bare Playwright timeout or, worse, let the caller proceed to
    measure an app that is not there."""
    _, PWTimeoutError = _pw()
    try:
        page.goto(url)
    except Exception as e:                       # noqa: BLE001 -- reported, not hidden
        raise ProbeFailure("cannot load %s: %s" % (url, e))
    try:
        page.wait_for_selector("#status.connected", timeout=timeout_s * 1000)
    except PWTimeoutError:
        raise ProbeFailure(
            "no #status.connected within %ds at %s -- the page is not a "
            "connected pgwt UI, so there is nothing to measure" % (timeout_s, url))


def _drill_to_timeline(page, timeout_s):
    _, PWTimeoutError = _pw()
    page.click(".tab[data-tab='sessions']")
    try:
        page.wait_for_selector("#table-container table tbody tr",
                               timeout=timeout_s * 1000)
    except PWTimeoutError:
        raise ProbeFailure(
            "the sessions table showed no rows within %ds -- there is no "
            "session to drill into, so the timeline cannot be measured "
            "(absent, not slow: refusing rather than reporting a zero)"
            % timeout_s)
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


def ledger(url, minutes, out_dir, timeout_s):
    """Demo-length per-tick ledger (issue #205 residual, #197's real question).

    Sits on the timeline tab for `minutes` of real live ticks while the trace
    grows underneath, and records ONE row per tick: the `__uiLiveTicks`
    timestamp, the timeline mount that followed it, the `xAxis.max` the chart
    was painting once it had, the trace size, and whether live mode and the
    transport were healthy at that moment.

    Tick identity is taken by INDEX into the app's own append-only
    `__uiLiveTicks` array -- `[baseline + k]`, never `[length - 1]`. That is
    the difference that matters: if this loop ever falls behind the 5 s
    cadence it lags, but it can never credit one tick's timestamp to another
    iteration, which is precisely how #205's evidence was manufactured.

    The tick hook is installed AFTER navigation, so the navigation's own
    `aas` requests are excluded by construction rather than by subtraction.
    """
    sync_playwright, PWTimeoutError = _pw()
    os.makedirs(out_dir, exist_ok=True)
    recs = []
    with sync_playwright() as p:
        browser = p.chromium.launch()
        context = browser.new_context(viewport={"width": 1280, "height": 900})
        page = context.new_page()
        _open_app(page, url, timeout_s)
        dl = time.time() + timeout_s
        while page.query_selector("#table-container table tbody tr") is None:
            if time.time() > dl:
                raise ProbeFailure("UI showed no data within %ds" % timeout_s)
            page.wait_for_timeout(200)
        _drill_to_timeline(page, timeout_s)
        page.evaluate(TICK_HOOK_JS)          # AFTER navigation, deliberately
        baseline = page.evaluate("() => window.__uiLiveTicks.length")
        if baseline != 0:
            raise ProbeFailure(
                "the tick hook was installed after navigation but already "
                "holds %d entries -- it cannot be trusted to count only live "
                "ticks" % baseline)

        expected = int(minutes * 60 / (TICK_MS / 1000.0))
        print("ledger: %d ticks expected over %s minutes" % (expected, minutes))
        for k in range(expected):
            try:
                page.wait_for_function(
                    "window.__uiLiveTicks && window.__uiLiveTicks.length >= %d" % (k + 1),
                    timeout=30000)
            except PWTimeoutError:
                raise ProbeFailure(
                    "tick %d/%d never arrived within 30s -- the live loop "
                    "stopped, which is a stall, not a slow tick"
                    % (k + 1, expected))
            tick_ts = page.evaluate("() => window.__uiLiveTicks[%d]" % k)
            mount = None
            mdl = time.time() + 30
            while time.time() < mdl:
                s = page.evaluate(SAMPLE_JS)
                if s.get("missing"):
                    raise ProbeFailure("window.%s disappeared mid-run" % s["missing"])
                m = s.get("mount")
                if m and m["id"] == "timeline" and m["at"] >= tick_ts:
                    mount = m
                    break
                page.wait_for_timeout(50)
            # Read the axis AFTER the mount that belongs to this tick, so the
            # value recorded is the one this tick painted -- not the previous
            # tick's, which is the mistake being corrected.
            s = page.evaluate(SAMPLE_JS)
            recs.append({
                "tick": k + 1, "tick_ts": tick_ts,
                "mount": mount,
                "axis_max": (s.get("axis") or {}).get("max"),
                "to": s.get("to"), "num_events": s.get("numEvents"),
                "degraded": s.get("degraded"), "live_active": s.get("liveActive"),
                "has_canvas": s.get("hasCanvas"),
            })
            if (k + 1) % 24 == 0:
                print("  ... %d/%d ticks" % (k + 1, expected))
        browser.close()

    summary = {"url": url, "minutes": minutes, "ticks": recs}
    try:
        summary["verdict"] = ledger_verdict(recs, expected)
        refused = None
    except ProbeFailure as e:
        summary["verdict"] = None
        summary["refused"] = str(e)
        refused = e
    path = os.path.join(out_dir, "summary.json")
    with open(path, "w") as fh:
        json.dump(summary, fh, indent=1)
    print("ledger: wrote %s" % path)
    if refused is not None:
        raise refused

    v = summary["verdict"]
    print("\n-- demo-length ledger --")
    print("  ticks                  : %d" % v["ticks"])
    print("  tick gap ms            : min %d median %d max %d"
          % (v["tick_gap_ms"]["min"], v["tick_gap_ms"]["median"], v["tick_gap_ms"]["max"]))
    print("  xAxis.max step ms      : min %d median %d max %d"
          % (v["axis_step_ms"]["min"], v["axis_step_ms"]["median"], v["axis_step_ms"]["max"]))
    print("  frozen axis pairs      : %d of %d"
          % (v["frozen_axis_pairs"], v["axis_pairs"]))
    print("  paint latency ms       : min %d median %d max %d"
          % (v["paint_latency_ms"]["min"], v["paint_latency_ms"]["median"],
             v["paint_latency_ms"]["max"]))
    print("  trace num_events       : %s -> %s"
          % (v["num_events_first"], v["num_events_last"]))
    print("  degraded ticks         : %s" % (v["degraded_ticks"] or "none"))
    return 0


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
    sync_playwright, PWTimeoutError = _pw()
    os.makedirs(out_dir, exist_ok=True)
    recs = []
    with sync_playwright() as p:
        browser = p.chromium.launch()
        context = browser.new_context(viewport={"width": 1280, "height": 900})
        page = context.new_page()
        _open_app(page, url, timeout_s)
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


def ledger_verdict(records, expected):
    """Pure verdict over a demo-length per-tick ledger (issue #205/#197).

    One record per live tick: the `__uiLiveTicks` timestamp, the timeline
    mount that followed it, the ECharts `xAxis.max` the chart was painting
    once it had, and the trace size at that moment.

    REFUSES (ProbeFailure) rather than scoring, whenever the ledger cannot
    answer the question it was collected for. A tick with no mount is not a
    tick to skip -- it is exactly the stall #205 alleges, and it must be
    loud. Likewise a tick whose axis could not be read: an unreadable axis is
    not a stable one.

    Returns a dict of counts/stats; the CALLER decides what is a pass, so
    this function can never quietly approve."""
    if not records:
        raise ProbeFailure("the ledger is empty -- no tick was ever recorded, "
                           "so nothing about freshness was measured")
    if len(records) < expected:
        raise ProbeFailure(
            "the ledger holds %d ticks, %d were asked for -- a partial run "
            "cannot settle a demo-length question" % (len(records), expected))

    missing_mount = [r["tick"] for r in records if not r.get("mount")]
    if missing_mount:
        raise ProbeFailure(
            "ticks %s produced no timeline mount at all -- that is a real "
            "stall, reported rather than skipped" % missing_mount)
    unreadable = [r["tick"] for r in records
                  if not r.get("axis_max") or not str(r["axis_max"]).isdigit()]
    if unreadable:
        raise ProbeFailure(
            "ticks %s had no readable xAxis.max -- an axis that cannot be "
            "read is not an axis that held still" % unreadable)
    not_live = [r["tick"] for r in records if not r.get("live_active")]
    if not_live:
        raise ProbeFailure(
            "live mode was off at ticks %s -- the window is meant to be "
            "frozen then, so those ticks say nothing" % not_live)

    axes = [int(r["axis_max"]) for r in records]
    lat = [r["mount"]["at"] - r["tick_ts"] for r in records]
    gaps = [records[i + 1]["tick_ts"] - records[i]["tick_ts"]
            for i in range(len(records) - 1)]
    steps_ms = [(axes[i + 1] - axes[i]) // 1000000 for i in range(len(axes) - 1)]
    lat_s, gap_s, step_s = sorted(lat), sorted(gaps), sorted(steps_ms)
    degraded = [r["tick"] for r in records if r.get("degraded")]
    return {
        "ticks": len(records),
        "frozen_axis_pairs": sum(1 for s in steps_ms if s == 0),
        "axis_pairs": len(steps_ms),
        "axis_step_ms": {"min": step_s[0], "median": step_s[len(step_s) // 2],
                         "max": step_s[-1]},
        "paint_latency_ms": {"min": lat_s[0], "median": lat_s[len(lat_s) // 2],
                             "max": lat_s[-1]},
        "tick_gap_ms": {"min": gap_s[0], "median": gap_s[len(gap_s) // 2],
                        "max": gap_s[-1]},
        "num_events_first": records[0].get("num_events"),
        "num_events_last": records[-1].get("num_events"),
        "degraded_ticks": degraded,
    }


def select_measurable(sent, ticks):
    """Pure: split the recorded outgoing frames into (session_timeline, info)
    and REFUSE unless there is enough of both to answer the question.

    This is the probe's blindness guard, and it is the part that decides
    whether a run is evidence at all. Every way it can be starved -- no frames
    at all, `info` ticking while the timeline never fetched, a partial run
    that captured fewer requests than the ticks asked for, a frame whose `cmd`
    is missing or unparseable -- must refuse, because in each of those cases
    the probe cannot see the window and MUST NOT report a zero as if it had.
    Tested in tests/test_timeline_window_probe.py."""
    if not isinstance(sent, list):
        raise ProbeFailure("no recorded frames at all -- the WS hook never "
                           "installed, so nothing was observed")
    tl, info, unparseable = [], [], 0
    for f in sent:
        if not isinstance(f, dict) or not isinstance(f.get("cmd"), str):
            unparseable += 1
            continue
        if f["cmd"] == "session_timeline":
            tl.append(f)
        elif f["cmd"] == "info":
            info.append(f)
    if unparseable:
        raise ProbeFailure(
            "%d recorded frame(s) had no readable `cmd` -- the recording is "
            "not trustworthy, refusing to summarise it" % unparseable)
    if not info:
        raise ProbeFailure("no `info` request observed -- the live loop never ticked")
    if len(tl) < ticks:
        raise ProbeFailure(
            "observed only %d session_timeline requests, needed %d -- "
            "the timeline tab was not fetching" % (len(tl), ticks))
    return tl, info


def frozen_pair_count(tos):
    """Pure: how many consecutive (to_ns) pairs did NOT move. The number the
    whole measurement turns on, kept separate from the printing so a test can
    feed it a frozen window and a moving one and see both answers."""
    vals = [int(v) for v in tos]
    return sum(1 for i in range(len(vals) - 1) if vals[i + 1] == vals[i])


def run(url, ticks, out_dir, timeout_s):
    sync_playwright, PWTimeoutError = _pw()
    os.makedirs(out_dir, exist_ok=True)
    with sync_playwright() as p:
        browser = p.chromium.launch()
        context = browser.new_context(viewport={"width": 1280, "height": 900})
        page = context.new_page()
        _open_app(page, url, timeout_s)
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

    tl, info = select_measurable(frames["sent"], ticks)

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
    frozen = frozen_pair_count(tos)
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
    ap.add_argument("--ledger", type=float, metavar="MINUTES",
                    help="demo-length per-tick ledger -> summary.json")
    a = ap.parse_args(argv)
    try:
        if a.ledger:
            return ledger(a.url, a.ledger, a.out, a.timeout)
        if a.emulate_smoke:
            return emulate_smoke(a.url, a.ticks, a.out, a.timeout)
        return run(a.url, a.ticks, a.out, a.timeout)
    except ProbeFailure as e:
        print("PROBE REFUSED: %s" % e, file=sys.stderr)
        return 2


if __name__ == "__main__":
    sys.exit(main())
