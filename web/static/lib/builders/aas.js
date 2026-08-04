/* pgwt — pure builder: AAS data -> ECharts option.
 *
 * The Average Active Sessions chart, migrated from ApexCharts to ECharts. This
 * builder is library-shaped only at the edge (it emits an ECharts option); the
 * data->series mapping is pure and Node-testable. It owns NO chart instance and
 * touches NO DOM. The overview view's mount() feeds the option to its ECharts
 * instance; the selection overlay (lib/selection.js) handles drag-zoom, and the
 * option's inside-dataZoom (U2a) handles wheel-zoom + shift-drag pan.
 *
 * Stacked area, one series per wait class (or per event in breakdown mode),
 * x = bucket timestamp, y = AAS. A markLine at numCpus mirrors the old
 * ApexCharts "N CPUs" annotation.
 *
 * ── Time coordinates (U2a, Track U) ─────────────────────────────────────────
 * The x axis is type:'time' with an explicitly pinned [min,max] — NEVER a
 * value axis. Root cause (SSR bisect + real-Chromium verification against the
 * vendored 5.5.1 bundle, 2026-07-31, docs/VISUAL_CHECKLIST.md "First catch"):
 * ECharts drops every stacked layer above series[0] whenever stacked 2D data
 * rides a type:'value' x axis, at ANY x magnitude — the AAS chart had only
 * ever painted series[0]'s area. Only type:'time' renders the full stack.
 * Axis coordinates are UNIX MILLISECONDS; the ns→ms conversion happens
 * exactly once, here, at the option boundary (NS_PER_MS). Everything upstream
 * (camera, strip cache, fidelity/escalation view-model geometry) stays in ns.
 * Axis labels + tooltip use explicit UTC formatters (fmtTime renders via
 * toUTCString): TZ-stable for pixel baselines, per the project's
 * all-times-are-UTC convention (UI-11) — never ECharts' locale defaults.
 */

import { WAIT_CLASSES, classIndex, eventColor, fmtTime, esc } from '../format.js';
import {
    buildFidelityShading, buildEscalationAnnotation, fidelityOf, fidelityLabel,
} from './fidelity.js';

/* Name of the dedicated silent annotation series that carries EVERY mark
 * (fidelity shading, escalation band/edge, the N-CPUs line). It is excluded
 * from seriesNames and legend.data, so no legend/hover state can remove the
 * trust annotations (U1, review P2). */
export const AAS_ANNOTATION_SERIES = 'pgwt-annotations';

/* ns→ms at the option boundary — the ONE conversion site (see header). */
const NS_PER_MS = 1e6;
const nsToMs = (ns) => ns / NS_PER_MS;

/* ── y-axis policy (Track U Phase U2, review P7) ─────────────────────────────
 * yMax = max(maxAas*1.2, min(numCpus*1.5, maxAas*4), 1).
 * The min() term is the P7 cap: the N-CPUs reference line may lift the axis
 * to at most 4x the data peak. Before the cap, a 64-vCPU host idling at
 * AAS≈3 rendered the entire stack in the bottom ~3% of the pane under a
 * dominant red capacity line (HIERARCHY violation — reference dwarfing
 * data). When the cap puts the CPU line off-scale, the builder emits an
 * explicit "N CPUs ↑" affordance at the top edge instead (see below) — the
 * capacity reference is never silently dropped. Duplicated verbatim in
 * lib/uplot-aas.js (the builders stay drop-in interchangeable while both
 * exist); the parity test in tests/web_unit/uplot-aas.test.mjs pins the two
 * implementations together. Hysteresis (grow now, shrink after 3 stable
 * ticks) deliberately does NOT live here: builders are pure per-payload,
 * so the smoothing state belongs to the view (views/active.js). */
function policyYMax(maxAas, numCpus) {
    return Math.max(
        maxAas * 1.2,
        numCpus > 0 ? Math.min(numCpus * 1.5, maxAas * 4) : 0,
        1
    );
}

