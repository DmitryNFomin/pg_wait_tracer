/* Node unit tests for the pure timeline builder (lib/builders/timeline.js).
 *
 * Runs under `node --test`. Proves the session_timeline data -> ECharts
 * custom-series option mapping: bar [start, end] math (Bug-1 regression: start
 * = s, NOT s+d), pid->row indexing, x-axis window bounds, truncation flags, and
 * empty-input handling. The renderItem is exercised against a fake api so the
 * rect geometry is locked too.
 *
 * The second half covers the #106 per-pixel-column density aggregation.
 */
import { test } from 'node:test';
import assert from 'node:assert/strict';
import {
    buildTimelineOption, timelineRenderItem, timelineTooltipFormatter,
    timelinePlotWidth, aggregateTimelineColumns, timelineAggRenderItem,
    timelineAggTooltipFormatter, timelineBannerNote,
    TIMELINE_AGG_SPANS_PER_PX, TIMELINE_AGG_MIN_SPANS,
    TIMELINE_AGG_MIN_DENSE_COLS, TIMELINE_DEFAULT_WIDTH,
} from '../../web/static/lib/builders/timeline.js';
import { fmtTime, WAIT_CLASSES } from '../../web/static/lib/format.js';

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

// ── Density aggregation (#106) ───────────────────────────────────────────────
//
// At real density (3000–5000 spans on ONE PID row over ~1100 px) the per-span
// rects fused into a solid block whose alpha-composited pixels rendered a
// yellow-green belonging to no wait class. Above TIMELINE_AGG_SPANS_PER_PX the
// builder buckets spans into pixel columns and emits, per column, a stack of
// class-colored segments sized by each class's share of that column's wait
// time. These cases pin the arithmetic, the threshold, the banner text, the
// palette-only colors and the cross-tick stability.

/* n spans on one PID row, evenly spread over [from, to). The class cycles over
 * `classes` so each column's composition is predictable. */
function denseData(n, classes, from, to) {
    const step = (to - from) / n;
    const events = [];
    for (let i = 0; i < n; i++) {
        const c = classes[i % classes.length];
        events.push({ s: Math.round(from + step * i), d: Math.round(step / 2),
            p: 7001, n: 'e' + c, c, q: '42' });
    }
    return { truncated: false, total_count: n, pids: [7001], events };
}

const PALETTE = new Set(WAIT_CLASSES.map(c => c.color));

test('plot width = host width minus the grid margins; default when unmeasured', () => {
    assert.equal(timelinePlotWidth(1240), 1240 - 100 - 20);
    assert.equal(timelinePlotWidth(0), TIMELINE_DEFAULT_WIDTH - 120);
    assert.equal(timelinePlotWidth(undefined), TIMELINE_DEFAULT_WIDTH - 120);
    assert.equal(timelinePlotWidth(10), 1);        // never zero columns
});

/* `perColumn` zero-length spans in each of the first `columns` pixel columns,
 * placed at the column centre so the density is EXACTLY perColumn per painted
 * pixel — the only way to sit on the threshold on purpose. The first
 * `extraCols` columns get one span more. */
function exactDensity(columns, perColumn, extraCols, from, to, width) {
    const cols = timelinePlotWidth(width);
    const colNs = (to - from) / cols;
    const events = [];
    for (let c = 0; c < columns; c++) {
        const n = perColumn + (c < (extraCols || 0) ? 1 : 0);
        for (let i = 0; i < n; i++) {
            events.push({ s: Math.round(from + colNs * (c + 0.5)), d: 0,
                p: 7001, n: 'CPU*', c: 0, q: '42' });
        }
    }
    return { truncated: false, total_count: events.length, pids: [7001], events };
}

test('threshold boundary: exactly N spans/px stays per-span, N+1 over a block aggregates', () => {
    const from = 0, to = 1_000_000_000, W = 1000;
    const N = TIMELINE_AGG_SPANS_PER_PX, D = TIMELINE_AGG_MIN_DENSE_COLS;
    const columns = 4 * D;

    const at = buildTimelineOption(exactDensity(columns, N, 0, from, to, W),
        { from, to, width: W });
    assert.equal(at.aggregated, false, 'exactly N per painted px: individual spans');
    assert.equal(at.option.series[0].data.length, columns * N);
    assert.equal(at.bannerNote, null);

    // One span past the threshold on a BLOCK of columns: aggregate.
    const over = buildTimelineOption(exactDensity(columns, N, D, from, to, W),
        { from, to, width: W });
    assert.equal(over.aggregated, true, 'a block of over-dense columns aggregates');
    assert.equal(over.columns, timelinePlotWidth(W));
    assert.equal(over.option.series[0].data.length, columns,
        'one stack per occupied column');
});

