/* pgwt — "events" view: the top wait-events table.
 *
 * Migrated to the { id, requests, build, mount, enter, leave } contract (B3
 * part 2). requests() fetches top_events on a single-flight channel; build() is
 * the PURE table model (sort applied via lib/table.js's buildTableModel over the
 * shared eventsConfig); mount() paints with the shared component and wires
 * header-sort + row-click drill-down. No chart, so enter/leave are no-ops.
 *
 * Behavior is identical to the old legacy adapter in app.js:
 *   - Rows arrive in server order (CPU* first); no default client sort.
 *   - Clicking a column header sorts (first click desc, toggles to asc).
 *   - Clicking a row drills into that event_id (→ queries, per the app PIVOT).
 *
 * U2 additions (reviews P10 + P3 wire 6), all response-derived:
 *   - Per-pane fidelity badge for sampled/mixed windows (the table's scaled
 *     estimates must not render pixel-identically to exact numbers).
 *   - Percentile-basis footnote: Avg/P50/P95/P99/Max come from exact-captured
 *     events only; over a non-exact window the footnote states that basis.
 *   - Truncation row when the server flags the row list incomplete.
 *   - Percentile CELLS drill to that event's latency distribution (the
 *     'histogram-event' pivot intent via ctx.onDrill; row-click unchanged).
 */

import { buildTableModel, buildTruncationRow } from '../lib/table.js';
import { eventsConfig, compareEventsConfig } from '../lib/builders/table-configs.js';
import { buildDeltaComparison, defaultDeltaSort } from '../lib/builders/compare.js';
import { shiftWindow } from '../lib/camera.js';
import {
    buildPaneFidelity, paneFidelityBadgeHtml,
    buildPercentileBasis, percentileBasisHtml,
    buildComparePaneFidelity, compareFidelityHtml,
} from '../lib/panels.js';

/* PURE: top_events response + sort -> render model. Exported for testing. */
export function buildEventsModel(data, sort) {
    if (data && data.compare) {
        const delta = buildDeltaComparison('events', data.a, data.b,
            { baselineUnavailable: data.baselinePredates });
        const appliedSort = defaultDeltaSort(sort);
        return {
            compare: true,
            hasRows: delta.rows.length > 0 || !!delta.truncation,
            table: buildTableModel(compareEventsConfig, delta.rows, appliedSort),
            compareFidelity: buildComparePaneFidelity(data.a, data.b,
                { baselinePredates: data.baselinePredates }),
            truncation: delta.truncation,
            sort: appliedSort,
            baselinePredates: delta.baselineUnavailable,
        };
    }
    const rows = (data && data.rows) || [];
    return {
        hasRows: rows.length > 0,
        table: buildTableModel(eventsConfig, rows, sort),
        paneFidelity: buildPaneFidelity(data),
        percentileBasis: buildPercentileBasis(data),
        truncation: buildTruncationRow(data),
    };
}

export function createEventsView() {
    return {
        id: 'events',

        async requests(ctx) {
            const aParams = {
                from: ctx.timeRange.from,
                to: ctx.timeRange.to,
                filters: ctx.filters.snapshot(),
            };
            const a = ctx.transport.request(ctx.channel('table.a'), 'top_events', aParams);
            if (!ctx.compare || !ctx.compare.enabled) return a;
            const bw = shiftWindow(aParams, ctx.compare.offsetNs);
            const predates = bw.from < ctx.server.fromNs;
            const b = predates ? Promise.resolve(null) : ctx.transport.request(
                    ctx.channel('table.b'), 'top_events', Object.assign({}, aParams, bw));
            const pair = await Promise.all([a, b]);
            return { compare: true, a: pair[0], b: pair[1], baselinePredates: predates };
        },

        build(data, ctx) {
            const sort = ctx.getSort('events');
            return buildEventsModel(data, sort);
        },

        mount(el, model, ctx) {
            if (ctx.summaryEl) ctx.summaryEl.innerHTML = '';
            if (!model.hasRows) {
                el.innerHTML = (model.compare
                    ? compareFidelityHtml(model.compareFidelity)
                    : paneFidelityBadgeHtml(model.paneFidelity)) +
                    '<div class="loading">' + (model.baselinePredates
                        ? 'Baseline unavailable for comparison'
                        : 'No data for selected range') + '</div>';
                return;
            }
            const config = model.compare ? compareEventsConfig : eventsConfig;
            ctx.mountTable(el, config, model.table, {
                sort: model.compare ? model.sort : ctx.getSort('events'),
                onSort: (key) => { ctx.toggleSort('events', key); ctx.refresh(); },
                onRowClick: (row) => {
                    const intent = config.onClick(row);
                    if (intent) ctx.onDrill(intent);
                },
                // Wire 6: percentile cell → 'histogram-event' pivot intent.
                onCellDrill: (intent) => ctx.onDrill(intent),
                truncation: model.truncation,
                tooltipEl: ctx.tooltipEl,
            });
            el.insertAdjacentHTML('afterbegin',
                model.compare ? compareFidelityHtml(model.compareFidelity)
                              : paneFidelityBadgeHtml(model.paneFidelity));
            if (!model.compare) el.insertAdjacentHTML('beforeend',
                percentileBasisHtml(model.percentileBasis));
        },

        enter() { /* no chart */ },
        leave() { /* no chart */ },
    };
}
