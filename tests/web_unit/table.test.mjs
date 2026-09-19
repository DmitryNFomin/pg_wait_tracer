/* Node unit tests for the shared table builder (lib/table.js) + the migrated
 * table configs (overview, events). Proves sort, cell formatting, row-class and
 * the drill-intent descriptors are correct without a browser.
 *
 * U2 additions: the stacked-bar truth model (review P11 — below-threshold vs
 * other split, pre-normalized widths, separators), the truncation-row model
 * (review P10), per-cell drill intents (review P3 wire 6), and the eventColor
 * identity tints in the pctBar sites (U1-review F4).
 */
import { test } from 'node:test';
import assert from 'node:assert/strict';
import {
    buildTableModel, buildTruncationRow, buildBarSegments,
    stackedBar, eventStackedBar,
    BAR_MIN_SEG_PCT, BAR_BELOW_COLOR, BAR_OTHER_COLOR,
} from '../../web/static/lib/table.js';
import { overviewConfig, eventsConfig } from '../../web/static/lib/builders/table-configs.js';
import { buildSummary } from '../../web/static/views/overview.js';
import { WAIT_CLASSES, classColor, eventColor, fmtMs } from '../../web/static/lib/format.js';

test('overview: rows keep server order (no sort) + clickable indent-1', () => {
    const rows = [
        { indent: 0, name: 'DB Time', ms: 12500, pct: 100, aas: 3.47 },
        { indent: 1, name: 'CPU*', ms: 4800, pct: 38.4, aas: 1.33 },
        { indent: 2, name: 'IO:DataFileRead', ms: 2100, pct: 16.8, aas: 0.58 },
    ];
    const model = buildTableModel(overviewConfig, rows, null);
    assert.equal(model.rows.length, 3);
    assert.equal(model.headers[0].label, 'Stat Name');
    // order preserved
    assert.ok(model.rows[0].cells[0].html.includes('DB Time'));
    // indent-1 is clickable, indent-2 is not
    assert.ok(model.rows[1].cls.includes('clickable'));
    assert.ok(!model.rows[2].cls.includes('clickable'));
    // DB Time pct shown as plain pct, class rows as bar
    assert.ok(model.rows[0].cells[2].html.includes('100.0%'));
    assert.ok(model.rows[1].cells[2].html.includes('pct-bar'));
});

test('overview onClick: indent-1 -> class drill intent, strips asterisk', () => {
    assert.deepEqual(
        overviewConfig.onClick({ indent: 1, name: 'CPU*' }),
        { filterKey: 'class', filterValue: 'CPU', label: 'CPU' });
    assert.equal(overviewConfig.onClick({ indent: 0, name: 'DB Time' }), null);
    assert.equal(overviewConfig.onClick({ indent: 2, name: 'IO:WalSync' }), null);
});

test('events: descending sort by count', () => {
    const rows = [
        { name: 'A', event_id: 1, count: 10, total_ms: 1, avg_us: 1, p50_us: 1,
          p95_us: 1, p99_us: 1, max_us: 1, pct: 1, aas: 0.1 },
        { name: 'B', event_id: 2, count: 30, total_ms: 1, avg_us: 1, p50_us: 1,
          p95_us: 1, p99_us: 1, max_us: 1, pct: 1, aas: 0.1 },
        { name: 'C', event_id: 3, count: 20, total_ms: 1, avg_us: 1, p50_us: 1,
          p95_us: 1, p99_us: 1, max_us: 1, pct: 1, aas: 0.1 },
    ];
    const model = buildTableModel(eventsConfig, rows, { key: 'count', asc: false });
    const order = model.rows.map(r => r.row.name);
    assert.deepEqual(order, ['B', 'C', 'A']);
    // sort arrow on the count header
    const countHdr = model.headers.find(h => h.key === 'count');
    assert.ok(countHdr.arrow.includes('▼'));
});

