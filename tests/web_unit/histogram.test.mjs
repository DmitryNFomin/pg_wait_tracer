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