/* The mean over painted columns is dragged over the threshold by ONE bad
 * pixel. Reviewer's counter-example: 50 waits stacked in a single pixel plus
 * 20 legible spread bars is 70/21 = 3.3 per painted column — over the mean
 * threshold, yet 20 of those bars are perfectly readable and master drew them
 * with their own tooltips. Aggregating that row would be a regression, so the
 * over-dense condition must hold on a BLOCK of columns, not a spike. */
test('one over-dense pixel does not aggregate a legible row', () => {
    const from = 0, to = 1_000_000_000, W = 1000;
    const cols = timelinePlotWidth(W);
    const colNs = (to - from) / cols;
    const events = [];
    for (let i = 0; i < 50; i++) {            // 50 waits in ONE pixel
        events.push({ s: Math.round(colNs * 0.5), d: 0, p: 7001,
            n: 'Timeout:x', c: 6, q: '42' });
    }
    for (let i = 0; i < 20; i++) {            // 20 legible, spread bars
        events.push({ s: Math.round(colNs * (40 * i + 20)), d: Math.round(colNs * 8),
            p: 7001, n: 'CPU*', c: 0, q: '42' });
    }
    const d = { truncated: false, total_count: 70, pids: [7001], events };
    const m = buildTimelineOption(d, { from, to, width: W });
    assert.equal(m.count, 70);
    assert.equal(m.aggregated, false, 'a spike is not a block');
    assert.equal(m.option.series[0].data.length, 70, 'all 70 keep their identity');
    assert.equal(m.option.series[0].renderItem, timelineRenderItem);
    assert.equal(m.bannerNote, null);
});

/* Code review's counter-example in the other direction: 199 spans inside 20 px
 * is a fused block by any reading — it MUST aggregate. 20 over-dense columns
 * clears TIMELINE_AGG_MIN_DENSE_COLS (16). */
test('199 spans in 20 px aggregates', () => {
    const from = 0, to = 1_000_000_000, W = 1000;
    const m = buildTimelineOption(exactDensity(20, 9, 19, from, to, W),
        { from, to, width: W });
    assert.equal(m.count, 199);
    assert.equal(m.aggregated, true);
    assert.equal(m.option.series[0].data.length, 20, 'one stack per painted pixel');
    assert.ok(m.spansPerPx >= 9);
});

/* The two floors are not independent: 16 columns carrying more than 2 spans
 * each is already at least 48 spans, so TIMELINE_AGG_MIN_SPANS can never be
 * the binding constraint at the current constants. Pinned so that lowering
 * MIN_DENSE_COLS (e.g. for #123's per-row aggregation) without revisiting the
 * span floor shows up here rather than in a user's chart. */
test('the dense-column guard implies the span floor at the current constants', () => {
    assert.ok(TIMELINE_AGG_MIN_DENSE_COLS * (TIMELINE_AGG_SPANS_PER_PX + 1)
        >= TIMELINE_AGG_MIN_SPANS);
});

test('a span that starts after the window is clipped, not folded into the last px', () => {
    // The per-span path hands such a rect an x past the plot and clip:true
    // drops it; counting it here would tint the last column and inflate the
    // density. A span that ENDED before the window still paints at the left
    // edge, exactly as the per-span path draws it.
    const from = 1000, to = 2000;
    const bars = [
        [3000, 2000, 0, 'Lock:relation', 2, '', 500, 3000],   // starts after `to`
        [1000, 500, 0, 'CPU*', 0, '', 500, 500],              // ended before `from`
    ];
    const agg = aggregateTimelineColumns(bars, 1, 10, from, to);
    assert.equal(agg.segments.length, 1, 'only the left-edge wait paints');
    assert.equal(agg.segments[0][3], 0, 'and it is the CPU one');
    assert.equal(agg.segments[0][0], from, 'in the first column');
    assert.equal(agg.columnSpanTotal, 1, 'the clipped span is not counted');
});

