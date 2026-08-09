import { test } from 'node:test';
import assert from 'node:assert/strict';
import {
    buildDeltaComparison, defaultDeltaSort,
} from '../../web/static/lib/builders/compare.js';
import { buildTableModel } from '../../web/static/lib/table.js';
import { compareEventsConfig } from '../../web/static/lib/builders/table-configs.js';

const ev = (event_id, name, total_ms) => ({
    event_id, name, class: name.split(':')[0], total_ms,
});

function fixture() {
    return {
        a: { db_time_ms: 10_000, rows: [
            ev(1, 'IO:large', 3000), ev(2, 'Lock:new', 500),
            ev(3, 'IPC:noise', 150), ev(5, 'LWLock:ratio', 2000),
        ] },
        b: { db_time_ms: 10_000, rows: [
            ev(1, 'IO:large', 1000), ev(3, 'IPC:noise', 100),
            ev(4, 'Client:gone', 700), ev(5, 'LWLock:ratio', 1000),
        ] },
    };
}

test('delta default ranks by absolute DB-time change', () => {
    const { a, b } = fixture();
    const d = buildDeltaComparison('events', a, b);
    const m = buildTableModel(compareEventsConfig, d.rows, defaultDeltaSort(null));
    assert.deepEqual(m.rows.map(r => r.row.event_id), [1, 5, 4, 2]);
    assert.equal(m.headers.find(h => h.key === 'abs_delta_ms').arrow, ' ▼');
    assert.equal(m.headers.find(h => h.key === 'b_ms').sortable, false);
    assert.deepEqual(defaultDeltaSort({ key: 'count', asc: true }),
        { key: 'abs_delta_ms', asc: false }, 'ordinary-table sorts cannot leak into compare');
});

test('delta sort toggles rank by A/B ratio or by A', () => {
    const { a, b } = fixture();
    const d = buildDeltaComparison('events', a, b);
    const ratio = buildTableModel(compareEventsConfig, d.rows,
        { key: 'ratio', asc: false });
    assert.deepEqual(ratio.rows.map(r => r.row.event_id), [1, 5, 2, 4]);
    const byA = buildTableModel(compareEventsConfig, d.rows,
        { key: 'a_ms', asc: false });
    assert.deepEqual(byA.rows.map(r => r.row.event_id), [1, 5, 2, 4]);
});

test('noise floor collapses exact count into the honest change-floor row', () => {
    const { a, b } = fixture();
    const d = buildDeltaComparison('events', a, b);
    assert.equal(d.floorMs, 100);
    assert.equal(d.belowFloor, 1);
    assert.equal(d.truncation.text, '… 1 entities below the change floor');
    assert.ok(!d.rows.some(r => r.event_id === 3));
});

test('one-window entities are new/gone and never receive infinity ratios', () => {
    const { a, b } = fixture();
    const d = buildDeltaComparison('events', a, b);
    const newly = d.rows.find(r => r.event_id === 2);
    const gone = d.rows.find(r => r.event_id === 4);
    assert.equal(newly.status, 'new');
    assert.equal(gone.status, 'gone');
    assert.equal(newly.ratio, null);
    assert.equal(gone.ratio, null);
    const m = buildTableModel(compareEventsConfig, [newly, gone], null);
    const ratioCells = m.rows.map(r => r.cells.at(-1).html);
    assert.ok(ratioCells[0].includes('new'));
    assert.ok(ratioCells[1].includes('gone'));
    assert.ok(!ratioCells.join('').includes('Infinity'));
    assert.ok(!ratioCells.join('').includes('∞'));
    assert.equal(m.rows[0].cells[2].html, '—', 'new row has no fabricated B value');
    assert.equal(m.rows[1].cells[1].html, '—', 'gone row has no fabricated A value');
});

test('retention-missing baseline emits no fabricated new entities', () => {
    const { a } = fixture();
    const d = buildDeltaComparison('events', a, null,
        { baselineUnavailable: true });
    assert.equal(d.baselineUnavailable, true);
    assert.deepEqual(d.rows, []);
    assert.equal(d.truncation, null);
    assert.equal(d.dbTimeB, null);
});

test('query IDs join as strings without uint64 coercion', () => {
    const qid = '18446744073709551615';
    const d = buildDeltaComparison('queries',
        { db_time_ms: 1000, rows: [{ query_id: qid, total_ms: 600 }] },
        { db_time_ms: 1000, rows: [{ query_id: qid, total_ms: 300 }] });
    assert.equal(d.rows.length, 1);
    assert.equal(d.rows[0].query_id, qid);
    assert.equal(d.rows[0].delta_ms, 300);
});