/* data: aas response { buckets[], bucket_ns, max_aas, breakdown?, series?,
 *                      fidelity, sample_period_ns, fidelity_ranges? }
 * opts: {
 *   numCpus,
 *   win:  { from, to },   // ns — the VISIBLE (camera) window: drives the
 *                         //   dataZoom crop, the escalation live edge, and
 *                         //   (when no axis opt) the pinned axis extent
 *   axis: { from, to },   // ns, OPTIONAL — the pinned axis extent (the
 *                         //   camera's quantized 3x strip skirt). Widening
 *                         //   the axis beyond the window is what gives the
 *                         //   inside-dataZoom pan/zoom-out gestures room:
 *                         //   the dataZoom window can never leave the axis
 *                         //   extent. Always unioned with win so the crop
 *                         //   is never clamped away from the camera.
 *   appliedYMax,          // OPTIONAL — view hysteresis' current axis top;
 *                         //   ECharts' CPU line/chip decision uses THIS top
 *   escalationStatus,
 * }
 * Returns { option, seriesNames, seriesColors, fidelity, shading, axisRange,
 * window } — names/colors drive the external HTML legend; fidelity + shading
 * drive the sampled/exact legend chip and the background band (Phase B5);
 * axisRange/window (ns) let the mount layer map dataZoom percents back to
 * time. */
