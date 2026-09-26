/* pgwt — dev fixture gallery (Phase U1, review §6 item 3).
 *
 * Renders every (builder × state) from tests/fixtures (canonically
 * web/static/dev/fixtures/) into a grid of cells, one chart/card per cell,
 * through the REAL pure builders and the app's real render path
 * (echarts.init(el, 'dark') + chart.setOption(option, true)). The builders are
 * treated as black boxes: the gallery calls them and paints what they return —
 * it asserts nothing about colors, series counts or option internals, so
 * builder-side rework (e.g. the P2 identity cluster) changes pixels here
 * without changing this file.
 *
 * Stable surface for tooling (the gallery-cell snapshot suite):
 *   - cell DOM id = gallery-<builder>-<state> (manifest cellId)
 *   - <body data-gallery-ready="1"> once every cell created during the
 *     initial render pass has fired its REAL completion signal (issue #155
 *     — see the "render-settle tracking" section below; this is NOT just
 *     "every renderCell() call returned", which races the chart paint)
 *   - tick-replay cells: data-tick="<n>" AND data-settled="1" on the cell
 *     once that tick's re-render has actually completed, and
 *     window.__gallery.setTick('<cellId>', n) for deterministic stepping
 *   - window.__gallery.manifest = the manifest entries
 *
 * Charts render at fixed CSS pixels with devicePixelRatio:1 so screenshots are
 * layout-independent. The ▶ button replays recorded ticks at 1 s per tick
 * through the same builder+setOption path the live app uses; ⏮/⏭ step
 * deterministically (the fixture DATA is fully deterministic — the interval
 * timer is only playback chrome).
 */

import { MANIFEST, FIXTURES } from './fixtures/manifest.mjs';
import { buildAasOption } from '../lib/builders/aas.js';
import {
    buildUplotSpec, overlayGeometry, overlayHooks, compareHooks, drawDiffStrip,
} from '../lib/uplot-aas.js';
import { buildTimelineOption } from '../lib/builders/timeline.js';
import { buildHeatmapOption } from '../lib/builders/histogram.js';
import { buildWaterfallOption } from '../lib/builders/waterfall.js';
import { buildExecScatterOption } from '../lib/builders/exec-scatter.js';
import { buildMatrixOption } from '../lib/builders/matrix.js';
import {
    buildTransitionsOption, buildVariantsHtml,
} from '../lib/builders/transitions.js';
import {
    buildConcurrencyOption, buildConcurrencyTables,
} from '../lib/builders/concurrency.js';
import {
    buildFidelityShading, buildEscalationAnnotation, buildUnavailablePanel,
    buildMetricsPanel, buildEscalateControl, buildCompareFidelity,
} from '../lib/builders/fidelity.js';
import { compareFidelityHtml } from '../lib/panels.js';
import { buildTableModel, mountTable } from '../lib/table.js';
import { buildDeltaComparison } from '../lib/builders/compare.js';
import {
    overviewConfig, eventsConfig, sessionsConfig, queriesConfig,
    compareEventsConfig,
} from '../lib/builders/table-configs.js';
import { esc } from '../lib/format.js';
import { createRenderTracker, createReadyGate } from './render-settle.mjs';
import { quantizeCellHeight } from './cell-footprint.mjs';

const CHART_W = 620;
const CHART_H = 280;

const TABLE_CONFIGS = {
    overview: overviewConfig,
    events: eventsConfig,
    sessions: sessionsConfig,
    queries: queriesConfig,
    'compare-events': compareEventsConfig,
};

/* cellId -> setTick(n) for tick-replay cells (used by ⏮/⏭ and Playwright). */
const tickHooks = {};

/* ── Render-settle tracking (issue #155) ─────────────────────────────────────
 * animation:false only removes SERIES transitions — the zrender/echarts paint
 * itself is still scheduled on requestAnimationFrame (see
 * echarts.min.js's Animation/`refresh()` scheduler), so the synchronous
 * setOption() call returns well before the canvas is actually painted.
 * Screenshotting right after the synchronous render loop therefore races
 * that paint, and the result depends on runner load — the same commit
 * rendered pixels that differed by up to 3.2% run to run. uPlot draws
 * synchronously (no rAF in its bundle) but is wired the same way for one
 * uniform signal and because a future uPlot version is not obligated to
 * stay synchronous.
 *
 * The bookkeeping (createRenderTracker/createReadyGate) is a pure module
 * (./render-settle.mjs, unit-tested in tests/web_unit/render-settle.test.mjs)
 * with no DOM/chart references; this section is only the DOM glue: it flips
 * cell.dataset.settled and document.body.dataset.galleryReady when the pure
 * gate says a render actually began/resolved. Tick-replay re-renders reuse
 * the same per-cell tracker (begin() again before each setOption/remount) so
 * data-settled tracks the LATEST requested tick, independent of the
 * one-time gallery-ready flag (which never un-readies once true). */
