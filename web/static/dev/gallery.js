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
 *   - <body data-gallery-ready="1"> once every cell has rendered
 *   - tick-replay cells: data-tick="<n>" on the cell, and
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
import { buildTimelineOption } from '../lib/builders/timeline.js';
import { buildHeatmapOption } from '../lib/builders/histogram.js';
import {
    buildTransitionsOption, buildVariantsHtml,
} from '../lib/builders/transitions.js';
import {
    buildConcurrencyOption, buildConcurrencyTables,
} from '../lib/builders/concurrency.js';
import {
    buildFidelityShading, buildEscalationAnnotation, buildUnavailablePanel,
    buildMetricsPanel, buildEscalateControl,
} from '../lib/builders/fidelity.js';
import { buildTableModel, mountTable } from '../lib/table.js';
import {
    overviewConfig, eventsConfig, sessionsConfig, queriesConfig,
} from '../lib/builders/table-configs.js';
import { esc } from '../lib/format.js';

const CHART_W = 620;
const CHART_H = 280;

const TABLE_CONFIGS = {
    overview: overviewConfig,
    events: eventsConfig,
    sessions: sessionsConfig,
    queries: queriesConfig,
};

/* cellId -> setTick(n) for tick-replay cells (used by ⏮/⏭ and Playwright). */
const tickHooks = {};

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
    chart.setOption(option, true);
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

function renderTimeline(body, foot, state) {
    const m = buildTimelineOption(state.data, state.opts);
    if (!m.hasData) { emptyCard(body, 'No timeline events in window'); return; }
    // The view sizes its container from chartHeight; the gallery caps the
    // canvas and scrolls the cell so 50-PID cells stay screenshot-sized.
    makeChart(div('chart', body), m.option, Math.min(m.chartHeight, 480));
    factLine(foot, 'bars: ' + m.count + ' of ' + (m.total_count || m.count) +
        ' · truncated: ' + m.truncated + ' · chartHeight: ' + m.chartHeight);
}

function renderHistogram(body, foot, state) {
    const m = buildHeatmapOption(state.data);
    if (!m.hasData) { emptyCard(body, 'No data for selected event/range'); return; }
    makeChart(div('chart', body), m.option, 340);
    factLine(foot, 'cells: ' + state.data.cells.length +
        ' · max_count: ' + state.data.max_count);
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
    const model = buildTableModel(cfg, state.rows, state.sort || null);
    const host = div('table-host', body);
    mountTable(host, cfg, model, {});
    factLine(foot, 'config: ' + state.config + ' · rows: ' + model.rows.length +
        (state.sort ? ' · sort: ' + state.sort.key +
            (state.sort.asc ? ' asc' : ' desc') : ''));
}

const RENDERERS = {
    'aas': (body, foot, state, cell) => (state.ticks
        ? renderAasTicks(body, foot, state, cell)
        : renderAasStatic(body, foot, state)),
    'fidelity': renderFidelity,
    'timeline': renderTimeline,
    'histogram': renderHistogram,
    'transitions': renderTransitions,
    'concurrency': renderConcurrency,
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
}

export function main() {
    const grid = document.getElementById('grid');

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

    for (const entry of MANIFEST) renderCell(grid, entry);

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
    document.body.dataset.galleryReady = '1';
}
