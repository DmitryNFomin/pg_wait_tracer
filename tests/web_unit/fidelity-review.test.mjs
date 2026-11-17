/* U0 review regressions: assertion names identify the negative controls. */
import { test } from 'node:test';
import assert from 'node:assert/strict';
import { readFileSync } from 'node:fs';
import { createRequire } from 'node:module';
import { buildAasOption, AAS_ANNOTATION_SERIES } from '../../web/static/lib/builders/aas.js';
import { buildUplotSpec, hitTest, drawOverlayRects } from '../../web/static/lib/uplot-aas.js';
import { buildFidelityShading, buildCompareFidelity, bandsToMarkArea } from '../../web/static/lib/builders/fidelity.js';
import {
    buildPaneFidelity, paneFidelityBadgeHtml, buildPercentileBasis,
    percentileBasisHtml, compareFidelityHtml,
} from '../../web/static/lib/panels.js';
import { AAS_CLASS_ORDER } from '../../web/static/lib/format.js';
import { MANIFEST, FIXTURES } from '../../web/static/dev/fixtures/manifest.mjs';

const WIN = { from: 1e9, to: 6e9 };
const UNKNOWN = { fidelity: 'future-pause-verdict' };
const MESSAGE = 'Incomplete capture: some of this window was not captured.';

// Exercise the vendored renderer and canvas painter, rather than just geometry.
const echarts = createRequire(import.meta.url)('../../web/static/vendor/echarts.min.js');
const BAND_LABELS = [
    ['none', 'Not captured'], ['incomplete', 'Incomplete capture'],
    ['sampled', undefined], ['mixed', undefined], ['exact', undefined],
];
function bandData(fidelity) {
    return { fidelity, bucket_ns: 1e9, max_aas: 1,
        buckets: [{ t: WIN.from, cpu: 1 }, { t: WIN.to, cpu: 1 }] };
}

test('markArea names only the none and incomplete band kinds', () => {
    for (const [kind, label] of BAND_LABELS) {
        const area = bandsToMarkArea([{ ...WIN, kind }]);
        assert.equal(area.data[0][0].name, label, 'MARK_AREA_NAMES_ONLY_UNCAPTURED: ' + kind);
        assert.equal(Object.hasOwn(area.data[0][0], 'name'), label !== undefined,
            'MARK_AREA_NAMES_ONLY_UNCAPTURED: ' + kind);
    }
});

test('ECharts paints only uncaptured band labels, entirely inside the plot', () => {
    for (const [kind, label] of BAND_LABELS) {
        const chart = echarts.init(null, null,
            { renderer: 'svg', ssr: true, width: 900, height: 320 });
        try {
            chart.setOption(buildAasOption(bandData(kind), { win: WIN }).option);
            const svg = chart.renderToSVGString();
            const texts = [...svg.matchAll(/<text[^>]*>([^<]*)<\/text>/g)]
                .map(m => m[1]).filter(t => /captur|Sampled|Mixed|Exact/.test(t));
            assert.deepEqual(texts, label ? [label] : [], 'ECHARTS_BAND_TEXT: ' + kind);
            const box = chart.getModel().getComponent('grid').coordinateSystem.getRect();
            const painted = chart.getZr().storage.getDisplayList().filter(el => label !== undefined && el.style?.text === label);
            assert.equal(painted.length, label ? 1 : 0, 'ECHARTS_BAND_TEXT: ' + kind);
            for (const el of painted) {
                const r = el.getBoundingRect().clone();
                r.applyTransform(el.getComputedTransform());
                assert.ok(r.x >= box.x && r.x + r.width <= box.x + box.width &&
                    r.y > box.y && r.y + r.height <= box.y + box.height,
                    'ECHARTS_BAND_LABEL_INSIDE_PLOT: ' + kind);
            }
        } finally {
            chart.dispose();
        }
    }
});

test('uPlot paints only uncaptured band labels, entirely inside the plot at both DPRs', () => {
    const oldDpr = globalThis.devicePixelRatio;
    try {
        for (const dpr of [1, 2]) {
            globalThis.devicePixelRatio = dpr;
            for (const [kind, label] of BAND_LABELS) {
                const texts = [];
                const bbox = { left: 55 * dpr, top: 14 * dpr, width: 825 * dpr, height: 278 * dpr };
                const ctx = {
                    save() {}, restore() {}, beginPath() {}, rect() {}, clip() {},
                    fillRect() {}, setLineDash() {}, moveTo() {}, lineTo() {}, stroke() {},
                    measureText(text) { return { width: text.length * 6 * dpr }; },
                    fillText(text, x, y) {
                        texts.push(text);
                        assert.equal(this.textAlign, 'center', 'UPLOT_BAND_LABEL_ALIGNMENT');
                        assert.equal(this.textBaseline, 'top', 'UPLOT_BAND_LABEL_ALIGNMENT');
                        assert.equal(this.font, (10 * dpr) + 'px sans-serif');
                        const halfWidth = this.measureText(text).width / 2;
                        assert.ok(x - halfWidth >= bbox.left && x + halfWidth <= bbox.left + bbox.width &&
                            y > bbox.top && y + 10 * dpr <= bbox.top + bbox.height,
                            'UPLOT_BAND_LABEL_INSIDE_PLOT: ' + kind);
                    },
                };
                const u = { ctx, bbox, valToPos: x => bbox.left +
                    (x - WIN.from / 1e6) / ((WIN.to - WIN.from) / 1e6) * bbox.width };
                drawOverlayRects(u, buildUplotSpec(bandData(kind), { win: WIN }).overlays);
                assert.deepEqual(texts, label ? [label] : [], 'UPLOT_BAND_TEXT: ' + kind);
            }
        }
    } finally {
        if (oldDpr === undefined) delete globalThis.devicePixelRatio;
        else globalThis.devicePixelRatio = oldDpr;
    }
});

test('review 2: both AAS renderers preserve each present class around missing classes and buckets', () => {
    const { data, opts } = FIXTURES.aas['missing-bucket-zero'];
    const ec = buildAasOption(data, opts);
    const up = buildUplotSpec(data, opts);
    const ecValues = ec.option.series.filter(s => s.name !== AAS_ANNOTATION_SERIES)
        .map(s => s.data.map(p => p[1]));
    assert.deepEqual(up.rawValues, ecValues, 'AAS_PER_CLASS_VALUES_AGREE');
    // Bucket 1 has CPU=1.5, no offcpu, IO=0.75, Lock=0.25. Every cumulative
    // edge stays numeric there, including the zero-thickness missing layers.
    assert.deepEqual(up.alignedData.slice(1).map(v => v[1]),
        [1.5, 1.5, 2.25, 2.5, 2.5, 2.5, 2.5, 2.5, 2.5, 2.5, 2.5, 2.5],
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
    assert.equal(hitTest(up, up.alignedData[0][1], 2)?.seriesName, io,
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
        [undefined, 'Not captured', 'Incomplete capture']);
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
