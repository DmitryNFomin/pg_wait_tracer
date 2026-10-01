import { test } from 'node:test';
import assert from 'node:assert/strict';
import {
    PLAN_COLOR, buildExecutionsModel, buildWaterfallOption,
    buildWaterfallReadout, executionHasDetail, pickDefaultExecution,
    waterfallRenderItem, waterfallTooltipFormatter,
    EXECUTIONS_SORT_DURATION, EXECUTIONS_SORT_RECENT, EXECUTIONS_SORT_DEFAULT,
    executionsSortLabel, executionsSortToggleLabel, executionsSortToggleTarget,
    fmtExecutionDuration, executionsCountsLabel,
    executionsStatusLabel, executionsHasInferredEnd,
} from '../../web/static/lib/builders/waterfall.js';
import { eventColor, fmtTimeNs } from '../../web/static/lib/format.js';

const START = 1_719_792_000_000_000_123n;
const at = (delta) => String(START + BigInt(delta));

function detail() {
    return {
        query_id: '42',
        leader: { pid: 1000, total_count: 2, truncated: false, events: [
            { we: 0x01000015, name: 'IO:DataFileRead',
              start_ns: at(2_000_000), dur_ns: '8000000', cpu_ns: '1200000' },
            { we: 0x03000001, name: 'Lock:relation',
              start_ns: at(12_000_000), dur_ns: '3000000', cpu_ns: null },
        ]},
        workers: [
            { pid: 1006, total_count: 1, truncated: false, events: [
                { we: 0x01000015, name: 'IO:DataFileRead',
                  start_ns: at(4_000_000), dur_ns: '5000000', cpu_ns: null },
            ]},
            { pid: 1008, total_count: 1, truncated: false, events: [
                { we: 0, name: 'CPU*', start_ns: at(1_000_000),
                  dur_ns: '2000000', cpu_ns: '1900000' },
            ]},
        ],
        plan: { start_ns: at(-4_000_000), end_ns: at(-1_000_000) },
        total_count: 4, kept_count: 4, truncated: false,
    };
}

test('lane layout: leader first, sorted worker payload order retained, labels explicit', () => {
    const m = buildWaterfallOption(detail(), {
        executionStart: String(START), executionEnd: at(20_000_000),
    });
    assert.deepEqual(m.lanes, ['PID 1000 (leader)', 'PID 1006', 'PID 1008']);
    assert.equal(m.chartHeight, Math.max(240, 3 * 54 + 100));
    assert.deepEqual(m.bars.filter(b => b[9] === 'event').map(b => b[2]), [0, 0, 1, 2]);
});

test('plan is a distinct leader-lane band and expands the full resident extent', () => {
    const m = buildWaterfallOption(detail(), {
        executionStart: String(START), executionEnd: at(20_000_000),
    });
    const plan = m.bars.find(b => b[9] === 'plan');
    assert.equal(plan[2], 0);
    assert.equal(plan[3], 'Plan phase');
    assert.equal(plan[10], PLAN_COLOR);
    assert.equal(m.fullFrom, at(-4_000_000));
    assert.equal(m.fullTo, at(20_000_000));
});

test('clipping changes draw geometry only; raw tooltip start/duration/cpu survive', () => {
    const from = at(5_000_000), to = at(7_000_000);
    const m = buildWaterfallOption(detail(), {
        from, to, executionStart: String(START), executionEnd: at(20_000_000),
    });
    const io = m.bars.find(b => b[3] === 'IO:DataFileRead' && b[2] === 0);
    assert.equal(io[0], 9_000_000);
    assert.equal(io[1], 11_000_000);
    assert.equal(io[5], at(2_000_000));
    assert.equal(io[6], '8000000');
    assert.equal(io[7], '1200000');
    assert.equal(m.option.series[0].clip, true);
    const tip = waterfallTooltipFormatter({ data: io });
    assert.ok(tip.includes(fmtTimeNs(at(2_000_000), 1_000_000) + ' UTC'));
    assert.ok(tip.includes('Measured CPU'));
    assert.equal(buildWaterfallReadout(io).cpu, '1.2ms');
});

test('event bars use stable eventColor identity tint, not matrix/plan colors', () => {
    const m = buildWaterfallOption(detail(), { executionStart: String(START),
        executionEnd: at(20_000_000) });
    const io = m.bars.find(b => b[3] === 'IO:DataFileRead');
    assert.equal(io[10], eventColor(null, 'IO:DataFileRead'));
    assert.notEqual(io[10], PLAN_COLOR);
});