const readyGate = createReadyGate();
const settleTrackers = new WeakMap();

/* #122: pin `cell`'s own height to an integer CSS px once its content has
 * settled — the quantization POLICY (round up, why, unit-tested) lives in
 * the pure ./cell-footprint.mjs; this is only the DOM glue, same split as
 * the render-settle tracking above. Clearing the inline height before
 * measuring re-derives the TRUE natural height each time (needed for
 * tick-replay cells, whose content can resize between ticks).
 */
function snapCellFootprint(cell) {
    cell.style.height = '';
    const h = cell.getBoundingClientRect().height;
    cell.style.height = quantizeCellHeight(h) + 'px';
}

/* `el` is any node inside the cell (the chart/plot host div). */
function trackerFor(el) {
    const cell = el.classList.contains('cell') ? el : el.closest('.cell');
    if (!cell) return null;
    let t = settleTrackers.get(cell);
    if (!t) {
        const tracker = createRenderTracker();
        t = {
            begin() {
                cell.dataset.settled = '0';
                if (tracker.begin()) readyGate.addPending();
            },
            settle() {
                if (!tracker.settle()) return; // stray/superseded completion
                snapCellFootprint(cell);
                cell.dataset.settled = '1';
                readyGate.resolvePending();
                if (readyGate.isReady) document.body.dataset.galleryReady = '1';
            },
        };
        settleTrackers.set(cell, t);
    }
    return t;
}

/* Marks a NEW render in flight against `el`'s owning cell, for callers that
 * re-render an already-tracked chart directly (tick replay) without going
 * back through makeChart/mountUplotAas. */
function beginRenderFor(el) {
    const t = trackerFor(el);
    if (t) t.begin();
}

function div(cls, parent) {
    const el = document.createElement('div');
    if (cls) el.className = cls;
    if (parent) parent.appendChild(el);
    return el;
}

function makeChart(host, option, height) {
    host.style.width = CHART_W + 'px';
    host.style.height = height + 'px';
    const chart = echarts.init(host, 'dark', {
        renderer: 'canvas', devicePixelRatio: 1,
        width: CHART_W, height,
    });
    // Register the completion listener BEFORE begin()/setOption so a
    // same-tick synchronous 'finished' (unlikely, but not guaranteed absent)
    // can never fire before we start counting it as pending.
    const tracker = trackerFor(host);
    if (tracker) {
        chart.on('finished', () => tracker.settle());
        tracker.begin();
    }
    try {
        chart.setOption(option, true);
    } catch (e) {
        // A builder/option bug throwing here must red-card ONLY this cell
        // (renderCell's own try/catch does that) — it must NOT leave this
        // cell's render permanently pending, or data-gallery-ready (and
        // every OTHER cell's capture) hangs on the 15s timeout behind one
        // broken cell. settle() is idempotent against a later 'finished'
        // that might still fire for a partially-applied option.
        if (tracker) tracker.settle();
        throw e;
    }
    return chart;
}

function emptyCard(body, text) {
    const el = div('empty-state', body);
    el.textContent = text;
}

function factLine(foot, text) {
    const el = div('facts', foot);
    el.textContent = text;
}

/* ── Fidelity model renderers (models, not charts — small HTML cards) ─────── */

/* Paint markArea-style band specs onto a horizontal track spanning [from,to]
 * so band position/extent is eyeballable, then the model JSON below it. */
function bandTrack(body, win, bands, edgeX) {
    const track = div('band-track', body);
    const span = win.to - win.from;
    for (const b of bands) {
        const from = b.from != null ? b.from : win.from;
        const to = b.to != null ? b.to : win.to;
        const seg = div('band band-' + (b.kind || 'esc'), track);
        seg.style.left = ((from - win.from) / span * 100) + '%';
        seg.style.width = ((to - from) / span * 100) + '%';
    }
    if (edgeX != null) {
        const edge = div('band-edge', track);
        edge.style.left = ((edgeX - win.from) / span * 100) + '%';
    }
}

