#!/usr/bin/env python3
"""verify_viewport.py -- #415/#436 viewport-evidence verification.

Drives the REAL pgwt web client + Go bridge + pgwt-server, serving the
RETAINED demo_rehearsal_4_1792234460 trace through the replay bridge (no
synthetic rows, no mock_server.py), at the real presentation viewport
(1710x981 CSS, DPR 2 -- docs/chrome-demo-viewport-2026-09-28.md), and
measures two things BEFORE any scroll or slider interaction conceals them:

  #415 -- drills into a real captured execution (Queries -> "Executions ->"
  -> click a wait bar), then reads #waterfall-readout's bounding-box bottom
  and window.scrollY IMMEDIATELY, before anything like
  scroll_into_view_if_needed() (scripts/demo-walk-record.py:668) runs. This
  script never calls it at all.

  #436 -- opens Transitions and reads #dfg-total's text + bounding box at
  the REVIEWED simplify setting, i.e. the slider's own untouched default
  (DEFAULT_THRESHOLD = 20, web/static/views/transitions.js:44) -- the same
  state scripts/demo_walk_record_lib.py's generic tab walk leaves it in
  (it never touches #dfg-slider). Checks that the text actually names
  "Lock:relation" verbatim, not just that the code path which would name it
  exists.

Requires a pgwt web client + pgwt-server already serving the retained trace
(see the companion run log for how this script's run started them). Takes
the base URL as its one argument.

Usage:
    python3 verify_viewport.py http://localhost:PORT/
"""
import json
import sys

try:
    from playwright.sync_api import sync_playwright
except ImportError:
    print("ERROR: pip install playwright", file=sys.stderr)
    sys.exit(1)

VIEWPORT = {"width": 1710, "height": 981}
DPR = 2


