/* pgwt — pure builders for the per-execution waterfall (the 10046 view).
 *
 * The custom-series tuple deliberately carries both clipped draw geometry and
 * raw values:
 *   [drawStart, drawEnd, lane, name, eventId, rawStart, rawDuration,
 *    cpuNs, pid, kind, color]
 * `kind` is "event" or "plan".  The view owns ECharts and all gestures; this
 * module only maps server data to option/table/readout models.
 */

import { eventColor, fmtTimeNs, fmtUs, fmtMs, esc } from '../format.js';
import { buildTableModel, buildTruncationRow } from '../table.js';

export const PLAN_COLOR = '#9b7bd3';

function ns(v) {
    if (v == null || v === '') return null;
    const n = Number(v);
    return Number.isFinite(n) ? n : null;
}

function nsBig(v) {
    if (v == null || v === '') return null;
    try { return BigInt(v); } catch (e) { return null; }
}

function sameExecution(a, b) {
    return !!(a && b && Number(a.pid) === Number(b.pid) &&
        String(a.start_ns) === String(b.start_ns));
}

export const executionsConfig = {
    columns: [
        { key: 'start_ns', label: 'Start (UTC)', format: (r) =>
            (r.started_before_window
                ? '<span class="execution-prewindow" title="Started before selected window">↤</span> '
                : '') + fmtTimeNs(r.start_ns, 1_000_000) },
        { key: 'pid', label: 'Leader PID', cls: 'num', format: (r) => String(r.pid) },
        { key: 'query_id', label: 'Query ID', format: (r) =>
            '<span class="query-id">' + esc(String(r.query_id || '0')) + '</span>' },
        { key: 'duration_ms', label: 'Duration', cls: 'num', format: (r) =>
            r.in_progress || r.duration_ms == null
                ? '<span class="execution-open">In progress</span>' : fmtMs(r.duration_ms) },
        { key: 'plan_ms', label: 'Plan', cls: 'num', format: (r) => fmtMs(r.plan_ms) },
        { key: 'n_events', label: 'Leader events', cls: 'num', format: (r) => String(r.n_events) },
        { key: 'n_workers', label: 'Workers', cls: 'num', format: (r) => String(r.n_workers) },
    ],
    rowClass: (r) => 'clickable' + (r._selected ? ' selected-execution' : ''),
};

/* executions response -> shared-table model. Server order is latest-first and
 * is preserved; client sorting is intentionally absent on this selector. */
export function buildExecutionsModel(data, selected) {
    const rows = ((data && data.rows) || []).map(r =>
        Object.assign({}, r, { _selected: sameExecution(r, selected) }));
    return {
        hasRows: rows.length > 0,
        table: buildTableModel(executionsConfig, rows, null),
        truncation: buildTruncationRow(Object.assign({}, data, { rows })),
        count: rows.length,
        total_count: data && typeof data.total_count === 'number'
            ? data.total_count : null,
        truncated: !!(data && data.truncated),
    };
}

export function waterfallRenderItem(params, api) {
    const start = api.coord([api.value(0), api.value(2)]);
    const end = api.coord([api.value(1), api.value(2)]);
    const band = api.size([0, 1])[1];
    const kind = api.value(9);
    const color = api.value(10) || '#888';
    const height = band * (kind === 'plan' ? 0.82 : 0.58);
    return {
        type: 'rect',
        shape: {
            x: start[0], y: start[1] - height / 2,
            width: Math.max(end[0] - start[0], 1), height,
        },
        style: kind === 'plan'
            ? { fill: color, opacity: 0.55, stroke: '#cbb8ef', lineWidth: 1 }
            : { fill: color },
        styleEmphasis: { fill: color, opacity: 0.9, stroke: '#fff', lineWidth: 1 },
    };
}

export function waterfallTooltipFormatter(params) {
    const d = params.data;
    let html = '<b>' + esc(d[3]) + '</b><br>' +
        'Lane: PID ' + esc(String(d[8])) + '<br>' +
        'Start: ' + fmtTimeNs(d[5], 1_000_000) + ' UTC<br>' +
        'Duration: <b>' + fmtUs(ns(d[6]) / 1000) + '</b>';
    if (d[7] != null) html += '<br>Measured CPU: ' + fmtUs(ns(d[7]) / 1000);
    return html;
}

/* A compact, DOM-free inspection model used by the waterfall click readout. */
export function buildWaterfallReadout(tuple) {
    if (!tuple) return null;
    return {
        name: String(tuple[3]),
        pid: Number(tuple[8]),
        start: fmtTimeNs(tuple[5], 1_000_000) + ' UTC',
        duration: fmtUs(ns(tuple[6]) / 1000),
        cpu: tuple[7] == null ? null : fmtUs(ns(tuple[7]) / 1000),
        kind: tuple[9],
    };
}

function laneList(data) {
    if (!data || !data.leader) return [];
    return [data.leader].concat(data.workers || []);
}

/* execution_detail response -> ECharts custom-series option.
 * opts: { from, to, executionStart, executionEnd }. Missing from/to derives a
 * full extent from the resident payload (including a pre-execution plan).
 */
