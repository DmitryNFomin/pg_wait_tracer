/* pgwt — transition matrix view. */

import { buildMatrixOption, matrixCellIntent } from '../lib/builders/matrix.js';
import { isUnavailable } from '../lib/builders/fidelity.js';
import { mountUnavailablePanel } from '../lib/panels.js';

export function createMatrixView() {
    let chart = null, ctxRef = null;
    function disposeChart() { if (chart) { chart.dispose(); chart = null; } }
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
            return buildMatrixOption(data, { limit: 20 });
        },
        mount(el, model, ctx) {
            ctxRef = ctx;
            if (ctx.summaryEl) ctx.summaryEl.innerHTML = '';
            disposeChart();
            if (model.unavailable) {
                mountUnavailablePanel(el, model.unavailable, ctx);
                return;
            }
            if (!model.hasData) {
                el.innerHTML = '<div class="loading">No transitions found</div>';
                return;
            }
            el.innerHTML =
                '<div class="matrix-shell">' +
                ' <div class="view-title">Transition matrix <span>cell color is log-scaled count; ' +
                'label color is event identity</span></div>' +
                ' <div id="matrix-notes" class="chart-notes"></div>' +
                ' <div id="matrix-chart"></div>' +
                '</div>';
            document.getElementById('matrix-notes').textContent = model.notes.join(' · ');
            const host = document.getElementById('matrix-chart');
            chart = ctx.echarts.init(host, 'dark');
            chart.on('click', (p) => {
                if (!p || p.componentType !== 'series' || !p.data) return;
                const intent = matrixCellIntent(p.data, model.labels);
                if (intent) ctxRef.onDrill(intent);
            });
            chart.setOption(model.option, true);
        },
        enter(ctx) { ctxRef = ctx; },
        leave() { disposeChart(); },
        resize() { if (chart) chart.resize(); },
    };
}
