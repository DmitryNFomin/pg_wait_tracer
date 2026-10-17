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

import {
    buildTableModel, buildTruncationRow, createStableOrder, applyDefaultOrder,
    createOrderResetGate,
} from '../lib/table.js';
import {
    makeQueriesConfig, queriesConfig, compareQueriesConfig,
    buildUnattributedNote, unattributedNoteHtml, queriesScopeNoteHtml,
} from '../lib/builders/table-configs.js';
import { buildDeltaComparison, defaultDeltaSort } from '../lib/builders/compare.js';
import { shiftWindow } from '../lib/camera.js';
import { settleBaseline } from '../lib/compare-request.js';
import { fidelityOf } from '../lib/builders/fidelity.js';
import {
    buildPaneFidelity, paneFidelityBadgeHtml,
    buildComparePaneFidelity, compareFidelityHtml,
    baselineUnavailableHtml,
} from '../lib/panels.js';

/* PURE: top_queries response + sort -> render model. Exported for testing.
 * U2 (review P10): carries the pane fidelity badge (sampled/mixed numbers are
 * scaled estimates) and the server-flagged truncation row.
 * #440 (contract item 6): `orderTracker` applies the refresh-ordering
 * hysteresis to the DEFAULT (no explicit column sort) order; optional so
 * every existing caller/test that does not pass one keeps today's plain
 * order (a REAL production call always passes one — createQueriesView()
 * below). */
export function buildQueriesModel(data, sort, orderTracker) {
    if (data && data.compare && data.baselineUnavailable) {
        const primary = data.a;
        const rows = (primary && primary.rows) || [];
        const ordered = orderTracker ? applyDefaultOrder(orderTracker, rows, sort) : rows;
        const config = makeQueriesConfig(fidelityOf(primary));
        return {
            hasRows: rows.length > 0,
            table: buildTableModel(config, ordered, sort),
            queryConfig: config,
            paneFidelity: buildPaneFidelity(primary),
            unattributedNote: buildUnattributedNote(primary),
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
    const ordered = orderTracker ? applyDefaultOrder(orderTracker, rows, sort) : rows;
    const config = makeQueriesConfig(fidelityOf(data));
    return {
        hasRows: rows.length > 0,
        table: buildTableModel(config, ordered, sort),
        queryConfig: config,
        paneFidelity: buildPaneFidelity(data),
        unattributedNote: buildUnattributedNote(data),
        truncation: buildTruncationRow(data),
    };
}

/* PURE: the "no rows" pane HTML. Exported for testing -- mount() is not a
 * pure function (it writes a real DOM element), so this is the unit-
 * testable half of the #439 fix: wholly-unattributed activity (no row has
 * its OWN query_id) is exactly the case where disclosure matters most, so
 * the empty-rows pane must NOT drop unattributedNote the way the exact
 * text/table pane never did. */
export function buildQueriesEmptyStateHtml(model) {
    return baselineUnavailableHtml(model.baselineUnavailable) +
        (model.compare
        ? compareFidelityHtml(model.compareFidelity)
        : paneFidelityBadgeHtml(model.paneFidelity)) +
        '<div class="loading">' + (model.baselinePredates
            ? 'Baseline unavailable for comparison'
            : 'No data for selected range') + '</div>' +
        (model.compare ? '' : unattributedNoteHtml(model.unattributedNote));
}

export function createQueriesView() {
    // #440: one hysteresis tracker for this mounted view's whole lifetime --
    // identity is query_id (stable, unique per row), value is total_ms (the
    // Queries table's own DB-time contribution column).
    const order = createStableOrder({
        idKey: (r) => String(r.query_id), valueKey: (r) => r.total_ms,
    });
    const resetIfChanged = createOrderResetGate();

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
            resetIfChanged(order, { from: ctx.timeRange.from, to: ctx.timeRange.to,
                filters: ctx.filters.snapshot(), sort });
            return buildQueriesModel(data, sort, order);
        },

        mount(el, model, ctx) {
            if (ctx.summaryEl) ctx.summaryEl.innerHTML = '';
            if (!model.hasRows) {
                el.innerHTML = buildQueriesEmptyStateHtml(model);
                return;
            }
            // #441: the live config tracks the response's own fidelity
            // (model.queryConfig, built by buildQueriesModel); the static
            // import is only the gallery/back-compat default.
            const config = model.compare ? compareQueriesConfig
                : (model.queryConfig || queriesConfig);
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
            // #439: disclosed beside the table, not folded into %/omitted.
            if (!model.compare) el.insertAdjacentHTML('beforeend',
                unattributedNoteHtml(model.unattributedNote));
            // #455: DB Time and Exec/Plan time different things (a wait
            // acquired before execution starts is in the former, never the
            // latter) -- static, so it is shown every time the live
            // (non-compare) columns are the ones with both families, not
            // gated on any one row's data.
            if (!model.compare) el.insertAdjacentHTML('beforeend', queriesScopeNoteHtml());
        },

        enter() { /* no chart */ },
        leave() { /* no chart */ },
    };
}
