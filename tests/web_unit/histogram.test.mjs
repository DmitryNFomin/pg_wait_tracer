/* Node unit tests for the pure histogram builders (lib/builders/histogram.js).
 *
 * Runs under `node --test` — no framework, no browser, no network. Proves the
 * heatmap data -> ECharts option mapping and the class/event selector model so a
 * dropped cell, a wrong axis, or a mis-grouped selector is caught in
 * milliseconds without Playwright.
 */
import { test } from 'node:test';
import assert from 'node:assert/strict';
import { buildHeatmapOption, buildSelectorModel } from '../../web/static/lib/builders/histogram.js';
import { nextStickyMax } from '../../web/static/views/histogram.js';

const fmtCount = (n) => String(n);

function heatmap() {
    return {
        bucket_ns: 1_000_000_000,
        max_count: 500,
        times: [1000, 2000, 3000],
        labels: ['<1', '1-2', '2-4'],
        cells: [[0, 0, 100], [1, 2, 500], [2, 1, 250]],
    };
}

test('heatmap option: category axes from times/labels, cells carry log1p + raw', () => {
    const { option, hasData } = buildHeatmapOption(heatmap());
    assert.equal(hasData, true);
    assert.equal(option.xAxis.type, 'category');
    assert.equal(option.yAxis.type, 'category');
    assert.equal(option.xAxis.data.length, 3);            // 3 time labels
    assert.deepEqual(option.yAxis.data, ['<1', '1-2', '2-4']);
    assert.equal(option.series[0].type, 'heatmap');
    // P8: dimension 2 = log1p(count) (what color maps), dimension 3 = the raw
    // count (what the tooltip shows).
    assert.deepEqual(option.series[0].data, [
        [0, 0, Math.log1p(100), 100],
        [1, 2, Math.log1p(500), 500],
        [2, 1, Math.log1p(250), 250],
    ]);
});

/* FLIPPED in U2 (P8): this test used to pin the pre-U2 shape — a LINEAR
 * visualMap 0→max_count over a 6-stop blue/green/red/orange/yellow rainbow.
 * That shape was the bug: log2 latency bands make heavy-tailed counts
 * structural, so one hot cell drove max_count and every other cell collapsed
 * into the two darkest blues, while the rainbow hues collided with 5 of the
 * semantic class hues (a red cell read as "Lock"). Color now maps
 * log1p(count) through a single-hue violet ramp (monotone lightness — reads
 * only as "more", never as an identity). */
test('heatmap visualMap maps log1p(count) through the single-hue violet ramp', () => {
    const { option } = buildHeatmapOption(heatmap());
    assert.equal(option.visualMap.min, 0);
    assert.equal(option.visualMap.max, Math.log1p(500));
    assert.equal(option.visualMap.dimension, 2);          // the log dimension
    assert.deepEqual(option.visualMap.inRange.color,
        ['#763e84', '#a04ab5', '#bb6ecf', '#d496e3', '#eac3f4']);
});

test('heatmap sticky max: opts.maxCount overrides the response max (P8)', () => {
    // The view pins the visualMap ceiling across live ticks (recomputed on
    // window/filter change only) — the builder must honor the override.
    const { option } = buildHeatmapOption(heatmap(), { maxCount: 800 });
    assert.equal(option.visualMap.max, Math.log1p(800));
    // The response's own max is still reported (the view's sticky-max input).
    assert.equal(buildHeatmapOption(heatmap()).rawMax, 500);
});