test('a single column is never aggregated, however deep: identity beats density', () => {
    const from = 0, to = 1_000_000_000, W = 1000;
    // Everything in ONE pixel, far past both the per-pixel threshold and the
    // span floor: still one bar's worth of screen, so the per-wait tooltips
    // are worth more than summarising it.
    for (const n of [TIMELINE_AGG_MIN_SPANS - 1, TIMELINE_AGG_MIN_SPANS, 500]) {
        const m = buildTimelineOption(exactDensity(1, n, 0, from, to, W),
            { from, to, width: W });
        assert.equal(m.aggregated, false, n + ' spans in one pixel');
        assert.equal(m.option.series[0].data.length, n);
    }
});

/* Regression (found by tests/test_web_ui.py's timeline-bar-positions case):
 * waits that ended BEFORE the window all clamp to the same edge pixel (P6),
 * so they read as N spans/px even though the chart holds N waits in total.
 * Aggregating those would have replaced a legible 8-bar chart — and its
 * per-wait tooltips, which are what that whole state is for — with one
 * summarised pixel. */
test('waits clamped to the window edge do not fake density on a small chart', () => {
    const from = 2_000_000_000, to = 2_900_000_000;
    const events = [];
    for (let i = 0; i < 8; i++) {
        events.push({ s: 1_000_000_000 + i * 10_000_000, d: 50_000_000,
            p: 1001, n: 'CPU*', c: 0, q: '42' });
    }
    const m = buildTimelineOption(
        { truncated: false, total_count: 8, pids: [1001], events },
        { from, to, width: 1240 });
    assert.equal(m.aggregated, false);
    assert.equal(m.option.series[0].data.length, 8);
    // Per-span tuples: the raw duration still rides at [6] for the tooltip.
    assert.equal(m.option.series[0].data[0][6], 50_000_000);
});

test('density is per PAINTED pixel: a burst in a sixth of the window aggregates', () => {
    // The live UI smoke's real shape (#106/#100): 519 spans packed into ~185 px
    // at one edge of an otherwise empty ~1100 px window. Averaged over the plot
    // that is 0.5 spans/px — under any threshold — but a painted pixel there
    // carries ~2.8, and those are the pixels that fuse.
    const from = 0, to = 6_000_000_000, W = 1240;
    const burst = denseData(519, [6], from, to / 6);
    const m = buildTimelineOption(burst, { from, to, width: W });
    assert.ok(519 / timelinePlotWidth(W) < TIMELINE_AGG_SPANS_PER_PX,
        'events/plotWidth alone would NOT trigger');
    assert.equal(m.aggregated, true);
    assert.ok(m.spansPerPx >= 2, 'reported density is per painted px');
});

test('density is the BUSIEST ROW, not the total: 50 rows x 8 bars stays per-span', () => {
    const from = 0, to = 1_000_000_000, pids = [], events = [];
    for (let p = 0; p < 50; p++) {
        pids.push(4000 + p);
        for (let j = 0; j < 8; j++) {
            events.push({ s: from + j * 1e8, d: 5e7, p: 4000 + p,
                n: 'CPU*', c: 0, q: '1' });
        }
    }
    const m = buildTimelineOption({ truncated: false, total_count: 400, pids, events },
        { from, to, width: 1000 });
    assert.equal(m.aggregated, false);
    assert.equal(m.option.series[0].data.length, 400);

    // ...but ONE dense row among 49 sparse ones still fuses, so it decides.
    const busy = events.slice();
    for (let i = 0; i < 4000; i++) {
        busy.push({ s: from + Math.round((to - from) * i / 4000), d: 5e4,
            p: 4000, n: 'Timeout:x', c: 6, q: '1' });
    }
    const m2 = buildTimelineOption(
        { truncated: false, total_count: 4400, pids, events: busy },
        { from, to, width: 1000 });
    assert.equal(m2.aggregated, true, 'the busiest row drives the decision');
});

