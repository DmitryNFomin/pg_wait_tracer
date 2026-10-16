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
import json
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from test_web_ui import start_mock_server, stop_mock_server, app_url  # noqa: E402

try:
    from playwright.sync_api import sync_playwright
except ImportError:
    print("ERROR: pip install playwright && playwright install chromium",
          file=sys.stderr)
    sys.exit(1)

PORT = int(os.environ.get("PGWT_WATERFALL_PORT", "18977"))


def run():
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
                return result
            finally:
                browser.close()
    finally:
        stop_mock_server(proc)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--out", default=None,
                     help="write the JSON result here too")
    args = ap.parse_args()
    result = run()
    print(json.dumps(result, indent=2))
    if args.out:
        with open(args.out, "w") as f:
            json.dump(result, f, indent=2)
    sys.exit(0 if result.get("ok") else 1)


if __name__ == "__main__":
    main()
