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
    burstSelectionNote, burstPidSampleMarker,
    burstNoiseNote, BURST_NOTABLE_FRACTION, BURST_NOTABLE_MIN_BUCKETS,
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

/* #105 (review follow-up): a burst can only ever originate from a CAPTURED
 * bucket — one whose containing bucket lands in the not-captured prefix is a
 * self-contradiction (`peakData[at] || b.sessions` used to paint a value
 * straight into the "Not captured" band). It must be dropped from BOTH the
 * chart's markPoint and the Burst Events table, the same way topPeaks
 * excludes pre-capture entries. */
test('captureFromNs: a burst in the not-captured prefix is dropped from markPoint and the bursts table; a captured one survives', () => {
    const d = fivePeaks();
    d.bursts = [
        // t=1500 -> bucket 0 (t=1000..2000): pre-capture under captureFromNs=2500.
        { timestamp_ns: 1500, timestamp_ms: 1, event: 'A', sessions: 4, pids: [1, 2, 3, 4] },
        // t=3500 -> bucket 2 (t=3000..4000): captured.
        { timestamp_ns: 3500, timestamp_ms: 3, event: 'C', sessions: 6, pids: [1, 2, 3, 4, 5, 6] },
    ];
    const { option, bursts, notCapturedCount } =
        buildConcurrencyOption(d, { captureFromNs: 2500 });
    assert.equal(notCapturedCount, 2);
    // markPoint: only the captured burst (sessions=6) survives.
    const mp = option.series[0].markPoint.data;
    assert.equal(mp.length, 1);
    assert.equal(mp[0].value, 6);
    assert.equal(mp[0].coord[0], 2);
    // Burst Events table model: same exclusion.
    assert.equal(bursts.length, 1);
    assert.equal(bursts[0].sessions, 6);
});

test('captureFromNs: every burst pre-capture -> markPoint omitted entirely, bursts table empty', () => {
    const d = fivePeaks();
    d.bursts = [{ timestamp_ns: 1500, timestamp_ms: 1, event: 'A', sessions: 4,
        pids: [1, 2, 3, 4] }];
    const { option, bursts } = buildConcurrencyOption(d, { captureFromNs: 2500 });
    assert.equal(option.series[0].markPoint, undefined);
    assert.deepEqual(bursts, []);
});

/* ── #276: the two declared bounds the response can still carry ────────────
 *
 * The server detects every burst onset and returns the largest in each bucket,
 * with `bursts_total` = onsets detected; a burst's `pids` list is a 64-pid
 * sample of an exact `sessions` count. Both must be VISIBLE, and — equally
 * important — must render exactly as before when there is nothing to declare,
 * or when an older server omits the fields. A note that always shows is a note
 * nobody reads.
 */
function burstData(extra) {
    return Object.assign({
        bucket_ns: 1_000,
        peaks: [
            { t: 1000, t_ms: 1, max: 4, event: 'A' },
            { t: 2000, t_ms: 2, max: 9, event: 'B' },
        ],
        bursts: [
            { timestamp_ns: 1500, timestamp_ms: 1, event: 'A', sessions: 4,
              pids: [1, 2, 3, 4] },
            { timestamp_ns: 2500, timestamp_ms: 2, event: 'B', sessions: 9,
              pids: [1, 2, 3, 4, 5, 6, 7, 8, 9] },
        ],
    }, extra || {});
}

test('#276 note: the bucket maxima shown are declared against the onsets detected', () => {
    const d = burstData({ bursts_total: 1284, bursts_truncated: true });
    const model = buildConcurrencyOption(d);
    assert.equal(model.burstNote,
        'Showing the largest burst in each of 2 buckets, out of 1,284 burst ' +
        'onsets detected across this window.');
    const html = buildConcurrencyTables(model);
    assert.ok(html.includes('class="chart-notes"'));
    assert.ok(html.includes('1,284 burst onsets detected'));
});

test('#276 note: a single bucket reads as a sentence, not "1 buckets"', () => {
    const d = burstData({ bursts_total: 40 });
    d.bursts = [d.bursts[0]];
    assert.equal(buildConcurrencyOption(d).burstNote,
        'Showing the largest burst in each of 1 bucket, out of 40 burst ' +
        'onsets detected across this window.');
});

/* The false-negative side: every way the note could appear when there is
 * nothing to declare, which is how a panel note stops being read. */
