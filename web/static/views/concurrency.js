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

import {
    buildConcurrencyOption, buildPeaksPanelHtml, buildBurstsPanelHtml,
} from '../lib/builders/concurrency.js';
import { isUnavailable } from '../lib/builders/fidelity.js';
import { mountUnavailablePanel } from '../lib/panels.js';

export function createConcurrencyView() {
    let chart = null;    // ECharts instance — owned here, nowhere else
    let ctxRef = null;   // last ctx (row-click → onZoom)
    let selected = 'peaks';   // narrow-layout selector state (view-local, like waterfall.js's `selected`)

    function disposeChart() { if (chart) { chart.dispose(); chart = null; } }

    /* #442 (review finding, separate from the owner-held threshold issue):
     * the two tables used to share ONE 640px-capped scroll container
     * (#burst-table), so the peaks table (ten rows fits easily) silently ate
     * the budget a 300-row burst table needed and Burst Events — the thing
     * the chart's red markers point at — never came above the fold. Two
     * independently-bounded panels, side by side at the demo viewport
     * (style.css .concurrency-panels), each keeping its own scrollbar and
     * its own final-row reachability. A narrow viewport instead shows one
     * panel at a time via the always-visible selector below, since side by
     * side has no room left for either table's columns. */
    function ensureShell(el) {
        if (document.getElementById('concurrency-chart')) return;
        disposeChart();   // shell is being rebuilt — any old chart DOM is gone
        el.innerHTML =
            '<div id="concurrency-chart" style="width:100%;height:270px;"></div>' +
            '<div class="concurrency-panel-selector">' +
                '<button type="button" class="cp-select active" data-panel="peaks">' +
                    'Peaks (<span class="cp-count-peaks">0</span>)</button>' +
                '<button type="button" class="cp-select" data-panel="bursts">' +
                    'Bursts (<span class="cp-count-bursts">0</span>)</button>' +
            '</div>' +
            '<div class="concurrency-panels">' +
                '<div id="peaks-table" class="concurrency-panel"></div>' +
                '<div id="burst-table" class="concurrency-panel"></div>' +
            '</div>';
        // P3 wire 2: one delegated listener per panel; rows carry
        // data-from/data-to (ns) from the pure builder (the EMITTER pads, per
        // the app's registry). Number() loses <256ns at epoch scale — the
        // same double precision every ns value in the app already lives
        // with. Emitted as the 'burst-zoom' pivot intent (pure time jump,
        // current tab kept); ctx.onZoom is the fallback for an app shell
        // without the registry.
        function onRowClick(e) {
            const tr = e.target && e.target.closest && e.target.closest('tr[data-from]');
            if (!tr || !ctxRef) return;
            const from = Number(tr.dataset.from), to = Number(tr.dataset.to);
            if (!isFinite(from) || !isFinite(to) || to <= from) return;
            if (ctxRef.onDrill) ctxRef.onDrill({ pivot: 'burst-zoom', from, to });
            else if (ctxRef.onZoom) ctxRef.onZoom(from, to);
        }
        document.getElementById('peaks-table').addEventListener('click', onRowClick);
        document.getElementById('burst-table').addEventListener('click', onRowClick);

        // Narrow-layout selector: only visible under the media-query width
        // (style.css), but the click handler is unconditional and harmless
        // at any width -- CSS alone decides whether `.cp-hidden` has effect.
        el.querySelector('.concurrency-panel-selector').addEventListener('click', (e) => {
            const btn = e.target.closest('button[data-panel]');
            if (!btn) return;
            selected = btn.dataset.panel;
            applySelection(el);
        });
    }

    function applySelection(el) {
        el.querySelectorAll('.cp-select').forEach(b =>
            b.classList.toggle('active', b.dataset.panel === selected));
        const peaks = document.getElementById('peaks-table');
        const bursts = document.getElementById('burst-table');
        if (peaks) peaks.classList.toggle('cp-hidden', selected !== 'peaks');
        if (bursts) bursts.classList.toggle('cp-hidden', selected !== 'bursts');
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

        build(data, ctx) {
            // EXACT-required (A3): a sampled-only window yields the structured
            // "unavailable" marker — surfaced as an explicit escalate panel.
            if (isUnavailable(data)) return { unavailable: data };
            // #105: ctx.server.fromNs is the server's earliest captured
            // timestamp (same field events.js/overview.js/active.js already
            // use for "predates the trace") — buckets before it are marked
            // "not captured", never painted as a measured zero.
            const captureFromNs = ctx && ctx.server ? ctx.server.fromNs : null;
            return buildConcurrencyOption(data, { captureFromNs });
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
            document.getElementById('peaks-table').innerHTML = buildPeaksPanelHtml(model);
            document.getElementById('burst-table').innerHTML = buildBurstsPanelHtml(model);
            const peaksCount = el.querySelector('.cp-count-peaks');
            const burstsCount = el.querySelector('.cp-count-bursts');
            if (peaksCount) peaksCount.textContent = String(model.topPeaks.length);
            if (burstsCount) burstsCount.textContent = String(model.bursts.length);
            applySelection(el);
        },

        enter(ctx) { ctxRef = ctx; /* chart created lazily in mount */ },
        leave() { disposeChart(); },
        resize() { if (chart) chart.resize(); },
    };
}
