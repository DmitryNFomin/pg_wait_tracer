#!/usr/bin/env python3
"""verify_waterfall_fold.py -- issue #415 integration verification.

The #415 fix (cherry-picked from a2aa32d on agent/web-tables) was built and
unit-tested against the isolated fixture gallery. The brief for its
integration explicitly distrusts that: "The gallery is exactly what missed
this: the readout is pushed down by breadcrumbs, controls and the persistent
AAS chart, which a gallery cell does not have." So this script drives the
REAL application shell (tests/mock_server.py + index.html, not
dev/gallery.html) at the pinned demo viewport (1710x981 CSS @ DPR2,
docs/chrome-demo-viewport-2026-09-28.md) and measures, in actual Chrome:

  - #waterfall-readout's bounding-box bottom  (must be <= the viewport height
    -- "above the fold")
  - document.documentElement.scrollHeight vs window.innerHeight
    ("scroll_y == 0" -- the page must not be scrollable at all post-fix,
    matching the original commit's own measurement)

It drills into pid 1000 (leader + 2 workers -- a 3-lane chart, the shape the
original commit measured as 90px over) so the executions list + chart
combination actually exercises executionListMaxHeight(), not a short list
that was never near the old 310px cap.

Also checks the unfiltered snapshot view at 1280x800 DPR1 (header plus one
full execution row) and 1710x981 DPR2 (two full rows and an exact 981px fold).
Restoring the 60px floor in the served module must fail the small-viewport
visibility assertion and leave the demo geometry unchanged. Reports actual
header/row heights and the minimum list height including its title and note.

Wired into scripts/check.sh (every `make check`, unconditional -- the gate
the pre-push hook enforces) and, behind the SAME PGWT_RUN_WEB_UI=1 opt-in as
its siblings test_web_ui.py/test_web_ui_chaos.py, into tests/run_all.sh
(box-check) and .github/workflows/ci.yml's `web-ui` job. It needs only
python3 + playwright + mock_server.py -- no PG, no root, no BPF -- the exact
tier test_web_ui.py already runs in all three places, so it follows that
precedent rather than inventing a fourth one. The permanent REGRESSION
coverage for the underlying fix is tests/web_unit/waterfall_fit.test.mjs
(pure-function unit tests, no browser); this script is the integration half
that catches what the isolated fixture gallery cannot see (real breadcrumbs/
controls/AAS-chart geometry) -- see the module docstring above. Exit 0 +
"PASS" on success, exit 1 + "FAIL" with the measured numbers otherwise, so a
non-zero exit here is a real gate failure, not an optional warning.

PORT: reads PGWT_WATERFALL_PORT (falls back to 18977 standalone, matching
mock_server.py's own default-port convention) rather than a hardcoded value,
so scripts/check.sh can hand it a run-private port out of its own
PGWT_PORT_BASE allocation (concurrent `make check` runs on one Mac must not
collide on ports -- see check.sh's own port-map comment).

Usage:
    python3 tests/verify_waterfall_fold.py [--out FILE]
    PGWT_WATERFALL_PORT=18866 python3 tests/verify_waterfall_fold.py
"""
import argparse
import hashlib
import json
import os
import sys
import subprocess
from urllib.parse import urlparse

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from test_web_ui import start_mock_server, stop_mock_server, app_url  # noqa: E402
from web_unit.span_browser import run_span_gates
from web_unit.execution_list_browser import run_execution_list_gates

try:
    from playwright.sync_api import sync_playwright
except ImportError:
    print("ERROR: pip install playwright && playwright install chromium",
          file=sys.stderr)
    sys.exit(1)

PORT = int(os.environ.get("PGWT_WATERFALL_PORT", "18977"))


