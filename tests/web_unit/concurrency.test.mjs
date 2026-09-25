/* Node unit tests for the pure concurrency builders (lib/builders/concurrency.js).
 *
 * Runs under `node --test`. Proves the concurrency data -> ECharts line option +
 * burst markers, and the top-peaks / burst HTML tables: peak series mapping,
 * burst marker placement (containing bucket, clamped), top-10-by-max ordering,
 * and empty-input handling.
 */
import { test } from 'node:test';
import assert from 'node:assert/strict';
import {
    buildConcurrencyOption, buildConcurrencyTables,
} from '../../web/static/lib/builders/concurrency.js';

function fivePeaks() {
    // bucket_ns 1000, t = 1000..5000 — a clean 5-bucket window for the
    // captureFromNs boundary math.
    return {
        bucket_ns: 1_000,
        peaks: [
            { t: 1000, t_ms: 1, max: 3, event: 'A' },
            { t: 2000, t_ms: 2, max: 4, event: 'B' },
            { t: 3000, t_ms: 3, max: 5, event: 'C' },
            { t: 4000, t_ms: 4, max: 2, event: 'D' },
            { t: 5000, t_ms: 5, max: 1, event: 'E' },
        ],
        bursts: [],
    };
}

function data() {
    return {
        // bucket_ns matches the peak t spacing (peak t values are bucket
        // STARTS) — the containing-bucket arithmetic depends on it.
        bucket_ns: 1_000,
        peaks: [
            { t: 1000, t_ms: 1, max: 2, event: 'A' },
            { t: 2000, t_ms: 2, max: 8, event: 'B' },
            { t: 3000, t_ms: 3, max: 5, event: 'C' },
        ],
        bursts: [
            { timestamp_ns: 2500, timestamp_ms: 2, event: 'B', sessions: 8,
              pids: [1, 2, 3, 4, 5, 6, 7, 8, 9, 10] },
        ],
    };
}

test('line series maps per-bucket peak max; area + symbol-none', () => {
    const { option, hasData } = buildConcurrencyOption(data());
    assert.equal(hasData, true);
    assert.equal(option.series[0].type, 'line');
    assert.deepEqual(option.series[0].data, [2, 8, 5]);
    assert.ok(option.series[0].areaStyle);
    assert.equal(option.series[0].symbol, 'none');
    assert.deepEqual(option.xAxis.data, [1000, 2000, 3000]);
});

/* FLIPPED in U0 (P6): this test used to pin the findIndex(p.t >= ts) placement,
 * which is off-by-one — timestamp 2500 landed at index 2 (t=3000), the bucket
 * AFTER the containing one. The marker now sits in the CONTAINING bucket. */
test('burst marker placed in the containing bucket', () => {
    const { option } = buildConcurrencyOption(data());
    const mp = option.series[0].markPoint.data;
    assert.equal(mp.length, 1);
    // timestamp_ns 2500 lies in bucket [2000, 3000) -> index 1
    assert.equal(mp[0].coord[0], 1);
    assert.equal(mp[0].coord[1], 8);       // y = the containing bucket's peak
    assert.equal(mp[0].value, 8);
    assert.equal(mp[0].symbol, 'triangle');
});

/* P6 regression: a burst inside the FINAL bucket has no peak with t >= ts, so
 * the old findIndex returned -1 and Math.max clamped the marker to bucket 0 —
 * a far-left triangle for exactly the burst you care about most (the latest). */
test('burst inside the final bucket -> marker at the last bucket, not bucket 0', () => {
    const d = data();
    // 3350 lies in the last bucket [3000, 4000): no peak t >= 3350.
    d.bursts = [{ timestamp_ns: 3350, timestamp_ms: 3, event: 'C', sessions: 4,
                  pids: [1, 2, 3, 4] }];
    const { option } = buildConcurrencyOption(d);
    const mp = option.series[0].markPoint.data;
    assert.equal(mp[0].coord[0], 2);       // last bucket (n-1), not 0
    assert.equal(mp[0].coord[1], 5);       // y = that bucket's peak, not sessions
});

