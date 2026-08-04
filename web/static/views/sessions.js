/* pgwt — "sessions" view: the top sessions (per-backend) table.
 *
 * Migrated to the { id, requests, build, mount, enter, leave } contract (B3
 * part 2). requests() fetches top_sessions on a single-flight channel; build()
 * is the PURE table model (sort via lib/table.js over the shared
 * sessionsConfig); mount() paints + wires header-sort and row-click drill-down.
 * No chart, so enter/leave are no-ops.
 *
 * Behavior is identical to the old legacy adapter in app.js:
 *   - Rows arrive in server order; no default client sort.
 *   - Clicking a column header sorts (first click desc, toggles to asc).
 *   - Clicking a row drills into that pid (→ timeline, per the app PIVOT).
 */

import { buildTableModel, buildTruncationRow } from '../lib/table.js';
import { sessionsConfig } from '../lib/builders/table-configs.js';
import { buildPaneFidelity, paneFidelityBadgeHtml } from '../lib/panels.js';

/* PURE: top_sessions response + sort -> render model. Exported for testing.
 * U2 (review P10): carries the pane fidelity badge (sampled/mixed numbers are
 * scaled estimates) and the server-flagged truncation row. */
export function buildSessionsModel(data, sort) {
    const rows = (data && data.rows) || [];
    return {
        hasRows: rows.length > 0,
        table: buildTableModel(sessionsConfig, rows, sort),
        paneFidelity: buildPaneFidelity(data),
        truncation: buildTruncationRow(data),
    };
}

export function createSessionsView() {
    return {
        id: 'sessions',

        async requests(ctx) {
            return ctx.transport.request(ctx.channel('table'), 'top_sessions', {
                from: ctx.timeRange.from,
                to: ctx.timeRange.to,
                filters: ctx.filters.snapshot(),
            });
        },

        build(data, ctx) {
            const sort = ctx.getSort('sessions');
            return buildSessionsModel(data, sort);
        },

        mount(el, model, ctx) {
            if (ctx.summaryEl) ctx.summaryEl.innerHTML = '';
            if (!model.hasRows) {
                el.innerHTML = paneFidelityBadgeHtml(model.paneFidelity) +
                    '<div class="loading">No data for selected range</div>';
                return;
            }
            ctx.mountTable(el, sessionsConfig, model.table, {
                sort: ctx.getSort('sessions'),
                onSort: (key) => { ctx.toggleSort('sessions', key); ctx.refresh(); },
                onRowClick: (row) => {
                    const intent = sessionsConfig.onClick(row);
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
