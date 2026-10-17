/* Node unit tests for the pure queries builder (views/queries.js) over the
 * shared queriesConfig. Proves row shape, the wait-profile stacked bar, the
 * query-text hover cell, sort, drill-intent (query_id -> events) and edges.
 */
import { test } from 'node:test';
import assert from 'node:assert/strict';
import {
    buildQueriesModel, buildQueriesEmptyStateHtml, createQueriesView,
} from '../../web/static/views/queries.js';
import {
    queriesConfig, buildUnattributedNote,
    queriesScopeNoteHtml, QUERIES_SCOPE_NOTE_TEXT,
} from '../../web/static/lib/builders/table-configs.js';
import { createStableOrder } from '../../web/static/lib/table.js';

function q(over) {
    return Object.assign({
        query_id: '0', text: 'SELECT 1', total_ms: 0, pct: 0, count: 0,
        avg_us: 0, top_wait: 'CPU*',
    }, over);
}

// #440: a minimal fake view-manager ctx -- just enough for build(data, ctx)
// (getSort/timeRange/filters.snapshot), no transport/DOM needed.
function fakeCtx(sortState) {
    return {
        getSort: () => sortState,
        timeRange: { from: 0, to: 1000 },
        filters: { snapshot: () => ({}) },
    };
}

const LONG = 'SELECT ' + 'a'.repeat(200) + ' FROM t';

test('no sort: server order; Query ID/Text/Wait Profile columns; all clickable', () => {
    const data = { rows: [
        q({ query_id: '111', total_ms: 4200, pct: 33.6,
            events: [{ name: 'CPU*', ms: 2000 }, { name: 'IO:DataFileRead', ms: 1200 }] }),
        q({ query_id: '222', total_ms: 3100, pct: 24.8,
            classes: [1500, 900, 0, 0, 0, 0, 0, 0, 0, 0, 0] }),
    ] };
    const m = buildQueriesModel(data, null);
    assert.equal(m.hasRows, true);
    assert.deepEqual(m.table.rows.map(r => r.row.query_id), ['111', '222']);
    assert.ok(m.table.headers.map(h => h.label).includes('Query ID'));
    assert.ok(m.table.headers.map(h => h.label).includes('Query Text'));
    assert.ok(m.table.headers.map(h => h.label).includes('Wait Profile'));
    assert.ok(m.table.rows.every(r => r.cls.includes('clickable')));
});

test('events present -> event stacked bar; only classes -> class stacked bar', () => {
    const withEvents = buildQueriesModel({ rows: [q({ total_ms: 100,
        events: [{ name: 'CPU*', ms: 60 }, { name: 'IO:DataFileRead', ms: 40 }] })] }, null);
    const withClasses = buildQueriesModel({ rows: [q({ total_ms: 100,
        classes: [60, 40, 0, 0, 0, 0, 0, 0, 0, 0, 0] })] }, null);
    // Waterfall is deliberately early, so Wait Profile is column 6 (index 5).
    assert.ok(withEvents.table.rows[0].cells[5].html.includes('stacked-bar'));
    assert.ok(withClasses.table.rows[0].cells[5].html.includes('stacked-bar'));
});

test('long query text gets a qt-hover cell carrying the full text', () => {
    const m = buildQueriesModel({ rows: [q({ query_id: '9', text: LONG })] }, null);
    const textCell = m.table.rows[0].cells[1].html;
    assert.ok(textCell.includes('qt-hover'));
    assert.ok(textCell.includes('data-fulltext'));
    // truncated visible portion ends with ellipsis
    assert.ok(textCell.includes('...'));
});

test('empty query text renders an em-dash placeholder', () => {
    const m = buildQueriesModel({ rows: [q({ query_id: '9', text: '' })] }, null);
    assert.ok(m.table.rows[0].cells[1].html.includes('—'));
});

test('descending sort by total_ms', () => {
    const data = { rows: [
        q({ query_id: 'a', total_ms: 100 }), q({ query_id: 'b', total_ms: 300 }),
        q({ query_id: 'c', total_ms: 200 }),
    ] };
    const m = buildQueriesModel(data, { key: 'total_ms', asc: false });
    assert.deepEqual(m.table.rows.map(r => r.row.query_id), ['b', 'c', 'a']);
});

