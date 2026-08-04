/* pgwt — drag-select-window overlay for the AAS chart.
 *
 * Replaces ApexCharts' built-in drag-select zoom (the only reason ApexCharts
 * was ever added). A custom overlay gives full visual control over the band,
 * is reusable on other time-axis charts, and — crucially — its pixel→time math
 * is a pure function, so it is Node-unit-testable without a browser.
 *
 * Split:
 *   pixelRangeToTime(pixels, chart)   PURE: maps an [x1,x2] pixel pair (in the
 *                                     chart's coordinate space) to a sorted
 *                                     {from,to} range via ECharts'
 *                                     convertFromPixel. The result is in the
 *                                     chart's X-AXIS UNITS — ms since epoch on
 *                                     the U2a AAS time axis (the consumer
 *                                     converts to ns); whatever the axis holds
 *                                     elsewhere. Tested with a fake `chart`
 *                                     that exposes convertFromPixel.
 *   attachSelection(el, chart, opts)  thin: pointer events on `el`, a styled
 *                                     band div, and a callback with the time
 *                                     range when the drag completes.
 *
 * Gesture split with the camera (U2a, constraint C): PLAIN drag = this
 * brush-select overlay; SHIFT+drag = the inside-dataZoom pan
 * (moveOnMouseMove:'shift'); wheel = cursor-anchored dataZoom zoom. onDown
 * therefore ignores shift-modified drags — the overlay must never draw a
 * selection band over an ECharts pan.
 */

const MIN_DRAG_PX = 5;   // ignore micro-drags (treat as clicks)

/* Pure: convert a pixel x-range to a time range using the chart's
 * convertFromPixel. `chart` only needs convertFromPixel([x,y]) -> [valueX,..].
 * Values are rounded to integers in the AXIS UNITS (ms on the U2a time axis).
 * Returns null for a degenerate range. */
export function pixelRangeToTime(x1, x2, chart) {
    const lo = Math.min(x1, x2);
    const hi = Math.max(x1, x2);
    if (hi - lo < 1) return null;
    const a = chart.convertFromPixel({ gridIndex: 0 }, [lo, 0]);
    const b = chart.convertFromPixel({ gridIndex: 0 }, [hi, 0]);
    if (a == null || b == null) return null;
    const va = Array.isArray(a) ? a[0] : a;
    const vb = Array.isArray(b) ? b[0] : b;
    if (va == null || vb == null || isNaN(va) || isNaN(vb)) return null;
    const from = Math.min(va, vb);
    const to = Math.max(va, vb);
    if (to <= from) return null;
    return { from: Math.round(from), to: Math.round(to) };
}

/* Pure 2-D counterpart used by the execution scatter. Values are returned in
 * axis units (milliseconds on a time axis, duration-ms on the log y axis).
 * ECharts' y pixels increase downward, so both dimensions are sorted here. */
export function pixelBoxToValues(x1, y1, x2, y2, chart) {
    if (Math.abs(x2 - x1) < 1) return null;
    const a = chart.convertFromPixel({ gridIndex: 0 }, [x1, y1]);
    const b = chart.convertFromPixel({ gridIndex: 0 }, [x2, y2]);
    if (!Array.isArray(a) || !Array.isArray(b) || a.length < 2 || b.length < 2)
        return null;
    const vals = [a[0], a[1], b[0], b[1]].map(Number);
    if (vals.some(v => !Number.isFinite(v))) return null;
    const from = Math.min(vals[0], vals[2]), to = Math.max(vals[0], vals[2]);
    const yFrom = Math.min(vals[1], vals[3]), yTo = Math.max(vals[1], vals[3]);
    if (!(to > from)) return null;
    return { from, to, yFrom, yTo };
}

/* Pure coordinate split: selection bands are children of the padded host,
 * while ECharts convertFromPixel consumes coordinates relative to its canvas.
 * Keeping both prevents the visible band and applied range from diverging. */
export function pointerCoordinates(clientX, clientY, hostRect, canvasRect) {
    canvasRect = canvasRect || hostRect;
    return {
        hostX: clientX - hostRect.left,
        hostY: clientY - hostRect.top,
        chartX: clientX - canvasRect.left,
        chartY: clientY - canvasRect.top,
    };
}

function localPoint(e, el, chart) {
    const hostRect = el.getBoundingClientRect();
    let viewport = null;
    /* ECharts' canvas is inset by host padding. Prefer the actual canvas rect;
     * custom adapters (uPlot AAS) have no canvas child and retain host coords. */
    const canvas = chart && typeof chart.getZr === 'function' &&
        el.querySelector ? el.querySelector('canvas') : null;
    if (canvas && canvas.getBoundingClientRect)
        viewport = canvas.getBoundingClientRect();
    return pointerCoordinates(e.clientX, e.clientY, hostRect, viewport);
}