test('#276 note: absent, zero, or complete -> no note, panel renders as before', () => {
    // Older server: no bursts_total at all.
    assert.equal(buildConcurrencyOption(burstData()).burstNote, '');
    // Nothing detected.
    assert.equal(buildConcurrencyOption(burstData({ bursts_total: 0 })).burstNote, '');
    // Every onset is on screen: 2 shown, 2 detected.
    assert.equal(buildConcurrencyOption(burstData({ bursts_total: 2 })).burstNote, '');
    // A server contradicting itself (fewer onsets than markers) says nothing
    // rather than printing "2 of 1".
    assert.equal(buildConcurrencyOption(burstData({ bursts_total: 1 })).burstNote, '');
    // Non-numeric field (garbled response): ignored, never rendered raw.
    assert.equal(burstSelectionNote({ bursts_total: 'lots' }, 2), '');
    assert.equal(burstSelectionNote(null, 2), '');
    // The empty <div> collapses via .chart-notes:empty, so a response with
    // nothing to declare carries no text at all.
    const html = buildConcurrencyTables(buildConcurrencyOption(burstData()));
    assert.ok(html.includes('<div class="chart-notes"></div>'));
    assert.ok(!html.includes('onsets detected'));
});

/* The note counts what is RENDERED, not what the server sent: #105 drops
 * bursts landing in the not-captured prefix, and a note claiming 2 buckets
 * beside a single row would be a new lie in place of the old one. */
test('#276 note: counts the bursts actually shown, after the not-captured filter', () => {
    const d = burstData({ bursts_total: 99 });
    const model = buildConcurrencyOption(d, { captureFromNs: 2000 });
    assert.equal(model.bursts.length, 1);
    assert.ok(model.burstNote.startsWith(
        'Showing the largest burst in each of 1 bucket,'));
});

test('#276 badge: a sampled PID list is marked on the row; a complete one is not', () => {
    const pids = Array.from({ length: 64 }, (_, i) => 1000 + i);
    const d = burstData({ bursts_total: 7 });
    d.bursts = [{ timestamp_ns: 1500, timestamp_ms: 1, event: 'A',
        sessions: 97, pids }];
    const html = buildConcurrencyTables(buildConcurrencyOption(d));
    assert.ok(html.includes('(64 of 97)'), 'row states the sample size');
    assert.ok(html.includes('<b>97</b>'), 'and still reports the exact count');
    // Complete list: no badge at all.
    assert.equal(burstPidSampleMarker({ sessions: 4, pids: [1, 2, 3, 4] }), '');
    // Exactly at the sample bound but complete: still no badge.
    assert.equal(burstPidSampleMarker({ sessions: 64, pids }), '');
});

test('#276 badge: missing or malformed fields render exactly as before', () => {
    assert.equal(burstPidSampleMarker(null), '');
    assert.equal(burstPidSampleMarker({ sessions: 9 }), '');               // no pids
    assert.equal(burstPidSampleMarker({ pids: [1, 2] }), '');              // no count
    assert.equal(burstPidSampleMarker({ sessions: '9', pids: [1] }), '');  // not a number
});

// ── review finding (separate from the owner-held #442 threshold): the 640px
// scroll cap used to wrap BOTH tables in one container, so Burst Events never
// reached the fold on a dense fixture. buildPeaksPanelHtml/buildBurstsPanelHtml
// are the two panels views/concurrency.js now mounts into separate, independently
// scrollable containers -- these tests prove each is TRULY standalone (not just
// a cosmetic split that still concatenates the other's content internally) and
// that the split preserves every row, including the exact zoom range of the
// LAST one, over a dense (300-bucket, 300-burst) fixture. ─────────────────────

import { buildPeaksPanelHtml, buildBurstsPanelHtml } from '../../web/static/lib/builders/concurrency.js';

function denseBurstData() {
    // 300 buckets, 1s apart; EVERY bucket has a burst (not just a sampled
    // few) -- the "300 bursts" dense scenario the gate names. max cycles so
    // more than 10 buckets qualify for the top-10 peaks table.
    const bucketNs = 1_000_000_000;
    const base = 1_700_000_000_000_000_000;
    const peaks = [];
    const bursts = [];
    for (let i = 0; i < 300; i++) {
        const t = base + i * bucketNs;
        const max = 2 + (i % 20);   // plenty of distinct values > 1
        peaks.push({ t, t_ms: Math.floor(t / 1e6), max, event: 'LWLock:BufferMapping' });
        const ts = t + 500_000_000;
        bursts.push({ timestamp_ns: ts, timestamp_ms: Math.floor(ts / 1e6),
            event: 'IO:DataFileRead', sessions: 4 + (i % 6),
            pids: [1000 + i, 1001 + i, 1002 + i, 1003 + i] });
    }
    return { bucket_ns: bucketNs, peaks, bursts, bursts_total: 300, bursts_truncated: false };
}