export function buildAasOption(data, opts) {
    opts = opts || {};
    const numCpus = opts.numCpus || 0;
    const bns = (data && data.bucket_ns) || 0;
    const buckets = (data && data.buckets) || [];

    const isEventBreakdown = data && data.breakdown === 'events' && data.series;

    let seriesDefs, seriesColors, seriesIds;
    if (isEventBreakdown) {
        // U1 (review P2): order series by stable IDENTITY — class (canonical
        // WAIT_CLASSES order, matching class mode), then event name — never by
        // the server's per-window AAS rank. The server re-ranks the top-N
        // every tick, which reshuffled stack positions (and, with the retired
        // index-keyed palette, colors) on every live refresh. Each bucket's
        // aas[] array stays indexed by the SERVER order, so every series keeps
        // its original index for data extraction; per-bucket totals are
        // invariant under any input permutation. Unrecognized class prefixes
        // sort after all known classes.
        const order = data.series.map((s, idx) => ({ name: s.name, idx }));
        order.sort((a, b) => {
            const ca = classIndex(a.name), cb = classIndex(b.name);
            const ra = ca < 0 ? WAIT_CLASSES.length : ca;
            const rb = cb < 0 ? WAIT_CLASSES.length : cb;
            if (ra !== rb) return ra - rb;
            return a.name < b.name ? -1 : a.name > b.name ? 1 : 0;
        });
        seriesDefs = order.map(o => ({
            name: o.name,
            data: buckets.map(b => [nsToMs(b.t), +(b.aas[o.idx] || 0).toFixed(4)]),
        }));
        // Identity-keyed color: a deterministic tint of the class hue (U1
        // color service) — same event, same color, in every view, every tick.
        seriesColors = order.map(o => eventColor(null, o.name));
        // U2 (P3 wire 1): the server's per-series event_id rides along so an
        // AAS band click can drill by event_id (the filter the events table
        // uses); null for servers/fixtures that omit it.
        seriesIds = order.map(o => {
            const id = data.series[o.idx] && data.series[o.idx].event_id;
            return id != null ? id : null;
        });
    } else {
        seriesDefs = WAIT_CLASSES.map(wc => ({
            name: wc.label,
            data: buckets.map(b => [nsToMs(b.t), +(b[wc.key] || 0).toFixed(4)]),
        }));
        seriesColors = WAIT_CLASSES.map(c => c.color);
        seriesIds = WAIT_CLASSES.map(() => null);   // class drills go by name
    }

    const maxAas = (data && data.max_aas) || 0;
    const policyMax = policyYMax(maxAas, numCpus);
    const yMax = opts.appliedYMax != null ? opts.appliedYMax : policyMax;

    // Fallback extent when no window is supplied (Node tests, degenerate
    // callers): the bucket span, or a 1 ms placeholder for empty data (the
    // empty state is never mounted — views clear the chart instead).
    const xMinNs = buckets.length ? buckets[0].t : 0;
    const xMaxNs = buckets.length ? buckets[buckets.length - 1].t : NS_PER_MS;
    const win = opts.win || { from: xMinNs, to: xMaxNs };

    // Pinned axis extent = (opts.axis ∪ win). U0's clamp-to-last-bucket-start
    // plumbing is gone: the axis is pinned to the window (or the wider strip
    // skirt), so marks at win.to are ON the axis by construction — the
    // escalation edge no longer needs rescuing from past-the-axis drops.
    let axisFromNs = (opts.axis && opts.axis.from != null) ? opts.axis.from : win.from;
    let axisToNs = (opts.axis && opts.axis.to != null) ? opts.axis.to : win.to;
    if (win.from < axisFromNs) axisFromNs = win.from;
    if (win.to > axisToNs) axisToNs = win.to;
    if (!(axisToNs > axisFromNs)) axisToNs = axisFromNs + NS_PER_MS;

    const series = seriesDefs.map((s, i) => ({
        name: s.name,
        type: 'line',
        stack: 'aas',
        areaStyle: { opacity: 0.85 },
        lineStyle: { width: 1 },
        symbol: 'none',
        emphasis: { disabled: true },
        color: seriesColors[i],
        // U2 (P3 wire 1): since ECharts 5.2.2 triggerLineEvent packs event
        // data onto the area polygon too, so a click anywhere inside a
        // stacked band reaches chart.on('click') — the fallback renderer's
        // click→drill source (the review's spec'd mechanism; the mount layer
        // adds convertFromPixel + the cumulative-sum walk).
        triggerLineEvent: true,
        data: s.data,
    }));

    // Fidelity shading + escalation annotation ride the dedicated annotation
    // series built below (they paint behind the stacked areas). The sampled/
    // mixed shading covers the FULL rendered extent (the strip skirt): any
    // data the camera can pan onto mid-gesture is shaded wherever it is —
    // the honesty band is view-model geometry, never lost under a camera
    // state (U2a constraint D). The escalation live edge stays at win.to.
    const shading = buildFidelityShading(data, { from: axisFromNs, to: axisToNs });
    // axisMax is a defensive clamp only since U2a: the time axis is pinned to
    // the window/strip extent, so win.to is always on-axis (contrast U0,
    // where the value axis ended at the last bucket START and the escalation
    // edge was dropped by ECharts until clamped, review P2).
    const escAnno = buildEscalationAnnotation(opts.escalationStatus,
        { from: win.from, to: win.to, axisMax: axisToNs });

    // Every emitted mark x must lie within the pinned axis extent — kept as a
    // defensive invariant (an off-axis markLine is dropped entirely, not
    // clipped). clampPoint also performs the single ns→ms conversion for
    // annotation geometry.
    const clampX = (x) => Math.min(Math.max(x, axisFromNs), axisToNs);
    const clampPoint = (pt) => (pt && pt.xAxis != null)
        ? Object.assign({}, pt, { xAxis: nsToMs(clampX(pt.xAxis)) }) : pt;
    const clampPairs = (pairs) => pairs.map(pair => pair.map(clampPoint));

    // markArea: fidelity band(s) first, then the escalation window on top.
    let markAreaData = [];
    if (shading.markArea) markAreaData = markAreaData.concat(clampPairs(shading.markArea.data));
    if (escAnno && escAnno.markArea) markAreaData = markAreaData.concat(clampPairs(escAnno.markArea.data));

    // markLine: CPU reference line plus an optional escalation-edge line. Both
    // live in one markLine.data array (per-entry label/lineStyle) on the
    // annotation series.
    //
    // P7: when the yMax cap puts the capacity line OFF-SCALE (numCpus > yMax),
    // the markLine is not emitted — ECharts drops an off-axis markLine
    // entirely, which is exactly the silent-vanish failure mode — and an
    // explicit "N CPUs ↑" graphic label at the top edge takes its place
    // (`graphic` below). The reference is always one of the two, never absent.
    const cpuLineOffScale = numCpus > 0 && numCpus > yMax;
    const markLineData = [];
    if (numCpus > 0 && !cpuLineOffScale) {
        markLineData.push({
            yAxis: numCpus,
            lineStyle: { color: '#E53935', type: 'dashed', width: 1 },
            label: {
                formatter: numCpus + ' CPUs', position: 'insideEndTop',
                color: '#fff', backgroundColor: '#E53935',
                padding: [2, 5, 2, 5], fontSize: 11,
            },
        });
    }
    if (escAnno && escAnno.to != null) {
        const border = escAnno.isAnomaly
            ? 'rgba(229, 57, 53, 0.8)' : 'rgba(79, 195, 247, 0.7)';
        markLineData.push({
            xAxis: nsToMs(clampX(escAnno.to)),
            lineStyle: { color: border, type: 'solid', width: 1 },
            label: {
                formatter: escAnno.label, position: 'insideStartTop',
                color: '#fff', backgroundColor: border,
                padding: [2, 5, 2, 5], fontSize: 10,
            },
        });
    }

    // U1 (review P2): EVERY mark rides a dedicated SILENT, dataless
    // annotation series — never series[0]. When the marks rode series[0],
    // legend-unselecting or hover-soloing away that series removed the N-CPUs
    // line and the sampled-honesty shading with it (empirically verified via
    // SSR of the vendored bundle). The annotation series' name is excluded
    // from seriesNames and legend.data, so no legend/hover state can touch
    // it. It sits FIRST so the marks keep painting behind the stacked areas.
    if (markAreaData.length || markLineData.length) {
        const anno = {
            name: AAS_ANNOTATION_SERIES,
            type: 'line',
            data: [],
            silent: true,
            showSymbol: false,
        };
        if (markAreaData.length) {
            anno.markArea = { silent: true, data: markAreaData };
        }
        if (markLineData.length) {
            anno.markLine = { silent: true, symbol: 'none', data: markLineData };
        }
        series.unshift(anno);
    }

    const option = {
        backgroundColor: 'transparent',
        animation: false,
        // U2a review F1: label formatters alone are NOT enough — ECharts'
        // time-scale TICK GENERATION uses local calendar boundaries unless
        // useUTC is set at the option root (SSR-verified: a 6h window under
        // TZ=Asia/Kathmandu places hour ticks at :15 without it). Baselines
        // and non-UTC users depend on this line.
        useUTC: true,
        // Hidden legend component so the external HTML legend can drive series
        // visibility via legendSelect/legendUnSelect dispatchAction.
        legend: { show: false, data: seriesDefs.map(s => s.name) },
        grid: { left: 55, right: 20, top: 14, bottom: 28 },
        xAxis: {
            // type:'time' is LOAD-BEARING — see the file header / the First
            // catch section of docs/VISUAL_CHECKLIST.md. min/max pin the axis
            // so it never derives from data (marks at win.to stay on-axis and
            // the dataZoom percent domain is exactly [min, max]).
            type: 'time',
            min: nsToMs(axisFromNs),
            max: nsToMs(axisToNs),
            axisLabel: {
                color: '#888', fontSize: 10,
                // Explicit UTC formatter (constraint B): fmtTime renders via
                // toUTCString, so labels are TZ-stable for pixel baselines —
                // never ECharts' locale/TZ-dependent time-axis defaults.
                formatter: (v) => fmtTime(v * NS_PER_MS, bns),
                hideOverlap: true,
            },
            axisLine: { lineStyle: { color: '#333' } },
            axisTick: { lineStyle: { color: '#333' } },
            splitLine: { show: false },
        },
        yAxis: {
            type: 'value',
            min: 0,
            max: yMax,
            name: 'Active Sessions',
            nameTextStyle: { color: '#888', fontSize: 11 },
            axisLabel: { color: '#888', fontSize: 10, formatter: (v) => v.toFixed(1) },
            splitLine: { lineStyle: { color: '#2a2a4a' } },
        },
        tooltip: {
            trigger: 'axis',
            backgroundColor: '#1e1e3a',
            borderColor: '#333',
            textStyle: { color: '#e0e0e0', fontSize: 12 },
            axisPointer: { type: 'line', lineStyle: { color: '#666', width: 1, type: 'dashed' } },
            formatter: (params) => aasTooltip(params, bns),
        },
        // Inside (gesture) dataZoom — the camera's event source (U2a).
        // filterMode 'none': the full cached strip stays resident and stacked
        // totals never recompute from a filtered subset; the [startValue,
        // endValue] window does the visible cropping against the pinned axis.
        // Wheel = cursor-anchored zoom; shift+drag = pan (plain drag belongs
        // to the brush-select overlay, lib/selection.js — constraint C).
        // minValueSpan mirrors the camera's MIN_SPAN_NS floor (1 ms).
        dataZoom: [{
            type: 'inside',
            xAxisIndex: 0,
            filterMode: 'none',
            zoomOnMouseWheel: true,
            moveOnMouseMove: 'shift',
            moveOnMouseWheel: false,
            preventDefaultMouseMove: true,
            startValue: nsToMs(win.to > win.from ? win.from : axisFromNs),
            endValue: nsToMs(win.to > win.from ? win.to : axisToNs),
            minValueSpan: 1,
        }],
        series,
    };

    // P7 off-scale affordance: the capacity reference as a top-edge chip in
    // the markLine label's exact dress (white on the reference red). Pinned
    // to the grid's top-right corner — the same spot the on-scale label
    // occupies when the line hugs the top. `silent` — it is a reference,
    // not a control.
    if (cpuLineOffScale) {
        option.graphic = [{
            type: 'text',
            right: 24,                       // grid right (20) + breathing room
            top: 16,                         // just under the grid top (14)
            silent: true,
            style: {
                text: numCpus + ' CPUs ↑',
                fill: '#fff',
                backgroundColor: '#E53935',
                padding: [2, 5, 2, 5],
                fontSize: 11,
            },
            z: 100,
        }];
    }

    return {
        option,
        seriesNames: seriesDefs.map(s => s.name),
        seriesColors,
        // U2 (P3 wire 1): parallel to seriesNames — event_id per series in
        // event-breakdown mode, null entries in class mode. The mount layer's
        // click→drill resolves a hit band to its drill intent through these
        // (parity-pinned against buildUplotSpec).
        seriesIds,
        breakdown: isEventBreakdown ? 'events' : 'classes',
        maxAas,
        hasData: buckets.length > 0,
        // Fidelity surface (B5): drives the sampled/exact legend chip + tooltip.
        fidelity: fidelityOf(data),
        fidelityLabel: fidelityLabel(fidelityOf(data)),
        shading,
        escalation: escAnno,
        // U2a camera geometry (ns): the pinned axis extent and the visible
        // window. The mount layer records axisRange to map inside-dataZoom
        // percent events (start/end of [0,100] over the axis extent) back to
        // absolute time for the camera.
        axisRange: { from: axisFromNs, to: axisToNs },
        window: { from: win.from, to: win.to },
    };
}

/* Pure tooltip renderer (exported for testing). `params` is the ECharts axis
 * tooltip param array; value[0] is the bucket time in AXIS units — ms on the
 * U2a time axis — converted back to ns for the UTC fmtTime rendering. */
export function aasTooltip(params, bns) {
    if (!params || !params.length) return '';
    const t = fmtTime(params[0].value[0] * NS_PER_MS, bns);
    let total = 0;
    const items = [];
    for (const p of params) {
        const val = (p.value && p.value[1]) || 0;
        if (val > 0.001) {
            items.push({ name: p.seriesName, value: val, color: p.color });
            total += val;
        }
    }
    items.reverse();  // top-of-stack first, matching the old ApexCharts order
    let html = '<div style="padding:4px"><b>' + t + '</b><br>Total AAS: <b>' +
               total.toFixed(2) + '</b><br>';
    for (const it of items) {
        const pct = total > 0 ? (it.value / total * 100).toFixed(0) : '0';
        html += '<span style="color:' + it.color + '">●</span> ' + esc(it.name) +
                ': <b>' + it.value.toFixed(2) + '</b> (' + pct + '%)<br>';
    }
    html += '</div>';
    return html;
}