test('aggregation: one stack per occupied column, class shares from wait time', () => {
    // 4 columns of 100 ns. Column 0 gets a 60 ns Timeout(6) span and a 40 ns
    // CPU(0) span; column 2 gets one CPU span; columns 1 and 3 stay empty.
    const from = 0, to = 400, cols = 4;
    const bars = [
        [0, 60, 0, 'Timeout:x', 6, '', 60, 0],
        [0, 40, 0, 'CPU*', 0, '', 40, 0],
        [200, 300, 0, 'CPU*', 0, '', 100, 200],
    ];
    const agg = aggregateTimelineColumns(bars, 1, cols, from, to);
    assert.equal(agg.occupiedColumns, 2);
    assert.equal(agg.maxColumnSpans, 2);
    assert.equal(agg.columnSpanTotal, 3);

    const col0 = agg.segments.filter(s => s[0] === 0);
    // Canonical WAIT_CLASSES order: CPU (0) sits below Timeout (6), always.
    assert.deepEqual(col0.map(s => s[3]), [0, 6]);
    assert.deepEqual(col0[0].slice(0, 3), [0, 100, 0]);      // column x-bounds, row
    assert.equal(col0[0][4], 0);                             // CPU from the bottom
    assert.ok(Math.abs(col0[0][5] - 0.4) < 1e-9, 'CPU share = 40/100');
    assert.ok(Math.abs(col0[1][4] - 0.4) < 1e-9, 'Timeout starts where CPU ends');
    assert.equal(col0[1][5], 1, 'the top class closes the stack at exactly 1');
    assert.equal(col0[0][6], 1);                             // spans of this class
    assert.equal(col0[0][7], 2);                             // spans in the column
    assert.equal(col0[0][8], 40);                            // class wait ns
    assert.equal(col0[0][9], 100);                           // column wait ns

    // Column 2 is single-class: one segment filling the band.
    const col2 = agg.segments.filter(s => s[0] === 200);
    assert.deepEqual(col2.map(s => [s[3], s[4], s[5]]), [[0, 0, 1]]);
});

test('aggregation: a span covering many columns paints every column it covers', () => {
    const agg = aggregateTimelineColumns(
        [[100, 300, 0, 'Lock:relation', 2, '', 200, 100]], 1, 4, 0, 400);
    assert.deepEqual(agg.segments.map(s => s[0]), [100, 200]);
    assert.deepEqual(agg.segments.map(s => s[8]), [100, 100], 'overlap ns per column');
});

/* A zero-length wait is PRESENT in its column — it happened, so it must appear
 * in the column's counts and share. That presence buys it no pixels: alone in
 * a column it is the only class and fills the band; beside a real wait it is
 * the sub-pixel sliver its share says it is (no floor, P11). */
test('aggregation: a zero-duration span is present in its column counts', () => {
    const alone = aggregateTimelineColumns(
        [[150, 150, 0, 'LWLock:WALInsert', 3, '', 0, 150]], 1, 4, 0, 400);
    assert.equal(alone.segments.length, 1);
    assert.deepEqual([alone.segments[0][3], alone.segments[0][4], alone.segments[0][5]],
        [3, 0, 1], 'the only class in the column fills the band');
    assert.equal(alone.segments[0][6], 1, 'counted');

    // Mixed column: a 99 ns CPU wait and a zero-length LWLock in the same pixel.
    const mixed = aggregateTimelineColumns([
        [101, 200, 0, 'CPU*', 0, '', 99, 101],
        [150, 150, 0, 'LWLock:WALInsert', 3, '', 0, 150],
    ], 1, 4, 0, 400);
    const col = mixed.segments.filter(s => s[0] === 100);
    assert.deepEqual(col.map(s => s[3]), [0, 3], 'both classes present');
    assert.equal(col[0][8], 99, 'CPU keeps its 99 ns');
    assert.equal(col[1][8], 1, 'the zero-length wait carries a nominal 1 ns');
    assert.equal(col[1][7], 2, 'the column counts 2 spans');
    assert.ok(col[1][5] - col[1][4] < 0.011,
        'its slice is ~1% of the band — presence, not pixels');
    assert.equal(col[1][5], 1, 'and the stack still closes at exactly 1');
});

test('aggregation: an out-of-range class becomes Unknown, never an off-palette fill', () => {
    const agg = aggregateTimelineColumns(
        [[10, 20, 0, 'weird', 99, '', 10, 10]], 1, 4, 0, 400);
    assert.equal(agg.segments[0][3], WAIT_CLASSES.length - 1);
    assert.equal(WAIT_CLASSES[agg.segments[0][3]].label, 'Unknown');
});

