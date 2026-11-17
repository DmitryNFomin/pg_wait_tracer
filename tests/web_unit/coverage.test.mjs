/* #947 / #871 U3: every web view renders the server's coverage verdict
 * through the one reader (lib/builders/fidelity.js coverageOf) and none
 * computes its own. Fixtures: web/static/dev/fixtures/coverage.mjs. */
import { test } from 'node:test';
import assert from 'node:assert/strict';
import { readdirSync } from 'node:fs';
import {
    coverageOf, coverageWithholds, coverageReasonText, aasScaleMax,
    COVERAGE_UNVERIFIED_LABEL, IN_FLIGHT_EXCLUDED_LABEL, IDLE_UNVERIFIED_LABEL,
    buildFidelityShading, INCOMPLETE_FIDELITY, LIVE_EDGE_RANGE_PREFIX, UP_TO_PREFIX,
} from '../../web/static/lib/builders/fidelity.js';
import {
    buildPaneFidelity, paneFidelityBadgeHtml, buildCoverageBadge,
    buildCoverageNotice, buildCompareCoverageNotice, coverageNoticeHtml, coverageEmptyStateHtml,
} from '../../web/static/lib/panels.js';
import {
    buildAasOption, AAS_ANNOTATION_SERIES, aasPeakText, aasEmptyText, aasEmptyStatusText,
} from '../../web/static/lib/builders/aas.js';
import { buildUplotSpec, overlayGeometry } from '../../web/static/lib/uplot-aas.js';
import {
    buildHeatmapOption, heatmapScaleMax, HEATMAP_COVERAGE_SERIES,
} from '../../web/static/lib/builders/histogram.js';
import { buildVariantsPanel } from '../../web/static/lib/builders/transitions.js';
import { buildDeltaComparison } from '../../web/static/lib/builders/compare.js';
import { pilotEvidenceHtml } from '../../web/static/lib/builders/pilot-evidence.js';
import { esc } from '../../web/static/lib/format.js';
import { buildEventsModel, createEventsView } from '../../web/static/views/events.js';
import { buildQueriesModel, buildQueriesEmptyStateHtml } from '../../web/static/views/queries.js';
import { buildSessionsModel, createSessionsView } from '../../web/static/views/sessions.js';
import { createOverviewView, buildSummary } from '../../web/static/views/overview.js';
import { createTransitionsView } from '../../web/static/views/transitions.js';
import { createMatrixView } from '../../web/static/views/matrix.js';
import { createConcurrencyView } from '../../web/static/views/concurrency.js';
import { createExecScatterView } from '../../web/static/views/exec-scatter.js';
import { createTimelineView } from '../../web/static/views/timeline.js';
import { createQueriesView } from '../../web/static/views/queries.js';
import { createHistogramView, selectorCoverageHtml } from '../../web/static/views/histogram.js';
import { createWaterfallView } from '../../web/static/views/waterfall.js';
import { aasFidelityChipHtml } from '../../web/static/views/active.js';
import { states as transitionsStates } from '../../web/static/dev/fixtures/transitions.mjs';
import { states as matrixStates } from '../../web/static/dev/fixtures/matrix.mjs';
import { states as concurrencyStates } from '../../web/static/dev/fixtures/concurrency.mjs';
import { states as scatterStates } from '../../web/static/dev/fixtures/exec-scatter.mjs';
import { states as waterfallStates } from '../../web/static/dev/fixtures/waterfall.mjs';
import { states as timelineStates } from '../../web/static/dev/fixtures/timeline.mjs';
import { MIN_NS as MIN_NS_T } from '../../web/static/dev/fixtures/base.mjs';
import {
    COVERAGE, INCOMPLETE_STATES, WITHHELD_COMMANDS, COV_FROM, COV_TO, COV_GAP,
    withheld, aasWindow, heatmapWindow, timeModelWindow, topEventsWindow,
    sessionTimelineWindow, execScatterWindow,
} from '../../web/static/dev/fixtures/coverage.mjs';

const WIN = { from: COV_FROM, to: COV_TO };
/* Real flow-variant cards (numbers included), from the gallery fixture. */
const VARIANTS_PAYLOAD = transitionsStates['variants-html'].variants;
const ctx = () => ({
    server: { numCpus: 4, fromNs: COV_FROM }, timeRange: { ...WIN, span: () => COV_TO - COV_FROM },
    filters: { snapshot: () => ({}), filters: {} }, getSort: () => null,
    // A table that was mounted anyway paints its model, numbers included.
    mountTable: (el, config, table) => { el.innerHTML += JSON.stringify(table); },
    tooltipEl: null,
});
function fakeEl() {
    const el = { innerHTML: '' };
    el.insertAdjacentHTML = (_where, html) => { el.innerHTML += html; };
    el.querySelector = () => el;   // nested hosts paint into the same text
    return el;
}

/* ── the reader ─────────────────────────────────────────────────────────── */

test('U3 reader: the spec label and its known causes', () => {
    const expect = {
        restart_postgres: ' (PostgreSQL restart)', restart_daemon: ' (daemon restart)',
        restart_unknown: ' (restart)', capture_loss: ' (capture loss)',
        unclean_end: ' (daemon stopped uncleanly)', missing_file: ' (trace file missing)',
        retention: ' (older than the retained data)',
        not_recorded: ' (coverage not recorded — capture predates coverage tracking)',
        clock_step: ' (system clock stepped back)',
        not_yet_covered: ' (not yet confirmed — live)', not_captured: ' (not captured)',
        restart_no_gap: ' (daemon restart)',
        capture_loss_and_restart: ' (capture loss) (PostgreSQL restart)',
    };
    assert.deepEqual(Object.keys(expect).sort(), INCOMPLETE_STATES.slice().sort());
    for (const [state, suffix] of Object.entries(expect)) {
        const cov = coverageOf({ coverage: COVERAGE[state] });
        assert.equal(cov.verdict, 'incomplete');
        assert.equal(cov.withheld, true, state);
        assert.equal(cov.label, COVERAGE_UNVERIFIED_LABEL + suffix, 'COVERAGE_LABEL_TEXT ' + state);
        assert.equal(cov.excludesInFlight, false, 'A1_NO_XF_ON_INCOMPLETE ' + state);
        assert.equal(cov.idleUnverified, false, state);
    }
    assert.equal(COVERAGE_UNVERIFIED_LABEL, 'Coverage unverified — whole-window metrics unavailable');
    assert.equal(coverageReasonText('restart', null), '(restart)');
});

test('U3 reader: complete and live_edge withhold nothing; xf only where the server set it', () => {
    for (const [state, xf] of [['complete', false], ['complete_xf', true], ['live_edge_xf', true]]) {
        const cov = coverageOf({ fidelity: 'exact', coverage: COVERAGE[state] });
        assert.equal(cov.withheld, false, state);
        assert.equal(cov.label, null);
        assert.equal(cov.excludesInFlight, xf, 'XF_MARK ' + state);
        assert.deepEqual(cov.uncovered, []);
    }
    assert.equal(coverageOf({ coverage: COVERAGE.live_edge_xf }).coveredTo, COV_TO - 2e9);
    // A1 item 1: a stray xf flag on an incomplete verdict carries no mark.
    const stray = { ...COVERAGE.capture_loss, excludes_in_flight: true };
    assert.equal(coverageOf({ coverage: stray }).excludesInFlight, false, 'A1_NO_XF_ON_INCOMPLETE stray');
});