test('events: ascending sort flips order', () => {
    const rows = [
        { name: 'A', count: 10 }, { name: 'B', count: 30 }, { name: 'C', count: 20 },
    ].map(r => ({ ...r, event_id: 0, total_ms: 0, avg_us: 0, p50_us: 0, p95_us: 0,
        p99_us: 0, max_us: 0, pct: 0, aas: 0 }));
    const model = buildTableModel(eventsConfig, rows, { key: 'count', asc: true });
    assert.deepEqual(model.rows.map(r => r.row.name), ['A', 'C', 'B']);
});

test('events onClick: event_id drill intent', () => {
    assert.deepEqual(
        eventsConfig.onClick({ event_id: 0x01000015, name: 'IO:DataFileRead' }),
        { filterKey: 'event_id', filterValue: 0x01000015, label: 'IO:DataFileRead' });
});

test('buildSummary: exact metric set + values', () => {
    const data = { wall_ms: 3600000, rows: [
        { indent: 0, name: 'DB Time', ms: 12500, pct: 100, aas: 3.47 },
        { indent: 1, name: 'CPU*', ms: 4800 },
        { indent: 0, name: 'Idle', ms: 45000 },
    ] };
    const s = buildSummary(data, 4);
    assert.deepEqual(s, [
        { label: 'DB Time', value: '12.5s' },
        { label: 'Wall', value: '3600.0s' },
        { label: 'AAS', value: '3.47' },
        { label: 'Idle', value: '45.0s' },
        { label: 'CPUs', value: '4' },
    ]);
});

test('buildSummary: empty data -> empty metrics', () => {
    assert.deepEqual(buildSummary(null, 4), []);
    assert.deepEqual(buildSummary({ rows: [] }, 4), []);
});

// ── U2 review P11: stacked-bar truth ────────────────────────────────────────

test('bar segments: below-threshold and other are SEPARATE labeled facts', () => {
    // A=70%, B=0.2% (< the 0.3% display threshold), and 29.8% of the total
    // is not covered by the listed events at all. Pre-U2 both remainders
    // fused into one gray "Other".
    const segs = buildBarSegments(
        [{ name: 'A', ms: 700, color: 'rgb(1,2,3)' },
         { name: 'B', ms: 2, color: 'rgb(4,5,6)' }],
        1000, { threshold: 0.3, fmt: fmtMs, noun: 'events',
                otherLabel: 'Other events (beyond the listed top events)' });
    assert.deepEqual(segs.map(s => s.kind), ['item', 'below', 'other']);
    const below = segs[1], other = segs[2];
    assert.equal(below.color, BAR_BELOW_COLOR);
    assert.equal(other.color, BAR_OTHER_COLOR);
    assert.notEqual(BAR_BELOW_COLOR, BAR_OTHER_COLOR);
    // Honest labels: the below segment counts its members; other names the gap.
    assert.ok(below.title.includes('1 events below the 0.3% display threshold'),
        below.title);
    assert.ok(below.title.includes('0.2%'), below.title);
    assert.ok(other.title.includes('Other events'), other.title);
    assert.ok(other.title.includes('29.8%'), other.title);
});

test('bar segments: zero-ms entries are absent, not "below threshold"', () => {
    const segs = buildBarSegments(
        [{ name: 'A', ms: 500, color: 'c' }, { name: 'B', ms: 0, color: 'c' }],
        500, { threshold: 0.5 });
    // No below segment for the zero entry; A covers 100% -> no other either.
    assert.deepEqual(segs.map(s => s.kind), ['item']);
});