function modelJson(body, model) {
    const pre = document.createElement('pre');
    pre.textContent = JSON.stringify(model, null, 1);
    body.appendChild(pre);
}

function renderFidelity(body, foot, state) {
    const args = state.args;
    if (state.fn === 'shading') {
        const model = buildFidelityShading(...args);
        bandTrack(body, args[1], model.bands, null);
        modelJson(body, model);
        factLine(foot, 'fidelity=' + model.fidelity +
            ' bands=' + model.bands.length + ' showLegend=' + model.showLegend);
    } else if (state.fn === 'annotation') {
        const model = buildEscalationAnnotation(...args);
        if (!model) { emptyCard(body, 'null (not escalated)'); return; }
        const bands = (model.from != null && model.to != null)
            ? [{ from: model.from, to: model.to, kind: 'esc' }] : [];
        bandTrack(body, args[1], bands, model.to);
        modelJson(body, model);
        factLine(foot, model.label + ' · band=' +
            (model.markArea ? 'observed span' : 'NONE (start unknown)') +
            ' · edge line=' + (model.markLine ? 'yes' : 'no'));
    } else if (state.fn === 'unavailable') {
        const model = buildUnavailablePanel(...args);
        const card = div('panel-card', body);
        const title = div('panel-title', card);
        title.textContent = model.title;
        const msg = div('panel-msg', card);
        msg.textContent = 'server: "' + model.message + '" (fidelity: ' + model.fidelity + ')';
        const hint = div('panel-hint', card);
        hint.textContent = model.hint;
        const btn = document.createElement('button');
        btn.textContent = 'Escalate';
        btn.disabled = !model.canEscalate;
        card.appendChild(btn);
    } else if (state.fn === 'metrics') {
        const model = buildMetricsPanel(...args);
        const card = div('panel-card', body);
        const title = div('panel-title', card);
        title.textContent = 'Daemon metrics · tier: ' + model.tier;
        const table = document.createElement('table');
        table.className = 'metrics';
        for (const r of model.rows) {
            const tr = document.createElement('tr');
            if (r.warn) tr.className = 'warn';
            const td1 = document.createElement('td');
            td1.textContent = r.label;
            const td2 = document.createElement('td');
            td2.textContent = r.value + (r.hint ? '  (' + r.hint + ')' : '');
            tr.append(td1, td2);
            table.appendChild(tr);
        }
        card.appendChild(table);
    } else if (state.fn === 'escalate') {
        const model = buildEscalateControl(...args);
        const card = div('panel-card', body);
        const btn = document.createElement('button');
        btn.textContent = model.buttonLabel;
        btn.disabled = !model.canEscalate;
        card.appendChild(btn);
        const hint = div('panel-hint', card);
        hint.textContent = model.budgetText +
            ' · canEscalate=' + model.canEscalate +
            ' · canDeescalate=' + model.canDeescalate;
    } else if (state.fn === 'compare') {
        const model = buildCompareFidelity(...args);
        const card = div('panel-card', body);
        card.innerHTML = compareFidelityHtml(model);
        factLine(foot, model.warning || model.note || 'matching evidence');
    } else {
        throw new Error('unknown fidelity fn: ' + state.fn);
    }
}

/* ── Chart-builder adapters ──────────────────────────────────────────────── */

function renderAasStatic(body, foot, state) {
    const m = buildAasOption(state.data, state.opts);
    if (!m.hasData) { emptyCard(body, 'No data in selected range'); return; }
    makeChart(div('chart', body), m.option, CHART_H);
    factLine(foot, 'fidelity: ' + m.fidelityLabel +
        ' · series: ' + m.seriesNames.length + ' · maxAas: ' + m.maxAas);
}

