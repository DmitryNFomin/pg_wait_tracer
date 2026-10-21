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
    stackedBar, eventStackedBar, createStableOrder, STABLE_ORDER_PASS_RATIO,
    BAR_MIN_SEG_PCT, BAR_BELOW_COLOR, BAR_OTHER_COLOR,
    createOrderResetGate, orderResetKey,
} from '../../web/static/lib/table.js';
import {
    overviewConfig, eventsConfig, buildOverflowNote, overflowNoteHtml,
    PCTL_OVERFLOW_SENTENCE, PCTL_FINITE_BOUND_SENTENCE, PCTL_UNRESOLVED_TEXT,
    queriesConfig, makeQueriesConfig,
    buildUnattributedNote, unattributedNoteHtml, createOverviewOrder, sessionsConfig,
    childRemainderRow,
} from '../../web/static/lib/builders/table-configs.js';
import { buildSummary } from '../../web/static/views/overview.js';
import { WAIT_CLASSES, classColor, eventColor, fmtMs, fmtUs } from '../../web/static/lib/format.js';
import { states as tableConfigFixtures } from '../fixtures/table-configs.mjs';

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

// #187 regression: the "CPU (running)" row must drill class=CPU (the bare
// server identity), not the raw display label — and the "CPU (waiting for a
// core)" row (a computed residual, no server-side class_name) must never
// drill at all, nor render as 'clickable' (no click there could ever do
// anything).
test('overview onClick: "CPU (running)" strips to class=CPU; "CPU (waiting for a core)" never drills', () => {
    assert.deepEqual(
        overviewConfig.onClick({ indent: 1, name: 'CPU (running)' }),
        { filterKey: 'class', filterValue: 'CPU', label: 'CPU' });
    assert.equal(
        overviewConfig.onClick({ indent: 1, name: 'CPU (waiting for a core)' }),
        null);
});

test('overview: "CPU (waiting for a core)" row is indent-1 but NOT clickable', () => {
    const rows = [
        { indent: 0, name: 'DB Time', ms: 12760, pct: 100, aas: 3.54 },
        { indent: 1, name: 'CPU (running)', ms: 4800, pct: 37.6, aas: 1.33 },
        { indent: 1, name: 'CPU (waiting for a core)', ms: 260, pct: 2.0, aas: 0.07 },
    ];
    const model = buildTableModel(overviewConfig, rows, null);
    assert.ok(model.rows[1].cls.includes('clickable'), 'CPU (running) is clickable');
    assert.ok(!model.rows[2].cls.includes('clickable'),
        'CPU (waiting for a core) is not clickable (no server class_name)');
});

// ── #435/#425 (contract item 1): idle is excluded, never a measured 0% ─────

test('#435 Idle row: %DB Time is "—" (excluded), not a 0% bar, even when the server sends numeric 0', () => {
    // src/server.c:3533 currently sends pct:0 for Idle (a recorded
    // contradiction with Events' own null, server.c:3661) -- the client
    // rule must hold against THAT shape, not only a hypothetical null one.
    const rows = [
        { indent: 0, name: 'DB Time', ms: 12760, pct: 100, aas: 3.54 },
        { indent: 0, name: 'Idle', ms: 45000, pct: 0, aas: 0.0 },
    ];
    const model = buildTableModel(overviewConfig, rows, null);
    const idleHtml = model.rows[1].cells[2].html;
    assert.ok(!idleHtml.includes('pct-bar'), idleHtml);
    assert.ok(!idleHtml.includes('0.0%'), idleHtml);
    assert.ok(idleHtml.includes('—'), idleHtml);   // em-dash
    assert.ok(idleHtml.includes('Excluded from DB Time'), idleHtml);
});

test('#435 Idle row: %DB Time stays "—" if the server ever sends null instead', () => {
    const rows = [{ indent: 0, name: 'Idle', ms: 45000, pct: null, aas: 0.0 }];
    const model = buildTableModel(overviewConfig, rows, null);
    assert.ok(model.rows[0].cells[2].html.includes('—'));
});

// ── #453: AAS column must be VISIBLY different for excluded rows, not just
// hover-relabeled -- a still frame (GIF/screenshot) cannot show a tooltip.

test('#453 Idle row: AAS cell shows the SAME visible em-dash as %DB Time, ' +
    'numeric occupancy labeled beside it, never bare under "AAS"', () => {
    const rows = [{ indent: 0, name: 'Idle', ms: 888000, pct: 0, aas: 24.62 }];
    const model = buildTableModel(overviewConfig, rows, null);
    const html = model.rows[0].cells[3].html;
    const plain = html.replace(/<[^>]*>/g, '');
    // The bare rate must never appear with nothing else around it -- that
    // IS the #453 bug (reads identically to real active-session AAS).
    assert.notEqual(plain.trim(), '24.62', html);
    assert.ok(html.includes('—'), html);             // em-dash is the headline glyph
    assert.ok(html.includes('24.62'), html);          // occupancy figure not lost
    assert.ok(html.includes('idle-excluded'), html);
    assert.ok(/idle occ/i.test(html), html);
    assert.ok(/not an active-session average|not active-session AAS/i.test(html), html);
});

// Control: a fixture where Idle's AAS (24.62) is ~19x DB Time's real AAS
// (1.28) -- demonstrating this is not a coincidence of small numbers. Both
// cells render under the SAME "AAS" column header; the control FAILS if a
// before/after plain-text strip of each cell is indistinguishable (the two
// would read as the same KIND of number to a viewer).
test('#453 control: Idle AAS (far exceeding real AAS) must not render ' +
    'identically to a real AAS value under the same column', () => {
    const rows = [
        { indent: 0, name: 'DB Time', ms: 12760, pct: 100, aas: 1.28 },
        { indent: 0, name: 'Idle', ms: 888000, pct: 0, aas: 24.62 },
    ];
    const model = buildTableModel(overviewConfig, rows, null);
    const dbHtml = model.rows[0].cells[3].html;
    const idleHtml = model.rows[1].cells[3].html;
    assert.equal(dbHtml, '1.28');   // real AAS: plain number, unmarked
    // Idle's cell must NOT be the same shape (a bare number) -- it carries
    // the dash + idle-excluded marker the real AAS cell does not.
    assert.notEqual(idleHtml, '24.62');
    assert.ok(idleHtml.includes('idle-excluded'), idleHtml);
    assert.ok(!dbHtml.includes('idle-excluded'), dbHtml);
});

test('#453 Idle row: a missing AAS value stays a bare "—", no fabricated occupancy number', () => {
    const rows = [{ indent: 0, name: 'Idle', ms: 45000, pct: 0, aas: null }];
    const model = buildTableModel(overviewConfig, rows, null);
    const html = model.rows[0].cells[3].html;
    assert.ok(html.includes('idle-excluded'), html);
    // No labeled occupancy figure beside the dash -- there is no number to show.
    assert.ok(!html.includes('idle-occ'), html);
});