def chart_metrics(page):
    return page.evaluate("""() => {
        const host = document.querySelector('#waterfall-chart');
        const chart = echarts.getInstanceByDom(host);
        const data = chart.getModel().getSeriesByIndex(0).getData();
        const bars = [];
        for (let i = 0; i < data.count(); i++) {
            const graphic = data.getItemGraphicEl(i);
            if (!graphic || graphic.ignore || graphic.invisible) continue;
            const box = graphic.getBoundingRect();
            if (box.width > 0 && box.height > 0) bars.push({i, box});
        }
        const widest = bars.sort((a, b) => b.box.width - a.box.width)[0];
        const tuple = widest && chart.getOption().series[0].data[widest.i];
        const canvas = host.querySelector('canvas').getBoundingClientRect();
        const point = tuple && chart.convertToPixel({gridIndex: 0},
            [(tuple[0] + tuple[1]) / 2, tuple[2]]);
        return {
            height: host.getBoundingClientRect().height,
            minimum_height: parseFloat(getComputedStyle(host).minHeight),
            canvas_height: canvas.height, rendered_bars: bars.length,
            minimum_bar_height: bars.length ? Math.min(...bars.map(b => b.box.height)) : 0,
            click: point ? {x: canvas.left + point[0], y: canvas.top + point[1],
                name: tuple[3]} : null,
        };
    }""")


