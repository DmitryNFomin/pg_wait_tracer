/* Node unit tests for the pure AAS -> uPlot spec builder (lib/uplot-aas.js).
 *
 * Runs under `node --test` — no framework, no browser, no network, no uPlot
 * (the module is pure; uPlot semantics it relies on are pinned by source
 * citations in its header against the vendored 1.6.32 bundle).
 *
 * The load-bearing suites here:
 *   - stacking correctness: cumulative sums, permutation-invariant totals,
 *     identity stack order — the official stack.js recipe pattern;
 *   - RENDERER PARITY: for identical inputs, buildUplotSpec and the ECharts
 *     buildAasOption must emit the same seriesNames/seriesColors (the
 *     external legend contract) and place the honesty marks at the same
 *     axis-ms coordinates — the renderer swap must not move a single
 *     trust annotation;
 *   - hitTest: cumulative-sum band walk with honest edges (before-data,
 *     gaps, above-stack, below-zero are misses, never misattributions);
 *   - overlay geometry: absolute (data-anchored) rects/lines, viewport
 *     clamping that clips bands but never MOVES a line into view;
 *   - UTC stability: tick-placement shim + label formatter under a
 *     deliberately hostile host TZ.
 */

// Hostile, fractional-offset host timezone (UTC+12:45/+13:45). Node >= 16.2
// honors runtime TZ changes; every assertion below is ALSO written against
// runtime-computed UTC getters, so the suite stays correct even where the
// env var is ignored.
process.env.TZ = 'Pacific/Chatham';

import { test } from 'node:test';
import assert from 'node:assert/strict';
import {
    buildUplotSpec, stackSeries, hitTest, overlayGeometry,
    utcTzDate, withAlpha, AREA_FILL_ALPHA, NCPUS_COLOR,
} from '../../web/static/lib/uplot-aas.js';
import {
    buildAasOption, AAS_ANNOTATION_SERIES,
} from '../../web/static/lib/builders/aas.js';
import { WAIT_CLASSES, eventColor, fmtTime } from '../../web/static/lib/format.js';
import {
    SAMPLED_BAND_COLOR, SAMPLED_BORDER, MIXED_BAND_COLOR, MIXED_BORDER,
    ESC_MANUAL_BORDER, ESC_ANOMALY_BORDER, ESC_MANUAL_COLOR, ESC_ANOMALY_COLOR,
} from '../../web/static/lib/builders/fidelity.js';

const approx = (a, b, eps = 1e-9) =>
    assert.ok(Math.abs(a - b) < eps, a + ' ~= ' + b);

/* Same class-mode fixture as aas.test.mjs (parity tests feed both builders
 * the identical object). */
function classBuckets(n) {
    const out = [];
    for (let i = 0; i < n; i++) {
        out.push({ t: 1000 + i, cpu: 1.0, io: 0.5, lock: 0.1, lwlock: 0.2,
            ipc: 0, client: 0, timeout: 0, bufferpin: 0, activity: 0,
            extension: 0, unknown: 0 });
    }
    return out;
}

/* Event-mode fixture: same three events, two server rank orders, aas[]
 * permuted consistently — both describe the SAME data (mirrors aas.test.mjs
 * permutedInputs). */
function permutedInputs() {
    const a = {
        bucket_ns: 1, max_aas: 1, breakdown: 'events',
        series: [{ name: 'Lock:relation' }, { name: 'IO:WalSync' },
                 { name: 'IO:DataFileRead' }],
        buckets: [
            { t: 10, aas: [0.3, 0.2, 0.1] },
            { t: 11, aas: [0.6, 0.5, 0.4] },
        ],
    };
    const b = {
        bucket_ns: 1, max_aas: 1, breakdown: 'events',
        series: [{ name: 'IO:DataFileRead' }, { name: 'Lock:relation' },
                 { name: 'IO:WalSync' }],
        buckets: [
            { t: 10, aas: [0.1, 0.3, 0.2] },
            { t: 11, aas: [0.4, 0.6, 0.5] },
        ],
    };
    return { a, b };
}

/* hitTest fixture: identity order IO:Alpha, IO:Beta, Lock:Gamma; dyadic
 * values so the cumulative sums are float-exact (0.25/0.5/1.0). bucket_ns =
 * 1e6 ns -> bucketMs = 1; bucket at t=14e6 leaves a gap at 12..13 ms. */
function hitFixture(vals) {
    return {
        bucket_ns: 1e6, max_aas: 1, breakdown: 'events',
        series: [{ name: 'IO:Alpha' }, { name: 'IO:Beta' }, { name: 'Lock:Gamma' }],
        buckets: [
            { t: 10e6, aas: vals },
            { t: 11e6, aas: vals },
            { t: 14e6, aas: vals },
        ],
    };
}

// ── Stacking (the official uPlot stack.js recipe pattern) ────────────────────

