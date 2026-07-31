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

test('class mode: one series per wait class, x=ts y=aas', () => {
    const data = { bucket_ns: 1, max_aas: 2.0, buckets: classBuckets(3) };
    const { option, seriesNames, seriesColors } = buildAasOption(data, { numCpus: 4 });

    assert.equal(dataSeries(option).length, WAIT_CLASSES.length);
    assert.deepEqual(seriesNames, WAIT_CLASSES.map(c => c.label));
    assert.deepEqual(seriesColors, WAIT_CLASSES.map(c => c.color));

    const cpu = option.series.find(s => s.name === 'CPU');
    assert.deepEqual(cpu.data, [[1000, 1], [1001, 1], [1002, 1]]);
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
    assert.deepEqual(sd[0].data, [[10, 0.4], [11, 0.5]]);
    assert.deepEqual(sd[1].data, [[10, 0.2], [11, 0.25]]);
});

test('empty data: no crash, hasData false', () => {
    const m = buildAasOption({ buckets: [], max_aas: 0, bucket_ns: 0 }, { numCpus: 4 });
    assert.equal(m.hasData, false);
    assert.equal(m.option.xAxis.min, 0);
    assert.equal(m.option.xAxis.max, 1);
});

test('x axis spans first..last bucket timestamp', () => {
    const opt = buildAasOption({ buckets: classBuckets(5), max_aas: 1, bucket_ns: 1 },
        { numCpus: 1 }).option;
    assert.equal(opt.xAxis.min, 1000);
    assert.equal(opt.xAxis.max, 1004);
});

test('aas values rounded to 4 decimals', () => {
    const data = { bucket_ns: 1, max_aas: 1, buckets: [
        { t: 1, cpu: 0.123456789 }] };
    const cpu = buildAasOption(data, { numCpus: 1 }).option.series.find(s => s.name === 'CPU');
    assert.equal(cpu.data[0][1], 0.1235);
});

/* U0 (review P2): the x-axis ends at the LAST BUCKET START, not win.to — a
 * mark placed at win.to sat past the axis max and ECharts DROPPED it, so the
 * escalation edge never rendered in any state. Invariant: every emitted mark
 * x coordinate lies within the axis range, and the escalation edge is pinned
 * at the axis max (clamped from the off-axis win.to). U1 moved every mark
 * from series[0] onto the dedicated annotation series; the on-axis invariant
 * is unchanged. */
test('escalation + fidelity marks: every mark x within the axis range', () => {
    const data = { bucket_ns: 1, max_aas: 1, fidelity: 'sampled',
        buckets: classBuckets(5) };                    // bucket starts 1000..1004
    const { option } = buildAasOption(data, {
        numCpus: 2,
        win: { from: 990, to: 1100 },                  // win.to past the axis max
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
    // The escalation edge paints AT the axis max (the last bucket start).
    const escLine = anno.markLine.data.find(d => d.xAxis != null);
    assert.equal(escLine.xAxis, option.xAxis.max);
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
