/* Node unit tests for the pure events builder (views/events.js) over the shared
 * eventsConfig (lib/builders/table-configs.js). Proves row shape, sort
 * correctness, drill-intent, and edge cases without a browser.
 *
 * U2 (review P10): the model now also carries the pane fidelity badge, the
 * percentile-basis footnote and the truncation-row model — all derived from
 * the response, never fabricated.
 */
import { test } from 'node:test';
import assert from 'node:assert/strict';
import { buildEventsModel, createEventsView } from '../../web/static/views/events.js';
import { eventsConfig, PCTL_UNRESOLVED_TEXT } from '../../web/static/lib/builders/table-configs.js';
import { createStableOrder } from '../../web/static/lib/table.js';
import {
    buildPaneFidelity, paneFidelityBadgeHtml,
    buildPercentileBasis, percentileBasisHtml,
} from '../../web/static/lib/panels.js';
import {
    SAMPLED_BAND_COLOR, SAMPLED_BORDER, MIXED_BAND_COLOR,
} from '../../web/static/lib/builders/fidelity.js';

function ev(over) {
    return Object.assign({
        name: 'X', event_id: 0, count: 0, total_ms: 0, avg_us: 0,
        p50_us: 0, p95_us: 0, p99_us: 0, max_us: 0, pct: 0, aas: 0,
    }, over);
}

// #440: a minimal fake view-manager ctx -- just enough for build(data, ctx)
// (getSort/timeRange/filters.snapshot), no transport/DOM needed.
function fakeCtx(sortState) {
    return {
        getSort: () => sortState,
        // #474: orderResetKey(ctx) calls ctx.isLiveTick() unconditionally;
        // this fixture is never live, so it is always false.
        isLiveTick: () => false,
        timeRange: { from: 0, to: 1000 },
        filters: { snapshot: () => ({}) },
    };
}

test('no sort: keeps server order; every row clickable', () => {
    const data = { rows: [
        ev({ name: 'CPU*', count: 100 }),
        ev({ name: 'IO:DataFileRead', event_id: 0x01000015, count: 50 }),
        ev({ name: 'Lock:relation', event_id: 0x03000000, count: 30 }),
    ] };
    const m = buildEventsModel(data, null);
    assert.equal(m.hasRows, true);
    assert.equal(m.table.rows.length, 3);
    // order preserved
    assert.deepEqual(m.table.rows.map(r => r.row.name),
        ['CPU*', 'IO:DataFileRead', 'Lock:relation']);
    // all clickable for drill-down
    assert.ok(m.table.rows.every(r => r.cls.includes('clickable')));
    // class dot + escaped name in the first cell
    assert.ok(m.table.rows[0].cells[0].html.includes('class-dot'));
    assert.ok(m.table.rows[0].cells[0].html.includes('CPU*'));
});

test('descending sort by count puts largest first + shows ▼ arrow', () => {
    const data = { rows: [
        ev({ name: 'A', count: 10 }), ev({ name: 'B', count: 30 }),
        ev({ name: 'C', count: 20 }),
    ] };
    const m = buildEventsModel(data, { key: 'count', asc: false });
    assert.deepEqual(m.table.rows.map(r => r.row.name), ['B', 'C', 'A']);
    assert.ok(m.table.headers.find(h => h.key === 'count').arrow.includes('▼'));
});

test('ascending sort flips order + shows ▲ arrow', () => {
    const data = { rows: [
        ev({ name: 'A', count: 10 }), ev({ name: 'B', count: 30 }),
        ev({ name: 'C', count: 20 }),
    ] };
    const m = buildEventsModel(data, { key: 'count', asc: true });
    assert.deepEqual(m.table.rows.map(r => r.row.name), ['A', 'C', 'B']);
    assert.ok(m.table.headers.find(h => h.key === 'count').arrow.includes('▲'));
});

test('drill intent targets event_id with the event name as label', () => {
    assert.deepEqual(
        eventsConfig.onClick(ev({ event_id: 0x01000015, name: 'IO:DataFileRead' })),
        { filterKey: 'event_id', filterValue: 0x01000015, label: 'IO:DataFileRead' });
});