test('stackSeries: cumulative sums, bands clip upper to lower, stackIdxs', () => {
    const { stacked, bands, stackIdxs } = stackSeries(
        [[1, 2], [3, 4], [5, 6]], [false, false, false]);
    assert.deepEqual(stacked, [[1, 2], [4, 6], [9, 12]]);
    // Band { series: [upper, lower] }: each visible series' fill is clipped
    // down to the next visible one below it (verified against the vendored
    // bundle's fillStroke: b.series[0] is the upper edge). No inert [-1, top]
    // band (recipe quirk dropped — uPlot ignores it anyway).
    assert.deepEqual(bands, [{ series: [2, 1] }, { series: [3, 2] }]);
    assert.deepEqual(stackIdxs, [1, 2, 3]);
});

test('stackSeries: hidden series pass through raw, keep their slot', () => {
    const { stacked, bands, stackIdxs } = stackSeries(
        [[1, 1], [10, 10], [2, 2]], [false, true, false]);
    assert.deepEqual(stacked, [[1, 1], [10, 10], [3, 3]]);  // slot 2 raw
    assert.deepEqual(bands, [{ series: [3, 1] }]);          // adjacency skips it
    assert.deepEqual(stackIdxs, [1, 3]);
});

test('class mode: alignedData is [xsMs, cum per wait class] in identity order', () => {
    const data = { bucket_ns: 1, max_aas: 2.0, buckets: classBuckets(3) };
    const spec = buildUplotSpec(data, { numCpus: 4 });
    assert.equal(spec.alignedData.length, 1 + WAIT_CLASSES.length);
    // ns -> ms exactly once at the spec boundary: t=1000..1002 ns.
    assert.deepEqual(spec.alignedData[0], [0.001, 0.001001, 0.001002]);
    // Cumulative stack: cpu 1.0, +io 0.5, +lock 0.1, +lwlock 0.2, rest +0.
    approx(spec.alignedData[1][0], 1.0);
    approx(spec.alignedData[2][0], 1.5);
    approx(spec.alignedData[3][0], 1.6);
    approx(spec.alignedData[4][0], 1.8);
    approx(spec.alignedData[11][0], 1.8);     // top of stack = total
    assert.deepEqual(spec.seriesNames, WAIT_CLASSES.map(c => c.label));
    assert.deepEqual(spec.seriesColors, WAIT_CLASSES.map(c => c.color));
    assert.deepEqual(spec.stackIdxs,
        WAIT_CLASSES.map((_, i) => i + 1));
    assert.equal(spec.bands.length, WAIT_CLASSES.length - 1);
    spec.bands.forEach((b, k) =>
        assert.deepEqual(b, { series: [k + 2, k + 1] }));
});

test('values rounded to 4 decimals before stacking (ECharts parity)', () => {
    const spec = buildUplotSpec(
        { bucket_ns: 1, max_aas: 1, buckets: [{ t: 1, cpu: 0.123456789 }] },
        { numCpus: 1 });
    assert.equal(spec.alignedData[1][0], 0.1235);
    assert.deepEqual(spec.rawValues[0], [0.1235]);
});

test('event mode: stack + colors + data invariant under permuted server rank', () => {
    const { a, b } = permutedInputs();
    const sa = buildUplotSpec(a, { numCpus: 0 });
    const sb = buildUplotSpec(b, { numCpus: 0 });
    // Identity order: class rank (IO before Lock), then event name.
    assert.deepEqual(sa.seriesNames,
        ['IO:DataFileRead', 'IO:WalSync', 'Lock:relation']);
    assert.deepEqual(sa.seriesNames, sb.seriesNames);
    assert.deepEqual(sa.seriesColors, sb.seriesColors);
    // Same underlying data -> identical aligned (stacked) arrays: totals and
    // every intermediate cumsum are permutation-invariant.
    assert.deepEqual(sa.alignedData, sb.alignedData);
    approx(sa.alignedData[3][0], 0.6);        // total bucket 0
    approx(sa.alignedData[3][1], 1.5);        // total bucket 1
});

test('hiddenNames: raw slot, show:false, chain/bands skip it, names stay full', () => {
    const { a } = permutedInputs();
    const spec = buildUplotSpec(a, { numCpus: 0, hiddenNames: ['IO:WalSync'] });
    // Legend contract unchanged: the FULL identity list, hidden included.
    assert.deepEqual(spec.seriesNames,
        ['IO:DataFileRead', 'IO:WalSync', 'Lock:relation']);
    // Slot 2 keeps RAW values (recipe pass-through) and is show:false.
    assert.deepEqual(spec.alignedData[2], [0.2, 0.5]);
    assert.equal(spec.uplotOpts.series[2].show, false);
    assert.equal(spec.uplotOpts.series[1].show, true);
    // Cumulative chain skips it: top = DataFileRead + Lock:relation.
    assert.deepEqual(spec.stackIdxs, [1, 3]);
    approx(spec.alignedData[3][0], 0.4);      // 0.1 + 0.3
    assert.deepEqual(spec.bands, [{ series: [3, 1] }]);
});