function renderAasTicks(body, foot, state, cell) {
    const ticks = state.ticks;
    const host = div('chart', body);
    const first = buildAasOption(ticks[0].data, ticks[0].opts);
    const chart = makeChart(host, first.option, CHART_H);

    const bar = div('tick-bar', body);
    const label = document.createElement('span');
    label.className = 'tick-label';

    let tick = 0;
    let timer = null;
    const setTick = (n) => {
        tick = ((n % ticks.length) + ticks.length) % ticks.length;
        const m = buildAasOption(ticks[tick].data, ticks[tick].opts);
        // The app's exact live-refresh path (views/active.js): full-option
        // setOption with notMerge — identity defects show as tick-N flashes.
        beginRenderFor(host);
        chart.setOption(m.option, true);
        cell.dataset.tick = String(tick);
        label.textContent = 'tick ' + (tick + 1) + '/' + ticks.length;
    };

    const btn = (text, onClick) => {
        const b = document.createElement('button');
        b.textContent = text;
        b.addEventListener('click', onClick);
        bar.appendChild(b);
        return b;
    };
    btn('⏮', () => setTick(tick - 1));
    const play = btn('▶', () => {
        if (timer) {
            clearInterval(timer); timer = null; play.textContent = '▶';
        } else {
            timer = setInterval(() => setTick(tick + 1), 1000);
            play.textContent = '⏸';
        }
    });
    btn('⏭', () => setTick(tick + 1));
    bar.appendChild(label);

    setTick(0);
    tickHooks[cell.id] = setTick;
    factLine(foot, ticks.length + ' recorded ticks · top-' +
        ticks[0].data.series.length + ' ranking changes between ticks');
}

/* ── uPlot AAS adapters (U2b) ────────────────────────────────────────────────
 *
 * The renderer-swap twin of the aas cells: the SAME fixture states through
 * buildUplotSpec + a REAL uPlot mount, exactly the app's mount contract
 * (views/active.js): real pixel dims at construct, overlay hooks merged
 * before construction recomputing honesty geometry against the CURRENT x
 * scale, setScale('x', spec.xWindow) to land on the window. The gallery
 * treats the spec as a black box — it paints what the builder returns. */

function mountUplotAas(host, data, opts) {
    const spec = buildUplotSpec(data, opts);
    if (!spec.hasData) return null;
    host.style.width = CHART_W + 'px';
    host.style.height = CHART_H + 'px';
    const comparing = !!opts.compareData;
    let plotHost = host;
    let diff = null;
    if (comparing) {
        plotHost = div('compare-gallery-plot', host);
        plotHost.style.width = CHART_W + 'px';
        plotHost.style.height = (CHART_H - 54) + 'px';
        diff = document.createElement('canvas');
        diff.style.cssText = 'display:block;width:' + CHART_W + 'px;height:54px';
        host.appendChild(diff);
    }
    const o = spec.uplotOpts;
    o.width = CHART_W;
    o.height = comparing ? CHART_H - 54 : CHART_H;
    const honesty = overlayHooks((u) => overlayGeometry(data, opts,
        { min: u.scales.x.min, max: u.scales.x.max }));
    const ghost = compareHooks(() => spec.compare);
    // uPlot draws synchronously (no rAF in its bundle), so this settle()
    // is redundant with the plain begin()/settle() bracket below in
    // practice — but it registers the REAL completion signal ('draw' fires
    // after every uPlot paint, including the setScale()-triggered redraw
    // below) rather than "the constructor returned", so a future async
    // uPlot cannot silently reintroduce this class of race.
    const tracker = trackerFor(host);
    o.hooks = {
        drawAxes: (honesty.drawAxes || []).concat(ghost.drawAxes || []),
        draw: (honesty.draw || []).concat(tracker ? [() => tracker.settle()] : []),
    };
    if (tracker) tracker.begin();
    const u = new uPlot(o, spec.alignedData, plotHost);
    u.setScale('x', spec.xWindow);
    if (diff) drawDiffStrip(diff, spec.compare, spec.xWindow);
    if (tracker) tracker.settle();
    return { u, spec };
}

function renderUplotAasStatic(body, foot, state) {
    const host = div('chart', body);
    const m = mountUplotAas(host, state.data, state.opts);
    if (!m) {
        host.remove();
        emptyCard(body, 'No data in selected range');
        return;
    }
    factLine(foot, 'renderer: uPlot · fidelity: ' + m.spec.fidelityLabel +
        ' · series: ' + m.spec.seriesNames.length + ' · maxAas: ' + m.spec.maxAas);
}

