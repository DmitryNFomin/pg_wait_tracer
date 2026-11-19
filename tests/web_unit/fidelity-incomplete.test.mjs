import { test } from 'node:test';
import assert from 'node:assert/strict';
import {
    fidelityOf, fidelityLabel, buildCompareFidelity, buildFidelityShading,
    buildUnavailablePanel, INCOMPLETE_FIDELITY, INCOMPLETE_CAPTURE_MESSAGE,
} from '../../web/static/lib/builders/fidelity.js';
import {
    buildPaneFidelity, paneFidelityBadgeHtml, buildPercentileBasis,
    buildComparePaneFidelity, compareFidelityHtml,
} from '../../web/static/lib/panels.js';
import { buildAasOption, AAS_ANNOTATION_SERIES } from '../../web/static/lib/builders/aas.js';
import { buildUplotSpec, stackSeries } from '../../web/static/lib/uplot-aas.js';
import { buildTransitionsFidelity } from '../../web/static/lib/builders/transitions.js';
import { buildEventsModel } from '../../web/static/views/events.js';
import { buildQueriesModel } from '../../web/static/views/queries.js';
import { buildSessionsModel } from '../../web/static/views/sessions.js';
import { createOverviewView } from '../../web/static/views/overview.js';
import { AAS_CLASS_ORDER } from '../../web/static/lib/format.js';

const WIN = { from: 1e9, to: 6e9 };
const unknown = { fidelity: 'future-pause-verdict', rows: [], buckets: [] };
const message = 'Incomplete capture: some of this window was not captured.';
const values = Object.fromEntries(AAS_CLASS_ORDER.map(c => [c.key, 0]));
const overviewContext = { server: { numCpus: 4 }, timeRange: WIN,
    filters: { snapshot: () => ({}) }, getSort: () => null };

test('unknown fidelity normalization never certifies exact', () => {
    for (const fidelity of ['future-pause-verdict', 'incomplete', '', null, 7, false]) {
        assert.equal(fidelityOf({ fidelity }), INCOMPLETE_FIDELITY,
            'UNKNOWN_TOKEN_IS_INCOMPLETE');
    }
});

test('unknown fidelity label never says Exact', () => {
    assert.equal(fidelityLabel(unknown.fidelity), 'Incomplete capture', 'UNKNOWN_LABEL_IS_INCOMPLETE');
    assert.equal(fidelityLabel(INCOMPLETE_FIDELITY), 'Incomplete capture');
    assert.equal(fidelityLabel(null), 'Incomplete capture');
});

test('absent fidelity retains the legacy presentation in every helper', () => {
    for (const data of [undefined, null, {}, { rows: [], buckets: [] }]) {
        assert.equal(fidelityOf(data), 'exact');
        assert.equal(fidelityLabel(), 'Exact');
        assert.equal(buildPaneFidelity(data), null);
        assert.equal(buildPercentileBasis(data), null);
        assert.equal(buildCompareFidelity(data, data).mismatch, false);
        assert.equal(buildAasOption(data).fidelityLabel, 'Exact');
        assert.equal(buildUplotSpec(data).fidelityLabel, 'Exact');
        assert.equal(buildTransitionsFidelity(data, WIN).fidelityLabel, 'Exact');
        assert.equal(buildFidelityShading(data, WIN).markArea, null);
    }
});

test('incomplete sides always cause a compare mismatch including identical tokens', () => {
    for (const [a, b, opts] of [
        [unknown, unknown], [unknown, {}], [{}, unknown],
        [unknown, { fidelity: 'none' }], [unknown, {}, { baselineUnavailable: true }],
    ]) {
        const model = buildComparePaneFidelity(a, b, opts);
        assert.equal(model.mismatch, true, 'INCOMPLETE_COMPARE_IS_MISMATCH');
        assert.match(model.warning, /incomplete capture|capture is incomplete/, 'COMPARE_PHRASE_IS_INCOMPLETE');
        assert.match(compareFidelityHtml(model), /Incomplete capture/);
    }
});

test('pane badge discloses incomplete capture with the agreed wording', () => {
    const badge = buildPaneFidelity(unknown);
    assert.equal(INCOMPLETE_CAPTURE_MESSAGE, message);
    assert.ok(badge, 'INCOMPLETE_PANE_BADGE_IS_VISIBLE');
    assert.equal(badge.label, message, 'INCOMPLETE_PANE_BADGE_IS_VISIBLE');
    assert.equal(badge.title, message);
    assert.match(paneFidelityBadgeHtml(badge), /Incomplete capture: some of this window was not captured\./);
    assert.equal(buildPercentileBasis(unknown), null);
    const unavailable = buildUnavailablePanel(unknown);
    assert.equal(unavailable.fidelity, INCOMPLETE_FIDELITY);
    assert.match(unavailable.hint, /incomplete capture/);
});