test("#453 Idle's own indent-2 child (e.g. \"Other (background)\") gets the " +
    'SAME AAS treatment as the parent -- the pre-fix bug only covered the ' +
    'Overview PARENT row', () => {
    const rows = [
        { indent: 0, name: 'Idle', ms: 888000, pct: null, aas: 24.62 },
        { indent: 2, name: 'Other (background)', ms: 147600, pct: null, aas: 4.10 },
    ];
    const model = buildTableModel(overviewConfig, rows, null);
    const childHtml = model.rows[1].cells[3].html;
    assert.ok(childHtml.includes('idle-excluded'), childHtml);
    assert.ok(childHtml.includes('4.10'), childHtml);
    assert.ok(!/^4\.10$/.test(childHtml.replace(/<[^>]*>/g, '').trim()), childHtml);
});

test('#453 negative control: DB Time and a normal class row are UNCHANGED by the idle rule', () => {
    const rows = [
        { indent: 0, name: 'DB Time', ms: 12760, pct: 100, aas: 3.54 },
        { indent: 1, name: 'IO', ms: 3200, pct: 25.1, aas: 0.89 },
    ];
    const model = buildTableModel(overviewConfig, rows, null);
    assert.ok(model.rows[0].cells[2].html.includes('100.0%'));
    assert.ok(model.rows[1].cells[2].html.includes('pct-bar'));
    assert.equal(model.rows[0].cells[3].html, '3.54');
    assert.equal(model.rows[1].cells[3].html, '0.89');
});

// ── #457: isIdleRow must match the LITERAL server idle row name, never a
// substring -- a latent false positive, not yet live (no such row exists in
// today's server output), but #453 made it load-bearing: a false match no
// longer just mislabels a percentage, it blanks AAS too. A real indent-0
// row named e.g. "IdleInTransaction" is a genuine, active, DB-Time-bearing
// wait state and must render its real pct/AAS, not Idle's em-dash. ──────────

test('#457 a real row whose name merely CONTAINS "Idle" (IdleInTransaction) ' +
    'is NOT excluded from DB Time -- its pct and AAS render normally', () => {
    const rows = [
        { indent: 0, name: 'DB Time', ms: 12760, pct: 100, aas: 3.54 },
        { indent: 0, name: 'IdleInTransaction', ms: 3200, pct: 25.1, aas: 0.89 },
        { indent: 0, name: 'Idle', ms: 888000, pct: 0, aas: 24.62 },
    ];
    const model = buildTableModel(overviewConfig, rows, null);
    // IdleInTransaction: a real pct bar and a real AAS number, same as any
    // other included row -- NOT the idle-excluded em-dash treatment.
    assert.ok(model.rows[1].cells[2].html.includes('pct-bar'), model.rows[1].cells[2].html);
    assert.ok(!model.rows[1].cells[2].html.includes('idle-excluded'), model.rows[1].cells[2].html);
    assert.equal(model.rows[1].cells[3].html, '0.89');
    assert.ok(!model.rows[1].cells[3].html.includes('idle-excluded'), model.rows[1].cells[3].html);
    // The REAL Idle row (exact name) must still get the em-dash treatment --
    // this fix must not weaken the #435/#453 protection it sits beside.
    assert.ok(model.rows[2].cells[2].html.includes('idle-excluded'), model.rows[2].cells[2].html);
    assert.ok(model.rows[2].cells[3].html.includes('idle-excluded'), model.rows[2].cells[3].html);
});

// ── #440 (contract item 6): refresh ordering + near-tie hysteresis ─────────
//
// The fixture CONTRACTS.md names explicitly: shuffled identical input,
// alternating 100/101, then 100/106, a selected-row deletion, then an
// explicit re-sort. Identity = row.id; value = row.v (contribution stand-in).

function rowsOf(pairs) {   // [[id, v], ...] -> row objects
    return pairs.map(([id, v]) => ({ id, v }));
}
const byId = (id) => (r) => r.id === id;
const idOrder = (rows) => rows.map((r) => r.id);

function tracker() {
    return createStableOrder({ idKey: (r) => r.id, valueKey: (r) => r.v });
}

test('refresh-near-ties: first render sorts by descending value, then identity', () => {
    const t = tracker();
    const out = t.apply(rowsOf([['c', 50], ['a', 90], ['b', 90]]));
    // b/a tie at 90 -> identity tiebreak (a < b); c last.
    assert.deepEqual(idOrder(out), ['a', 'b', 'c']);
});

test('refresh-near-ties: shuffled IDENTICAL input never reorders after the first render', () => {
    const t = tracker();
    const first = t.apply(rowsOf([['x', 10], ['y', 10], ['z', 10]]));
    const established = idOrder(first);
    // Same values, every tick, but the INPUT ARRAY ORDER is reshuffled --
    // exactly what a server response with no ordering guarantee would do.
    const shuffles = [
        [['z', 10], ['x', 10], ['y', 10]],
        [['y', 10], ['z', 10], ['x', 10]],
        [['x', 10], ['z', 10], ['y', 10]],
    ];
    for (const s of shuffles) {
        assert.deepEqual(idOrder(t.apply(rowsOf(s))), established,
            'identical values must never jitter regardless of input order');
    }
});

test('refresh-near-ties: alternating 100/101 (within 5%) -> no jitter across many ticks', () => {
    const t = tracker();
    const established = idOrder(t.apply(rowsOf([['a', 100], ['b', 95]])));
    assert.deepEqual(established, ['a', 'b']);
    // b's value alternates 95 <-> 101 (every value here is well within 1.05x
    // of a's 100) across 6 ticks -- a real noisy-but-stable workload.
    for (let i = 0; i < 6; i++) {
        const bVal = i % 2 === 0 ? 101 : 95;
        const out = t.apply(rowsOf([['a', 100], ['b', bVal]]));
        assert.deepEqual(idOrder(out), ['a', 'b'],
            `tick ${i} (b=${bVal}): must not jitter`);
    }
});

test('refresh-near-ties: a swing to 106 (>5%) IS a material reorder', () => {
    const t = tracker();
    t.apply(rowsOf([['a', 100], ['b', 95]]));
    // b jumps to 106: 106 > 100 * 1.05 (105) -- strictly past the threshold.
    const out = t.apply(rowsOf([['a', 100], ['b', 106]]));
    assert.deepEqual(idOrder(out), ['b', 'a'],
        'b must pass a once it is genuinely, not marginally, ahead');
});

test('refresh-near-ties: 105 exactly does NOT pass (strictly greater, not >=)', () => {
    const t = tracker();
    t.apply(rowsOf([['a', 100], ['b', 95]]));
    const out = t.apply(rowsOf([['a', 100], ['b', 105]]));
    assert.deepEqual(idOrder(out), ['a', 'b']);
});

