import { test } from 'node:test';
import assert from 'node:assert/strict';
import {
    buildExecScatterOption, meaningfulScatterYRange, scatterExecutionIntent,
    scatterTooltipFormatter,
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
    assert.equal(m.notes[0], '1 in-progress execution excluded from the log axis');
    assert.equal(m.notes[1], '1 zero/unknown-duration execution excluded from the log axis');
});

test('downsample line uses only server kept/total counts', () => {
    const m = buildExecScatterOption({ points: [
        { t: T, duration_ms: 1, pid: 1, query_id: '1' },
    ], total_count: 9_000, kept_count: 2_000, downsampled: true });
    assert.equal(m.notes[0], 'Showing 2,000 of 9,000 executions (server-downsampled)');
    const unknown = buildExecScatterOption({ points: [
        { t: T, duration_ms: 1, pid: 1, query_id: '1' },
    ], downsampled: true });
    assert.deepEqual(unknown.notes, []);
});

test('tooltip uses raw UTC time and execution identity', () => {
    const m = buildExecScatterOption({ points: [
        { t: T, duration_ms: 12.5, pid: 1000, query_id: '42' },
    ]});
    const tip = scatterTooltipFormatter({ data: m.points[0] });
    assert.ok(tip.includes(fmtTime(Number(T), 1_000_000) + ' UTC'));
    assert.ok(tip.includes('PID 1000'));
    assert.ok(tip.includes('Query: 42'));
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
    assert.equal(m.notes[0], '1 in-progress execution excluded from the log axis');
});

test('time axis pads both edges so first/last symbols are not half-clipped', () => {
    const m = buildExecScatterOption({ points: [
        { t: T, duration_ms: 1, pid: 1, query_id: '1' },
        { t: '1719792001123456789', duration_ms: 2, pid: 2, query_id: '2' },
    ]});
    assert.deepEqual(m.option.xAxis.boundaryGap, ['3%', '3%']);
});
