/* pgwt — table column configs for the migrated table views.
 *
 * Each config is { columns, rowClass?, onClick? } in the shape lib/table.js
 * consumes. They are declarative and pure (cell formatters return HTML
 * strings), so buildTableModel() over a config is Node-testable. onClick here
 * is a *descriptor* — it returns the drill intent { filterKey, filterValue,
 * label } rather than performing navigation, keeping the config free of app
 * wiring. The view translates the intent into filters.drill(). A column may
 * additionally carry `intent(row) -> intent|null` — a per-CELL drill
 * descriptor (U2 wire 6: the events percentile cells emit the
 * 'histogram-event' pivot); lib/table.js marks such cells drillable and the
 * view routes them through ctx.onDrill like every other intent.
 *
 * Used by the migrated overview, events, sessions and queries views (the
 * drill-down tables all share lib/table.js via these configs).
 */

import {
    WAIT_CLASSES, classColor, eventColor,
    fmtMs, fmtUs, fmtCount, fmtPct, fmtAas, esc,
} from '../format.js';
import { dot, pctBar, stackedBar, eventStackedBar } from '../table.js';

const _dot = (name) => dot(name, classColor);

/* U2 wire 6 (review P3): the events table's percentile cells drill into the
 * latency-distribution view for THAT event. The intent is the app's pivot
 * shape ({pivot:'histogram-event'} + a normal event_id filter drill); it is
 * null — cell not drillable — when the value itself is null (sampled-only
 * row: no exact durations, no distribution to show, FID-3) or for CPU*
 * (event_id 0 is not a wait event; the histogram is a wait-latency view and
 * an event_id=0 filter means "unfiltered" server-side). */
function histogramIntent(key) {
    return (r) => {
        if (!(r.event_id > 0) || r[key] == null) return null;
        return { pivot: 'histogram-event', filterKey: 'event_id',
                 filterValue: r.event_id, label: r.name };
    };
}

/* Percentile cell: the value, wrapped in a visible drill affordance when the
 * cell is drillable (same predicate as histogramIntent). */
function pctlCell(key) {
    const intent = histogramIntent(key);
    return (r) => {
        const v = fmtUs(r[key]);
        if (!intent(r)) return v;
        return '<span class="cell-drill" style="border-bottom:1px dotted #778"' +
               ' title="' + esc('View the latency distribution of ' + r.name) +
               '">' + v + '</span>';
    };
}

export const overviewConfig = {
    columns: [
        { key: 'name', label: 'Stat Name', format: (r) => {
            if (r.indent >= 1) return _dot(r.name) + esc(r.name);
            return esc(r.name);
        }},
        { key: 'ms', label: 'Time', cls: 'num', format: (r) => fmtMs(r.ms) },
        { key: 'pct', label: '%DB Time', cls: 'num', format: (r) => {
            if (r.indent === 0 && r.name === 'DB Time') return fmtPct(r.pct);
            /* U2 (U1-review F4): event rows (indent 2) take the event's
             * identity tint so "same event = same color" holds against the
             * AAS breakdown and the wait-profile bars; class rows (indent 1)
             * keep the flat class hue — they ARE the class. */
            const color = (r.indent >= 2 ? eventColor(null, r.name)
                                         : classColor(r.name)) || '#4fc3f7';
            return pctBar(r.pct, color);
        }},
        { key: 'aas', label: 'AAS', cls: 'num', format: (r) => fmtAas(r.aas) },
    ],
    rowClass: (r) => {
        let c = '';
        if (r.indent === 1) c += ' indent-1 clickable';
        if (r.indent === 2) c += ' indent-2';
        return c;
    },
    onClick: (r) => {
        if (r.indent !== 1) return null;
        const cls = r.name.replace('*', '');
        return { filterKey: 'class', filterValue: cls, label: cls };
    },
};

export const eventsConfig = {
    columns: [
        { key: 'name', label: 'Wait Event', format: (r) => _dot(r.name) + esc(r.name) },
        { key: 'count', label: 'Count', cls: 'num', format: (r) => fmtCount(r.count) },
        { key: 'total_ms', label: 'Total', cls: 'num', format: (r) => fmtMs(r.total_ms) },
        { key: 'avg_us', label: 'Avg', cls: 'num', format: (r) => fmtUs(r.avg_us) },
        { key: 'p50_us', label: 'P50', cls: 'num', format: pctlCell('p50_us'),
          intent: histogramIntent('p50_us') },
        { key: 'p95_us', label: 'P95', cls: 'num', format: pctlCell('p95_us'),
          intent: histogramIntent('p95_us') },
        { key: 'p99_us', label: 'P99', cls: 'num', format: pctlCell('p99_us'),
          intent: histogramIntent('p99_us') },
        { key: 'max_us', label: 'Max', cls: 'num', format: (r) => fmtUs(r.max_us) },
        { key: 'pct', label: '%DB', cls: 'num', format: (r) => {
            /* Idle-but-visible events (e.g. Client:ClientRead) have time but
             * no meaningful share of DB Time; the server sends pct=null so we
             * render "—" instead of a bogus bar. */
            if (r.pct == null) return '<span style="color:#555">—</span>';
            /* U2 (U1-review F4): identity tint, not the flat class hue. */
            const color = eventColor(null, r.name) || '#4fc3f7';
            return pctBar(r.pct, color);
        }},
        { key: 'aas', label: 'AAS', cls: 'num', format: (r) => fmtAas(r.aas) },
    ],
    rowClass: () => 'clickable',
    onClick: (r) => ({ filterKey: 'event_id', filterValue: r.event_id, label: r.name }),
};

