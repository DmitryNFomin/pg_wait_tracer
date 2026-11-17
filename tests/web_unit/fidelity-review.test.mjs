/* U0 review regressions: assertion names also identify the six negative controls. */
import { test } from 'node:test';
import assert from 'node:assert/strict';
import { readFileSync } from 'node:fs';
import { buildAasOption, AAS_ANNOTATION_SERIES } from '../../web/static/lib/builders/aas.js';
import { buildUplotSpec, hitTest } from '../../web/static/lib/uplot-aas.js';
import { buildFidelityShading, buildCompareFidelity } from '../../web/static/lib/builders/fidelity.js';
import {
    buildPaneFidelity, paneFidelityBadgeHtml, buildPercentileBasis,
    percentileBasisHtml, compareFidelityHtml,
} from '../../web/static/lib/panels.js';
import { AAS_CLASS_ORDER } from '../../web/static/lib/format.js';
import { MANIFEST, FIXTURES } from '../../web/static/dev/fixtures/manifest.mjs';

const WIN = { from: 1e9, to: 6e9 };
const UNKNOWN = { fidelity: 'future-pause-verdict' };
const MESSAGE = 'Incomplete capture: some of this window was not captured.';

test('review 1: sampled and mixed overlays keep their presentation without chips', () => {
    for (const fidelity of ['sampled', 'mixed']) {
        const spec = buildUplotSpec({ fidelity }, { win: WIN });
        assert.equal(Object.hasOwn(spec.overlays.rects[0], 'label'), false,
            'SAMPLED_MIXED_OVERLAYS_HAVE_NO_CHIP');
    }
    for (const [fidelity, label] of [['none', 'Not captured'],
        ['future-pause-verdict', 'Incomplete capture']]) {
        assert.equal(buildUplotSpec({ fidelity }, { win: WIN }).overlays.rects[0].label, label,
            'UNCAPTURED_OVERLAYS_KEEP_LABELS');
    }
});

test('review 2: both AAS renderers preserve each present class around missing classes and buckets', () => {
    const { data, opts } = FIXTURES.aas['missing-bucket-zero'];
    const ec = buildAasOption(data, opts);
    const up = buildUplotSpec(data, opts);
    const ecValues = ec.option.series.filter(s => s.name !== AAS_ANNOTATION_SERIES)
        .map(s => s.data.map(p => p[1]));
    assert.deepEqual(up.rawValues, ecValues, 'AAS_PER_CLASS_VALUES_AGREE');
    // Bucket 1 has CPU=1, no offcpu, IO=0.5, Lock=0.1. Every cumulative
    // edge stays numeric there, including the zero-thickness missing layers.
    assert.deepEqual(up.alignedData.slice(1).map(v => v[1]),
        [1, 1, 1.5, 1.6, 1.6, 1.6, 1.6, 1.6, 1.6, 1.6, 1.6, 1.6],
        'MISSING_CLASS_KEEPS_LATER_CLASSES_VISIBLE');
    for (let j = 0; j < data.buckets.length; j++) {
        const present = ecValues.some(v => v[j] != null);
        let lower = 0;
        up.alignedData.slice(1).forEach((edge, i) => {
            if (!present) {
                assert.equal(edge[j], null, 'ALL_MISSING_BUCKET_IS_GAP');
            } else {
                assert.ok(edge[j] != null, 'MISSING_CLASS_KEEPS_LATER_CLASSES_VISIBLE');
                assert.equal(+(edge[j] - lower).toFixed(4), ecValues[i][j] ?? 0,
                    'AAS_DRAWN_CLASS_VALUES_AGREE');
                lower = edge[j];
            }
        });
    }
    assert.deepEqual(up.alignedData.slice(1).map(v => v[3]), AAS_CLASS_ORDER.map(() => 0),
        'OBSERVED_ZERO_BUCKET_STAYS_ZERO');
    const io = AAS_CLASS_ORDER.find(c => c.key === 'io').label;
    assert.equal(hitTest(up, up.alignedData[0][1], 1.25)?.seriesName, io,
        'PRESENT_IO_REMAINS_HITTABLE');
    assert.equal(hitTest(up, up.alignedData[0][2], 0), null, 'GAP_HAS_NO_HIT');
});