test('aggregated fills come ONLY from the shared palette (no composited hue)', () => {
    const from = 0, to = 1_000_000_000;
    const m = buildTimelineOption(denseData(5000, [0, 6, 1], from, to),
        { from, to, width: 1240 });
    assert.equal(m.aggregated, true);
    const ri = m.option.series[0].renderItem;
    const seen = new Set();
    for (const seg of m.option.series[0].data) {
        const api = { value: (i) => seg[i], coord: ([v]) => [v, 100], size: () => [0, 40] };
        const r = ri({}, api);
        seen.add(r.style.fill);
        assert.ok(PALETTE.has(r.style.fill), 'fill ' + r.style.fill + ' is a class color');
        assert.equal(r.style.opacity, undefined, 'no alpha: nothing composites');
    }
    assert.deepEqual([...seen].sort(),
        [WAIT_CLASSES[0].color, WAIT_CLASSES[1].color, WAIT_CLASSES[6].color].sort());
});

test('aggregated column stacks fill the band exactly (no gap, no overflow)', () => {
    const from = 0, to = 1_000_000_000;
    const m = buildTimelineOption(denseData(5000, [0, 6], from, to),
        { from, to, width: 1240 });
    const byCol = new Map();
    for (const s of m.option.series[0].data) {
        const k = s[2] + ':' + s[0];
        if (!byCol.has(k)) byCol.set(k, []);
        byCol.get(k).push(s);
    }
    assert.ok(byCol.size > 0);
    for (const segs of byCol.values()) {
        assert.equal(segs[0][4], 0, 'the stack starts at the band bottom');
        assert.equal(segs[segs.length - 1][5], 1, 'and ends at the band top');
        for (let i = 1; i < segs.length; i++) {
            assert.equal(segs[i][4], segs[i - 1][5], 'segments are contiguous');
            assert.ok(segs[i][3] > segs[i - 1][3], 'canonical class order bottom-up');
        }
    }
});

/* ONE banner line carries both facts (truncation + density), one number
 * format, one zoom instruction — two yellow lines over one chart read as two
 * problems. It also has to say what aggregation COSTS the reader. */
test('banner: one line stating truncation, density and what zooming restores', () => {
    const from = 0, to = 1_000_000_000;
    const d = denseData(5000, [0, 6], from, to);
    const m = buildTimelineOption({ ...d, truncated: true, total_count: 9812 },
        { from, to, width: 1240 });
    assert.equal(m.spansPerPx, 5);   // 5000 spans over 1120 painted columns
    assert.equal(m.bannerNote,
        'Showing 5,000 of 9,812 events, aggregated at 5 per px into per-pixel ' +
        'class shares — individual waits and their queries appear when you ' +
        'drag to zoom in.');
    assert.equal(m.bannerNote, timelineBannerNote(5000, 9812, true, 5));

    // Not truncated: no "of M", same sentence otherwise.
    const whole = buildTimelineOption(d, { from, to, width: 1240 });
    assert.equal(whole.bannerNote,
        'Showing 5,000 events, aggregated at 5 per px into per-pixel class ' +
        'shares — individual waits and their queries appear when you drag to zoom in.');

    // Truncated but NOT dense: one line, same number format, same instruction.
    const sparse = buildTimelineOption(
        { ...data(), truncated: true, total_count: 1200 }, { from: 0, to: 1000, width: 1240 });
    assert.equal(sparse.aggregated, false);
    assert.equal(sparse.bannerNote,
        'Showing 3 of 1,200 events — drag to zoom in for the rest of the window.');

    // Neither: no banner at all.
    assert.equal(buildTimelineOption(data(), { from: 0, to: 1000 }).bannerNote, null);
});

test('density banner reports the REAL per-pixel density of a burst', () => {
    const from = 0, to = 1_000_000_000;
    // 3036 spans inside 1/6th of the window: events/plotWidth would say 2.7.
    const burst = buildTimelineOption(
        denseData(3036, [0, 6], from, from + (to - from) / 6), { from, to, width: 1240 });
    assert.equal(burst.aggregated, true);
    assert.ok(burst.spansPerPx >= 16,
        'burst density is per painted pixel (got ' + burst.spansPerPx + ')');
    assert.ok(burst.bannerNote.startsWith('Showing 3,036 events, aggregated at '));
});

