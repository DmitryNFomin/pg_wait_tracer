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
    let startX = 0;

    function localX(e) {
        return e.clientX - el.getBoundingClientRect().left;
    }

    function onDown(e) {
        // Shift+drag is the camera pan (inside-dataZoom moveOnMouseMove:
        // 'shift', U2a) — not a brush. See the header's gesture split.
        if (e.button !== 0 || e.shiftKey) return;
        dragging = true;
        startX = localX(e);
        band.style.left = startX + 'px';
        band.style.width = '0px';
        band.style.display = 'block';
    }

    function onMove(e) {
        if (!dragging) return;
        const x = localX(e);
        const lo = Math.min(startX, x);
        const hi = Math.max(startX, x);
        band.style.left = lo + 'px';
        band.style.width = (hi - lo) + 'px';
    }

    function onUp(e) {
        if (!dragging) return;
        dragging = false;
        band.style.display = 'none';
        const endX = localX(e);
        if (Math.abs(endX - startX) < minDrag) return;  // a click, not a drag
        const range = pixelRangeToTime(startX, endX, chart);
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