test('review 3: mixed fallback survives none and incomplete ranges without hiding them', () => {
    const model = buildFidelityShading({ fidelity: 'mixed', fidelity_ranges: [
        { from: 1e9, to: 2e9, fidelity: 'none' },
        { from: 2e9, to: 3e9, fidelity: 'future-pause-verdict' },
        { from: 3e9, to: 6e9, fidelity: 'exact' },
    ] }, WIN);
    assert.deepEqual(model.bands, [
        { from: 1e9, to: 6e9, kind: 'mixed' },
        { from: 1e9, to: 2e9, kind: 'none' },
        { from: 2e9, to: 3e9, kind: 'incomplete' },
    ], 'MIXED_FALLBACK_WITH_UNCAPTURED_RANGES');
    assert.deepEqual(model.markArea.data.map(pair => pair[0].name),
        ['Mixed (sampled + exact)', 'Not captured', 'Incomplete capture']);
    assert.equal(model.disclaimer, null);
    for (const fidelity of ['sampled', 'mixed']) {
        const located = buildFidelityShading({ fidelity: 'mixed', fidelity_ranges: [
            { ...WIN, fidelity },
        ] }, WIN);
        assert.deepEqual(located.bands, [{ ...WIN, kind: fidelity }]);
    }
});

test('review 4: unavailable baseline wording never describes a nonexistent delta', () => {
    for (const opts of [{ baselineUnavailable: true }, { baselinePredates: true }, {}]) {
        const model = buildCompareFidelity(UNKNOWN, { fidelity: 'none' }, opts);
        assert.equal(model.warning,
            "No comparison: the baseline has no data; this window's capture is incomplete.",
            'UNAVAILABLE_BASELINE_HAS_NO_DELTA_WORDING');
        assert.doesNotMatch(compareFidelityHtml(model), /Δ/);
        assert.equal(model.mismatch, true);
    }
    assert.equal(buildCompareFidelity(UNKNOWN, { fidelity: 'exact' }).warning,
        'Δ compares incomplete capture against exact');
});

test('review 5: incomplete pane message appears in its badge without a percentile duplicate', () => {
    const data = { ...UNKNOWN, rows: [{ count: 5, exact_count: 2, p50_us: 20 }] };
    const badge = paneFidelityBadgeHtml(buildPaneFidelity(data));
    assert.ok(badge.includes('>' + MESSAGE + '</div>'), 'INCOMPLETE_MESSAGE_STAYS_IN_BADGE');
    assert.equal(percentileBasisHtml(buildPercentileBasis(data)), '',
        'INCOMPLETE_PANE_HAS_NO_DUPLICATE_FOOTNOTE');
    for (const fidelity of ['sampled', 'mixed']) {
        assert.match(buildPercentileBasis({ ...data, fidelity }).text,
            /Percentiles from exact-captured events only: 2 of 5 waits/);
    }
});

test('review 6: gap, uncaptured range, unknown badge and compare states are gallery capture cells', () => {
    const snapshots = readFileSync(new URL('../test_web_ui_snapshots.py', import.meta.url), 'utf8');
    const staticList = snapshots.match(/^GALLERY_STATIC_CELLS = \[([\s\S]*?)^\]/m)[1];
    const captures = new Set([...staticList.matchAll(/"(gallery-[a-z0-9-]+)"/g)].map(m => m[1]));
    const expected = [
        'aas/missing-bucket-zero', 'uplot-aas/missing-bucket-zero',
        'aas/not-captured-range', 'uplot-aas/not-captured-range',
        'fidelity/pane-unknown', 'fidelity/compare-unknown',
        'fidelity/compare-unknown-unavailable',
    ];
    for (const id of expected) {
        const entry = MANIFEST.find(e => e.id === id);
        assert.ok(entry, 'GALLERY_INCOMPLETE_STATES_REGISTERED: ' + id);
        assert.ok(captures.has(entry.cellId), 'GALLERY_INCOMPLETE_STATES_CAPTURED: ' + id);
    }
    for (const name of ['missing-bucket-zero', 'not-captured-range']) {
        assert.equal(FIXTURES.aas[name], FIXTURES['uplot-aas'][name]);
    }
    const range = FIXTURES.aas['not-captured-range'];
    assert.equal(buildUplotSpec(range.data, range.opts).overlays.rects[0].label, 'Not captured');
    const pane = FIXTURES.fidelity['pane-unknown'];
    assert.equal(pane.fn, 'pane');
    assert.equal(buildPaneFidelity(...pane.args).label, MESSAGE);
    const compare = FIXTURES.fidelity['compare-unknown'];
    assert.equal(compare.fn, 'compare');
    assert.match(compareFidelityHtml(buildCompareFidelity(...compare.args)), /Incomplete capture/);
});
