/* pgwt — "concurrency" view: peak concurrent sessions overlay + burst tables.
 *
 * Migrated to the { id, requests, build, mount, enter, leave } contract (B3
 * part 3). requests() fetches concurrency on a single-flight channel; build()
 * is PURE (lib/builders/concurrency.js -> ECharts option + table models);
 * mount() reuses a stable shell (the histogram ensureShell pattern, P5): the
 * ECharts instance is created ONCE and fed setOption on every refresh — the
 * old dispose/echarts.init/setOption(_, true) per mount replayed the ~1s
 * draw-in on every tab entry/zoom and destroyed open tooltips. The view OWNS
 * its instance: disposed in leave() — no module-level chart global.
 *
 * U2 (P3 wire 2): peak/burst table rows are zoom intents — clicking one
 * emits the 'burst-zoom' pivot (ctx.onDrill; zoomTo(ts ± 5 buckets), no
 * filter, current tab kept). The pure builder embeds the ns range as
 * data-from/data-to on each row; the view only delegates the click.
 *
 * Behavior otherwise matches the old legacy adapter in app.js:
 *   - Empty peaks → "No concurrency data".
 *   - Peak area-line with burst markers + the top-peaks and burst tables.
 *   - Bucket count scales with the AAS chart width (same heuristic as before).
 */

import { buildConcurrencyOption, buildConcurrencyTables } from '../lib/builders/concurrency.js';
import { isUnavailable } from '../lib/builders/fidelity.js';
import { mountUnavailablePanel } from '../lib/panels.js';

export function createConcurrencyView() {
    let chart = null;    // ECharts instance — owned here, nowhere else
    let ctxRef = null;   // last ctx (row-click → onZoom)

    function disposeChart() { if (chart) { chart.dispose(); chart = null; } }

    /* P5: build the chart + table containers ONCE; refreshes only setOption /
     * re-fill the table. Rebuilt lazily whenever another view (or an
     * unavailable/no-data mount) replaced the container's content. */
    function ensureShell(el) {
        if (document.getElementById('concurrency-chart')) return;
        disposeChart();   // shell is being rebuilt — any old chart DOM is gone
        el.innerHTML =
            '<div id="concurrency-chart" style="width:100%;height:350px;"></div>' +
            '<div id="burst-table" style="padding:10px;"></div>';
        // P3 wire 2: one delegated listener; rows carry data-from/data-to (ns)
        // from the pure builder (the EMITTER pads, per the app's registry).
        // Number() loses <256ns at epoch scale — the same double precision
        // every ns value in the app already lives with. Emitted as the
        // 'burst-zoom' pivot intent (pure time jump, current tab kept);
        // ctx.onZoom is the fallback for an app shell without the registry.
        document.getElementById('burst-table').addEventListener('click', (e) => {
            const tr = e.target && e.target.closest && e.target.closest('tr[data-from]');
            if (!tr || !ctxRef) return;
            const from = Number(tr.dataset.from), to = Number(tr.dataset.to);
            if (!isFinite(from) || !isFinite(to) || to <= from) return;
            if (ctxRef.onDrill) ctxRef.onDrill({ pivot: 'burst-zoom', from, to });
            else if (ctxRef.onZoom) ctxRef.onZoom(from, to);
        });
    }

    return {
        id: 'concurrency',

        async requests(ctx) {
            const chartWidth = (ctx.chartEl && ctx.chartEl.clientWidth) || 800;
            const numBuckets = Math.min(Math.floor(chartWidth / 4), 300);
            return ctx.transport.request(ctx.channel('table'), 'concurrency', {
                from: ctx.timeRange.from, to: ctx.timeRange.to,
                buckets: numBuckets, filters: ctx.filters.snapshot(),
            });
        },

        build(data) {
            // EXACT-required (A3): a sampled-only window yields the structured
            // "unavailable" marker — surfaced as an explicit escalate panel.
            if (isUnavailable(data)) return { unavailable: data };
            return buildConcurrencyOption(data);
        },

        mount(el, model, ctx) {
            ctxRef = ctx;
            if (model.unavailable) {
                disposeChart();
                mountUnavailablePanel(el, model.unavailable, ctx);
                return;
            }
            if (ctx.summaryEl) ctx.summaryEl.innerHTML = '';
            if (!model.hasData) {
                disposeChart();
                el.innerHTML = '<p style="color:#888;padding:20px">No concurrency data</p>';
                return;
            }
            ensureShell(el);
            if (!chart) {
                chart = ctx.echarts.init(
                    document.getElementById('concurrency-chart'), 'dark');
            }
            chart.setOption(model.option, true);
            document.getElementById('burst-table').innerHTML = buildConcurrencyTables(model);
        },

        enter(ctx) { ctxRef = ctx; /* chart created lazily in mount */ },
        leave() { disposeChart(); },
        resize() { if (chart) chart.resize(); },
    };
}