test('empty data -> hasRows false, zero rows', () => {
    assert.equal(buildEventsModel(null, null).hasRows, false);
    assert.equal(buildEventsModel({ rows: [] }, null).hasRows, false);
    assert.equal(buildEventsModel({ rows: [] }, null).table.rows.length, 0);
});

test('single row -> stable', () => {
    const m = buildEventsModel({ rows: [ev({ name: 'Solo', count: 1 })] },
        { key: 'count', asc: false });
    assert.equal(m.table.rows.length, 1);
    assert.equal(m.table.rows[0].row.name, 'Solo');
});

test('B-only compare failure falls back to the ordinary A events model', () => {
    const a = { fidelity: 'exact', rows: [ev({ name: 'IO:A', total_ms: 250 })] };
    const m = buildEventsModel({ compare: true, a, b: null,
        baselineUnavailable: true }, null);
    assert.equal(m.baselineUnavailable, true);
    assert.equal(m.compare, undefined);
    assert.deepEqual(m.table.rows.map(r => r.row.name), ['IO:A']);
    assert.ok(m.table.headers.some(h => h.label === 'Wait Event'));
    assert.ok(!m.table.headers.some(h => h.label === 'B'));
});

// %DB column is index 8 (name,count,total_ms,avg_us,p50,p95,p99,max,pct,aas).
const PCT_COL = 8;

test('idle event with pct=null renders "—" for %DB, not a bar', () => {
    // Client:ClientRead is idle (excluded from DB Time): server sends
    // pct=null, the cell must show an em-dash rather than a bogus pct-bar.
    const m = buildEventsModel({ rows: [
        ev({ name: 'Client:ClientRead', event_id: 0x06000000,
             count: 100, total_ms: 5000, pct: null }),
    ] }, null);
    const cell = m.table.rows[0].cells[PCT_COL].html;
    assert.ok(cell.includes('—'), `expected em-dash, got: ${cell}`);
    assert.ok(!cell.includes('pct-bar'), `should not render a bar, got: ${cell}`);
    // The row is still visible with its time intact.
    assert.equal(m.table.rows[0].row.total_ms, 5000);
});

test('non-idle event with numeric pct still renders a pct-bar', () => {
    const m = buildEventsModel({ rows: [
        ev({ name: 'IO:DataFileRead', event_id: 0x01000015,
             count: 100, total_ms: 2100, pct: 16.8 }),
    ] }, null);
    const cell = m.table.rows[0].cells[PCT_COL].html;
    assert.ok(cell.includes('pct-bar'), `expected pct-bar, got: ${cell}`);
    assert.ok(cell.includes('16.8%'), `expected 16.8%, got: ${cell}`);
});

// AAS column is index 9 (one after %DB).
const AAS_COL = 9;

// #453: the Events tab is the OTHER half the pre-fix bug missed entirely
// (table-configs.js:332 had no idle marking at all for AAS -- the bare rate
// printed under "AAS" with zero visible marker).
test('#453 idle-but-visible event (Client:ClientRead): AAS renders the ' +
    'visible em-dash, occupancy labeled beside it, never the bare rate', () => {
    const m = buildEventsModel({ rows: [
        ev({ name: 'Client:ClientRead', event_id: 0x06000000,
             count: 42000, total_ms: 39000, pct: null, aas: 19.62 }),
    ] }, null);
    const cell = m.table.rows[0].cells[AAS_COL].html;
    const plain = cell.replace(/<[^>]*>/g, '');
    assert.notEqual(plain.trim(), '19.62', cell);
    assert.ok(cell.includes('—'), cell);
    assert.ok(cell.includes('19.62'), cell);
    assert.ok(cell.includes('idle-excluded'), cell);
});

test('#453 non-idle event (numeric pct) AAS is UNCHANGED: bare rate, no marker', () => {
    const m = buildEventsModel({ rows: [
        ev({ name: 'IO:DataFileRead', event_id: 0x01000015,
             count: 100, total_ms: 2100, pct: 16.8, aas: 0.58 }),
    ] }, null);
    assert.equal(m.table.rows[0].cells[AAS_COL].html, '0.58');
});

