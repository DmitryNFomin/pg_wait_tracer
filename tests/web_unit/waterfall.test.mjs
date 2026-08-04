import { test } from 'node:test';
import assert from 'node:assert/strict';
import {
    PLAN_COLOR, buildExecutionsModel, buildWaterfallOption,
    buildWaterfallReadout, waterfallRenderItem, waterfallTooltipFormatter,
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
    assert.ok(m.table.rows[0].cells[3].html.includes('In progress'));
    assert.ok(m.table.rows[0].cells[0].html.includes('Started before selected window'));
    assert.equal(m.table.headers[5].label, 'Leader events');
    assert.ok(m.table.rows[0].cls.includes('selected-execution'));
    assert.equal(m.truncation.omitted, 4);
});

test('empty detail stays empty without invented count', () => {
    const m = buildWaterfallOption(null, {});
    assert.equal(m.hasData, false);
    assert.equal(m.option, null);
    assert.equal(m.total_count, 0);
});