test('#442-review: buildPeaksPanelHtml carries no <h3> Burst Events heading (true separation)', () => {
    const model = buildConcurrencyOption(denseBurstData());
    const html = buildPeaksPanelHtml(model);
    assert.ok(html.includes('<h3'), html.slice(0, 200));
    assert.ok(html.includes('Top Peak Moments'), html.slice(0, 200));
    // TOP_PEAKS_CAPTION's text legitimately CROSS-REFERENCES "Burst Events"
    // in prose ("...listed separately under Burst Events") -- the thing that
    // must be absent is the OTHER panel's <h3> HEADING, not the phrase.
    assert.equal((html.match(/<h3/g) || []).length, 1,
        'peaks panel must carry exactly its own heading, not the bursts one too');
    assert.ok(!/<h3[^>]*>Burst Events/.test(html),
        'peaks panel must not carry the bursts heading');
    // Exactly the top 10 by max, as the model already guarantees. One extra
    // <tr> is the <thead> header row, hence -1.
    assert.equal((html.match(/<tr/g) || []).length - 1, 10);
});

test('#442-review: buildBurstsPanelHtml never mentions Top Peak Moments (true separation)', () => {
    const model = buildConcurrencyOption(denseBurstData());
    const html = buildBurstsPanelHtml(model);
    assert.ok(html.includes('Burst Events'), html.slice(0, 200));
    assert.ok(!html.includes('Top Peak Moments'), 'bursts panel must not carry the peaks heading');
});

test('#442-review: every one of 300 bursts survives the split, none dropped', () => {
    const model = buildConcurrencyOption(denseBurstData());
    assert.equal(model.bursts.length, 300);
    const html = buildBurstsPanelHtml(model);
    // -1 for the <thead> header row.
    assert.equal((html.match(/<tr/g) || []).length - 1, 300,
        'the burst table must render all 300 rows, not a 640px-shaped subset');
});

test('#442-review: the LAST burst row carries its exact zoom range', () => {
    const data = denseBurstData();
    const model = buildConcurrencyOption(data);
    const html = buildBurstsPanelHtml(model);
    const last = data.bursts[data.bursts.length - 1];
    const pad = 5 * model.bucketNs;
    const expectedFrom = last.timestamp_ns - pad;
    const expectedTo = last.timestamp_ns + pad;
    assert.ok(html.includes('data-from="' + expectedFrom + '" data-to="' + expectedTo + '"'),
        `expected exact zoom range for the final burst (from=${expectedFrom}, to=${expectedTo}) ` +
        `not found in rendered HTML`);
});

test('#442-review: buildConcurrencyTables (combined) is still exactly the two panels concatenated', () => {
    const model = buildConcurrencyOption(denseBurstData());
    assert.equal(buildConcurrencyTables(model),
        buildPeaksPanelHtml(model) + buildBurstsPanelHtml(model));
});

/* ── #442: burst markers must stop firing on (near-)every bucket ──────────
 *
 * The owner's diagnosis (issue #442 comment) requires the fix to be gated
 * BOTH ways: a quiet/uniform fixture where the OLD absolute threshold marks
 * every bucket must now show NO chart markers (the bar this whole fix
 * exists to clear), and a genuinely isolated burst must NOT be collaterally
 * suppressed just because the relative rule exists. Neither assertion was
 * even expressible before burstNoiseNote/burstsMarkersSuppressed existed —
 * see the PR description for the full revert-and-rerun RED transcript.
 */
function manyBucketsData(n, burstBucketIndices) {
    const bucketNs = 1_000;
    const marked = new Set(burstBucketIndices);
    const peaks = [];
    const bursts = [];
    for (let i = 0; i < n; i++) {
        const t = (i + 1) * bucketNs;
        peaks.push({ t, t_ms: t, max: 5, event: 'LWLock:BufferMapping' });
        if (marked.has(i)) {
            const ts = t + 500;
            bursts.push({ timestamp_ns: ts, timestamp_ms: ts, event: 'IO:DataFileRead',
                sessions: 4, pids: [1, 2, 3, 4] });
        }
    }
    return { bucket_ns: bucketNs, peaks, bursts };
}

