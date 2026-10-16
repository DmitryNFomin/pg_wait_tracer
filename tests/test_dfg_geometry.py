#!/usr/bin/env python3
"""test_dfg_geometry.py -- the DFG's viewport-fit guard must measure the
space ECharts actually draws in, not the space the builder's option object
describes (issue #436, adviser-review finding).

ECharts' `graph` series, even with `layout: 'none'` (explicit pixel x/y per
node), normalizes those coordinates through its own "view" coordinateSystem,
which by default fits the laid-out nodes' own bounding box into the series'
box (not a 1:1 pixel mapping) unless that transform is pinned to identity.
A pure-JS test that only inspects `option.series[0].data[i].x/y` cannot see
this at all -- it is blind to a space the renderer does not draw in, which
was EXACTLY what made the first #436 fix a false guard: the margins were
computed correctly in data space, then silently moved by the renderer.

This test is the live, rendered-coordinate half of the guard: it reads
`chart.getModel().getSeriesByIndex(0).coordinateSystem.dataToPoint([x, y])`
on a REALLY rendered instance (ECharts 5.5.1, web/static/vendor/
echarts.min.js) -- the exact function the renderer itself uses to place
nodes -- never `getOption()`'s raw, pre-transform coordinates. The fixture
(`six-equal-duration` in dev/fixtures/transitions.mjs) is the negative
control from the adviser's review: six nodes with IDENTICAL total_ms (so no
single huge node can mask the effect) in a 620x460 box. Before the fix
(dropping `left/top/width/height` + the two invisible corner-pinning
`layoutAnchorNodes`), the bottom row renders ~5% further from center than
its data coordinates and clips; after, data coordinates ARE pixel
coordinates. Demonstrated red/green by hand (see the PR body); this file is
the permanent, always-run half of that demonstration.

No root, no PG, no SSH -- uses mock_server.py's static file serving, exactly
like test_chip_label_alignment.py (gallery.html is static; no WS pair).
"""
import os
import signal
import socket
import subprocess
import sys
import time

try:
    from playwright.sync_api import sync_playwright
except ImportError:
    msg = "playwright not installed (pip install playwright && playwright install chromium)"
    if os.environ.get("CI"):
        print(f"ERROR: {msg}", file=sys.stderr)
        sys.exit(1)
    print(f"SKIP: {msg}")
    sys.exit(0)

HTTP_PORT = int(os.environ.get("PGWT_DFG_GEOM_PORT", "18881"))
MOCK_SCRIPT = os.path.join(os.path.dirname(os.path.abspath(__file__)), "mock_server.py")
CELL = "gallery-transitions-six-equal-duration"
GALLERY_URL = f"http://127.0.0.1:{HTTP_PORT}/dev/gallery.html?isolate={CELL}"

# Reads the REAL rendered node positions for the isolated DFG cell: the
# series' own coordinateSystem.dataToPoint(), not getOption()'s raw x/y.
# isLayoutAnchorNode is imported from the real, served builder module (a
# dynamic import, not a re-implementation) so this check can never silently
# drift from which entries the builder itself considers synthetic.
GEOMETRY_SCRIPT = """
async (cellId) => {
    const { isLayoutAnchorNode } = await import('/lib/builders/transitions.js');
    const host = document.querySelector('#' + cellId + ' .chart');
    if (!host) return { error: 'no .chart host found in #' + cellId };
    const chart = echarts.getInstanceByDom(host);
    if (!chart) return { error: 'no echarts instance on the chart host' };
    const opt = chart.getOption();
    const series = opt && opt.series && opt.series[0];
    if (!series || !series.data) return { error: 'no series/data in the rendered option' };
    const coordSys = chart.getModel().getSeriesByIndex(0).coordinateSystem;
    if (!coordSys || typeof coordSys.dataToPoint !== 'function') {
        return { error: 'series coordinateSystem has no dataToPoint (' +
            (coordSys && coordSys.type) + ')' };
    }
    const probe = document.createElement('canvas').getContext('2d');
    const nodes = series.data.filter(n => !isLayoutAnchorNode(n.name)).map(n => {
        const px = coordSys.dataToPoint([n.x, n.y]);
        const label = n.label || {};
        probe.font = (label.fontSize || 10) + 'px sans-serif';
        const lines = String(label.formatter || '').split('\\n');
        const maxLineW = Math.max(0, ...lines.map(l => probe.measureText(l).width));
        const radius = (n.symbolSize || 0) / 2;
        const halfW = Math.max(radius, maxLineW / 2);
        const lineHeight = label.lineHeight || 14;
        const bottom = radius + 6 + lines.length * lineHeight;
        return { name: n.name, dataXY: [n.x, n.y], pixelXY: px, radius, halfW, bottom };
    });
    return { nodes, dims: { width: host.clientWidth, height: host.clientHeight } };
}
"""