test('UTC x-axis formatter and animation/clip pins', () => {
    const m = buildWaterfallOption(detail(), { executionStart: String(START),
        executionEnd: at(20_000_000) });
    assert.equal(m.option.useUTC, true);
    assert.equal(m.option.animation, false);
    assert.equal(m.option.xAxis.axisLabel.formatter(4_000_000),
        fmtTimeNs(String(START), 1_000_000));
});

test('renderItem gives micro bars a visible 1px width', () => {
    const tuple = [10, 10, 0, 'CPU*', 0, '10', '0', null, 1, 'event', '#abc'];
    const api = { value: (i) => tuple[i], coord: ([x]) => [x, 50], size: () => [0, 20] };
    const shape = waterfallRenderItem({}, api);
    assert.equal(shape.shape.width, 1);
    assert.equal(shape.shape.height, 11.6);
    assert.equal(shape.style.fill, '#abc');
});

test('honest payload counts and per-lane truncation are carried without invention', () => {
    const d = detail();
    d.total_count = 12; d.kept_count = 4; d.truncated = true;
    d.workers[0].total_count = 9; d.workers[0].truncated = true;
    const m = buildWaterfallOption(d, { executionStart: String(START),
        executionEnd: at(20_000_000) });
    assert.equal(m.total_count, 12);
    assert.equal(m.kept_count, 4);
    assert.deepEqual(m.laneTruncations, [
        { pid: 1006, kept_count: 1, total_count: 9, truncated: true },
    ]);
});

test('executions table renders in-progress honestly and derives only known truncation count', () => {
    const response = { rows: [
        { pid: 7, query_id: '42', start_ns: String(START), end_ns: null,
          duration_ms: null, plan_ms: null, n_events: 3, n_workers: 0,
          in_progress: true, started_before_window: true },
    ], truncated: true, total_count: 5 };
    const m = buildExecutionsModel(response, response.rows[0]);
    // No windowTo passed (3rd arg omitted): falls back to the plain "In
    // progress" text, same as before this row had an elapsed-so-far bound
    // to show.
    assert.ok(m.table.rows[0].cells[3].html.includes('In progress'));
    assert.ok(m.table.rows[0].cells[0].html.includes('Started before selected window'));
    assert.equal(m.table.headers[5].label, 'Leader events');
    assert.ok(m.table.rows[0].cls.includes('selected-execution'));
    assert.equal(m.truncation.omitted, 4);
});

// #222 review (blocker follow-up): the Duration cell must say WHY an open
// row outranks a finished one under "longest running first" -- src/server.c
// ranks it by elapsed-so-far (to - start_ns), invisible if the cell just
// reads "In progress" with no number (docs/VISUAL_CHECKLIST.md SEMANTICS,
// #103 precedent for an open-ended value: "must read >=N and say why").
test('in_progress duration reads as a lower bound, not a bare "In progress"', () => {
    const row = { start_ns: String(START), in_progress: true };
    const windowTo = String(START + 997_000_000n);   // 997ms elapsed
    const d = fmtExecutionDuration(row, windowTo);
    assert.equal(d.text, '≥ 997.0ms (running)');
    assert.ok(d.tooltip && /still running/i.test(d.tooltip));
});

test('in_progress with no usable windowTo falls back to plain "In progress"', () => {
    assert.equal(fmtExecutionDuration({ start_ns: String(START), in_progress: true },
        undefined).text, 'In progress');
    assert.equal(fmtExecutionDuration({ start_ns: String(START), in_progress: true },
        String(START)).text, 'In progress');   // to == start: no elapsed span
});

// #222 review: a row closed at CMD_END (an error/cancel/timeout), not a
// real EXEC_END, carries a real span but was never MEASURED at the
// query's own completion -- src/server.c's end_inferred. Must not render
// identically to a normal completion (same fail-safe problem the blocker
// itself is about, one level down).
test('an inferred-end row is flagged, not presented as an equal measurement', () => {
    const measured = fmtExecutionDuration(
        { in_progress: false, duration_ms: 80, end_inferred: false }, null);
    assert.equal(measured.text, '80.0ms');
    assert.equal(measured.tooltip, null);

    const inferred = fmtExecutionDuration(
        { in_progress: false, duration_ms: 80, end_inferred: true }, null);
    assert.equal(inferred.text, '80.0ms *');
    assert.ok(inferred.tooltip && /not measured/i.test(inferred.tooltip));
});

