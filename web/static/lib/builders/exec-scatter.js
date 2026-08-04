/* pgwt — pure builder: execution latency over time -> ECharts scatter. */

import { fmtTime, fmtMs, esc } from '../format.js';

function ns(v) {
    const n = Number(v);
    return Number.isFinite(n) ? n : null;
}

export function scatterTooltipFormatter(params) {
    const d = params.data;
    return '<b>PID ' + esc(String(d[2])) + '</b><br>' +
        'Start: ' + fmtTime(ns(d[4]), 1_000_000) + ' UTC<br>' +
        'Duration: <b>' + fmtMs(d[1]) + '</b><br>' +
        'Query: ' + esc(String(d[3] || '0'));
}

/* Exact execution identity intent. start/end stay decimal strings. The
 * waterfall reconciles it with the authoritative executions row before
 * requesting detail, avoiding double-rounded duration bounds. */
export function scatterExecutionIntent(tuple) {
    if (!tuple) return null;
    const start = String(tuple[4]);
    let end = null;
    try {
        end = (BigInt(start) + BigInt(Math.round(Number(tuple[1]) * 1e6))).toString();
    } catch (e) { return null; }
    return { pivot: 'scatter-execution', pid: Number(tuple[2]),
        start_ns: start, end_ns: end, query_id: String(tuple[3] || '0') };
}

/* Is a box substantially narrower than the full logarithmic y domain? */
export function meaningfulScatterYRange(yFrom, yTo, domainMin, domainMax) {
    if (!(yFrom > 0) || !(yTo > yFrom) || !(domainMin > 0) || !(domainMax > domainMin))
        return false;
    const selected = Math.log(yTo) - Math.log(yFrom);
    const full = Math.log(domainMax) - Math.log(domainMin);
    return selected / full < 0.8;
}

export function buildExecScatterOption(data) {
    const points = (data && data.points) || [];
    let excludedCount = 0, inProgressCount = 0, zeroDurationCount = 0;
    const plotted = [];
    for (const p of points) {
        const t = ns(p.t), duration = Number(p.duration_ms);
        if (t == null || p.duration_ms == null || !(duration > 0)) {
            excludedCount++;
            if (p.in_progress === true) inProgressCount++;
            else zeroDurationCount++;
            continue;
        }
        plotted.push([t / 1e6, duration, p.pid, p.query_id, String(p.t)]);
    }
    const notes = [];
    if (data && data.downsampled === true &&
        typeof data.kept_count === 'number' && typeof data.total_count === 'number') {
        notes.push('Showing ' + data.kept_count.toLocaleString() + ' of ' +
            data.total_count.toLocaleString() + ' executions (server-downsampled)');
    }
    if (inProgressCount)
        notes.push(inProgressCount.toLocaleString() + ' in-progress execution' +
            (inProgressCount === 1 ? '' : 's') + ' excluded from the log axis');
    if (zeroDurationCount)
        notes.push(zeroDurationCount.toLocaleString() + ' zero/unknown-duration execution' +
            (zeroDurationCount === 1 ? '' : 's') + ' excluded from the log axis');
    if (!plotted.length) {
        return { option: null, hasData: false, points: [], excludedCount,
            inProgressCount, zeroDurationCount,
            notes, downsampled: !!(data && data.downsampled), yMin: null, yMax: null };
    }
    const ys = plotted.map(p => p[1]);
    const yMin = Math.min(...ys), yMax = Math.max(...ys);
    const option = {
        backgroundColor: 'transparent', animation: false, useUTC: true,
        tooltip: {
            trigger: 'item', backgroundColor: '#1e1e3a', borderColor: '#333',
            textStyle: { color: '#e0e0e0', fontSize: 12 },
            formatter: scatterTooltipFormatter,
        },
        grid: { left: 78, right: 26, top: 34, bottom: 52 },
        xAxis: {
            type: 'time', boundaryGap: ['3%', '3%'],
            axisLabel: { color: '#888', fontSize: 10,
                formatter: (v) => fmtTime(Number(v) * 1e6, 1_000_000_000) },
            axisLine: { lineStyle: { color: '#333' } },
            splitLine: { lineStyle: { color: '#292944' } },
        },
        yAxis: {
            type: 'log', name: 'Duration (ms)', nameTextStyle: { color: '#777' },
            axisLabel: { color: '#888', fontSize: 10 },
            axisLine: { lineStyle: { color: '#333' } },
            splitLine: { lineStyle: { color: '#292944' } },
        },
        series: [{
            name: 'Executions', type: 'scatter', large: true, largeThreshold: 500,
            symbolSize: 7, itemStyle: { color: '#4fc3f7', opacity: 0.72 },
            emphasis: { itemStyle: { color: '#fff', opacity: 1 } }, data: plotted,
        }],
    };
    return { option, hasData: true, points: plotted, excludedCount, notes,
        inProgressCount, zeroDurationCount,
        downsampled: !!(data && data.downsampled), yMin, yMax };
}