test('empty data: hasData false, sane empty aligned arrays, no crash', () => {
    const spec = buildUplotSpec({ buckets: [], max_aas: 0, bucket_ns: 0 },
        { numCpus: 4 });
    assert.equal(spec.hasData, false);
    assert.deepEqual(spec.alignedData[0], []);
    assert.equal(spec.alignedData.length, 1 + WAIT_CLASSES.length);
});

// ── Renderer parity with the ECharts builder ─────────────────────────────────

test('PARITY: seriesNames/seriesColors identical to buildAasOption (both modes)', () => {
    const cls = { bucket_ns: 1, max_aas: 2, buckets: classBuckets(2) };
    const mE = buildAasOption(cls, { numCpus: 4 });
    const mU = buildUplotSpec(cls, { numCpus: 4 });
    assert.deepEqual(mU.seriesNames, mE.seriesNames);
    assert.deepEqual(mU.seriesColors, mE.seriesColors);

    const { a, b } = permutedInputs();
    for (const input of [a, b]) {
        const e = buildAasOption(input, { numCpus: 0 });
        const u = buildUplotSpec(input, { numCpus: 0 });
        assert.deepEqual(u.seriesNames, e.seriesNames);
        assert.deepEqual(u.seriesColors, e.seriesColors);
        assert.deepEqual(u.seriesColors,
            u.seriesNames.map(n => eventColor(null, n)));
    }
});

/* U2 (review P7): the yMax policy — yMax = max(maxAas*1.2,
 * min(numCpus*1.5, maxAas*4), 1) — is implemented in BOTH builders (they
 * stay drop-in interchangeable); this parity pin holds the two copies
 * together across the exact regimes the cap distinguishes. */
test('PARITY: P7 yMax policy identical in both builders across regimes', () => {
    const cases = [
        { max_aas: 1, numCpus: 8, want: 4 },      // cap active: min(12, 4)
        { max_aas: 3, numCpus: 64, want: 12 },    // P7 flagship: min(96, 12)
        { max_aas: 4.5, numCpus: 4, want: 6 },    // cap inactive: min(6, 18)
        { max_aas: 100, numCpus: 4, want: 120 },  // data dominates
        { max_aas: 0, numCpus: 0, want: 1 },      // floor
    ];
    for (const c of cases) {
        const data = { bucket_ns: 1, max_aas: c.max_aas, buckets: classBuckets(1) };
        const u = buildUplotSpec(data, { numCpus: c.numCpus });
        const e = buildAasOption(data, { numCpus: c.numCpus });
        assert.equal(u.yMax, c.want, 'uplot ' + JSON.stringify(c));
        assert.equal(e.option.yAxis.max, c.want, 'echarts ' + JSON.stringify(c));
        assert.deepEqual(u.uplotOpts.scales.y.range, [0, c.want]);
    }
});

/* U2 (review P3 wire 1): both builders expose the same drill surface —
 * seriesIds parallel to seriesNames (event_id in event mode, nulls in class
 * mode) and the breakdown discriminator — so the click→drill mount code is
 * renderer-agnostic. */
test('PARITY: seriesIds + breakdown identical to buildAasOption', () => {
    const ev = {
        bucket_ns: 1, max_aas: 1, breakdown: 'events',
        series: [{ name: 'Lock:relation', event_id: 42 },
                 { name: 'IO:DataFileRead', event_id: 7 },
                 { name: 'IO:WalSync' }],                 // id omitted -> null
        buckets: [{ t: 10, aas: [0.3, 0.1, 0.2] }],
    };
    const u = buildUplotSpec(ev, {});
    const e = buildAasOption(ev, {});
    assert.deepEqual(u.seriesNames,
        ['IO:DataFileRead', 'IO:WalSync', 'Lock:relation']);
    assert.deepEqual(u.seriesIds, [7, null, 42]);   // ids follow identity order
    assert.deepEqual(u.seriesIds, e.seriesIds);
    assert.equal(u.breakdown, 'events');
    assert.equal(e.breakdown, 'events');

    const cls = { bucket_ns: 1, max_aas: 1, buckets: classBuckets(1) };
    const uc = buildUplotSpec(cls, {});
    const ec = buildAasOption(cls, {});
    assert.equal(uc.breakdown, 'classes');
    assert.deepEqual(uc.seriesIds, WAIT_CLASSES.map(() => null));
    assert.deepEqual(uc.seriesIds, ec.seriesIds);
});