test('executionsHasInferredEnd sees an inferred-ended row, and only that', () => {
    assert.equal(executionsHasInferredEnd([{ end_inferred: true }]), true);
    assert.equal(executionsHasInferredEnd([{ end_inferred: false }]), false);
    assert.equal(executionsHasInferredEnd([{}]), false);
    assert.equal(executionsHasInferredEnd([]), false);
    assert.equal(executionsHasInferredEnd(null), false);
    // An in-progress row has no end to infer, so it never shows a '*'
    // (fmtExecutionDuration renders it as "≥ … (running)") and must not
    // pull the legend on by itself.
    assert.equal(
        executionsHasInferredEnd([{ in_progress: true, end_inferred: true }]),
        false);
});

test('executionsStatusLabel spells the * out inline — nobody hovers on a '
    + 'projected screen (#222 review round 3)', () => {
    // RED input for the legend: this exact call. Before the fix the view
    // rendered executionsCountsLabel(150, 3) alone, so a page containing a
    // '*' said nothing at all about what it meant unless you hovered.
    assert.equal(executionsStatusLabel(150, 3, true),
        '150 running, 3 completed · * = inferred end');
    // No '*' on the page -> no legend; the title row stays uncluttered.
    assert.equal(executionsStatusLabel(150, 3, false),
        '150 running, 3 completed');
    // Counts unavailable (an older server) must not suppress the legend:
    // the '*' is still on screen and still needs explaining.
    assert.equal(executionsStatusLabel(null, null, true), '* = inferred end');
    // Nothing to say at all.
    assert.equal(executionsStatusLabel(null, null, false), null);
});

test('buildExecutionsModel exposes has_inferred_end from the rows it '
    + 'actually renders', () => {
    const withInferred = buildExecutionsModel(
        { rows: [{ pid: 1, start_ns: '1', duration_ms: 5, end_inferred: true }] },
        null, '10');
    assert.equal(withInferred.has_inferred_end, true);
    const without = buildExecutionsModel(
        { rows: [{ pid: 1, start_ns: '1', duration_ms: 5, end_inferred: false }] },
        null, '10');
    assert.equal(without.has_inferred_end, false);
    // Empty page: no rows, no '*', no legend.
    assert.equal(buildExecutionsModel({ rows: [] }, null, '10').has_inferred_end,
        false);
});

test('executionsCountsLabel renders the open/completed split, or nothing '
     + 'when the counts are unavailable', () => {
    assert.equal(executionsCountsLabel(150, 3), '150 running, 3 completed');
    assert.equal(executionsCountsLabel(0, 101), '0 running, 101 completed');
    assert.equal(executionsCountsLabel(null, null), null);
    assert.equal(executionsCountsLabel(undefined, 3), null);
});

test('buildExecutionsModel threads windowTo into the duration cell and '
     + 'passes open_count/completed_count through unmodified', () => {
    const response = { rows: [
        { pid: 7, query_id: '42', start_ns: String(START), end_ns: null,
          duration_ms: null, plan_ms: null, n_events: 0, n_workers: 0,
          in_progress: true, started_before_window: false },
    ], truncated: false, total_count: 1, open_count: 1, completed_count: 0 };
    const windowTo = String(START + 997_000_000n);
    const m = buildExecutionsModel(response, null, windowTo);
    assert.ok(m.table.rows[0].cells[3].html.includes('997.0ms (running)'));
    assert.equal(m.open_count, 1);
    assert.equal(m.completed_count, 0);
});

test('empty detail stays empty without invented count', () => {
    const m = buildWaterfallOption(null, {});
    assert.equal(m.hasData, false);
    assert.equal(m.option, null);
    assert.equal(m.total_count, 0);
});

/* ── issue #101: which execution the panel opens on ─────────────────────────
 *
 * Rows and the empty detail below are verbatim shapes from a real --mode full
 * capture on the gate box (pgbench 4 clients, --rate=25, PG18): the newest
 * execution is a microsecond-scale statement with no events, no workers and
 * no plan, and its execution_detail comes back with an empty leader lane.
 * Defaulting to rows[0] left the waterfall with nothing to draw, so the view
 * mounted no ECharts instance at all — the live-UI smoke's "no echarts
 * instance" / "#waterfall-chart canvas never appeared" failure.
 */