test('sticky max carries only across one forward live tick at the same span', () => {
    const first = nextStickyMax(null, 500,
        { span: 900e9, anchor: 100e9, filters: '{"class":"IO"}', live: true });
    const tick = nextStickyMax(first, 200,
        { span: 900e9, anchor: 105e9, filters: '{"class":"IO"}', live: true });
    assert.equal(tick.value, 500, 'one 5 s forward tick keeps the ceiling');
    assert.notEqual(tick.key, first.key, 'the window anchor is part of the key');
    const pan = nextStickyMax(tick, 100,
        { span: 900e9, anchor: 205e9, filters: '{"class":"IO"}', live: false });
    assert.equal(pan.value, 100, 'same-span jump recomputes');
    const backward = nextStickyMax(pan, 80,
        { span: 900e9, anchor: 200e9, filters: '{"class":"IO"}', live: false });
    assert.equal(backward.value, 80, 'backward pan recomputes');
    const changedFilter = nextStickyMax(backward, 60,
        { span: 900e9, anchor: 205e9, filters: '{}', live: true });
    assert.equal(changedFilter.value, 60, 'filter change recomputes');
});

test('heatmap tooltip + visualMap labels stay in RAW counts, never log values', () => {
    const { option } = buildHeatmapOption(heatmap());
    // Tooltip reads dimension 3 (raw), not the log dimension.
    const tip = option.tooltip.formatter({ data: [1, 2, Math.log1p(500), 500] });
    assert.ok(tip.includes('Count: <b>500</b>'));
    assert.ok(!tip.includes('6.2'));                      // no log1p leakage
    // visualMap labels map the log value back to a raw count.
    assert.equal(option.visualMap.formatter(Math.log1p(500)), '500');
    assert.equal(option.visualMap.formatter(0), '0');
});

test('option root disables animation (U0: no replayed draw-in on refresh)', () => {
    assert.equal(buildHeatmapOption(heatmap()).option.animation, false);
});

/* ── The Histogram-blink gate (single-frame paint) ──────────────────────────
 *
 * ECharts' GLOBAL defaults are progressive: 400 / progressiveThreshold: 3000
 * (vendor echarts.min.js 5.6.0: `progressive:400` + `progressiveThreshold:3e3`
 * in the global default block, and Scheduler.restorePipelines sets
 * `progressiveEnabled: seriesModel.getProgressive() && ...`). So a heatmap
 * series that does NOT pin `progressive` paints a >=3000-cell grid in
 * ceil(n/400) chunks, one per animation frame, and the view's mount() hands
 * the sweep anchor out after chunk 1 — the panel is still filling for several
 * hundred ms after the frame the gate treats as settled. A demo-length window
 * reaches ~4500-5000 cells, so this is the normal regime, not an edge case.
 *
 * `progressive: 0` disables chunking: one setOption -> one paint.
 *
 * REFUSAL, not approval: this gate's subject is the heatmap-typed series that
 * actually carries cells. assertSinglePassPaint throws when there is no such
 * series (empty/absent response, or a reorder that drops it), so the check can
 * never be satisfied by the thing it checks being ABSENT rather than right --
 * which is what a bare `if (option) ...` or a truthiness assertion
 * (`assert(!series.progressive)`, true for undefined) would do. The two tests
 * after this one exercise exactly those bypasses. */
function assertSinglePassPaint(res, label) {
    assert.equal(res.hasData, true, label + ': expected a built option, got hasData=' + res.hasData);
    assert.ok(res.option && Array.isArray(res.option.series), label + ': no series array to check');
    const withCells = res.option.series.filter(
        s => s.type === 'heatmap' && Array.isArray(s.data) && s.data.length > 0);
    assert.equal(withCells.length, 1, label + ': expected exactly 1 cell-carrying heatmap series, got ' + withCells.length);
    for (const s of withCells) {
        // strictEqual, not truthiness: `undefined` (the ECharts default, i.e.
        // progressive: 400) must FAIL here, and it is the only value this bug
        // ever had.
        assert.strictEqual(s.progressive, 0,
            label + ': heatmap series must pin progressive: 0, got ' + String(s.progressive));
    }
    return withCells[0];
}

/* A grid past ECharts' own progressiveThreshold (3000), i.e. the regime the
 * blink was measured in -- not a 3-cell toy that would never chunk anyway. */
