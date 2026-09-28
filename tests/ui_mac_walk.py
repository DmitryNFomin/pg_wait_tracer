#!/usr/bin/env python3
"""ui_mac_walk.py -- the Mac-side demo-path walk.

Every rehearsal so far has walked the UI on the VM, in headless Chromium,
against a bridge and daemon that are also on the VM. The actual demo is
different in every one of those dimensions: real Google Chrome, on the
owner's Mac, at the Mac's real Retina resolution, talking to a Go bridge
running on the Mac that ssh's out to a VM for the daemon + PostgreSQL. This
script is a first honest measurement of THAT path, not a copy of the
existing gate walk pointed somewhere else -- see the REUSE / NOT REUSED
section below for exactly what carries over from tests/ui_live_smoke.py and
what does not.

It expects a Go bridge already running and reachable at --url (started by
tests/ui_mac_walk.sh, which also provisions the VM, starts the daemon +
workload there, and tears everything down). This script only drives the
browser and writes the verdict; it does not touch ssh, the VM, or the
bridge process itself.

REUSE, NOT FORK (tests/ui_live_smoke.py is read-only from here -- see the
task's constraint -- so everything below is an IMPORT, never a copy):
  - ConsoleErrorGuard, UNHANDLED_REJECTION_HOOK, TICK_HOOK_JS: verbatim.
  - _navigate_to_tab, _poll_render_check, _is_pgwt_error, SmokeFailure,
    PANEL_CHECKS (via _poll_render_check), _PANEL_ELEMENT_SELECTOR: verbatim.
  - ui_live_smoke_lib.TABS: the same 11-tab order the gate walk uses.

NOT reused, and why: tests/ui_live_smoke.py's own run_tab() cannot be called
as-is -- it hardcodes `viewport={"width": 1280, "height": 900}` in its own
browser.new_context() call with no parameter to override it, and this task's
whole point is the Mac's PINNED 1440x932 @ DPR2 viewport (criteria doc "The
client, pinned"). Calling run_tab() unmodified would silently walk at the
wrong resolution; forking it just to change one literal would maintain two
copies of ~250 lines. So this script drives its OWN, much lighter per-tab
loop built from the same imported pieces instead. It deliberately drops
run_tab's multi-tick blink-sweep and leak-probe instrumentation (issue #119
/ #193) -- out of scope for a first look at a path that has never been
exercised even once, and each tick's 5-offset screenshot sweep alone would
multiply an already tight ~12-minute real-network budget past what a single
exploratory run can afford. What IS collected per tab, per the task: time to
first paint, console errors, whether the panel rendered, and (in a second,
lighter pass -- see freshness_pass below) how stale its data was by the end
of the walk.

Mac attribution (criteria doc: "a tick under non-normal pressure is VOID,
not a failure"): a background MacSampler thread records loadavg +
memory-pressure level every SAMPLE_INTERVAL_S for the whole run. Each tab's
visit window is checked against those samples by ui_mac_walk_lib
.window_void_reason -- a tab visited while the Mac was not at pressure level
1 (normal) is marked void, separately from whether it actually rendered.

Usage:
    python3 tests/ui_mac_walk.py --url http://localhost:8384/
"""
import argparse
import json
import os
import sys
import time
import traceback

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import ui_mac_walk_lib as lib
import ui_live_smoke as uls  # reused pieces only -- see module docstring
import ui_live_smoke_lib as uls_lib
from mac_sampler import MacSampler, SAMPLE_INTERVAL_S

try:
    from playwright.sync_api import sync_playwright, TimeoutError as PWTimeoutError
except ImportError:
    print("ERROR: pip install playwright && playwright install", file=sys.stderr)
    sys.exit(1)

RESULTS_DIR = os.path.join(os.path.dirname(os.path.abspath(__file__)),
                            "results", "ui_mac_walk")

# criteria doc "The client, pinned": the Mac's built-in Retina panel is
# 2880x1864 physical pixels at deviceScaleFactor 2, i.e. 1440x932 CSS.
VIEWPORT = {"width": 1440, "height": 932}
DEVICE_SCALE_FACTOR = 2

# MacSampler / SAMPLE_INTERVAL_S: tests/mac_sampler.py -- shared with
# tests/ui_mac_walk_safari.py so both walks attribute ticks against the
# same sampling code instead of two copies drifting apart.


