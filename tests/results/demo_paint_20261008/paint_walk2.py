#!/usr/bin/env python3
"""Click-to-painted walk, demo topology: real Chrome on the Mac -> local Go
bridge -> pgwt-server over ssh on the gate box.

Derived from the orchestrator's scratchpad `paint_walk.py` (the harness
behind the 2079 ms pin). What changed, and why:

 * `--url/--out/--label` instead of module constants, so four walks write
   four files without editing the script between them.
 * `samples` (the raw per-navigation ms) is recorded, not only min/med/max.
   The pin's own raw values lived in a scratchpad JSON nobody could cite;
   these land under tests/results/.
 * A tab that fails no longer aborts its own remaining navigations and no
   longer vanishes from the record: the error is stored WITH whatever
   samples were taken, and the worst-case verdict is computed by
   paint_analyse.py, which REFUSES on an incomplete walk instead of
   quietly taking the max over the tabs that happened to work.
 * Readiness `detail` is kept per tab (row/cell/node counts) so "it
   painted" can be distinguished from "it painted nothing".
 * The Overview time model is scraped after the walk (#295's `Idle` row and
   its children) and its row count recorded.
 * `workload_alive` is filled in by the caller after asking the box.

Unchanged on purpose: readiness is ui_live_smoke.py's own PANEL_CHECKS (not
a fixed sleep), every navigation is preceded by a full page reload so
nothing is measured warm, timeline is LAST because its drill-down sets a
pid filter, Cmd+0 resets the demo profile's persisted zoom, and the
viewport is two agreeing reads asserted against the pinned 1710x981 @ DPR 2.
"""
import argparse
import json
import pathlib
import statistics
import sys
import time

# THIS worktree's tests/, not the main checkout's. The 2026-10-07 copy
# hardcoded the main checkout, which would have imported master's
# PANEL_CHECKS while measuring this branch's tree -- harmless only as long as
# nobody changes PANEL_CHECKS, which is exactly the kind of "harmless only as
# long as" that stops being true without anyone noticing.
sys.path.insert(0, str(pathlib.Path(__file__).resolve().parents[2]))
from ui_live_smoke import PANEL_CHECKS          # the authoritative definition
from playwright.sync_api import sync_playwright

TABS = ["overview", "events", "sessions", "queries", "histogram",
        "transitions", "concurrency", "waterfall", "scatter", "matrix",
        "timeline"]   # timeline LAST: its drill-down sets a pid filter
NAVS = 3
POLL_MS = 25
TIMEOUT_MS = 15000
PINNED = (1710, 981, 2)