test('every view fidelity model propagates unknown and legacy verdicts', () => {
    for (const data of [unknown, { rows: [], buckets: [] }]) {
        const expected = data.fidelity ? INCOMPLETE_FIDELITY : 'exact';
        for (const model of [buildAasOption(data), buildUplotSpec(data), buildTransitionsFidelity(data, WIN)]) {
            assert.equal(model.fidelity, expected);
            assert.equal(model.fidelityLabel, data.fidelity ? 'Incomplete capture' : 'Exact');
        }
        for (const model of [buildEventsModel(data), buildQueriesModel(data), buildSessionsModel(data),
            createOverviewView().build(data, overviewContext)]) {
            assert.equal(model.paneFidelity?.fidelity ?? 'exact', expected);
        }
        for (const model of [buildEventsModel({ compare: true, a: data, b: data }),
            buildQueriesModel({ compare: true, a: data, b: data }),
            createOverviewView().build({ compare: true, a: data, b: data }, overviewContext)]) {
            assert.equal(model.compareFidelity.mismatch, !!data.fidelity);
        }
    }
});

test('none and incomplete ranges have labeled shading in every chart model', () => {
    const data = { fidelity: 'mixed', buckets: [], fidelity_ranges: [
        { from: 1e9, to: 2e9, fidelity: 'none' },
        { from: 2e9, to: 3e9, fidelity: 'future-pause-verdict' },
        { from: 3e9, to: 4e9, fidelity: 'incomplete' },
        { from: 4e9, to: 5e9, fidelity: 'exact' },
        { from: 5e9, to: 6e9, fidelity: 'sampled' },
    ] };
    const shading = buildFidelityShading(data, WIN);
    assert.ok(shading.bands.some(b => b.kind === 'none'), 'NONE_RANGE_IS_SHADED');
    assert.deepEqual(shading.bands.map(b => b.kind), ['none', INCOMPLETE_FIDELITY, INCOMPLETE_FIDELITY, 'sampled']);
    assert.equal(shading.markArea.data[0][0].name, 'Not captured');
    assert.equal(shading.markArea.data[1][0].name, 'Incomplete capture');
    assert.equal(shading.showLegend, true);
    assert.equal(buildTransitionsFidelity(data, WIN).stripBands.length, 4);
    const ec = buildAasOption(data, { win: WIN });
    assert.equal(ec.option.series.find(s => s.name === AAS_ANNOTATION_SERIES).markArea.data.length, 4);
    const up = buildUplotSpec(data, { win: WIN });
    assert.deepEqual(up.overlays.rects.map(b => b.label),
        ['Not captured', 'Incomplete capture', 'Incomplete capture', undefined]);
    for (const fidelity of ['none', unknown.fidelity]) {
        assert.equal(buildFidelityShading({ fidelity }, WIN).bands.length, 1);
    }
});

for (const mode of ['classes', 'events']) {
    const data = mode === 'classes' ? { buckets: [
        { ...values, t: 1e9, cpu: 1.23456 }, { ...values, t: 2e9, cpu: null },
        { ...values, t: 3e9 }, { ...values, t: 4e9 }, { ...values, t: 5e9, cpu: 2 },
    ] } : { breakdown: 'events', series: [{ name: 'CPU' }], buckets: [
        { t: 1e9, aas: [1.23456] }, { t: 2e9, aas: [null] },
        { t: 3e9, aas: [] }, { t: 4e9, aas: [0] }, { t: 5e9, aas: [2] },
    ] };
    if (mode === 'classes') delete data.buckets[2].cpu;
    test(`ECharts ${mode} preserves missing values and real zero without interpolation`, () => {
        const model = buildAasOption(data, { win: WIN });
        const series = model.option.series.filter(s => s.name !== AAS_ANNOTATION_SERIES)[0];
        assert.deepEqual(series.data.map(p => p[1]), [1.2346, null, null, 0, 2],
            `ECHARTS_${mode.toUpperCase()}_GAP_NOT_ZERO`);
        assert.equal(series.connectNulls, false, 'ECHARTS_DOES_NOT_INTERPOLATE');
    });
    test(`uPlot ${mode} preserves missing values and real zero without interpolation`, () => {
        const spec = buildUplotSpec(data, { win: WIN });
        assert.deepEqual(spec.rawValues[0], [1.2346, null, null, 0, 2],
            `UPLOT_${mode.toUpperCase()}_GAP_NOT_ZERO`);
        assert.deepEqual(spec.alignedData[1], mode === 'classes'
            ? [1.2346, 0, 0, 0, 2] : [1.2346, null, null, 0, 2]);
        assert.equal(spec.uplotOpts.series[1].spanGaps, false, 'UPLOT_DOES_NOT_INTERPOLATE');
    });
    test(`${mode} tolerates null buckets and empty series`, () => {
        const nulled = { ...data, buckets: [null, ...data.buckets, null] };
        assert.doesNotThrow(() => buildAasOption(nulled));
        assert.doesNotThrow(() => buildUplotSpec(nulled));
        assert.doesNotThrow(() => buildAasOption({ ...data, buckets: [], series: [] }));
        assert.doesNotThrow(() => buildUplotSpec({ ...data, buckets: [], series: [] }));
        const missing = { ...data, buckets: [{ t: 1e9, aas: null }, { t: 2e9 }] };
        assert.doesNotThrow(() => buildAasOption(missing));
        assert.doesNotThrow(() => buildUplotSpec(missing));
    });
}

test('uPlot cumulative edges skip missing classes and preserve wholly missing buckets', () => {
    assert.deepEqual(stackSeries([[1, null, 0, null], [2, 3, 0, null]], []).stacked,
        [[1, 0, 0, null], [3, 3, 0, null]], 'STACK_SKIPS_MISSING_CLASS');
});
