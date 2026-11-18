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
import { coverageOf, fidelityBandStyle, INCOMPLETE_FIDELITY } from './fidelity.js';

/* U3: dedicated series for the server's uncovered ranges (coverage verdict). */
export const HEATMAP_COVERAGE_SERIES = 'pgwt-heatmap-coverage';

/* Bucket indices whose [t, t + bucket_ns) meets an uncovered range with a
 * non-empty interior; their cells are missing, never a measured count. */
export function uncoveredBucketIndices(times, bucketNs, uncovered) {
    const out = [];
    if (!uncovered || !uncovered.length) return out;
    times.forEach((t, i) => {
        const end = t + (bucketNs > 0 ? bucketNs : 0);
        if (uncovered.some(u => u.from < end && u.to > t)) out.push(i);
    });
    return out;
}

/* The colour-scale input in the raw-count domain. A withheld window has no
 * whole-window max_count; the scale then follows the cells outside the
 * uncovered buckets. Shared with the view's sticky max (views/histogram.js). */
export function heatmapScaleMax(data) {
    if (!data) return 1;
    if (Number.isFinite(data.max_count) && data.max_count > 0) return data.max_count;
    const cov = coverageOf(data);
    if (!(cov && cov.withheld)) return data.max_count || 1;
    const skip = new Set(uncoveredBucketIndices(data.times || [], data.bucket_ns || 0, cov.uncovered));
    let max = 1;
    for (const c of data.cells || []) {
        if (!skip.has(c[0]) && Number.isFinite(c[2]) && c[2] > max) max = c[2];
    }
    return max;
}

function indexRuns(idx) {
    const runs = [];
    for (const i of idx) {
        const last = runs[runs.length - 1];
        if (last && i === last[1] + 1) last[1] = i;
        else runs.push([i, i]);
    }
    return runs;
}

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
    const cov = coverageOf(data);
    const uncoveredIdx = uncoveredBucketIndices(rawTimes, hbns, cov ? cov.uncovered : []);
    const uncoveredSet = new Set(uncoveredIdx);
    const cells = data.cells.filter(c => !uncoveredSet.has(c[0]));
    const hmData = cells.map(c => [c[0], c[1], Math.log1p(c[2]), c[2]]);
    const rawMax = heatmapScaleMax(data);
    const vmaxRaw = opts.maxCount != null ? opts.maxCount : rawMax;

    const series = [{
        type: 'heatmap', data: hmData,
        // Single-pass paint, so the mount anchor can never be published
        // mid-fill. This is a GUARANTEE, not the cure for an observed live
        // blink: no live pixel divergence was ever measured against it (see
        // below).
        //
        // ECharts' GLOBAL defaults are progressive: 400 /
        // progressiveThreshold: 3000 (vendor echarts.min.js 5.6.0; heatmap
        // overrides neither), so past 3000 cells the heatmap renders
        // incrementally, 400 cells per animation frame. view-manager.js
        // records lastMount.at AFTER view.mount() returns, i.e. after chunk 1
        // of N — every later chunk paints AFTER the anchor the live-UI-smoke
        // sweep is anchored on. Measured in a browser against this builder
        // (tests/results/hist_blink/mech_check.py): without this line, 3500
        // cells -> getProgressive()=400, 9 paints, 8 of them after setOption
        // returned, at +33..+119 ms; 4600 cells (a demo-length window) -> 12
        // paints, the last at +168 ms. With it: getProgressive()=0, exactly
        // 1 paint, at +35 ms.
        //
        // That window stayed under the sweep's first 200 ms frame on the
        // hardware we could measure (gate-2, ~3200 cells: 16/16 ticks clean
        // on both builders), so this change fixes no blink anyone has seen in
        // pixels. It removes the multi-frame window itself, which is cheap:
        // ~5k plain rects paint in one frame (35 ms above), and animation is
        // already off.
        progressive: 0,
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

    if (uncoveredIdx.length) {
        const style = fidelityBandStyle(INCOMPLETE_FIDELITY);
        series.push({
            name: HEATMAP_COVERAGE_SERIES, type: 'line', data: [], silent: true,
            showSymbol: false,
            markArea: {
                silent: true,
                label: { show: true, position: 'insideTop', color: '#999',
                    fontSize: 10, formatter: style.label },
                data: indexRuns(uncoveredIdx).map(([a, b]) => [
                    { xAxis: a, itemStyle: { color: style.fill,
                        borderColor: style.border, borderWidth: 1,
                        borderType: 'dashed' } },
                    { xAxis: b },
                ]),
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
    return { option, hasData: true, rawMax, notCapturedCount,
        coverage: cov, uncoveredCount: uncoveredIdx.length };
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