// ── U2 review P10: pane fidelity badge ──────────────────────────────────────

test('exact window: no badge, no percentile note, no truncation row', () => {
    const m = buildEventsModel({ fidelity: 'exact', rows: [ev({})] }, null);
    assert.equal(m.paneFidelity, null);
    assert.equal(m.percentileBasis, null);
    assert.equal(m.truncation, null);
    // The HTML side renders NOTHING for the null models (common case stays
    // noise-free, matching the AAS chip).
    assert.equal(paneFidelityBadgeHtml(null), '');
    assert.equal(percentileBasisHtml(null), '');
});

test('legacy response without a fidelity field defaults to exact: no badge', () => {
    assert.equal(buildPaneFidelity({ rows: [] }), null);
});

test('sampled window: badge model reuses the AAS chip colors + label', () => {
    const m = buildEventsModel({ fidelity: 'sampled', rows: [ev({})] }, null);
    assert.ok(m.paneFidelity);
    assert.equal(m.paneFidelity.fidelity, 'sampled');
    assert.equal(m.paneFidelity.label, 'Sampled (estimated)');
    assert.equal(m.paneFidelity.fill, SAMPLED_BAND_COLOR);
    assert.equal(m.paneFidelity.border, SAMPLED_BORDER);
    const html = paneFidelityBadgeHtml(m.paneFidelity);
    assert.ok(html.includes('pane-fidelity-badge'), html);
    assert.ok(html.includes('Sampled (estimated)'), html);
    assert.ok(html.includes(SAMPLED_BAND_COLOR), html);
    assert.ok(html.includes('scaled estimates'), html);   // the honesty copy
});

test('mixed window: badge uses the mixed chip colors', () => {
    const b = buildPaneFidelity({ fidelity: 'mixed', rows: [] });
    assert.equal(b.fidelity, 'mixed');
    assert.equal(b.fill, MIXED_BAND_COLOR);
    assert.equal(b.label, 'Mixed (sampled + exact)');
});

// ── U2 review P10: percentile-basis footnote ────────────────────────────────

test('mixed window with percentiles: qualitative basis note (no counts today)', () => {
    // The server does not emit per-row exact_count (recorded gap), so the
    // note states the BASIS without inventing N/M.
    const m = buildEventsModel({ fidelity: 'mixed', rows: [
        ev({ p50_us: 10, p95_us: 40, p99_us: 90 }),
    ] }, null);
    assert.ok(m.percentileBasis);
    assert.equal(m.percentileBasis.exact, null);
    assert.equal(m.percentileBasis.total, null);
    assert.ok(m.percentileBasis.text.includes('exact-captured events only'),
        m.percentileBasis.text);
    assert.ok(!/\d+ of \d+/.test(m.percentileBasis.text),
        `no fabricated counts: ${m.percentileBasis.text}`);
});

test('sampled window, all percentiles null: note explains the em-dashes', () => {
    const m = buildEventsModel({ fidelity: 'sampled', rows: [
        ev({ avg_us: null, p50_us: null, p95_us: null, p99_us: null, max_us: null }),
    ] }, null);
    assert.ok(m.percentileBasis);
    assert.ok(m.percentileBasis.text.includes('none'), m.percentileBasis.text);
});

test('percentile basis renders exact "N of M" ONLY when every row carries counts', () => {
    // Forward contract for the recorded server gap: once handle_top_events
    // emits per-row exact_count (the field already exists in
    // struct pgwt_event_row), the same builder upgrades to real counts.
    const withCounts = buildPercentileBasis({ fidelity: 'mixed', rows: [
        { count: 100, exact_count: 20, p50_us: 5 },
        { count: 50, exact_count: 30, p50_us: 9 },
    ] });
    assert.equal(withCounts.exact, 50);
    assert.equal(withCounts.total, 150);
    assert.ok(withCounts.text.includes('50 of 150'), withCounts.text);
    // One row missing the field -> back to qualitative (no partial sums).
    const partial = buildPercentileBasis({ fidelity: 'mixed', rows: [
        { count: 100, exact_count: 20, p50_us: 5 },
        { count: 50, p50_us: 9 },
    ] });
    assert.equal(partial.exact, null);
    assert.ok(!/\d+ of \d+/.test(partial.text), partial.text);
});

