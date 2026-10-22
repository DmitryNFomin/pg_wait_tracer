/* Node unit tests for the pure sessions builder (views/sessions.js) over the
 * shared sessionsConfig. Proves row shape, sort, drill-intent (pid -> timeline)
 * and edge cases without a browser.
 */
import { test } from 'node:test';
import assert from 'node:assert/strict';
import { buildSessionsModel, createSessionsView } from '../../web/static/views/sessions.js';
import {
    sessionsConfig, sessionsScopeNoteHtml, SESSIONS_SCOPE_NOTE_TEXT,
} from '../../web/static/lib/builders/table-configs.js';
import { createStableOrder } from '../../web/static/lib/table.js';

function sess(over) {
    return Object.assign({
        pid: 0, type: 'client', user: 'postgres', db: 'testdb',
        db_time_ms: 0, cpu_pct: 0, wait_pct: 0, top_wait: 'CPU*',
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

test('no sort: server order; PID/Backend/Top Wait columns; all clickable', () => {
    const data = { rows: [
        sess({ pid: 1001, db_time_ms: 3000, top_wait: 'IO:DataFileRead' }),
        sess({ pid: 1002, db_time_ms: 2000, top_wait: 'Lock:relation' }),
        sess({ pid: 4870, type: 'checkpointer', user: '', db: '' }),
    ] };
    const m = buildSessionsModel(data, null);
    assert.equal(m.hasRows, true);
    assert.deepEqual(m.table.rows.map(r => r.row.pid), [1001, 1002, 4870]);
    assert.deepEqual(m.table.headers.map(h => h.label),
        ['PID', 'Backend', 'User', 'Database', 'DB Time', 'CPU%', 'Wait%', 'Top Wait']);
    assert.ok(m.table.rows.every(r => r.cls.includes('clickable')));
    // Top Wait cell carries a class dot
    assert.ok(m.table.rows[0].cells[7].html.includes('class-dot'));
});

test('descending sort by db_time_ms', () => {
    const data = { rows: [
        sess({ pid: 1, db_time_ms: 100 }), sess({ pid: 2, db_time_ms: 300 }),
        sess({ pid: 3, db_time_ms: 200 }),
    ] };
    const m = buildSessionsModel(data, { key: 'db_time_ms', asc: false });
    assert.deepEqual(m.table.rows.map(r => r.row.pid), [2, 3, 1]);
});

test('ascending sort by pid', () => {
    const data = { rows: [
        sess({ pid: 3 }), sess({ pid: 1 }), sess({ pid: 2 }),
    ] };
    const m = buildSessionsModel(data, { key: 'pid', asc: true });
    assert.deepEqual(m.table.rows.map(r => r.row.pid), [1, 2, 3]);
});

test('drill intent targets pid (-> timeline) with "PID n" label', () => {
    assert.deepEqual(sessionsConfig.onClick(sess({ pid: 1001 })),
        { filterKey: 'pid', filterValue: 1001, label: 'PID 1001' });
});

test('missing user/db render empty, not "undefined"', () => {
    const m = buildSessionsModel({ rows: [sess({ pid: 9, user: '', db: '' })] }, null);
    assert.equal(m.table.rows[0].cells[2].html, '');
    assert.equal(m.table.rows[0].cells[3].html, '');
});

test('empty + single-row edge cases', () => {
    assert.equal(buildSessionsModel(null, null).hasRows, false);
    assert.equal(buildSessionsModel({ rows: [] }, null).hasRows, false);
    const m = buildSessionsModel({ rows: [sess({ pid: 7 })] }, { key: 'pid', asc: false });
    assert.equal(m.table.rows.length, 1);
    assert.equal(m.table.rows[0].row.pid, 7);
});

// ── U2 review P10: fidelity badge + truncation reach the sessions model ─────

test('exact window: no badge, no truncation (noise-free common case)', () => {
    const m = buildSessionsModel({ fidelity: 'exact', rows: [sess({ pid: 1 })] }, null);
    assert.equal(m.paneFidelity, null);
    assert.equal(m.truncation, null);
});

test('sampled window: the pane badge model is present', () => {
    const m = buildSessionsModel({ fidelity: 'sampled', rows: [sess({ pid: 1 })] }, null);
    assert.ok(m.paneFidelity);
    assert.equal(m.paneFidelity.fidelity, 'sampled');
    assert.equal(m.paneFidelity.label, 'Sampled (estimated)');
});

test('server-flagged truncation reaches the sessions model', () => {
    const m = buildSessionsModel(
        { truncated: true, rows: [sess({ pid: 1 })] }, null);
    assert.deepEqual(m.truncation, { omitted: null, text: '… more not shown' });
});

// ── #440 (coordinator finding): the production path must actually USE the
// tracker it is handed. Mirrors events.test.mjs's wiring proof exactly --
// if views/sessions.js stopped threading the tracker into rows before
// buildTableModel, this is the test that goes red. ──────────────────────────

test('#440 default order (no explicit sort) is damped by the tracker across ticks', () => {
    const tracker = createStableOrder({
        idKey: (r) => String(r.pid), valueKey: (r) => r.db_time_ms,
    });
    buildSessionsModel({ rows: [
        sess({ pid: 1, db_time_ms: 100 }), sess({ pid: 2, db_time_ms: 95 }),
    ] }, null, tracker);
    // pid 2 rises to 101 -- within 5% of pid 1's 100, must NOT reorder.
    const m = buildSessionsModel({ rows: [
        sess({ pid: 1, db_time_ms: 100 }), sess({ pid: 2, db_time_ms: 101 }),
    ] }, null, tracker);
    assert.deepEqual(m.table.rows.map((r) => r.row.pid), [1, 2]);
});

test('#440 a genuine >5% swing DOES reorder through the same tracker', () => {
    const tracker = createStableOrder({
        idKey: (r) => String(r.pid), valueKey: (r) => r.db_time_ms,
    });
    buildSessionsModel({ rows: [
        sess({ pid: 1, db_time_ms: 100 }), sess({ pid: 2, db_time_ms: 95 }),
    ] }, null, tracker);
    const m = buildSessionsModel({ rows: [
        sess({ pid: 1, db_time_ms: 100 }), sess({ pid: 2, db_time_ms: 106 }),
    ] }, null, tracker);
    assert.deepEqual(m.table.rows.map((r) => r.row.pid), [2, 1]);
});

test('#440 an EXPLICIT column sort bypasses the tracker entirely (exact value order)', () => {
    const tracker = createStableOrder({
        idKey: (r) => String(r.pid), valueKey: (r) => r.db_time_ms,
    });
    buildSessionsModel({ rows: [
        sess({ pid: 1, db_time_ms: 1000 }), sess({ pid: 2, db_time_ms: 1 }),
    ] }, null, tracker);
    const m = buildSessionsModel({ rows: [
        sess({ pid: 1, db_time_ms: 50 }), sess({ pid: 2, db_time_ms: 100 }),
    ] }, { key: 'db_time_ms', asc: false }, tracker);
    assert.deepEqual(m.table.rows.map((r) => r.row.pid), [2, 1]);
});

test('#440 no tracker passed -> unchanged (plain) order, exactly today\'s behavior', () => {
    const m = buildSessionsModel({ rows: [
        sess({ pid: 1, db_time_ms: 50 }), sess({ pid: 2, db_time_ms: 100 }),
    ] }, null);
    assert.deepEqual(m.table.rows.map((r) => r.row.pid), [1, 2]);
});

// This is the test that actually closes the gap: it exercises the REAL
// production object createSessionsView() returns. If its build() ever
// stops passing its tracker into buildSessionsModel, THIS is the test that
// goes red -- the ones above only prove buildSessionsModel CAN apply a
// tracker it is handed, not that the view actually hands it one.
test('#440 WIRING: createSessionsView().build() damps order across real build() calls', () => {
    // Incoming row order flips tick-to-tick (see events.test.mjs's
    // identical comment) -- only passes under real hysteresis.
    const view = createSessionsView();
    const ctx = fakeCtx(null);
    view.build({ rows: [
        sess({ pid: 1, db_time_ms: 100 }), sess({ pid: 2, db_time_ms: 95 }),
    ] }, ctx);
    const m = view.build({ rows: [
        sess({ pid: 2, db_time_ms: 101 }), sess({ pid: 1, db_time_ms: 100 }),
    ] }, ctx);
    assert.deepEqual(m.table.rows.map((r) => r.row.pid), [1, 2],
        'pid 2 at 101 is within 5% of pid 1\'s 100 -- must not jitter');
});

// #439 (Sessions half): this is the test that closes the gap the same way
// #440's WIRING test above does -- it exercises the REAL mount() the view
// manager calls, not just the pure HTML-string builder in isolation. A note
// that exists but is never inserted into the DOM discloses nothing to an
// actual viewer.
function mountSessions(data) {
    const el = { innerHTML: '', insertAdjacentHTML(_where, html) { this.innerHTML += html; } };
    const view = createSessionsView();
    const ctx = { ...fakeCtx(null), mountTable() {}, summaryEl: null };
    view.mount(el, view.build(data, ctx), ctx);
    return el.innerHTML;
}

test('#439 Sessions mount() actually inserts the cross-tab scope note into the DOM', () => {
    const html = mountSessions({ rows: [sess({ pid: 4870, type: 'checkpointer',
        user: '', db: '', db_time_ms: 1200 })] });
    assert.ok(html.includes(SESSIONS_SCOPE_NOTE_TEXT), html);
    assert.equal(html.includes(sessionsScopeNoteHtml()), true, html);
});

test('#439 Sessions mount() with no rows still shows the empty state (note is not required there)', () => {
    const html = mountSessions({ rows: [] });
    assert.ok(html.includes('No data for selected range'), html);
});