test('PARITY: y pin, x window and axis-label text match the ECharts option', () => {
    const win = { from: 2e15, to: 3e15 };
    const data = { bucket_ns: 60e9, max_aas: 1, buckets: classBuckets(2) };
    const e = buildAasOption(data, { numCpus: 8, win });
    const u = buildUplotSpec(data, { numCpus: 8, win });
    // y: same formula (max_aas*1.2 vs numCpus*1.5 vs 1), pinned range.
    assert.equal(u.yMax, e.option.yAxis.max);
    assert.deepEqual(u.uplotOpts.scales.y.range, [0, e.option.yAxis.max]);
    // x: the camera window in axis ms — the ECharts dataZoom crop values.
    assert.deepEqual(u.xWindow,
        { min: e.option.dataZoom[0].startValue, max: e.option.dataZoom[0].endValue });
    // axis labels: byte-identical UTC text for the same split.
    const ms = Date.UTC(2026, 0, 15, 12, 34, 56);
    assert.deepEqual(u.uplotOpts.axes[0].values(null, [ms]),
        [e.option.xAxis.axisLabel.formatter(ms)]);
    assert.equal(u.uplotOpts.axes[0].values(null, [ms])[0], '12:34:56');
});

test('PARITY: honesty marks at the same axis-ms coordinates as ECharts', () => {
    const data = { bucket_ns: 1e9, max_aas: 1, fidelity: 'sampled',
        buckets: classBuckets(2) };
    const opts = {
        numCpus: 2,
        win: { from: 2e15, to: 3e15 },
        axis: { from: 1e15, to: 4e15 },          // 3x strip skirt
        escalationStatus: { tier: 'escalated', escalation_reason: 'manual',
            escalation_seconds_remaining: 30, observed_start_ns: 2.5e15 },
    };
    const e = buildAasOption(data, opts);
    const u = buildUplotSpec(data, opts);
    const anno = e.option.series.find(s => s.name === AAS_ANNOTATION_SERIES);

    // Sampled shading: full rendered strip extent, both renderers.
    const eBand = anno.markArea.data[0];
    const uBand = u.overlays.rects.find(r => r.kind === 'sampled');
    assert.equal(uBand.x0, eBand[0].xAxis);
    assert.equal(uBand.x1, eBand[1].xAxis);
    assert.equal(uBand.fill, SAMPLED_BAND_COLOR);
    assert.equal(uBand.stroke, SAMPLED_BORDER);

    // Escalation live edge: at win.to on both (never the skirt edge).
    const eEdge = anno.markLine.data.find(d => d.xAxis != null);
    const uEdge = u.overlays.vlines.find(l => l.kind === 'escalation-edge');
    assert.equal(uEdge.x, eEdge.xAxis);
    assert.equal(uEdge.x, 3e15 / 1e6);
    assert.equal(uEdge.color, ESC_MANUAL_BORDER);
    assert.equal(uEdge.label, 'Escalated (manual)');

    // Escalation band: observed span only.
    const uEsc = u.overlays.rects.find(r => r.kind === 'escalation');
    assert.equal(uEsc.x0, 2.5e15 / 1e6);
    assert.equal(uEsc.x1, 3e15 / 1e6);

    // N-CPUs line: same y as the ECharts markLine.
    const eCpu = anno.markLine.data.find(d => d.yAxis != null);
    const uCpu = u.overlays.hlines.find(l => l.kind === 'ncpus');
    assert.equal(uCpu.y, eCpu.yAxis);
    assert.equal(uCpu.color, NCPUS_COLOR);
    assert.equal(uCpu.label, '2 CPUs');

    // ns parity fields match too (integrator swaps mount layers, not math).
    assert.deepEqual(u.axisRange, e.axisRange);
    assert.deepEqual(u.window, e.window);
});

// ── Spec plumbing: gesture ownership, scales, windows ────────────────────────

test('gesture ownership pins: ms=1, UTC tzDate, our cursor, no uPlot legend', () => {
    const spec = buildUplotSpec(
        { bucket_ns: 1, max_aas: 1, buckets: classBuckets(1) }, { numCpus: 1 });
    const o = spec.uplotOpts;
    assert.equal(o.ms, 1);                        // x = UNIX ms, not seconds
    assert.equal(o.tzDate, utcTzDate);            // UTC tick placement
    assert.equal(o.legend.show, false);
    // uPlot's built-in drag zoom/pan must never race the camera gestures
    // (wheel = zoom, shift+drag = pan, plain drag = brush overlay).
    assert.deepEqual(o.cursor.drag, { x: false, y: false, setScale: false });
    assert.equal(o.scales.x.time, true);
    assert.equal(o.scales.x.auto, false);
    // x scale carries NO range override: the default pass-through keeps
    // setScale('x', {min,max}) exact — the camera IS the viewport.
    assert.equal(o.scales.x.range, undefined);
    assert.equal(o.bands, spec.bands);
});

test('series defs: identity stroke, 0.85 alpha fill, thin line, no points', () => {
    const spec = buildUplotSpec(
        { bucket_ns: 1, max_aas: 1, buckets: classBuckets(1) }, { numCpus: 1 });
    spec.seriesNames.forEach((name, i) => {
        const s = spec.uplotOpts.series[i + 1];
        assert.equal(s.label, name);
        assert.equal(s.stroke, spec.seriesColors[i]);
        assert.equal(s.fill, withAlpha(spec.seriesColors[i], AREA_FILL_ALPHA));
        assert.equal(s.width, 1);
        assert.equal(s.points.show, false);
        assert.equal(s.show, true);
    });
});