// ── U2 review P10: truncation row + P3 wire 6 in the assembled model ────────

test('server-flagged truncation reaches the events model', () => {
    const m = buildEventsModel({ truncated: true, total_count: 40,
        rows: [ev({}), ev({})] }, null);
    assert.deepEqual(m.truncation,
        { omitted: 38, text: '… 38 more below threshold' });
});

test('#103 overflow percentiles reach the assembled model as ">=" bounds', () => {
    // The shape the live capture produced: a Lock row whose every percentile
    // saturated the histogram's open-ended top bucket while Avg/Max are
    // seconds. Columns 4..6 are P50/P95/P99, 7 is Max.
    const m = buildEventsModel({ rows: [
        ev({ name: 'Lock:relation', event_id: 0x03000000, count: 900,
             total_ms: 1600, avg_us: 1800000, max_us: 2600000,
             p50_us: 16384, p95_us: 16384, p99_us: 16384,
             p50_overflow: true, p95_overflow: true, p99_overflow: true }),
    ] }, null);
    for (const ci of [4, 5, 6]) {
        const html = m.table.rows[0].cells[ci].html;
        assert.equal((html.replace(/<[^>]*>/g, '')), PCTL_UNRESOLVED_TEXT,
            `col ${ci}: ${html}`);
        assert.ok(!html.replace(/<[^>]*>/g, '').includes('16.4'), `col ${ci} still shows a number: ${html}`);
        // #437: the exact agreed sentence, verbatim -- "the true value is
        // higher" (the pre-#437 wording) wrongly excluded equality.
        assert.ok(html.includes('Percentiles marked Unresolved fall in the ' +
            'open-ended \u226516.384 ms histogram bucket; they are lower ' +
            'bounds, and their actual values are unavailable.'), html);
    }
    // Avg and Max are exact numbers and keep saying so.
    assert.ok(m.table.rows[0].cells[3].html.includes('1.8s'),
        m.table.rows[0].cells[3].html);
    assert.ok(m.table.rows[0].cells[7].html.includes('2.6s'),
        m.table.rows[0].cells[7].html);
    // #437: shown BESIDE the table too, not only in the per-cell tooltip.
    assert.deepEqual(m.overflowNote, { text:
        'Percentiles marked Unresolved fall in the open-ended ' +
        '\u226516.384 ms histogram bucket; they are lower bounds, and ' +
        'their actual values are unavailable.' });
});

// #454: every FINITE percentile is a histogram bucket upper edge too, not
// just the open-ended overflow tail -- a window with ordinary, non-overflow
// percentiles now ALSO gets a beside-table disclosure (the finite-bound
// sentence, distinct from #437's overflow sentence). Before #454 this was
// `null` (see git history / the superseded test this replaces) -- a window
// with real percentile numbers and nothing disclosed about their meaning is
// exactly the #454 bug.
test('#454 ordinary (non-overflow) percentiles still get a beside-table bound note', () => {
    const m = buildEventsModel({ rows: [
        ev({ name: 'IO:DataFileRead', event_id: 0x01000015,
             p50_us: 8, p95_us: 40, p99_us: 90 }),
    ] }, null);
    assert.deepEqual(m.overflowNote, { text:
        'Percentiles marked ≤ are the upper edge of the histogram ' +
        'bucket the value fell in, not the exact observed duration; a ' +
        'rounded bound can read above Max when few samples exist. The ' +
        'exact tail lives in Max.' });
});

// True negative control: NO percentile data anywhere (sampled-only window,
// every p50/p95/p99 null) -> no note at all. Without this, a note that
// always renders regardless of data would pass the positive test above too.
test('#454 no percentile data anywhere -> no beside-table note', () => {
    const m = buildEventsModel({ rows: [
        ev({ name: 'CPU*', event_id: 0, p50_us: null, p95_us: null, p99_us: null }),
    ] }, null);
    assert.equal(m.overflowNote, null);
});

