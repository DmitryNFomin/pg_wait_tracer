/* pgwt — "overview" view: the time-model table + summary bar.
 *
 * Migrated to the { id, requests, build, mount, enter, leave } contract. It is
 * a tab view driven by the view-manager: requests() fetches time_model on a
 * single-flight channel, build() is the pure table model + summary numbers,
 * mount() paints via the shared lib/table.js component. No chart instance, so
 * enter/leave are no-ops here (the AAS chart belongs to the persistent "active"
 * view). Drill-down on a wait-class row is expressed as an intent by the table
 * config and turned into navigation by the shared onRowClick wiring.
 */

import { buildTableModel, buildTruncationRow } from '../lib/table.js';
import { overviewConfig, compareOverviewConfig } from '../lib/builders/table-configs.js';
import { buildDeltaComparison, defaultDeltaSort } from '../lib/builders/compare.js';
import { shiftWindow } from '../lib/camera.js';
import { settleBaseline } from '../lib/compare-request.js';
import {
    buildPaneFidelity, paneFidelityBadgeHtml,
    buildComparePaneFidelity, compareFidelityHtml,
    baselineUnavailableHtml,
} from '../lib/panels.js';
import { fmtMs, fmtAas } from '../lib/format.js';

/* PURE: time_model response -> summary metrics array. Exported for testing. */
export function buildSummary(data, numCpus) {
    if (!data || !data.rows || data.rows.length === 0) return [];
    const dbRow = data.rows[0];  // "DB Time" always first
    const idleRow = data.rows.find(r => r.indent === 0 && r.name.indexOf('Idle') >= 0);

    const out = [{ label: 'DB Time', value: fmtMs(dbRow.ms) }];
    if (data.wall_ms) out.push({ label: 'Wall', value: fmtMs(data.wall_ms) });
    out.push({ label: 'AAS', value: fmtAas(dbRow.aas) });
    if (idleRow) out.push({ label: 'Idle', value: fmtMs(idleRow.ms) });
    out.push({ label: 'CPUs', value: String(numCpus) });
    return out;
}

function summaryHtml(metrics) {
    return metrics.map(m =>
        '<div class="metric"><span class="metric-label">' + m.label +
        '</span><span class="metric-value">' + m.value + '</span></div>'
    ).join('');
}

export function createOverviewView() {
    return {
        id: 'overview',

        async requests(ctx) {
            const aParams = {
                from: ctx.timeRange.from,
                to: ctx.timeRange.to,
                filters: ctx.filters.snapshot(),
            };
            const a = ctx.transport.request(ctx.channel('table.a'), 'time_model', aParams);
            if (!ctx.compare || !ctx.compare.enabled) return a;
            const bw = shiftWindow(aParams, ctx.compare.offsetNs);
            const predates = bw.from < ctx.server.fromNs;
            if (predates) {
                return { compare: true, a: await a, b: null,
                    baselinePredates: true, baselineUnavailable: false };
            }
            const b = settleBaseline(ctx.transport.request(ctx.channel('table.b'),
                'time_model', Object.assign({}, aParams, bw)));
            const [aData, bResult] = await Promise.all([a, b]);
            return { compare: true, a: aData,
                b: bResult.ok ? bResult.payload : null,
                baselinePredates: false, baselineUnavailable: !bResult.ok };
        },

        build(data, ctx) {
            if (data && data.compare && data.baselineUnavailable) {
                const primary = data.a;
                return {
                    table: buildTableModel(overviewConfig, primary && primary.rows, null),
                    summary: buildSummary(primary, ctx.server.numCpus),
                    hasRows: !!(primary && primary.rows && primary.rows.length),
                    paneFidelity: buildPaneFidelity(primary),
                    truncation: buildTruncationRow(primary),
                    baselineUnavailable: true,
                };
            }
            if (data && data.compare) {
                const delta = buildDeltaComparison('overview', data.a, data.b,
                    { baselineUnavailable: data.baselinePredates });
                const sort = defaultDeltaSort(ctx.getSort('overview'));
                return {
                    compare: true,
                    table: buildTableModel(compareOverviewConfig, delta.rows, sort),
                    summary: delta.baselineUnavailable ? [
                        { label: 'DB Time A', value: fmtMs(delta.dbTimeA) },
                    ] : [
                        { label: 'DB Time A', value: fmtMs(delta.dbTimeA) },
                        { label: 'DB Time B', value: fmtMs(delta.dbTimeB) },
                        { label: 'Δ', value: (delta.dbTimeA >= delta.dbTimeB ? '+' : '−') +
                            fmtMs(Math.abs(delta.dbTimeA - delta.dbTimeB)) },
                    ],
                    hasRows: delta.rows.length > 0 || !!delta.truncation,
                    compareFidelity: buildComparePaneFidelity(data.a, data.b,
                        { baselinePredates: data.baselinePredates }),
                    truncation: delta.truncation,
                    sort,
                    baselinePredates: delta.baselineUnavailable,
                };
            }
            // Overview keeps its server-defined hierarchy: no client sort.
            const table = buildTableModel(overviewConfig, data && data.rows, null);
            const summary = buildSummary(data, ctx.server.numCpus);
            return {
                table, summary,
                hasRows: !!(data && data.rows && data.rows.length),
                // U2 (review P10): sampled/mixed numbers are scaled
                // estimates — badge the pane; render the truncation row
                // when the server flags the list incomplete.
                paneFidelity: buildPaneFidelity(data),
                truncation: buildTruncationRow(data),
            };
        },

        mount(el, model, ctx) {
            const summaryEl = ctx.summaryEl;
            if (summaryEl) summaryEl.innerHTML = summaryHtml(model.summary);

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
            const config = model.compare ? compareOverviewConfig : overviewConfig;
            ctx.mountTable(el, config, model.table, {
                sort: model.compare ? model.sort : null,
                onSort: model.compare ? (key) => {
                    ctx.toggleSort('overview', key); ctx.refresh();
                } : null,
                onRowClick: (row) => {
                    const intent = config.onClick(row);
                    if (intent) ctx.onDrill(intent);
                },
                truncation: model.truncation,
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