test('xWindow: camera window in ms; degenerate window falls back to axis extent', () => {
    const win = { from: 2e15, to: 3e15 };
    const spec = buildUplotSpec(
        { bucket_ns: 1e9, max_aas: 1, buckets: classBuckets(2) },
        { numCpus: 1, win });
    assert.deepEqual(spec.xWindow, { min: 2e9, max: 3e9 });
    // Degenerate window (from == to): serve the axis extent instead of a
    // zero-width scale range.
    const spec2 = buildUplotSpec(
        { bucket_ns: 1e9, max_aas: 1, buckets: classBuckets(2) },
        { numCpus: 1, win: { from: 5e15, to: 5e15 } });
    assert.ok(spec2.xWindow.max > spec2.xWindow.min);
    assert.equal(spec2.xWindow.min, 5e15 / 1e6);
});

test('axisRange is the union of opts.axis and win (never narrower than win)', () => {
    const win = { from: 2e15, to: 3e15 };
    const spec = buildUplotSpec(
        { bucket_ns: 1e9, max_aas: 1, buckets: classBuckets(2) },
        { numCpus: 1, win, axis: { from: 2.2e15, to: 2.8e15 } });   // narrower
    assert.deepEqual(spec.axisRange, { from: 2e15, to: 3e15 });
    assert.deepEqual(spec.window, { from: 2e15, to: 3e15 });
});

test('y axis values render one decimal (ECharts formatter parity)', () => {
    const spec = buildUplotSpec(
        { bucket_ns: 1, max_aas: 1, buckets: classBuckets(1) }, { numCpus: 1 });
    assert.deepEqual(spec.uplotOpts.axes[1].values(null, [0, 1.25, 12]),
        ['0.0', '1.3', '12.0']);
});

// ── UTC stability under a hostile host TZ ────────────────────────────────────

test('utcTzDate: local getters of the shifted Date read UTC components', () => {
    // The shim uPlot's own tzDate uses for UTC (vendored source 966-971):
    // tick placement must be UTC-calendar-aligned on any host.
    for (const ts of [
        Date.UTC(2026, 0, 15, 12, 34, 56),
        Date.UTC(2026, 6, 1, 0, 0, 0),
        Date.UTC(2026, 11, 31, 23, 59, 59),
    ]) {
        const src = new Date(ts);
        const d = utcTzDate(ts);
        assert.equal(d.getFullYear(), src.getUTCFullYear());
        assert.equal(d.getMonth(), src.getUTCMonth());
        assert.equal(d.getDate(), src.getUTCDate());
        assert.equal(d.getHours(), src.getUTCHours());
        assert.equal(d.getMinutes(), src.getUTCMinutes());
        assert.equal(d.getSeconds(), src.getUTCSeconds());
    }
});

test('x axis label text is UTC via fmtTime regardless of host TZ', () => {
    const spec = buildUplotSpec(
        { bucket_ns: 60e9, max_aas: 1, buckets: classBuckets(2) }, { numCpus: 1 });
    const ms = Date.UTC(2026, 0, 15, 12, 34, 56);
    assert.deepEqual(spec.uplotOpts.axes[0].values(null, [ms]), ['12:34:56']);
    // Sub-second buckets keep fmtTime's fractional rendering.
    const spec2 = buildUplotSpec(
        { bucket_ns: 1e6, max_aas: 1, buckets: classBuckets(2) }, { numCpus: 1 });
    assert.equal(spec2.uplotOpts.axes[0].values(null, [ms])[0],
        fmtTime(ms * 1e6, 1e6));
});

test('withAlpha: rgb -> rgba at the stacked-area opacity; others untouched', () => {
    assert.equal(withAlpha('rgb(80,250,123)', 0.85), 'rgba(80,250,123,0.85)');
    assert.equal(withAlpha('#123456', 0.85), '#123456');
});

// ── hitTest: cumulative-sum band walk ────────────────────────────────────────

test('hitTest: walks bands bottom to top (dyadic cums 0.25/0.5/1.0)', () => {
    const spec = buildUplotSpec(hitFixture([0.25, 0.25, 0.5]), {});
    assert.deepEqual(hitTest(spec, 10.5, 0.1),
        { seriesName: 'IO:Alpha', seriesIdx: 1, bucketIdx: 0 });
    assert.deepEqual(hitTest(spec, 10.5, 0.4),
        { seriesName: 'IO:Beta', seriesIdx: 2, bucketIdx: 0 });
    assert.deepEqual(hitTest(spec, 10.5, 0.9),
        { seriesName: 'Lock:Gamma', seriesIdx: 3, bucketIdx: 0 });
    // Boundary y belongs to the band that ENDS there (what the eye sees).
    assert.equal(hitTest(spec, 10.5, 0.25).seriesName, 'IO:Alpha');
    // Baseline click hits the bottom band.
    assert.equal(hitTest(spec, 10.5, 0).seriesName, 'IO:Alpha');
});