test('refresh-near-ties: a selected row\'s deletion drops it, keeps survivors\' order, no crash', () => {
    const t = tracker();
    t.apply(rowsOf([['a', 100], ['b', 95], ['c', 50]]));
    const out = t.apply(rowsOf([['a', 100], ['c', 50]]));   // 'b' (selected) gone
    assert.deepEqual(idOrder(out), ['a', 'c']);
    assert.equal(out.find(byId('b')), undefined);
});

test('refresh-near-ties: a newcomer is ranked in before the hysteresis passes run', () => {
    const t = tracker();
    t.apply(rowsOf([['a', 100], ['b', 95]]));
    // 'c' arrives at 200 -- far above both survivors; it must be able to
    // reach the top in the SAME tick it appears, not be stuck appended last
    // until a future tick "catches up" to it.
    const out = t.apply(rowsOf([['a', 100], ['b', 95], ['c', 200]]));
    assert.deepEqual(idOrder(out), ['c', 'a', 'b']);
});

test('refresh-near-ties: explicit re-sort (reset()) discards accumulated order', () => {
    const t = tracker();
    t.apply(rowsOf([['a', 100], ['b', 95]]));
    // Without reset(), 'b' at 101 would NOT pass 'a' at 100 (within 5%).
    const beforeReset = t.apply(rowsOf([['a', 100], ['b', 101]]));
    assert.deepEqual(idOrder(beforeReset), ['a', 'b']);
    t.reset();
    const afterReset = t.apply(rowsOf([['a', 100], ['b', 101]]));
    assert.deepEqual(idOrder(afterReset), ['b', 'a'],
        'a fresh first-render sort must rank by the exact current value');
});

test('refresh-near-ties NEGATIVE CONTROL: plain exact-descending-every-tick jitters on 100/101', () => {
    // This is the bug #440 fixes, reproduced with the policy literally
    // replaced by "sort exact descending every tick" -- demonstrates the
    // positive tests above are not vacuously true for ANY ordering.
    const plainDescendingEveryTick = (rows) =>
        rows.slice().sort((a, b) => b.v - a.v || String(a.id).localeCompare(String(b.id)))
            .map((r) => r.id);
    const seq = [
        rowsOf([['a', 100], ['b', 95]]),
        rowsOf([['a', 100], ['b', 101]]),   // within 5% -- must NOT reorder
        rowsOf([['a', 100], ['b', 95]]),
        rowsOf([['a', 100], ['b', 101]]),
    ];
    const orders = seq.map(plainDescendingEveryTick);
    const allStable = orders.every((o) => JSON.stringify(o) === JSON.stringify(orders[0]));
    assert.ok(!allStable,
        'sanity: plain descending-every-tick DOES jitter on this input (' +
        JSON.stringify(orders) + ') -- proves the policy, not the fixture, prevents it');
    // And createStableOrder, fed the exact same sequence, does not jitter.
    const t = tracker();
    const stableOrders = seq.map((rows) => idOrder(t.apply(rows)));
    const allStableReal = stableOrders.every(
        (o) => JSON.stringify(o) === JSON.stringify(stableOrders[0]));
    assert.ok(allStableReal, JSON.stringify(stableOrders));
});

// timeout: this exact case (two non-finite neighbors, -Infinity vs -Infinity)
// is what makes `>` -> `>=` in the hysteresis pass loop forever instead of
// just misbehaving -- -Infinity >= -Infinity is true, so every pass re-swaps
// the same pair. Bounded here so that regression fails fast instead of
// hanging the whole suite (verified: reverting to `>=` times out at 120s
// under the bare `node --test` run, SIGKILLed, no assertion output at all).
test('refresh-near-ties: unavailable (non-finite) values sort last and never pass', { timeout: 2000 }, () => {
    const t = tracker();
    const out = t.apply(rowsOf([['a', 100], ['b', NaN], ['c', null]]));
    assert.deepEqual(idOrder(out), ['a', 'b', 'c']);   // b/c tie at -Infinity -> identity
    const out2 = t.apply(rowsOf([['a', 100], ['b', NaN], ['c', null]]));
    assert.deepEqual(idOrder(out2), ['a', 'b', 'c'], 'stays stable, no spurious reorder');
});

test('STABLE_ORDER_PASS_RATIO is the documented 1.05 draft threshold', () => {
    assert.equal(STABLE_ORDER_PASS_RATIO, 1.05);
});

// ── #440: Overview sibling groups preserve hierarchy under hysteresis ───────

function overviewRow(indent, name, ms) { return { indent, name, ms, pct: 0, aas: 0 }; }

test('createOverviewOrder: DB Time/Idle bookends are NEVER ranked into the class pool', () => {
    const o = createOverviewOrder();
    // Idle's ms (45000) dwarfs every class -- it must NOT float to the top.
    const rows = [
        overviewRow(0, 'DB Time', 12760),
        overviewRow(1, 'CPU', 4800),
        overviewRow(1, 'IO', 3200),
        overviewRow(0, 'Idle', 45000),
    ];
    const out = o.apply(rows);
    assert.deepEqual(out.map((r) => r.name), ['DB Time', 'CPU', 'IO', 'Idle']);
});

test('createOverviewOrder: a class\'s event children move WITH it and reorder only among siblings', () => {
    const o = createOverviewOrder();
    // IO's parent ms (2900) is the EXACT sum of its two children below --
    // #484's remainder row only fires on a real gap, and this test is about
    // ordering, not about exercising that row (see the dedicated #484 tests
    // above for the gap case itself).
    const rows = [
        overviewRow(0, 'DB Time', 10000),
        overviewRow(1, 'IO', 2900),
        overviewRow(2, 'IO:DataFileRead', 2100),
        overviewRow(2, 'IO:WalSync', 800),
        overviewRow(1, 'Lock', 1500),
        overviewRow(2, 'Lock:relation', 1500),
    ];
    const out = o.apply(rows);
    assert.deepEqual(out.map((r) => r.name),
        ['DB Time', 'IO', 'IO:DataFileRead', 'IO:WalSync', 'Lock', 'Lock:relation']);
    // IO:WalSync jumps from 800 to 3000 (>5% past IO:DataFileRead's 2100) --
    // it must pass its SIBLING within IO, and must not leak into Lock's group.
    // IO's own ms is unchanged at 2900 here deliberately: the children now
    // SUM TO MORE than their parent (an impossible-in-practice case), which
    // must never render a (nonsensical, negative) remainder either.
    const rows2 = [
        overviewRow(0, 'DB Time', 10000),
        overviewRow(1, 'IO', 2900),
        overviewRow(2, 'IO:DataFileRead', 2100),
        overviewRow(2, 'IO:WalSync', 3000),
        overviewRow(1, 'Lock', 1500),
        overviewRow(2, 'Lock:relation', 1500),
    ];
    const out2 = o.apply(rows2);
    assert.deepEqual(out2.map((r) => r.name),
        ['DB Time', 'IO', 'IO:WalSync', 'IO:DataFileRead', 'Lock', 'Lock:relation']);
});

