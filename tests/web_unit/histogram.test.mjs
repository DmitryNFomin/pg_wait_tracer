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

test('heatmap option: category axes from times/labels, data triples preserved', () => {
    const { option, hasData } = buildHeatmapOption(heatmap());
    assert.equal(hasData, true);
    assert.equal(option.xAxis.type, 'category');
    assert.equal(option.yAxis.type, 'category');
    assert.equal(option.xAxis.data.length, 3);            // 3 time labels
    assert.deepEqual(option.yAxis.data, ['<1', '1-2', '2-4']);
    assert.equal(option.series[0].type, 'heatmap');
    assert.deepEqual(option.series[0].data, [[0, 0, 100], [1, 2, 500], [2, 1, 250]]);
});

test('heatmap visualMap min 0 / max = max_count with the 6-stop ramp', () => {
    const { option } = buildHeatmapOption(heatmap());
    assert.equal(option.visualMap.min, 0);
    assert.equal(option.visualMap.max, 500);
    assert.equal(option.visualMap.inRange.color.length, 6);
    assert.equal(option.visualMap.inRange.color[0], '#1a5276');
    assert.equal(option.visualMap.inRange.color[5], '#F44336');
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
    assert.equal(option.visualMap.max, 1);
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