test('hitTest: above the stack top, below zero, NaN -> null', () => {
    const spec = buildUplotSpec(hitFixture([0.25, 0.25, 0.5]), {});
    assert.equal(hitTest(spec, 10.5, 1.01), null);
    assert.equal(hitTest(spec, 10.5, -0.01), null);
    assert.equal(hitTest(spec, 10.5, NaN), null);
    assert.equal(hitTest(spec, NaN, 0.1), null);
});

test('hitTest: bucket ownership [t, t+bucketMs) — before-data, gaps, past-end miss', () => {
    const spec = buildUplotSpec(hitFixture([0.25, 0.25, 0.5]), {});
    assert.equal(spec.bucketMs, 1);
    assert.equal(hitTest(spec, 9.99, 0.1), null);         // before first bucket
    assert.equal(hitTest(spec, 11.999, 0.1).bucketIdx, 1); // inside bucket 1
    assert.equal(hitTest(spec, 12.5, 0.1), null);          // gap 12..14
    assert.equal(hitTest(spec, 14.2, 0.1).bucketIdx, 2);   // last bucket
    assert.equal(hitTest(spec, 15.0, 0.1), null);          // past last bucket end
});

test('hitTest: zero-thickness layers are unhittable', () => {
    // IO:Beta contributes 0 -> cums 0.25 / 0.25 / 0.5: the 0.25 boundary
    // belongs to Alpha, anything above it to Gamma — Beta has no band.
    const spec = buildUplotSpec(hitFixture([0.25, 0, 0.25]), {});
    assert.equal(hitTest(spec, 10.5, 0.25).seriesName, 'IO:Alpha');
    assert.equal(hitTest(spec, 10.5, 0.3).seriesName, 'Lock:Gamma');
});

test('hitTest: hidden series are skipped (visible chain only)', () => {
    const spec = buildUplotSpec(hitFixture([0.25, 0.25, 0.25]),
        { hiddenNames: ['IO:Beta'] });
    // Visible cums: Alpha 0.25, Gamma 0.5 — a y in (0.25, 0.5] is Gamma.
    assert.equal(hitTest(spec, 10.5, 0.3).seriesName, 'Lock:Gamma');
    assert.equal(hitTest(spec, 10.5, 0.6), null);
});

test('hitTest: empty spec / no buckets -> null', () => {
    const spec = buildUplotSpec({ buckets: [], max_aas: 0, bucket_ns: 0 }, {});
    assert.equal(hitTest(spec, 1, 0.1), null);
    assert.equal(hitTest(null, 1, 0.1), null);
});

// ── Overlay geometry: the honesty overlays as camera-space rects/lines ───────

test('sampled window: one dashed band over the full rendered strip extent', () => {
    const data = { bucket_ns: 1e9, max_aas: 1, fidelity: 'sampled',
        buckets: classBuckets(2) };
    const opts = { numCpus: 0, win: { from: 2e15, to: 3e15 },
        axis: { from: 1e15, to: 4e15 } };
    const geo = overlayGeometry(data, opts, null);
    assert.equal(geo.rects.length, 1);
    assert.deepEqual(geo.rects[0], {
        kind: 'sampled', x0: 1e9, x1: 4e9,
        fill: SAMPLED_BAND_COLOR, stroke: SAMPLED_BORDER, dash: [4, 4],
    });
    assert.deepEqual(geo.vlines, []);
    assert.deepEqual(geo.hlines, []);
});

test('mixed window with fidelity_ranges: sampled sub-bands at absolute positions', () => {
    const data = { bucket_ns: 1e9, max_aas: 1, fidelity: 'mixed',
        fidelity_ranges: [
            { from: 2.1e15, to: 2.4e15, fidelity: 'sampled' },
            { from: 2.4e15, to: 2.6e15, fidelity: 'exact' },
            { from: 2.6e15, to: 2.9e15, fidelity: 'sampled' },
        ],
        buckets: classBuckets(2) };
    const geo = overlayGeometry(data,
        { numCpus: 0, win: { from: 2e15, to: 3e15 } }, null);
    assert.deepEqual(geo.rects.map(r => [r.kind, r.x0, r.x1]), [
        ['sampled', 2.1e9, 2.4e9],
        ['sampled', 2.6e9, 2.9e9],
    ]);
});

test('mixed window without sub-ranges: whole extent banded mixed', () => {
    const data = { bucket_ns: 1e9, max_aas: 1, fidelity: 'mixed',
        buckets: classBuckets(2) };
    const geo = overlayGeometry(data,
        { numCpus: 0, win: { from: 2e15, to: 3e15 } }, null);
    assert.equal(geo.rects.length, 1);
    assert.equal(geo.rects[0].kind, 'mixed');
    assert.equal(geo.rects[0].fill, MIXED_BAND_COLOR);
    assert.equal(geo.rects[0].stroke, MIXED_BORDER);
});