test('U3 reader: fails closed; absent coverage keeps the legacy presentation', () => {
    assert.equal(coverageOf({ fidelity: 'exact', rows: [] }), null, 'ABSENT_IS_LEGACY');
    assert.equal(coverageOf(null), null);
    for (const coverage of [null, 7, 'complete', {}, { verdict: 'future' }]) {
        assert.equal(coverageOf({ coverage }).verdict, 'incomplete', 'MALFORMED_IS_INCOMPLETE');
    }
    // The withheld code alone is incomplete even when the object is lost.
    assert.equal(coverageWithholds({ code: 'coverage_unverified' }), true);
    // A withheld code beats a contradictory verdict.
    assert.equal(coverageWithholds({ code: 'coverage_unverified', coverage: COVERAGE.complete }), true);
    // Idle: an idle object with an unknown verdict is unverified.
    assert.equal(coverageOf({ coverage: { ...COVERAGE.complete, idle: {} } }).idleUnverified, true);
    assert.equal(coverageOf({ coverage: COVERAGE.idle_unverified }).idleUnverified, true);
    assert.equal(coverageOf({ coverage: COVERAGE.complete }).idleUnverified, false);
});

test('U3 reader: uncovered ranges carry the reason text the spec names, and only that', () => {
    const cov = coverageOf({ coverage: COVERAGE.capture_loss_and_restart });
    assert.deepEqual(cov.uncovered.map(u => u.text), ['(capture loss)', '(PostgreSQL restart)']);
    const mf = coverageOf({ coverage: COVERAGE.missing_file });
    assert.deepEqual(mf.uncovered.map(u => u.text), ['(trace file missing)']);
    // A reason the spec does not name gets no text, never its token.
    const future = coverageOf({ coverage: { ...COVERAGE.missing_file,
        reasons: ['future_reason'], uncovered: [{ ...COVERAGE.missing_file.uncovered[0], reason: 'future_reason' }] } });
    assert.deepEqual(future.uncovered.map(u => u.text), [null], 'NO_INVENTED_REASON_TEXT');
    assert.equal(future.label, COVERAGE_UNVERIFIED_LABEL);
    const html = coverageNoticeHtml(buildCoverageNotice(withheld('missing_file')));
    assert.ok(!html.includes('missing_file'), 'RAW_REASON_TOKEN_NEVER_SHOWN');
});

/* ── the guard: no numeric whole-window metric while incomplete ──────────── */

/* Text a reader sees, with the uncovered-range list (wall times) removed. */
function visibleNumbers(html) {
    const text = String(html)
        .replace(/<ul class="coverage-ranges"[\s\S]*?<\/ul>/g, '')
        .replace(/<[^>]*>/g, ' ');
    return text.match(/\d/g) || [];
}

/* A server that forgot to withhold: incomplete verdict, numbers still sent. */
function leaky(state, payload) {
    return { ...payload, fidelity: 'incomplete', coverage: COVERAGE[state] };
}

function renderedWholeWindowViews(state) {
    const c = ctx();
    const tm = leaky(state, timeModelWindow('complete'));
    const ev = leaky(state, topEventsWindow('complete'));
    const sessions = leaky(state, { rows: [{ pid: 1001, db_time_ms: 5000, cpu_pct: 50 }] });
    const queries = leaky(state, { rows: [{ query_id: '1', total_ms: 5000, calls: 3 }] });
    const transitions = leaky(state, { links: [{ source: 'a', target: 'b', value: 10 }], total: 10 });
    const concurrency = leaky(state, { buckets: [{ t: COV_FROM, active: 3 }], max_active: 3 });
    const out = {};

    const ov = createOverviewView();
    const ovModel = ov.build(tm, c);
    const ovEl = fakeEl(); const ovSummary = { innerHTML: '' };
    ov.mount(ovEl, ovModel, { ...c, summaryEl: ovSummary });
    out.overview = ovSummary.innerHTML + ovEl.innerHTML;
    const ovCmp = ov.build({ compare: true, a: tm, b: timeModelWindow('complete') }, c);
    const ovCmpEl = fakeEl(); const ovCmpSummary = { innerHTML: '' };
    ov.mount(ovCmpEl, ovCmp, { ...c, summaryEl: ovCmpSummary });
    out['overview compare'] = ovCmpSummary.innerHTML + ovCmpEl.innerHTML;
    const ovCmpB = ov.build({ compare: true, a: timeModelWindow('complete'), b: tm }, c);
    const ovCmpBEl = fakeEl(); const ovCmpBSummary = { innerHTML: '' };
    ov.mount(ovCmpBEl, ovCmpB, { ...c, summaryEl: ovCmpBSummary });
    out['overview compare (B incomplete)'] = ovCmpBSummary.innerHTML + ovCmpBEl.innerHTML;

    const evView = createEventsView();
    const evEl = fakeEl();
    evView.mount(evEl, evView.build(ev, c), { ...c, summaryEl: { innerHTML: '' } });
    out.events = evEl.innerHTML;
    const evCmpEl = fakeEl();
    evView.mount(evCmpEl, buildEventsModel({ compare: true, a: topEventsWindow('complete'), b: ev }),
        { ...c, summaryEl: { innerHTML: '' } });
    out['events compare (B incomplete)'] = evCmpEl.innerHTML;

    const sView = createSessionsView();
    const sEl = fakeEl();
    sView.mount(sEl, sView.build(sessions, c), { ...c, summaryEl: { innerHTML: '' } });
    out.sessions = sEl.innerHTML;

    out.queries = buildQueriesEmptyStateHtml(buildQueriesModel(queries));
    out['queries compare'] = buildQueriesEmptyStateHtml(
        buildQueriesModel({ compare: true, a: queries, b: queries }));

    for (const [name, make, data] of [
        ['transitions', createTransitionsView, { transitions: transitions }],
        ['matrix', createMatrixView, transitions],
        ['concurrency', createConcurrencyView, concurrency],
    ]) {
        const v = make();
        const model = v.build(data, c);
        const el = fakeEl();
        v.mount(el, model, { ...c, summaryEl: { innerHTML: '' } });
        out[name] = el.innerHTML;
    }
    out.variants = buildVariantsPanel('ready', leaky(state, VARIANTS_PAYLOAD), esc);
    out.pilot = pilotEvidenceHtml(leaky(state, {
        wait_time: { value: '123', unit: 'ns', completeness: 'complete', reasons: [], diagnostic: {} },
        completed_waits: { value: '4', unit: 'waits', completeness: 'complete', reasons: [], diagnostic: {} },
        storage: {} }));
    const aasEc = buildAasOption(leaky(state, { ...aasWindow('complete') }), { win: WIN, numCpus: 4 });
    const aasUp = buildUplotSpec(leaky(state, { ...aasWindow('complete') }), { win: WIN, numCpus: 4 });
    out['aas status (ECharts)'] = aasPeakText(aasEc);
    out['aas status (uPlot)'] = aasPeakText(aasUp);
    return out;
}