test('option root disables animation (U0: no replayed draw-in on refresh)', () => {
    assert.equal(buildConcurrencyOption(data()).option.animation, false);
});

test('no bursts -> markPoint omitted', () => {
    const { option } = buildConcurrencyOption({ ...data(), bursts: [] });
    assert.equal(option.series[0].markPoint, undefined);
});

test('empty peaks -> hasData false, empty tables/topPeaks', () => {
    const m = buildConcurrencyOption({ peaks: [] });
    assert.equal(m.hasData, false);
    assert.equal(m.option, null);
    assert.deepEqual(m.topPeaks, []);
});

test('topPeaks: only max>1, sorted desc, capped at 10', () => {
    const peaks = [{ t: 0, t_ms: 0, max: 1, event: 'x' }]   // dropped (max=1)
        .concat(Array.from({ length: 15 }, (_, i) =>
            ({ t: i + 1, t_ms: i + 1, max: i + 2, event: 'e' + i })));
    const m = buildConcurrencyOption({ peaks, bursts: [], bucket_ns: 1 });
    assert.equal(m.topPeaks.length, 10);
    assert.equal(m.topPeaks[0].max, 16);                 // largest first
    assert.ok(m.topPeaks.every(p => p.max > 1));
});

test('tables HTML: top-peaks + burst sections, PID truncation at 8', () => {
    const m = buildConcurrencyOption(data());
    const html = buildConcurrencyTables(m);
    assert.ok(html.includes('Top Peak Moments'));
    assert.ok(html.includes('Burst Events'));
    assert.ok(html.includes('<b>8</b>'));               // burst sessions
    // 10 pids -> show first 8 then ellipsis
    assert.ok(html.includes('1, 2, 3, 4, 5, 6, 7, 8...'));
});

/* U2 / P3 wire 2: peak + burst rows are zoom intents. The pure builder embeds
 * the target window (ts ± 5×bucket_ns, ns) as data-from/data-to; the view
 * delegates a click straight into ctx.onZoom. Bursts are 4+ sessions in 10ms —
 * unreachable by drag-select at ~1s/pixel, so the row is the only way in. */
test('rows carry the ±5-bucket zoom window as data-from/data-to', () => {
    const m = buildConcurrencyOption(data());
    const html = buildConcurrencyTables(m);
    // Burst at timestamp_ns 2500, bucket 1000 -> pad 5000 -> [-2500, 7500].
    assert.ok(html.includes('data-from="-2500" data-to="7500"'));
    // Top peak row t=2000 (max 8) -> [-3000, 7000].
    assert.ok(html.includes('data-from="-3000" data-to="7000"'));
    // Affordance: pointer cursor + .row-zoom hook + a title.
    assert.ok(html.includes('class="row-zoom"'));
    assert.ok(html.includes('cursor:pointer'));
    assert.ok(html.includes('title="Zoom to'));
});

test('rows are inert (no zoom attributes) when bucket_ns is missing', () => {
    // Without a bucket width there is no honest pad — emit plain rows rather
    // than a zero-width zoom target.
    const d = { ...data(), bucket_ns: undefined };
    const html = buildConcurrencyTables(buildConcurrencyOption(d));
    assert.ok(!html.includes('data-from'));
    assert.ok(!html.includes('row-zoom'));
});

test('tables HTML: no bursts -> explicit "No burst events" line', () => {
    const m = buildConcurrencyOption({ ...data(), bursts: [] });
    const html = buildConcurrencyTables(m);
    assert.ok(html.includes('No burst events detected'));
});

/* #105: y-axis title geometry. Default nameLocation clipped the leading "S"
 * of "Simultaneous Sessions" off the canvas at 1280px (the name text is
 * anchored past the axis start under the default 'end' location). middle +
 * a rotated strip inside grid.left is the same fix shape as the matrix axis
 * names (PR #147) — pinned here so the margin can never regress under it. */
