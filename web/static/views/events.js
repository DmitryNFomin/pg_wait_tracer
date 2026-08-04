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
import { eventsConfig } from '../lib/builders/table-configs.js';
import {
    buildPaneFidelity, paneFidelityBadgeHtml,
    buildPercentileBasis, percentileBasisHtml,
} from '../lib/panels.js';

/* PURE: top_events response + sort -> render model. Exported for testing. */
export function buildEventsModel(data, sort) {
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
            return ctx.transport.request(ctx.channel('table'), 'top_events', {
                from: ctx.timeRange.from,
                to: ctx.timeRange.to,
                filters: ctx.filters.snapshot(),
            });
        },

        build(data, ctx) {
            const sort = ctx.getSort('events');
            return buildEventsModel(data, sort);
        },

        mount(el, model, ctx) {
            if (ctx.summaryEl) ctx.summaryEl.innerHTML = '';
            if (!model.hasRows) {
                el.innerHTML = paneFidelityBadgeHtml(model.paneFidelity) +
                    '<div class="loading">No data for selected range</div>';
                return;
            }
            ctx.mountTable(el, eventsConfig, model.table, {
                sort: ctx.getSort('events'),
                onSort: (key) => { ctx.toggleSort('events', key); ctx.refresh(); },
                onRowClick: (row) => {
                    const intent = eventsConfig.onClick(row);
                    if (intent) ctx.onDrill(intent);
                },
                // Wire 6: percentile cell → 'histogram-event' pivot intent.
                onCellDrill: (intent) => ctx.onDrill(intent),
                truncation: model.truncation,
                tooltipEl: ctx.tooltipEl,
            });
            el.insertAdjacentHTML('afterbegin',
                paneFidelityBadgeHtml(model.paneFidelity));
            el.insertAdjacentHTML('beforeend',
                percentileBasisHtml(model.percentileBasis));
        },

        enter() { /* no chart */ },
        leave() { /* no chart */ },
    };
}
