/* pgwt — execution-latency scatter view. */

import {
    buildExecScatterOption, meaningfulScatterYRange, scatterExecutionIntent,
} from '../lib/builders/exec-scatter.js';
import { isUnavailable } from '../lib/builders/fidelity.js';
import { mountUnavailablePanel } from '../lib/panels.js';
import { attachBoxSelection } from '../lib/selection.js';

function yText(a, b) {
    const fmt = (v) => v >= 1000 ? (v / 1000).toFixed(2) + ' s'
        : v >= 1 ? v.toFixed(2) + ' ms' : (v * 1000).toFixed(1) + ' µs';
    return fmt(a) + '–' + fmt(b);
}

export function createExecScatterView() {
    let chart = null, detachSelection = null, ctxRef = null;
    let modelRef = null, yReadout = '', yWindow = null;

    function disposeChart() {
        if (detachSelection) { detachSelection(); detachSelection = null; }
        if (chart) { chart.dispose(); chart = null; }
    }

    return {
        id: 'scatter', pausesLive: true,

        async requests(ctx) {
            ctxRef = ctx;
            return ctx.transport.request(ctx.channel('scatter'), 'exec_scatter', {
                from: ctx.timeRange.from, to: ctx.timeRange.to, max_points: 2000,
                filters: ctx.filters.snapshot(),
            });
        },

        build(data) {
            if (isUnavailable(data)) return { unavailable: data };
            return buildExecScatterOption(data);
        },

        mount(el, model, ctx) {
            ctxRef = ctx; modelRef = model;
            if (ctx.summaryEl) ctx.summaryEl.innerHTML = '';
            disposeChart();
            if (model.unavailable) {
                mountUnavailablePanel(el, model.unavailable, ctx);
                return;
            }
            if (!model.hasData) {
                el.innerHTML = '<div class="scatter-shell"><div class="chart-notes">' +
                    model.notes.join(' · ') + '</div>' +
                    '<div class="loading">No completed, positive-duration executions for selected range</div></div>';
                return;
            }
            el.innerHTML =
                '<div class="scatter-shell">' +
                ' <div class="waterfall-banner">Plain drag zooms time; a 2D box also constrains latency. ' +
                'Double-click zooms out.</div>' +
                ' <div id="scatter-notes" class="chart-notes"></div>' +
                ' <div id="scatter-chart"></div>' +
                ' <div id="scatter-readout" class="chart-readout"></div>' +
                '</div>';
            document.getElementById('scatter-notes').textContent = model.notes.join(' · ');
            document.getElementById('scatter-readout').textContent = yReadout;
            const host = document.getElementById('scatter-chart');
            chart = ctx.echarts.init(host, 'dark');
            chart.on('click', (p) => {
                if (!p || p.componentType !== 'series' || !p.data) return;
                const intent = scatterExecutionIntent(p.data);
                if (intent) ctxRef.onDrill(intent);
            });
            detachSelection = attachBoxSelection(host, chart, {
                onSelect: (box) => {
                    const constrained = box.hasYRange &&
                        meaningfulScatterYRange(box.yFrom, box.yTo,
                        modelRef.yMin, modelRef.yMax);
                    yWindow = constrained ? { min: box.yFrom, max: box.yTo } : null;
                    yReadout = constrained
                        ? 'Selected latency context: ' + yText(box.yFrom, box.yTo) +
                          ' (time and latency ranges applied)' : '';
                    const readout = document.getElementById('scatter-readout');
                    if (readout) readout.textContent = yReadout;
                    ctxRef.onZoom(Math.round(box.from * 1e6), Math.round(box.to * 1e6));
                },
            });
            host.addEventListener('dblclick', (e) => {
                e.preventDefault();
                yWindow = null; yReadout = '';
                if (ctxRef.onZoomOut) ctxRef.onZoomOut();
            });
            if (yWindow) {
                model.option.yAxis.min = yWindow.min;
                model.option.yAxis.max = yWindow.max;
            }
            chart.setOption(model.option, true);
        },

        enter(ctx) { ctxRef = ctx; },
        leave() { disposeChart(); modelRef = null; yReadout = ''; yWindow = null; },
        resize() { if (chart) chart.resize(); },
    };
}
