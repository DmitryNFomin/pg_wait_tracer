/* Node unit tests for the pure queries builder (views/queries.js) over the
 * shared queriesConfig. Proves row shape, the wait-profile stacked bar, the
 * query-text hover cell, sort, drill-intent (query_id -> events) and edges.
 */
import { test } from 'node:test';
import assert from 'node:assert/strict';
import { buildQueriesModel } from '../../web/static/views/queries.js';
import { queriesConfig } from '../../web/static/lib/builders/table-configs.js';

function q(over) {
    return Object.assign({
        query_id: '0', text: 'SELECT 1', total_ms: 0, pct: 0, count: 0,
        avg_us: 0, top_wait: 'CPU*',
    }, over);
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