test('U3 GUARD: no view shows a numeric whole-window metric while coverage.verdict is incomplete', () => {
    for (const state of INCOMPLETE_STATES) {
        const rendered = renderedWholeWindowViews(state);
        assert.equal(Object.keys(rendered).length, 15);
        for (const [view, html] of Object.entries(rendered)) {
            assert.ok(String(html).includes(COVERAGE_UNVERIFIED_LABEL),
                `WITHHELD_VIEW_SHOWS_LABEL ${view} ${state}`);
            assert.deepEqual(visibleNumbers(html), [],
                `NO_NUMBER_WHILE_INCOMPLETE ${view} ${state}: ${html}`);
        }
    }
});

test('U3 GUARD negative control: the same views do show numbers when complete', () => {
    const c = ctx();
    const ov = createOverviewView();
    const summary = buildSummary(timeModelWindow('complete'), 4, WIN);
    assert.ok(summary.some(m => /\d/.test(m.value)), 'control: summary has numbers');
    const ovModel = ov.build(timeModelWindow('complete'), c);
    assert.equal(ovModel.coverageNotice, undefined);
    assert.ok(ovModel.hasRows);
    assert.match(aasPeakText(buildAasOption(aasWindow('complete'), { win: WIN })), /^peak \d/);
    assert.equal(buildEventsModel(topEventsWindow('complete')).coverageNotice, undefined);
});

test('U3: every withheld command renders the notice from the server shape itself', () => {
    for (const cmd of WITHHELD_COMMANDS) {
        const data = withheld('capture_loss');
        const notice = buildCoverageNotice(data);
        assert.ok(notice, cmd);
        const html = coverageNoticeHtml(notice);
        assert.match(html, /Coverage unverified — whole-window metrics unavailable \(capture loss\)/);
        assert.match(html, /coverage-ranges/, 'UNCOVERED_RANGE_LISTED');
    }
    assert.equal(buildCompareCoverageNotice(topEventsWindow('complete'), withheld('restart_daemon')).label,
        COVERAGE_UNVERIFIED_LABEL + ' (daemon restart)', 'COMPARE_EITHER_SIDE_WITHHOLDS');
    assert.equal(buildCompareCoverageNotice(topEventsWindow('complete'), topEventsWindow('complete')), null);
});

/* ── time-bucketed views ─────────────────────────────────────────────────── */

test('U3 AAS: uncovered buckets missing (not zero), shaded at the uncovered range, label in the model', () => {
    const data = aasWindow('capture_loss');
    const ec = buildAasOption(data, { win: WIN, numCpus: 4 });
    const cpu = ec.option.series.find(s => s.name !== AAS_ANNOTATION_SERIES).data.map(p => p[1]);
    assert.deepEqual(cpu.slice(29, 36), [1.25, null, null, null, null, null, 1.25], 'AAS_GAP_NOT_ZERO');
    const band = ec.option.series.find(s => s.name === AAS_ANNOTATION_SERIES).markArea.data;
    assert.equal(band.length, 1, 'ONE_BAND_AT_THE_UNCOVERED_RANGE');
    assert.deepEqual([band[0][0].xAxis, band[0][1].xAxis], [COV_GAP.from / 1e6, COV_GAP.to / 1e6]);
    assert.equal(ec.coverage.label, COVERAGE_UNVERIFIED_LABEL + ' (capture loss)');
    const up = buildUplotSpec(data, { win: WIN, numCpus: 4 });
    assert.deepEqual(up.rawValues[0].slice(29, 36), [1.25, null, null, null, null, null, 1.25]);
    assert.equal(up.coverage.label, ec.coverage.label, 'BOTH_RENDERERS_ONE_VERDICT');
    // The y-scale follows the buckets sent while max_aas is withheld.
    assert.equal(data.max_aas, null);
    const sums = data.buckets.filter(b => b.cpu != null)
        .map(b => b.cpu + b.io + b.lock + b.lwlock);
    assert.equal(aasScaleMax(data), Math.max(...sums));
    assert.equal(ec.maxAas, up.maxAas);
});

test('U3 AAS live_edge: buckets past covered_to are missing; the in-flight mark rides along', () => {
    const data = aasWindow('live_edge_xf');
    const ec = buildAasOption(data, { win: WIN });
    const cpu = ec.option.series.find(s => s.name !== AAS_ANNOTATION_SERIES).data.map(p => p[1]);
    assert.equal(cpu[59], null);
    assert.equal(cpu[58], 1.25);
    assert.equal(ec.coverage.excludesInFlight, true);
    assert.match(aasPeakText(ec), /^peak /, 'live_edge keeps its peak');
});

test('U3 heatmap: uncovered buckets shaded, cells absent, scale from the cells sent', () => {
    const data = heatmapWindow('capture_loss');
    const m = buildHeatmapOption(data);
    const xs = new Set(m.option.series[0].data.map(c => c[0]));
    for (let i = 30; i < 35; i++) assert.ok(!xs.has(i), 'HEATMAP_GAP_HAS_NO_CELLS');
    const cov = m.option.series.find(s => s.name === HEATMAP_COVERAGE_SERIES);
    assert.ok(cov, 'HEATMAP_GAP_SHADED');
    assert.deepEqual(cov.markArea.data.map(p => [p[0].xAxis, p[1].xAxis]), [[30, 34]]);
    assert.equal(cov.markArea.label.formatter, 'Incomplete capture');
    assert.equal(heatmapScaleMax(data), Math.max(...data.cells.map(c => c[2])));
    // A leaky server: cells inside the uncovered range are still not drawn.
    const leakyData = { ...data, cells: data.cells.concat([[31, 4, 9999]]) };
    assert.ok(!buildHeatmapOption(leakyData).option.series[0].data.some(c => c[0] === 31),
        'HEATMAP_GAP_HAS_NO_CELLS (leaky server)');
    assert.equal(heatmapScaleMax(leakyData), heatmapScaleMax(data));
    // Complete window: nothing shaded, the server's max used.
    const full = buildHeatmapOption(heatmapWindow('complete'));
    assert.ok(!full.option.series.some(s => s.name === HEATMAP_COVERAGE_SERIES));
    assert.equal(full.rawMax, 499);
});

test('U3 shading: uncovered ranges become labelled incomplete bands', () => {
    const s = buildFidelityShading({ fidelity: 'incomplete', coverage: COVERAGE.unclean_end }, WIN);
    assert.deepEqual(s.bands, [{ from: COV_TO - 600e9, to: COV_TO, kind: INCOMPLETE_FIDELITY }]);
    // Row 3: no uncovered point; the whole window is shaded incomplete.
    const whole = buildFidelityShading({ fidelity: 'incomplete', coverage: COVERAGE.restart_no_gap }, WIN);
    assert.deepEqual(whole.bands, [{ from: WIN.from, to: WIN.to, kind: INCOMPLETE_FIDELITY }]);
    assert.equal(buildFidelityShading({ fidelity: 'exact', coverage: COVERAGE.complete }, WIN).markArea, null);
});