const LIVE_ROWS = [
    // newest: 4.1 us, nothing recorded inside it
    { pid: 7241, query_id: '6097083398544187049',
      start_ns: '1790342322836263503', end_ns: '1790342322836267630',
      duration_ms: 0.004127, plan_ms: null, n_events: 0, n_workers: 0,
      in_progress: false, started_before_window: false },
    { pid: 7244, query_id: '6097083398544187049',
      start_ns: '1790342321473531618', end_ns: '1790342321473541937',
      duration_ms: 0.010319, plan_ms: null, n_events: 0, n_workers: 0,
      in_progress: false, started_before_window: false },
    // first row that actually has a waterfall
    { pid: 7240, query_id: '3886912043147135675',
      start_ns: '1790342321423302579', end_ns: '1790342321423342733',
      duration_ms: 0.040154, plan_ms: 0.012, n_events: 2, n_workers: 0,
      in_progress: false, started_before_window: false },
];

const EMPTY_DETAIL = {
    query_id: '6097083398544187049',
    leader: { pid: 7241, query_id: '6097083398544187049', events: [],
              total_count: 0, truncated: false },
    workers: [], plan: null,
    total_count: 0, kept_count: 0, truncated: false,
};

test('an execution with no events, workers or plan has no waterfall to draw', () => {
    assert.equal(executionHasDetail(LIVE_ROWS[0]), false);
    assert.equal(executionHasDetail(LIVE_ROWS[2]), true);
    assert.equal(executionHasDetail(null), false);
    // and that is exactly what the builder says about its detail payload
    const m = buildWaterfallOption(EMPTY_DETAIL, {
        executionStart: LIVE_ROWS[0].start_ns,
        executionEnd: LIVE_ROWS[0].end_ns,
    });
    assert.equal(m.hasData, false);
    assert.equal(m.option, null);
});

test('default selection skips the newest empty executions to one with data', () => {
    const chosen = pickDefaultExecution(LIVE_ROWS);
    assert.equal(chosen, LIVE_ROWS[2]);
    assert.equal(executionHasDetail(chosen), true);
});

test('default selection prefers events, then workers, then a plan phase', () => {
    const planOnly = { pid: 1, n_events: 0, n_workers: 0, plan_ms: 1.5 };
    const workerOnly = { pid: 2, n_events: 0, n_workers: 2, plan_ms: null };
    const withEvents = { pid: 3, n_events: 4, n_workers: 0, plan_ms: null };
    const bare = { pid: 4, n_events: 0, n_workers: 0, plan_ms: null };
    assert.equal(pickDefaultExecution([bare, planOnly, workerOnly, withEvents]),
                 withEvents);
    assert.equal(pickDefaultExecution([bare, planOnly, workerOnly]), workerOnly);
    assert.equal(pickDefaultExecution([bare, planOnly]), planOnly);
});

test('default selection falls back to the newest row when nothing is drawable', () => {
    const rows = [LIVE_ROWS[0], LIVE_ROWS[1]];
    assert.equal(pickDefaultExecution(rows), rows[0]);
    assert.equal(pickDefaultExecution([]), null);
    assert.equal(pickDefaultExecution(null), null);
});

// #222: which slice the Waterfall tab asks the server for, and the toggle
// between "longest running first" (the default) and "latest first".
test('the shipped default is duration_desc (longest running first)', () => {
    assert.equal(EXECUTIONS_SORT_DEFAULT, EXECUTIONS_SORT_DURATION);
});

test('sort label reflects the current mode; anything but explicit "start_desc" reads as longest-running', () => {
    // Review on #222: an open row now ranks by elapsed-so-far alongside a
    // closed row's real duration, one shared axis -- "longest running",
    // not "slowest" (which would misdescribe an in-progress row that
    // outranks a finished short query only because it is still running).
    assert.equal(executionsSortLabel(EXECUTIONS_SORT_DURATION), 'longest running first');
    assert.equal(executionsSortLabel(EXECUTIONS_SORT_RECENT), 'latest first');
    // Undefined/unset sort state (e.g. before the view has ever requested
    // anything) reads as the default, not as some third, unlabeled mode.
    assert.equal(executionsSortLabel(undefined), 'longest running first');
});

test('toggle target and toggle-button label are always the OTHER mode', () => {
    assert.equal(executionsSortToggleTarget(EXECUTIONS_SORT_DURATION),
                 EXECUTIONS_SORT_RECENT);
    assert.equal(executionsSortToggleTarget(EXECUTIONS_SORT_RECENT),
                 EXECUTIONS_SORT_DURATION);
    // The toggle's "latest first" state is unaffected by the wording
    // change -- recency was never ambiguous.
    assert.equal(executionsSortToggleLabel(EXECUTIONS_SORT_DURATION),
                 'Show latest first');
    assert.equal(executionsSortToggleLabel(EXECUTIONS_SORT_RECENT),
                 'Show longest running first');
});