test('bar widths: floor applied, deficit paid by large segments, sum stays 100', () => {
    // B (0.4%) is visible but sub-floor; its floor width comes OUT of the
    // large segments deterministically — not renegotiated by CSS flexing.
    const segs = buildBarSegments(
        [{ name: 'A', ms: 970, color: 'c' }, { name: 'B', ms: 4, color: 'c' }],
        1000, { threshold: 0.3 });
    assert.deepEqual(segs.map(s => s.kind), ['item', 'item', 'other']);
    for (const s of segs) assert.ok(s.width >= BAR_MIN_SEG_PCT - 1e-9,
        `width ${s.width} under floor`);
    const sum = segs.reduce((a, s) => a + s.width, 0);
    assert.ok(Math.abs(sum - 100) < 0.05, `widths sum to ${sum}`);
    // The floor was paid for by A; A's TRUE pct stays in the title.
    assert.ok(segs[0].width < 97 && segs[0].width > 95, String(segs[0].width));
    assert.equal(segs[0].pct, 97);
    assert.ok(segs[0].title.includes('(97.0%)'), segs[0].title);
});

test('bar widths: shares summing past 100% are scaled, titles keep true pct', () => {
    // Defensive: if listed ms overlap/exceed the row total the track must
    // not overflow (the old flex renormalization hid this class of lie).
    const segs = buildBarSegments(
        [{ name: 'A', ms: 600, color: 'c' }, { name: 'B', ms: 500, color: 'c' }],
        1000, { threshold: 0.5 });
    const sum = segs.reduce((a, s) => a + s.width, 0);
    assert.ok(sum <= 100.05, `widths sum to ${sum}`);
    assert.ok(segs[0].title.includes('(60.0%)'), segs[0].title);
    assert.ok(segs[1].title.includes('(50.0%)'), segs[1].title);
});

test('bar widths: degenerate many-tiny case falls back to equal widths', () => {
    const items = [];
    for (let i = 0; i < 80; i++) items.push({ name: 'E' + i, ms: 1, color: 'c' });
    const segs = buildBarSegments(items, 80, { threshold: 0.5 });
    // 80 segments x 1.5% floor > 100% -> equal split, deterministic.
    const sum = segs.reduce((a, s) => a + s.width, 0);
    assert.ok(Math.abs(sum - 100) < 0.5, `widths sum to ${sum}`);
    assert.ok(segs.every(s => Math.abs(s.width - segs[0].width) < 0.02));
});