function renderUplotAasTicks(body, foot, state, cell) {
    const ticks = state.ticks;
    const host = div('chart', body);
    let mounted = mountUplotAas(host, ticks[0].data, ticks[0].opts);

    const bar = div('tick-bar', body);
    const label = document.createElement('span');
    label.className = 'tick-label';

    let tick = 0;
    let timer = null;
    const setTick = (n) => {
        tick = ((n % ticks.length) + ticks.length) % ticks.length;
        // The app's series-set-change path (views/active.js mountUplot): the
        // top-N ranking churns between ticks, so each tick REBUILDS the
        // instance — the rare, non-gesture rebuild, never the setScale loop.
        if (mounted) { mounted.u.destroy(); mounted = null; }
        host.textContent = '';
        mounted = mountUplotAas(host, ticks[tick].data, ticks[tick].opts);
        cell.dataset.tick = String(tick);
        label.textContent = 'tick ' + (tick + 1) + '/' + ticks.length;
    };

    const btn = (text, onClick) => {
        const b = document.createElement('button');
        b.textContent = text;
        b.addEventListener('click', onClick);
        bar.appendChild(b);
        return b;
    };
    btn('⏮', () => setTick(tick - 1));
    const play = btn('▶', () => {
        if (timer) {
            clearInterval(timer); timer = null; play.textContent = '▶';
        } else {
            timer = setInterval(() => setTick(tick + 1), 1000);
            play.textContent = '⏸';
        }
    });
    btn('⏭', () => setTick(tick + 1));
    bar.appendChild(label);

    setTick(0);
    tickHooks[cell.id] = setTick;
    factLine(foot, 'renderer: uPlot · ' + ticks.length + ' recorded ticks · top-' +
        ticks[0].data.series.length + ' ranking changes between ticks');
}

function renderTimeline(body, foot, state) {
    // #106: the builder buckets dense spans per PIXEL COLUMN, so it must be
    // told the width it will actually be drawn at — CHART_W, the cell's own
    // canvas width, not the builder's blind TIMELINE_DEFAULT_WIDTH. Feeding it
    // the default would bucket a 620 px cell into 1080 columns and re-fuse the
    // very block the cell exists to show.
    const m = buildTimelineOption(state.data, { ...state.opts, width: CHART_W });
    if (!m.hasData) { emptyCard(body, 'No timeline events in window'); return; }
    // The banner is the view's, not the option's — but it carries the FEEDBACK
    // half of the density fix, so the cell has to show it or the reviewer is
    // grading half the change.
    if (m.bannerNote) {
        const b = div('', body);
        // Same chrome the view paints it in (views/timeline.js), inline so the
        // gallery stylesheet stays untouched.
        b.style.cssText = 'padding:8px 10px;font-size:12px;color:#ffd700;' +
            'background:#3d3200;border-bottom:1px solid #555;width:' +
            CHART_W + 'px;box-sizing:border-box';
        b.textContent = m.bannerNote;
    }
    // The view sizes its container from chartHeight; the gallery caps the
    // canvas and scrolls the cell so 50-PID cells stay screenshot-sized.
    makeChart(div('chart', body), m.option, Math.min(m.chartHeight, 480));
    factLine(foot, 'bars: ' + m.count + ' of ' + (m.total_count || m.count) +
        ' · truncated: ' + m.truncated + ' · chartHeight: ' + m.chartHeight +
        ' · aggregated: ' + m.aggregated +
        (m.aggregated ? ' (' + m.spansPerPx + '/px, ' + m.columns + ' columns, ' +
            m.segmentCount + ' draw items)' : ''));
}

function renderHistogram(body, foot, state) {
    const m = buildHeatmapOption(state.data);
    if (!m.hasData) { emptyCard(body, 'No data for selected event/range'); return; }
    makeChart(div('chart', body), m.option, 340);
    factLine(foot, 'cells: ' + state.data.cells.length +
        ' · max_count: ' + state.data.max_count);
}

function renderWaterfall(body, foot, state) {
    const m = buildWaterfallOption(state.data, state.opts);
    if (!m.hasData) { emptyCard(body, 'No execution events captured'); return; }
    makeChart(div('chart', body), m.option, Math.min(m.chartHeight, 420));
    factLine(foot, 'lanes: ' + m.lanes.length + ' · events: ' + m.kept_count +
        (m.total_count == null ? '' : ' of ' + m.total_count) +
        ' · plan: ' + (state.data.plan ? 'present' : 'absent') +
        ' · truncated: ' + m.truncated);
}

