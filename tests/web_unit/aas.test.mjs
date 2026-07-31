/* Node unit tests for the pure AAS builder (lib/builders/aas.js).
 *
 * Runs under `node --test` — no framework, no browser, no network. Proves the
 * data -> ECharts option mapping (shape + filter/breakdown correctness) so an
 * off-by-one or a dropped series is caught in milliseconds without Playwright.
 */
import { test } from 'node:test';
import assert from 'node:assert/strict';
import {
    buildAasOption, aasTooltip, AAS_ANNOTATION_SERIES,
} from '../../web/static/lib/builders/aas.js';
import { WAIT_CLASSES, eventColor } from '../../web/static/lib/format.js';

function classBuckets(n) {
    const out = [];
    for (let i = 0; i < n; i++) {
        out.push({ t: 1000 + i, cpu: 1.0, io: 0.5, lock: 0.1, lwlock: 0.2,
            ipc: 0, client: 0, timeout: 0, bufferpin: 0, activity: 0,
            extension: 0, unknown: 0 });
    }
    return out;
}

/* Data series = everything except the silent annotation series (U1). */
function dataSeries(option) {
    return option.series.filter(s => s.name !== AAS_ANNOTATION_SERIES);
}

/* Hue (0-360) of an 'rgb(r,g,b)' string — for the tint-keeps-the-class-hue
 * contract; lightness/saturation may vary per event, hue may not. */
function hueOf(rgbStr) {
    const m = /^rgb\((\d+),\s*(\d+),\s*(\d+)\)$/.exec(rgbStr);
    const r = +m[1] / 255, g = +m[2] / 255, b = +m[3] / 255;
    const max = Math.max(r, g, b), min = Math.min(r, g, b), d = max - min;
    if (d === 0) return 0;
    let h;
    if (max === r) h = (g - b) / d + (g < b ? 6 : 0);
    else if (max === g) h = (b - r) / d + 2;
    else h = (r - g) / d + 4;
    return h * 60;
}

/* FLIPPED in U2a: bucket x values used to be raw ns on a type:'value' axis —
 * which is exactly the First-catch bug (docs/VISUAL_CHECKLIST.md): stacked 2D
 * data on a value x-axis drops every layer above series[0]. The axis is now
 * type:'time' and the option boundary converts ns -> UNIX ms exactly once, so
 * bucket pairs are [tMs, aas]. */
test('class mode: one series per wait class, x=tMs y=aas', () => {
    const data = { bucket_ns: 1, max_aas: 2.0, buckets: classBuckets(3) };
    const { option, seriesNames, seriesColors } = buildAasOption(data, { numCpus: 4 });

    assert.equal(dataSeries(option).length, WAIT_CLASSES.length);
    assert.deepEqual(seriesNames, WAIT_CLASSES.map(c => c.label));
    assert.deepEqual(seriesColors, WAIT_CLASSES.map(c => c.color));

    const cpu = option.series.find(s => s.name === 'CPU');
    // t = 1000..1002 ns -> 0.001..0.001002 ms (ns / 1e6, the ONE conversion).
    assert.deepEqual(cpu.data, [[0.001, 1], [0.001001, 1], [0.001002, 1]]);
    // Stacked area
    assert.equal(cpu.stack, 'aas');
    assert.ok(cpu.areaStyle);
});

test('y max accounts for cpu count and max_aas', () => {
    // numCpus*1.5 dominates when max_aas is small
    let opt = buildAasOption({ buckets: classBuckets(1), max_aas: 1, bucket_ns: 1 },
        { numCpus: 8 }).option;
    assert.equal(opt.yAxis.max, 12);  // 8 * 1.5
    // max_aas*1.2 dominates when AAS is high
    opt = buildAasOption({ buckets: classBuckets(1), max_aas: 100, bucket_ns: 1 },
        { numCpus: 4 }).option;
    assert.equal(opt.yAxis.max, 120);  // 100 * 1.2
});

/* FLIPPED in U1 (review P2): this used to pin the CPU markLine onto
 * series[0] — but any mark riding a data series vanishes when that series is
 * legend-unselected or hover-soloed away. All marks now ride the dedicated
 * silent annotation series; data series carry NO marks. */