test('stackedBar HTML: pre-normalized flex widths, min-width:0, 1px separators', () => {
    const classes = WAIT_CLASSES.map(() => 0);
    classes[0] = 4800;   // CPU 38.4%
    classes[1] = 2100;   // IO 16.8%  -> unattributed remainder 44.8%
    const html = stackedBar(classes, 12500, WAIT_CLASSES, fmtMs);
    const segs = html.match(/class="bar-seg/g) || [];
    assert.equal(segs.length, 3);
    // Every segment: builder-owned width + the CSS min-width:2px defeated.
    assert.equal((html.match(/flex:0 0 /g) || []).length, 3);
    assert.equal((html.match(/min-width:0/g) || []).length, 3);
    // 1px inset separator on every segment except the last.
    assert.equal((html.match(/box-shadow:inset -1px 0 0/g) || []).length, 2);
    // The remainder is the UNATTRIBUTED kind here (nothing below threshold).
    assert.ok(html.includes('bar-seg-other'), html);
    assert.ok(!html.includes('bar-seg-below'), html);
    assert.ok(html.includes('Unattributed'), html);
    assert.ok(html.includes('44.8%'), html);
});

test('eventStackedBar HTML: identity tints + separators + honest remainders', () => {
    const events = [
        { name: 'IO:DataFileRead', ms: 700 },
        { name: 'Lock:relation', ms: 2 },     // 0.2% -> below threshold
    ];
    const html = eventStackedBar(events, 1000, eventColor, fmtMs);
    // U1 color service: the segment carries the event's tint, and the same
    // call in any other view yields the same color.
    assert.ok(html.includes(eventColor(null, 'IO:DataFileRead')), html);
    assert.ok(html.includes('bar-seg-below'), html);
    assert.ok(html.includes('bar-seg-other'), html);
    assert.ok(html.includes('below the 0.3% display threshold'), html);
    assert.ok(html.includes('Other events (beyond the listed top events)'), html);
    // 3 segments -> 2 separators.
    assert.equal((html.match(/box-shadow:inset -1px 0 0/g) || []).length, 2);
});

test('bars: empty/zero-total inputs render nothing (unchanged contract)', () => {
    assert.equal(stackedBar(null, 100, WAIT_CLASSES, fmtMs), '');
    assert.equal(stackedBar([1, 2], 0, WAIT_CLASSES, fmtMs), '');
    assert.equal(eventStackedBar([], 100, eventColor, fmtMs), '');
    assert.equal(eventStackedBar([{ name: 'A', ms: 1 }], 0, eventColor, fmtMs), '');
});

// ── U2 review P10: truncation-row model ─────────────────────────────────────

test('buildTruncationRow: nothing without an explicit server truncated flag', () => {
    // The response carries no truncation signal today (recorded server gap);
    // implying truncation the server did not state would itself be a lie.
    assert.equal(buildTruncationRow(null), null);
    assert.equal(buildTruncationRow({ rows: [1, 2, 3] }), null);
    assert.equal(buildTruncationRow({ rows: [], truncated: false }), null);
});

test('buildTruncationRow: count only when derivable, never invented', () => {
    // truncated + total_count (the session_timeline field pair) -> exact N.
    assert.deepEqual(
        buildTruncationRow({ truncated: true, total_count: 15, rows: [1, 2, 3] }),
        { omitted: 12, text: '… 12 more below threshold' });
    // truncated alone -> qualitative, no invented N.
    assert.deepEqual(
        buildTruncationRow({ truncated: true, rows: [1, 2, 3] }),
        { omitted: null, text: '… more not shown' });
    // Inconsistent total_count (<= shown) -> qualitative, never negative N.
    assert.deepEqual(
        buildTruncationRow({ truncated: true, total_count: 2, rows: [1, 2, 3] }),
        { omitted: null, text: '… more not shown' });
});

// ── U2 review P3 wire 6: percentile cells carry the histogram pivot ─────────

// Column order: name,count,total_ms,avg_us,p50,p95,p99,max,pct,aas.
const P50_COL = 4, P95_COL = 5, P99_COL = 6, MAX_COL = 7;

function evRow(over) {
    return Object.assign({
        name: 'IO:DataFileRead', event_id: 0x01000015, count: 100,
        total_ms: 500, avg_us: 10, p50_us: 8, p95_us: 40, p99_us: 90,
        max_us: 200, pct: 10, aas: 0.1,
    }, over);
}

test('exact event row: P50/P95/P99 cells are drillable histogram pivots', () => {
    const m = buildTableModel(eventsConfig, [evRow({})], null);
    for (const ci of [P50_COL, P95_COL, P99_COL]) {
        const cell = m.rows[0].cells[ci];
        assert.ok(cell.cls.includes('drillable'), `col ${ci}: ${cell.cls}`);
        assert.deepEqual(cell.intent, {
            pivot: 'histogram-event', filterKey: 'event_id',
            filterValue: 0x01000015, label: 'IO:DataFileRead',
        });
        assert.ok(cell.html.includes('cell-drill'), cell.html);
    }
    // Max is not a percentile cell: no drill.
    assert.equal(m.rows[0].cells[MAX_COL].intent, null);
    assert.ok(!m.rows[0].cells[MAX_COL].cls.includes('drillable'));
});

test('sampled-only row (null percentiles): cells render — and are NOT drillable', () => {
    const m = buildTableModel(eventsConfig, [evRow({
        avg_us: null, p50_us: null, p95_us: null, p99_us: null, max_us: null,
    })], null);
    for (const ci of [P50_COL, P95_COL, P99_COL]) {
        const cell = m.rows[0].cells[ci];
        assert.equal(cell.intent, null);
        assert.ok(!cell.cls.includes('drillable'));
        assert.ok(cell.html.includes('—'), cell.html);
        assert.ok(!cell.html.includes('cell-drill'), cell.html);
    }
});

test('CPU* (event_id 0) percentile cells are not drillable (not a wait event)', () => {
    const m = buildTableModel(eventsConfig,
        [evRow({ name: 'CPU*', event_id: 0 })], null);
    for (const ci of [P50_COL, P95_COL, P99_COL]) {
        assert.equal(m.rows[0].cells[ci].intent, null);
        assert.ok(!m.rows[0].cells[ci].html.includes('cell-drill'));
    }
});

// ── #103: percentiles that land in the open-ended top histogram bucket ─────
//
// The server derives P50/P95/P99 from a 16-bucket latency histogram whose top
// bucket holds everything above 16.384 ms; a percentile there is a LOWER
// BOUND. Real capture printed "Lock:relation Avg 1.8s / Max 2.6s /
// P50=P95=P99=16.4ms" — a mean 100x above the stated P99.

/* The cell's visible text, with markup (and its title attribute) stripped. */
const cellText = (html) => html.replace(/<[^>]*>/g, '');

test('#103 overflow percentile renders ">=", never an exact-looking number', () => {
    const m = buildTableModel(eventsConfig, [evRow({
        p50_us: 16384, p95_us: 16384, p99_us: 16384, max_us: 2600000,
        p50_overflow: true, p95_overflow: true, p99_overflow: true,
    })], null);
    for (const ci of [P50_COL, P95_COL, P99_COL]) {
        const html = m.rows[0].cells[ci].html;
        // The rendered text is the bound, never the bare number.
        assert.equal(cellText(html), '\u226516.4ms', `col ${ci}: ${html}`);
        // The disclosure is in words, not just the drill affordance: it
        // leads with the fact and points at the column that has the tail.
        assert.ok(/title="At least 16\.4ms\./.test(html), html);
        assert.ok(/title="[^"]*see Max for the real tail[^"]*"/.test(html), html);
    }
    // Max is the exact tail and stays exact.
    assert.ok(m.rows[0].cells[MAX_COL].html.includes('2.6s'),
        m.rows[0].cells[MAX_COL].html);
});

test('#103 a normal-bucket percentile is unchanged (no ">=", same tooltip)', () => {
    const m = buildTableModel(eventsConfig, [evRow({
        p50_us: 8, p95_us: 40, p99_us: 8192,
        p50_overflow: false, p95_overflow: false, p99_overflow: false,
    })], null);
    const p99 = m.rows[0].cells[P99_COL].html;
    assert.equal(cellText(p99), '8.2ms', p99);
    assert.ok(p99.includes('View the latency distribution of'), p99);
    assert.equal(cellText(m.rows[0].cells[P50_COL].html), '8\u00b5s',
        m.rows[0].cells[P50_COL].html);
});

test('#103 rows with no overflow flags at all behave exactly as before', () => {
    // Old server / compare-baseline rows: absent key is not an overflow.
    const m = buildTableModel(eventsConfig, [evRow({ p95_us: 16384 })], null);
    const html = m.rows[0].cells[P95_COL].html;
    assert.ok(html.includes('16.4ms') && !html.includes('\u2265'), html);
});

test('#103 an overflow cell keeps its drill affordance and pivot intent', () => {
    const m = buildTableModel(eventsConfig,
        [evRow({ p95_us: 16384, p95_overflow: true })], null);
    const cell = m.rows[0].cells[P95_COL];
    assert.ok(cell.cls.includes('drillable'), cell.cls);
    assert.ok(cell.html.includes('cell-drill'), cell.html);
    assert.deepEqual(cell.intent, {
        pivot: 'histogram-event', filterKey: 'event_id',
        filterValue: 0x01000015, label: 'IO:DataFileRead',
    });
    assert.ok(/title="[^"]*latency distribution[^"]*"/.test(cell.html), cell.html);
});

test('#103 a NON-drillable overflow cell (CPU*) still discloses the bound', () => {
    const m = buildTableModel(eventsConfig, [evRow({
        name: 'CPU*', event_id: 0, p99_us: 16384, p99_overflow: true,
    })], null);
    const cell = m.rows[0].cells[P99_COL];
    assert.equal(cell.intent, null);
    assert.ok(!cell.cls.includes('drillable'), cell.cls);
    assert.ok(!cell.html.includes('cell-drill'), cell.html);
    assert.ok(cell.html.includes('\u226516.4ms'), cell.html);
    assert.ok(/title="At least 16\.4ms\./.test(cell.html), cell.html);
    // ...and it carries the quieter .pctl-overflow affordance (style.css),
    // not the drill's inline underline, so the bound still reads as
    // inspectable on a row that cannot be drilled.
    assert.ok(cell.html.includes('class="pctl-overflow"'), cell.html);
    assert.ok(!cell.html.includes('border-bottom'), cell.html);
});

test('#103 a null percentile ignores an overflow flag and stays "—"', () => {
    const m = buildTableModel(eventsConfig,
        [evRow({ p95_us: null, p95_overflow: true })], null);
    const html = m.rows[0].cells[P95_COL].html;
    assert.ok(html.includes('\u2014'), html);
    assert.ok(!html.includes('\u2265'), html);
});

test('#103 sort treats ">=16.4ms" as 16.4ms (the shown bound), not +Infinity', () => {
    // Both rows report the same P95 number; only one of them is an overflow.
    // Sorting on p95_us must therefore leave their relative order alone — a
    // +Infinity reading would hoist the overflow row above an equal, visible
    // 16.4ms. The true tail is ranked by Max, which differs here.
    const rows = [
        evRow({ name: 'IO:DataFileRead', event_id: 0x01000015,
                p95_us: 16384, max_us: 16000 }),
        evRow({ name: 'Lock:relation', event_id: 0x03000000,
                p95_us: 16384, p95_overflow: true, max_us: 2600000 }),
    ];
    const desc = buildTableModel(eventsConfig, rows, { key: 'p95_us', asc: false });
    assert.deepEqual(desc.rows.map(r => r.row.name),
        ['IO:DataFileRead', 'Lock:relation']);
    const asc = buildTableModel(eventsConfig, rows, { key: 'p95_us', asc: true });
    assert.deepEqual(asc.rows.map(r => r.row.name),
        ['IO:DataFileRead', 'Lock:relation']);
    // Max still ranks the real tail.
    const byMax = buildTableModel(eventsConfig, rows, { key: 'max_us', asc: false });
    assert.deepEqual(byMax.rows.map(r => r.row.name),
        ['Lock:relation', 'IO:DataFileRead']);
});

// ── U2 (U1-review F4): eventColor identity tints in the pctBar sites ────────

test('events %DB bar uses the event identity tint, not the flat class hue', () => {
    const m = buildTableModel(eventsConfig, [evRow({})], null);
    const html = m.rows[0].cells[8].html;   // pct column
    assert.ok(html.includes(eventColor(null, 'IO:DataFileRead')), html);
});

test('overview %DB bars: class rows keep class hue, event rows take the tint', () => {
    const rows = [
        { indent: 1, name: 'IO', ms: 100, pct: 20, aas: 0.2 },
        { indent: 2, name: 'IO:DataFileRead', ms: 80, pct: 16, aas: 0.16 },
    ];
    const m = buildTableModel(overviewConfig, rows, null);
    assert.ok(m.rows[0].cells[2].html.includes(classColor('IO')));
    assert.ok(m.rows[1].cells[2].html.includes(eventColor(null, 'IO:DataFileRead')));
});