function renderExecScatter(body, foot, state) {
    const m = buildExecScatterOption(state.data);
    if (!m.hasData) {
        emptyCard(body, 'No completed, positive-duration executions');
        if (m.notes.length) factLine(foot, m.notes.join(' · '));
        return;
    }
    makeChart(div('chart', body), m.option, 340);
    factLine(foot, 'plotted: ' + m.points.length + ' · excluded: ' +
        m.excludedCount + (m.notes.length ? ' · ' + m.notes.join(' · ') : ''));
}

function renderMatrix(body, foot, state) {
    const m = buildMatrixOption(state.data, state.opts);
    if (!m.hasData) { emptyCard(body, 'No transitions found'); return; }
    makeChart(div('chart', body), m.option, 500);
    factLine(foot, 'events: ' + m.visibleCount + ' · cells: ' + m.cells.length +
        (m.notes.length ? ' · ' + m.notes.join(' · ') : ''));
}

function renderTransitions(body, foot, state) {
    if (state.variants) {
        const wrap = div('variants', body);
        wrap.innerHTML = buildVariantsHtml(state.variants, esc);
        return;
    }
    const m = buildTransitionsOption(state.data, state.opts.threshold,
        state.opts.dims);
    if (!m.option) {
        emptyCard(body, 'No transitions above threshold');
        factLine(foot, 'visibleCount: 0');
        return;
    }
    makeChart(div('chart', body), m.option, state.opts.dims.height);
    factLine(foot, 'visibleCount: ' + m.visibleCount +
        ' · threshold: ' + state.opts.threshold + '%');
}

function renderConcurrency(body, foot, state) {
    const m = buildConcurrencyOption(state.data);
    if (!m.hasData) { emptyCard(body, 'No concurrency data'); return; }
    makeChart(div('chart', body), m.option, CHART_H);
    // The tables under the chart are part of the same builder surface.
    div('conc-tables', body).innerHTML = buildConcurrencyTables(m);
    factLine(foot, 'peaks: ' + state.data.peaks.length +
        ' · bursts: ' + m.bursts.length);
}

function renderTable(body, foot, state) {
    const cfg = TABLE_CONFIGS[state.config];
    if (!cfg) throw new Error('unknown table config: ' + state.config);
    const delta = state.compare
        ? buildDeltaComparison(state.compare.kind, state.compare.a, state.compare.b)
        : null;
    const rows = delta ? delta.rows : state.rows;
    const model = buildTableModel(cfg, rows, state.sort || null);
    const host = div('table-host', body);
    mountTable(host, cfg, model, { truncation: delta && delta.truncation });
    factLine(foot, 'config: ' + state.config + ' · rows: ' + model.rows.length +
        (state.sort ? ' · sort: ' + state.sort.key +
            (state.sort.asc ? ' asc' : ' desc') : ''));
}

const RENDERERS = {
    'aas': (body, foot, state, cell) => (state.ticks
        ? renderAasTicks(body, foot, state, cell)
        : renderAasStatic(body, foot, state)),
    'uplot-aas': (body, foot, state, cell) => (state.ticks
        ? renderUplotAasTicks(body, foot, state, cell)
        : renderUplotAasStatic(body, foot, state)),
    'fidelity': renderFidelity,
    'timeline': renderTimeline,
    'histogram': renderHistogram,
    'transitions': renderTransitions,
    'concurrency': renderConcurrency,
    'waterfall': renderWaterfall,
    'exec-scatter': renderExecScatter,
    'matrix': renderMatrix,
    'table-configs': renderTable,
};

/* ── Page assembly ───────────────────────────────────────────────────────── */