test('exact window: no fidelity rects; N-CPUs hline iff numCpus > 0', () => {
    const data = { bucket_ns: 1e9, max_aas: 1, fidelity: 'exact',
        buckets: classBuckets(2) };
    const win = { from: 2e15, to: 3e15 };
    const withCpus = overlayGeometry(data, { numCpus: 4, win }, null);
    assert.deepEqual(withCpus.rects, []);
    assert.deepEqual(withCpus.hlines, [{
        kind: 'ncpus', y: 4, color: NCPUS_COLOR, dash: [4, 4], label: '4 CPUs',
    }]);
    const noCpus = overlayGeometry(data, { numCpus: 0, win }, null);
    assert.deepEqual(noCpus.hlines, []);
});

/* U2 (review P7): with the CURRENT y scale top supplied (scaleWindow.yMax —
 * the view passes u.scales.y.max, i.e. the hysteresis-applied top), an
 * N-CPUs reference ABOVE it becomes the explicit 'ncpus-offscale' top-edge
 * affordance: label "N CPUs ↑" pinned at the scale top, NO line (a line at
 * the rim would claim capacity sits there). The reference must never
 * silently vanish — off-scale is a rendered state, not a dropped one. */
test('N-CPUs above the y scale top -> ncpus-offscale affordance (P7)', () => {
    const data = { bucket_ns: 1e9, max_aas: 3, fidelity: 'exact',
        buckets: classBuckets(2) };
    const win = { from: 2e15, to: 3e15 };
    const scale = { min: 2e9, max: 3e9, yMax: 12 };   // policy top for 64 CPUs

    const off = overlayGeometry(data, { numCpus: 64, win }, scale);
    assert.deepEqual(off.hlines, [{
        kind: 'ncpus-offscale', y: 12, color: NCPUS_COLOR, dash: null,
        label: '64 CPUs ↑',
    }]);

    // On-scale under the same viewport: the plain reference line survives.
    const on = overlayGeometry(data, { numCpus: 4, win }, scale);
    assert.deepEqual(on.hlines, [{
        kind: 'ncpus', y: 4, color: NCPUS_COLOR, dash: [4, 4], label: '4 CPUs',
    }]);

    // Exactly AT the top is on-scale (the line hugs the rim, honestly).
    const rim = overlayGeometry(data, { numCpus: 12, win },
        { min: 2e9, max: 3e9, yMax: 12 });
    assert.equal(rim.hlines[0].kind, 'ncpus');

    // Absolute geometry (scaleWindow null) knows no viewport: the reference
    // stays a plain hline at its true y — spec.overlays is never transformed.
    const abs = overlayGeometry(data, { numCpus: 64, win }, null);
    assert.equal(abs.hlines[0].kind, 'ncpus');
    assert.equal(abs.hlines[0].y, 64);
});

test('escalation: observed span band + live-edge line; anomaly recolors', () => {
    const win = { from: 2e15, to: 3e15 };
    const data = { bucket_ns: 1e9, max_aas: 1, buckets: classBuckets(2) };
    const manual = overlayGeometry(data, { numCpus: 0, win,
        escalationStatus: { tier: 'escalated', escalation_reason: 'manual',
            escalation_seconds_remaining: 30, observed_start_ns: 2.5e15 } }, null);
    assert.deepEqual(manual.rects, [{
        kind: 'escalation', x0: 2.5e9, x1: 3e9,
        fill: ESC_MANUAL_COLOR, stroke: ESC_MANUAL_BORDER, dash: null,
    }]);
    assert.deepEqual(manual.vlines, [{
        kind: 'escalation-edge', x: 3e9, color: ESC_MANUAL_BORDER,
        label: 'Escalated (manual)',
    }]);
    const anomaly = overlayGeometry(data, { numCpus: 0, win,
        escalationStatus: { tier: 'escalated', escalation_reason: 'anomaly',
            escalation_seconds_remaining: 30, observed_start_ns: 2.5e15 } }, null);
    assert.equal(anomaly.rects[0].fill, ESC_ANOMALY_COLOR);
    assert.equal(anomaly.vlines[0].color, ESC_ANOMALY_BORDER);
    assert.equal(anomaly.vlines[0].label, 'Escalated (anomaly)');
});

test('escalation with unknown start: NO band, edge line only (never fabricate)', () => {
    // Page loaded mid-escalation: the daemon reports no window start, so we
    // must not invent a duration (UI-10) — same rule the ECharts path pins.
    const geo = overlayGeometry(
        { bucket_ns: 1e9, max_aas: 1, buckets: classBuckets(2) },
        { numCpus: 0, win: { from: 2e15, to: 3e15 },
          escalationStatus: { tier: 'escalated', escalation_reason: 'manual',
              escalation_seconds_remaining: 30 } }, null);
    assert.deepEqual(geo.rects, []);
    assert.equal(geo.vlines.length, 1);
    assert.equal(geo.vlines[0].x, 3e9);
});