// #454 gate case: the live capture that opened the issue -- Count=1, exact
// Max ~1.0 ms, every percentile landing on the bucket-10 edge (2048 us ==
// 2.0 ms), a bound VISIBLY BIGGER than the row's own exact Max. Finite edge
// must render qualified ("<=2.0ms"), unchanged numerically, not clamped to
// Max, and the cell's underlying sort value must still be the raw 2048.
test('#454 Count=1 finite edge exceeding Max renders "<=2.0ms", unclamped, sort value intact', () => {
    const m = buildEventsModel({ rows: [
        ev({ name: 'Lock:tuple', event_id: 0x03000001, count: 1,
             total_ms: 1, max_us: 1000,
             p50_us: 2048, p95_us: 2048, p99_us: 2048,
             p50_overflow: false, p95_overflow: false, p99_overflow: false }),
    ] }, null);
    for (const ci of [4, 5, 6]) {
        const html = m.table.rows[0].cells[ci].html;
        assert.ok(html.includes('≤2.0ms'), `col ${ci}: ${html}`);
        assert.ok(!html.includes('≤2.0ms'.replace('2.0', '1.0')), html);
        // #454: never clamp to Max -- the bound stays 2.0ms even though
        // Max (exact) is only 1.0ms.
    }
    assert.ok(m.table.rows[0].cells[7].html.includes('1.0ms'),
        m.table.rows[0].cells[7].html);
    // Sort value is the raw row field, never altered by the cell's display.
    assert.equal(m.table.rows[0].row.p50_us, 2048);
});

// #454 gate case: bucket 14 (finite, upper edge 16384) and bucket 15
// (open-ended) can report the IDENTICAL 16384 number -- only the qualifier
// (driven by the `_overflow` flag) may tell them apart. This is the
// regression a "qualify anything >= 16384" shortcut would fail: it would
// print both rows the SAME way.
test('#454 bucket 14 (finite) and bucket 15 (overflow) both carry 16384 but render differently', () => {
    const m = buildEventsModel({ rows: [
        ev({ name: 'LWLock:LockManager', event_id: 0x04000012,
             p99_us: 16384, p99_overflow: false }),
        ev({ name: 'Lock:relation', event_id: 0x03000000,
             p99_us: 16384, p99_overflow: true }),
    ] }, null);
    const finiteCell = m.table.rows[0].cells[6].html;
    const overflowCell = m.table.rows[1].cells[6].html;
    const cellText = (html) => html.replace(/<[^>]*>/g, '');
    assert.ok(finiteCell.includes('≤16.4ms'), finiteCell);
    assert.equal(cellText(overflowCell), PCTL_UNRESOLVED_TEXT, overflowCell);
    assert.notEqual(finiteCell, overflowCell);
});

test('percentile cells in the assembled model carry the histogram pivot intent', () => {
    const m = buildEventsModel({ rows: [
        ev({ name: 'IO:DataFileRead', event_id: 0x01000015,
             p50_us: 8, p95_us: 40, p99_us: 90 }),
    ] }, null);
    // P50 column (index 4) — the same intent shape app.js PIVOTS consumes.
    const cell = m.table.rows[0].cells[4];
    assert.deepEqual(cell.intent, {
        pivot: 'histogram-event', filterKey: 'event_id',
        filterValue: 0x01000015, label: 'IO:DataFileRead',
    });
});

// ── #440 (coordinator finding): the production path must actually USE the
// tracker it is handed, not just build()/discard it. These tests pass a
// REAL createStableOrder() instance through buildEventsModel exactly as
// views/events.js's createEventsView() wires it -- if the view stopped
// threading the tracker into rows before buildTableModel, this is the
// test that goes red. ──────────────────────────────────────────────────────