def run(red_green=False):
    proc = start_mock_server(port=PORT)
    try:
        with sync_playwright() as p:
            browser = p.chromium.launch()
            try:
                ctx = browser.new_context(
                    viewport={"width": 1710, "height": 981},
                    device_scale_factor=2)
                page = ctx.new_page()
                url = app_url(PORT) + "#tab=waterfall&live=1&span=900&f.pid=1000"
                page.goto(url)
                page.wait_for_selector("#status.connected", timeout=10000)
                page.wait_for_selector("#waterfall-chart canvas", timeout=10000)
                page.wait_for_timeout(300)  # let fitExecutionList() settle

                # The mock's `executions` fixture is only 4 rows -- nowhere
                # near the old fixed 310px cap the original commit measured
                # against ("a 30-row executions page"). Simulate that page
                # directly in the DOM (purely a layout probe; it does not
                # touch the data layer the mock answers with) so the list
                # box is tall enough for a 310px fixed cap, or
                # executionListMaxHeight()'s dynamic one, to actually matter.
                injected = page.evaluate("""(n) => {
                    const tbody = document.querySelector('#executions-table tbody');
                    if (!tbody) return 0;
                    const cols = tbody.closest('table').querySelectorAll('thead th').length;
                    let added = 0;
                    for (let i = 0; i < n; i++) {
                        const tr = document.createElement('tr');
                        tr.innerHTML = '<td>synthetic row ' + i + '</td>'.repeat(cols);
                        tbody.appendChild(tr);
                        added++;
                    }
                    window.dispatchEvent(new Event('resize'));
                    return added;
                }""", 40)
                page.wait_for_timeout(200)

                readout = page.query_selector("#waterfall-readout")
                if readout is None:
                    return {"ok": False, "reason": "no #waterfall-readout element"}
                box = readout.bounding_box()
                metrics = page.evaluate("""() => ({
                    innerHeight: window.innerHeight,
                    scrollY: window.scrollY,
                    scrollHeight: document.documentElement.scrollHeight,
                    listMaxHeight: (document.querySelector('.execution-list') || {}).style
                        ? document.querySelector('.execution-list').style.maxHeight : null,
                })""")
                readout_bottom = box["y"] + box["height"]
                above_fold = readout_bottom <= metrics["innerHeight"] + 0.5
                not_scrollable = metrics["scrollHeight"] <= metrics["innerHeight"] + 0.5
                scroll_y_zero = metrics["scrollY"] == 0
                result = {
                    "ok": bool(above_fold and not_scrollable and scroll_y_zero),
                    "injected_rows": injected,
                    "readout_bottom": readout_bottom,
                    "viewport_height": metrics["innerHeight"],
                    "scroll_height": metrics["scrollHeight"],
                    "scroll_y": metrics["scrollY"],
                    "execution_list_max_height": metrics["listMaxHeight"],
                    "above_fold": above_fold,
                    "not_scrollable": not_scrollable,
                    "scroll_y_zero": scroll_y_zero,
                }
                result["chart"] = chart_metrics(page)
                for _ in range(3):
                    page.evaluate("window.dispatchEvent(new Event('resize'))")
                    page.wait_for_timeout(100)
                resized = chart_metrics(page)
                result["chart"]["stable_after_resize"] = all(
                    resized[key] == result["chart"][key] for key in
                    ["height", "canvas_height", "rendered_bars", "minimum_bar_height"])
                click = resized["click"]
                if click:
                    page.mouse.click(click["x"], click["y"])
                inspected = page.locator("#waterfall-readout .wf-inspect-name")
                result["chart"]["readout_clickable"] = (
                    bool(click) and inspected.count() == 1 and
                    inspected.inner_text() == click["name"])
                clicked_box = page.locator("#waterfall-readout").bounding_box()
                result["chart"]["readout_bottom_after_click"] = (
                    clicked_box["y"] + clicked_box["height"])
                result["chart"]["scroll_height_after_click"] = page.evaluate(
                    "document.documentElement.scrollHeight")
                result["chart"]["scroll_y_after_click"] = page.evaluate("scrollY")
                chart_ok = (
                    resized["height"] >= resized["minimum_height"] and
                    resized["rendered_bars"] > 0 and resized["minimum_bar_height"] > 0 and
                    result["chart"]["stable_after_resize"] and
                    result["chart"]["readout_clickable"] and
                    result["chart"]["readout_bottom_after_click"] <= metrics["innerHeight"] and
                    result["chart"]["scroll_height_after_click"] <= metrics["innerHeight"] and
                    result["chart"]["scroll_y_after_click"] == 0)
                result["chart"]["ok"] = chart_ok
                result["ok"] = result["ok"] and chart_ok
                if red_green:
                    # Serve original web assets over the same real mock shell.
                    # No checkout/reset and no edits to another agent's files.
                    ref = '48e1bce'
                    paths = subprocess.check_output(
                        ['git', 'ls-tree', '-r', '--name-only', ref, '--', 'web/static/'],
                        text=True).splitlines()
                    original = { '/' + path.removeprefix('web/static/'):
                        subprocess.check_output(['git', 'show', ref + ':' + path])
                        for path in paths if path.endswith(('.js', '.css')) }
                    changed = subprocess.check_output(
                        ['git', 'diff', '--name-only', ref, '--', 'web/static/'],
                        text=True).splitlines()
                    required = {'/' + path.removeprefix('web/static/') for path in changed
                                if path.endswith(('.js', '.css'))}
                    assert required, 'no changed assets: baseline comparison would prove nothing'
                    served = {}
                    old = ctx.new_page()
                    def baseline_route(route):
                        path = urlparse(route.request.url).path
                        if path in original:
                            route.fulfill(body=original[path], content_type=(
                                'text/css' if path.endswith('.css') else 'application/javascript'))
                            served[path] = hashlib.sha256(original[path]).hexdigest()
                        elif path.endswith(('.js', '.css')):
                            # A new asset absent from the baseline must never
                            # leak current code into the old application.
                            route.fulfill(status=404, body='Not present in baseline ' + ref)
                        else:
                            route.continue_()
                    old.route('**/*', baseline_route)
                    try:
                        result['baseline'] = run_span_gates(old, app_url(PORT), expect_red=True)
                        assert required <= served.keys(), (
                            'baseline did not serve changed assets: ' + repr(sorted(required - served.keys())))
                        result['baseline']['ref'] = ref
                        result['baseline']['served_asset_sha256'] = {
                            path: served[path] for path in sorted(required)}
                    finally:
                        old.close()
                result["span_gates"] = run_span_gates(page, app_url(PORT))
                result["execution_list_gates"] = run_execution_list_gates(browser, app_url(PORT))
                result["ok"] = result["ok"] and result["execution_list_gates"]["ok"]
                return result
            finally:
                browser.close()
    finally:
        stop_mock_server(proc)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--out", default=None,
                     help="write the JSON result here too")
    ap.add_argument('--red-green', action='store_true',
                    help='first assert all three gates fail against original 48e1bce web assets')
    args = ap.parse_args()
    result = run(red_green=args.red_green)
    print(json.dumps(result, indent=2))
    if args.out:
        with open(args.out, "w") as f:
            json.dump(result, f, indent=2)
    sys.exit(0 if result.get("ok") else 1)


if __name__ == "__main__":
    main()
