/* pgwt — per-execution waterfall view. Selection is view-local; every
 * navigation into the view still arrives through app.js's PIVOTS registry. */

import {
    buildExecutionsModel, buildWaterfallOption, buildWaterfallReadout,
    executionsConfig,
} from '../lib/builders/waterfall.js';
import { isUnavailable } from '../lib/builders/fidelity.js';
import { mountUnavailablePanel } from '../lib/panels.js';
import { attachSelection } from '../lib/selection.js';
import { esc, fmtTimeNs } from '../lib/format.js';

function sameExecution(a, b) {
    return !!(a && b && Number(a.pid) === Number(b.pid) &&
        String(a.start_ns) === String(b.start_ns));
}

export function createWaterfallView() {
    let chart = null;
    let detachSelection = null;
    let ctxRef = null;
    let selected = null;
    let pendingSelection = null;
    let detailRef = null;
    let detailOpts = null;
    let currentWindow = null;

    function disposeChart() {
        if (detachSelection) { detachSelection(); detachSelection = null; }
        if (chart) { chart.dispose(); chart = null; }
    }

    function chooseExecution(rows) {
        if (pendingSelection) {
            const authoritative = rows.find(r => sameExecution(r, pendingSelection));
            selected = authoritative || pendingSelection;
            pendingSelection = null;
            return selected;
        }
        if (selected) {
            const stillThere = rows.find(r => sameExecution(r, selected));
            if (stillThere) { selected = stillThere; return selected; }
        }
        selected = rows[0] || null;
        return selected;
    }

    function setReadout(tuple) {
        const host = document.getElementById('waterfall-readout');
        if (!host) return;
        const r = buildWaterfallReadout(tuple);
        if (!r) { host.textContent = 'Click a bar to inspect it.'; return; }
        host.innerHTML =
            '<span class="wf-inspect-name">' + esc(r.name) + '</span>' +
            '<span>PID ' + r.pid + '</span>' +
            '<span>Start ' + esc(r.start) + '</span>' +
            '<span>Duration ' + esc(r.duration) + '</span>' +
            (r.cpu == null ? '' : '<span>Measured CPU ' + esc(r.cpu) + '</span>');
    }

    function updateZoomLabel(model) {
        const el = document.getElementById('waterfall-zoom-state');
        if (!el || !model) return;
        if (!currentWindow || (currentWindow.from === model.fullFrom &&
            currentWindow.to === model.fullTo)) {
            el.textContent = 'Full execution';
        } else {
            el.textContent = 'Zoomed: ' + fmtTimeNs(currentWindow.from, 1_000_000) +
                '–' + fmtTimeNs(currentWindow.to, 1_000_000) + ' UTC';
        }
    }

    function renderDetail(from, to) {
        const host = document.getElementById('waterfall-chart');
        if (!host || !detailRef) return;
        const model = buildWaterfallOption(detailRef,
            Object.assign({}, detailOpts, from == null ? {} : { from, to }));
        if (!model.hasData) {
            disposeChart();
            host.innerHTML = '<div class="loading">No execution events captured</div>';
            return;
        }
        currentWindow = { from: model.windowFrom, to: model.windowTo };
        host.style.height = model.chartHeight + 'px';
        if (!chart) {
            chart = ctxRef.echarts.init(host, 'dark');
            chart.on('click', (p) => {
                if (p && p.componentType === 'series' && p.data) setReadout(p.data);
            });
            detachSelection = attachSelection(host, chart, {
                onSelect: (range) => {
                    const origin = BigInt(model.axisOrigin);
                    renderDetail(String(origin + BigInt(Math.round(range.from))),
                                 String(origin + BigInt(Math.round(range.to))));
                },
            });
            host.addEventListener('dblclick', (e) => {
                e.preventDefault();
                const full = buildWaterfallOption(detailRef, detailOpts);
                renderDetail(full.fullFrom, full.fullTo);
            });
        }
        chart.setOption(model.option, true);
        updateZoomLabel(model);
    }

    const view = {
        id: 'waterfall',
        pausesLive: true,

        /* Called only by app.js's registered scatter-execution pivot. */
        selectExecution(intent) {
            if (!intent || !intent.pid || !intent.start_ns) return;
            pendingSelection = {
                pid: intent.pid, start_ns: String(intent.start_ns),
                end_ns: intent.end_ns == null ? null : String(intent.end_ns),
                query_id: String(intent.query_id || '0'),
                duration_ms: intent.duration_ms,
                in_progress: intent.end_ns == null,
            };
        },

        getSelection() {
            const row = pendingSelection || selected;
            if (!row) return null;
            return { pid: Number(row.pid), start_ns: String(row.start_ns),
                end_ns: row.end_ns == null ? null : String(row.end_ns),
                query_id: String(row.query_id || '0') };
        },

        async requests(ctx) {
            ctxRef = ctx;
            const executions = await ctx.transport.request(ctx.channel('executions'),
                'executions', {
                    from: ctx.timeRange.from, to: ctx.timeRange.to, limit: 100,
                    filters: ctx.filters.snapshot(),
                });
            if (isUnavailable(executions)) return { executions, detail: null, selected: null };
            const chosen = chooseExecution(executions.rows || []);
            if (!chosen) return { executions, detail: null, selected: null };
            const detailEnd = chosen.end_ns != null
                ? String(chosen.end_ns) : String(Math.round(ctx.timeRange.to));
            const detailFilters = Object.assign({}, ctx.filters.snapshot(), { pid: chosen.pid });
            const detail = await ctx.transport.request(ctx.channel('detail'),
                'execution_detail', {
                    filters: detailFilters, start_ns: String(chosen.start_ns),
                    end_ns: detailEnd,
                });
            return { executions, detail, selected: chosen, detailEnd };
        },

        build(data) {
            if (isUnavailable(data.executions)) return { unavailable: data.executions };
            if (isUnavailable(data.detail)) return { unavailable: data.detail };
            const table = buildExecutionsModel(data.executions, data.selected);
            const wf = data.detail ? buildWaterfallOption(data.detail, {
                executionStart: data.selected.start_ns,
                executionEnd: data.detailEnd,
            }) : null;
            return { table, waterfall: wf, detail: data.detail,
                selected: data.selected, detailEnd: data.detailEnd };
        },

        mount(el, model, ctx) {
            ctxRef = ctx;
            disposeChart();
            detailRef = null; detailOpts = null; currentWindow = null;
            if (ctx.summaryEl) ctx.summaryEl.innerHTML = '';
            if (model.unavailable) {
                mountUnavailablePanel(el, model.unavailable, ctx);
                return;
            }
            el.innerHTML =
                '<section class="execution-list">' +
                ' <div class="view-title">Executions <span>latest first</span></div>' +
                ' <div id="executions-table"></div>' +
                '</section>' +
                '<section class="waterfall-pane">' +
                ' <div class="waterfall-banner"><span>Plain drag zooms this loaded execution; ' +
                'double-click restores it.</span><span id="waterfall-zoom-state"></span></div>' +
                ' <div id="waterfall-truncation"></div>' +
                ' <div id="waterfall-chart"></div>' +
                ' <div id="waterfall-readout" class="chart-readout">Click a bar to inspect it.</div>' +
                '</section>';
            const tableHost = document.getElementById('executions-table');
            if (!model.table.hasRows) {
                tableHost.innerHTML = '<div class="loading">No executions for selected range</div>';
                document.querySelector('.waterfall-pane').style.display = 'none';
                return;
            }
            ctx.mountTable(tableHost, executionsConfig, model.table.table, {
                onRowClick: (row) => ctx.onDrill(Object.assign({
                    pivot: 'waterfall-execution',
                }, row)),
                truncation: model.table.truncation,
            });
            if (!model.detail || !model.waterfall) {
                document.getElementById('waterfall-chart').innerHTML =
                    '<div class="loading">Select an execution</div>';
                return;
            }
            detailRef = model.detail;
            detailOpts = { executionStart: model.selected.start_ns,
                executionEnd: model.detailEnd };
            const lines = [];
            if (model.waterfall.truncated && model.waterfall.total_count != null) {
                lines.push('Showing ' + model.waterfall.kept_count + ' of ' +
                    model.waterfall.total_count + ' execution events.');
            }
            for (const lane of model.waterfall.laneTruncations) {
                lines.push(lane.total_count == null
                    ? 'PID ' + lane.pid + ': more events not shown.'
                    : 'PID ' + lane.pid + ': showing ' + lane.kept_count + ' of ' +
                      lane.total_count + ' events.');
            }
            if (model.selected.in_progress) {
                lines.push('Execution is in progress; detail is captured through ' +
                    fmtTimeNs(model.detailEnd, 1_000_000) + ' UTC.');
            }
            document.getElementById('waterfall-truncation').textContent = lines.join(' ');
            document.getElementById('waterfall-truncation').className = lines.length
                ? 'honesty-note' : '';
            renderDetail(model.waterfall.fullFrom, model.waterfall.fullTo);
        },

        enter(ctx) { ctxRef = ctx; },
        leave() {
            disposeChart(); detailRef = null; currentWindow = null;
            pendingSelection = null; selected = null;
        },
        resize() { if (chart) chart.resize(); },
    };
    return view;
}
