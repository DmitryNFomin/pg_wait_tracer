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

/* #222 review (blocker follow-up): the Duration cell must say WHY an open
 * row outranks a finished one under "longest running first" -- the sort
 * key is elapsed-so-far (src/server.c's execution_sort_duration_ns), which
 * is invisible if the cell just reads "In progress" with no number
 * (docs/VISUAL_CHECKLIST.md SEMANTICS, #103 precedent: an open-ended value
 * "is a bound, not a value: it must read >=N and say why in its tooltip").
 * `windowTo` is the request's own `to` (the view already has it from
 * ctx.timeRange.to) -- the SAME lower bound the server used to rank the
 * row, not wall-clock now, so the two never disagree.
 *
 * A CLOSED row can also be real-but-not-measured: end_inferred (new
 * server field, #222 review) means this row was closed at its pid's next
 * CMD_END, not a real EXEC_END -- an error, cancel, timeout, disconnect,
 * or a lost EXEC_END marker. The duration is still a genuine wall-clock
 * span, just not one PostgreSQL itself reported as "this query finished
 * normally" -- flagged, not hidden, and NOT styled like "still running"
 * (it is closed) or left indistinguishable from a normal completion. */
export function fmtExecutionDuration(row, windowTo) {
    if (row.in_progress) {
        const to = nsBig(windowTo);
        const start = nsBig(row.start_ns);
        if (to != null && start != null && to > start) {
            const elapsedMs = Number(to - start) / 1e6;
            return { text: '≥ ' + fmtMs(elapsedMs) + ' (running)',
                tooltip: 'Still running: elapsed time so far, not a final duration.' };
        }
        return { text: 'In progress', tooltip: null };
    }
    if (row.duration_ms == null) return { text: '—', tooltip: null };
    const text = fmtMs(row.duration_ms);
    if (row.end_inferred) {
        return { text: text + ' *',
            tooltip: 'Inferred, not measured: this execution never reached '
                + 'a normal completion (likely an error, cancel, or '
                + 'timeout). Duration is real elapsed time, measured up to '
                + 'when the connection went idle, not PostgreSQL’s own '
                + 'query-end signal.' };
    }
    return { text, tooltip: null };
}

function durationCellHtml(r) {
    const d = fmtExecutionDuration(r, r._windowTo);
    if (!d.tooltip) return d.text;
    const cls = r.in_progress ? 'execution-open' : 'execution-inferred';
    return '<span class="' + cls + '" title="' + esc(d.tooltip) + '">' + d.text + '</span>';
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
        { key: 'duration_ms', label: 'Duration', cls: 'num', format: durationCellHtml },
        { key: 'plan_ms', label: 'Plan', cls: 'num', format: (r) => fmtMs(r.plan_ms) },
        { key: 'n_events', label: 'Leader events', cls: 'num', format: (r) => String(r.n_events) },
        { key: 'n_workers', label: 'Workers', cls: 'num', format: (r) => String(r.n_workers) },
    ],
    rowClass: (r) => 'clickable' + (r._selected ? ' selected-execution' : ''),
};

/* #222: which slice of executions the table asks for. The server orders the
 * whole matching window before truncating to `limit` (src/server.c), so this
 * is a REQUEST parameter, not a client-side re-sort -- picking "recent" here
 * after the server already truncated to the newest rows cannot recover a
 * slow outlier that aged out of that slice. Two values only: the server
 * falls back to start_desc for anything else it doesn't recognize.
 *
 * Default is duration_desc: on a busy OLTP system the recency slice is
 * roughly the last second of traffic (#222 -- a query firing every 9.5s was
 * reliably crowded out of it by pgbench at ~125 exec/s, order 75k executions
 * in a 10-minute capture), and "the slowest or most-wait-heavy executions"
 * is what this tab exists to answer. "Latest first" stays one click away
 * (the sort toggle in waterfall.js) for whoever wants recent activity
 * instead -- both are legitimate questions, but only one can be silently
 * the default, and the demo workload showed which one users hit the tab
 * needing. */
export const EXECUTIONS_SORT_DURATION = 'duration_desc';
export const EXECUTIONS_SORT_RECENT = 'start_desc';
export const EXECUTIONS_SORT_DEFAULT = EXECUTIONS_SORT_DURATION;

/* Label for the current sort and for the toggle control that switches to
 * the OTHER mode -- exported separately so the view can put the state
 * label and the action label in different spots (a status span vs. a
 * button) without duplicating the ternary.
 *
 * duration_desc reads "longest running first", not "slowest first"
 * (review on #222): once an open row ranks by elapsed-so-far alongside a
 * closed row's real duration, the list is ordered by ONE shared axis --
 * longest elapsed-or-completed -- and a reader seeing an "In progress" row
 * outrank a finished 30ms query needs that stated, not inferred. The
 * toggle's OTHER state ("Show latest first") is unaffected: recency was
 * never ambiguous. */
export function executionsSortLabel(sort) {
    return sort === EXECUTIONS_SORT_RECENT ? 'latest first' : 'longest running first';
}

export function executionsSortToggleTarget(sort) {
    return sort === EXECUTIONS_SORT_RECENT ? EXECUTIONS_SORT_DURATION : EXECUTIONS_SORT_RECENT;
}

export function executionsSortToggleLabel(sort) {
    return sort === EXECUTIONS_SORT_RECENT
        ? 'Show longest running first' : 'Show latest first';
}