test('STABILITY: the same spans in a window shifted by whole columns translate', () => {
    const from = 0, to = 1_120_000_000;
    const cols = timelinePlotWidth(1240);            // 1120 columns
    const colNs = (to - from) / cols;                // 1 ms per column
    const shift = 10 * colNs;
    const a = buildTimelineOption(denseData(5000, [0, 6, 2], from, to),
        { from, to, width: 1240 });
    const b = buildTimelineOption(denseData(5000, [0, 6, 2], from + shift, to + shift),
        { from: from + shift, to: to + shift, width: 1240 });
    assert.equal(a.aggregated, true);
    assert.equal(b.aggregated, true);
    assert.equal(a.option.series[0].data.length, b.option.series[0].data.length);
    for (let i = 0; i < a.option.series[0].data.length; i++) {
        const sa = a.option.series[0].data[i], sb = b.option.series[0].data[i];
        assert.equal(sb[0] - sa[0], shift, 'column ' + i + ' translated by the shift');
        assert.equal(sa[2], sb[2]);
        assert.equal(sa[3], sb[3], 'same class in the same stack slot across ticks');
        assert.ok(Math.abs(sa[4] - sb[4]) < 1e-9 && Math.abs(sa[5] - sb[5]) < 1e-9,
            'same share across ticks');
    }
    assert.equal(a.bannerNote, b.bannerNote);
});

test('aggregated renderItem: stacked rect inside the 60% band, bottom-up', () => {
    // Band 40px tall centered at y=200 -> rect 24px tall spanning y 188..212.
    const seg = [100, 200, 0, 6, 0.25, 1, 3, 4, 75, 100];
    const api = { value: (i) => seg[i], coord: ([v]) => [v, 200], size: () => [0, 40] };
    const r = timelineAggRenderItem({}, api);
    assert.equal(r.type, 'rect');
    assert.equal(r.shape.x, 100);
    assert.equal(r.shape.width, 100);
    assert.equal(r.shape.height, 24 * 0.75);          // share of the band
    assert.equal(r.shape.y, 188);                     // top of the band (frac1 = 1)
    assert.equal(r.style.fill, WAIT_CLASSES[6].color);
    // The bottom segment of the same column abuts it exactly — the stack is a
    // proportion, so NOTHING is clamped to a minimum height (P11).
    const low = [100, 200, 0, 0, 0, 0.25, 1, 4, 25, 100];
    const r2 = timelineAggRenderItem({}, { ...api, value: (i) => low[i] });
    assert.equal(r2.shape.y, r.shape.y + r.shape.height, 'no gap between segments');
    assert.equal(r2.shape.y + r2.shape.height, 212, 'and the stack ends at the band floor');
    assert.equal(r2.shape.height, 24 * 0.25);
    const tiny = [100, 200, 0, 0, 0, 0.001, 1, 4, 1, 1000];
    assert.equal(timelineAggRenderItem({}, { ...api, value: (i) => tiny[i] }).shape.height,
        24 * 0.001, 'a 0.1% share stays 0.1% of the band, never clamped up');
});

/* The hover target for a 1 % class is a sub-pixel sliver, so hovering ANY
 * slice must describe the WHOLE column — otherwise the minority classes the
 * stack exists to preserve are unreadable in practice. */
test('aggregated tooltip lists every class in the column, marking the hovered one', () => {
    const breakdown = [[0, 12, 24_000_000], [6, 9, 75_000_000], [3, 1, 1_000_000]];
    const seg = [1_000_000_000, 1_001_000_000, 0, 6, 0.25, 1, 9, 22,
        75_000_000, 100_000_000, breakdown];
    const html = timelineAggTooltipFormatter({ data: seg });
    assert.ok(html.includes('<b>Aggregated column</b>'));
    assert.ok(html.includes(fmtTime(1_000_000_000, 1_000_000)), 'column start');
    assert.ok(html.includes(fmtTime(1_001_000_000, 1_000_000)), 'column end');
    assert.ok(html.includes('22 spans'), 'the column total');
    // Every class present, with share / wait / spans — including the 1 % one.
    assert.ok(html.includes('CPU') && html.includes('24.0%') && html.includes('12 spans'));
    assert.ok(html.includes('75.0%') && html.includes('9 spans'));
    assert.ok(html.includes('LWLock') && html.includes('1.0%') && html.includes('1 span<'),
        'a 1% class is readable from a neighbour\'s pixels');
    // The hovered class is the bolded one; its swatch is its palette color.
    assert.ok(html.includes('<b>Timeout</b>'));
    assert.ok(!html.includes('<b>CPU</b>'));
    for (const k of [0, 6, 3]) assert.ok(html.includes(WAIT_CLASSES[k].color));
    assert.ok(html.includes('Zoom in for individual waits'));
    // No query text at all in aggregated mode: a column mixes executions, so
    // untrusted SQL never reaches this path (UI-6).
    assert.ok(!html.includes('Query:'));
});