function bigHeatmap(ncells = 3500) {
    const cells = [];
    for (let i = 0; i < ncells; i++) cells.push([i % 700, i % 5, (i % 97) + 1]);
    return { bucket_ns: 1_000_000_000, max_count: 97,
        times: Array.from({ length: 700 }, (_, i) => 1000 + i),
        labels: ['<1', '1-2', '2-4', '4-8', '8-16'], cells };
}

test('heatmap pins progressive: 0 so a >3000-cell grid lands in ONE frame (blink fix)', () => {
    const res = buildHeatmapOption(bigHeatmap(3500));
    const s = assertSinglePassPaint(res, '3500 cells');
    assert.equal(res.option.series[0].type, 'heatmap');   // index-0 contract (#105)
    assert.equal(s.data.length, 3500);                    // past progressiveThreshold
    // and the small-grid case, which must not be special-cased away
    assertSinglePassPaint(buildHeatmapOption(heatmap()), '3 cells');
});

test('blink gate still finds the heatmap series when the "not captured" series rides along', () => {
    // #105 appends a second (dataless) series. The gate must grade the series
    // that carries CELLS, not whatever sits at index 0 -- a reorder would
    // otherwise let an unpinned heatmap through.
    const data = bigHeatmap(3200);
    const res = buildHeatmapOption(data, { captureFromNs: 1000 + 50 });
    assert.equal(res.notCapturedCount, 50);
    assert.equal(res.option.series.length, 2);
    const s = assertSinglePassPaint(res, 'with annotation series');
    assert.equal(s.data.length, 3200);
});

test('blink gate REFUSES an absent heatmap instead of approving it', () => {
    // Bypass 1: empty/absent response -> option null. A conditional assertion
    // would pass vacuously here; this one must throw.
    assert.throws(() => assertSinglePassPaint(buildHeatmapOption(null), 'null'),
        /expected a built option/);
    assert.throws(() => assertSinglePassPaint(buildHeatmapOption({ cells: [] }), 'empty'),
        /expected a built option/);
    // Bypass 2: the series is there but carries no cells (nothing to chunk,
    // nothing to grade) -> also a refusal, never a pass.
    assert.throws(() => assertSinglePassPaint(
        { hasData: true, option: { series: [{ type: 'heatmap', data: [] }] } }, 'no cells'),
        /expected exactly 1 cell-carrying heatmap series/);
    // Bypass 3: cells present, progressive left at the ECharts default
    // (undefined -> 400). This is the pre-fix product, and it must be RED.
    assert.throws(() => assertSinglePassPaint(
        { hasData: true, option: { series: [{ type: 'heatmap', data: [[0, 0, 0, 1]] }] } }, 'default'),
        /must pin progressive: 0, got undefined/);
    // Bypass 4: a non-zero progressive (someone "tunes" it instead of
    // disabling it) is still chunked rendering -> still RED.
    assert.throws(() => assertSinglePassPaint(
        { hasData: true, option: { series: [{ type: 'heatmap', data: [[0, 0, 0, 1]], progressive: 400 }] } }, '400'),
        /must pin progressive: 0, got 400/);
});

test('heatmap empty / absent -> hasData false, no option', () => {
    assert.equal(buildHeatmapOption(null).hasData, false);
    assert.equal(buildHeatmapOption({ cells: [] }).hasData, false);
    assert.equal(buildHeatmapOption({ cells: [] }).option, null);
});

test('heatmap max_count defaults to 1 when missing (no NaN max)', () => {
    const { option } = buildHeatmapOption({ cells: [[0, 0, 1]], times: [1], labels: ['a'] });
    assert.equal(option.visualMap.max, Math.log1p(1));
});

