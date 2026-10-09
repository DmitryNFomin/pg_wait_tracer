/* The Waterfall table's two count columns must never put a poison value on
 * screen (paint-latency Phase 3, acceptance criterion 3).
 *
 * src/exec_index.h writes PGWT_EXEC_INDEX_NOT_INDEXED (-1) into n_events and
 * n_workers, because a BOUNDARY index cannot carry window-clipped per-event
 * quantities and absence must never read as zero. Those two fields are
 * serialised by src/server.c and rendered as "Leader events" / "Workers"
 * (web/static/lib/builders/waterfall.js), so the string "-1" would land in a
 * column of counts, indistinguishable to a reader from a measurement of
 * minus one event.
 *
 * The server refuses such a response outright
 * (serialize_execution_row -> pgwt_execution_counts_known, pinned by
 * tests/test_exec_index_guards.c). This file pins the SECOND, independent
 * guard: a column whose correctness depends on another process never having
 * a bug is not guarded at all.
 *
 * RED WITHOUT THE FIX: before this change the formatter was
 * `(r) => String(r.n_events)`, so `n_events: -1` rendered the cell as the
 * literal '-1' and every assertion below on cells[5] / cells[6] fails.
 */
import { test } from 'node:test';
import assert from 'node:assert/strict';
import {
    buildExecutionsModel, executionHasDetail, pickDefaultExecution,
    fmtExecutionCount, executionCountsKnown,
} from '../../web/static/lib/builders/waterfall.js';

const START = 1_719_792_000_000_000_123n;

function row(over = {}) {
    return {
        pid: 7, query_id: '42', start_ns: String(START),
        end_ns: String(START + 1_000_000n), duration_ms: 1.0, plan_ms: null,
        n_events: 3, n_workers: 1, in_progress: false,
        started_before_window: false, end_inferred: false,
        ...over,
    };
}

const model = (rows) =>
    buildExecutionsModel({ rows, truncated: false, total_count: rows.length },
                         null);

/* ── the defect itself ─────────────────────────────────────────────────── */

test('a not-indexed count never renders as "-1"', () => {
    const m = model([row({ n_events: -1, n_workers: -1 })]);
    const cells = m.table.rows[0].cells;
    // Columns 5 and 6 are "Leader events" and "Workers".
    assert.equal(m.table.headers[5].label, 'Leader events');
    assert.equal(m.table.headers[6].label, 'Workers');
    assert.equal(cells[5].html, '—');
    assert.equal(cells[6].html, '—');
    assert.ok(!cells[5].html.includes('-1'));
    assert.ok(!cells[6].html.includes('-1'));
});

test('one poisoned field does not launder the other', () => {
    // A half-poisoned row is the shape a guard that required BOTH fields to
    // be poison would wave through.
    const m = model([row({ n_events: -1, n_workers: 4 })]);
    assert.equal(m.table.rows[0].cells[5].html, '—');
    assert.equal(m.table.rows[0].cells[6].html, '4');
});

/* ── real counts are untouched: a guard that blanks everything is just a
 *    broken column with a different look ──────────────────────────────── */

test('real counts, including zero, still render as numbers', () => {
    const m = model([row({ n_events: 0, n_workers: 0 }),
                     row({ n_events: 12, n_workers: 3 })]);
    assert.equal(m.table.rows[0].cells[5].html, '0');
    assert.equal(m.table.rows[0].cells[6].html, '0');
    assert.equal(m.table.rows[1].cells[5].html, '12');
    assert.equal(m.table.rows[1].cells[6].html, '3');
});

/* ── the formatter, directly: every way a non-count can arrive ─────────── */

test('fmtExecutionCount accepts counts and refuses everything else', () => {
    assert.equal(fmtExecutionCount(0), '0');
    assert.equal(fmtExecutionCount(5), '5');
    assert.equal(fmtExecutionCount('5'), '5');     // wire sends numbers, but
    assert.equal(fmtExecutionCount(-1), '—');
    assert.equal(fmtExecutionCount(-7), '—');      // not just the -1 sentinel
    assert.equal(fmtExecutionCount(null), '—');    // an older server omitting
    assert.equal(fmtExecutionCount(undefined), '—'); // the field entirely
    assert.equal(fmtExecutionCount(NaN), '—');
    assert.equal(fmtExecutionCount(Infinity), '—');
    assert.equal(fmtExecutionCount('nonsense'), '—');
    // An em dash, not an empty string: a blank cell reads as "nothing here",
    // which for a count means zero.
    assert.notEqual(fmtExecutionCount(-1), '');
    assert.notEqual(fmtExecutionCount(-1), '0');
});