def _wait_port(proc, port, timeout=10.0):
    deadline = time.time() + timeout
    while True:
        if proc.poll() is not None:
            return False
        try:
            with socket.create_connection(("127.0.0.1", port), timeout=0.25):
                return True
        except OSError:
            if time.time() >= deadline:
                return False
            time.sleep(0.05)


def start_mock_server():
    proc = subprocess.Popen(
        [sys.executable, MOCK_SCRIPT, "--port", str(HTTP_PORT)],
        stdout=subprocess.PIPE, stderr=subprocess.PIPE,
    )
    if not _wait_port(proc, HTTP_PORT):
        if proc.poll() is None:
            proc.kill()
        out, err = proc.communicate()
        print(f"mock_server failed to start on port {HTTP_PORT}:\n"
              f"{out.decode()}\n{err.decode()}")
        sys.exit(1)
    return proc


def stop_mock_server(proc):
    proc.send_signal(signal.SIGTERM)
    try:
        proc.wait(timeout=5)
    except subprocess.TimeoutExpired:
        proc.kill()
        proc.wait()


def check_node_fits(node, dims):
    x, y = node["pixelXY"]
    half_w, radius, bottom = node["halfW"], node["radius"], node["bottom"]
    return (x - half_w >= -1 and x + half_w <= dims["width"] + 1 and
            y - radius >= -1 and y + bottom <= dims["height"] + 1)


def main():
    with sync_playwright() as p:
        proc = start_mock_server()
        try:
            browser = p.chromium.launch(headless=True)
            page = browser.new_page()
            errors = []
            page.on("pageerror", lambda e: errors.append(str(e)))
            page.goto(GALLERY_URL)
            page.wait_for_selector("body[data-gallery-ready='1']", timeout=15000)
            result = page.evaluate(GEOMETRY_SCRIPT, CELL)
            browser.close()
        finally:
            stop_mock_server(proc)

    if errors:
        print("FAIL: DFG rendered-geometry check -- page error(s): " + "; ".join(errors))
        sys.exit(1)
    if "error" in result:
        print(f"FAIL: DFG rendered-geometry check -- {result['error']} "
              "(the gallery cell, builder export, or echarts API changed "
              "shape; this check cannot see what it is supposed to grade)")
        sys.exit(1)

    nodes = result["nodes"]
    dims = result["dims"]
    if len(nodes) != 6:
        print(f"FAIL: DFG rendered-geometry check -- expected 6 real nodes "
              f"(the six-equal-duration fixture), got {len(nodes)}: "
              f"{[n['name'] for n in nodes]}")
        sys.exit(1)

    clipped = [n for n in nodes if not check_node_fits(n, dims)]
    if clipped:
        print("FAIL: DFG node/label clips the REAL rendered container "
              f"(dims={dims}):")
        for n in clipped:
            print(f"  {n['name']}: data={n['dataXY']} rendered={n['pixelXY']} "
                  f"halfW={n['halfW']:.1f} radius={n['radius']:.1f} bottom={n['bottom']:.1f}")
        sys.exit(1)

    # Positive confirmation that the transform is pinned to identity (not
    # merely "happened not to clip this time"): every node's rendered pixel
    # position must equal its data position, to a tight tolerance.
    drifted = [n for n in nodes
               if abs(n["pixelXY"][0] - n["dataXY"][0]) > 0.5
               or abs(n["pixelXY"][1] - n["dataXY"][1]) > 0.5]
    if drifted:
        print("FAIL: DFG data->pixel mapping is not identity (the view "
              "transform pin regressed), even though nothing happened to clip:")
        for n in drifted:
            print(f"  {n['name']}: data={n['dataXY']} rendered={n['pixelXY']}")
        sys.exit(1)

    print("PASS: DFG rendered geometry -- six equal-duration nodes at "
          f"{dims['width']}x{dims['height']} all fit, data==pixel for every node "
          "(issue #436, adviser-review control)")


if __name__ == "__main__":
    main()