test('cpu markLine rides the annotation series iff numCpus > 0', () => {
    const withCpu = buildAasOption({ buckets: classBuckets(1), max_aas: 1, bucket_ns: 1 },
        { numCpus: 4 }).option;
    const anno = withCpu.series.find(s => s.name === AAS_ANNOTATION_SERIES);
    assert.ok(anno, 'annotation series present');
    assert.equal(anno.markLine.data[0].yAxis, 4);
    for (const s of dataSeries(withCpu)) {
        assert.ok(!s.markLine && !s.markArea, 'data series carry no marks');
    }

    const noCpu = buildAasOption({ buckets: classBuckets(1), max_aas: 1, bucket_ns: 1 },
        { numCpus: 0 }).option;
    assert.ok(!noCpu.series.some(s => s.name === AAS_ANNOTATION_SERIES),
        'no marks -> no annotation series');
});

/* FLIPPED in U1 (review P2): this used to pin EVENT_PALETTE[idx] colors —
 * index-keyed over series the server re-ranks by AAS every window, so the
 * same event changed color (and hue family) on every live tick. Colors are
 * now identity-keyed via the eventColor service. */
test('event breakdown mode: one series per event with identity-keyed colors', () => {
    const data = {
        bucket_ns: 1, max_aas: 1.0, breakdown: 'events',
        series: [{ name: 'IO:DataFileRead' }, { name: 'IO:WalSync' }],
        buckets: [
            { t: 10, aas: [0.4, 0.2] },
            { t: 11, aas: [0.5, 0.25] },
        ],
    };
    const { option, seriesNames, seriesColors } = buildAasOption(data, { numCpus: 2 });
    assert.deepEqual(seriesNames, ['IO:DataFileRead', 'IO:WalSync']);
    assert.deepEqual(seriesColors,
        [eventColor(null, 'IO:DataFileRead'), eventColor(null, 'IO:WalSync')]);
    const sd = dataSeries(option);
    // FLIPPED in U2a: x is ms on the time axis (t=10 ns -> 1e-5 ms).
    assert.deepEqual(sd[0].data, [[0.00001, 0.4], [0.000011, 0.5]]);
    assert.deepEqual(sd[1].data, [[0.00001, 0.2], [0.000011, 0.25]]);
});

test('empty data: no crash, hasData false', () => {
    const m = buildAasOption({ buckets: [], max_aas: 0, bucket_ns: 0 }, { numCpus: 4 });
    assert.equal(m.hasData, false);
    // U2a: with no buckets and no window the axis degrades to a sane 1 ms
    // placeholder extent (0..1 in axis MS units); the empty state is never
    // mounted anyway (the view clears the chart).
    assert.equal(m.option.xAxis.min, 0);
    assert.equal(m.option.xAxis.max, 1);
});

/* FLIPPED in U2a: without opts.win the axis still falls back to the bucket
 * extent, but the pinned min/max are now MS on the type:'time' axis. With a
 * window (the production path) the axis pins to the window, not the last
 * bucket start — see the First-catch regression test below. */
test('x axis falls back to first..last bucket timestamp (ms) without a window', () => {
    const opt = buildAasOption({ buckets: classBuckets(5), max_aas: 1, bucket_ns: 1 },
        { numCpus: 1 }).option;
    assert.equal(opt.xAxis.min, 0.001);      // 1000 ns
    assert.equal(opt.xAxis.max, 0.001004);   // 1004 ns
});

test('aas values rounded to 4 decimals', () => {
    const data = { bucket_ns: 1, max_aas: 1, buckets: [
        { t: 1, cpu: 0.123456789 }] };
    const cpu = buildAasOption(data, { numCpus: 1 }).option.series.find(s => s.name === 'CPU');
    assert.equal(cpu.data[0][1], 0.1235);
});

/* U0 (review P2) pinned this invariant when the value axis ended at the LAST
 * BUCKET START: a mark at win.to sat past the axis max and ECharts DROPPED it
 * (an off-axis markLine is dropped, not clipped), so the escalation edge
 * never rendered. FLIPPED in U2a: the type:'time' axis is now PINNED to the
 * window, so win.to is on-axis BY CONSTRUCTION and the escalation edge sits
 * exactly at win.to == axis max. The every-mark-on-axis invariant survives as
 * the builder's defensive clamp (mark coordinates are in axis MS now). */