# ── one tab's TTFP / render / console-error visit ───────────────────────────

def visit_tab(browser, tab_id, url, first_data_timeout, out_dir):
    context = browser.new_context(viewport=VIEWPORT,
                                   device_scale_factor=DEVICE_SCALE_FACTOR)
    context.add_init_script(uls.UNHANDLED_REJECTION_HOOK)
    page = context.new_page()
    guard = uls.ConsoleErrorGuard(page)

    wall_start = time.time()
    t0 = time.monotonic()
    render_ok, render_detail, ttfp_s = False, "never checked", None
    try:
        page.goto(url)
        try:
            page.wait_for_selector("#status.connected", timeout=first_data_timeout * 1000)
        except PWTimeoutError:
            raise uls.SmokeFailure(
                f"WebSocket never reached #status.connected within {first_data_timeout}s")

        deadline = time.monotonic() + first_data_timeout
        while page.query_selector("#table-container table tbody tr") is None:
            if time.monotonic() > deadline:
                raise uls.SmokeFailure(f"UI showed no data within {first_data_timeout}s")
            page.wait_for_timeout(200)

        uls._navigate_to_tab(page, tab_id, first_data_timeout)
        ttfp_s = time.monotonic() - t0
        render_ok, render_detail = uls._poll_render_check(page, tab_id)
    except uls.SmokeFailure as e:
        render_detail = str(e)
        print(f"  FAIL [{tab_id}]: {e}", file=sys.stderr)
    except Exception:
        render_detail = "unhandled exception (see stderr traceback)"
        traceback.print_exc()

    console_errors, pgwt_errors = [], []
    for e in guard.drain():
        (pgwt_errors if uls._is_pgwt_error(e) else console_errors).append(e)

    shot_path = None
    try:
        sel = uls._PANEL_ELEMENT_SELECTOR.get(tab_id)
        panel = page.query_selector(sel) if sel else None
        if panel is not None:
            tab_dir = os.path.join(out_dir, tab_id)
            os.makedirs(tab_dir, exist_ok=True)
            shot_path = os.path.join(tab_dir, "ttfp.png")
            panel.screenshot(path=shot_path)
    except Exception as e:
        print(f"  note [{tab_id}]: could not capture screenshot: {e}", file=sys.stderr)

    wall_end = time.time()
    context.close()
    return {
        "render_ok": render_ok, "render_detail": render_detail, "ttfp_s": ttfp_s,
        "console_errors": console_errors, "pgwt_errors": pgwt_errors,
        "wall_start": wall_start, "wall_end": wall_end, "screenshot": shot_path,
    }


# ── freshness pass: one persistent page, click through every tab AGAIN at
# the end of the walk, read the global AAS-tick age (see module docstring
# for why this is a proxy, not a per-chart-bucket read) ────────────────────

def version_skew_snapshot(page):
    """Reads #version-skew (web/static/app.js renderVersionSkew) so the
    verdict can report client/server version skew directly instead of
    relying on a human noticing the header badge in a screenshot."""
    return page.evaluate(
        "() => { const el = document.getElementById('version-skew'); "
        "if (!el) return {present: false}; "
        "const cs = getComputedStyle(el); "
        "return {present: true, visible: cs.display !== 'none', "
        "text: el.textContent, cls: el.className}; }")