/* ── record-level views: label only ──────────────────────────────────────── */

test('U3 record-level views keep their records and carry the label', () => {
    const c = ctx();
    const tl = createTimelineView().build(sessionTimelineWindow('capture_loss'), c);
    assert.equal(tl.hasData, true, 'TIMELINE_RECORDS_KEPT');
    assert.equal(tl.coverageBadge.label, COVERAGE_UNVERIFIED_LABEL + ' (capture loss)');
    const sc = createExecScatterView().build(execScatterWindow('restart_postgres'));
    assert.equal(sc.hasData, true, 'SCATTER_RECORDS_KEPT');
    assert.match(paneFidelityBadgeHtml(sc.coverageBadge), /PostgreSQL restart/);
    const scXf = createExecScatterView().build(execScatterWindow('live_edge_xf'));
    assert.deepEqual(scXf.coverageBadge.marks, ['up to 09:46:38', IN_FLIGHT_EXCLUDED_LABEL]);
    assert.equal(createTimelineView().build(sessionTimelineWindow('complete'), c).coverageBadge, null);
    // Executions / execution_detail use the same badge.
    assert.equal(buildCoverageBadge({ rows: [], coverage: COVERAGE.unclean_end }).label,
        COVERAGE_UNVERIFIED_LABEL + ' (daemon stopped uncleanly)');
});

/* ── xf mark and Idle unverified ─────────────────────────────────────────── */

test('U3 in-flight mark: complete and live_edge panes carry it; legacy panes stay quiet', () => {
    for (const [state, marks] of [['complete_xf', [IN_FLIGHT_EXCLUDED_LABEL]],
        ['live_edge_xf', ['up to 09:46:38', IN_FLIGHT_EXCLUDED_LABEL]]]) {
        const badge = buildPaneFidelity({ fidelity: 'exact', coverage: COVERAGE[state] });
        assert.deepEqual(badge.marks, marks, state);
        assert.match(paneFidelityBadgeHtml(badge), /in-flight time excluded/);
    }
    assert.equal(buildPaneFidelity({ fidelity: 'exact', coverage: COVERAGE.complete }), null);
    const sampledXf = buildPaneFidelity({ fidelity: 'sampled', coverage: COVERAGE.complete_xf });
    assert.equal(sampledXf.label, 'Sampled (estimated)');
    assert.deepEqual(sampledXf.marks, [IN_FLIGHT_EXCLUDED_LABEL]);
});

test('U3 Idle unverified: overview summary and rows, events rows and badge, compare skips', () => {
    const c = ctx();
    const ov = createOverviewView().build(timeModelWindow('idle_unverified'), c);
    assert.equal(ov.summary.find(m => m.label === 'Idle').value, IDLE_UNVERIFIED_LABEL);
    const rowsOf = (model) => model.table.rows.map(r => r.row);
    const idleRows = rowsOf(ov).filter(r => r.idleUnverified);
    assert.equal(idleRows.length, 3, 'IDLE_PARENT_AND_CHILDREN');
    assert.ok(idleRows.every(r => r.ms == null && r.aas == null), 'IDLE_ROWS_NULL');
    assert.ok(rowsOf(ov).filter(r => !r.idleUnverified).every(r => r.ms != null));
    const idleCell = ov.table.rows.find(r => r.row.name === 'Idle').cells[1];
    assert.match(JSON.stringify(idleCell), /Idle time unverified/, 'IDLE_ROW_SHOWS_LABEL');
    // A leaky server: Idle numbers sent anyway are still not shown.
    const leakyTm = { ...timeModelWindow('complete'), coverage: COVERAGE.idle_unverified };
    const leakyOv = createOverviewView().build(leakyTm, c);
    assert.equal(leakyOv.summary.find(m => m.label === 'Idle').value, IDLE_UNVERIFIED_LABEL);
    assert.ok(rowsOf(leakyOv).filter(r => r.idleUnverified).every(r => r.ms == null),
        'IDLE_ROWS_MASKED_EVEN_IF_SENT');
    const ev = buildEventsModel({ ...topEventsWindow('complete'), coverage: COVERAGE.idle_unverified });
    const evIdle = rowsOf(ev).find(r => r.name === 'Client:ClientRead');
    assert.equal(evIdle.idleUnverified, true);
    assert.equal(evIdle.total_ms, null);
    assert.ok(ev.paneFidelity.marks.includes(IDLE_UNVERIFIED_LABEL));
    const io = rowsOf(ev).find(r => r.name === 'IO:DataFileRead');
    assert.equal(io.total_ms, 5500, 'DB_TIME_ROWS_UNTOUCHED');
    // Compare: an Idle row without a value on one side has no delta.
    const delta = buildDeltaComparison('events',
        { rows: rowsOf(ev) }, topEventsWindow('complete'));
    assert.ok(!delta.rows.some(r => r.name === 'Client:ClientRead'), 'NO_DELTA_AGAINST_UNVERIFIED_IDLE');
    // Not an idle verdict: rows untouched.
    const plain = createOverviewView().build(timeModelWindow('complete'), c);
    assert.ok(!rowsOf(plain).some(r => r.idleUnverified));
});

/* ── Amendment A3: label text for every reason and for the live edge ─────── */

/* Every reason the verdict can emit (spec §2 States, A2 item 1) and the text
 * A3 / §2 Output gives it. restart takes its cause (A1 item 2). */
const A3_REASON_TEXT = [
    ['capture_loss', null, '(capture loss)'],
    ['missing_file', null, '(trace file missing)'],
    ['unclean_end', null, '(daemon stopped uncleanly)'],
    ['retention', null, '(older than the retained data)'],
    ['restart', 'postgres', '(PostgreSQL restart)'],
    ['restart', 'daemon', '(daemon restart)'],
    ['restart', 'unknown', '(restart)'],
    ['clock_step', null, '(system clock stepped back)'],
    ['not_yet_covered', null, '(not yet confirmed — live)'],
    ['not_recorded', null, '(coverage not recorded — capture predates coverage tracking)'],
    ['not_captured', null, '(not captured)'],
];
const RAW_TOKENS = ['capture_loss', 'missing_file', 'unclean_end', 'retention',
    'clock_step', 'not_yet_covered', 'not_recorded', 'not_captured', 'live_edge',
    'coverage_unverified', 'excludes_in_flight', 'covered_to', 'restart_cause',
    'postgres', 'future_reason'];

function oneReason(reason, cause) {
    const u = { from: COV_GAP.from, to: COV_GAP.to, reason };
    return { verdict: 'incomplete', reasons: [reason], covered_to: null,
        uncovered: [u], restart_cause: cause, excludes_in_flight: false,
        idle: { verdict: 'incomplete', uncovered: [{ ...u }] } };
}

