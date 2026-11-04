/* pgwt — pure builder: heatmap data -> ECharts option.
 *
 * The histogram view's latency-over-time heatmap. Buckets on x (time) and
 * latency-band on y, color = event count. This builder is library-shaped only
 * at the edge (it emits an ECharts option); the data->cell mapping is pure and
 * Node-testable. It owns NO chart instance and touches NO DOM — the view's
 * mount() feeds the option to its ECharts instance.
 *
 * Mapping (P8 — the log1p/single-hue fix; the pre-U2 shape was the linear
 * rainbow the review called "the heatmap looks weird"):
 *   - x category labels = formatted bucket times (fmtTime with bucket_ns)
 *   - y category labels = the server-supplied latency-band labels
 *   - series data = [timeIdx, latIdx, log1p(count), count] — COLOR maps the
 *     log (dimension 2), the tooltip and visualMap labels keep RAW counts
 *   - visualMap max = log1p(opts.maxCount || max_count); the view pins
 *     maxCount sticky across live ticks (no per-tick global recolor)
 */

import { fmtTime, fmtCount, esc } from '../format.js';

/* P8: single-hue perceptually-ordered ramp — violet (H≈288), monotone
 * lightness, dark→light = few→many on the dark surface. Replaces the 6-stop
 * blue/green/red/orange/yellow rainbow whose hues collided with 5 of the
 * semantic class hues: a hot cell must read as "more", never as "Lock".
 * Violet is the one hue family WAIT_CLASSES leaves free (nearest are
 * Activity purple and LWLock pink, ≥25° away — and a lightness ramp cannot
 * be read as a flat identity tone). Validated with the ordinal-ramp checks
 * (monotone OKLCH L, adjacent ΔL ≥ 0.06, dark end ≥ 2:1 vs the #1a1a2e
 * surface, single hue). Part of the visual contract: changing it recolors
 * every heatmap baseline. */
const HEATMAP_COLORS =
    ['#763e84', '#a04ab5', '#bb6ecf', '#d496e3', '#eac3f4'];

/* #105: dedicated silent annotation series name, mirroring aas.js
 * AAS_ANNOTATION_SERIES — the "not captured" markArea rides its own dataless
 * series (never the heatmap series) so it is unaffected by any future
 * legend/hover state on the data itself. */
export const HEATMAP_ANNOTATION_SERIES = 'pgwt-heatmap-annotations';
export const NOT_CAPTURED_BAND_COLOR = 'rgba(136, 136, 136, 0.10)';
export const NOT_CAPTURED_BORDER = 'rgba(136, 136, 136, 0.45)';

/* data: heatmap response { bucket_ns, max_count, times[], labels[], cells[] }
 * opts.maxCount (optional): sticky visualMap ceiling in RAW-count domain —
 *   the view recomputes it on window/filter change, not per live tick (P8).
 * opts.captureFromNs (optional, #105): the server's earliest captured
 *   timestamp (ctx.server.fromNs). Sparse cells already leave a pre-capture
 *   bucket blank (the server only emits non-zero cells — src/server.c
 *   handle_heatmap), but a bare blank region reads the same as "captured,
 *   zero events" — indistinguishable from real no-data. A labeled markArea
 *   makes the distinction explicit, consistent with the concurrency chart.
 * Returns { option, hasData, rawMax, notCapturedCount }. hasData is false for
 * an empty/absent grid so the view can paint its "No data" placeholder
 * instead of an empty chart; rawMax is this response's own max (the view's
 * sticky-max input). */
