#!/usr/bin/env python3
"""Answers the brief's first question with evidence, not a guess:

  on re-entry, is the histogram's ECharts instance REUSED and updated, or
  DISPOSED and RECREATED -- and if recreated, does that happen BEFORE or
  AFTER the ViewManager mount event the blink sweep anchors on?

Wraps echarts.init/dispose and records every call with the page clock and
the ViewManager's lastMount at that instant.
"""
import json
import os
import sys

TESTS = os.environ["PGWT_TESTS_DIR"]
sys.path.insert(0, TESTS)

import ui_live_smoke as live_smoke       # noqa: E402
import ui_live_smoke_lib as lib          # noqa: E402
from playwright.sync_api import sync_playwright  # noqa: E402

PROBE = """() => {
    window.__hmLog = [];
    const mountOf = () => {
        try { return window.__pgwt && window.__pgwt.viewMount(); }
        catch (e) { return null; }
    };
    const ec = window.echarts;
    const origInit = ec.init;
    ec.init = function (dom, theme, opts) {
        const inst = origInit.call(this, dom, theme, opts);
        window.__hmLog.push({t: Date.now(), ev: 'init', id: inst.id,
                             el: dom && dom.id, mount: mountOf()});
        const origDispose = inst.dispose.bind(inst);
        inst.dispose = function () {
            window.__hmLog.push({t: Date.now(), ev: 'dispose', id: inst.id,
                                 el: dom && dom.id, mount: mountOf()});
            return origDispose();
        };
        const origSetOption = inst.setOption.bind(inst);
        inst.setOption = function (o, n) {
            window.__hmLog.push({t: Date.now(), ev: 'setOption', id: inst.id,
                                 el: dom && dom.id, mount: mountOf()});
            return origSetOption(o, n);
        };
        return inst;
    };
    return true;
}"""


def dump(page, label):
    log = page.evaluate("() => window.__hmLog")
    mount = page.evaluate("() => window.__pgwt.viewMount()")
    print("--- %s   lastMount=%s" % (label, json.dumps(mount)))
    for e in log:
        if e.get("el") == "heatmap-container" or e.get("ev") == "dispose":
            print("    %s id=%s el=%s mountSeq=%s" % (
                e["ev"], e["id"], e.get("el"),
                (e.get("mount") or {}).get("seq")))
    page.evaluate("() => { window.__hmLog = []; }")


def main():
    if os.environ.get("PGWT_BIG_MOCK"):
        live_smoke.MOCK_SCRIPT = os.path.join(
            os.path.dirname(os.path.abspath(__file__)), "big_mock.py")
    proc = live_smoke.start_mock_server(live_smoke.MOCK_PORT)
    url = live_smoke.mock_app_url(live_smoke.MOCK_PORT)
    try:
        with sync_playwright() as p:
            browser = p.chromium.launch()
            ctx = browser.new_context(
                viewport={"width": live_smoke.DEMO_VIEWPORT_WIDTH,
                          "height": live_smoke.DEMO_VIEWPORT_HEIGHT},
                device_scale_factor=live_smoke.DEMO_DEVICE_SCALE_FACTOR)
            page = ctx.new_page()
            page.goto(url)
            page.wait_for_selector("#status.connected", timeout=30000)
            page.wait_for_selector("#table-container table tbody tr", timeout=30000)
            page.evaluate(PROBE)

            page.click(".tab[data-tab='histogram']")
            page.wait_for_selector("#heatmap-container canvas", timeout=30000)
            page.wait_for_timeout(1500)
            dump(page, "FIRST VISIT")

            page.click(".tab[data-tab='events']")
            page.wait_for_selector("#table-container table tbody tr", timeout=30000)
            page.wait_for_timeout(500)
            dump(page, "LEAVE to events")

            page.click(".tab[data-tab='histogram']")
            page.wait_for_selector("#heatmap-container canvas", timeout=30000)
            page.wait_for_timeout(1500)
            dump(page, "REVISIT")

            # Does anything paint the canvas AFTER the mount event the sweep
            # anchors on?  Same offsets as lib.SWEEP_OFFSETS_MS, read from the
            # canvas itself (no screenshot, no clip, no scroll).
            base = page.evaluate("() => window.__pgwt.viewMount()")
            shots = []
            for off in lib.SWEEP_OFFSETS_MS:
                page.wait_for_timeout(off - (shots and lib.SWEEP_OFFSETS_MS[
                    len(shots) - 1] or 0))
                shots.append(page.evaluate(
                    "() => { const c = document.querySelector("
                    "'#heatmap-container canvas'); return c ? "
                    "c.toDataURL('image/png').length + ':' + "
                    "c.toDataURL('image/png').slice(-64) : null; }"))
            after = page.evaluate("() => window.__pgwt.viewMount()")
            print("--- post-mount canvas sweep (revisit) seq %s -> %s"
                  % (base["seq"], after["seq"]))
            for off, s in zip(lib.SWEEP_OFFSETS_MS, shots):
                print("    +%4dms  %s" % (off, s))
            print("    all identical: %s" % (len(set(shots)) == 1))
            browser.close()
    finally:
        live_smoke.stop_mock_server(proc)


main()