for (const [reason, cause, text] of A3_REASON_TEXT) {
    test(`A3 reason text: ${reason}${cause ? ' / ' + cause : ''} → ${text}`, () => {
        const coverage = oneReason(reason, cause);
        const cov = coverageOf({ code: 'coverage_unverified', coverage });
        assert.equal(cov.label, COVERAGE_UNVERIFIED_LABEL + ' ' + text, 'A3_NOTICE_TEXT');
        assert.deepEqual(cov.uncovered.map(u => u.text), [text], 'A3_RANGE_TEXT');
        const html = coverageNoticeHtml(buildCoverageNotice({ code: 'coverage_unverified', coverage }));
        assert.ok(html.includes(esc(COVERAGE_UNVERIFIED_LABEL + ' ' + text)), 'A3_NOTICE_RENDERED');
        assert.match(html, new RegExp('<li>[^<]*' + esc(text).replace(/[()]/g, '\\$&') + '</li>'),
            'A3_RANGE_RENDERED');
        // Record-level badge and the AAS chip use the same range text.
        assert.ok(paneFidelityBadgeHtml(buildCoverageBadge({ coverage })).includes(esc(text)));
    });
}

test('A3 reason text: every reason the verdict emits has a case above', () => {
    const covered = new Set(A3_REASON_TEXT.map(r => r[0]));
    for (const r of ['capture_loss', 'missing_file', 'unclean_end', 'retention', 'restart',
        'clock_step', 'not_yet_covered', 'not_recorded', 'not_captured']) {
        assert.ok(covered.has(r), r);
    }
    // Several reasons: server order, each once, joined by a space (A3 item 3).
    const cov = coverageOf({ coverage: { ...oneReason('clock_step', null),
        reasons: ['missing_file', 'clock_step', 'missing_file'] } });
    assert.equal(cov.label, COVERAGE_UNVERIFIED_LABEL +
        ' (trace file missing) (system clock stepped back)');
});

test('A3 live_edge: "up to HH:MM:SS" on whole-window numbers, the clipped range named', () => {
    const cov = coverageOf({ fidelity: 'exact', coverage: COVERAGE.live_edge_xf });
    // covered_to = COV_TO - 2 s = 1773999998 s since the epoch = 09:46:38 UTC.
    assert.equal(cov.upTo, 'up to 09:46:38', 'A3_UP_TO');
    assert.equal(UP_TO_PREFIX + '09:46:38', cov.upTo);
    assert.equal(cov.liveEdge.text, 'Live — not yet confirmed after 09:46:38', 'A3_LIVE_EDGE_TEXT');
    assert.equal(LIVE_EDGE_RANGE_PREFIX + '09:46:38', cov.liveEdge.text);
    assert.equal(cov.liveEdge.from, COV_TO - 2e9);
    assert.equal(cov.withheld, false);
    // Pane badge: both texts.
    const html = paneFidelityBadgeHtml(buildPaneFidelity({ fidelity: 'exact', coverage: COVERAGE.live_edge_xf }));
    assert.ok(html.includes('up to 09:46:38'), 'A3_UP_TO_IN_BADGE');
    assert.ok(html.includes('<li>Live — not yet confirmed after 09:46:38</li>'),
        'A3_LIVE_EDGE_IN_BADGE (the range text alone: the clipped range has no end yet)');
    // A sampled live window keeps the sampled label and gains both texts.
    const sampled = buildPaneFidelity({ fidelity: 'sampled', coverage: COVERAGE.live_edge_xf });
    assert.equal(sampled.label, 'Sampled (estimated)');
    assert.deepEqual(sampled.ranges.map(r => r.text), [cov.liveEdge.text]);
    // AAS peak (status line) names where its data ends, both renderers.
    const data = aasWindow('live_edge_xf');
    const ec = buildAasOption(data, { win: WIN });
    const up = buildUplotSpec(data, { win: WIN });
    assert.equal(aasPeakText(ec), 'peak 2.6 AAS (up to 09:46:38)', 'A3_UP_TO_ON_PEAK');
    assert.equal(aasPeakText(up), aasPeakText(ec));
    // The clipped range is shaded from covered_to to the window end and
    // labelled with the range text, in both renderers; buckets stay missing.
    const band = ec.option.series.find(s => s.name === AAS_ANNOTATION_SERIES).markArea.data;
    assert.equal(band.length, 1);
    assert.equal(band[0][0].xAxis, (COV_TO - 2e9) / 1e6);
    assert.equal(band[0][0].name, cov.liveEdge.text, 'A3_LIVE_EDGE_BAND_LABEL (ECharts)');
    const rects = overlayGeometry(data, { win: WIN }).rects.filter(r => r.label);
    assert.deepEqual(rects.map(r => r.label), [cov.liveEdge.text], 'A3_LIVE_EDGE_BAND_LABEL (uPlot)');
    const cpu = ec.option.series.find(s => s.name !== AAS_ANNOTATION_SERIES).data.map(p => p[1]);
    assert.equal(cpu[59], null, 'LIVE_EDGE_BUCKET_STAYS_MISSING');
    // A sampled live window keeps its whole-window sampled band too.
    const sh = buildFidelityShading({ fidelity: 'sampled', coverage: COVERAGE.live_edge_xf }, WIN);
    assert.deepEqual(sh.bands.map(b => b.kind), ['sampled', INCOMPLETE_FIDELITY]);
    // Complete windows carry neither text.
    const done = coverageOf({ coverage: COVERAGE.complete_xf });
    assert.equal(done.upTo, null);
    assert.equal(done.liveEdge, null);
    assert.match(aasPeakText(buildAasOption(aasWindow('complete'), { win: WIN })), /^peak [\d.]+ AAS$/);
    // A live_edge verdict without covered_to fails closed: its numbers would
    // have no stated end.
    const noEnd = coverageOf({ coverage: { ...COVERAGE.live_edge_xf, covered_to: null } });
    assert.equal(noEnd.verdict, 'incomplete', 'LIVE_EDGE_WITHOUT_END_FAILS_CLOSED');
    assert.equal(noEnd.withheld, true);
});

test('A3: no raw reason, verdict or cause token reaches rendered output', () => {
    const states = Object.keys(COVERAGE);
    const extra = { future: { ...oneReason('future_reason', 'postgres') } };
    const rendered = [];
    for (const [name, coverage] of [...states.map(k => [k, COVERAGE[k]]), ...Object.entries(extra),
        ...A3_REASON_TEXT.map(([r, c]) => [r + '/' + c, oneReason(r, c)])]) {
        const data = { fidelity: coverage.verdict === 'incomplete' ? 'incomplete' : 'exact', coverage };
        const wh = { code: 'coverage_unverified', coverage };
        rendered.push([name + ' notice', coverageNoticeHtml(buildCoverageNotice(wh))]);
        rendered.push([name + ' pane', paneFidelityBadgeHtml(buildPaneFidelity(data, { idle: true }))]);
        rendered.push([name + ' badge', paneFidelityBadgeHtml(buildCoverageBadge(data, { idle: true }))]);
        const aas = { ...aasWindow('complete'), ...data };
        const ec = buildAasOption(aas, { win: WIN });
        rendered.push([name + ' peak', aasPeakText(ec)]);
        rendered.push([name + ' bands', JSON.stringify(
            (ec.option.series.find(s => s.name === AAS_ANNOTATION_SERIES)?.markArea?.data || [])
                .map(p => p[0].name || ''))]);
        rendered.push([name + ' uPlot bands', JSON.stringify(
            overlayGeometry(aas, { win: WIN }).rects.map(r => r.label || ''))]);
        rendered.push([name + ' heatmap', JSON.stringify(
            buildHeatmapOption({ ...heatmapWindow('complete'), ...data }).option.series
                .map(s => s.markArea && s.markArea.label ? s.markArea.label.formatter : ''))]);
    }
    let nonEmpty = 0;
    for (const [where, html] of rendered) {
        if (html) nonEmpty++;
        for (const tok of RAW_TOKENS) {
            assert.ok(!String(html).includes(tok), `RAW_TOKEN_IN_OUTPUT ${tok} in ${where}: ${html}`);
        }
    }
    assert.ok(nonEmpty > rendered.length / 2, 'the check rendered real output');
});

