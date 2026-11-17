/* #947 / #871 U3: every web view renders the server's coverage verdict
 * through the one reader (lib/builders/fidelity.js coverageOf) and none
 * computes its own. Fixtures: web/static/dev/fixtures/coverage.mjs. */
import { test } from 'node:test';
import assert from 'node:assert/strict';
import {
    coverageOf, coverageWithholds, coverageReasonText, aasScaleMax,
    COVERAGE_UNVERIFIED_LABEL, IN_FLIGHT_EXCLUDED_LABEL, IDLE_UNVERIFIED_LABEL,
    buildFidelityShading, INCOMPLETE_FIDELITY, LIVE_EDGE_RANGE_PREFIX, UP_TO_PREFIX,
} from '../../web/static/lib/builders/fidelity.js';
import {
    buildPaneFidelity, paneFidelityBadgeHtml, buildCoverageBadge,
    buildCoverageNotice, buildCompareCoverageNotice, coverageNoticeHtml,
} from '../../web/static/lib/panels.js';
import { buildAasOption, AAS_ANNOTATION_SERIES, aasPeakText } from '../../web/static/lib/builders/aas.js';
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
import {
    COVERAGE, INCOMPLETE_STATES, WITHHELD_COMMANDS, COV_FROM, COV_TO, COV_GAP,
    withheld, aasWindow, heatmapWindow, timeModelWindow, topEventsWindow,
    sessionTimelineWindow, execScatterWindow,
} from '../../web/static/dev/fixtures/coverage.mjs';

const WIN = { from: COV_FROM, to: COV_TO };
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
    out.variants = buildVariantsPanel('ready', leaky(state, { variants: [{ steps: 3, count: 9 }] }), esc);
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
