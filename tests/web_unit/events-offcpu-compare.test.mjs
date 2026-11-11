/* #757: Events compare mode must join the synthetic "CPU (waiting for a
 * core)" off-CPU remainder (addCpuWaitingRow, lib/builders/table-configs.js)
 * the same way the non-compare path does, so a window-over-window off-CPU
 * change is visible in Events compare the same way it already is in
 * Overview's Time Model. Before the #757 fix, buildEventsModel's compare
 * branch called buildDeltaComparison on the raw payload rows only, so the
 * synthetic row never reached the join at all.
 */
import { test } from 'node:test';
import assert from 'node:assert/strict';
import { buildEventsModel } from '../../web/static/views/events.js';
import { eventsConfig, compareEventsConfig } from '../../web/static/lib/builders/table-configs.js';
import { createOverviewView } from '../../web/static/views/overview.js';

const WAITING = 'CPU (waiting for a core)';

/* One measured window as both top_events and time_model represent it.
 * db_time_ms stays fixed; CPU time is split between "running" (on the
 * "CPU (running)" row on both tabs) and "waiting
 * for a core" (the synthetic row on both tabs). */
function window(offcpuMs) {
    const dbTimeMs = 1000;
    const oncpuMs = 800 - offcpuMs;
    return {
        events: {
            db_time_ms: dbTimeMs, has_measured_cpu: offcpuMs > 0, offcpu_ms: offcpuMs,
            offcpu_aas: offcpuMs / 2000,
            rows: [
                { event_id: 0, name: 'CPU (running)', total_ms: oncpuMs },
                { event_id: 0x01000015, name: 'IO:DataFileRead', total_ms: 200 },
            ],
        },
        overview: {
            db_time_ms: dbTimeMs,
            rows: [
                { indent: 0, name: 'DB Time', ms: dbTimeMs, aas: 1 },
                { indent: 1, name: 'CPU (running)', ms: oncpuMs },
                { indent: 1, name: WAITING, ms: offcpuMs, aas: offcpuMs / 1000 },
                { indent: 1, name: 'IO', ms: 200 },
            ],
        },
    };
}

function waitingRows(model) {
    return model.table.rows.filter(r => r.row.name === WAITING);
}

const overviewCtx = { getSort: () => null };

test('#757 Events compare joins the off-CPU row on both sides, A-only and B-only', () => {
    // [aMs, bMs, expectedStatus]: both-sides, A-only (new), B-only (gone).
    for (const [aMs, bMs, status] of [[460, 260, null], [260, 0, 'new'], [0, 260, 'gone']]) {
        const a = window(aMs).events;
        const b = window(bMs).events;
        const beforeA = structuredClone(a);
        const beforeB = structuredClone(b);
        const model = buildEventsModel({ compare: true, a, b }, null);
        const matches = waitingRows(model);
        assert.equal(matches.length, 1,
            `A=${aMs}ms B=${bMs}ms: expected exactly one joined waiting-for-a-core row, got ${matches.length}`);
        const row = matches[0].row;
        assert.equal(row.a_ms, aMs);
        assert.equal(row.b_ms, bMs);
        assert.equal(row.delta_ms, aMs - bMs);
        assert.equal(row.status, status);
        // addCpuWaitingRow must not mutate the payload it augments.
        assert.deepEqual(a, beforeA, 'augmenting A for the join must not mutate the A payload');
        assert.deepEqual(b, beforeB, 'augmenting B for the join must not mutate the B payload');
    }
});

test('#757 Events off-CPU delta equals Overview off-CPU delta for the same windows', () => {
    for (const [aMs, bMs] of [[460, 260], [260, 460], [260, 0], [0, 260]]) {
        const a = window(aMs);
        const b = window(bMs);
        const events = buildEventsModel({ compare: true, a: a.events, b: b.events }, null);
        const overview = createOverviewView().build(
            { compare: true, a: a.overview, b: b.overview }, overviewCtx);
        const eventsDelta = waitingRows(events).reduce((sum, r) => sum + r.row.delta_ms, 0);
        const overviewDelta = waitingRows(overview).reduce((sum, r) => sum + r.row.delta_ms, 0);
        assert.equal(overviewDelta, aMs - bMs, 'overview fixture sanity: expected movement');
        assert.equal(eventsDelta, overviewDelta,
            `off-CPU delta parity A=${aMs}ms B=${bMs}ms: Events=${eventsDelta} Overview=${overviewDelta}`);
    }
});