/* ── r3: every §3 Web view, mounted ──────────────────────────────────────── */

/* A minimal DOM: an element registers every id its innerHTML names, so a
 * view's ensureShell() builds its shell and getElementById finds only what
 * the view itself wrote. text() is everything painted, in any element. */
function fakeDom() {
    const known = new Map();
    const all = [];
    function makeEl(id) {
        let html = '';
        const el = {
            id, style: {}, dataset: {}, textContent: '', className: '', hidden: false,
            disabled: false, value: '', isConnected: true, parentNode: null,
            clientWidth: 1000, clientHeight: 500, offsetWidth: 1000, offsetHeight: 500,
            classList: { add() {}, remove() {}, toggle() {}, contains() { return false; } },
            addEventListener() {}, removeEventListener() {}, appendChild() {}, remove() {},
            before() {}, insertBefore() {}, scrollHeight: 0,
            get parentElement() { return makeEl(null); },
            querySelector: () => makeEl(null), querySelectorAll: () => [],
            getBoundingClientRect: () => ({ top: 0, left: 0, width: 1000, height: 500, bottom: 500, right: 1000 }),
            insertAdjacentHTML(where, h) { el.innerHTML = where === 'afterbegin' ? h + html : html + h; },
            get innerHTML() { return html; },
            set innerHTML(v) {
                html = String(v);
                for (const m of html.matchAll(/id="([^"]+)"/g)) {
                    if (!known.has(m[1])) known.set(m[1], makeEl(m[1]));
                }
            },
        };
        all.push(el);
        return el;
    }
    const document = {
        getElementById: (id) => known.get(id) || null,
        querySelector: () => makeEl(null), querySelectorAll: () => [],
        createElement: () => makeEl(null), activeElement: null,
    };
    const echarts = { init: () => ({
        setOption() {}, on() {}, off() {}, dispose() {}, resize() {}, clear() {},
        getOption: () => ({ series: [] }), getWidth: () => 1000, getHeight: () => 500,
        dispatchAction() {}, convertToPixel: () => [0, 0], convertFromPixel: () => [0, 0],
        getZr: () => ({ on() {}, off() {} }), isDisposed: () => false,
    }) };
    return { document, echarts, makeEl,
        text: () => all.map(e => e.innerHTML + ' ' + e.textContent).join('\n') };
}

function withDom(fn) {
    const saved = ['document', 'window', 'getComputedStyle', 'requestAnimationFrame']
        .map(k => [k, Object.getOwnPropertyDescriptor(globalThis, k)]);
    const dom = fakeDom();
    globalThis.document = dom.document;
    globalThis.window = { innerHeight: 800, innerWidth: 1280, devicePixelRatio: 1,
        location: { search: '' }, addEventListener() {}, removeEventListener() {} };
    globalThis.getComputedStyle = () => ({ position: 'relative', getPropertyValue: () => '' });
    globalThis.requestAnimationFrame = () => 0;   // deferred paints never run here
    try { return fn(dom); } finally {
        for (const [k, d] of saved) {
            if (d) Object.defineProperty(globalThis, k, d); else delete globalThis[k];
        }
    }
}

/* Mount `view` on `data` in a fresh fake DOM; everything painted. */
function mounted(view, data, extra) {
    return withDom((dom) => {
        const c = { ...ctx(), echarts: dom.echarts, summaryEl: dom.makeEl('summary'),
            chartEl: dom.makeEl('chart'), isLiveTick: () => false, onDrill() {},
            ...(extra || {}) };
        const el = dom.makeEl('root');
        view.mount(el, view.build(data, c), c);
        return dom.text();
    });
}

const withCov = (state, payload) => ({ ...payload,
    fidelity: COVERAGE[state].verdict === 'incomplete' ? 'incomplete' : 'exact',
    coverage: COVERAGE[state] });
const TL = timelineStates['single-point'].data;
const SC = scatterStates['single-point'].data;
const WF = waterfallStates['single-point'].data;
const WF_ROW = { pid: 4100, query_id: WF.query_id, start_ns: WF.leader.events[0].start_ns,
    end_ns: '999999999999', duration_ms: 1, n_events: 1, n_workers: 0, in_progress: false };

/* Every view of spec §3 "Web", each rendered the way the app paints it, for
 * one coverage state. `empty`: the same response with no records (the
 * empty-state path). A view the app can only paint in a browser (active:
 * uPlot/ECharts) renders its exported pure surface: the status line and the
 * chip HTML the mount writes. */
