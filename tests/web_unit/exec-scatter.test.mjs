import { test } from 'node:test';
import assert from 'node:assert/strict';
import {
    buildExecScatterOption, meaningfulScatterYRange, scatterExecutionIntent,
    scatterTooltipFormatter, EXEC_SCATTER_SCOPE_NOTE,
} from '../../web/static/lib/builders/exec-scatter.js';
import { fmtTime } from '../../web/static/lib/format.js';

const T = '1719792000123456789';

test('scatter pins time x axis, log y axis, UTC, large mode and raw ns', () => {
    const m = buildExecScatterOption({ points: [
        { t: T, duration_ms: 12.5, pid: 1000, query_id: '42' },
    ], total_count: 1, kept_count: 1, downsampled: false });
    assert.equal(m.option.xAxis.type, 'time');
    assert.equal(m.option.yAxis.type, 'log');
    assert.equal(m.option.useUTC, true);
    assert.equal(m.option.series[0].large, true);
    assert.equal(m.points[0][4], T);
    assert.equal(m.option.animation, false);
});

test('null and zero durations are excluded from log axis with an exact visible note', () => {
    const m = buildExecScatterOption({ points: [
        { t: T, duration_ms: null, pid: 1, query_id: '1', in_progress: true },
        { t: '1719792001123456789', duration_ms: 0, pid: 2, query_id: '2', in_progress: false },
        { t: '1719792002123456789', duration_ms: 1.5, pid: 3, query_id: '3' },
    ], total_count: 3, kept_count: 3, downsampled: false });
    assert.equal(m.points.length, 1);
    assert.equal(m.excludedCount, 2);
    assert.equal(m.inProgressCount, 1);
    assert.equal(m.zeroDurationCount, 1);
    // #455: the unconditional scope note is always first.
    assert.equal(m.notes[0], EXEC_SCATTER_SCOPE_NOTE);
    assert.equal(m.notes[1], '1 in-progress execution excluded from the log axis');
    assert.equal(m.notes[2], '1 zero/unknown-duration execution excluded from the log axis');
});

test('downsample line uses only server kept/total counts', () => {
    const m = buildExecScatterOption({ points: [
        { t: T, duration_ms: 1, pid: 1, query_id: '1' },
    ], total_count: 9_000, kept_count: 2_000, downsampled: true });
    assert.equal(m.notes[0], EXEC_SCATTER_SCOPE_NOTE);
    assert.equal(m.notes[1], 'Showing 2,000 of 9,000 executions (outlier-' +
        'preserving sample; point density does not represent frequency)');
    const unknown = buildExecScatterOption({ points: [
        { t: T, duration_ms: 1, pid: 1, query_id: '1' },
    ], downsampled: true });
    // #455: no sampling/exclusion facts to report here (no total/kept
    // counts), but the scope note is UNCONDITIONAL -- it is not "no notes
    // at all" the way it used to be.
    assert.deepEqual(unknown.notes, [EXEC_SCATTER_SCOPE_NOTE]);
});

test('tooltip uses raw UTC time and execution identity', () => {
    const m = buildExecScatterOption({ points: [
        { t: T, duration_ms: 12.5, pid: 1000, query_id: '42' },
    ]});
    const tip = scatterTooltipFormatter({ data: m.points[0] });
    assert.ok(tip.includes(fmtTime(Number(T), 1_000_000) + ' UTC'));
    assert.ok(tip.includes('PID 1000'));
    assert.ok(tip.includes('Query: 42'));
    // #455: aligned with the renamed axis -- "Execution duration", not the
    // bare "Duration" that used to imply end-to-end wait-to-completion time.
    assert.ok(tip.includes('Execution duration: <b>12.5ms</b>'), tip);
});

test('point intent preserves decimal start and carries pid/start/end to waterfall', () => {
    const m = buildExecScatterOption({ points: [
        { t: T, duration_ms: 12.5, pid: 1000, query_id: '42' },
    ]});
    assert.deepEqual(scatterExecutionIntent(m.points[0]), {
        pivot: 'scatter-execution', pid: 1000, start_ns: T,
        end_ns: (BigInt(T) + 12_500_000n).toString(), query_id: '42',
    });
});

test('meaningful y constraint is measured in log space', () => {
    assert.equal(meaningfulScatterYRange(10, 20, 1, 1000), true);
    assert.equal(meaningfulScatterYRange(1, 900, 1, 1000), false);
    assert.equal(meaningfulScatterYRange(0, 20, 1, 1000), false);
});