/* "150 running, 3 completed" next to the sort label -- open_count/
 * completed_count (src/server.c, #222 review) over the FULL matching
 * window, so a viewer can tell the crowding risk elapsed-so-far ranking
 * reopened is (or is not) happening here, instead of the split being
 * computed and never read. null counts (an older server, or a refused
 * request) render nothing rather than "null running, null completed". */
export function executionsCountsLabel(openCount, completedCount) {
    if (typeof openCount !== 'number' || typeof completedCount !== 'number')
        return null;
    return openCount + ' running, ' + completedCount + ' completed';
}

/* Does this page contain a row whose end was INFERRED (closed at the pid's
 * next CMD_END, not measured at a real EXEC_END)? Those render their
 * duration with a trailing '*' -- see fmtExecutionDuration. */
export function executionsHasInferredEnd(rows) {
    return (rows || []).some(r => r && !r.in_progress && !!r.end_inferred);
}

/* The one status line next to the sort control: the open/completed split
 * plus, when and only when a '*' is actually on screen, what it means.
 *
 * #222 review round 3, item 3: the '*' was explained by a hover tooltip
 * alone, and NOBODY HOVERS ON A PROJECTED SCREEN. This is the tab that
 * carries the demo's main story, so the meaning has to be readable without
 * a pointer. Deliberately independent of the counts: an older server that
 * sends no open_count/completed_count still renders rows, so gating the
 * legend on the counts being present would hide it exactly when the rest
 * of the header is already degraded. Returns null when there is nothing to
 * say, so the title row stays uncluttered on an ordinary page. */
export function executionsStatusLabel(openCount, completedCount, hasInferredEnd) {
    const parts = [];
    const counts = executionsCountsLabel(openCount, completedCount);
    if (counts) parts.push(counts);
    if (hasInferredEnd) parts.push('* = inferred end');
    return parts.length ? parts.join(' \u00b7 ') : null;
}

/* executions response -> shared-table model. Server order matches whatever
 * `sort` the request asked for (see EXECUTIONS_SORT_* above) and is
 * preserved; client sorting is intentionally absent on this selector. */
export function buildExecutionsModel(data, selected, windowTo) {
    const rows = ((data && data.rows) || []).map(r =>
        Object.assign({}, r, { _selected: sameExecution(r, selected),
            _windowTo: windowTo }));
    return {
        hasRows: rows.length > 0,
        table: buildTableModel(executionsConfig, rows, null),
        truncation: buildTruncationRow(Object.assign({}, data, { rows })),
        count: rows.length,
        total_count: data && typeof data.total_count === 'number'
            ? data.total_count : null,
        truncated: !!(data && data.truncated),
        // #222 review should-fix: the cheap mitigation for the crowding
        // risk elapsed-so-far ranking reopened -- the truncated `rows`
        // page cannot say which kind (open vs. completed) got crowded
        // out, but these two counts (over the FULL matching window,
        // src/server.c) can, so the view surfaces them instead of leaving
        // them computed-and-unread.
        open_count: data && typeof data.open_count === 'number'
            ? data.open_count : null,
        completed_count: data && typeof data.completed_count === 'number'
            ? data.completed_count : null,
        // #222 review round 3: drives the inline '* = inferred end'
        // legend. Computed from the rows actually being rendered, so the
        // legend appears exactly when a '*' can appear and never when it
        // cannot.
        has_inferred_end: executionsHasInferredEnd(rows),
    };
}

function count(v) {
    const n = Number(v);
    return Number.isFinite(n) ? n : 0;
}

/* Does this execution row have a waterfall to draw?
 *
 * buildWaterfallOption() only produces a chart when execution_detail comes
 * back with at least one bar: a leader wait event, a parallel-worker event,
 * or the plan phase. The executions row already reports all three
 * (n_events / n_workers / plan_ms), so the selector can tell before asking.
 */
export function executionHasDetail(row) {
    if (!row) return false;
    return count(row.n_events) > 0 || count(row.n_workers) > 0 ||
        row.plan_ms != null;
}

/* Which execution the waterfall opens on when the user has not picked one.
 *
 * NOT simply rows[0]. Measured on a real `--mode full` capture (issue #101,
 * 40 simulated live ticks over a 3-minute pgbench trace, latest-first order):
 * the newest execution was drawable in 0 of 40 ticks. At pgbench rates most
 * executions are microsecond-scale statements that never change wait state,
 * so the newest row's execution_detail answers {leader:{events:[]},
 * workers:[], plan:null} — buildWaterfallOption returns hasData:false, the
 * view mounts no ECharts instance, and the panel sits on "No execution
 * events captured" forever. About half of the 100 returned rows WERE
 * drawable in every one of those ticks, with the first drawable row at
 * index 1-3.
 *
 * So: the first row that actually has something to show, preferring one
 * with real wait events over a worker-only or plan-only row. This still
 * applies unchanged under #222's duration_desc default -- a genuinely slow
 * row is even more likely than a fast one to have events, but a slow
 * in_progress row can still legitimately have none yet, so the same
 * fallback chain (not a bare rows[0]) still matters. When nothing in the
 * page qualifies we still return the first row — the empty state is then
 * the honest answer, not a hidden failure.
 */
export function pickDefaultExecution(rows) {
    if (!rows || !rows.length) return null;
    return rows.find(r => count(r.n_events) > 0) ||
        rows.find(r => count(r.n_workers) > 0) ||
        rows.find(r => r.plan_ms != null) ||
        rows[0];
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