TIME_MODEL_SCRAPE = """() => {
    const rows = Array.from(document.querySelectorAll('#table-container tbody tr'));
    return rows.map(tr => {
        const c = tr.querySelector('td');
        return {text: (tr.innerText || '').replace(/\\s+/g, ' ').trim().slice(0, 120),
                pad: c ? (getComputedStyle(c).paddingLeft || '') : '',
                cls: c ? (c.className || '') : ''};
    });
}"""


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--url", default="http://localhost:8384/")
    ap.add_argument("--cdp", default="http://127.0.0.1:9333")
    ap.add_argument("--out", required=True)
    ap.add_argument("--label", required=True)
    # --tabs: only for the dead-bridge red demonstration below, where a full
    # 11-tab walk would spend 11 x 3 x 15 s timing out. Every MEASUREMENT run
    # walks the full default list -- a short list is exactly the "partial
    # range" paint_analyse.py refuses.
    ap.add_argument("--tabs", default=",".join(TABS))
    args = ap.parse_args()
    url = args.url
    tabs = [t for t in args.tabs.split(",") if t]

    with sync_playwright() as p:
        browser = p.chromium.connect_over_cdp(args.cdp)
        ctx = browser.contexts[0]
        page = ctx.pages[0] if ctx.pages else ctx.new_page()
        errors = []
        page.on("console", lambda m: errors.append((m.type, m.text[:200]))
                if m.type == "error" else None)
        page.on("pageerror", lambda e: errors.append(("pageerror", str(e)[:200])))

        # A bridge that is down must produce a RECORD, not a traceback: a
        # crashed harness leaves no file for the analyser to refuse, and
        # "no file" is the easiest thing in the world to mistake for
        # "no problem". Every navigation below is guarded the same way.
        load_errors = []
        try:
            page.goto(url, wait_until="domcontentloaded")
        except Exception as e:
            load_errors.append(str(e)[:140])
            print("initial load FAILED:", str(e)[:140])
        page.wait_for_timeout(1500)

        cdp = ctx.new_cdp_session(page)
        wid = cdp.send("Browser.getWindowForTarget")["windowId"]
        cdp.send("Browser.setWindowBounds",
                 {"windowId": wid, "bounds": {"windowState": "fullscreen"}})
        page.wait_for_timeout(2500)

        # The demo Chrome profile PERSISTS page zoom. A saved zoom silently put
        # one earlier walk at 1200x937 instead of the pinned 1710x981 -- a
        # smaller viewport does less work, so the numbers were optimistic and
        # unpinnable. Reset, do not trust.
        try:
            page.bring_to_front()
            page.keyboard.press("Meta+0")
            page.wait_for_timeout(800)
        except Exception as e:
            print("zoom reset failed:", str(e)[:80])

        def vp():
            return page.evaluate("()=>({w:innerWidth,h:innerHeight,dpr:devicePixelRatio})")

        # A viewport read right after the fullscreen animation can be a
        # transient (DEMO_REHEARSAL_CRITERIA records a 1710x1069 mid-animation
        # reading superseded 637 ms later). Two agreeing reads or nothing.
        r1 = vp(); page.wait_for_timeout(1200); r2 = vp()
        agree = (r1 == r2)
        pinned = (r1["w"], r1["h"], r1["dpr"]) == PINNED
        print("viewport read1:", r1, "read2:", r2, "| agree:", agree,
              "| matches pinned 1710x981@2:", pinned)
        if not agree:
            print("ABORT: viewport unstable; a transient reading cannot anchor a number.")
            sys.exit(2)
        if not pinned:
            print("ABORT: viewport is NOT the pinned demo geometry.")
            sys.exit(2)

        ver = browser.version
        rng = page.evaluate("()=>{const a=document.querySelector("
                            "'.range-btn.active, [data-range].active');"
                            "return a?a.textContent.trim():'?'}")
        print("chrome:", ver, "| active range button:", rng)

        def goto_tab(name):
            page.click('button.tab[data-tab="%s"]' % name)

        def settle_overview():
            """Full reload, not just a tab click: a timeline drill-down leaves a
            pid filter set and a burst-row click leaves a zoom. Either silently
            changes the workload the next tab measures."""
            try:
                page.goto(url, wait_until="domcontentloaded")
                page.wait_for_timeout(1400)
                goto_tab("overview")
            except Exception as e:
                load_errors.append(str(e)[:140])
            page.wait_for_timeout(700)

        def measure(tab):
            """Click, then poll the panel's own readiness check. Returns ms or None."""
            check = PANEL_CHECKS[tab]
            try:
                if tab == "timeline":
                    goto_tab("sessions")
                    page.wait_for_timeout(1200)
                    row = page.query_selector('#table-container tbody tr')
                    t0 = time.time()
                    if row:
                        row.click()
                    else:
                        return None, "no sessions row to drill"
                else:
                    t0 = time.time()
                    goto_tab(tab)
            except Exception as e:
                return None, "navigation failed: %s" % str(e)[:80]
            deadline = t0 + TIMEOUT_MS / 1000.0
            last = None
            while time.time() < deadline:
                try:
                    res = page.evaluate(check)
                except Exception as e:
                    res = {"ok": False, "detail": "eval: %s" % str(e)[:60]}
                last = res
                if res and res.get("ok"):
                    return (time.time() - t0) * 1000.0, res.get("detail", "")
                page.wait_for_timeout(POLL_MS)
            return None, (last or {}).get("detail", "timeout")

        results = {}
        for tab in tabs:
            samples, detail, err = [], "", None
            for i in range(NAVS):
                settle_overview()
                ms, d = measure(tab)
                if ms is None:
                    # Record the failure; do NOT abandon the rest of this tab's
                    # navigations and do NOT drop the tab from the record.
                    err = d
                    print("  %s nav %d: NOT RENDERED (%s)" % (tab, i, d))
                    continue
                samples.append(ms)
                detail = d
            entry = {"samples": samples, "detail": detail}
            if err is not None:
                entry["error"] = err
            results[tab] = entry
            if samples:
                print("  %-12s min %6.0f  med %6.0f  max %6.0f  n=%d  %s"
                      % (tab, min(samples), statistics.median(samples),
                         max(samples), len(samples), detail[:52]))

        # #295's Idle row: scrape the Overview time model after the timings, so
        # the scrape cannot perturb them.
        settle_overview()
        page.wait_for_timeout(1200)
        try:
            tm_rows = page.evaluate(TIME_MODEL_SCRAPE)
            summary_bar = page.evaluate(
                "()=>Array.from(document.querySelectorAll('.metric')).map("
                "e=>(e.innerText||'').replace(/\\s+/g,' ').trim())")
        except Exception as e:
            tm_rows, summary_bar = [], ["scrape failed: %s" % str(e)[:80]]
        print("\ntime-model rows: %d" % len(tm_rows))
        for r in tm_rows:
            print("   pad=%-7s %s" % (r["pad"], r["text"][:90]))
        print("summary bar:", summary_bar)

        out = {
            "label": args.label,
            "url": url,
            "chrome": ver,
            "viewport": r1,
            "viewport_read2": r2,
            "viewport_agree": agree,
            "pinned": pinned,
            "range": rng,
            "navs_per_tab": NAVS,
            "tabs_walked": tabs,
            "results": results,
            "time_model_rows": tm_rows,
            "summary_bar": summary_bar,
            "console_errors": errors[:40],
            "console_error_count": len(errors),
            "load_errors": load_errors[:20],
            "finished_epoch": time.time(),
            # Filled in by the caller after asking the box; left absent on
            # purpose so paint_analyse refuses rather than assuming.
        }
        pathlib.Path(args.out).write_text(json.dumps(out, indent=2))
        print("\nconsole errors:", len(errors))
        for e in errors[:8]:
            print("   ", e)
        print("written:", args.out)


if __name__ == "__main__":
    main()