/* The tooltip's promise — hover any slice, read the whole column — only holds
 * if every segment of a column carries the SAME breakdown, complete and in
 * canonical order. Asserted on a BUILT option, by reference, so a future
 * refactor that rebuilds the array per segment (or emits a partial one) fails
 * here rather than in a DBA's tooltip. */
test('every segment of a column shares one complete breakdown, by reference', () => {
    const from = 0, to = 1_000_000_000;
    const m = buildTimelineOption(denseData(5000, [6, 0, 3], from, to),
        { from, to, width: 1240 });
    assert.equal(m.aggregated, true);
    const byCol = new Map();
    for (const s of m.option.series[0].data) {
        const k = s[2] + ':' + s[0];
        if (!byCol.has(k)) byCol.set(k, []);
        byCol.get(k).push(s);
    }
    assert.ok(byCol.size > 100);
    let multiClassColumns = 0;
    for (const segs of byCol.values()) {
        const breakdown = segs[0][10];
        if (segs.length > 1) multiClassColumns++;
        for (const s of segs) {
            assert.equal(s[10], breakdown, 'same array instance, not a copy');
        }
        // Complete: one entry per segment of the column, same classes.
        assert.equal(breakdown.length, segs.length);
        assert.deepEqual(breakdown.map(b => b[0]), segs.map(s => s[3]));
        // Canonical class order, and the per-class numbers agree with the
        // segment's own [classSpans, classWaitNs].
        for (let i = 0; i < breakdown.length; i++) {
            if (i) assert.ok(breakdown[i][0] > breakdown[i - 1][0], 'canonical order');
            assert.equal(breakdown[i][1], segs[i][6]);
            assert.equal(breakdown[i][2], segs[i][8]);
        }
        // ...and they sum to the column totals the segments carry.
        assert.equal(breakdown.reduce((a, b) => a + b[1], 0), segs[0][7]);
        assert.ok(Math.abs(breakdown.reduce((a, b) => a + b[2], 0) - segs[0][9]) < 1e-6);
    }
    assert.ok(multiClassColumns > 0, 'the fixture actually mixes classes per column');
});

test('aggregated tooltip survives a segment with no breakdown (defensive)', () => {
    const seg = [0, 100, 0, 6, 0, 1, 3, 3, 90, 90];
    const html = timelineAggTooltipFormatter({ data: seg });
    assert.ok(html.includes('<b>Timeout</b>'));
    assert.ok(html.includes('100.0%'));
});

test('aggregated option keeps the window axis, clipping and animation:false', () => {
    const from = 10_000, to = 1_000_010_000;
    const m = buildTimelineOption(denseData(5000, [0, 6], from, to),
        { from, to, width: 1240 });
    assert.equal(m.option.xAxis.min, from);
    assert.equal(m.option.xAxis.max, to);
    assert.equal(m.option.series[0].clip, true);
    assert.equal(m.option.animation, false);
    assert.equal(m.option.series[0].type, 'custom');
    assert.deepEqual(m.option.series[0].encode, { x: [0, 1], y: 2 });
    assert.equal(m.option.tooltip.formatter, timelineAggTooltipFormatter);
    assert.equal(m.count, 5000, 'count still reports SPANS, not columns');
});

test('sub-threshold builds are untouched: per-span renderItem and tooltip', () => {
    const m = buildTimelineOption(data(), { from: 0, to: 1000, width: 1240 });
    assert.equal(m.aggregated, false);
    assert.equal(m.bannerNote, null);
    assert.equal(m.spansPerPx, 0);
    assert.equal(m.option.series[0].renderItem, timelineRenderItem);
    assert.equal(m.option.tooltip.formatter, timelineTooltipFormatter);
});

test('aggregation needs a real window: no from/to means no aggregation', () => {
    const d = denseData(5000, [0, 6], 0, 1_000_000_000);
    assert.equal(buildTimelineOption(d, { width: 1240 }).aggregated, false);
    assert.equal(buildTimelineOption(d, { from: 5, to: 5, width: 1240 }).aggregated, false);
});
