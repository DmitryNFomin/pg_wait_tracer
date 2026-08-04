/* Node unit tests for the pure sessions builder (views/sessions.js) over the
 * shared sessionsConfig. Proves row shape, sort, drill-intent (pid -> timeline)
 * and edge cases without a browser.
 */
import { test } from 'node:test';
import assert from 'node:assert/strict';
import { buildSessionsModel } from '../../web/static/views/sessions.js';
import { sessionsConfig } from '../../web/static/lib/builders/table-configs.js';

function sess(over) {
    return Object.assign({
        pid: 0, type: 'client', user: 'postgres', db: 'testdb',
        db_time_ms: 0, cpu_pct: 0, wait_pct: 0, top_wait: 'CPU*',
    }, over);
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
