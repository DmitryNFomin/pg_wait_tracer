#!/usr/bin/env python3
"""Local histogram blink repro against tests/mock_server.py.

Runs tests/ui_live_smoke.py's own run_tab() -- same sweep, same
blink_sweep_gate_verdict, same BLINK_THRESHOLD -- twice:

  first   : stock navigation (one visit to the tab)
  revisit : visit the tab, leave to another tab, come back, THEN measure

Usage: repro_blink.py [tab] [ticks] [scenario...]
"""
import json
import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
TESTS = os.environ["PGWT_TESTS_DIR"]
sys.path.insert(0, TESTS)

import ui_live_smoke as live_smoke       # noqa: E402
import ui_live_smoke_lib as lib          # noqa: E402
from playwright.sync_api import sync_playwright  # noqa: E402

_orig_nav = live_smoke._navigate_to_tab


def revisit_nav(page, tab_id, timeout_s):
    _orig_nav(page, tab_id, timeout_s)
    other = "events" if tab_id != "events" else "queries"
    page.click(".tab[data-tab='%s']" % other)
    page.wait_for_selector("#table-container table tbody tr",
                           timeout=timeout_s * 1000)
    return _orig_nav(page, tab_id, timeout_s)


def main():
    tab = sys.argv[1] if len(sys.argv) > 1 else "histogram"
    ticks = int(sys.argv[2]) if len(sys.argv) > 2 else 6
    scenarios = sys.argv[3:] or ["first", "revisit"]

    if os.environ.get("PGWT_BIG_MOCK"):
        live_smoke.MOCK_SCRIPT = os.path.join(HERE, "big_mock.py")
    proc = live_smoke.start_mock_server(live_smoke.MOCK_PORT)
    url = live_smoke.mock_app_url(live_smoke.MOCK_PORT)
    out = os.path.join(HERE, "out")
    results = {}
    try:
        with sync_playwright() as p:
            browser = p.chromium.launch()
            try:
                for scen in scenarios:
                    live_smoke._navigate_to_tab = (
                        revisit_nav if scen == "revisit" else _orig_nav)
                    d = os.path.join(out, scen)
                    os.makedirs(d, exist_ok=True)
                    r = live_smoke.run_tab(
                        browser, tab, url, d, ticks,
                        lib.FIRST_DATA_TIMEOUT_S, lib.BLINK_THRESHOLD,
                        viewport={"width": live_smoke.DEMO_VIEWPORT_WIDTH,
                                  "height": live_smoke.DEMO_VIEWPORT_HEIGHT},
                        device_scale_factor=live_smoke.DEMO_DEVICE_SCALE_FACTOR)
                    results[scen] = r
                    nb = r["no_blink"]
                    print("\n### %-8s %s ok=%s no_blink.ok=%s ratio=%s thr=%s"
                          % (scen, tab, r["ok"], nb["ok"], nb["ratio"],
                             nb["threshold"]))
                    for i, t in enumerate(r["blink_sweep"]["ticks"], 1):
                        print("   tick %d seq=%s ratios=%s ach=%s dims=%s %s"
                              % (i, t.get("mount_seq"),
                                 [round(x, 5) for x in t["ratios"]],
                                 t.get("achieved_offsets_ms"),
                                 t.get("panel_dims"), t.get("notes")))
                    for nm in nb.get("not_measured", []):
                        print("   not-measured:", nm)
            finally:
                browser.close()
    finally:
        live_smoke.stop_mock_server(proc)
    with open(os.path.join(out, "repro.json"), "w") as f:
        json.dump(results, f, indent=1, default=str)
    print("\nwrote", os.path.join(out, "repro.json"))


main()
