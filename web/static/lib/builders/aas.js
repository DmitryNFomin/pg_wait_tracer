/* pgwt — pure builder: AAS data -> ECharts option.
 *
 * The Average Active Sessions chart, migrated from ApexCharts to ECharts. This
 * builder is library-shaped only at the edge (it emits an ECharts option); the
 * data->series mapping is pure and Node-testable. It owns NO chart instance and
 * touches NO DOM. The overview view's mount() feeds the option to its ECharts
 * instance; the selection overlay (lib/selection.js) handles drag-zoom.
 *
 * Stacked area, one series per wait class (or per event in breakdown mode),
 * x = bucket timestamp (ns, numeric), y = AAS. A markLine at numCpus mirrors
 * the old ApexCharts "N CPUs" annotation.
 */

import { WAIT_CLASSES, EVENT_PALETTE, fmtTime, esc } from '../format.js';
import {
    buildFidelityShading, buildEscalationAnnotation, fidelityOf, fidelityLabel,
} from './fidelity.js';

/* data: aas response { buckets[], bucket_ns, max_aas, breakdown?, series?,
 *                      fidelity, sample_period_ns, fidelity_ranges? }
 * opts: { numCpus, win:{from,to}, escalationStatus }
 * Returns { option, seriesNames, seriesColors, fidelity, shading } — names/
 * colors drive the external HTML legend; fidelity + shading drive the
 * sampled/exact legend chip and the background band (Phase B5). */
export function buildAasOption(data, opts) {
    opts = opts || {};
    const numCpus = opts.numCpus || 0;
    const bns = (data && data.bucket_ns) || 0;
    const buckets = (data && data.buckets) || [];

    const isEventBreakdown = data && data.breakdown === 'events' && data.series;

    let seriesDefs, seriesColors;
    if (isEventBreakdown) {
        seriesDefs = data.series.map((s, idx) => ({
            name: s.name,
            data: buckets.map(b => [b.t, +(b.aas[idx] || 0).toFixed(4)]),
        }));
        seriesColors = data.series.map((_, idx) => EVENT_PALETTE[idx % EVENT_PALETTE.length]);
    } else {
        seriesDefs = WAIT_CLASSES.map(wc => ({
            name: wc.label,
            data: buckets.map(b => [b.t, +(b[wc.key] || 0).toFixed(4)]),
        }));
        seriesColors = WAIT_CLASSES.map(c => c.color);
    }

    const maxAas = (data && data.max_aas) || 0;
    const yMax = Math.max(
        maxAas * 1.2,
        numCpus > 0 ? numCpus * 1.5 : 0,
        1
    );

    const xMin = buckets.length ? buckets[0].t : 0;
    const xMax = buckets.length ? buckets[buckets.length - 1].t : 1;

    const series = seriesDefs.map((s, i) => ({
        name: s.name,
        type: 'line',
        stack: 'aas',
        areaStyle: { opacity: 0.85 },
        lineStyle: { width: 1 },
        symbol: 'none',
        emphasis: { disabled: true },
        color: seriesColors[i],
        data: s.data,
    }));

    // Fidelity shading + escalation annotation are attached to the first
    // series' markArea / markLine (they paint behind the stacked areas). The
    // window for full-extent bands comes from opts.win, falling back to the
    // bucket span so a sampled window shades end-to-end even with sparse data.
    const win = opts.win || { from: xMin, to: xMax };
    const shading = buildFidelityShading(data, win);
    // axisMax pins the escalation marks ON the axis: the x-axis ends at the
    // last bucket START (xMax) < win.to, and ECharts DROPS a markLine placed
    // past the axis max — the escalation edge never rendered (review P2,
    // fixed in U0). A value time axis that removes the clamp is Phase U2.
    const escAnno = buildEscalationAnnotation(opts.escalationStatus,
        { from: win.from, to: win.to, axisMax: xMax });

    // Every emitted mark x must lie within [xMin, xMax]. For markAreas the
    // clamp is pixel-identical (ECharts clips them to the plot edge anyway);
    // it exists so the on-axis invariant holds for every mark we emit.
    const clampX = (x) => Math.min(Math.max(x, xMin), xMax);
    const clampPoint = (pt) => (pt && pt.xAxis != null)
        ? Object.assign({}, pt, { xAxis: clampX(pt.xAxis) }) : pt;
    const clampPairs = (pairs) => pairs.map(pair => pair.map(clampPoint));

    // markArea: fidelity band(s) first, then the escalation window on top.
    let markAreaData = [];
    if (shading.markArea) markAreaData = markAreaData.concat(clampPairs(shading.markArea.data));
    if (escAnno && escAnno.markArea) markAreaData = markAreaData.concat(clampPairs(escAnno.markArea.data));

    // markLine: CPU reference line plus an optional escalation-edge line. Both
    // live in one markLine.data array (per-entry label/lineStyle) so we keep a
    // single markLine per series — no extra series that would pollute the
    // legend or the "series with data" assertions.
    const markLineData = [];
    if (numCpus > 0) {
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
            xAxis: clampX(escAnno.to),
            lineStyle: { color: border, type: 'solid', width: 1 },
            label: {
                formatter: escAnno.label, position: 'insideStartTop',
                color: '#fff', backgroundColor: border,
                padding: [2, 5, 2, 5], fontSize: 10,
            },
        });
    }

    if (series.length) {
        if (markAreaData.length) {
            series[0].markArea = { silent: true, data: markAreaData };
        }
        if (markLineData.length) {
            series[0].markLine = { silent: true, symbol: 'none', data: markLineData };
        }
    }

    const option = {
        backgroundColor: 'transparent',
        animation: false,
        // Hidden legend component so the external HTML legend can drive series
        // visibility via legendSelect/legendUnSelect dispatchAction.
        legend: { show: false, data: seriesDefs.map(s => s.name) },
        grid: { left: 55, right: 20, top: 14, bottom: 28 },
        xAxis: {
            type: 'value',
            min: xMin,
            max: xMax,
            axisLabel: {
                color: '#888', fontSize: 10,
                formatter: (v) => fmtTime(v, bns),
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
        series,
    };

    return {
        option,
        seriesNames: seriesDefs.map(s => s.name),
        seriesColors,
        maxAas,
        hasData: buckets.length > 0,
        // Fidelity surface (B5): drives the sampled/exact legend chip + tooltip.
        fidelity: fidelityOf(data),
        fidelityLabel: fidelityLabel(fidelityOf(data)),
        shading,
        escalation: escAnno,
    };
}

/* Pure tooltip renderer (exported for testing). `params` is the ECharts axis
 * tooltip param array. */
export function aasTooltip(params, bns) {
    if (!params || !params.length) return '';
    const t = fmtTime(params[0].value[0], bns);
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