const VIEW_RENDERERS = {
    overview: (s, empty) => {
        const tm = timeModelWindow(COVERAGE[s].verdict === 'incomplete' ? s : 'complete');
        const data = COVERAGE[s].verdict === 'incomplete' ? tm
            : { ...tm, coverage: COVERAGE[s], rows: empty ? [] : tm.rows };
        return mounted(createOverviewView(), data);
    },
    events: (s, empty) => {
        if (COVERAGE[s].verdict === 'incomplete') return mounted(createEventsView(), withheld(s));
        const ev = { ...topEventsWindow('complete'), coverage: COVERAGE[s] };
        return mounted(createEventsView(), empty ? { ...ev, rows: [] } : ev);
    },
    queries: (s, empty) => mounted(createQueriesView(),
        COVERAGE[s].verdict === 'incomplete' ? withheld(s)
            : withCov(s, { rows: empty ? [] : [{ query_id: '1', total_ms: 5000, calls: 3 }] })),
    sessions: (s, empty) => mounted(createSessionsView(),
        COVERAGE[s].verdict === 'incomplete' ? withheld(s)
            : withCov(s, { rows: empty ? [] : [{ pid: 1001, db_time_ms: 5000, cpu_pct: 50 }] })),
    active: (s, empty) => {
        const data = empty ? withCov(s, { bucket_ns: MIN_NS_T, buckets: [], max_aas: 0 }) : aasWindow(s);
        const out = [];
        for (const m of [buildAasOption(data, { win: WIN, numCpus: 4 }),
            buildUplotSpec(data, { win: WIN, numCpus: 4 })]) {
            if (!m.hasData) out.push(aasEmptyText(m), aasEmptyStatusText(m));
            else out.push(aasPeakText(m), aasFidelityChipHtml(m) || '');
        }
        return out.join('\n');
    },
    transitions: (s, empty) => mounted(createTransitionsView(), { transitions:
        COVERAGE[s].verdict === 'incomplete' ? withheld(s)
            : withCov(s, empty ? { total: 0, nodes: [], links: [] } : transitionsStates['single-self-loop'].data) }),
    variants: (s, empty) => buildVariantsPanel('ready',
        COVERAGE[s].verdict === 'incomplete' ? withheld(s)
            : withCov(s, empty ? {} : transitionsStates['variants-html'].variants), esc),
    histogram: (s, empty) => {
        const hm = heatmapWindow(empty ? 'not_recorded' : s);
        const data = empty ? { ...hm, coverage: COVERAGE[s], cells: [],
            fidelity: COVERAGE[s].verdict === 'incomplete' ? 'incomplete' : 'exact' } : hm;
        const ev = COVERAGE[s].verdict === 'incomplete' ? withheld(s) : topEventsWindow('complete');
        const view = createHistogramView();
        return withDom((dom) => {
            const c = { ...ctx(), echarts: dom.echarts, summaryEl: dom.makeEl('summary'),
                isLiveTick: () => false, onDrill() {},
                transport: { request: async () => null }, channel: (n) => n };
            const el = dom.makeEl('root');
            const model = view.build({ events: (ev && ev.rows) || [],
                eventsNotice: buildCoverageNotice(ev), heatmap: data }, c);
            view.mount(el, model, c);
            return dom.text();
        });
    },
    matrix: (s, empty) => mounted(createMatrixView(),
        COVERAGE[s].verdict === 'incomplete' ? withheld(s)
            : withCov(s, empty ? { total: 0, nodes: [], links: [] } : matrixStates['single-cell'].data)),
    concurrency: (s, empty) => mounted(createConcurrencyView(),
        COVERAGE[s].verdict === 'incomplete' ? withheld(s)
            : withCov(s, empty ? concurrencyStates.empty.data : concurrencyStates['single-point'].data)),
    'exec-scatter': (s, empty) => mounted(createExecScatterView(),
        withCov(s, empty ? scatterStates.empty.data : SC)),
    waterfall: (s, empty) => mounted(createWaterfallView(), {
        executions: withCov(s, { rows: empty ? [] : [WF_ROW], total_count: empty ? 0 : 1 }),
        selected: empty ? null : WF_ROW,
        detail: empty ? null : withCov(s, WF), detailEnd: WF_ROW.end_ns, windowTo: COV_TO }),
    timeline: (s, empty) => mounted(createTimelineView(),
        withCov(s, empty ? timelineStates.empty.data : TL)),
    // The pilot has no empty placeholder; its "empty" is a value-less answer.
    'events-pilot': (s, empty) => pilotEvidenceHtml(
        COVERAGE[s].verdict === 'incomplete' ? withheld(s) : withCov(s, {
            wait_time: { value: empty ? null : '123', unit: 'ns', completeness: 'complete', reasons: [], diagnostic: {} },
            completed_waits: { value: empty ? null : '4', unit: 'waits', completeness: 'complete', reasons: [], diagnostic: {} },
            storage: {} })),
};

/* Spec §3 Web, the view list, plus the variants panel (a §3 Withhold command
 * the transitions view paints). */
const SPEC_WEB_VIEWS = ['overview', 'events', 'queries', 'sessions', 'active', 'transitions',
    'histogram', 'matrix', 'concurrency', 'exec-scatter', 'waterfall', 'timeline', 'events-pilot'];

test('r3: the renderer list is the full §3 Web list and every file in web/static/views', () => {
    const files = readdirSync(new URL('../../web/static/views/', import.meta.url))
        .filter(f => f.endsWith('.js')).map(f => f.slice(0, -3)).sort();
    assert.deepEqual(files, SPEC_WEB_VIEWS.slice().sort(),
        'VIEW_ADDED_WITHOUT_COVERAGE_RENDERER: a new view must join SPEC_WEB_VIEWS and VIEW_RENDERERS');
    assert.deepEqual(Object.keys(VIEW_RENDERERS).sort(), [...SPEC_WEB_VIEWS, 'variants'].sort());
});

test('r3 F1: every view showing whole-window numbers carries "up to HH:MM:SS" and the in-flight mark on live_edge_xf', () => {
    for (const name of Object.keys(VIEW_RENDERERS)) {
        const text = VIEW_RENDERERS[name]('live_edge_xf', false);
        assert.ok(text.includes('up to 09:46:38'), `LIVE_EDGE_UP_TO_MISSING ${name}: ${text.slice(0, 400)}`);
        assert.ok(text.includes(IN_FLIGHT_EXCLUDED_LABEL), `IN_FLIGHT_MARK_MISSING ${name}`);
        assert.ok(!text.includes(COVERAGE_UNVERIFIED_LABEL), `LIVE_EDGE_IS_NOT_WITHHELD ${name}`);
        // complete_xf: the in-flight mark alone, no end time.
        const done = VIEW_RENDERERS[name]('complete_xf', false);
        assert.ok(done.includes(IN_FLIGHT_EXCLUDED_LABEL), `IN_FLIGHT_MARK_MISSING (complete) ${name}`);
        assert.ok(!done.includes('up to '), `UP_TO_ON_COMPLETE ${name}`);
    }
    // The AAS chip itself (not only the status line) names both marks, in
    // both renderers.
    for (const m of [buildAasOption(aasWindow('live_edge_xf'), { win: WIN }),
        buildUplotSpec(aasWindow('live_edge_xf'), { win: WIN })]) {
        const chip = aasFidelityChipHtml(m);
        assert.ok(chip.includes('up to 09:46:38'), 'AAS_CHIP_UP_TO');
        assert.ok(chip.includes(IN_FLIGHT_EXCLUDED_LABEL), 'AAS_CHIP_IN_FLIGHT');
    }
    // Negative control: a plain complete window carries neither mark.
    for (const name of Object.keys(VIEW_RENDERERS)) {
        const text = VIEW_RENDERERS[name]('complete', false);
        assert.ok(!text.includes(IN_FLIGHT_EXCLUDED_LABEL) && !text.includes('up to '),
            `MARK_WITHOUT_CAUSE ${name}`);
    }
});

/* Any empty-state placeholder: "No data …", "No events …", "No transitions
 * …", "No concurrency data", "No completed …", "No execution(s) …". */
const PLACEHOLDER = /\b[Nn]o [a-z]/;