test('escalation + fidelity marks: every mark x within the axis range', () => {
    const data = { bucket_ns: 1, max_aas: 1, fidelity: 'sampled',
        buckets: classBuckets(5) };                    // bucket starts 1000..1004
    const { option } = buildAasOption(data, {
        numCpus: 2,
        win: { from: 990, to: 1100 },                  // win.to beyond the buckets
        escalationStatus: { tier: 'escalated', escalation_reason: 'manual',
            escalation_seconds_remaining: 30, observed_start_ns: 1002 },
    });
    const anno = option.series.find(s => s.name === AAS_ANNOTATION_SERIES);
    assert.ok(anno, 'annotation series carries the marks');
    const xs = [];
    anno.markLine.data.forEach(d => { if (d.xAxis != null) xs.push(d.xAxis); });
    anno.markArea.data.forEach(pair => pair.forEach(pt => {
        if (pt && pt.xAxis != null) xs.push(pt.xAxis);
    }));
    assert.ok(xs.length >= 5, 'escalation line + band + sampled band emitted');
    for (const x of xs) {
        assert.ok(x >= option.xAxis.min && x <= option.xAxis.max,
            'mark x ' + x + ' within [' + option.xAxis.min + ', ' + option.xAxis.max + ']');
    }
    // The escalation edge paints AT win.to — which IS the axis max now that
    // the axis pins to the window (U2a), no longer a clamped stand-in.
    const escLine = anno.markLine.data.find(d => d.xAxis != null);
    assert.equal(escLine.xAxis, option.xAxis.max);
    assert.equal(escLine.xAxis, 1100 / 1e6);           // win.to in axis ms
});

test('option root disables animation (U0: no replayed draw-in on refresh)', () => {
    const { option } = buildAasOption(
        { buckets: classBuckets(1), max_aas: 1, bucket_ns: 1 }, { numCpus: 1 });
    assert.equal(option.animation, false);
});

// ── U1 identity cluster (review P2) ──────────────────────────────────────────

/* Event-mode fixture: same three events, two different server rank orders.
 * The aas[] arrays are permuted consistently with the series order, so both
 * inputs describe the SAME underlying data. */
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

test('event mode: stack order + colors stable under permuted server rank', () => {
    const { a, b } = permutedInputs();
    const ma = buildAasOption(a, { numCpus: 0 });
    const mb = buildAasOption(b, { numCpus: 0 });
    // Identity order: class rank (IO before Lock), then event name — never
    // the server's per-window AAS rank.
    assert.deepEqual(ma.seriesNames,
        ['IO:DataFileRead', 'IO:WalSync', 'Lock:relation']);
    assert.deepEqual(ma.seriesNames, mb.seriesNames);
    assert.deepEqual(ma.seriesColors, mb.seriesColors);
    // The data follows the event, not the input slot.
    assert.deepEqual(dataSeries(ma.option).map(s => s.data),
                     dataSeries(mb.option).map(s => s.data));
});

test('event mode: stacked totals invariant under input order permutation', () => {
    const { a, b } = permutedInputs();
    const totals = (m, bi) => dataSeries(m.option)
        .reduce((acc, s) => acc + s.data[bi][1], 0);
    const ma = buildAasOption(a, { numCpus: 0 });
    const mb = buildAasOption(b, { numCpus: 0 });
    for (const bi of [0, 1]) {
        assert.ok(Math.abs(totals(ma, bi) - totals(mb, bi)) < 1e-9);
    }
    assert.ok(Math.abs(totals(ma, 0) - 0.6) < 1e-9);
    assert.ok(Math.abs(totals(ma, 1) - 1.5) < 1e-9);
});

test('annotation series: dataless, silent, excluded from legend + seriesNames', () => {
    const m = buildAasOption(
        { bucket_ns: 1, max_aas: 1, fidelity: 'sampled', buckets: classBuckets(3) },
        { numCpus: 4, win: { from: 990, to: 1010 } });
    const anno = m.option.series.find(s => s.name === AAS_ANNOTATION_SERIES);
    assert.ok(anno, 'annotation series present');
    assert.equal(anno.silent, true);
    assert.deepEqual(anno.data, []);
    assert.ok(anno.markArea.data.length >= 1, 'sampled band rides it');
    assert.ok(anno.markLine.data.length >= 1, 'N-CPUs line rides it');
    // Excluded from every legend surface: no legend/hover state can touch it.
    assert.ok(!m.seriesNames.includes(AAS_ANNOTATION_SERIES));
    assert.ok(!m.option.legend.data.includes(AAS_ANNOTATION_SERIES));
    // First in the array so marks paint behind the stacked areas.
    assert.equal(m.option.series[0], anno);
});