def freshness_pass(browser, url, first_data_timeout):
    context = browser.new_context(viewport=VIEWPORT,
                                   device_scale_factor=DEVICE_SCALE_FACTOR)
    page = context.new_page()
    ages = {}
    version_skew = None
    try:
        page.goto(url)
        page.wait_for_selector("#status.connected", timeout=first_data_timeout * 1000)
        page.evaluate(uls.TICK_HOOK_JS)
        version_skew = lib.version_skew_from_snapshot(version_skew_snapshot(page))
        for tab_id in uls_lib.TABS:
            try:
                uls._navigate_to_tab(page, tab_id, first_data_timeout)
                page.wait_for_timeout(300)
                last_ts = page.evaluate(
                    "window.__uiLiveTicks && window.__uiLiveTicks.length "
                    "? window.__uiLiveTicks[window.__uiLiveTicks.length - 1] : null")
                now_ms = page.evaluate("Date.now()")
                ages[tab_id] = (now_ms - last_ts) / 1000.0 if last_ts is not None else None
            except Exception as e:
                print(f"  note [freshness {tab_id}]: {e}", file=sys.stderr)
                ages[tab_id] = None
    finally:
        context.close()
    return ages, version_skew


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                  formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--url", required=True,
                     help="URL of the Mac-local Go bridge (tests/ui_mac_walk.sh starts it)")
    ap.add_argument("--out-dir", default=RESULTS_DIR)
    ap.add_argument("--first-data-timeout", type=float, default=uls_lib.FIRST_DATA_TIMEOUT_S)
    ap.add_argument("--sample-interval-s", type=float, default=SAMPLE_INTERVAL_S)
    args = ap.parse_args()

    os.makedirs(args.out_dir, exist_ok=True)
    # Every artifact belongs to THIS run only.
    for name in os.listdir(args.out_dir):
        p = os.path.join(args.out_dir, name)
        if os.path.isdir(p):
            import shutil
            shutil.rmtree(p)
        else:
            os.remove(p)

    run_id = os.environ.get("PGWT_RUN_MARKER", str(int(time.time())))
    with open(os.path.join(args.out_dir, "run.id"), "w") as f:
        f.write(run_id)

    sampler = MacSampler(args.sample_interval_s)
    sampler.start()

    tabs = []
    notes = []
    version_skew = None
    try:
        with sync_playwright() as p:
            browser = p.chromium.launch(channel="chrome")
            try:
                for tab_id in uls_lib.TABS:
                    print(f"=== {tab_id} (TTFP pass) ===")
                    v = visit_tab(browser, tab_id, args.url,
                                  args.first_data_timeout, args.out_dir)
                    void_reason = lib.window_void_reason(
                        sampler.samples, v["wall_start"], v["wall_end"])
                    entry = lib.build_tab_entry(
                        tab_id, v["render_ok"], v["render_detail"], v["ttfp_s"],
                        v["console_errors"], v["pgwt_errors"],
                        freshness_age_s=None, mac_void_reason=void_reason)
                    tabs.append(entry)
                    print(f"  rendered={entry['rendered']} ttfp_s={v['ttfp_s']} "
                          f"console_errors={len(v['console_errors'])} "
                          f"void={void_reason!r}")

                print("=== freshness pass ===")
                ages, version_skew = freshness_pass(browser, args.url, args.first_data_timeout)
                print(f"  version_skew: {version_skew}")
                for entry in tabs:
                    entry["freshness"]["age_s"] = ages.get(entry["tab"])
                    ok, detail = lib.freshness_ok(ages.get(entry["tab"]))
                    entry["freshness"]["ok"] = ok
                    entry["freshness"]["detail"] = detail
                    # freshness is reported per tab but deliberately does not
                    # gate entry["ok"] here -- see the appended note below.
            finally:
                browser.close()
    finally:
        sampler.stop()

    notes.append("freshness is a page-global AAS-tick-age proxy, not a per-chart-type "
                 "newest-bucket read (criteria doc §5 wants the latter) -- see module "
                 "docstring; it is reported per tab but does not gate `ok` here.")
    verdict = lib.build_verdict(run_id, tabs, sampler.samples, notes=notes)
    verdict["version_skew"] = version_skew
    verdict_path = os.path.join(args.out_dir, "summary.json")
    with open(verdict_path, "w") as f:
        json.dump(verdict, f, indent=2, sort_keys=True)

    assert lib.run_id_matches(verdict, run_id), "run.id mismatch immediately after writing -- bug"

    print()
    print("════════════════════════════════════════")
    print("  MAC WALK SUMMARY")
    print("════════════════════════════════════════")
    for tab_id in uls_lib.TABS:
        e = verdict["tabs"][tab_id]
        label = "VOID" if e["void_reason"] else ("PASS" if e["ok"] else "FAIL")
        print(f"  {label:5s} {tab_id:12s} ttfp={e['ttfp_s']} "
              f"rendered={e['rendered']} console_errors={len(e['console_errors'])} "
              f"freshness={e['freshness']}")
    print(f"  mac samples: {len(sampler.samples)} "
          f"(non-normal: {sum(1 for s in sampler.samples if not lib.sample_is_normal(s))})")
    print(f"  summary: {verdict_path}")
    print(f"  overall ok: {verdict['ok']} (void tabs: {verdict['void_tabs']})")
    return 0


if __name__ == "__main__":
    sys.exit(main())