export function buildWaterfallOption(data, opts) {
    opts = opts || {};
    const lanes = laneList(data);
    if (!lanes.length) {
        return { option: null, hasData: false, lanes: [], bars: [],
            fullFrom: null, fullTo: null, count: 0, total_count: 0,
            kept_count: 0, truncated: false, laneTruncations: [] };
    }

    const labels = lanes.map((l, i) => 'PID ' + l.pid + (i === 0 ? ' (leader)' : ''));
    const raw = [];
    lanes.forEach((lane, laneIdx) => {
        (lane.events || []).forEach(ev => {
            const start = nsBig(ev.start_ns), dur = nsBig(ev.dur_ns);
            if (start == null || dur == null) return;
            raw.push({ start, end: start + dur, laneIdx, name: ev.name,
                eventId: ev.we, rawStart: ev.start_ns, rawDuration: ev.dur_ns,
                cpu: ev.cpu_ns, pid: lane.pid, kind: 'event',
                color: eventColor(null, ev.name) });
        });
    });
    if (data && data.plan) {
        const start = nsBig(data.plan.start_ns), end = nsBig(data.plan.end_ns);
        if (start != null && end != null && end >= start) {
            raw.push({ start, end, laneIdx: 0, name: 'Plan phase', eventId: null,
                rawStart: String(data.plan.start_ns), rawDuration: String(end - start),
                cpu: null, pid: lanes[0].pid, kind: 'plan', color: PLAN_COLOR });
        }
    }

    const extentStarts = raw.map(b => b.start);
    const extentEnds = raw.map(b => b.end);
    const execStart = nsBig(opts.executionStart), execEnd = nsBig(opts.executionEnd);
    if (execStart != null) extentStarts.push(execStart);
    if (execEnd != null) extentEnds.push(execEnd);
    const fullFromNs = extentStarts.length
        ? extentStarts.reduce((a, b) => a < b ? a : b) : execStart;
    const fullToNs = extentEnds.length
        ? extentEnds.reduce((a, b) => a > b ? a : b) : execEnd;
    const fromNs = opts.from != null ? nsBig(opts.from) : fullFromNs;
    const toNs = opts.to != null ? nsBig(opts.to) : fullToNs;
    const origin = fullFromNs;
    const axis = (v) => Number(v - origin);
    const fullFrom = fullFromNs == null ? null : String(fullFromNs);
    const fullTo = fullToNs == null ? null : String(fullToNs);

    const bars = raw.filter(b => fromNs == null || toNs == null ||
        (b.end >= fromNs && b.start <= toNs)).map(b => [
        axis(fromNs == null || b.start > fromNs ? b.start : fromNs),
        axis(toNs == null || b.end < toNs ? b.end : toNs),
        b.laneIdx, b.name, b.eventId, b.rawStart, b.rawDuration, b.cpu,
        b.pid, b.kind, b.color,
    ]);

    const laneTruncations = lanes.map(l => ({
        pid: l.pid,
        kept_count: (l.events || []).length,
        total_count: typeof l.total_count === 'number' ? l.total_count : null,
        truncated: !!l.truncated,
    })).filter(l => l.truncated);
    const totalCount = data && typeof data.total_count === 'number' ? data.total_count : null;
    const keptCount = data && typeof data.kept_count === 'number'
        ? data.kept_count : raw.filter(b => b.kind === 'event').length;

    if (!raw.length || fromNs == null || toNs == null || !(toNs > fromNs) ||
        origin == null) {
        return { option: null, hasData: false, lanes: labels, bars,
            fullFrom, fullTo, count: keptCount, total_count: totalCount,
            kept_count: keptCount, truncated: !!(data && data.truncated),
            laneTruncations, axisOrigin: origin == null ? null : String(origin),
            windowFrom: fromNs == null ? null : String(fromNs),
            windowTo: toNs == null ? null : String(toNs) };
    }

    const option = {
        backgroundColor: 'transparent', animation: false, useUTC: true,
        tooltip: {
            trigger: 'item', backgroundColor: '#1e1e3a', borderColor: '#333',
            textStyle: { color: '#e0e0e0', fontSize: 12 },
            formatter: waterfallTooltipFormatter,
        },
        grid: { left: 145, right: 24, top: 22, bottom: 44 },
        xAxis: {
            type: 'value', min: axis(fromNs), max: axis(toNs),
            axisLabel: { color: '#888', fontSize: 10,
                formatter: (v) => fmtTimeNs(origin + BigInt(Math.round(v)), 1_000_000) },
            axisLine: { lineStyle: { color: '#333' } },
        },
        yAxis: {
            type: 'category', data: labels,
            axisLabel: { color: '#aaa', fontSize: 11 },
            axisLine: { lineStyle: { color: '#333' } },
        },
        series: [{
            name: 'Execution', type: 'custom', renderItem: waterfallRenderItem,
            clip: true, encode: { x: [0, 1], y: 2 }, data: bars,
        }],
    };
    return {
        option, hasData: true, lanes: labels, bars, fullFrom, fullTo,
        axisOrigin: String(origin), windowFrom: String(fromNs), windowTo: String(toNs),
        chartHeight: Math.max(240, lanes.length * 54 + 100),
        count: keptCount, total_count: totalCount, kept_count: keptCount,
        truncated: !!(data && data.truncated), laneTruncations,
    };
}
