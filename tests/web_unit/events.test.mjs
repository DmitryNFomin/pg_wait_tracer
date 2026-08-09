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
import { buildEventsModel } from '../../web/static/views/events.js';
import { eventsConfig } from '../../web/static/lib/builders/table-configs.js';
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