test('#757 baselineUnavailable fallback still adds the row exactly once', () => {
    // The baselineUnavailable branch (events.js:50-62) already calls
    // addCpuWaitingRow directly; the #757 fix must not double-add there.
    const a = window(260).events;
    const model = buildEventsModel({ compare: true, a, b: null, baselineUnavailable: true }, null);
    assert.equal(model.compare, undefined, 'falls back to the ordinary (non-compare) model shape');
    assert.equal(waitingRows(model).length, 1);
    assert.equal(waitingRows(model)[0].row.total_ms, 260);
});

test('#757 Events renders the server AAS using its full requested window', () => {
    const a = window(460).events;
    const ordinary = buildEventsModel(a, null);
    const fallback = buildEventsModel({ compare: true, a, baselineUnavailable: true }, null);
    for (const model of [ordinary, fallback]) {
        const r = waitingRows(model)[0];
        assert.equal(r.row.aas, 0.23);
        assert.equal(r.cells[eventsConfig.columns.findIndex(c => c.key === 'aas')].html, '0.23');
    }
});

test('#757 compare columns render each payload AAS, including one-sided rows', () => {
    for (const [aMs, bMs] of [[460, 260], [260, 0], [0, 260]]) {
        const a = window(aMs).events, b = window(bMs).events;
        const model = buildEventsModel({ compare: true, a, b }, null);
        const r = waitingRows(model)[0];
        const cell = key => r.cells[compareEventsConfig.columns.findIndex(c => c.key === key)]?.html;
        assert.equal(cell('a_aas'), aMs ? (aMs / 2000).toFixed(2) : '—');
        assert.equal(cell('b_aas'), bMs ? (bMs / 2000).toFixed(2) : '—');
    }
});

test('#757 unavailable AAS stays unavailable; measured zero remains numeric', () => {
    const a = window(260).events;
    delete a.offcpu_aas;
    assert.equal(waitingRows(buildEventsModel(a, null))[0].row.aas, null);
    a.offcpu_aas = 0;
    assert.equal(waitingRows(buildEventsModel(a, null))[0].row.aas, 0);
});

test('#757 an available zero off-CPU row renders zero AAS', () => {
    const a = window(0).events;
    a.has_measured_cpu = true;
    const r = waitingRows(buildEventsModel(a, null))[0];
    assert.equal(r.row.total_ms, 0);
    assert.equal(r.row.aas, 0);
    assert.equal(r.cells[eventsConfig.columns.findIndex(c => c.key === 'aas')].html, '0.00');
});

test('#757 compare preserves measured zero and unavailable AAS separately', () => {
    const zero = window(0).events;
    zero.has_measured_cpu = true;
    const positive = window(260).events;
    for (const [a, b, wantA, wantB] of [
        [zero, positive, '0.00', '0.13'],
        [positive, zero, '0.13', '0.00'],
    ]) {
        const r = waitingRows(buildEventsModel({ compare: true, a, b }, null))[0];
        const cell = key => r.cells[compareEventsConfig.columns.findIndex(c => c.key === key)].html;
        assert.equal(cell('a_aas'), wantA);
        assert.equal(cell('b_aas'), wantB);
    }
    positive.offcpu_aas = null;
    const r = waitingRows(buildEventsModel({ compare: true, a: positive, b: zero }, null))[0];
    const cell = key => r.cells[compareEventsConfig.columns.findIndex(c => c.key === key)].html;
    assert.equal(cell('a_aas'), '—');
    assert.equal(cell('b_aas'), '0.00');
});