test('#440 default order (no explicit sort) is damped by the tracker across ticks', () => {
    const tracker = createStableOrder({
        idKey: (r) => String(r.event_id), valueKey: (r) => r.total_ms,
    });
    buildEventsModel({ rows: [
        ev({ name: 'A', event_id: 1, total_ms: 100 }),
        ev({ name: 'B', event_id: 2, total_ms: 95 }),
    ] }, null, tracker);
    // B rises to 101 -- within 5% of A's 100, must NOT reorder.
    const m = buildEventsModel({ rows: [
        ev({ name: 'A', event_id: 1, total_ms: 100 }),
        ev({ name: 'B', event_id: 2, total_ms: 101 }),
    ] }, null, tracker);
    assert.deepEqual(m.table.rows.map((r) => r.row.name), ['A', 'B']);
});

test('#440 a genuine >5% swing DOES reorder through the same tracker', () => {
    const tracker = createStableOrder({
        idKey: (r) => String(r.event_id), valueKey: (r) => r.total_ms,
    });
    buildEventsModel({ rows: [
        ev({ name: 'A', event_id: 1, total_ms: 100 }),
        ev({ name: 'B', event_id: 2, total_ms: 95 }),
    ] }, null, tracker);
    const m = buildEventsModel({ rows: [
        ev({ name: 'A', event_id: 1, total_ms: 100 }),
        ev({ name: 'B', event_id: 2, total_ms: 106 }),
    ] }, null, tracker);
    assert.deepEqual(m.table.rows.map((r) => r.row.name), ['B', 'A']);
});

test('#440 an EXPLICIT column sort bypasses the tracker entirely (exact value order)', () => {
    const tracker = createStableOrder({
        idKey: (r) => String(r.event_id), valueKey: (r) => r.total_ms,
    });
    // Prime the tracker with an order an explicit sort must NOT honor.
    buildEventsModel({ rows: [
        ev({ name: 'A', event_id: 1, total_ms: 1000 }),
        ev({ name: 'B', event_id: 2, total_ms: 1 }),
    ] }, null, tracker);
    const m = buildEventsModel({ rows: [
        ev({ name: 'A', event_id: 1, total_ms: 50 }),
        ev({ name: 'B', event_id: 2, total_ms: 100 }),
    ] }, { key: 'total_ms', asc: false }, tracker);
    assert.deepEqual(m.table.rows.map((r) => r.row.name), ['B', 'A']);
});

test('#440 no tracker passed -> unchanged (plain) order, exactly today\'s behavior', () => {
    const m = buildEventsModel({ rows: [
        ev({ name: 'A', event_id: 1, total_ms: 50 }),
        ev({ name: 'B', event_id: 2, total_ms: 100 }),
    ] }, null);
    assert.deepEqual(m.table.rows.map((r) => r.row.name), ['A', 'B']);
});

// This is the test that actually closes the gap: it exercises the REAL
// production object createEventsView() returns, not buildEventsModel called
// directly with a hand-made tracker. If createEventsView()'s build() ever
// stops passing its tracker into buildEventsModel, THIS is the test that
// goes red -- the ones above only prove buildEventsModel can apply one it
// is handed, not that the view actually hands it one.
test('#440 WIRING: createEventsView().build() damps order across real build() calls', () => {
    // The incoming row ORDER itself flips tick-to-tick (a server that
    // re-sorts by current value every response, exactly like a plain
    // descending-every-tick policy would look from the wire) -- so this
    // input only passes under REAL hysteresis. Un-wiring build() (dropping
    // the tracker arg) falls back to plain/incoming order, which WOULD
    // follow the flip to [B, A] -- verified RED below before restoring.
    const view = createEventsView();
    const ctx = fakeCtx(null);
    view.build({ rows: [
        ev({ name: 'A', event_id: 1, total_ms: 100 }),
        ev({ name: 'B', event_id: 2, total_ms: 95 }),
    ] }, ctx);
    const m = view.build({ rows: [
        ev({ name: 'B', event_id: 2, total_ms: 101 }),
        ev({ name: 'A', event_id: 1, total_ms: 100 }),
    ] }, ctx);
    assert.deepEqual(m.table.rows.map((r) => r.row.name), ['A', 'B'],
        'B at 101 is within 5% of A\'s 100 -- must not jitter, even though ' +
        'the SERVER sent B first this tick');
});