test('same event keeps the same color across builds and modes', () => {
    const ev = {
        bucket_ns: 1, max_aas: 1, breakdown: 'events',
        series: [{ name: 'IO:DataFileRead' }],
        buckets: [{ t: 1, aas: [0.5] }],
    };
    const c1 = buildAasOption(ev, {}).seriesColors[0];
    // A class-mode build in between must not perturb event identity colors.
    buildAasOption({ bucket_ns: 1, max_aas: 1, buckets: classBuckets(2) },
        { numCpus: 2 });
    const c2 = buildAasOption(ev, {}).seriesColors[0];
    assert.equal(c1, c2);
    // ... and equals the color service's answer under any class spelling.
    assert.equal(c1, eventColor(null, 'IO:DataFileRead'));
    assert.equal(c1, eventColor('io', 'IO:DataFileRead'));
    assert.equal(c1, eventColor('IO', 'IO:DataFileRead'));
});

test('eventColor: deterministic tint that keeps the class hue', () => {
    const io = WAIT_CLASSES.find(c => c.key === 'io').color;
    for (const name of ['IO:DataFileRead', 'IO:WalSync', 'IO:WalWrite']) {
        const c = eventColor(null, name);
        assert.match(c, /^rgb\(\d+,\d+,\d+\)$/);
        assert.equal(c, eventColor(null, name), 'stable across calls');
        // The tint varies lightness/saturation only — hue is the class hue.
        assert.ok(Math.abs(hueOf(c) - hueOf(io)) < 2,
            name + ' hue ' + hueOf(c) + ' ~= IO hue ' + hueOf(io));
    }
    // Unknown-class names fall back to the achromatic Unknown gray — never a
    // hue that means some class elsewhere.
    const g = eventColor(null, 'Bogus:Whatever');
    const gm = /^rgb\((\d+),(\d+),(\d+)\)$/.exec(g);
    assert.ok(gm && gm[1] === gm[2] && gm[2] === gm[3], 'gray stays gray: ' + g);
});

// ── U2a camera-lite: time axis + gesture dataZoom (Track U) ──────────────────

/* FIRST-CATCH REGRESSION (docs/VISUAL_CHECKLIST.md "First catch"): the AAS
 * x-axis MUST be type:'time' with an explicitly pinned [min,max]. On a
 * type:'value' x-axis, ECharts drops every stacked layer above series[0] at
 * ANY x magnitude (verified in real Chromium against the vendored 5.5.1
 * bundle, 2026-07-31) — the production chart had only ever painted the first
 * series' area. Value axes are forbidden here; this test pins the fix. */
test('x axis is type time, pinned to the window (First-catch regression)', () => {
    const win = { from: 1_700_000_000_000_000_000, to: 1_700_000_900_000_000_000 };
    const { option } = buildAasOption(
        { bucket_ns: 3e9, max_aas: 1, buckets: classBuckets(3) },
        { numCpus: 4, win });
    assert.equal(option.xAxis.type, 'time');
    assert.equal(option.xAxis.min, win.from / 1e6);   // pinned, in UNIX ms
    assert.equal(option.xAxis.max, win.to / 1e6);
});

/* Constraint B: axis labels and the tooltip time must be TZ-STABLE (explicit
 * UTC formatters) — never ECharts' locale/TZ-dependent defaults — so pixel
 * baselines are identical on any machine. */
test('axis label + tooltip formatters render UTC regardless of host TZ', () => {
    const { option } = buildAasOption(
        { bucket_ns: 60e9, max_aas: 1, buckets: classBuckets(2) },
        { numCpus: 1 });
    const ms = Date.UTC(2026, 0, 15, 12, 34, 56);
    assert.equal(option.xAxis.axisLabel.formatter(ms), '12:34:56');
    const html = aasTooltip([{ seriesName: 'CPU', value: [ms, 1.5], color: '#1' }], 60e9);
    assert.ok(html.includes('12:34:56'), 'tooltip renders the UTC time: ' + html);
});

/* The inside dataZoom is the camera's gesture source (U2a): wheel =
 * cursor-anchored zoom, shift+drag = pan (plain drag stays with the
 * brush-select overlay — constraint C), filterMode 'none' so the cached
 * strip is never filtered and the pinned axis + [startValue,endValue] do the
 * visible cropping. */
