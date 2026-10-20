/* pgwt — per-execution waterfall view. Selection is view-local; every
 * navigation into the view still arrives through app.js's PIVOTS registry. */

import {
    buildExecutionsModel, buildWaterfallOption, buildWaterfallReadout,
    executionListMaxHeight, waterfallChartFitHeight, containingActivityWindow, waterfallContextHtml,
    executionsConfig, pickDefaultExecution, EXECUTIONS_SORT_DEFAULT,
    executionsSortLabel, executionsSortToggleLabel, executionsSortToggleTarget,
    executionsStatusLabel, execScopeNoteHtml, zoomLabelText,
} from '../lib/builders/waterfall.js';
import { backendActivityHtml } from '../lib/builders/backend-span.js';
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
    let naturalChartHeight = null;
    let detachSelection = null;
    let ctxRef = null;
    let selected = null;
    let pendingSelection = null;
    let preferredPid = null;
    let detailRef = null;
    let detailOpts = null;
    let currentWindow = null;
    // #222: which slice requests() asks the server for -- see
    // EXECUTIONS_SORT_* in lib/builders/waterfall.js for why the default is
    // duration_desc. View-local, like `selected`; not persisted to the URL
    // hash (a deliberate scope cut, not an oversight -- see PR body).
    let sortMode = EXECUTIONS_SORT_DEFAULT;
    // #171: the drag-select listener below is attached ONCE against the
    // persisted chart instance, so it must read the CURRENT axisOrigin
    // through this ref rather than close over the `model` local from
    // whichever renderDetail() call happened to create it -- a later tick's
    // model would otherwise silently zoom against a stale origin.
    let axisOriginRef = null;

    function disposeChart() {
        if (detachSelection) { detachSelection(); detachSelection = null; }
        if (chart) { chart.dispose(); chart = null; }
        naturalChartHeight = null;
    }

    /* #171: build the executions-list + waterfall-pane shell ONCE (the
     * histogram/concurrency/timeline ensureShell pattern); refreshes only
     * refill the table and feed the persisted chart instance (renderDetail
     * already does that part -- see its own `if (!chart)` guard). The old
     * el.innerHTML on EVERY mount tore the whole panel down and rebuilt it
     * every live tick even when the selected execution had not changed --
     * the whole panel flashed against unchanged data (issue #171). Rebuilt
     * lazily whenever an unavailable mount replaced the container's content
     * -- a GENUINE identity change. */
    function ensureShell(el) {
        if (document.getElementById('executions-table')) return;
        disposeChart();   // shell is being rebuilt -- any old chart DOM is gone
        el.innerHTML =
            '<section class="execution-list">' +
            ' <div class="view-title">Executions ' +
            '<span id="executions-sort-label"></span>' +
            '<button id="executions-sort-toggle" class="link-button" type="button"></button>' +
            '<span id="executions-counts-label"></span>' +
            '</div>' +
            ' ' + execScopeNoteHtml() +
            ' <div id="executions-table"></div>' +
            '</section>' +
            '<section class="waterfall-pane">' +
            ' <div id="waterfall-context" class="waterfall-context"></div>' +
            ' <div class="waterfall-banner"><span>Plain drag zooms this loaded execution; ' +
            'double-click restores it.</span><span id="waterfall-zoom-state"></span></div>' +
            ' <div id="waterfall-truncation"></div>' +
            ' <div id="waterfall-chart"></div>' +
            ' <div id="waterfall-readout" class="chart-readout">Click a bar to inspect it.</div>' +
            // Integration finding: this panel sat ABOVE the chart. Empty, it
            // cost nothing -- and it WAS empty until piece 3 taught the mock
            // (and the real server) to send execution_detail's
            // backend_activity_span. Populated it is 134 px tall, which pushed
            // the ECharts canvas from top=675 to top=747 in the gated
            // 1280x900 viewport: canvas bottom 979, i.e. 79 px below the fold,
            // and the per-bar readout became unreachable (tests/test_web_ui.py
            // "bar click opened compact readout"). It belongs beside the
            // per-bar readout anyway: both are detail about what is selected.
            ' <div id="waterfall-backend-activity" class="backend-activity-inline"></div>' +
            '</section>';
        // Wired once against the persisted shell (like the chart's drag
        // listener above): sortMode is a `let` in this closure, so the
        // handler always reads its CURRENT value, and ctxRef is reassigned
        // on every mount() before a click can fire.
        document.getElementById('executions-sort-toggle')
            .addEventListener('click', () => {
                sortMode = executionsSortToggleTarget(sortMode);
                if (ctxRef) ctxRef.refresh();
            });
    }

    function updateSortControl() {
        const label = document.getElementById('executions-sort-label');
        const toggle = document.getElementById('executions-sort-toggle');
        if (label) label.textContent = executionsSortLabel(sortMode);
        if (toggle) toggle.textContent = executionsSortToggleLabel(sortMode);
    }

    function chooseExecution(rows, filters) {
        const matches = r => (filters.pid == null || Number(r.pid) === Number(filters.pid)) &&
            (filters.query_id == null || String(r.query_id) === String(filters.query_id));
        rows = rows.filter(matches);
        if (pendingSelection && !matches(pendingSelection)) pendingSelection = null;
        if (selected && !matches(selected)) selected = null;
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
        /* Not rows[0]: the newest execution on a real workload usually has
         * no events, no workers and no plan, and an empty detail leaves the
         * panel with no chart at all (issue #101). */
        // A query's longest backend span need not own an execution of that
        // query. Prefer its PID only within the already-filtered executions;
        // keep explicit session filters authoritative and allow query fallback.
        const preferred = preferredPid == null ? [] :
            rows.filter(r => Number(r.pid) === Number(preferredPid));
        selected = pickDefaultExecution(preferred.length ? preferred : rows);
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

    /* #415: give the executions list exactly the height left over once the
     * waterfall pane below it is fully on screen -- see
     * executionListMaxHeight() in lib/builders/waterfall.js for why the
     * list yields down to its measured row floor. Then #462 takes any
     * remaining overflow from the chart, respecting its CSS minimum.
     * Always budget from the natural chart height so repeated resizes
     * cannot trade the recovered chart space back into the list. */
    function fitExecutionList() {
        const list = document.querySelector('.execution-list');
        const pane = document.querySelector('.waterfall-pane');
        if (!list || !pane || pane.style.display === 'none') return;
        const host = document.getElementById('waterfall-chart');
        if (chart && naturalChartHeight != null) {
            host.style.height = naturalChartHeight + 'px';
        }
        const paneBox = pane.getBoundingClientRect();
        const listBox = list.getBoundingClientRect();
        const style = getComputedStyle(list);
        const borderBottom = parseFloat(style.borderBottomWidth) || 0;
        const borderTop = parseFloat(style.borderTopWidth) || 0;
        const scrollbarHeight = Math.max(0,
            list.offsetHeight - list.clientHeight - borderTop - borderBottom);
        const rowListHeights = [...list.querySelectorAll('tbody tr.clickable')]
            .map(row => row.getBoundingClientRect().bottom - listBox.top +
                list.scrollTop + borderBottom + scrollbarHeight);
        const context = document.getElementById('waterfall-context');
        const contextHeight = context ? context.getBoundingClientRect().height +
            (parseFloat(getComputedStyle(context).marginBottom) || 0) : 0;
        const px = executionListMaxHeight({
            viewportHeight: window.innerHeight,
            listTop: listBox.top,
            paneHeight: paneBox.height,
            contextHeight,
            rowListHeights,
            // The container owns the trailing padding. document.scrollHeight
            // is at least the viewport height, so it also counts unused space
            // and would shrink the list again on every resize.
            trailingBelowPane: list.parentElement.getBoundingClientRect().bottom
                - paneBox.bottom,
        });
        list.style.maxHeight = px == null ? '' : px + 'px';
        if (chart && naturalChartHeight != null && px != null) {
            // Measure after applying the list cap: a short list can be
            // smaller than its cap. The container includes trailing padding.
            const overflow = list.parentElement.getBoundingClientRect().bottom
                - window.innerHeight;
            const minimum = parseFloat(getComputedStyle(host).minHeight);
            if (Number.isFinite(minimum)) {
                host.style.height = waterfallChartFitHeight(
                    naturalChartHeight, minimum, overflow) + 'px';
            }
        }
        if (chart) chart.resize();
    }

    function updateZoomLabel(model) {
        const el = document.getElementById('waterfall-zoom-state');
        if (!el || !model) return;
        el.textContent = zoomLabelText(currentWindow, model);
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
        axisOriginRef = model.axisOrigin;
        naturalChartHeight = model.chartHeight;
        host.style.height = naturalChartHeight + 'px';
        if (!chart) {
            host.innerHTML = '';   // clear a leftover "no events"/"select an execution" placeholder
            chart = ctxRef.echarts.init(host, 'dark');
            chart.on('click', (p) => {
                if (p && p.componentType === 'series' && p.data) setReadout(p.data);
            });
            detachSelection = attachSelection(host, chart, {
                onSelect: (range) => {
                    const origin = BigInt(axisOriginRef);
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
        fitExecutionList();
    }

    const view = {
        id: 'waterfall',
        pausesLive: true,

        // A new query drill must not retain a previous execution selection.
        selectQuery(intent) {
            pendingSelection = null; selected = null;
            preferredPid = intent?.pid ?? null;
        },

        /* Explicit execution selection from scatter or the executions list. */
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
            const windowTo = ctx.timeRange.to;
            const executions = await ctx.transport.request(ctx.channel('executions'),
                'executions', {
                    from: ctx.timeRange.from, to: windowTo, limit: 100,
                    sort: sortMode, filters: ctx.filters.snapshot(),
                });
            if (isUnavailable(executions))
                return { executions, detail: null, selected: null, windowTo };
            const chosen = chooseExecution(executions.rows || [], ctx.filters.snapshot());
            if (!chosen) return { executions, detail: null, selected: null, windowTo };
            const detailEnd = chosen.end_ns != null
                ? String(chosen.end_ns) : String(Math.round(ctx.timeRange.to));
            const detailFilters = { pid: chosen.pid, query_id: chosen.query_id };
            const detail = await ctx.transport.request(ctx.channel('detail'),
                'execution_detail', {
                    filters: detailFilters, start_ns: String(chosen.start_ns),
                    end_ns: detailEnd,
                });
            const activityWindow = containingActivityWindow(detail, chosen);
            let activityTimeline = null;
            if (activityWindow) {
                try {
                    activityTimeline = await ctx.transport.request(ctx.channel('activity-timeline'),
                        'session_timeline', activityWindow);
                } catch (err) {
                    if (err.name === 'CancelledError') throw err;
                    // Evidence failure must not erase a valid executor chart.
                    // The visible context refuses to claim pre-execution timing.
                }
            }
            return { executions, detail, activityTimeline, selected: chosen, detailEnd, windowTo };
        },

        build(data) {
            if (isUnavailable(data.executions)) return { unavailable: data.executions };
            if (isUnavailable(data.detail)) return { unavailable: data.detail };
            const table = buildExecutionsModel(data.executions, data.selected, data.windowTo);
            const wf = data.detail ? buildWaterfallOption(data.detail, {
                executionStart: data.selected.start_ns,
                executionEnd: data.detailEnd,
            }) : null;
            return { table, waterfall: wf, detail: data.detail,
                selected: data.selected, detailEnd: data.detailEnd,
                context: waterfallContextHtml(data.detail, data.selected, data.activityTimeline),
                // Keep only detailed metadata collapsed; identity, wait, span and
                // scope live in the always-visible context inside the pane.
                backendActivity: data.selected
                    ? '<details><summary>Containing leader backend activity</summary>' +
                        backendActivityHtml(
                            data.detail && Object.hasOwn(data.detail, 'backend_activity_span')
                                ? data.detail : data.selected, true) +
                        '</details>'
                    : '' };
        },

        mount(el, model, ctx) {
            ctxRef = ctx;
            detailRef = null; detailOpts = null; currentWindow = null;
            if (ctx.summaryEl) ctx.summaryEl.innerHTML = '';
            if (model.unavailable) {
                disposeChart();
                mountUnavailablePanel(el, model.unavailable, ctx);
                return;
            }
            ensureShell(el);
            updateSortControl();
            document.getElementById('waterfall-context').innerHTML = model.context || '';
            document.getElementById('waterfall-backend-activity').innerHTML = model.backendActivity || '';
            const countsLabel = document.getElementById('executions-counts-label');
            if (countsLabel) {
                const text = executionsStatusLabel(
                    model.table.open_count, model.table.completed_count,
                    model.table.has_inferred_end);
                countsLabel.textContent = text ? ' · ' + text : '';
            }
            const tableHost = document.getElementById('executions-table');
            const pane = document.querySelector('.waterfall-pane');
            if (!model.table.hasRows) {
                disposeChart();
                tableHost.innerHTML = '<div class="loading">No executions match the selected query/session filters and time range.</div>';
                pane.style.display = 'none';
                return;
            }
            pane.style.display = '';
            ctx.mountTable(tableHost, executionsConfig, model.table.table, {
                onRowClick: (row) => ctx.onDrill(Object.assign({
                    pivot: 'waterfall-execution',
                }, row)),
                truncation: model.table.truncation,
            });
            if (!model.detail || !model.waterfall) {
                disposeChart();
                document.getElementById('waterfall-chart').innerHTML =
                    '<div class="loading">' + (model.selected
                        ? 'No detail was returned for the selected execution.'
                        : 'No execution matches the selected query/session filters and time range.') + '</div>';
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
            axisOriginRef = null;
            pendingSelection = null; selected = null;
            preferredPid = null;
        },
        resize() { fitExecutionList(); },
    };
    return view;
}