function renderCell(grid, entry) {
    const state = FIXTURES[entry.builder][entry.state];
    const cell = document.createElement('section');
    cell.className = 'cell';
    cell.id = entry.cellId;
    cell.dataset.builder = entry.builder;
    cell.dataset.state = entry.state;

    const header = document.createElement('header');
    const idEl = document.createElement('code');
    idEl.textContent = entry.id;
    header.appendChild(idEl);
    for (const tag of entry.tags) {
        const t = document.createElement('span');
        t.className = tag === tag.toUpperCase() ? 'tag tag-word' : 'tag tag-free';
        t.textContent = tag;
        header.appendChild(t);
    }
    cell.appendChild(header);

    const body = div('cell-body', cell);
    const foot = document.createElement('footer');
    const desc = div('desc', foot);
    desc.textContent = state.description;
    cell.appendChild(foot);
    grid.appendChild(cell);

    try {
        RENDERERS[entry.builder](body, foot, state, cell);
    } catch (e) {
        // A fixture/builder crash must be a LOUD red card, not a blank cell.
        console.error('[gallery] render failed: ' + entry.id, e);
        const err = div('render-error', body);
        err.textContent = 'RENDER FAILED: ' + e.message;
        cell.classList.add('failed');
    }
    // #122: chart cells get re-pinned on their async 'finished'/draw settle
    // (see trackerFor), but a plain HTML cell (table/panel, no chart) never
    // fires one — its content is already final here, synchronously.
    snapCellFootprint(cell);
}

/* #122 follow-up: ?isolate=<cellId> (see main()) renders ONLY that one
 * cell — used exclusively by the snapshot suite, never by a human browsing
 * the gallery. Pinning every cell's own height (snapCellFootprint) closed
 * the DOCUMENT-position coupling, but text/canvas content painted deep into
 * a long page can still rasterize a handful of pixels differently purely
 * from being at a DIFFERENT absolute (if still whole-pixel) page Y — a
 * browser tile/text-hinting effect, not a CSS bug, and the same class of
 * coupling #122 is about: a cell's rendered bytes must depend on nothing
 * but its own content. Isolating the capture page means every cell is
 * always painted at the SAME small Y (the top of an otherwise empty grid)
 * regardless of what else the manifest contains, closing that loophole
 * completely instead of chasing each new symptom of it. Pure (no DOM), so
 * it's unit-tested directly (tests/web_unit/gallery-isolate.test.mjs)
 * without needing a browser. Returns an EMPTY array for an isolateId that
 * matches nothing (a typo'd cellId), never silently falling back to
 * "show everything" — a bad ?isolate= value should render a visibly empty
 * page, not defeat the isolation it asked for. */
export function selectDisplayEntries(orderedEntries, isolateId) {
    return isolateId
        ? orderedEntries.filter(e => e.cellId === isolateId)
        : orderedEntries;
}

export function main() {
    const grid = document.getElementById('grid');
    // Keep every established cell at its historical document coordinate so
    // adding a fixture cannot churn unrelated pixel baselines. Compare cells
    // remain in the manifest/TOC under their owning builders, but render after
    // the existing gallery corpus.
    const orderedEntries = MANIFEST.filter(e => !e.tags.includes('compare'))
        .concat(MANIFEST.filter(e => e.tags.includes('compare')));

    const isolateId = new URLSearchParams(location.search).get('isolate');
    const displayEntries = selectDisplayEntries(orderedEntries, isolateId);

    // Sidebar index: one link per cell, grouped by builder.
    const toc = document.getElementById('toc');
    let lastBuilder = null;
    for (const entry of MANIFEST) {
        if (entry.builder !== lastBuilder) {
            const h = document.createElement('div');
            h.className = 'toc-builder';
            h.textContent = entry.builder;
            toc.appendChild(h);
            lastBuilder = entry.builder;
        }
        const a = document.createElement('a');
        a.href = '#' + entry.cellId;
        a.textContent = entry.state + (entry.ticks ? ' ▶' : '');
        toc.appendChild(a);
    }

    for (const entry of displayEntries) renderCell(grid, entry);

    const counts = document.getElementById('counts');
    counts.textContent = MANIFEST.length + ' cells · ' +
        new Set(MANIFEST.map(e => e.builder)).size + ' builders';

    window.__gallery = {
        manifest: MANIFEST,
        setTick(cellId, n) {
            if (!tickHooks[cellId]) throw new Error('not a tick cell: ' + cellId);
            tickHooks[cellId](n);
        },
    };
    // Every renderCell() call above has returned (the DOM/setOption calls
    // are synchronous), but the charts' own paint is not — see the
    // render-settle tracking block above. Flip data-gallery-ready only once
    // every one of them has actually fired its completion event; if none are
    // pending (e.g. an all-panel/table page with no charts) this fires
    // immediately.
    readyGate.markLoopDone();
    if (readyGate.isReady) document.body.dataset.galleryReady = '1';
}