test('drill intent targets query_id (-> events); label is text prefix', () => {
    assert.deepEqual(
        queriesConfig.onClick(q({ query_id: '42', text: 'UPDATE pgbench_accounts SET x' })),
        { filterKey: 'query_id', filterValue: '42',
          label: 'UPDATE pgbench_accounts SET x' });
    // no text -> falls back to "Query <id>"
    assert.deepEqual(
        queriesConfig.onClick(q({ query_id: '42', text: '' })),
        { filterKey: 'query_id', filterValue: '42', label: 'Query 42' });
});

test('dedicated Waterfall cell emits query-executions without changing row drill', () => {
    const row = q({ query_id: '10', text: 'SELECT * FROM accounts' });
    const m = buildQueriesModel({ rows: [row] }, null);
    const cell = m.table.rows[0].cells[2];
    assert.equal(m.table.headers[2].label, 'Waterfall');
    assert.equal(cell.intent.pivot, 'query-executions');
    assert.equal(cell.intent.filterKey, 'query_id');
    assert.equal(cell.intent.filterValue, '10');
    assert.ok(cell.html.includes('Executions'));
    assert.equal(queriesConfig.onClick(row).filterKey, 'query_id');
    assert.equal(queriesConfig.onClick(row).pivot, undefined);
});

test('empty + single-row edge cases', () => {
    assert.equal(buildQueriesModel(null, null).hasRows, false);
    assert.equal(buildQueriesModel({ rows: [] }, null).hasRows, false);
    const m = buildQueriesModel({ rows: [q({ query_id: 'solo' })] },
        { key: 'total_ms', asc: false });
    assert.equal(m.table.rows.length, 1);
});

test('B-only compare failure falls back to the ordinary A queries model', () => {
    const a = { fidelity: 'exact', rows: [q({ query_id: 'A', total_ms: 250 })] };
    const m = buildQueriesModel({ compare: true, a, b: null,
        baselineUnavailable: true }, null);
    assert.equal(m.baselineUnavailable, true);
    assert.equal(m.compare, undefined);
    assert.deepEqual(m.table.rows.map(r => r.row.query_id), ['A']);
    assert.ok(m.table.headers.some(h => h.label === 'Query ID'));
    assert.ok(!m.table.headers.some(h => h.label === 'B'));
});

// ── #440 (coordinator finding): the production path must actually USE the
// tracker it is handed. Mirrors events.test.mjs's wiring proof exactly --
// if views/queries.js stopped threading the tracker into rows before
// buildTableModel, this is the test that goes red. ──────────────────────────

test('#440 default order (no explicit sort) is damped by the tracker across ticks', () => {
    const tracker = createStableOrder({
        idKey: (r) => String(r.query_id), valueKey: (r) => r.total_ms,
    });
    buildQueriesModel({ rows: [
        q({ query_id: 'A', total_ms: 100 }), q({ query_id: 'B', total_ms: 95 }),
    ] }, null, tracker);
    // B rises to 101 -- within 5% of A's 100, must NOT reorder.
    const m = buildQueriesModel({ rows: [
        q({ query_id: 'A', total_ms: 100 }), q({ query_id: 'B', total_ms: 101 }),
    ] }, null, tracker);
    assert.deepEqual(m.table.rows.map((r) => r.row.query_id), ['A', 'B']);
});

test('#440 a genuine >5% swing DOES reorder through the same tracker', () => {
    const tracker = createStableOrder({
        idKey: (r) => String(r.query_id), valueKey: (r) => r.total_ms,
    });
    buildQueriesModel({ rows: [
        q({ query_id: 'A', total_ms: 100 }), q({ query_id: 'B', total_ms: 95 }),
    ] }, null, tracker);
    const m = buildQueriesModel({ rows: [
        q({ query_id: 'A', total_ms: 100 }), q({ query_id: 'B', total_ms: 106 }),
    ] }, null, tracker);
    assert.deepEqual(m.table.rows.map((r) => r.row.query_id), ['B', 'A']);
});

test('#440 an EXPLICIT column sort bypasses the tracker entirely (exact value order)', () => {
    const tracker = createStableOrder({
        idKey: (r) => String(r.query_id), valueKey: (r) => r.total_ms,
    });
    buildQueriesModel({ rows: [
        q({ query_id: 'A', total_ms: 1000 }), q({ query_id: 'B', total_ms: 1 }),
    ] }, null, tracker);
    const m = buildQueriesModel({ rows: [
        q({ query_id: 'A', total_ms: 50 }), q({ query_id: 'B', total_ms: 100 }),
    ] }, { key: 'total_ms', asc: false }, tracker);
    assert.deepEqual(m.table.rows.map((r) => r.row.query_id), ['B', 'A']);
});