test('createOverviewOrder: a class swing past the 5% threshold reorders the class group', () => {
    const o = createOverviewOrder();
    o.apply([overviewRow(0, 'DB Time', 10000), overviewRow(1, 'CPU', 4800),
        overviewRow(1, 'IO', 3200)]);
    const out = o.apply([overviewRow(0, 'DB Time', 10000), overviewRow(1, 'CPU', 4800),
        overviewRow(1, 'IO', 6000)]);   // 6000 > 4800*1.05 (5040)
    assert.deepEqual(out.map((r) => r.name), ['DB Time', 'IO', 'CPU']);
});

test('createOverviewOrder.reset(): discards accumulated order for classes AND children', () => {
    const o = createOverviewOrder();
    o.apply([overviewRow(0, 'DB Time', 10000), overviewRow(1, 'CPU', 100),
        overviewRow(1, 'IO', 95)]);
    o.reset();
    const out = o.apply([overviewRow(0, 'DB Time', 10000), overviewRow(1, 'CPU', 95),
        overviewRow(1, 'IO', 100)]);
    // A fresh first-render sort ranks IO ahead of CPU by the exact current
    // value -- would still read CPU-first without the reset.
    assert.deepEqual(out.map((r) => r.name), ['DB Time', 'IO', 'CPU']);
});

test('createOverviewOrder: a class that disappears and returns starts its child group fresh', () => {
    const o = createOverviewOrder();
    o.apply([overviewRow(0, 'DB Time', 10000), overviewRow(1, 'Lock', 500),
        overviewRow(2, 'Lock:relation', 500)]);
    o.apply([overviewRow(0, 'DB Time', 10000)]);   // Lock momentarily absent
    const out = o.apply([overviewRow(0, 'DB Time', 10000), overviewRow(1, 'Lock', 500),
        overviewRow(2, 'Lock:relation', 400), overviewRow(2, 'Lock:tuple', 500)]);
    // A fresh child group sorts by current value, not stale history --
    // Lock:tuple (new, higher) ranks first.
    assert.deepEqual(out.map((r) => r.name),
        ['DB Time', 'Lock', 'Lock:tuple', 'Lock:relation']);
});

// ── Review finding: Idle's own indent-2 children attached to the WRONG
// parent (the preceding class), or vanished entirely when no class had
// been seen yet. Contract §6 requires hierarchy to be preserved, not
// reassigned -- and NEVER silently dropped. ───────────────────────────────

test("Idle's indent-2 child attaches to IDLE, not to the preceding class", () => {
    const o = createOverviewOrder();
    // Both parents' ms match their sole child exactly (#484: a real gap
    // would add a remainder row here and break the exact name-list
    // assertion below -- this test is about attachment, not the gap case).
    const rows = [
        overviewRow(0, 'DB Time', 10000),
        overviewRow(1, 'IO', 2100),
        overviewRow(2, 'IO:DataFileRead', 2100),
        overviewRow(0, 'Idle', 39000),
        overviewRow(2, 'Client:ClientRead', 39000),
    ];
    const out = o.apply(rows);
    assert.deepEqual(out.map((r) => r.name),
        ['DB Time', 'IO', 'IO:DataFileRead', 'Idle', 'Client:ClientRead']);
    // Specifically: Client:ClientRead must NOT render under IO (before
    // Idle) -- it must come AFTER Idle, as its own child.
    const idleIdx = out.findIndex((r) => r.name === 'Idle');
    const clientIdx = out.findIndex((r) => r.name === 'Client:ClientRead');
    assert.ok(clientIdx > idleIdx,
        `Client:ClientRead (${clientIdx}) must follow Idle (${idleIdx})`);
});

test('an indent-2 row with NO parent seen yet is never silently dropped', () => {
    const o = createOverviewOrder();
    // Pathological/defensive: an indent-2 row arrives before any indent-0
    // or indent-1 row at all (should not happen from a real server, but a
    // dropped row is worse than a defensively-placed one).
    const rows = [
        overviewRow(2, 'Orphan:child', 100),
        overviewRow(0, 'DB Time', 10000),
    ];
    const out = o.apply(rows);
    assert.ok(out.some((r) => r.name === 'Orphan:child'),
        `Orphan:child must not vanish: ${JSON.stringify(out.map((r) => r.name))}`);
});

test("Idle's children get their OWN nested hysteresis, independent of any class", () => {
    const o = createOverviewOrder();
    o.apply([
        overviewRow(0, 'DB Time', 10000),
        overviewRow(1, 'IO', 3200),
        overviewRow(0, 'Idle', 45000),
        overviewRow(2, 'Client:ClientRead', 39000),
        overviewRow(2, 'Timeout:PgSleep', 6000),
    ]);
    // Timeout:PgSleep jumps past ClientRead by >5% -- must reorder WITHIN
    // Idle's children, independent of IO (which has none here).
    const out = o.apply([
        overviewRow(0, 'DB Time', 10000),
        overviewRow(1, 'IO', 3200),
        overviewRow(0, 'Idle', 45000),
        overviewRow(2, 'Client:ClientRead', 39000),
        overviewRow(2, 'Timeout:PgSleep', 42000),
    ]);
    assert.deepEqual(out.map((r) => r.name),
        ['DB Time', 'IO', 'Idle', 'Timeout:PgSleep', 'Client:ClientRead']);
});

// ── #484: a parent must equal the sum of what is shown. compute.c caps a
// class's children at 5 server-side with no total_count/omitted-identity
// sent for the rest (unlike the table-level truncated/total_count pair
// buildTruncationRow reads -- time_model never emits that shape), so the
// ONLY exact fact available client-side is ms: row.ms minus the sum of the
// children actually rendered. These assert the STRONGER property the
// reviewer asked for -- the full rendered row set sums to the parent --
// not merely "a remainder row exists" (a weaker assertion that would pass
// even if the remainder carried the wrong number). ───────────────────────

function sumRenderedChildren(out, parentName) {
    const i = out.findIndex((r) => r.name === parentName);
    let sum = 0;
    for (let j = i + 1; j < out.length && out[j].indent >= 2; j++) sum += out[j].ms;
    return sum;
}