test('y-axis title uses nameLocation middle with a gap that fits inside grid.left', () => {
    const { option } = buildConcurrencyOption(fivePeaks());
    assert.equal(option.yAxis.name, 'Simultaneous Sessions');
    assert.equal(option.yAxis.nameLocation, 'middle');
    assert.ok(option.yAxis.nameGap > 0);
    assert.ok(option.yAxis.nameGap < option.grid.left,
        'yAxis name must stay inside the left grid margin, not spill onto the plot');
});

/* #105: a bucket that starts before capture began is EXCLUDED (null), never
 * painted as a measured "0 sessions" — the old `p.max || 0` fallback drew a
 * solid zero line across 12 real minutes that were never captured. */
test('captureFromNs: buckets before it are null in the series, not 0', () => {
    const d = fivePeaks();
    // Buckets at t=1000,2000 (< 2500) are pre-capture; t=3000.. are real.
    const { option, notCapturedCount } = buildConcurrencyOption(d, { captureFromNs: 2500 });
    assert.equal(notCapturedCount, 2);
    assert.deepEqual(option.series[0].data, [null, null, 5, 2, 1]);
});

test('captureFromNs omitted or before the window start -> no exclusion, no markArea', () => {
    const d = fivePeaks();
    const bare = buildConcurrencyOption(d);
    assert.equal(bare.notCapturedCount, 0);
    assert.equal(bare.option.series[0].markArea, undefined);
    assert.deepEqual(bare.option.series[0].data, [3, 4, 5, 2, 1]);

    const before = buildConcurrencyOption(d, { captureFromNs: 0 });
    assert.equal(before.notCapturedCount, 0);
    assert.equal(before.option.series[0].markArea, undefined);
});

test('captureFromNs: markArea covers exactly the not-captured buckets and is labeled', () => {
    const d = fivePeaks();
    const { option } = buildConcurrencyOption(d, { captureFromNs: 3500 });
    // t=1000,2000,3000 < 3500 -> 3 not-captured buckets, indices 0..2.
    const area = option.series[0].markArea;
    assert.ok(area, 'markArea must be present when a pre-capture span exists');
    assert.equal(area.data[0][0].xAxis, 0);
    assert.equal(area.data[0][1].xAxis, 2);
    assert.equal(area.label.formatter, 'Not captured');
});

test('captureFromNs: tooltip reads "Not yet captured" for a pre-capture bucket, never a peak value', () => {
    const d = fivePeaks();
    const { option } = buildConcurrencyOption(d, { captureFromNs: 2500 });
    const tip0 = option.tooltip.formatter([{ dataIndex: 0, axisValue: 1000, value: null }]);
    assert.ok(tip0.includes('Not yet captured'));
    assert.ok(!tip0.includes('Peak:'));
    const tip2 = option.tooltip.formatter([{ dataIndex: 2, axisValue: 3000, value: 5 }]);
    assert.ok(tip2.includes('Peak: <b>5 sessions</b>'));
});

test('captureFromNs: whole window predates capture -> every bucket null, still hasData (no crash)', () => {
    const d = fivePeaks();
    const { option, hasData, notCapturedCount } =
        buildConcurrencyOption(d, { captureFromNs: 9999 });
    assert.equal(hasData, true);
    assert.equal(notCapturedCount, 5);
    assert.deepEqual(option.series[0].data, [null, null, null, null, null]);
});

/* #105: a not-captured bucket can never be a "Top Peak Moment" — the chart
 * excludes it from the line, so the table below it must agree, or the two
 * halves of the same view contradict each other. */
test('captureFromNs: a pre-capture bucket is excluded from topPeaks even if its raw max > 1', () => {
    const d = fivePeaks(); // maxes: 3,4,5,2,1 — index 1 (max=4) would normally top the list
    const { topPeaks } = buildConcurrencyOption(d, { captureFromNs: 2500 }); // excludes idx 0,1
    assert.ok(topPeaks.every(p => p.t >= 3000));
    assert.equal(topPeaks[0].max, 5);          // idx 2, the largest CAPTURED peak
    assert.equal(topPeaks.some(p => p.max === 4), false);
});