export const sessionsConfig = {
    columns: [
        { key: 'pid', label: 'PID', format: (r) => r.pid },
        { key: 'type', label: 'Backend', format: (r) => esc(r.type || 'unknown') },
        { key: 'user', label: 'User', format: (r) => esc(r.user || '') },
        { key: 'db', label: 'Database', format: (r) => esc(r.db || '') },
        { key: 'db_time_ms', label: 'DB Time', cls: 'num', format: (r) => fmtMs(r.db_time_ms) },
        { key: 'cpu_pct', label: 'CPU%', cls: 'num', format: (r) => pctBar(r.cpu_pct, 'rgb(80,250,123)') },
        { key: 'wait_pct', label: 'Wait%', cls: 'num', format: (r) => {
            const color = classColor(r.top_wait) || '#F44336';
            return pctBar(r.wait_pct, color);
        }},
        { key: 'top_wait', label: 'Top Wait', format: (r) => _dot(r.top_wait) + esc(r.top_wait) },
    ],
    rowClass: () => 'clickable',
    onClick: (r) => ({ filterKey: 'pid', filterValue: r.pid, label: 'PID ' + r.pid }),
};

export const queriesConfig = {
    columns: [
        { key: 'query_id', label: 'Query ID', cls: 'sticky-col sticky-col-0', format: (r) =>
            '<span class="query-id">' + r.query_id + '</span>' },
        { key: 'text', label: 'Query Text', cls: 'sticky-col sticky-col-1', format: (r) => {
            if (!r.text) return '<span style="color:#555">—</span>';
            const truncated = esc(r.text.substring(0, 80)) +
                (r.text.length > 80 ? '...' : '');
            return '<span class="qt-hover" data-fulltext="' +
                esc(r.text).replace(/"/g, '&quot;') + '">' +
                _dot(r.top_wait) + truncated + '</span>';
        }},
        { key: '_executions', label: 'Waterfall', sortable: false,
          cls: 'query-executions-cell',
          format: () => '<span class="cell-drill query-executions-action" ' +
              'title="Open individual executions">Executions →</span>',
          intent: (r) => ({ pivot: 'query-executions', filterKey: 'query_id',
              filterValue: r.query_id,
              label: r.text ? r.text.substring(0, 40) : 'Query ' + r.query_id }) },
        { key: 'total_ms', label: 'DB Time', cls: 'num', format: (r) => fmtMs(r.total_ms) },
        { key: 'pct', label: '%DB', cls: 'num', format: (r) => fmtPct(r.pct) },
        { key: 'classes', label: 'Wait Profile', format: (r) =>
            r.events ? eventStackedBar(r.events, r.total_ms, eventColor, fmtMs)
                     : stackedBar(r.classes, r.total_ms, WAIT_CLASSES, fmtMs) },
        { key: 'exec_count', label: 'Execs', cls: 'num', format: (r) =>
            r.exec_count != null ? fmtCount(r.exec_count) : '—' },
        { key: 'plan_count', label: 'Plans', cls: 'num', format: (r) =>
            r.plan_count != null ? fmtCount(r.plan_count) : '—' },
        { key: 'plan_ratio', label: 'Plan%', cls: 'num', format: (r) =>
            (r.exec_count > 0 && r.plan_count != null)
                ? (r.plan_count / r.exec_count * 100).toFixed(0) + '%' : '—' },
        { key: 'avg_exec_ms', label: 'Avg Exec', cls: 'num', format: (r) =>
            r.avg_exec_ms != null ? fmtMs(r.avg_exec_ms) : '—' },
        { key: 'p95_exec_ms', label: 'p95 Exec', cls: 'num', format: (r) =>
            r.p95_exec_ms != null ? fmtMs(r.p95_exec_ms) : '—' },
        { key: 'p99_exec_ms', label: 'p99 Exec', cls: 'num', format: (r) =>
            r.p99_exec_ms != null ? fmtMs(r.p99_exec_ms) : '—' },
        { key: 'avg_plan_ms', label: 'Avg Plan', cls: 'num', format: (r) =>
            r.avg_plan_ms != null ? fmtMs(r.avg_plan_ms) : '—' },
        { key: 'p95_plan_ms', label: 'p95 Plan', cls: 'num', format: (r) =>
            r.p95_plan_ms != null ? fmtMs(r.p95_plan_ms) : '—' },
        { key: 'p99_plan_ms', label: 'p99 Plan', cls: 'num', format: (r) =>
            r.p99_plan_ms != null ? fmtMs(r.p99_plan_ms) : '—' },
        { key: 'count', label: 'Waits', cls: 'num', format: (r) => fmtCount(r.count) },
        { key: 'avg_us', label: 'Avg Wait', cls: 'num', format: (r) => fmtUs(r.avg_us) },
    ],
    rowClass: () => 'clickable',
    onClick: (r) => ({
        filterKey: 'query_id', filterValue: r.query_id,
        label: r.text ? r.text.substring(0, 40) : 'Query ' + r.query_id,
    }),
};
