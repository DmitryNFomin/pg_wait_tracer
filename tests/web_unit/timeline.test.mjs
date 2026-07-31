/* Node unit tests for the pure timeline builder (lib/builders/timeline.js).
 *
 * Runs under `node --test`. Proves the session_timeline data -> ECharts
 * custom-series option mapping: bar [start, end] math (Bug-1 regression: start
 * = s, NOT s+d), pid->row indexing, x-axis window bounds, truncation flags, and
 * empty-input handling. The renderItem is exercised against a fake api so the
 * rect geometry is locked too.
 */
import { test } from 'node:test';
import assert from 'node:assert/strict';
import {
    buildTimelineOption, timelineRenderItem, timelineTooltipFormatter,
} from '../../web/static/lib/builders/timeline.js';
import { fmtTime } from '../../web/static/lib/format.js';

function data() {
    return {
        truncated: false, total_count: 3, pids: [1001, 1002],
        events: [
            { s: 100, d: 50, p: 1001, n: 'CPU*', c: 0, q: '42' },
            { s: 150, d: 30, p: 1001, n: 'IO:DataFileRead', c: 1, q: '42' },
            { s: 100, d: 80, p: 1002, n: 'Lock:relation', c: 2, q: '7' },
        ],
    };
}

test('bar data: [start=s, end=s+d, pidIdx, name, classIdx, query, dur, rawStart]', () => {
    const { option } = buildTimelineOption(data(), { from: 0, to: 1000 });
    const bars = option.series[0].data;
    assert.deepEqual(bars[0], [100, 150, 0, 'CPU*', 0, '42', 50, 100]);
    assert.deepEqual(bars[1], [150, 180, 0, 'IO:DataFileRead', 1, '42', 30, 150]);
    // pid 1002 maps to row index 1
    assert.deepEqual(bars[2], [100, 180, 1, 'Lock:relation', 2, '7', 80, 100]);
});

test('start uses s (not s+d) — Bug 1 regression', () => {
    const { option } = buildTimelineOption(data(), { from: 0, to: 1000 });
    const first = option.series[0].data[0];
    assert.equal(first[0], 100);           // start = s
    assert.equal(first[1] - first[0], 50); // duration preserved
});

test('y categories are "PID <n>" in pids order; x spans the view window', () => {
    const { option } = buildTimelineOption(data(), { from: 5, to: 900 });
    assert.deepEqual(option.yAxis.data, ['PID 1001', 'PID 1002']);
    assert.equal(option.xAxis.min, 5);
    assert.equal(option.xAxis.max, 900);
    assert.equal(option.series[0].type, 'custom');
});

test('chartHeight scales with pid count; truncation surfaced', () => {
    const m = buildTimelineOption(data(), { from: 0, to: 1 });
    assert.equal(m.chartHeight, Math.max(200, 2 * 50 + 80));  // 200
    const t = buildTimelineOption({ ...data(), truncated: true, total_count: 99 },
        { from: 0, to: 1 });
    assert.equal(t.truncated, true);
    assert.equal(t.total_count, 99);
    assert.equal(t.count, 3);
});

test('empty events -> hasData false, no option', () => {
    const m = buildTimelineOption({ events: [], pids: [] }, { from: 0, to: 1 });
    assert.equal(m.hasData, false);
    assert.equal(m.option, null);
});

/* P6 regression: the server emits start_ns = timestamp - duration with NO
 * clamp to the window, and custom series default clip:false in this bundle —
 * bars from long waits painted left across the PID axis labels. The drawn
 * geometry is now clamped to [from, to]; the RAW start survives at [7] so the
 * tooltip still tells the analyst when the wait truly began. */
test('pre-window start: drawn geometry clamped, raw start kept for the tooltip', () => {
    const from = 3_600_000_000_000, to = 3_660_000_000_000;   // 01:00:00-01:01:00
    const d = { truncated: false, total_count: 1, pids: [1001],
        events: [{ s: 3_599_000_000_000, d: 2_000_000_000,     // starts 00:59:59
                   p: 1001, n: 'Lock:relation', c: 2, q: '7' }] };
    const { option } = buildTimelineOption(d, { from, to });
    const bar = option.series[0].data[0];
    assert.equal(bar[0], from);                    // drawn start clamped to the window
    assert.equal(bar[1], 3_601_000_000_000);       // end = s + d, inside the window
    assert.equal(bar[6], 2_000_000_000);           // raw duration preserved
    assert.equal(bar[7], 3_599_000_000_000);       // raw start preserved
    // Belt-and-braces against off-axis paint: the series clips to the grid.
    assert.equal(option.series[0].clip, true);
    // The tooltip shows the TRUE start (00:59:59), not the clamped one.
    const html = timelineTooltipFormatter({ data: bar });
    assert.ok(html.includes('Start: ' + fmtTime(3_599_000_000_000)));
    assert.ok(!html.includes('Start: ' + fmtTime(from)));
});

test('option root disables animation (U0: no replayed draw-in on refresh)', () => {
    const { option } = buildTimelineOption(data(), { from: 0, to: 1000 });
    assert.equal(option.animation, false);
});

test('renderItem draws a class-colored rect at the bar start, 60% band height', () => {
    // Fake ECharts api: value(i) reads the bar tuple; coord maps [val,cat] ->
    // pixels; size returns the band dimensions.
    const bar = [100, 150, 0, 'CPU*', 0, '42', 50];
    const api = {
        value: (i) => bar[i],
        coord: ([v]) => [v, 200],          // x=value, fixed y
        size: () => [0, 40],               // band height 40
    };
    const r = timelineRenderItem({}, api);
    assert.equal(r.type, 'rect');
    assert.equal(r.shape.x, 100);          // starts at s
    assert.equal(r.shape.width, 50);       // end-start
    assert.equal(r.shape.height, 24);      // 40 * 0.6
    assert.equal(r.shape.y, 200 - 12);     // centered
    // class 0 (CPU) color from WAIT_CLASSES
    assert.equal(r.style.fill, 'rgb(80,250,123)');
});

test('renderItem: unknown classIdx falls back to grey, width >= 1', () => {
    const bar = [100, 100, 0, 'X', 99, '', 0];  // zero-width, bad class
    const api = { value: (i) => bar[i], coord: ([v]) => [v, 10], size: () => [0, 20] };
    const r = timelineRenderItem({}, api);
    assert.equal(r.style.fill, '#888');
    assert.equal(r.shape.width, 1);             // clamped to >= 1px
});

// ── Tooltip formatter (UI-6: query text is UNTRUSTED — any DB user's SQL) ────

test('tooltip escapes query text (HTML injection from SQL)', () => {
    const evil = 'SELECT 1 /* <img src=x onerror=alert(1)> */ <script>x</script>';
    const bar = [100, 150, 0, 'CPU*', 0, evil, 50_000, 100];
    const html = timelineTooltipFormatter({ data: bar });
    assert.ok(!html.includes('<img'), 'no raw <img injected');
    assert.ok(!html.includes('<script'), 'no raw <script injected');
    assert.ok(html.includes('&lt;script&gt;'), 'query text is escaped');
});

test('tooltip escapes the event name too, and omits empty/zero query', () => {
    const bar = [100, 150, 0, '<b>evil</b>', 0, '0', 50_000, 100];
    const html = timelineTooltipFormatter({ data: bar });
    assert.ok(!html.includes('<b>evil</b>'), 'name is escaped');
    assert.ok(html.includes('&lt;b&gt;evil&lt;/b&gt;'));
    assert.ok(!html.includes('Query:'), 'q="0" means no query line');
});