test('#440 no tracker passed -> unchanged (plain) order, exactly today\'s behavior', () => {
    const m = buildQueriesModel({ rows: [
        q({ query_id: 'A', total_ms: 50 }), q({ query_id: 'B', total_ms: 100 }),
    ] }, null);
    assert.deepEqual(m.table.rows.map((r) => r.row.query_id), ['A', 'B']);
});

// This is the test that actually closes the gap: it exercises the REAL
// production object createQueriesView() returns. If its build() ever stops
// passing its tracker into buildQueriesModel, THIS is the test that goes
// red -- the ones above only prove buildQueriesModel CAN apply a tracker it
// is handed, not that the view actually hands it one.
test('#440 WIRING: createQueriesView().build() damps order across real build() calls', () => {
    // Incoming row order flips tick-to-tick (see events.test.mjs's
    // identical comment) -- only passes under real hysteresis.
    const view = createQueriesView();
    const ctx = fakeCtx(null);
    view.build({ rows: [
        q({ query_id: 'A', total_ms: 100 }), q({ query_id: 'B', total_ms: 95 }),
    ] }, ctx);
    const m = view.build({ rows: [
        q({ query_id: 'B', total_ms: 101 }), q({ query_id: 'A', total_ms: 100 }),
    ] }, ctx);
    assert.deepEqual(m.table.rows.map((r) => r.row.query_id), ['A', 'B'],
        'B at 101 is within 5% of A\'s 100 -- must not jitter');
});

// ── #439 (coordinator finding): wholly-unattributed activity must still
// disclose unattributedNote even when there are NO query_id rows -- an
// empty table is exactly when disclosure matters most (it is then ALL of
// the foreground time, not a caveat on some of it). ─────────────────────────

test('#439 empty rows + unattributed time: the model STILL carries the note', () => {
    const data = { rows: [], unattributed_available: true,
        unattributed_ms: 5000, unattributed_count: 1, db_time_ms: 0 };
    const m = buildQueriesModel(data, null);
    assert.equal(m.hasRows, false);
    assert.ok(m.unattributedNote, 'unattributedNote must survive the empty-rows case');
    assert.deepEqual(m.unattributedNote, buildUnattributedNote(data));
});

test('#439 empty-state PANE (what mount() actually writes to the DOM) discloses it too', () => {
    // This is the real bug location: buildQueriesModel already carried
    // unattributedNote (above) even before the fix -- the empty-rows DOM
    // pane (mount()'s early return) is what used to drop it on the floor.
    const data = { rows: [], unattributed_available: true,
        unattributed_ms: 5000, unattributed_count: 1, db_time_ms: 0 };
    const m = buildQueriesModel(data, null);
    const html = buildQueriesEmptyStateHtml(m);
    assert.ok(html.includes('No data for selected range'), html);
    assert.ok(html.includes('unattributed-note'), html);
    assert.ok(html.includes('not attributed to any query'), html);
});

test('#439 empty-state pane: NO unattributed time -> no note, "No data" stands alone', () => {
    const m = buildQueriesModel({ rows: [] }, null);
    const html = buildQueriesEmptyStateHtml(m);
    assert.ok(html.includes('No data for selected range'), html);
    assert.ok(!html.includes('unattributed-note'), html);
});

test('#439 empty-state pane: compare mode never shows the (compare-less) unattributed note', () => {
    const html = buildQueriesEmptyStateHtml({ compare: true, baselinePredates: true });
    assert.ok(html.includes('Baseline unavailable for comparison'), html);
    assert.ok(!html.includes('unattributed-note'), html);
});

// ── #455: DB Time (includes a pre-execution wait, e.g. a parse-time lock)
// vs Avg/p95/p99 Exec and Plan (executor/planner time only) -- static
// disclosure, independent of any one row's data. ────────────────────────

test('#455 queriesScopeNoteHtml discloses DB Time includes a pre-execution wait', () => {
    const html = queriesScopeNoteHtml();
    assert.ok(html.includes(QUERIES_SCOPE_NOTE_TEXT), html);
    assert.ok(/before execution/i.test(html), html);
    assert.ok(/DB Time/.test(html), html);
    assert.ok(html.includes('exec-scope-note'), html);
});