export function buildHeatmapOption(data, opts) {
    opts = opts || {};
    if (!data || !data.cells || data.cells.length === 0) {
        return { option: null, hasData: false, rawMax: 0, notCapturedCount: 0 };
    }
    const hbns = data.bucket_ns || 0;
    const rawTimes = data.times || [];
    const timeLabels = rawTimes.map(t => fmtTime(t, hbns));
    const latLabels = data.labels || [];
    const captureFromNs = Number.isFinite(opts.captureFromNs) ? opts.captureFromNs : null;
    const notCapturedCount = captureFromNs != null
        ? rawTimes.filter(t => t < captureFromNs).length : 0;
    // P8: log2 latency bands make heavy-tailed counts structural — a linear
    // color map collapses everything but the hottest cell into the darkest
    // stops. Color therefore maps log1p(count); the raw count rides along in
    // dimension 3 for the tooltip.
    const hmData = data.cells.map(c => [c[0], c[1], Math.log1p(c[2]), c[2]]);
    const rawMax = data.max_count || 1;
    const vmaxRaw = opts.maxCount != null ? opts.maxCount : rawMax;

    const series = [{
        type: 'heatmap', data: hmData,
        emphasis: { itemStyle: { borderColor: '#fff', borderWidth: 1 } },
        itemStyle: { borderWidth: 0 },
    }];
    if (notCapturedCount > 0) {
        // #105: appended, never unshifted — series[0].type === 'heatmap' is a
        // load-bearing contract several test harnesses assert directly
        // (tests/ui_live_smoke.py, tests/test_web_ui.py); the sparse grid
        // already has no cells to occlude in the pre-capture range, so
        // z-order here is cosmetically irrelevant anyway.
        series.push({
            name: HEATMAP_ANNOTATION_SERIES, type: 'line', data: [], silent: true,
            showSymbol: false,
            markArea: {
                silent: true,
                label: { show: true, position: 'insideTop', color: '#999',
                    fontSize: 10, formatter: 'Not captured' },
                data: [[
                    { xAxis: 0, itemStyle: { color: NOT_CAPTURED_BAND_COLOR,
                        borderColor: NOT_CAPTURED_BORDER, borderWidth: 1,
                        borderType: 'dashed' } },
                    { xAxis: notCapturedCount - 1 },
                ]],
            },
        });
    }

    const option = {
        backgroundColor: 'transparent',
        animation: false,
        tooltip: {
            position: 'top', backgroundColor: '#1e1e3a', borderColor: '#333',
            textStyle: { color: '#e0e0e0', fontSize: 12 },
            formatter: (p) => '<b>' + esc(timeLabels[p.data[0]]) + '</b><br>' +
                'Latency: ' + esc(latLabels[p.data[1]]) + '<br>' +
                'Count: <b>' + p.data[3].toLocaleString() + '</b>',
        },
        grid: { left: 90, right: 40, top: 10, bottom: 60 },
        xAxis: {
            type: 'category', data: timeLabels,
            axisLabel: { color: '#888', fontSize: 10 },
            axisLine: { lineStyle: { color: '#333' } }, splitArea: { show: false },
        },
        yAxis: {
            type: 'category', data: latLabels,
            axisLabel: { color: '#888', fontSize: 10 },
            axisLine: { lineStyle: { color: '#333' } }, splitArea: { show: false },
        },
        // #105: piecewise legend with numeric bucket labels (the matrix fix,
        // PR #147) — the old `continuous` type rendered a bare gradient bar;
        // ECharts' continuous min/max end-labels were getting clipped out by
        // the pane width, leaving no numbers at all. Piecewise buckets always
        // draw their own label beside each swatch.
        visualMap: {
            // dimension 2 = log1p(count); labels map back to raw counts so
            // the legend never shows log values.
            type: 'piecewise', min: 0, max: Math.log1p(vmaxRaw), splitNumber: 5,
            dimension: 2, orient: 'horizontal', left: 'center', bottom: 0,
            textStyle: { color: '#888', fontSize: 10 },
            formatter: (min, max) => {
                const lo = fmtCount(Math.round(Math.expm1(min)));
                if (max == null || !Number.isFinite(Number(max))) return lo;
                const hi = fmtCount(Math.round(Math.expm1(max)));
                return lo === hi ? lo : lo + '–' + hi;
            },
            inRange: { color: HEATMAP_COLORS },
        },
        series,
    };
    return { option, hasData: true, rawMax, notCapturedCount };
}

/* PURE: derive the class/event selector model from a top_events row list.
 *
 * Returns { classes: [string], eventsByClass: { class: [{event_id,label}] },
 *   allEvents: [{event_id,label}] }. Mirrors the old populateSelectors /
 *   populateEventSelect logic (distinct classes, up to 50 events, name+count
 *   label) without touching the DOM. */
export function buildSelectorModel(events, fmtCount) {
    const evs = events || [];
    const classes = [];
    const seen = new Set();
    for (const r of evs) {
        if (r.class && !seen.has(r.class)) { seen.add(r.class); classes.push(r.class); }
    }
    const label = (ev) => ev.name + ' (' + fmtCount(ev.count) + ')';
    const toOpt = (ev) => ({ event_id: ev.event_id, label: label(ev) });
    const allEvents = evs.slice(0, 50).map(toOpt);
    const eventsByClass = {};
    for (const cls of classes) {
        eventsByClass[cls] = evs.filter(r => r.class === cls).slice(0, 50).map(toOpt);
    }
    return { classes, eventsByClass, allEvents };
}