test('inside dataZoom: gesture config + window crop pinned to win', () => {
    const win = { from: 2e15, to: 3e15 };
    const { option } = buildAasOption(
        { bucket_ns: 1e9, max_aas: 1, buckets: classBuckets(2) },
        { numCpus: 1, win });
    const dz = option.dataZoom[0];
    assert.equal(dz.type, 'inside');
    assert.equal(dz.filterMode, 'none');
    assert.equal(dz.zoomOnMouseWheel, true);
    assert.equal(dz.moveOnMouseMove, 'shift');
    assert.equal(dz.moveOnMouseWheel, false);
    assert.equal(dz.preventDefaultMouseMove, true);
    assert.equal(dz.startValue, win.from / 1e6);
    assert.equal(dz.endValue, win.to / 1e6);
    assert.equal(dz.minValueSpan, 1);   // the camera's MIN_SPAN_NS floor (1 ms)
});

/* opts.axis (the camera's quantized 3x strip skirt) widens the pinned axis
 * beyond the window — that skirt is what gives pan/zoom-out gestures room
 * (the dataZoom window cannot leave the axis extent). The window keeps doing
 * the visible crop, and the escalation edge stays at the WINDOW's live edge,
 * never drifting to the skirt edge (constraint D). */
test('opts.axis widens the pinned axis; window still crops; marks stay put', () => {
    const win = { from: 2e15, to: 3e15 };
    const axis = { from: 1e15, to: 4e15 };            // 3x strip skirt
    const m = buildAasOption(
        { bucket_ns: 1e9, max_aas: 1, fidelity: 'sampled', buckets: classBuckets(2) },
        { numCpus: 2, win, axis,
          escalationStatus: { tier: 'escalated', escalation_reason: 'manual',
              escalation_seconds_remaining: 30, observed_start_ns: 2.5e15 } });
    assert.equal(m.option.xAxis.min, axis.from / 1e6);
    assert.equal(m.option.xAxis.max, axis.to / 1e6);
    assert.equal(m.option.dataZoom[0].startValue, win.from / 1e6);
    assert.equal(m.option.dataZoom[0].endValue, win.to / 1e6);
    // ns geometry reported back for the mount layer's percent->time mapping.
    assert.deepEqual(m.axisRange, { from: axis.from, to: axis.to });
    assert.deepEqual(m.window, { from: win.from, to: win.to });
    // Escalation edge at the window's live edge (interior of the axis now).
    const anno = m.option.series.find(s => s.name === AAS_ANNOTATION_SERIES);
    const escLine = anno.markLine.data.find(d => d.xAxis != null);
    assert.equal(escLine.xAxis, win.to / 1e6);
    // Sampled shading covers the FULL rendered extent (any data the camera
    // can pan onto mid-gesture is shaded — honesty geometry, constraint D).
    const band = anno.markArea.data[0];
    assert.equal(band[0].xAxis, axis.from / 1e6);
    assert.equal(band[1].xAxis, axis.to / 1e6);
});

/* The axis must always CONTAIN the window (axis = opts.axis ∪ win): a
 * mis-passed narrower axis would let ECharts clamp the dataZoom crop away
 * from the camera window. */
test('axis extent is the union of opts.axis and win (invariant)', () => {
    const win = { from: 2e15, to: 3e15 };
    const m = buildAasOption(
        { bucket_ns: 1e9, max_aas: 1, buckets: classBuckets(2) },
        { numCpus: 1, win, axis: { from: 2.2e15, to: 2.8e15 } });   // narrower
    assert.equal(m.option.xAxis.min, win.from / 1e6);
    assert.equal(m.option.xAxis.max, win.to / 1e6);
});

test('tooltip totals visible series and orders top-of-stack first', () => {
    const params = [
        { seriesName: 'CPU', value: [1000, 1.0], color: '#1' },
        { seriesName: 'IO', value: [1000, 0.5], color: '#2' },
        { seriesName: 'Idle', value: [1000, 0], color: '#3' },  // dropped (<=0.001)
    ];
    const html = aasTooltip(params, 1000000000);
    assert.ok(html.includes('Total AAS: <b>1.50</b>'));
    // IO is later in params (top of stack) -> appears first after reverse
    assert.ok(html.indexOf('IO') < html.indexOf('CPU'));
    assert.ok(!html.includes('Idle'));
});

// U2a review F1: formatters alone don't pin tick POSITIONS — ECharts time-scale
// tick generation is local-calendar unless option.useUTC is set. Guard the root.
test('option root pins useUTC (tick positions TZ-stable)', () => {
  const { option } = buildAasOption(
    { buckets: classBuckets(1), max_aas: 1, bucket_ns: 1 }, { numCpus: 4 });
  assert.strictEqual(option.useUTC, true);
});
