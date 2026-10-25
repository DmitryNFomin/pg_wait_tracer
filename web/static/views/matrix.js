/* pgwt — transition matrix view. */

import {
    buildMatrixOption, matrixCellIntent,
    matrixChartFitHeight, MATRIX_CHART_NATURAL_PX, MATRIX_CHART_MIN_PX,
} from '../lib/builders/matrix.js';
import { isUnavailable } from '../lib/builders/fidelity.js';
import { mountUnavailablePanel } from '../lib/panels.js';

export function createMatrixView() {
    let chart = null, ctxRef = null, prevLabels = null, labelsRef = null;
    function disposeChart() { if (chart) { chart.dispose(); chart = null; } }

    /* #171: build the title + notes + chart shell ONCE (the histogram/
     * concurrency/timeline ensureShell pattern); refreshes only setOption on
     * the persisted instance. The old el.innerHTML + echarts.init on EVERY
     * mount tore the matrix down and rebuilt it every live tick even when
     * cell values (and STABILITY's held row/column order) had not moved --
     * the whole panel flashed against unchanged data (issue #171). Rebuilt
     * lazily whenever another render (unavailable/no-data) replaced the
     * container's content -- a GENUINE identity change. */
    function ensureShell(el) {
        if (document.getElementById('matrix-chart')) return;
        disposeChart();   // shell is being rebuilt -- any old chart DOM is gone
        el.innerHTML =
            '<div class="matrix-shell">' +
            ' <div class="view-title">Transition matrix <span>cell color is log-scaled count; ' +
            'label color is event identity</span></div>' +
            ' <div id="matrix-cpu-note" class="panel-caption"></div>' +
            ' <div id="matrix-chart"></div>' +
            ' <div id="matrix-notes" class="chart-notes"></div>' +
            '</div>';
    }

    /* #473 item 1: the chart's CSS height (660px) is a FLOOR-less fixed
     * value -- see matrixChartFitHeight's doc comment in lib/builders/
     * matrix.js. Same shape as waterfall.js's fitExecutionList: measure how
     * much the chart's natural height would overflow window.innerHeight at
     * its current DOM position and shrink by exactly that much (plus a
     * fold reserve), never below the CSS-floor minimum. Always budgeted
     * from the NATURAL height (not the previously-set one), so repeated
     * resizes cannot ratchet the chart down further than the current
     * viewport actually requires. */
    function fitChartHeight() {
        const host = document.getElementById('matrix-chart');
        if (!host || !chart) return;
        const top = host.getBoundingClientRect().top;
        const overflow = (top + MATRIX_CHART_NATURAL_PX) - window.innerHeight;
        const px = matrixChartFitHeight(
            MATRIX_CHART_NATURAL_PX, MATRIX_CHART_MIN_PX, overflow);
        host.style.height = px + 'px';
        chart.resize();
    }

    return {
        id: 'matrix', pausesLive: true,
        async requests(ctx) {
            ctxRef = ctx;
            return ctx.transport.request(ctx.channel('matrix'), 'transitions', {
                from: ctx.timeRange.from, to: ctx.timeRange.to,
                filters: ctx.filters.snapshot(), buckets: 200,
            });
        },
        build(data) {
            if (isUnavailable(data)) return { unavailable: data };
            // Row/column order is held across ticks (STABILITY, #104): pass
            // last tick's label order back in so unchanged events don't
            // reshuffle; a fresh mount (leave() clears prevLabels) starts
            // from rank order again.
            const model = buildMatrixOption(data, { limit: 20, prevLabels });
            if (model.hasData) prevLabels = model.labels;
            return model;
        },
        mount(el, model, ctx) {
            ctxRef = ctx; labelsRef = model.labels;
            if (ctx.summaryEl) ctx.summaryEl.innerHTML = '';
            if (model.unavailable) {
                disposeChart();
                mountUnavailablePanel(el, model.unavailable, ctx);
                return;
            }
            if (!model.hasData) {
                disposeChart();
                el.innerHTML = '<div class="loading">No transitions found</div>';
                return;
            }
            ensureShell(el);
            document.getElementById('matrix-cpu-note').textContent = model.cpuNote || '';
            document.getElementById('matrix-notes').textContent = model.notes.join(' · ');
            const host = document.getElementById('matrix-chart');
            if (!chart) {
                chart = ctx.echarts.init(host, 'dark');
                // labelsRef (not the `model` this closure was created against)
                // -- the persisted instance survives many ticks, and the
                // label set can shift as events enter/leave the top-20
                // (STABILITY #104); a captured-at-attach `model` would map a
                // later click to a stale label.
                chart.on('click', (p) => {
                    if (!p || p.componentType !== 'series' || !p.data) return;
                    const intent = matrixCellIntent(p.data, labelsRef);
                    if (intent) ctxRef.onDrill(intent);
                });
            }
            chart.setOption(model.option, true);
            fitChartHeight();
        },
        enter(ctx) { ctxRef = ctx; },
        leave() { disposeChart(); prevLabels = null; labelsRef = null; },
        resize() { fitChartHeight(); },
    };
}
