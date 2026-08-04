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