test('r3 F2: no view renders a "No data"-style placeholder together with an incomplete coverage notice', () => {
    for (const state of INCOMPLETE_STATES) {
        for (const name of Object.keys(VIEW_RENDERERS)) {
            for (const empty of [true, false]) {
                const text = VIEW_RENDERERS[name](state, empty);
                assert.ok(text.includes(COVERAGE_UNVERIFIED_LABEL),
                    `INCOMPLETE_SHOWS_LABEL ${name} ${state} empty=${empty}`);
                assert.doesNotMatch(text, PLACEHOLDER,
                    `NO_DATA_UNDER_COVERAGE_NOTICE ${name} ${state} empty=${empty}`);
                // One pane, one label: the notice and a badge never stack.
                // (active: its status line and chart card, both renderers;
                // histogram: its disabled selectors say why, separately.)
                if (name !== 'active') {
                    const pane = text.replace(/<span class="selector-coverage"[^>]*>[^<]*<\/span>/g, '')
                        .replace(/<[^>]*>/g, ' ');   // visible text, not title attributes
                    assert.equal(pane.split(COVERAGE_UNVERIFIED_LABEL).length - 1, 1,
                        `LABEL_ONCE ${name} ${state} empty=${empty}`);
                }
            }
        }
    }
    // Waterfall's two detail-pane placeholders: a selected execution whose
    // detail has no events ("No execution events captured") and one whose
    // detail did not arrive ("No detail was returned …").
    const wfCases = (s) => [
        { executions: withCov(s, { rows: [WF_ROW], total_count: 1 }), selected: WF_ROW,
          detail: withCov(s, waterfallStates.empty.data), detailEnd: WF_ROW.end_ns, windowTo: COV_TO },
        { executions: withCov(s, { rows: [WF_ROW], total_count: 1 }), selected: WF_ROW,
          detail: null, detailEnd: WF_ROW.end_ns, windowTo: COV_TO },
    ];
    for (const state of INCOMPLETE_STATES) {
        for (const data of wfCases(state)) {
            const text = mounted(createWaterfallView(), data);
            assert.ok(text.includes(COVERAGE_UNVERIFIED_LABEL), `INCOMPLETE_SHOWS_LABEL waterfall detail ${state}`);
            assert.doesNotMatch(text, PLACEHOLDER, `NO_DATA_UNDER_COVERAGE_NOTICE waterfall detail ${state}`);
        }
    }
    for (const data of wfCases('complete')) {
        assert.match(mounted(createWaterfallView(), data), PLACEHOLDER, 'PLACEHOLDER_CONTROL waterfall detail');
    }
    // Negative control: the same empty responses on a complete window do
    // paint their placeholders (the regex sees them; the rule is scoped).
    const withPlaceholder = Object.keys(VIEW_RENDERERS)
        .filter(name => PLACEHOLDER.test(VIEW_RENDERERS[name]('complete', true)));
    assert.deepEqual(withPlaceholder.sort(), ['active', 'concurrency', 'events', 'exec-scatter',
        'histogram', 'matrix', 'overview', 'queries', 'sessions', 'timeline', 'transitions',
        'waterfall'].sort(), 'PLACEHOLDER_CONTROL');
    // ... and AAS's status line says "no data" there, not the label.
    const emptyAas = { bucket_ns: 60e9, buckets: [], max_aas: 0, fidelity: 'exact', coverage: COVERAGE.complete };
    assert.match(aasEmptyStatusText(buildAasOption(emptyAas, { win: WIN })), /^no data$/);
    assert.equal(aasEmptyStatusText(buildAasOption(withCov('capture_loss', { ...emptyAas }), { win: WIN })),
        COVERAGE_UNVERIFIED_LABEL + ' (capture loss)', 'AAS_EMPTY_STATUS_IS_LABEL');
});

test('r3 F2: the histogram of a wholly uncovered window shows the notice, not "No data"', () => {
    const data = heatmapWindow('not_recorded');
    assert.equal(buildHeatmapOption(data).hasData, false);
    const text = VIEW_RENDERERS.histogram('not_recorded', false);
    assert.match(text, /Coverage unverified — whole-window metrics unavailable \(coverage not recorded/);
    assert.doesNotMatch(text, /No data for selected event\/range/, 'HISTOGRAM_NO_DATA_UNDER_NOTICE');
    // The label appears once in the heatmap pane (the badge gives way).
    const pane = text.split('\n').filter(l => l.includes('coverage-unavailable') || l.includes('pane-fidelity-badge'));
    assert.equal(pane.filter(l => l.includes('pane-fidelity-badge')).length, 0, 'BADGE_GIVES_WAY_TO_NOTICE');
    assert.equal(coverageEmptyStateHtml(null, 'No data'), 'No data');
});

test('r3 nit: withheld top_events disables the histogram selectors and says why', () => {
    assert.equal(selectorCoverageHtml(null), '');
    const html = selectorCoverageHtml(buildCoverageNotice(withheld('capture_loss')));
    assert.match(html, /Coverage unverified — whole-window metrics unavailable \(capture loss\)/);
    withDom((dom) => {
        const c = { ...ctx(), echarts: dom.echarts, summaryEl: dom.makeEl('summary'),
            isLiveTick: () => false, onDrill() {} };
        const view = createHistogramView();
        const el = dom.makeEl('root');
        view.mount(el, view.build({ events: [], eventsNotice: buildCoverageNotice(withheld('capture_loss')),
            heatmap: heatmapWindow('capture_loss') }, c), c);
        assert.equal(dom.document.getElementById('hm-class').disabled, true, 'SELECTOR_DISABLED');
        assert.equal(dom.document.getElementById('hm-event').disabled, true, 'SELECTOR_DISABLED');
        assert.match(dom.document.getElementById('hm-coverage').innerHTML, /Coverage unverified/);
        // Control: with the event list, the selectors are live.
        const view2 = createHistogramView();
        const dom2 = fakeDom();
        globalThis.document = dom2.document;
        view2.mount(dom2.makeEl('root'), view2.build({ events: topEventsWindow('complete').rows,
            eventsNotice: null, heatmap: heatmapWindow('complete') }, c), { ...c, echarts: dom2.echarts });
        assert.equal(dom2.document.getElementById('hm-class').disabled, false);
        assert.equal(dom2.document.getElementById('hm-coverage').innerHTML, '');
        globalThis.document = dom.document;
    });
});

test('r3 nit: the heatmap fixture drops cells past covered_to, as aasWindow and the mock do (spec §2)', () => {
    const cov = COVERAGE.live_edge_xf;
    const hm = heatmapWindow('live_edge_xf');
    const late = hm.cells.filter(c => hm.times[c[0]] + hm.bucket_ns > cov.covered_to);
    assert.deepEqual(late, [], 'HEATMAP_LIVE_EDGE_CELLS_DROPPED');
    const aas = aasWindow('live_edge_xf');
    const lateAas = aas.buckets.filter(b => b.t + aas.bucket_ns > cov.covered_to);
    assert.ok(lateAas.length > 0 && lateAas.every(b => b.cpu === null), 'AAS_LIVE_EDGE_BUCKETS_NULL');
    // The bucket set dropped is the same in both fixture sources.
    const hmKept = new Set(hm.cells.map(c => c[0]));
    const aasKept = aas.buckets.map((b, i) => [b, i]).filter(([b]) => b.cpu !== null).map(([, i]) => i);
    assert.deepEqual([...hmKept].sort((a, b) => a - b), aasKept);
});