test('createOverviewOrder: a class whose children are capped gets a remainder row, and the parent equals the sum of every rendered row', () => {
    const o = createOverviewOrder();
    // The exact #484 GIF numbers (ms, not seconds): IO 70300 with 5 children
    // summing to 67800 -- a 2500ms gap the shipped frame left unexplained.
    const rows = [
        overviewRow(0, 'DB Time', 70300),
        overviewRow(1, 'IO', 70300),
        overviewRow(2, 'IO:BufFileWrite', 50200),
        overviewRow(2, 'IO:DataFileRead', 6500),
        overviewRow(2, 'IO:WalWrite', 4000),
        overviewRow(2, 'IO:DataFileWrite', 3700),
        overviewRow(2, 'IO:SLRURead', 3400),
    ];
    const out = o.apply(rows);
    const io = out.find((r) => r.name === 'IO');
    assert.equal(sumRenderedChildren(out, 'IO'), io.ms,
        'rendered children (including any remainder) must sum to the parent');
    const remainder = out.find((r) => r.isRemainder);
    assert.ok(remainder, 'a 2500ms gap must produce a remainder row');
    assert.equal(remainder.ms, 2500);
    assert.equal(remainder.indent, 2);
});

test('createOverviewOrder: children that already sum to their parent get no remainder row', () => {
    const o = createOverviewOrder();
    // Matches the brief's own Lock counter-example: exact reconciliation,
    // no row. A spurious near-zero row here would be the bypass this test
    // exists to catch -- the gate must be silent exactly when there is
    // nothing to disclose.
    const rows = [
        overviewRow(0, 'DB Time', 875400),
        overviewRow(1, 'Lock', 875400),
        overviewRow(2, 'Lock:relation', 716600),
        overviewRow(2, 'Lock:tuple', 92400),
        overviewRow(2, 'Lock:transactionid', 66400),
        overviewRow(2, 'Lock:page', 846),
    ];
    const out = o.apply(rows);
    assert.equal(sumRenderedChildren(out, 'Lock'), 716600 + 92400 + 66400 + 846);
    assert.ok(!out.some((r) => r.isRemainder), 'exact reconciliation must not add a row');
});

test('childRemainderRow: pure edge cases (bypass suite)', () => {
    // No children at all -- nothing truncated, nothing to disclose.
    assert.equal(childRemainderRow({ name: 'IO', ms: 100 }, []), null);
    assert.equal(childRemainderRow({ name: 'IO', ms: 100 }, null), null);
    // Exact reconciliation -- diff is 0.
    assert.equal(childRemainderRow({ name: 'IO', ms: 100 },
        [{ ms: 60 }, { ms: 40 }]), null);
    // Sub-rounding noise (fmtMs shows nothing below ~0.001ms) must not
    // produce a spurious row.
    assert.equal(childRemainderRow({ name: 'IO', ms: 100 },
        [{ ms: 60 }, { ms: 39.99999 }]), null);
    // Children summing to MORE than the parent (should not happen from a
    // real server, but must never render a negative-looking remainder).
    assert.equal(childRemainderRow({ name: 'IO', ms: 100 },
        [{ ms: 60 }, { ms: 60 }]), null);
    // The real #484 case: a non-trivial, exact gap.
    const r = childRemainderRow({ name: 'IO', ms: 70300 },
        [{ ms: 50200 }, { ms: 6500 }, { ms: 4000 }, { ms: 3700 }, { ms: 3400 }]);
    assert.ok(r);
    assert.equal(r.ms, 2500);
    assert.equal(r.indent, 2);
    assert.equal(r.isRemainder, true);
});

