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
import { overviewConfig } from '../lib/builders/table-configs.js';
import { buildPaneFidelity, paneFidelityBadgeHtml } from '../lib/panels.js';
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
            return ctx.transport.request(ctx.channel('table'), 'time_model', {
                from: ctx.timeRange.from,
                to: ctx.timeRange.to,
                filters: ctx.filters.snapshot(),
            });
        },

        build(data, ctx) {
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
                el.innerHTML = paneFidelityBadgeHtml(model.paneFidelity) +
                    '<div class="loading">No data for selected range</div>';
                return;
            }
            ctx.mountTable(el, overviewConfig, model.table, {
                onRowClick: (row) => {
                    const intent = overviewConfig.onClick(row);
                    if (intent) ctx.onDrill(intent);
                },
                truncation: model.truncation,
            });
            el.insertAdjacentHTML('afterbegin',
                paneFidelityBadgeHtml(model.paneFidelity));
        },

        enter() { /* no chart */ },
        leave() { /* no chart */ },
    };
}