def main():
    if len(sys.argv) != 2:
        print(f"usage: {sys.argv[0]} <base_url>", file=sys.stderr)
        return 2
    url = sys.argv[1]
    result = {"ok": True, "items": {}}

    with sync_playwright() as p:
        browser = p.chromium.launch()
        try:
            ctx = browser.new_context(viewport=VIEWPORT, device_scale_factor=DPR)
            page = ctx.new_page()
            errors = []
            page.on("pageerror", lambda e: errors.append(str(e)))

            page.goto(url)
            page.wait_for_selector("#status.connected", timeout=20000)

            # Fit the actual replayed capture, like demos/record-web.mjs does --
            # a fixed default range can select an empty window.
            page.click("#time-range")
            page.click('#time-picker button[data-range="0"]')
            page.wait_for_timeout(1500)

            # --- #415: Queries -> drill into a real execution -> Waterfall ---
            page.click('#tabs button[data-tab="queries"]')
            page.wait_for_selector("#table-container tbody tr", timeout=20000)
            page.wait_for_timeout(500)

            rows = page.eval_on_selector_all(
                "#table-container tbody tr.clickable",
                """(rows) => rows.map((r, i) => {
                    const cell = r.querySelector('.query-executions-action');
                    return {index: i, hasDrill: !!cell};
                })""",
            )
            drillable = [r["index"] for r in rows if r["hasDrill"]]
            if not drillable:
                result["ok"] = False
                result["items"]["415"] = {"verdict": "FAIL",
                                           "reason": "no drillable query row found"}
            else:
                # Walk candidates (like select_varying_query in
                # scripts/demo-walk-record.py) until one actually has a
                # drawable non-CPU wait bar -- some queries in this capture
                # are CPU-only and would leave the readout on its
                # "Click a bar to inspect it." placeholder, not exercising
                # real content.
                idx = None
                point = None
                for cand in drillable:
                    if page.locator("#breadcrumb .crumb-clear").count():
                        page.locator("#breadcrumb .crumb-clear").click()
                        page.wait_for_timeout(300)
                    page.click('#tabs button[data-tab="queries"]')
                    page.wait_for_selector("#table-container tbody tr", timeout=20000)
                    page.wait_for_timeout(300)
                    row = page.locator("#table-container tbody tr.clickable").nth(cand)
                    row.locator(".query-executions-action").click()
                    page.wait_for_selector("#waterfall-chart canvas", timeout=20000)
                    page.wait_for_selector("#executions-table tr", timeout=20000)
                    page.wait_for_timeout(500)
                    # Click an actual drawn non-CPU wait bar, like
                    # scripts/demo-walk-record.py:671-680 does, so the
                    # readout carries real PID/Duration content -- NOT a
                    # synthetic injected row (tests/verify_waterfall_fold.py's
                    # method, which the brief says answers a different
                    # question).
                    point = page.evaluate("""() => {
                        const el = document.querySelector('#waterfall-chart');
                        const c = echarts.getInstanceByDom(el);
                        if (!c) return null;
                        const opt = c.getOption();
                        const data = opt.series.flatMap(s => s.data || []);
                        const wait = data.find(d => Array.isArray(d) &&
                            typeof d[3] === 'string' && d[3].includes(':'));
                        if (!wait) return null;
                        const pixel = t => c.convertToPixel({gridIndex: 0}, [t, wait[2]]);
                        const xy = pixel((Number(wait[0]) + Number(wait[1])) / 2);
                        const b = el.getBoundingClientRect();
                        return {x: b.left + xy[0], y: b.top + xy[1], wait: wait[3]};
                    }""")
                    if point:
                        idx = cand
                        break
                if point:
                    page.mouse.click(point["x"], point["y"])
                    page.wait_for_timeout(300)
                else:
                    idx = drillable[0]

                # Measure NOW, before anything scrolls -- no
                # scroll_into_view_if_needed() call exists anywhere in this
                # script.
                metrics = page.evaluate("""() => {
                    const el = document.getElementById('waterfall-readout');
                    const b = el ? el.getBoundingClientRect() : null;
                    return {
                        scrollY: window.scrollY,
                        innerHeight: window.innerHeight,
                        readoutBottom: b ? b.bottom : null,
                        readoutText: el ? el.innerText.trim() : null,
                    };
                }""")
                page.screenshot(path="tests/results/viewport-evidence/415-waterfall-viewport.png")
                above_fold = (metrics["readoutBottom"] is not None
                              and metrics["readoutBottom"] <= metrics["innerHeight"] + 0.5)
                scroll_zero = metrics["scrollY"] == 0
                verdict = "PASS" if (above_fold and scroll_zero) else "FAIL"
                overflow = None
                if metrics["readoutBottom"] is not None:
                    overflow = round(metrics["readoutBottom"] - metrics["innerHeight"], 2)
                result["items"]["415"] = {
                    "verdict": verdict,
                    "scrollY": metrics["scrollY"],
                    "innerHeight": metrics["innerHeight"],
                    "readoutBottom": metrics["readoutBottom"],
                    "overflow_px": overflow,
                    "readoutText": metrics["readoutText"],
                    "clickedWait": point["wait"] if point else None,
                    "drilledRowIndex": idx,
                }
                if verdict == "FAIL":
                    result["ok"] = False

            # --- #436: Transitions, untouched (reviewed) simplify setting ---
            # Re-fit: this replay serves the trace as a live feed, so the
            # window captured by the FIRST "Fit" click (above) is stale by
            # now -- the #415 drill loop spent real wall time clicking
            # through candidate queries. Re-fitting here is what keeps this
            # measurement over the SAME full capture the rehearsal evidence
            # discusses, not whatever narrower tail is now "live".
            if page.locator("#breadcrumb .crumb-clear").count():
                page.locator("#breadcrumb .crumb-clear").click()
                page.wait_for_timeout(300)
            page.click("#time-range")
            page.click('#time-picker button[data-range="0"]')
            page.wait_for_timeout(1500)
            page.click('#tabs button[data-tab="transitions"]')
            page.wait_for_selector("#dfg-container canvas", timeout=20000)
            page.wait_for_timeout(1500)

            slider_value = page.eval_on_selector("#dfg-slider", "el => el.value")
            dfg = page.evaluate("""() => {
                const el = document.getElementById('dfg-total');
                if (!el) return null;
                const b = el.getBoundingClientRect();
                return {
                    text: el.textContent,
                    top: b.top, bottom: b.bottom, left: b.left, right: b.right,
                    innerWidth: window.innerWidth, innerHeight: window.innerHeight,
                };
            }""")
            page.screenshot(path="tests/results/viewport-evidence/436-transitions-viewport.png")
            if dfg is None:
                verdict = "FAIL"
                names_lock = False
                in_viewport = False
            else:
                names_lock = "Lock:relation" in dfg["text"]
                in_viewport = (0 <= dfg["top"] <= dfg["innerHeight"]
                               and 0 <= dfg["bottom"] <= dfg["innerHeight"]
                               and 0 <= dfg["left"] <= dfg["innerWidth"]
                               and 0 <= dfg["right"] <= dfg["innerWidth"])
                verdict = "PASS" if (names_lock and in_viewport and dfg["text"]) else "FAIL"
            result["items"]["436"] = {
                "verdict": verdict,
                "sliderValue": slider_value,
                "dfgTotalText": dfg["text"] if dfg else None,
                "bounds": dfg,
                "namesLockRelation": names_lock,
                "inViewport": in_viewport,
            }
            if verdict == "FAIL":
                # #436 failure does not block, per the brief -- still record it.
                pass

            if errors:
                result["pageErrors"] = errors

        finally:
            browser.close()

    with open("tests/results/viewport-evidence/measurements.json", "w") as f:
        json.dump(result, f, indent=2)
    print(json.dumps(result, indent=2))
    return 0 if result["ok"] else 1


if __name__ == "__main__":
    sys.exit(main())