test('selector model: distinct classes in first-seen order, events labeled name+count', () => {
    const events = [
        { event_id: 1, name: 'IO:DataFileRead', class: 'IO', count: 100 },
        { event_id: 2, name: 'IO:WalSync', class: 'IO', count: 50 },
        { event_id: 3, name: 'LWLock:WALInsert', class: 'LWLock', count: 30 },
    ];
    const m = buildSelectorModel(events, fmtCount);
    assert.deepEqual(m.classes, ['IO', 'LWLock']);
    assert.deepEqual(m.eventsByClass['IO'].map(e => e.event_id), [1, 2]);
    assert.deepEqual(m.eventsByClass['LWLock'].map(e => e.event_id), [3]);
    assert.equal(m.allEvents[0].label, 'IO:DataFileRead (100)');
    assert.equal(m.allEvents.length, 3);
});

test('selector model: caps the event list at 50', () => {
    const events = Array.from({ length: 80 }, (_, i) =>
        ({ event_id: i, name: 'E' + i, class: 'IO', count: i }));
    const m = buildSelectorModel(events, fmtCount);
    assert.equal(m.allEvents.length, 50);
    assert.equal(m.eventsByClass['IO'].length, 50);
});

test('selector model: empty input -> empty model, no crash', () => {
    const m = buildSelectorModel(null, fmtCount);
    assert.deepEqual(m.classes, []);
    assert.deepEqual(m.allEvents, []);
    assert.deepEqual(m.eventsByClass, {});
});

/* #105: the legend used to be `type: 'continuous'` — a bare gradient bar with
 * no numbers (Matrix's piecewise legend, on the same screen, always shows
 * numeric bucket ranges). Piecewise buckets always draw their own label. */
test('visualMap legend is piecewise with numeric bucket labels (#105)', () => {
    const { option } = buildHeatmapOption(heatmap());
    assert.equal(option.visualMap.type, 'piecewise');
    assert.ok(option.visualMap.splitNumber > 0);
    assert.equal(option.visualMap.formatter(Math.log1p(4)), '4');
    assert.equal(option.visualMap.formatter(Math.log1p(4), Math.log1p(9)), '4–9');
});

/* #105: buckets before capture began must never look like captured, empty
 * (zero-event) time — the server already omits their cells (sparse grid),
 * but a bare blank region is ambiguous. A labeled markArea on a dedicated
 * silent series makes "not captured" explicit. */
test('captureFromNs: buckets before it are counted and get a labeled markArea', () => {
    const h = heatmap(); // times: [1000, 2000, 3000], bucket_ns 1e9
    const bare = buildHeatmapOption(h);
    assert.equal(bare.notCapturedCount, 0);
    assert.equal(bare.option.series.length, 1);
    assert.equal(bare.option.series[0].type, 'heatmap');

    const m = buildHeatmapOption(h, { captureFromNs: 2500 });
    assert.equal(m.notCapturedCount, 2);              // t=1000, 2000 < 2500
    assert.equal(m.option.series.length, 2);
    // series[0].type === 'heatmap' is load-bearing (tests/ui_live_smoke.py,
    // tests/test_web_ui.py both assert it directly) — the annotation series
    // rides LAST, dataless, and carries the markArea; the heatmap series
    // (and its cell data) keeps its index and is otherwise untouched.
    assert.equal(m.option.series[0].type, 'heatmap');
    assert.deepEqual(m.option.series[0].data, bare.option.series[0].data);
    const anno = m.option.series[1];
    assert.equal(anno.type, 'line');
    assert.deepEqual(anno.data, []);
    assert.equal(anno.markArea.data[0][0].xAxis, 0);
    assert.equal(anno.markArea.data[0][1].xAxis, 1);
    assert.equal(anno.markArea.label.formatter, 'Not captured');
});

test('captureFromNs before every bucket -> no exclusion, no annotation series', () => {
    const m = buildHeatmapOption(heatmap(), { captureFromNs: 0 });
    assert.equal(m.notCapturedCount, 0);
    assert.equal(m.option.series.length, 1);
});