test('executionCountsKnown mirrors the C predicate', () => {
    assert.equal(executionCountsKnown({ n_events: 0, n_workers: 0 }), true);
    assert.equal(executionCountsKnown({ n_events: 9, n_workers: 2 }), true);
    assert.equal(executionCountsKnown({ n_events: -1, n_workers: 2 }), false);
    assert.equal(executionCountsKnown({ n_events: 9, n_workers: -1 }), false);
    assert.equal(executionCountsKnown({ n_events: -1, n_workers: -1 }), false);
    assert.equal(executionCountsKnown({ n_workers: 2 }), false);
    assert.equal(executionCountsKnown({}), false);
    assert.equal(executionCountsKnown(null), false);
});

/* ── the detail gate ──────────────────────────────────────────────────────
 * executionHasDetail() decided on count(row.n_events) > 0, and count(-1) is
 * -1, so a poisoned row answered "no detail" for the right reason by
 * accident -- until plan_ms is non-null, at which point it answered "yes,
 * open a waterfall" on the strength of a value that is not a count. */

test('a poisoned row is not auto-selected even when plan_ms is set', () => {
    const poisoned = { pid: 1, n_events: -1, n_workers: -1, plan_ms: 1.5 };
    assert.equal(executionHasDetail(poisoned), false);

    const real = { pid: 2, n_events: 0, n_workers: 0, plan_ms: 1.5 };
    assert.equal(executionHasDetail(real), true);
});

test('a poisoned row loses to a real one in default selection', () => {
    const poisoned = { pid: 1, n_events: -1, n_workers: -1, plan_ms: null };
    const real = { pid: 2, n_events: 4, n_workers: 0, plan_ms: null };
    assert.equal(pickDefaultExecution([poisoned, real]), real);
    // And when the page holds nothing but poisoned rows the fallback is the
    // first row with no detail claimed -- the empty state, not a chart.
    assert.equal(executionHasDetail(pickDefaultExecution([poisoned])), false);
});

/* ── bypass suite: ways this test could stop seeing the defect ─────────── */

test('bypass: a fixture with real counts is BLIND to the defect', () => {
    // String(3) === '3' === fmtExecutionCount(3): the old and new formatters
    // agree on every valid count, so a fixture built only from valid counts
    // can never go red. Asserted so nobody mistakes the tests above for
    // coverage of this one.
    for (const v of [0, 1, 3, 12, 99999]) {
        assert.equal(String(v), fmtExecutionCount(v),
            `the old formatter and the new one agree on ${v}: a fixture of `
            + 'valid counts cannot detect the poison bug');
    }
});

test('bypass: the column index is pinned by its header, not by position', () => {
    // If the Waterfall gains or loses a column, an assertion on cells[5]
    // would silently start checking a different cell -- and pass. The
    // header check above is what stops that; this states it as its own case.
    const m = model([row({ n_events: -1, n_workers: -1 })]);
    const i = m.table.headers.findIndex(h => h.label === 'Leader events');
    const j = m.table.headers.findIndex(h => h.label === 'Workers');
    assert.ok(i >= 0 && j >= 0, 'both count columns still exist');
    assert.equal(m.table.rows[0].cells[i].html, '—');
    assert.equal(m.table.rows[0].cells[j].html, '—');
});

test('bypass: an empty row set neither passes nor hides anything', () => {
    // A gate that cannot see must refuse, never approve: with no rows there
    // is nothing to check, and the test says so instead of going green on
    // an empty loop.
    const m = model([]);
    assert.equal(m.table.rows.length, 0);
    // The assertions above are therefore driven by non-empty fixtures only;
    // this one exists to make the empty case explicit rather than implicit.
});