// Review finding: the remainder row's AAS cell used to fall through
// isExcludedFromDbTime() (pct == null is true for it too) straight into
// idleAasCellHtml(), which hardcodes "this is idle occupancy ... no session
// is 'active' while idle" -- FALSE for #484's remainder, which is real DB
// time in the class, merely not broken out by event name. The em-dash is
// still right (no per-row AAS is knowable for an unnamed bucket); the
// STORY behind it must not borrow Idle's.
test("#484 remainder row's AAS cell tells its OWN story, not Idle's", () => {
    const remainder = childRemainderRow({ name: 'IO', ms: 70300 },
        [{ ms: 50200 }, { ms: 6500 }, { ms: 4000 }, { ms: 3700 }, { ms: 3400 }]);
    const rows = [{ indent: 1, name: 'IO', ms: 70300, pct: 100, aas: 1.0 }, remainder];
    const model = buildTableModel(overviewConfig, rows, null);
    const html = model.rows[1].cells[3].html;
    assert.ok(html.includes('—'), html);   // still an em-dash: no per-row AAS to show
    // Must NOT claim this time is idle / excluded from DB Time -- it is the
    // opposite: real, counted DB time, just not itemized by event.
    assert.ok(!/idle/i.test(html), html);
    assert.ok(!/active-session average/i.test(html), html);
    // Must say what it actually is.
    assert.ok(/not broken out by event|per-event|event cap/i.test(html), html);
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

// #457: the summary bar's own idle-row finder has the SAME substring bug as
// isIdleRow in table-configs.js -- a real "IdleInTransaction" row must never
// be picked up as "the" Idle summary metric (it is a different, real wait
// state; reporting its time as "Idle" would misreport the summary bar).
test('#457 buildSummary never mistakes a substring match for the real Idle row', () => {
    const data = { wall_ms: 3600000, rows: [
        { indent: 0, name: 'DB Time', ms: 12500, pct: 100, aas: 3.47 },
        { indent: 0, name: 'IdleInTransaction', ms: 9000, pct: 10, aas: 0.5 },
    ] };
    const s = buildSummary(data, 4);
    assert.ok(!s.some(m => m.label === 'Idle'),
        'no real "Idle" row present -- IdleInTransaction must not stand in for it: ' +
        JSON.stringify(s));
});

test('#457 buildSummary still reports the real Idle row by its exact name', () => {
    const data = { wall_ms: 3600000, rows: [
        { indent: 0, name: 'DB Time', ms: 12500, pct: 100, aas: 3.47 },
        { indent: 0, name: 'IdleInTransaction', ms: 9000, pct: 10, aas: 0.5 },
        { indent: 0, name: 'Idle', ms: 45000 },
    ] };
    const s = buildSummary(data, 4);
    assert.deepEqual(s.find(m => m.label === 'Idle'), { label: 'Idle', value: '45.0s' });
});

// ── #472(ii): AAS discloses a window mostly before capture started ─────────
// Disclosure only -- the AAS NUMBER itself (dbRow.aas, computed server-side
// over whatever was actually captured) is UNCHANGED; this only appends a
// visible note when the window the user asked for is mostly uncaptured, so
// a low/zero AAS does not read as "the database was idle" when it actually
// reads "there is almost no data here yet".

test('#472(ii) buildSummary: captured < 90% of window appends a disclosure to AAS', () => {
    const data = { rows: [{ indent: 0, name: 'DB Time', ms: 12500, aas: 3.47 }] };
    // 900s window, capture only started 35s before the window's end.
    const window = { from: 0, to: 900e9, captureFromNs: 865e9 };
    const s = buildSummary(data, 4, window);
    assert.equal(s.find(m => m.label === 'AAS').value, '3.47 · captured 35s of 900s');
});

test('#472(ii) buildSummary: captured >= 90% of window -> no disclosure suffix', () => {
    const data = { rows: [{ indent: 0, name: 'DB Time', ms: 1000, aas: 1.0 }] };
    // 900s window, captured the last 850s (94.4%) -- comfortably above 90%.
    const window = { from: 0, to: 900e9, captureFromNs: 50e9 };
    const s = buildSummary(data, 4, window);
    assert.equal(s.find(m => m.label === 'AAS').value, '1.00');
});

test('#472(ii) buildSummary: no window argument is unchanged (back-compat)', () => {
    const data = { rows: [{ indent: 0, name: 'DB Time', ms: 1000, aas: 1.0 }] };
    assert.equal(buildSummary(data, 4).find(m => m.label === 'AAS').value, '1.00');
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

// #437: a saturated percentile used to render ">=16.4ms" -- a number-shaped
// value sitting next to an avg of 1.8s and a max of 2.6s in the SAME row,
// which reads as a measurement contradicting its own neighbours rather than
// as "unresolved". The standing rule (docs/VISUAL_CHECKLIST.md SEMANTICS) is
// that absence is reported, never rendered as a measured-looking value, so
// an overflowing cell's VISIBLE text is now the literal word "Unresolved" --
// never a number -- while the histogram drill, the lower-bound explanation
// (title) and Max (the real, exact tail) are all retained unchanged.
test('#437 overflow percentile renders "Unresolved", never a number', () => {
    const m = buildTableModel(eventsConfig, [evRow({
        p50_us: 16384, p95_us: 16384, p99_us: 16384, max_us: 2600000,
        p50_overflow: true, p95_overflow: true, p99_overflow: true,
    })], null);
    for (const ci of [P50_COL, P95_COL, P99_COL]) {
        const html = m.rows[0].cells[ci].html;
        // The rendered text is the literal word, never a number -- "16.4"
        // must not appear anywhere in the visible text.
        assert.equal(cellText(html), PCTL_UNRESOLVED_TEXT, `col ${ci}: ${html}`);
        assert.ok(!cellText(html).includes('16.4'), `col ${ci} still shows a number: ${html}`);
        // The disclosure is in words, not just the drill affordance: it
        // leads with the fact and points at the column that has the tail.
        // #437: the exact agreed sentence, verbatim.
        assert.ok(html.includes('title="Percentiles marked ≥16.4 ms ' +
            'fall in the open-ended ≥16.384 ms histogram bucket; ' +
            'they are lower bounds, and their actual values are ' +
            'unavailable.'), html);
    }
    // Max is the exact tail and stays exact.
    assert.ok(m.rows[0].cells[MAX_COL].html.includes('2.6s'),
        m.rows[0].cells[MAX_COL].html);
});

// #454: a normal-bucket (non-overflow) percentile is STILL a bound -- every
// one of the 16 histogram buckets (hist_upper_us, src/compute.c:1673) is an
// edge, not an exact observed duration, so a finite percentile now renders
// "<=" with its own (distinct from #437's ">=") disclosure sentence. The
// pre-#454 behavior this replaces printed the bare number here, which is
// exactly the #454 bug: a Count=1 live capture read "Max ~1.0ms / P50=2.0ms"
// with nothing marking the 2.0ms as a rounded bound bigger than Max.
test('#454 a normal-bucket percentile renders "<=", its own bound disclosure', () => {
    const m = buildTableModel(eventsConfig, [evRow({
        p50_us: 8, p95_us: 40, p99_us: 8192,
        p50_overflow: false, p95_overflow: false, p99_overflow: false,
    })], null);
    const p99 = m.rows[0].cells[P99_COL].html;
    assert.equal(cellText(p99), '\u22648.2ms', p99);
    assert.ok(p99.includes(PCTL_FINITE_BOUND_SENTENCE), p99);
    assert.ok(!p99.includes(PCTL_OVERFLOW_SENTENCE), p99);
    assert.ok(p99.includes('Click to see its latency distribution.'), p99);
    assert.equal(cellText(m.rows[0].cells[P50_COL].html), '\u22648\u00b5s',
        m.rows[0].cells[P50_COL].html);
});

test('#103/#454 rows with no overflow flags at all are finite bounds, never ">="', () => {
    // Old server / compare-baseline rows: absent key is not an overflow.
    const m = buildTableModel(eventsConfig, [evRow({ p95_us: 16384 })], null);
    const html = m.rows[0].cells[P95_COL].html;
    assert.ok(html.includes('\u226416.4ms'), html);
    assert.ok(!html.includes('\u2265'), html);
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
    assert.equal(cellText(cell.html), PCTL_UNRESOLVED_TEXT, cell.html);
    assert.ok(!cellText(cell.html).includes('16.4'), cell.html);
    // #437: the exact agreed sentence, verbatim.
    assert.ok(cell.html.includes('title="Percentiles marked \u226516.4 ms ' +
        'fall in the open-ended \u226516.384 ms histogram bucket; they ' +
        'are lower bounds, and their actual values are unavailable.'),
        cell.html);
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

// ── #437: beside-table overflow disclosure (not only on hover) ─────────────

// #454: both rows here carry FINITE percentiles too (the overflowing row's
// p50/p99 are untouched defaults, 8/90, non-overflow; the second row is
// entirely finite) -- so the note is now the TWO sentences together, not
// just the overflow one alone.
test('buildOverflowNote: one overflowing row + finite rows -> both sentences', () => {
    const note = buildOverflowNote([
        evRow({ p95_us: 16384, p95_overflow: true }),
        evRow({ name: 'IO:DataFileRead', event_id: 1 }),
    ]);
    assert.deepEqual(note,
        { text: PCTL_FINITE_BOUND_SENTENCE + ' ' + PCTL_OVERFLOW_SENTENCE });
    assert.ok(PCTL_OVERFLOW_SENTENCE.includes('lower bounds'));
    assert.ok(PCTL_OVERFLOW_SENTENCE.includes('unavailable'));
    // #437: "the true value is higher" wrongly excludes equality -- gone.
    assert.ok(!PCTL_OVERFLOW_SENTENCE.includes('higher'));
});

// #454: no row OVERFLOWED, but evRow({}) still carries finite percentiles
// (p50/p95/p99 8/40/90) -- those are bounds too, so the note is the finite
// sentence alone, never null. Only a page with NO percentile data anywhere
// (every value null) has nothing to disclose.
test('buildOverflowNote: no row overflowed, but finite percentiles present -> finite sentence only', () => {
    assert.deepEqual(buildOverflowNote([evRow({})]), { text: PCTL_FINITE_BOUND_SENTENCE });
});

test('buildOverflowNote: empty/null input or no percentile data anywhere -> null', () => {
    assert.equal(buildOverflowNote([]), null);
    assert.equal(buildOverflowNote(null), null);
    assert.equal(buildOverflowNote([evRow({
        p50_us: null, p95_us: null, p99_us: null,
    })]), null);
});

test('buildOverflowNote: a false overflow flag does not count as overflow (finite sentence only)', () => {
    const note = buildOverflowNote([evRow({ p99_overflow: false })]);
    assert.deepEqual(note, { text: PCTL_FINITE_BOUND_SENTENCE });
    assert.ok(!note.text.includes(PCTL_OVERFLOW_SENTENCE));
});

test('overflowNoteHtml: renders the sentence escaped; null model renders nothing', () => {
    const html = overflowNoteHtml({ text: PCTL_OVERFLOW_SENTENCE });
    assert.ok(html.includes('pctl-overflow-banner'), html);
    assert.ok(html.includes(PCTL_OVERFLOW_SENTENCE), html);
    assert.equal(overflowNoteHtml(null), '');
});

// #437/#454 CONTRACTS.md item 4's "single fixture": the existing
// events-overflow-pctl gallery/dev fixture, extended with the finite-bucket
// CONTROL row (LWLock:LockManager) called for there -- same 16384 number as
// an overflow row, but p99_overflow:false, so #454 renders it "<=" (a
// finite bound), never the overflow row's ">=".
test('events-overflow-pctl fixture: the finite-bucket control renders "<=", not ">="', () => {
    const st = tableConfigFixtures['events-overflow-pctl'];
    const model = buildTableModel(eventsConfig, st.rows, st.sort);
    const control = model.rows.find(r => r.row.name === 'LWLock:LockManager');
    assert.ok(control, 'fixture carries the finite-bucket control row');
    assert.equal(cellText(control.cells[P99_COL].html), '≤16.4ms',
        control.cells[P99_COL].html);
    assert.ok(!control.cells[P99_COL].html.includes('≥'),
        control.cells[P99_COL].html);
    // Every genuinely overflowing row in the SAME fixture reads "Unresolved"
    // (#437), never a number -- and never the finite control's "<=" either.
    for (const name of ['Lock:relation', 'Timeout:CheckpointWriteDelay',
                         'IO:DataFileRead', 'CPU*']) {
        const row = model.rows.find(r => r.row.name === name);
        assert.equal(cellText(row.cells[P99_COL].html), PCTL_UNRESOLVED_TEXT,
            `${name}: ${row.cells[P99_COL].html}`);
    }
    // #454: the fixture has BOTH overflowing rows and finite-bound rows, so
    // the beside-table note is both sentences together.
    assert.deepEqual(buildOverflowNote(st.rows),
        { text: PCTL_FINITE_BOUND_SENTENCE + ' ' + PCTL_OVERFLOW_SENTENCE });
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

// ── #439 (contract item 1/3): Queries' % column names its actual, NARROWER
// denominator (query-attributed active time), never the Overview total ──────

test('#439 Queries pct column is labeled "% attributed DB Time", not "%DB"', () => {
    const headerLabels = queriesConfig.columns.map(c => c.label);
    assert.ok(headerLabels.includes('% attributed DB Time'), headerLabels.join(', '));
    assert.ok(!headerLabels.includes('%DB'), headerLabels.join(', '));
});

// ── #439-label: "Other (background)" is NOT a contradiction of #453's
// "excluded from DB Time" rule -- compute.c:745 classifies background
// non-idle work as PGWT_CAT_BG (genuinely inside DB Time), while this row
// is the UNRELATED hidden-Activity parking remainder under Idle
// (PGWT_PCT_DB_IDLE, compute.c:1283). Same English words, two different
// quantities -- the label is what is misleading, not the semantics. Fixed
// by relabeling only; no src/ change, no wire field renamed.

test('#439-label Overview renders "Other (background)" as "Background idle (parked)"', () => {
    const rows = [
        { indent: 0, name: 'DB Time', ms: 12500, pct: 100, aas: 3.47 },
        { indent: 0, name: 'Idle', ms: 3000, pct: null, aas: null },
        { indent: 2, name: 'Other (background)', ms: 1476, pct: null, aas: 4.10 },
    ];
    const model = buildTableModel(overviewConfig, rows, null);
    const html = model.rows[2].cells[0].html;
    assert.ok(html.includes('Background idle (parked)'), html);
    assert.ok(!html.includes('Other (background)'), html);
});

test('#439-label Sessions DB Time header discloses background processes are included', () => {
    const headerCol = sessionsConfig.columns.find(c => c.key === 'db_time_ms');
    assert.equal(headerCol.label, 'DB Time');
    assert.ok(headerCol.title && headerCol.title.length > 0, headerCol.title);
    assert.ok(headerCol.title.includes('non-idle time of this process'), headerCol.title);
    assert.ok(headerCol.title.includes('background processes included'), headerCol.title);
    // The header model (what mountTable actually paints into <th title="...">)
    // must carry it through -- a column-config-only title that buildTableModel
    // drops would never reach the DOM.
    const model = buildTableModel(sessionsConfig, [{ pid: 1, db_time_ms: 100 }], null);
    const header = model.headers.find(h => h.key === 'db_time_ms');
    assert.equal(header.title, headerCol.title);
});

test('buildUnattributedNote: discloses the excluded time, pct and occurrence count', () => {
    const note = buildUnattributedNote({
        db_time_ms: 9500, unattributed_available: true,
        unattributed_ms: 500, unattributed_count: 4,
    });
    assert.ok(note.text.includes(fmtMs(500)), note.text);
    // 500 / (9500+500) = 5.0%
    assert.ok(note.text.includes('5.0%'), note.text);
    assert.ok(note.text.includes('4 occurrences'), note.text);
    assert.ok(note.text.includes('not attributed to any query'), note.text);
});

test('buildUnattributedNote: unattributed_available:false is reported, not silently dropped', () => {
    const note = buildUnattributedNote({ unattributed_available: false,
        db_time_ms: 9500, unattributed_ms: 0, unattributed_count: 0 });
    assert.ok(note, 'the false-availability case still returns a note');
    assert.ok(note.text.includes('not available'), note.text);
    assert.ok(!note.text.includes('0ms'), note.text);   // never a fabricated number
});

test('buildUnattributedNote: genuinely zero unattributed time -> no note (nothing to disclose)', () => {
    assert.equal(buildUnattributedNote({
        db_time_ms: 9500, unattributed_available: true,
        unattributed_ms: 0, unattributed_count: 0,
    }), null);
});

test('buildUnattributedNote: missing/malformed input -> null, never a crash or a fabricated note', () => {
    assert.equal(buildUnattributedNote(null), null);
    assert.equal(buildUnattributedNote({}), null);
    assert.equal(buildUnattributedNote({ unattributed_ms: 'lots' }), null);
    assert.equal(buildUnattributedNote({ unattributed_ms: -5 }), null);
});

test('unattributedNoteHtml: renders the note; null renders nothing', () => {
    const html = unattributedNoteHtml({ text: '500ms unattributed.' });
    assert.ok(html.includes('unattributed-note'), html);
    assert.ok(html.includes('500ms unattributed.'), html);
    assert.equal(unattributedNoteHtml(null), '');
});

// ── #441 (contract item 3): Queries' count/avg name what they actually are ──

test('#441 exact fidelity: "Wait/CPU intervals" + "Avg interval", not "Waits"/"Avg Wait"', () => {
    const cfg = makeQueriesConfig('exact');
    const countCol = cfg.columns.find(c => c.key === 'count');
    const avgCol = cfg.columns.find(c => c.key === 'avg_us');
    assert.equal(countCol.label, 'Wait/CPU intervals');
    assert.equal(avgCol.label, 'Avg interval');
    assert.equal(avgCol.format({ avg_us: 50 }), fmtUs(50));
});

test('#441 sampled/mixed fidelity: "Observations"; Avg interval is unavailable, never invented', () => {
    for (const fidelity of ['sampled', 'mixed']) {
        const cfg = makeQueriesConfig(fidelity);
        const countCol = cfg.columns.find(c => c.key === 'count');
        const avgCol = cfg.columns.find(c => c.key === 'avg_us');
        assert.equal(countCol.label, 'Observations', fidelity);
        // #441 negative control: a sampled row CAN carry a numeric avg_us
        // (the server might still send one) -- the column must still
        // refuse to show it, proving this is a fidelity-gated suppression,
        // not merely "format the field that happens to be present".
        assert.equal(avgCol.format({ avg_us: 50 }), '—', fidelity);
    }
});

test('#441 default queriesConfig export stays the exact-tier shape (gallery/back-compat)', () => {
    const countCol = queriesConfig.columns.find(c => c.key === 'count');
    assert.equal(countCol.label, 'Wait/CPU intervals');
});

// ── #471: Plan% must never exceed 100% -- a query with more plan events ──
// than executions (a plan was recorded for an execution still in flight,
// not yet counted as a completed exec) is a real, expected state, not a
// data error; the ratio is clamped at the natural ceiling (every execution
// has at most one plan counted against it) and the overflow is disclosed
// via title text rather than a misleading ">100%" style number.

test('#471 Plan% caps at 100% (never 200%) when plan_count > exec_count, and discloses why', () => {
    const cfg = makeQueriesConfig('exact');
    const planRatioCol = cfg.columns.find(c => c.key === 'plan_ratio');
    const html = planRatioCol.format({ exec_count: 1, plan_count: 2 });
    assert.ok(html.includes('100%'), html);
    assert.ok(!html.includes('200%'), html);
    assert.ok(html.includes('2 plans for executions still in flight'), html);
});

test('#471 Plan% normal case (plan_count <= exec_count) is the plain ratio, no overflow note', () => {
    const cfg = makeQueriesConfig('exact');
    const planRatioCol = cfg.columns.find(c => c.key === 'plan_ratio');
    assert.equal(planRatioCol.format({ exec_count: 10, plan_count: 5 }), '50%');
});

test('#471 Plans column keeps the RAW uncapped count even when plan_count > exec_count', () => {
    const cfg = makeQueriesConfig('exact');
    const plansCol = cfg.columns.find(c => c.key === 'plan_count');
    assert.equal(plansCol.format({ plan_count: 2, exec_count: 1 }), '2');
});

// ── #474: the live-tick refresh loop must not defeat its own hysteresis ──
//
// #440 added createStableOrder's near-tie hysteresis plus createOrderResetGate
// to reset it on a REAL window/filter change. But every call site fed the
// gate raw {from, to} -- and in live mode `to` (and `from`) advance on EVERY
// 5s refresh tick, so the gate's key changed every tick and reset() fired
// every tick, silently defeating the hysteresis it was supposed to apply.
// #440 shipped with no test exercising a live tick, so this regression was
// invisible on the unit suite (static {from,to} inputs never "tick").
// orderResetKey(ctx) is the fix's pure key function: during a live tick
// (ctx.isLiveTick()) the key is {live:true, spanSecs} -- stable while the
// live window just slides forward -- and only the SPAN (not the absolute
// edges) changing, or leaving live mode, produces a new key.

function fakeLiveCtx(from, to, liveRangeSecs) {
    return { isLiveTick: () => true, timeRange: { from, to, liveRangeSecs } };
}
function fakeStaticCtx(from, to) {
    return { isLiveTick: () => false, timeRange: { from, to, liveRangeSecs: 900 } };
}

test('#474 orderResetKey: a live tick advancing from/to keeps the SAME key (span unchanged)', () => {
    const k1 = orderResetKey(fakeLiveCtx(100, 1000, 900));
    const k2 = orderResetKey(fakeLiveCtx(130, 1030, 900));
    assert.deepEqual(k1, k2, 'advancing from/to on a live tick must not change the reset key');
});

test('#474 orderResetKey: a span change produces a DIFFERENT key even during a live tick', () => {
    const k1 = orderResetKey(fakeLiveCtx(100, 1000, 900));
    const k2 = orderResetKey(fakeLiveCtx(100, 1000, 300));
    assert.notDeepEqual(k1, k2, 'a real span change must still change the reset key');
});

test('#474 orderResetKey: outside a live tick, the key is the raw window (explicit navigation resets)', () => {
    const k1 = orderResetKey(fakeStaticCtx(100, 1000));
    const k2 = orderResetKey(fakeStaticCtx(200, 1100));
    assert.notDeepEqual(k1, k2, 'a non-live window change must reset (unchanged pre-#474 behavior)');
});

test('#474 createOrderResetGate + orderResetKey: live ticks do not reset order; a real span change does', () => {
    const resetIfChanged = createOrderResetGate();
    let resets = 0;
    const tracker = { reset: () => { resets++; } };
    const filters = 'f1';

    resetIfChanged(tracker, { window: orderResetKey(fakeLiveCtx(100, 1000, 900)), filters });
    assert.equal(resets, 1, 'first call always establishes the baseline key');

    resetIfChanged(tracker, { window: orderResetKey(fakeLiveCtx(130, 1030, 900)), filters });
    assert.equal(resets, 1, 'advancing from/to on a live tick must not reset');

    resetIfChanged(tracker, { window: orderResetKey(fakeLiveCtx(160, 1060, 900)), filters });
    assert.equal(resets, 1, 'a second live tick still must not reset');

    resetIfChanged(tracker, { window: orderResetKey(fakeLiveCtx(160, 1060, 300)), filters });
    assert.equal(resets, 2, 'a real span change must reset');
});
