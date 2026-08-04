/* pgwt — "queries" view: the top queries (per-fingerprint) table.
 *
 * Migrated to the { id, requests, build, mount, enter, leave } contract (B3
 * part 2). requests() fetches top_queries on a single-flight channel; build()
 * is the PURE table model (sort via lib/table.js over the shared queriesConfig,
 * which renders the stacked wait-profile bars + the query-text hover cell);
 * mount() paints + wires header-sort, row-click drill-down, and the query-text
 * tooltip. No chart, so enter/leave are no-ops.
 *
 * Behavior is identical to the old legacy adapter in app.js:
 *   - Rows arrive in server order; no default client sort.
 *   - Clicking a column header sorts (first click desc, toggles to asc).
 *   - Clicking a row drills into that query_id (→ events, per the app PIVOT).
 *   - Long query text shows a hover tooltip (lib/table.js tooltipEl wiring).
 */

import { buildTableModel, buildTruncationRow } from '../lib/table.js';
import { queriesConfig } from '../lib/builders/table-configs.js';
import { buildPaneFidelity, paneFidelityBadgeHtml } from '../lib/panels.js';

/* PURE: top_queries response + sort -> render model. Exported for testing.
 * U2 (review P10): carries the pane fidelity badge (sampled/mixed numbers are
 * scaled estimates) and the server-flagged truncation row. */
export function buildQueriesModel(data, sort) {
    const rows = (data && data.rows) || [];
    return {
        hasRows: rows.length > 0,
        table: buildTableModel(queriesConfig, rows, sort),
        paneFidelity: buildPaneFidelity(data),
        truncation: buildTruncationRow(data),
    };
}

export function createQueriesView() {
    return {
        id: 'queries',

        async requests(ctx) {
            return ctx.transport.request(ctx.channel('table'), 'top_queries', {
                from: ctx.timeRange.from,
                to: ctx.timeRange.to,
                filters: ctx.filters.snapshot(),
            });
        },

        build(data, ctx) {
            const sort = ctx.getSort('queries');
            return buildQueriesModel(data, sort);
        },

        mount(el, model, ctx) {
            if (ctx.summaryEl) ctx.summaryEl.innerHTML = '';
            if (!model.hasRows) {
                el.innerHTML = paneFidelityBadgeHtml(model.paneFidelity) +
                    '<div class="loading">No data for selected range</div>';
                return;
            }
            ctx.mountTable(el, queriesConfig, model.table, {
                sort: ctx.getSort('queries'),
                onSort: (key) => { ctx.toggleSort('queries', key); ctx.refresh(); },
                onRowClick: (row) => {
                    const intent = queriesConfig.onClick(row);
                    if (intent) ctx.onDrill(intent);
                },
                truncation: model.truncation,
                tooltipEl: ctx.tooltipEl,
            });
            el.insertAdjacentHTML('afterbegin',
                paneFidelityBadgeHtml(model.paneFidelity));
        },

        enter() { /* no chart */ },
        leave() { /* no chart */ },
    };
}
