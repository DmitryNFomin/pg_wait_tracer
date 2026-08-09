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
import { queriesConfig, compareQueriesConfig } from '../lib/builders/table-configs.js';
import { buildDeltaComparison, defaultDeltaSort } from '../lib/builders/compare.js';
import { shiftWindow } from '../lib/camera.js';
import { settleBaseline } from '../lib/compare-request.js';
import {
    buildPaneFidelity, paneFidelityBadgeHtml,
    buildComparePaneFidelity, compareFidelityHtml,
    baselineUnavailableHtml,
} from '../lib/panels.js';

/* PURE: top_queries response + sort -> render model. Exported for testing.
 * U2 (review P10): carries the pane fidelity badge (sampled/mixed numbers are
 * scaled estimates) and the server-flagged truncation row. */
export function buildQueriesModel(data, sort) {
    if (data && data.compare && data.baselineUnavailable) {
        const primary = data.a;
        const rows = (primary && primary.rows) || [];
        return {
            hasRows: rows.length > 0,
            table: buildTableModel(queriesConfig, rows, sort),
            paneFidelity: buildPaneFidelity(primary),
            truncation: buildTruncationRow(primary),
            baselineUnavailable: true,
        };
    }
    if (data && data.compare) {
        const delta = buildDeltaComparison('queries', data.a, data.b,
            { baselineUnavailable: data.baselinePredates });
        const appliedSort = defaultDeltaSort(sort);
        return {
            compare: true,
            hasRows: delta.rows.length > 0 || !!delta.truncation,
            table: buildTableModel(compareQueriesConfig, delta.rows, appliedSort),
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
        table: buildTableModel(queriesConfig, rows, sort),
        paneFidelity: buildPaneFidelity(data),
        truncation: buildTruncationRow(data),
    };
}

export function createQueriesView() {
    return {
        id: 'queries',

        async requests(ctx) {
            const aParams = {
                from: ctx.timeRange.from,
                to: ctx.timeRange.to,
                filters: ctx.filters.snapshot(),
            };
            const a = ctx.transport.request(ctx.channel('table.a'), 'top_queries', aParams);
            if (!ctx.compare || !ctx.compare.enabled) return a;
            const bw = shiftWindow(aParams, ctx.compare.offsetNs);
            const predates = bw.from < ctx.server.fromNs;
            if (predates) {
                return { compare: true, a: await a, b: null,
                    baselinePredates: true, baselineUnavailable: false };
            }
            const b = settleBaseline(ctx.transport.request(ctx.channel('table.b'),
                'top_queries', Object.assign({}, aParams, bw)));
            const [aData, bResult] = await Promise.all([a, b]);
            return { compare: true, a: aData,
                b: bResult.ok ? bResult.payload : null,
                baselinePredates: false, baselineUnavailable: !bResult.ok };
        },

        build(data, ctx) {
            const sort = ctx.getSort('queries');
            return buildQueriesModel(data, sort);
        },

        mount(el, model, ctx) {
            if (ctx.summaryEl) ctx.summaryEl.innerHTML = '';
            if (!model.hasRows) {
                el.innerHTML = baselineUnavailableHtml(model.baselineUnavailable) +
                    (model.compare
                    ? compareFidelityHtml(model.compareFidelity)
                    : paneFidelityBadgeHtml(model.paneFidelity)) +
                    '<div class="loading">' + (model.baselinePredates
                        ? 'Baseline unavailable for comparison'
                        : 'No data for selected range') + '</div>';
                return;
            }
            const config = model.compare ? compareQueriesConfig : queriesConfig;
            ctx.mountTable(el, config, model.table, {
                sort: model.compare ? model.sort : ctx.getSort('queries'),
                onSort: (key) => { ctx.toggleSort('queries', key); ctx.refresh(); },
                onRowClick: (row) => {
                    const intent = config.onClick(row);
                    if (intent) ctx.onDrill(intent);
                },
                onCellDrill: (intent) => ctx.onDrill(intent),
                truncation: model.truncation,
                tooltipEl: ctx.tooltipEl,
            });
            el.insertAdjacentHTML('afterbegin',
                baselineUnavailableHtml(model.baselineUnavailable) +
                (model.compare ? compareFidelityHtml(model.compareFidelity)
                               : paneFidelityBadgeHtml(model.paneFidelity)));
        },

        enter() { /* no chart */ },
        leave() { /* no chart */ },
    };
}