/* Attach a drag-select overlay to `el` driving `chart`. opts:
 *   { onSelect({from,to}), minDragPx }
 * Returns a detach() function that removes listeners and the band element. */
export function attachSelection(el, chart, opts) {
    opts = opts || {};
    const minDrag = opts.minDragPx || MIN_DRAG_PX;

    // Ensure the host can position the absolute band.
    if (getComputedStyle(el).position === 'static') {
        el.style.position = 'relative';
    }

    const band = document.createElement('div');
    band.className = 'pgwt-select-band';
    band.style.cssText =
        'position:absolute;top:0;bottom:0;display:none;pointer-events:none;' +
        'background:rgba(79,195,247,0.18);border-left:1px solid #4fc3f7;' +
        'border-right:1px solid #4fc3f7;z-index:5';
    el.appendChild(band);

    let dragging = false;
    let startHostX = 0, startChartX = 0;

    function onDown(e) {
        // Shift+drag is the camera pan (inside-dataZoom moveOnMouseMove:
        // 'shift', U2a) — not a brush. See the header's gesture split.
        if (e.button !== 0 || e.shiftKey) return;
        const p = localPoint(e, el, chart);
        dragging = true;
        startHostX = p.hostX;
        startChartX = p.chartX;
        band.style.left = startHostX + 'px';
        band.style.width = '0px';
        band.style.display = 'block';
    }

    function onMove(e) {
        if (!dragging) return;
        const x = localPoint(e, el, chart).hostX;
        const lo = Math.min(startHostX, x);
        const hi = Math.max(startHostX, x);
        band.style.left = lo + 'px';
        band.style.width = (hi - lo) + 'px';
    }

    function onUp(e) {
        if (!dragging) return;
        dragging = false;
        band.style.display = 'none';
        const p = localPoint(e, el, chart);
        if (Math.abs(p.hostX - startHostX) < minDrag) return;
        const range = pixelRangeToTime(startChartX, p.chartX, chart);
        if (range && opts.onSelect) opts.onSelect(range);
    }

    el.addEventListener('mousedown', onDown);
    window.addEventListener('mousemove', onMove);
    window.addEventListener('mouseup', onUp);

    return function detach() {
        el.removeEventListener('mousedown', onDown);
        window.removeEventListener('mousemove', onMove);
        window.removeEventListener('mouseup', onUp);
        if (band.parentNode) band.parentNode.removeChild(band);
    };
}

/* Attach a rectangular plain-drag selector. Kept separate from
 * attachSelection so timeline/waterfall retain their full-height band and
 * exact callback contract. opts: { onSelect(box), minDragPx }. */
export function attachBoxSelection(el, chart, opts) {
    opts = opts || {};
    const minDrag = opts.minDragPx || MIN_DRAG_PX;
    if (getComputedStyle(el).position === 'static') el.style.position = 'relative';

    const band = document.createElement('div');
    band.className = 'pgwt-select-band pgwt-select-box';
    band.style.cssText =
        'position:absolute;display:none;pointer-events:none;' +
        'background:rgba(79,195,247,0.18);border:1px solid #4fc3f7;z-index:5';
    el.appendChild(band);
    let dragging = false;
    let startHostX = 0, startHostY = 0, startChartX = 0, startChartY = 0;
    function onDown(e) {
        if (e.button !== 0 || e.shiftKey) return;
        const p = localPoint(e, el, chart);
        dragging = true;
        startHostX = p.hostX; startHostY = p.hostY;
        startChartX = p.chartX; startChartY = p.chartY;
        band.style.left = startHostX + 'px'; band.style.top = startHostY + 'px';
        band.style.width = '0px'; band.style.height = '0px';
        band.style.display = 'block';
    }
    function onMove(e) {
        if (!dragging) return;
        const p = localPoint(e, el, chart);
        band.style.left = Math.min(startHostX, p.hostX) + 'px';
        band.style.top = Math.min(startHostY, p.hostY) + 'px';
        band.style.width = Math.abs(p.hostX - startHostX) + 'px';
        band.style.height = Math.abs(p.hostY - startHostY) + 'px';
    }
    function onUp(e) {
        if (!dragging) return;
        dragging = false; band.style.display = 'none';
        const p = localPoint(e, el, chart);
        if (Math.abs(p.hostX - startHostX) < minDrag) return;
        const box = pixelBoxToValues(startChartX, startChartY,
                                     p.chartX, p.chartY, chart);
        if (box)
            box.hasYRange = Math.abs(p.hostY - startHostY) >= minDrag;
        if (box && opts.onSelect) opts.onSelect(box);
    }
    el.addEventListener('mousedown', onDown);
    window.addEventListener('mousemove', onMove);
    window.addEventListener('mouseup', onUp);
    return function detach() {
        el.removeEventListener('mousedown', onDown);
        window.removeEventListener('mousemove', onMove);
        window.removeEventListener('mouseup', onUp);
        if (band.parentNode) band.parentNode.removeChild(band);
    };
}
