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

import {
    buildTableModel, buildTruncationRow, createStableOrder, applyDefaultOrder,
    createOrderResetGate, orderResetKey,
} from '../lib/table.js';
import { sessionsConfig, sessionsScopeNoteHtml } from '../lib/builders/table-configs.js';
import { buildPaneFidelity, paneFidelityBadgeHtml } from '../lib/panels.js';

/* PURE: top_sessions response + sort -> render model. Exported for testing.
 * U2 (review P10): carries the pane fidelity badge (sampled/mixed numbers are
 * scaled estimates) and the server-flagged truncation row.
 * #440 (contract item 6): `orderTracker` applies the refresh-ordering
 * hysteresis to the DEFAULT (no explicit column sort) order; optional so
 * every existing caller/test that does not pass one keeps today's plain
 * order (a REAL production call always passes one — createSessionsView()
 * below). */
export function buildSessionsModel(data, sort, orderTracker) {
    const rows = (data && data.rows) || [];
    const ordered = orderTracker ? applyDefaultOrder(orderTracker, rows, sort) : rows;
    return {
        hasRows: rows.length > 0,
        table: buildTableModel(sessionsConfig, ordered, sort),
        paneFidelity: buildPaneFidelity(data),
        truncation: buildTruncationRow(data),
    };
}

export function createSessionsView() {
    // #440: one hysteresis tracker for this mounted view's whole lifetime --
    // identity is pid (stable, unique per row), value is db_time_ms (the
    // Sessions table's own DB-time contribution column).
    const order = createStableOrder({
        idKey: (r) => String(r.pid), valueKey: (r) => r.db_time_ms,
    });
    const resetIfChanged = createOrderResetGate();

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
            resetIfChanged(order, { window: orderResetKey(ctx),
                filters: ctx.filters.snapshot(), sort });
            return buildSessionsModel(data, sort, order);
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
            el.insertAdjacentHTML('afterbegin', sessionsScopeNoteHtml());
        },

        enter() { /* no chart */ },
        leave() { /* no chart */ },
    };
}