test('#442: a uniform fixture (every captured bucket bursts) draws NO chart markers', () => {
    const d = manyBucketsData(20, Array.from({ length: 20 }, (_, i) => i));
    const model = buildConcurrencyOption(d);
    assert.equal(model.option.series[0].markPoint, undefined,
        'the old behavior (a marker on every bucket) must be gone');
    assert.equal(model.burstsMarkersSuppressed, true);
    // The table is NOT silently emptied -- #276's "must all stay reachable"
    // still governs every burst the server actually detected.
    assert.equal(model.bursts.length, 20);
    assert.ok(model.burstNote.includes('fired in 20 of 20 captured buckets'),
        model.burstNote);
    const html = buildConcurrencyTables(model);
    assert.ok(html.includes('fired in 20 of 20 captured buckets'));
});

test('#442: an isolated burst (minority of buckets) still draws its marker', () => {
    const d = manyBucketsData(20, [10]);
    const model = buildConcurrencyOption(d);
    const mp = model.option.series[0].markPoint.data;
    assert.equal(mp.length, 1, 'the one real burst must not be suppressed');
    assert.equal(model.burstsMarkersSuppressed, false);
    assert.equal(model.bursts.length, 1);
    assert.ok(!model.burstNote.includes('Chart burst markers hidden'), model.burstNote);
});

test('#442: just over the minority line (11 of 20) suppresses; just under (10 of 20) does not', () => {
    // BURST_NOTABLE_FRACTION is 0.5 -- the boundary is "more than half", so
    // an exact half (10/20) must NOT suppress and 11/20 must.
    assert.equal(BURST_NOTABLE_FRACTION, 0.5);
    const half = buildConcurrencyOption(manyBucketsData(20, Array.from({ length: 10 }, (_, i) => i)));
    assert.equal(half.burstsMarkersSuppressed, false);
    assert.equal(half.option.series[0].markPoint.data.length, 10);
    const overHalf = buildConcurrencyOption(manyBucketsData(20, Array.from({ length: 11 }, (_, i) => i)));
    assert.equal(overHalf.burstsMarkersSuppressed, true);
    assert.equal(overHalf.option.series[0].markPoint, undefined);
});

test('#442: tiny windows (below BURST_NOTABLE_MIN_BUCKETS) never suppress, even at 100%', () => {
    assert.equal(BURST_NOTABLE_MIN_BUCKETS, 10);
    const d = manyBucketsData(3, [0, 1, 2]);   // 100% of buckets burst, but only 3 of them
    const model = buildConcurrencyOption(d);
    assert.equal(model.burstsMarkersSuppressed, false);
    assert.equal(model.option.series[0].markPoint.data.length, 3);
});

test('#442: burstNoiseNote is pure and false-negative-safe (absent inputs -> no note)', () => {
    assert.equal(burstNoiseNote(0, 0), '');
    assert.equal(burstNoiseNote(20, 0), '');            // no bursts at all
    assert.equal(burstNoiseNote(5, 5), '');              // below the bucket-count floor
    assert.equal(burstNoiseNote(20, 10), '');            // exactly half: not a majority
    assert.ok(burstNoiseNote(20, 11).includes('fired in 11 of 20 captured buckets'));
});

test('#442: captureFromNs shrinks the denominator to CAPTURED buckets only', () => {
    // 30 buckets total, the first 15 pre-capture (captureFromNs excludes
    // them) -- 15 captured buckets remain (over BURST_NOTABLE_MIN_BUCKETS),
    // 12 of which burst: 12/15 = 80%, over the line even though it is
    // 12/30 = 40% of the RAW window (which would NOT suppress).
    const d = manyBucketsData(30, Array.from({ length: 12 }, (_, i) => 15 + i));
    const model = buildConcurrencyOption(d, { captureFromNs: 16_000 });
    assert.equal(model.notCapturedCount, 15);
    assert.equal(model.burstsMarkersSuppressed, true);
    assert.ok(model.burstNote.includes('fired in 12 of 15 captured buckets'), model.burstNote);
});