test('viewport clamp: rects clip to the window, never stretch past their data', () => {
    const data = { bucket_ns: 1e9, max_aas: 1, fidelity: 'sampled',
        buckets: classBuckets(2) };
    const opts = { numCpus: 2, win: { from: 2e15, to: 3e15 },
        axis: { from: 1e15, to: 4e15 } };
    // Zoomed IN: the strip-wide band clips to the visible window.
    const zoomed = overlayGeometry(data, opts, { min: 2.2e9, max: 2.4e9 });
    assert.deepEqual(zoomed.rects.map(r => [r.x0, r.x1]), [[2.2e9, 2.4e9]]);
    // Zoomed OUT past the strip: the band stays at its data edges — shading
    // must never cover time the response doesn't (fabricated confidence).
    const wide = overlayGeometry(data, opts, { min: 0.5e9, max: 8e9 });
    assert.deepEqual(wide.rects.map(r => [r.x0, r.x1]), [[1e9, 4e9]]);
    // Panned fully away: the band is dropped, the y-space hline survives.
    const away = overlayGeometry(data, opts, { min: 6e9, max: 8e9 });
    assert.deepEqual(away.rects, []);
    assert.equal(away.hlines.length, 1);
});

test('viewport clamp: an off-window line is DROPPED, never moved into view', () => {
    const data = { bucket_ns: 1e9, max_aas: 1, buckets: classBuckets(2) };
    const opts = { numCpus: 0, win: { from: 2e15, to: 3e15 },
        escalationStatus: { tier: 'escalated', escalation_reason: 'manual',
            escalation_seconds_remaining: 30, observed_start_ns: 2.5e15 } };
    // Camera panned left of the live edge: clamping the edge to the window
    // rim would LIE about when the escalation window ends.
    const panned = overlayGeometry(data, opts, { min: 2.0e9, max: 2.8e9 });
    assert.deepEqual(panned.vlines, []);
    // The escalation band still clips honestly to what's visible.
    assert.deepEqual(panned.rects.map(r => [r.x0, r.x1]), [[2.5e9, 2.8e9]]);
    // Edge exactly on the window rim stays (inclusive).
    const atRim = overlayGeometry(data, opts, { min: 2.0e9, max: 3e9 });
    assert.equal(atRim.vlines.length, 1);
    assert.equal(atRim.vlines[0].x, 3e9);
});

test('spec.overlays is the absolute (unclamped) geometry', () => {
    const data = { bucket_ns: 1e9, max_aas: 1, fidelity: 'sampled',
        buckets: classBuckets(2) };
    const opts = { numCpus: 2, win: { from: 2e15, to: 3e15 },
        axis: { from: 1e15, to: 4e15 },
        escalationStatus: { tier: 'escalated', escalation_reason: 'manual',
            escalation_seconds_remaining: 30, observed_start_ns: 2.5e15 } };
    const spec = buildUplotSpec(data, opts);
    assert.deepEqual(spec.overlays, overlayGeometry(data, opts, null));
    // All three honesty surfaces present in one spec.
    assert.deepEqual(spec.overlays.rects.map(r => r.kind),
        ['sampled', 'escalation']);
    assert.equal(spec.overlays.vlines.length, 1);
    assert.equal(spec.overlays.hlines.length, 1);
});

// U2b review F3: viewport-cut band edges are NOT band boundaries — the
// geometry marks cut sides so the painter skips their border stroke (a
// dashed rim stroke would claim a boundary where the band continues).
test('overlayGeometry marks viewport-clamped rect sides (F3)', () => {
    const data = { bucket_ns: 1e9, max_aas: 1, fidelity: 'sampled',
        buckets: classBuckets(2) };
    const opts = { numCpus: 2, win: { from: 2e15, to: 3e15 },
        axis: { from: 1e15, to: 4e15 } };          // band = [1e9, 4e9] ms
    // Window inside the band: both sides are viewport cuts, not boundaries.
    const cut = overlayGeometry(data, opts, { min: 2.2e9, max: 2.4e9 });
    assert.equal(cut.rects.length, 1);
    assert.equal(cut.rects[0].clampedLeft, true);
    assert.equal(cut.rects[0].clampedRight, true);
    // Window containing the band: both edges are TRUE data boundaries.
    const uncut = overlayGeometry(data, opts, { min: 0.5e9, max: 8e9 });
    assert.equal(uncut.rects[0].clampedLeft, false);
    assert.equal(uncut.rects[0].clampedRight, false);
    // One-sided cut.
    const half = overlayGeometry(data, opts, { min: 2.2e9, max: 8e9 });
    assert.equal(half.rects[0].clampedLeft, true);
    assert.equal(half.rects[0].clampedRight, false);
});