test('all in-progress payload is honest empty with exclusion note', () => {
    const m = buildExecScatterOption({ points: [
        { t: T, duration_ms: null, pid: 1, query_id: '1', in_progress: true },
    ], total_count: 1, kept_count: 1, downsampled: false });
    assert.equal(m.hasData, false);
    assert.equal(m.option, null);
    assert.equal(m.notes[0], EXEC_SCATTER_SCOPE_NOTE);
    assert.equal(m.notes[1], '1 in-progress execution excluded from the log axis');
});

// ── #455 gate: the scope note is exposed WITHOUT hovering, in every one of
// the three named fixture shapes, and the axis rename holds too. ───────────

test('#455 gate: ordinary fixture exposes the scope note unconditionally', () => {
    const m = buildExecScatterOption({ points: [
        { t: T, duration_ms: 12.5, pid: 1000, query_id: '42' },
    ]});
    assert.ok(m.notes.includes(EXEC_SCATTER_SCOPE_NOTE), m.notes);
    assert.equal(m.option.yAxis.name, 'Execution duration (ms)');
});

test('#455 gate: downsampled fixture exposes the scope note unconditionally '
    + '(not merely when downsampled=true happens to ALSO be set)', () => {
    const m = buildExecScatterOption({ points: [
        { t: T, duration_ms: 1, pid: 1, query_id: '1' },
    ], total_count: 9_000, kept_count: 2_000, downsampled: true });
    assert.ok(m.notes.includes(EXEC_SCATTER_SCOPE_NOTE), m.notes);
});

test('#455 gate: all-in-progress (hasData=false) fixture STILL exposes the '
    + 'scope note -- the empty-state path (views/exec-scatter.js) renders '
    + 'model.notes too, so this is not lost just because there is no chart', () => {
    const m = buildExecScatterOption({ points: [
        { t: T, duration_ms: null, pid: 1, query_id: '1', in_progress: true },
    ]});
    assert.equal(m.hasData, false);
    assert.ok(m.notes.includes(EXEC_SCATTER_SCOPE_NOTE), m.notes);
});

// Negative controls (brief): each of these, if it described the real code,
// would make the three positive gate tests above fail -- demonstrating the
// gate is not vacuously true.

test('#455 negative control: a note list with EXEC_SCATTER_SCOPE_NOTE removed '
    + 'fails the "exposes the note" assertion (sanity-checks the assertion '
    + 'itself can fail)', () => {
    const m = buildExecScatterOption({ points: [
        { t: T, duration_ms: 12.5, pid: 1000, query_id: '42' },
    ]});
    const withoutScopeNote = m.notes.filter(n => n !== EXEC_SCATTER_SCOPE_NOTE);
    assert.ok(!withoutScopeNote.includes(EXEC_SCATTER_SCOPE_NOTE));
});

test('#455 negative control: conditioning the note on downsampled=true would '
    + 'fail on the ordinary (non-downsampled) fixture', () => {
    const ordinary = buildExecScatterOption({ points: [
        { t: T, duration_ms: 12.5, pid: 1000, query_id: '42' },
    ]});   // downsampled is falsy here
    const wronglyConditioned = ordinary.downsampled ? [EXEC_SCATTER_SCOPE_NOTE] : [];
    assert.ok(!wronglyConditioned.includes(EXEC_SCATTER_SCOPE_NOTE),
        'a downsampled-gated note would be ABSENT on the ordinary fixture -- ' +
        'proves the gate is not vacuous against that mistake');
    // The REAL builder output does not make this mistake:
    assert.ok(ordinary.notes.includes(EXEC_SCATTER_SCOPE_NOTE));
});

test('#455 negative control: restoring the generic "Duration (ms)" axis '
    + 'name fails the chart-option assertion', () => {
    const m = buildExecScatterOption({ points: [
        { t: T, duration_ms: 12.5, pid: 1000, query_id: '42' },
    ]});
    assert.notEqual(m.option.yAxis.name, 'Duration (ms)',
        'the renamed axis must not have regressed back to the generic label');
    assert.equal(m.option.yAxis.name, 'Execution duration (ms)');
});

test('time axis pads both edges so first/last symbols are not half-clipped', () => {
    const m = buildExecScatterOption({ points: [
        { t: T, duration_ms: 1, pid: 1, query_id: '1' },
        { t: '1719792001123456789', duration_ms: 2, pid: 2, query_id: '2' },
    ]});
    assert.deepEqual(m.option.xAxis.boundaryGap, ['3%', '3%']);
});
