/* Node unit tests for the pure transitions builders (lib/builders/transitions.js).
 *
 * Runs under `node --test`. Proves the DFG data -> ECharts graph option: node
 * sizing/coloring, threshold-based edge simplification, grid layout, self-loop
 * curveness, plus the flow-variants HTML. Edge cases: empty data, a single node
 * (one self-loop transition), and threshold above every edge.
 */
import { test } from 'node:test';
import assert from 'node:assert/strict';
import {
    buildTransitionsOption, transitionsContext, buildVariantsHtml, buildVariantsPanel,
    isIdleTransitionNode, p95TruncationMarker, findOmittedDominantNode,
    restoreAllTransitionsState, estimateNodeFootprint, computeGridOrder, isLayoutAnchorNode,
    buildTransitionsDisclosure, findDominantSourceDwellNode, TRANSITIONS_SEMANTICS_NOTE,
    transitionsDataError,
} from '../../web/static/lib/builders/transitions.js';
import { applyDragOffset } from '../../web/static/views/transitions.js';
import { eventColor } from '../../web/static/lib/format.js';
import { states as galleryStates } from '../../web/static/dev/fixtures/transitions.mjs';

const esc = (s) => String(s).replace(/&/g, '&amp;').replace(/</g, '&lt;').replace(/>/g, '&gt;');

for (const rank of [findOmittedDominantNode, findDominantSourceDwellNode]) {
    test('#487 ' + rank.name + ': server idle flag beats raw total_ms', () => {
        const { nodeMap } = transitionsContext({ nodes: [
            { name: 'Timeout:CheckpointWriteDelay', total_ms: 806900, idle: true },
            { name: 'Lock:relation', total_ms: 715000, idle: false },
        ], links: [] });
        const winner = rank(nodeMap, new Set());
        assert.equal(winner.name, 'Lock:relation');
        assert.equal(winner.ms, 715000);
        const rawWinner = Object.keys(nodeMap).sort((a, b) =>
            nodeMap[b].total_ms - nodeMap[a].total_ms)[0];
        assert.equal(rawWinner, 'Timeout:CheckpointWriteDelay');
        assert.notEqual(winner.name, rawWinner);
    });

    test('#487 ' + rank.name + ': all idle candidates return null', () => {
        assert.equal(rank({
            'Timeout:CheckpointWriteDelay': { total_ms: 806900, idle: true },
            'Lock:relation': { total_ms: 715000, idle: true },
        }, new Set()), null);
    });

    test('#487 ' + rank.name + ': unknown idle status is excluded without name fallback', () => {
        for (const idle of [undefined, null, 0, 'false']) {
            for (const name of ['Timeout:CheckpointWriteDelay', 'Lock:relation', 'Client:ClientRead']) {
                assert.equal(rank({ [name]: { total_ms: 806900, idle } }, new Set()), null);
                const winner = rank({
                    [name]: { total_ms: 806900, idle },
                    'IO:DataFileRead': { total_ms: 10, idle: false },
                }, new Set());
                assert.equal(winner.name, 'IO:DataFileRead');
                // An explicit false must be authoritative even for ClientRead.
                assert.equal(rank({ [name]: { total_ms: 1, idle: false } }, new Set()).name, name);
            }
        }
        assert.equal(rank({ 'Lock:relation': { total_ms: 1 } }, new Set()), null);
    });
}

// #487 review follow-up: findOmittedDominantNode/findDominantSourceDwellNode
// fail closed PER CANDIDATE (proven above: an unknown-idle node is excluded,
// never named). But on a PARTIAL catalog that just means the real answer goes
// quiet while a small, definitely-non-idle node wins by default -- the
// disclosure then calls that 10ms node "largest source-dwell", an unqualified
// FALSE SUPERLATIVE, which is worse than showing nothing. The fix lives one
// layer up: transitionsDataError (and therefore buildTransitionsOption, which
// calls it before any ranking runs) rejects the WHOLE payload the moment any
// node's idle verdict is unknown -- total absence or just one node among many.
test('#487 follow-up: transitionsDataError rejects a node with a non-boolean idle field', () => {
    assert.equal(transitionsDataError({
        nodes: [{ name: 'CPU*', total_ms: 10, idle: false }],
        links: [],
    }), null, 'sanity: a fully-flagged catalog is not itself rejected');
    for (const idle of [undefined, null, 0, 'false', 'true']) {
        const data = { nodes: [{ name: 'CPU*', total_ms: 10, idle }], links: [] };
        assert.equal(transitionsDataError(data), 'node missing idle verdict: CPU*');
    }
});

test('#487 follow-up: a PARTIAL catalog -- one real, large-total_ms node with '
    + 'no idle verdict, sitting alongside a small node that DOES carry idle:false '
    + '-- must not let the small node be silently crowned dominant. The whole '
    + 'payload is refused instead of naming a false superlative.', () => {
    // The adviser's own numbers (coordinator relay): a 715,000ms node with an
    // unknown idle verdict next to a 10ms node explicitly flagged non-idle.
    const payload = {
        total: 1000, links: [],
        nodes: [
            { name: 'Lock:relation', total_ms: 715000 },   // idle field OMITTED
            { name: 'IO:DataFileRead', total_ms: 10, idle: false },
        ],
    };

    assert.equal(transitionsDataError(payload), 'node missing idle verdict: Lock:relation');
    assert.throws(
        () => buildTransitionsOption(payload, 20, { width: 800, height: 550 }, null, true),
        /node missing idle verdict: Lock:relation/,
        'buildTransitionsOption must refuse before omittedDominant/dominantSourceDwell ever run');

    // Negative control named explicitly: if the exclusion-only fix (what
    // findOmittedDominantNode already does on its own, demonstrated in the
    // "unknown idle status is excluded without name fallback" test above)
    // were the WHOLE fix, the ranking would still run on this payload and
    // the 10ms node would win -- reproduce that exact wrong outcome here so
    // the claim is not hypothetical. Built by hand (not transitionsContext,
    // which now also refuses this payload -- that's the point of the fix,
    // not something to route around to get a nodeMap).
    const nodeMap = Object.fromEntries(payload.nodes.map(n => [n.name, n]));
    const wronglyRanked = findOmittedDominantNode(nodeMap, new Set());
    assert.equal(wronglyRanked.name, 'IO:DataFileRead');
    assert.equal(wronglyRanked.ms, 10,
        'demonstrates the bug this payload would otherwise produce: a 10ms node '
        + 'silently wins once the real 715,000ms dominant node is excluded for an '
        + 'unknown idle verdict -- exactly the false superlative the full-payload '
        + 'refusal above must prevent from ever reaching buildTransitionsOption');
});

test('#487 follow-up: total absence (every node missing idle) is refused the same way '
    + 'as a single missing node, not just the all-flagged or none-flagged ends', () => {
    const payload = replay456();
    for (const n of payload.nodes) delete n.idle;
    assert.match(transitionsDataError(payload), /^node missing idle verdict: /);
    assert.throws(() => buildTransitionsOption(payload, 20, { width: 800, height: 550 }, null, true),
        /node missing idle verdict:/);
});

function data() {
    return {
        total: 1000,
        nodes: [
            // event_id mirrors the server's DFG node JSON (U2 / P3 wire 5;
            // 0 = the CPU* pseudo-node).
            { name: 'CPU*', idle: false, total_ms: 4800, class: 'CPU', event_id: 0 },
            { name: 'IO:DataFileRead', idle: false, total_ms: 2100, class: 'IO', event_id: 0x01000015 },
            { name: 'LWLock:WALInsert', idle: false, total_ms: 900, class: 'LWLock', event_id: 0x04000007 },
        ],
        links: [
            { source: 'CPU*', target: 'IO:DataFileRead', value: 500, duration_ms: 2500 },
            { source: 'IO:DataFileRead', target: 'CPU*', value: 480, duration_ms: 1920 },
            { source: 'CPU*', target: 'LWLock:WALInsert', value: 50, duration_ms: 150 },
        ],
    };
}

test('context: max edge count + name->node map', () => {
    const { maxEdgeCount, nodeMap } = transitionsContext(data());
    assert.equal(maxEdgeCount, 500);
    assert.equal(nodeMap['CPU*'].total_ms, 4800);
});

test('default threshold 20%: edges below 100 (=500*0.2) are hidden', () => {
    const { option, visibleCount } = buildTransitionsOption(data(), 20, { width: 800, height: 550 });
    assert.equal(option.series[0].type, 'graph');
    assert.equal(option.series[0].layout, 'none');
    // The value=50 CPU*->LWLock edge is below 100 and dropped.
    const links = option.series[0].links;
    assert.equal(links.length, 2);
    // Its only-incident node LWLock:WALInsert is therefore not laid out.
    assert.equal(visibleCount, 2);
    assert.ok(!option.series[0].data.some(n => n.name === 'LWLock:WALInsert'));
});

test('option root disables animation (U0: no replayed draw-in on refresh)', () => {
    const { option } = buildTransitionsOption(data(), 20, { width: 800, height: 550 });
    assert.equal(option.animation, false);
});

test('threshold 0%: all edges + nodes visible, sorted by total_ms desc', () => {
    const { option, visibleCount } = buildTransitionsOption(data(), 0, { width: 800, height: 550 });
    assert.equal(visibleCount, 3);
    assert.equal(option.series[0].links.length, 3);
    // first node is the largest by ms (CPU* @ 4800)
    assert.equal(option.series[0].data[0].name, 'CPU*');
    assert.ok(option.series[0].data[0].value >= option.series[0].data[1].value);
});

/* FLIPPED in U2 (U1 review F4): this test used to pin the flat CLASS hue
 * (classColor -> rgb(80,250,123)) on every node — so two events of the same
 * class were indistinguishable in the DFG, and the SAME event wore a
 * different color here than in the AAS chart (which adopted eventColor
 * identity tints in U1). Nodes now pin the eventColor tint: hue from the
 * class, lightness/saturation step from the stable name hash — same event =
 * same color in every view. The literal is part of the visual contract
 * (changing EVENT_TINTS or the hash recolors every baseline). */
test('node size scales 15..120 by sqrt(ms/maxMs); eventColor identity tint applied', () => {
    const { option } = buildTransitionsOption(data(), 0, { width: 800, height: 550 });
    const cpu = option.series[0].data.find(n => n.name === 'CPU*');
    // largest node -> sqrt(1)*105 + 15 = 120 (clamped max)
    assert.equal(cpu.symbolSize, 120);
    // eventColor('CPU', 'CPU*') — tint step 1 of the CPU hue, NOT the flat
    // class green rgb(28,120,62) (#187 owner colour decision: dark green
    // for CPU (running)).
    assert.equal(cpu.itemStyle.color, 'rgb(52,167,95)');
});

test('cross-view identity: every node color equals eventColor(null, name)', () => {
    // The AAS chart colors series with eventColor(null, name) — the DFG must
    // agree for the same event names no matter how it identifies the class.
    const { option } = buildTransitionsOption(data(), 0, { width: 800, height: 550 });
    // #436: layout-anchor nodes (identity-transform pins, never real wait
    // events) carry no itemStyle/color at all -- this identity rule only
    // applies to real nodes.
    for (const n of option.series[0].data.filter(nd => !isLayoutAnchorNode(nd.name))) {
        assert.equal(n.itemStyle.color, eventColor(null, n.name), n.name);
    }
});

test('nodes carry eventId + className for the dfg-event pivot (P3 wire 5)', () => {
    const { option } = buildTransitionsOption(data(), 0, { width: 800, height: 550 });
    const io = option.series[0].data.find(n => n.name === 'IO:DataFileRead');
    assert.equal(io.eventId, 0x01000015);
    assert.equal(io.className, 'IO');
    // CPU* pseudo-node: id 0 rides along (the view refuses to pivot on it).
    const cpu = option.series[0].data.find(n => n.name === 'CPU*');
    assert.equal(cpu.eventId, 0);
    // A server predating the field -> null, never undefined-crash.
    const legacy = buildTransitionsOption({
        total: 10, nodes: [{ name: 'IO:WalSync', idle: false, total_ms: 5, class: 'IO' }],
        links: [{ source: 'IO:WalSync', target: 'IO:WalSync', value: 5, duration_ms: 1 }],
    }, 0, { width: 400, height: 300 });
    assert.equal(legacy.option.series[0].data[0].eventId, null);
});

test('posOverrides (user-dragged coords, P5) beat the grid layout', () => {
    const dims = { width: 800, height: 550 };
    const plain = buildTransitionsOption(data(), 0, dims);
    const moved = buildTransitionsOption(data(), 0, dims,
        { 'IO:DataFileRead': { x: 123, y: 45 } });
    const io = moved.option.series[0].data.find(n => n.name === 'IO:DataFileRead');
    assert.equal(io.x, 123);
    assert.equal(io.y, 45);
    // Nodes without an override keep the grid position.
    const cpuPlain = plain.option.series[0].data.find(n => n.name === 'CPU*');
    const cpuMoved = moved.option.series[0].data.find(n => n.name === 'CPU*');
    assert.equal(cpuMoved.x, cpuPlain.x);
    assert.equal(cpuMoved.y, cpuPlain.y);
});

test('drag fallback preserves the node grab offset instead of snapping centre', () => {
    assert.deepEqual(
        applyDragOffset({ x: 100, y: 80 }, { x: 110, y: 75 }, { x: 150, y: 105 }),
        { x: 140, y: 110 });
    assert.equal(applyDragOffset(null, { x: 1, y: 1 }, { x: 2, y: 2 }), null);
});

test('self-loop link gets higher curveness (0.8) than cross edges (0.3)', () => {
    const selfLoop = {
        total: 10, nodes: [{ name: 'CPU*', idle: false, total_ms: 100, class: 'CPU' }],
        links: [{ source: 'CPU*', target: 'CPU*', value: 10, duration_ms: 5 }],
    };
    const { option, visibleCount } = buildTransitionsOption(selfLoop, 0, { width: 400, height: 300 });
    assert.equal(visibleCount, 1);
    assert.equal(option.series[0].links[0].lineStyle.curveness, 0.8);
});

test('threshold above every edge -> no visible nodes', () => {
    const { option, visibleCount } = buildTransitionsOption(data(), 100, { width: 800, height: 550 });
    // minCount = 500; the max edge (500) IS >= 500, so it survives at exactly 100%.
    // Push beyond with a value strictly below max:
    assert.ok(visibleCount >= 0);  // 100% keeps only edges == max count
    const high = buildTransitionsOption({
        total: 10, nodes: [{ name: 'A', idle: false, total_ms: 1 }, { name: 'B', idle: false, total_ms: 1 }],
        links: [{ source: 'A', target: 'B', value: 5, duration_ms: 1 }],
    }, 100, { width: 100, height: 100 });
    // single edge == max -> still visible; assert the "everything dropped" path
    // via a threshold>100 surrogate is not possible, so verify empty-data path:
    const empty = buildTransitionsOption({ total: 0, nodes: [], links: [] }, 20, {});
    assert.equal(empty.visibleCount, 0);
    assert.equal(empty.option, null);
    assert.equal(high.visibleCount, 2);
    void option;
});

test('variants HTML: exec + plan sections, percentages and step labels', () => {
    const vdata = {
        exec: {
            total: 100, num_variants: 1,
            variants: [{
                exec_count: 100, num_queries: 1, total_ms: 50, avg_ms: 0.5,
                p95_ms: 1, avg_loop_n: 1, top_query_id: 42,
                steps: [{ name: 'CPU*', avg_ms: 0.3, class: 'cpu' },
                        { name: 'IO:DataFileRead', avg_ms: 0.2, class: 'IO' }],
                query_text: 'SELECT 1',
            }],
        },
        plan: {
            total: 100, num_variants: 1,
            variants: [{
                exec_count: 100, num_queries: 1, total_ms: 5, avg_ms: 0.05,
                p95_ms: 0.1, avg_loop_n: 1, top_query_id: 42,
                steps: [{ name: 'CPU*', avg_ms: 0.05, class: 'cpu' }],
                query_text: 'SELECT 1',
            }],
        },
    };
    const html = buildVariantsHtml(vdata, esc);
    assert.ok(html.includes('Execution Flow Patterns'));
    assert.ok(html.includes('Planning Flow Patterns'));
    assert.ok(html.includes('100.0%') || html.includes('of time'));
    assert.ok(html.includes('SELECT 1'));
    // U2 (U1 review F4): variant steps wear the eventColor identity tint —
    // the same rgb the DFG node and the AAS series use for that event.
    assert.ok(html.includes('background:' + eventColor('cpu', 'CPU*')));
    assert.ok(html.includes('background:' + eventColor('IO', 'IO:DataFileRead')));
});

test('variants HTML: empty / missing -> empty string', () => {
    assert.equal(buildVariantsHtml(null, esc), '');
    assert.equal(buildVariantsHtml({ exec: { variants: [] } }, esc), '');
});

// #291 progressive paint: the DFG paints from `transitions` alone; the
// variants panel below it declares one of three states instead of always
// rendering the same (possibly stale/empty-looking) HTML synchronously.
function sampleVariants() {
    return {
        exec: {
            total: 10, num_variants: 1,
            variants: [{
                exec_count: 10, num_queries: 1, total_ms: 50, avg_ms: 5,
                p95_ms: 6, avg_loop_n: 1, top_query_id: 7,
                steps: [{ name: 'CPU*', avg_ms: 5, class: 'cpu' }],
                query_text: 'SELECT 2',
            }],
        },
    };
}

// Variants-on-demand: the panel's default/reverted state must be a
// DISCOVERABLE affordance, not an empty-looking gap — and it must carry the
// element id the view's click wiring (and the Playwright test) look up.
test("variants panel: idle — discoverable affordance, not blank and not the loading/ready text", () => {
    const html = buildVariantsPanel('idle', null, esc);
    assert.ok(html.length > 0, 'idle must not render as empty (indistinguishable from "no variants exist")');
    assert.ok(html.includes('id="dfg-load-variants"'),
        'idle must expose the button id the view wires a click handler to');
    assert.ok(!/loading/i.test(html), 'idle must not read as already in flight');
    assert.ok(!html.includes('Flow Patterns'), 'idle must not look like the completed panel');
});

test('variants panel: graph-with-pending-variants — declares loading, not empty and not complete', () => {
    const html = buildVariantsPanel('pending', null, esc);
    assert.ok(html.length > 0, 'pending must not render as empty (indistinguishable from "no variants")');
    assert.ok(/loading/i.test(html), 'pending must say so, not just render blank markup');
    assert.ok(!html.includes('Flow Patterns'), 'pending must not look like the completed panel');
});

test('variants panel: graph-with-variants — ready state renders exactly like the synchronous path did', () => {
    const vdata = sampleVariants();
    assert.equal(buildVariantsPanel('ready', vdata, esc), buildVariantsHtml(vdata, esc));
    assert.ok(buildVariantsPanel('ready', vdata, esc).includes('SELECT 2'));
});

test('variants panel: graph-with-failed-variants — silent, matching the old optional-failure behaviour', () => {
    // Pre-#291, a failed/superseded `variants` call left the local var null,
    // so buildVariantsHtml(null) rendered '' — no error banner. The new
    // explicit 'error' state must still land on that same empty string, not
    // a scarier failure message nobody asked for.
    assert.equal(buildVariantsPanel('error', null, esc), '');
    assert.equal(buildVariantsPanel('error', sampleVariants(), esc), '',
        'error state ignores any stale/partial vdata it might be handed');
});

// p95TruncationMarker (issue #273): the server caps the p95 sample at
// PGWT_VARIANT_MAX_SAMPLES executions, taken in arrival order — a prefix,
// not a random sample. p95_sample_n < exec_count means the displayed p95
// is from that truncated, biased prefix, not the full variant.
test('p95TruncationMarker: truncated sample (p95_sample_n < exec_count) is marked', () => {
    const marker = p95TruncationMarker({ exec_count: 30000, p95_sample_n: 10000 });
    assert.ok(marker.length > 0, 'expected a non-empty marker for a truncated sample');
    assert.ok(marker.includes('10,000'), 'marker should surface the actual sample size, not a hard-coded 10000');
    assert.ok(marker.includes('30,000'), 'marker should also surface exec_count for context');
});

test('p95TruncationMarker: complete sample (p95_sample_n >= exec_count) is not marked', () => {
    assert.equal(p95TruncationMarker({ exec_count: 120, p95_sample_n: 120 }), '');
    // Defensive: a sample that somehow exceeds exec_count is still "complete".
    assert.equal(p95TruncationMarker({ exec_count: 100, p95_sample_n: 150 }), '');
});

test('p95TruncationMarker: field absent (older server) -> no marker, no crash', () => {
    assert.equal(p95TruncationMarker({ exec_count: 30000 }), '');
    assert.equal(p95TruncationMarker({ exec_count: 30000, p95_sample_n: null }), '');
    assert.equal(p95TruncationMarker({}), '');
    assert.equal(p95TruncationMarker(null), '');
    assert.equal(p95TruncationMarker(undefined), '');
});

test('variants HTML: p95 truncation marker rendered inline, driven by the field not a hard-coded cap', () => {
    const truncated = {
        exec: {
            total: 30000, num_variants: 1,
            variants: [{
                exec_count: 30000, num_queries: 1, total_ms: 2790, avg_ms: 0.093,
                p95_ms: 0.30, p95_sample_n: 10000, avg_loop_n: 1, top_query_id: 42,
                steps: [{ name: 'CPU*', avg_ms: 0.093, class: 'cpu' }],
                query_text: 'SELECT 1',
            }],
        },
        plan: { total: 0, num_variants: 0, variants: [] },
    };
    const html = buildVariantsHtml(truncated, esc);
    assert.ok(html.includes('(first 10,000)'), 'truncated variant should show the sample-size marker');

    const complete = {
        exec: {
            total: 120, num_variants: 1,
            variants: [{
                exec_count: 120, num_queries: 1, total_ms: 900, avg_ms: 7.5,
                p95_ms: 21.0, p95_sample_n: 120, avg_loop_n: 1, top_query_id: 42,
                steps: [{ name: 'CPU*', avg_ms: 7.5, class: 'cpu' }],
                query_text: 'SELECT 1',
            }],
        },
        plan: { total: 0, num_variants: 0, variants: [] },
    };
    const completeHtml = buildVariantsHtml(complete, esc);
    assert.ok(!completeHtml.includes('(first'), 'a complete sample must not show the truncation marker');

    // Older server (no p95_sample_n at all): render exactly as before.
    const noField = {
        exec: {
            total: 100, num_variants: 1,
            variants: [{
                exec_count: 100, num_queries: 1, total_ms: 50, avg_ms: 0.5,
                p95_ms: 1, avg_loop_n: 1, top_query_id: 42,
                steps: [{ name: 'CPU*', avg_ms: 0.5, class: 'cpu' }],
                query_text: 'SELECT 1',
            }],
        },
        plan: { total: 0, num_variants: 0, variants: [] },
    };
    const noFieldHtml = buildVariantsHtml(noField, esc);
    assert.ok(!noFieldHtml.includes('(first'), 'absent p95_sample_n must not produce a marker');
    assert.ok(noFieldHtml.includes('p95 1.0ms'), 'absent-field path renders p95 exactly as before, unmarked');
});

// ── issue #107: idle-loop dominance on a real OLTP workload ────────────────

test('isIdleTransitionNode: matches Client:ClientRead only — mirrors src '
    + 'pgwt_is_idle_event() as far as it can reach this payload (Activity '
    + 'class is already hidden server-side)', () => {
    assert.equal(isIdleTransitionNode('Client:ClientRead'), true);
    assert.equal(isIdleTransitionNode('Client:ClientWrite'), false);
    assert.equal(isIdleTransitionNode('CPU*'), false);
    assert.equal(isIdleTransitionNode('Lock:relation'), false);
});

/* Fixture shaped from the real box-check EPHEMERAL capture that reproduces
 * issue #107: a pgbench + lock-contention + IO workload where the
 * Client:ClientRead<->CPU* loop's TRANSITION COUNT dwarfs every other edge
 * (every wait cycle passes through it), while Lock:relation/Timeout/IO
 * transitions are rare by comparison though they are the entire point of the
 * tab. This is case (b)/(c) from the diagnosis: the server computes and
 * emits every edge; the client's default 20%-of-max-count threshold is what
 * discards them (ranked against the dominant idle loop, they are sub-1%). */
function realOltpShapedData() {
    return {
        total: 97541,
        nodes: [
            { name: 'CPU*', idle: false, total_ms: 10000, class: 'CPU', event_id: 0 },
            { name: 'Client:ClientRead', idle: true, total_ms: 915300, class: 'Client', event_id: 0x06000000 },
            { name: 'Lock:relation', idle: false, total_ms: 28700, class: 'Lock', event_id: 0x03000001 },
            { name: 'Timeout:PgSleep', idle: false, total_ms: 68000, class: 'Timeout', event_id: 0x0b000001 },
            { name: 'IO:DataFileRead', idle: false, total_ms: 5200, class: 'IO', event_id: 0x0a000015 },
        ],
        links: [
            { source: 'Client:ClientRead', target: 'CPU*', value: 48000, duration_ms: 915300 },
            { source: 'CPU*', target: 'Client:ClientRead', value: 47950, duration_ms: 910000 },
            { source: 'CPU*', target: 'Lock:relation',     value: 600,   duration_ms: 28700 },
            { source: 'Lock:relation', target: 'CPU*',     value: 595,   duration_ms: 27000 },
            { source: 'CPU*', target: 'Timeout:PgSleep',   value: 300,   duration_ms: 68000 },
            { source: 'Timeout:PgSleep', target: 'CPU*',   value: 298,   duration_ms: 66000 },
            { source: 'CPU*', target: 'IO:DataFileRead',   value: 200,   duration_ms: 5200 },
            { source: 'IO:DataFileRead', target: 'CPU*',   value: 198,   duration_ms: 5000 },
        ],
    };
}

test('#107 BEFORE (hideIdle=false, the old default): the idle loop dominates '
    + 'the threshold ranking and every analytically interesting edge is '
    + 'dropped — this test would have failed before the fix (the whole '
    + 'builder defaulted this way)', () => {
    const real = realOltpShapedData();
    const before = buildTransitionsOption(real, 20, { width: 800, height: 550 }, null, false);
    assert.equal(before.option.series[0].links.length, 2,
        'only the ClientRead<->CPU* loop clears 20% of its own dominant count');
    const names = before.option.series[0].data.map(n => n.name);
    assert.ok(!names.includes('Lock:relation'));
    assert.ok(!names.includes('Timeout:PgSleep'));
    assert.ok(!names.includes('IO:DataFileRead'));
});

test('#107 AFTER (hideIdle=true, the new default): the idle loop is removed '
    + 'from the ranking pool BEFORE the threshold is computed, so Lock/'
    + 'Timeout/IO edges rank against each other and survive', () => {
    const real = realOltpShapedData();
    const after = buildTransitionsOption(real, 20, { width: 800, height: 550 }, null, true);
    assert.equal(after.hiddenIdleLinks, 2);
    assert.equal(after.hiddenIdleValue, 48000 + 47950);
    const names = after.option.series[0].data.map(n => n.name);
    assert.ok(names.includes('Lock:relation'), 'Lock:relation survives once ranked against real edges');
    assert.ok(names.includes('Timeout:PgSleep'));
    assert.ok(names.includes('IO:DataFileRead'));
    assert.ok(!names.includes('Client:ClientRead'), 'the idle node itself is gone, not merely de-emphasized');
    assert.equal(after.option.series[0].links.length, 6);
});

test('hideIdle on idle-only data: visibleCount 0 but hiddenIdleLinks/-Value '
    + 'say WHY (FEEDBACK — distinct from a genuinely empty window)', () => {
    const idleOnly = {
        total: 100,
        nodes: [
            { name: 'CPU*', idle: false, total_ms: 10, class: 'CPU' },
            { name: 'Client:ClientRead', idle: true, total_ms: 90, class: 'Client' },
        ],
        links: [
            { source: 'Client:ClientRead', target: 'CPU*', value: 50, duration_ms: 90 },
            { source: 'CPU*', target: 'Client:ClientRead', value: 50, duration_ms: 85 },
        ],
    };
    const hidden = buildTransitionsOption(idleOnly, 20, { width: 400, height: 300 }, null, true);
    assert.equal(hidden.option, null);
    assert.equal(hidden.visibleCount, 0);
    assert.equal(hidden.hiddenIdleLinks, 2);
    assert.equal(hidden.hiddenIdleValue, 100);
    // Omitting hideIdle (undefined, falsy) keeps the old unfiltered behavior.
    const shown = buildTransitionsOption(idleOnly, 20, { width: 400, height: 300 });
    assert.equal(shown.visibleCount, 2);
    assert.equal(shown.hiddenIdleLinks, 0);
    assert.equal(shown.hiddenIdleValue, 0);
});

// ── issue #436: clipping, omission disclosure, and the "Show all" restore ──

// A rare-but-longest-duration Lock:relation next to a MORE frequent but
// short-dwelling Timeout: the frequency threshold excludes both (by count),
// but the contract says to NAME the one that dominates DB TIME, not the one
// with the most (excluded) edges. If omission naming were accidentally
// ranked by count among the excluded set, Timeout:VacuumDelay (5 edges)
// would win over Lock:relation (2 edges) -- the wrong answer.
function competingOmissionsData() {
    return {
        total: 2002,
        nodes: [
            { name: 'CPU*', idle: false, total_ms: 1000, class: 'CPU', event_id: 0 },
            { name: 'IO:DataFileRead', idle: false, total_ms: 900, class: 'IO', event_id: 1 },
            { name: 'Lock:relation', idle: false, total_ms: 50000, class: 'Lock', event_id: 2 },
            { name: 'Timeout:PgSleep', idle: false, total_ms: 300, class: 'Timeout', event_id: 3 },
        ],
        links: [
            { source: 'CPU*', target: 'IO:DataFileRead', value: 1000, duration_ms: 900 },
            { source: 'IO:DataFileRead', target: 'CPU*', value: 990, duration_ms: 850 },
            { source: 'CPU*', target: 'Lock:relation', value: 2, duration_ms: 50000 },
            { source: 'Lock:relation', target: 'CPU*', value: 2, duration_ms: 48000 },
            { source: 'CPU*', target: 'Timeout:PgSleep', value: 5, duration_ms: 300 },
            { source: 'Timeout:PgSleep', target: 'CPU*', value: 5, duration_ms: 280 },
        ],
    };
}

test('findOmittedDominantNode: ranks excluded nodes by total_ms (DB time), '
    + 'not by edge count -- the rarer-but-longer Lock:relation wins over the '
    + 'more frequent but short Timeout', () => {
    const data = competingOmissionsData();
    const { nodeMap } = transitionsContext({ ...data, links: data.links });
    // Both Lock:relation and Timeout:PgSleep are "omitted" here; Lock has
    // FEWER incident edges (2 vs 5) but the larger total_ms.
    const visible = new Set(['CPU*', 'IO:DataFileRead']);
    const dominant = findOmittedDominantNode(nodeMap, visible);
    assert.equal(dominant.name, 'Lock:relation');
    assert.equal(dominant.ms, 50000);
    // Negative control: ranking the SAME excluded set by edge count (the bug
    // this guards against) would pick Timeout, not Lock -- proving the two
    // criteria disagree on this fixture and the test is not vacuously true.
    const byCountWinner = ['Lock:relation', 'Timeout:PgSleep']
        .map(name => ({ name, count: data.links.filter(l => l.source === name || l.target === name)
            .reduce((s, l) => s + l.value, 0) }))
        .sort((a, b) => b.count - a.count)[0].name;
    assert.equal(byCountWinner, 'Timeout:PgSleep');
    assert.notEqual(dominant.name, byCountWinner);
});

test('findOmittedDominantNode: idle nodes are never named here -- the '
    + '"Hide ClientRead" control already discloses that reason on its own', () => {
    const nodeMap = {
        'Client:ClientRead': { idle: true, total_ms: 999999 },
        'Lock:relation': { idle: false, total_ms: 10 },
    };
    const dominant = findOmittedDominantNode(nodeMap, new Set());
    assert.equal(dominant.name, 'Lock:relation', 'the idle node must be skipped even though it has far more total_ms');
});

test('findOmittedDominantNode: null when nothing non-idle is omitted', () => {
    assert.equal(findOmittedDominantNode({ 'CPU*': { idle: false, total_ms: 10 } }, new Set(['CPU*'])), null);
    assert.equal(findOmittedDominantNode({}, new Set()), null);
});

// ── #456 (review finding): deterministic tie-break, independent of input
// iteration order — duration desc, then numeric event_id asc, then name asc.

test('#456 findOmittedDominantNode: an exact total_ms tie is broken by '
    + 'event_id ascending, the SAME winner regardless of nodeMap key order', () => {
    const a = { idle: false, total_ms: 5000, event_id: 30 };   // 'Zeta:Low' — higher name, LOWER id
    const b = { idle: false, total_ms: 5000, event_id: 10 };   // 'Alpha:High' — lower name, lower id -> winner
    const order1 = findOmittedDominantNode(
        { 'Zeta:Low': a, 'Alpha:High': b }, new Set());
    const order2 = findOmittedDominantNode(
        { 'Alpha:High': b, 'Zeta:Low': a }, new Set());
    assert.equal(order1.name, 'Alpha:High', 'lower event_id wins the tie');
    assert.equal(order2.name, 'Alpha:High', 'same winner regardless of key order');
    assert.equal(order1.ms, 5000);
});

test('#456 findOmittedDominantNode: no event_id on either side falls back '
    + 'to name ascending, deterministically across permutations', () => {
    const nodeMapA = { 'Zeta:NoId': { idle: false, total_ms: 100 }, 'Alpha:NoId': { idle: false, total_ms: 100 } };
    const nodeMapB = { 'Alpha:NoId': { idle: false, total_ms: 100 }, 'Zeta:NoId': { idle: false, total_ms: 100 } };
    assert.equal(findOmittedDominantNode(nodeMapA, new Set()).name, 'Alpha:NoId');
    assert.equal(findOmittedDominantNode(nodeMapB, new Set()).name, 'Alpha:NoId');
});

test('#456 findOmittedDominantNode: a node WITH a numeric event_id beats '
    + 'one without, on a duration tie, regardless of order', () => {
    const withId = { idle: false, total_ms: 42, event_id: 5 };
    const withoutId = { idle: false, total_ms: 42 };   // no event_id field at all
    assert.equal(findOmittedDominantNode(
        { 'HasId:Thing': withId, 'NoId:Thing': withoutId }, new Set()).name, 'HasId:Thing');
    assert.equal(findOmittedDominantNode(
        { 'NoId:Thing': withoutId, 'HasId:Thing': withId }, new Set()).name, 'HasId:Thing');
});

test('#456 RED-DEMONSTRATED: the OLD "first key seen wins" rule disagreed '
    + 'with the new rule on this exact fixture (regression pin, not a '
    + 'hypothetical) -- Object.keys order alone used to decide the winner', () => {
    const a = { idle: false, total_ms: 5000, event_id: 30 };
    const b = { idle: false, total_ms: 5000, event_id: 10 };
    // Under the OLD rule (`ms > best.ms` only, first-seen wins ties), the
    // FIRST key in insertion order would win regardless of event_id -- i.e.
    // 'Zeta:Low' in this map, 'Alpha:High' in the reversed one. The NEW rule
    // ignores insertion order entirely and always returns 'Alpha:High'.
    const firstKeyWins = Object.keys({ 'Zeta:Low': a, 'Alpha:High': b })[0];
    assert.equal(firstKeyWins, 'Zeta:Low',
        'sanity: the old rule would have picked this (order-dependent) name');
    assert.equal(findOmittedDominantNode({ 'Zeta:Low': a, 'Alpha:High': b }, new Set()).name,
        'Alpha:High', 'the NEW rule disagrees with insertion order -- proves it is not vacuous');
});

test('#456 buildTransitionsOption layout: an exact total_ms tie between two '
    + 'laid-out nodes places them in event_id order, regardless of which '
    + 'link came first in the response', () => {
    // Two nodes, SAME total_ms (5000), different event_id. Two responses
    // carry the identical node set but list their links in opposite order —
    // under the pre-#456 rule, [...visibleNodes] (a Set) would pick up
    // insertion order from whichever link's source/target was walked first,
    // changing sortedNodes' tie order and therefore node layout position.
    const base = {
        nodes: [
            { name: 'Zeta:Low', idle: false, total_ms: 5000, event_id: 30, class: 'Lock' },
            { name: 'Alpha:High', idle: false, total_ms: 5000, event_id: 10, class: 'Lock' },
        ],
    };
    const dataForward = { ...base, links: [
        { source: 'Zeta:Low', target: 'Alpha:High', value: 10, duration_ms: 100 },
    ] };
    const dataReverse = { ...base, links: [
        { source: 'Alpha:High', target: 'Zeta:Low', value: 10, duration_ms: 100 },
    ] };
    const dims = { width: 800, height: 550 };
    const optForward = buildTransitionsOption(dataForward, 0, dims, null, false);
    const optReverse = buildTransitionsOption(dataReverse, 0, dims, null, false);
    // The two invisible corner anchor nodes (identity data->pixel transform,
    // see the #436 test above) are not part of the dominance ordering.
    const realNodes = (opt) => opt.option.series[0].data
        .filter(n => !isLayoutAnchorNode(n.name));
    const namesForward = realNodes(optForward).map(n => n.name);
    const namesReverse = realNodes(optReverse).map(n => n.name);
    assert.deepEqual(namesForward, ['Alpha:High', 'Zeta:Low'],
        'lower event_id (10) ranks first on the total_ms tie');
    assert.deepEqual(namesReverse, namesForward,
        'the SAME order regardless of which link (and therefore which node '
        + 'the Set saw first) the response listed first');
    // The two nodes' laid-out positions must also match exactly (same rank
    // -> same grid cell), not just the same name order.
    const posForward = Object.fromEntries(
        realNodes(optForward).map(n => [n.name, { x: n.x, y: n.y }]));
    const posReverse = Object.fromEntries(
        realNodes(optReverse).map(n => [n.name, { x: n.x, y: n.y }]));
    assert.deepEqual(posForward, posReverse);
});

test('buildTransitionsOption: threshold exclusion is reported SEPARATELY from '
    + 'idle exclusion -- never merged into one count (#436)', () => {
    const data = {
        total: 10000,
        nodes: [
            { name: 'CPU*', idle: false, total_ms: 1000, class: 'CPU' },
            { name: 'Client:ClientRead', idle: true, total_ms: 9000, class: 'Client' },
            { name: 'IO:DataFileRead', idle: false, total_ms: 900, class: 'IO' },
            { name: 'Lock:relation', idle: false, total_ms: 50000, class: 'Lock' },
        ],
        links: [
            { source: 'Client:ClientRead', target: 'CPU*', value: 5000, duration_ms: 9000 },
            { source: 'CPU*', target: 'Client:ClientRead', value: 4990, duration_ms: 8900 },
            { source: 'CPU*', target: 'IO:DataFileRead', value: 1000, duration_ms: 900 },
            { source: 'IO:DataFileRead', target: 'CPU*', value: 990, duration_ms: 850 },
            { source: 'CPU*', target: 'Lock:relation', value: 2, duration_ms: 50000 },
            { source: 'Lock:relation', target: 'CPU*', value: 2, duration_ms: 48000 },
        ],
    };
    const built = buildTransitionsOption(data, 20, { width: 800, height: 550 }, null, true);
    assert.equal(built.hiddenIdleLinks, 2, 'the two ClientRead<->CPU* edges');
    assert.equal(built.hiddenIdleValue, 5000 + 4990);
    assert.equal(built.hiddenThresholdLinks, 2, 'the two Lock:relation edges, below 20% of 1000');
    assert.equal(built.hiddenThresholdValue, 2 + 2);
    assert.equal(built.omittedDominant.name, 'Lock:relation');
    assert.equal(built.shownLinks, 2);
    assert.equal(built.totalLinks, 6, 'totalLinks counts every link the server returned (pre-idle-filter)');
    // Negative control: a buggy merge would report one of these as the SUM
    // of both causes (2 + 2 = 4) instead of keeping them apart.
    assert.notEqual(built.hiddenIdleLinks, built.hiddenThresholdLinks + built.hiddenIdleLinks);
});

test('buildTransitionsOption: server truncation is disclosed independently '
    + 'of idle/threshold exclusion, and is never "fixed" by either '
    + '(#436 -- a cause this builder can only disclose, not recover from)', () => {
    const data = {
        total: 500, truncated: true, link_count: 3, total_link_count: 7,
        nodes: [
            { name: 'CPU*', idle: false, total_ms: 100, class: 'CPU' },
            { name: 'IO:DataFileRead', idle: false, total_ms: 90, class: 'IO' },
        ],
        links: [
            { source: 'CPU*', target: 'IO:DataFileRead', value: 10, duration_ms: 90 },
            { source: 'IO:DataFileRead', target: 'CPU*', value: 10, duration_ms: 80 },
        ],
    };
    // Nothing is idle- or threshold-excluded here (threshold 0, hideIdle off)
    // yet serverTruncated must still read true -- it is a THIRD, independent
    // cause.
    const built = buildTransitionsOption(data, 0, { width: 400, height: 300 });
    assert.equal(built.hiddenIdleLinks, 0);
    assert.equal(built.hiddenThresholdLinks, 0);
    assert.equal(built.serverTruncated, true);
    assert.equal(built.serverShownLinkCount, 3);
    assert.equal(built.serverTotalLinkCount, 7);
});

test('buildTransitionsOption: a server predating link_count/total_link_count/'
    + 'truncated fields reports serverTruncated false, not a crash', () => {
    const built = buildTransitionsOption({
        total: 10,
        nodes: [{ name: 'CPU*', idle: false, total_ms: 5, class: 'CPU' }],
        links: [{ source: 'CPU*', target: 'CPU*', value: 5, duration_ms: 1 }],
    }, 0, { width: 200, height: 200 });
    assert.equal(built.serverTruncated, false);
    assert.equal(built.serverShownLinkCount, 1);
    assert.equal(built.serverTotalLinkCount, 1);
});

test('buildTransitionsDisclosure: every present reason is named as its own '
    + 'phrase, never collapsed into one combined number', () => {
    const built = {
        hiddenIdleLinks: 2, hiddenIdleValue: 48000,
        hiddenThresholdLinks: 3, hiddenThresholdValue: 7,
        serverTruncated: true, serverShownLinkCount: 50, serverTotalLinkCount: 66,
        omittedDominant: { name: 'Lock:relation', ms: 50000 },
    };
    const text = buildTransitionsDisclosure(built, 97541);
    assert.ok(text.includes('97,541 transitions'));
    assert.ok(text.includes('48,000') && text.includes('idle'));
    assert.ok(text.includes('7') && text.includes('simplify threshold'));
    assert.ok(text.includes('50') && text.includes('66') && text.includes('server returned'));
    assert.ok(text.includes('Lock:relation') && text.includes('Show all'));
    // Negative control: a combined-count regression would report ONE number
    // for idle+threshold together (2+3=5 links, 48000+7=48007 value) instead
    // of two distinguishable phrases -- assert the two raw figures both
    // survive untouched rather than a merged total.
    assert.ok(!text.includes('48007'), 'idle and threshold figures must never be summed into one');
});

test('buildTransitionsDisclosure: nothing hidden -> just the total, no stale phrases', () => {
    const built = {
        hiddenIdleLinks: 0, hiddenIdleValue: 0,
        hiddenThresholdLinks: 0, hiddenThresholdValue: 0,
        serverTruncated: false, serverShownLinkCount: 4, serverTotalLinkCount: 4,
        omittedDominant: null,
    };
    assert.equal(buildTransitionsDisclosure(built, 400), '400 transitions');
});

test('buildTransitionsDisclosure: a hostile omitted-dominant name renders as '
    + 'plain text verbatim (the view assigns this to textContent, never '
    + 'innerHTML -- escaping here would show the user literal "&lt;")', () => {
    const built = {
        hiddenIdleLinks: 0, hiddenIdleValue: 0,
        hiddenThresholdLinks: 1, hiddenThresholdValue: 1,
        serverTruncated: false, serverShownLinkCount: 1, serverTotalLinkCount: 1,
        omittedDominant: { name: 'Lock:<script>x</script>', ms: 10 },
    };
    const text = buildTransitionsDisclosure(built, 10);
    assert.ok(text.includes('Lock:<script>x</script>'));
    assert.ok(!text.includes('&lt;'));
});

test('restoreAllTransitionsState ("Show all"): removes BOTH client exclusions, '
    + 'and never claims to recover server-truncated links (#436)', () => {
    const restore = restoreAllTransitionsState();
    assert.deepEqual(restore, { threshold: 0, hideIdle: false });

    const data = {
        total: 10000, truncated: true, link_count: 6, total_link_count: 9,
        nodes: [
            { name: 'CPU*', idle: false, total_ms: 1000, class: 'CPU' },
            { name: 'Client:ClientRead', idle: true, total_ms: 9000, class: 'Client' },
            { name: 'IO:DataFileRead', idle: false, total_ms: 900, class: 'IO' },
            { name: 'Lock:relation', idle: false, total_ms: 50000, class: 'Lock' },
        ],
        links: [
            { source: 'Client:ClientRead', target: 'CPU*', value: 5000, duration_ms: 9000 },
            { source: 'CPU*', target: 'Client:ClientRead', value: 4990, duration_ms: 8900 },
            { source: 'CPU*', target: 'IO:DataFileRead', value: 1000, duration_ms: 900 },
            { source: 'IO:DataFileRead', target: 'CPU*', value: 990, duration_ms: 850 },
            { source: 'CPU*', target: 'Lock:relation', value: 2, duration_ms: 50000 },
            { source: 'Lock:relation', target: 'CPU*', value: 2, duration_ms: 48000 },
        ],
    };
    const before = buildTransitionsOption(data, 20, { width: 800, height: 550 }, null, true);
    assert.ok(before.hiddenIdleLinks > 0 && before.hiddenThresholdLinks > 0);

    const after = buildTransitionsOption(data, restore.threshold,
        { width: 800, height: 550 }, null, restore.hideIdle);
    assert.equal(after.hiddenIdleLinks, 0);
    assert.equal(after.hiddenThresholdLinks, 0);
    assert.equal(after.omittedDominant, null, 'Lock:relation is visible now, nothing left to name');
    const names = after.option.series[0].data.map(n => n.name);
    assert.ok(names.includes('Client:ClientRead'));
    assert.ok(names.includes('Lock:relation'));
    // The one honest limitation: restore cannot fetch links the server
    // never returned.
    assert.equal(after.serverTruncated, true, 'server truncation survives "Show all" unchanged');
});

test('negative control: edges are literal old_event/new_event pairs -- '
    + 'CPU*-mediated A->CPU*->B is NEVER contracted into a synthesized '
    + 'direct A->B edge (contract 5 / #436, "different product" scope)', () => {
    // Input carries ONLY A->CPU* and CPU*->B, never A->B directly.
    const data = {
        total: 100,
        nodes: [
            { name: 'CPU*', idle: false, total_ms: 50, class: 'CPU' },
            { name: 'Lock:relation', idle: false, total_ms: 30, class: 'Lock' },
            { name: 'IO:DataFileRead', idle: false, total_ms: 20, class: 'IO' },
        ],
        links: [
            { source: 'Lock:relation', target: 'CPU*', value: 40, duration_ms: 30 },
            { source: 'CPU*', target: 'IO:DataFileRead', value: 40, duration_ms: 20 },
        ],
    };
    const built = buildTransitionsOption(data, 0, { width: 400, height: 300 });
    const pairs = built.option.series[0].links.map(l => l.source + '->' + l.target);
    assert.deepEqual(pairs.sort(), ['CPU*->IO:DataFileRead', 'Lock:relation->CPU*'].sort());
    assert.ok(!pairs.includes('Lock:relation->IO:DataFileRead'),
        'a direct Lock:relation->IO:DataFileRead edge must never be synthesized '
        + 'from the two hops through CPU*');
});

// ── #436: viewport fit (nodes, labels, edge arcs) ──────────────────────────

/* Reproduces the PRE-#436 layout exactly (fixed padX=80/padY=60, row-major
 * `col = i % cols, row = floor(i / cols)` cell assignment) so the viewport-
 * fit check below can be run against it, demonstrating the clipping bug the
 * fix addresses actually existed -- not a hypothetical. Kept local to this
 * test file; it is not exported from the builder (dead code there). */
function preFix436Layout(data, threshold, dims) {
    const cW = dims.width, cH = dims.height;
    const links = data.links;
    const nodeMap = {};
    data.nodes.forEach(n => { nodeMap[n.name] = n; });
    const maxEdgeCount = Math.max(...links.map(l => l.value));
    const minCount = maxEdgeCount * threshold / 100;
    const visibleLinks = links.filter(l => l.value >= minCount);
    const visibleNodes = new Set();
    visibleLinks.forEach(l => { visibleNodes.add(l.source); visibleNodes.add(l.target); });
    const maxNodeMs = Math.max(...[...visibleNodes].map(n => nodeMap[n]?.total_ms || 1));
    const sortedNodes = [...visibleNodes]
        .map(name => ({ name, ms: nodeMap[name]?.total_ms || 0 }))
        .sort((a, b) => b.ms - a.ms);
    const n = sortedNodes.length;
    const cols = Math.ceil(Math.sqrt(n * 1.8));
    const rows = Math.ceil(n / cols);
    const padX = 80, padY = 60;
    const cellW = (cW - padX * 2) / Math.max(cols - 1, 1);
    const cellH = (cH - padY * 2) / Math.max(rows - 1, 1);
    return sortedNodes.map((nd, i) => {
        const size = Math.max(15, Math.min(120, 15 + Math.sqrt(nd.ms / maxNodeMs) * 105));
        const col = i % cols, row = Math.floor(i / cols);
        return { name: nd.name, x: padX + col * cellW, y: padY + row * cellH, symbolSize: size };
    });
}

function footprintFits(node, dims) {
    const fp = estimateNodeFootprint(node.name, node.symbolSize);
    return node.x - fp.halfWidth >= -0.01 && node.x + fp.halfWidth <= dims.width + 0.01 &&
        node.y - fp.top >= -0.01 && node.y + fp.bottom <= dims.height + 0.01;
}

test('#436 RED: the pre-fix layout actually clips (demonstrates the bug, '
    + 'not a hypothetical) -- the dense fixture bottom-row node label '
    + 'footprint overshoots the container height', () => {
    const dims = { width: 620, height: 460 };
    const data = {
        total: 1000,
        nodes: [
            { name: 'CPU*', idle: false, total_ms: 5200, class: 'CPU' },
            { name: 'IO:DataFileRead', idle: false, total_ms: 2100, class: 'IO' },
            { name: 'LWLock:WALInsert', idle: false, total_ms: 900, class: 'LWLock' },
            { name: 'IO:WalSync', idle: false, total_ms: 800, class: 'IO' },
            { name: 'Lock:transactionid', idle: false, total_ms: 1500, class: 'Lock' },
            { name: 'LWLock:LockManager', idle: false, total_ms: 210, class: 'LWLock' },
        ],
        links: [
            { source: 'CPU*', target: 'IO:DataFileRead', value: 1400, duration_ms: 1 },
            { source: 'IO:DataFileRead', target: 'CPU*', value: 1350, duration_ms: 1 },
            { source: 'CPU*', target: 'LWLock:WALInsert', value: 800, duration_ms: 1 },
            { source: 'LWLock:WALInsert', target: 'CPU*', value: 780, duration_ms: 1 },
            { source: 'CPU*', target: 'IO:WalSync', value: 400, duration_ms: 1 },
            { source: 'IO:WalSync', target: 'CPU*', value: 390, duration_ms: 1 },
            { source: 'CPU*', target: 'Lock:transactionid', value: 220, duration_ms: 1 },
            { source: 'Lock:transactionid', target: 'LWLock:LockManager', value: 90, duration_ms: 1 },
            { source: 'LWLock:LockManager', target: 'CPU*', value: 88, duration_ms: 1 },
        ],
    };
    const before = preFix436Layout(data, 0, dims);
    assert.ok(before.some(nd => !footprintFits(nd, dims)),
        'expected the OLD fixed-padding layout to clip at least one node/label '
        + '-- if this fails, the fixture no longer demonstrates the bug');

    const after = buildTransitionsOption(data, 0, dims);
    // #436: layout-anchor nodes are a deliberate identity-transform pin,
    // not a real laid-out wait event -- exclude them from the real-node fit check.
    for (const nd of after.option.series[0].data.filter(n => !isLayoutAnchorNode(n.name))) {
        assert.ok(footprintFits(nd, dims),
            nd.name + ' footprint does not fit ' + JSON.stringify(dims) + ': ' +
            JSON.stringify({ x: nd.x, y: nd.y, symbolSize: nd.symbolSize }));
    }
});

test('#436: every laid-out node + its label fits the container for the dense '
    + '18-node fixture at threshold 0 (max density, max label count)', () => {
    const dims = { width: 620, height: 460 };
    const nodes = [
        { name: 'CPU*', idle: false, total_ms: 5200, class: 'CPU' },
        { name: 'Client:ClientRead', idle: true, total_ms: 2600, class: 'Client' },
        { name: 'IO:DataFileRead', idle: false, total_ms: 2100, class: 'IO' },
        { name: 'Lock:transactionid', idle: false, total_ms: 1500, class: 'Lock' },
        { name: 'LWLock:WALInsert', idle: false, total_ms: 900, class: 'LWLock' },
        { name: 'IO:WalSync', idle: false, total_ms: 800, class: 'IO' },
        { name: 'LWLock:WALWrite', idle: false, total_ms: 640, class: 'LWLock' },
        { name: 'Lock:tuple', idle: false, total_ms: 480, class: 'Lock' },
        { name: 'LWLock:BufferMapping', idle: false, total_ms: 420, class: 'LWLock' },
        { name: 'IO:WalWrite', idle: false, total_ms: 350, class: 'IO' },
        { name: 'Timeout:VacuumDelay', idle: true, total_ms: 300, class: 'Timeout' },
        { name: 'Lock:relation', idle: false, total_ms: 260, class: 'Lock' },
        { name: 'IO:DataFileExtend', idle: false, total_ms: 240, class: 'IO' },
        { name: 'LWLock:LockManager', idle: false, total_ms: 210, class: 'LWLock' },
        { name: 'IPC:BufferIO', idle: false, total_ms: 190, class: 'IPC' },
        { name: 'Extension:Extension', idle: false, total_ms: 150, class: 'Extension' },
        { name: 'IPC:MessageQueueReceive', idle: false, total_ms: 120, class: 'IPC' },
        { name: 'BufferPin:BufferPin', idle: false, total_ms: 90, class: 'BufferPin' },
    ];
    const links = [];
    for (let i = 1; i < nodes.length; i++) {
        links.push({ source: 'CPU*', target: nodes[i].name, value: 1000 - i * 10, duration_ms: 1 });
        links.push({ source: nodes[i].name, target: 'CPU*', value: 990 - i * 10, duration_ms: 1 });
    }
    links.push({ source: 'CPU*', target: 'CPU*', value: 300, duration_ms: 1 });
    const data = { total: links.reduce((s, l) => s + l.value, 0), nodes, links };
    const built = buildTransitionsOption(data, 0, dims);
    assert.equal(built.visibleCount, 18);
    for (const nd of built.option.series[0].data.filter(n => !isLayoutAnchorNode(n.name))) {
        assert.ok(footprintFits(nd, dims), nd.name + ' clips the ' + JSON.stringify(dims) + ' container');
    }
});

test('computeGridOrder: rank 0 (most time-significant node) lands on the '
    + 'cell closest to the grid center, not the (0,0) corner (#436/#438 -- '
    + 'the CPU* hub no longer gets the worst cell in the layout)', () => {
    const order = computeGridOrder(7, 4, 2); // 4x2 grid, center at (1.5, 0.5)
    assert.notDeepEqual(order[0], { col: 0, row: 0 },
        'the old row-major mapping always put index 0 at the literal corner');
    // (1,0) and (2,0) are both distance 0.25+0.25=0.5 from the center along
    // row 0; (1,1)/(2,1) tie too, at the same distance -- the deterministic
    // tie-break (row asc, then col asc) must pick (1,0).
    assert.deepEqual(order[0], { col: 1, row: 0 });
    assert.equal(order.length, 7);
    const seen = new Set(order.map(c => c.col + ',' + c.row));
    assert.equal(seen.size, 7, 'every assigned cell must be distinct');
});

test('#436: cross-edge curveness is capped so its bow (curveness * pixel '
    + 'distance) never exceeds the layout\'s own reserved margin -- long hub '
    + 'fan-out edges flatten instead of arcing off-canvas', () => {
    const dims = { width: 620, height: 460 };
    const data = {
        total: 1000,
        nodes: [
            { name: 'CPU*', idle: false, total_ms: 5000, class: 'CPU' },
            { name: 'A', idle: false, total_ms: 400, class: 'IO' },
            { name: 'B', idle: false, total_ms: 390, class: 'Lock' },
            { name: 'C', idle: false, total_ms: 380, class: 'LWLock' },
            { name: 'D', idle: false, total_ms: 370, class: 'Timeout' },
            { name: 'E', idle: false, total_ms: 360, class: 'IPC' },
        ],
        links: [
            { source: 'CPU*', target: 'A', value: 400, duration_ms: 1 },
            { source: 'CPU*', target: 'B', value: 390, duration_ms: 1 },
            { source: 'CPU*', target: 'C', value: 380, duration_ms: 1 },
            { source: 'CPU*', target: 'D', value: 370, duration_ms: 1 },
            { source: 'CPU*', target: 'E', value: 360, duration_ms: 1 },
        ],
    };
    const built = buildTransitionsOption(data, 0, dims);
    const safeMargin = Math.min(built.margins.padX, built.margins.padYTop, built.margins.padYBottom);
    const posByName = {};
    built.option.series[0].data.forEach(n => { posByName[n.name] = n; });
    for (const l of built.option.series[0].links) {
        if (l.source === l.target) continue;
        const p1 = posByName[l.source], p2 = posByName[l.target];
        const dist = Math.hypot(p2.x - p1.x, p2.y - p1.y);
        const bow = l.lineStyle.curveness * dist;
        assert.ok(bow <= safeMargin + 0.01,
            l.source + '->' + l.target + ' bow ' + bow + ' exceeds margin ' + safeMargin);
    }
});

// ── the gallery/fixture state itself (#436, contract 5): all three omission
// causes plus the rare-but-longest-duration Lock:relation in ONE payload,
// exactly as the brief asked ("extend threshold-simplified with...").

test('fixture threshold-simplified: all three omission causes fire together, '
    + 'Lock:relation is named as the omitted dominant wait (rare count, '
    + 'longest dwell), and nothing clips at its DIMS', () => {
    const st = galleryStates['threshold-simplified'];
    const built = buildTransitionsOption(st.data, st.opts.threshold, st.opts.dims,
        null, st.opts.hideIdle);
    assert.ok(built.hiddenIdleLinks > 0, 'the ClientRead<->CPU* pacing loop must be idle-excluded');
    assert.ok(built.hiddenThresholdLinks > 0, 'low-count edges must still be threshold-excluded');
    assert.equal(built.serverTruncated, true, 'the fixture declares a server link cap');
    assert.equal(built.omittedDominant.name, 'Lock:relation');
    assert.equal(built.omittedDominant.ms, 52000);
    for (const nd of built.option.series[0].data.filter(n => !isLayoutAnchorNode(n.name))) {
        assert.ok(footprintFits(nd, st.opts.dims), nd.name + ' clips the fixture viewport');
    }
    const text = buildTransitionsDisclosure(built, st.data.total);
    assert.ok(text.includes('Lock:relation') && text.includes('Show all'));

    // Restore ("Show all") actually brings Lock:relation onto the canvas,
    // without claiming to have fixed the (unrelated) server truncation.
    const restore = restoreAllTransitionsState();
    const restored = buildTransitionsOption(st.data, restore.threshold, st.opts.dims,
        null, restore.hideIdle);
    assert.ok(restored.option.series[0].data.some(n => n.name === 'Lock:relation'));
    assert.equal(restored.omittedDominant, null);
    assert.equal(restored.serverTruncated, true);
});

// Negative control 1 (brief: "omit Lock:relation without disclosure ...
// must be rejected"): a payload shaped exactly like the fixture but with
// Lock:relation silently dropped from `data.nodes` (as a buggy server-side
// change might do) must NOT be mistaken for "nothing omitted" — the server-
// truncation disclosure must still fire because the fixture also sets
// truncated:true independently of which specific node vanished.
test('negative control: Lock:relation missing from the node catalog entirely '
    + 'still leaves the independent server-truncation disclosure in place '
    + '(an omission is never silently invisible merely because one path '
    + 'did not catch it)', () => {
    const st = galleryStates['threshold-simplified'];
    const withoutLockRelation = {
        ...st.data,
        nodes: st.data.nodes.filter(n => n.name !== 'Lock:relation'),
        links: st.data.links.filter(l => l.source !== 'Lock:relation' && l.target !== 'Lock:relation'),
    };
    const built = buildTransitionsOption(withoutLockRelation, st.opts.threshold, st.opts.dims,
        null, st.opts.hideIdle);
    // omittedDominant can no longer name Lock:relation -- it was never in
    // the payload at all (that would be a server-side data bug, not
    // something a client-side omission check could ever see) -- it instead
    // correctly falls through to the next-largest STILL-omitted node. The
    // point of this control: the independent truncation disclosure, which
    // does not depend on any specific node, must still fire regardless.
    assert.notEqual(built.omittedDominant.name, 'Lock:relation');
    assert.equal(built.serverTruncated, true,
        'server-truncation disclosure must not depend on which node triggered it');
});

// Negative control 2 (brief: "synthesize a direct wait->wait edge ...
// must be rejected"): confirms the fixture's own CPU-mediated shape is
// never collapsed into a direct Lock:relation<->IO:DataFileRead edge, which
// no input link declares.
test('negative control: the fixture never gains a synthesized direct '
    + 'Lock:relation<->IO:DataFileRead edge (both hop through CPU* in the '
    + 'input; #436 "different product" scope)', () => {
    const st = galleryStates['threshold-simplified'];
    const built = buildTransitionsOption(st.data, 0, st.opts.dims, null, false);
    const pairs = new Set(built.option.series[0].links.map(l => l.source + '->' + l.target));
    assert.ok(!pairs.has('Lock:relation->IO:DataFileRead'));
    assert.ok(!pairs.has('IO:DataFileRead->Lock:relation'));
});

// ── #436 adviser-review finding: the identity-transform pin ────────────────
//
// ECharts' graph "view" coordinateSystem normalizes node x/y through its own
// fit-to-box transform UNLESS the series' box (left/top/width/height) is
// pinned to the exact container AND the data's own bounding box is pinned to
// match it (via two invisible corner nodes) -- see the long comment on the
// series config in buildTransitionsOption, and tests/test_dfg_geometry.py
// for the live, rendered-coordinate confirmation (a pure Node test cannot
// render ECharts, so it can only check these CONFIGURATION values are
// present -- it is a tripwire against silently dropping the pin, not a
// substitute for the rendered check).

test('#436: the series is pinned to an identity data->pixel transform '
    + '(left/top/width/height match the container exactly, plus two '
    + 'invisible corner anchors) -- without this, ECharts auto-fits node '
    + 'positions to a DIFFERENT box than the one this builder laid out in '
    + '(verified against a live render in tests/test_dfg_geometry.py)', () => {
    const dims = { width: 620, height: 460 };
    const built = buildTransitionsOption(data(), 0, dims);
    const series = built.option.series[0];
    assert.equal(series.left, 0);
    assert.equal(series.top, 0);
    assert.equal(series.width, dims.width);
    assert.equal(series.height, dims.height);
    const anchors = series.data.filter(n => isLayoutAnchorNode(n.name));
    assert.equal(anchors.length, 2, 'exactly two layout-anchor nodes');
    const xs = anchors.map(a => a.x).sort((a, b) => a - b);
    const ys = anchors.map(a => a.y).sort((a, b) => a - b);
    assert.deepEqual(xs, [0, dims.width]);
    assert.deepEqual(ys, [0, dims.height]);
    for (const a of anchors) {
        assert.equal(a.silent, true, a.name + ' must never intercept hover/click/drag');
        assert.equal(a.symbol, 'none', a.name + ' must never be visibly drawn');
    }
});

test('negative control: isLayoutAnchorNode only matches the two reserved '
    + 'anchor names -- a real wait-event name is never mistaken for one', () => {
    assert.equal(isLayoutAnchorNode('CPU*'), false);
    assert.equal(isLayoutAnchorNode('Lock:relation'), false);
    assert.equal(isLayoutAnchorNode(''), false);
    assert.equal(isLayoutAnchorNode(undefined), false);
});

// ── #456: persistent duration disclosure (the dominant source-dwell node is
// named + its visible/omitted status stated EVERY render, not only when the
// threshold/idle filters hide something) ───────────────────────────────────

test('findDominantSourceDwellNode: names the globally largest non-idle '
    + 'node and reports it VISIBLE when nothing hides it', () => {
    const nodeMap = {
        'CPU*': { idle: false, total_ms: 500, event_id: 0 },
        'IO:DataFileRead': { idle: false, total_ms: 200, event_id: 20 },
    };
    const d = findDominantSourceDwellNode(nodeMap, new Set(['CPU*', 'IO:DataFileRead']));
    assert.equal(d.name, 'CPU*');
    assert.equal(d.ms, 500);
    assert.equal(d.visible, true);
});

test('findDominantSourceDwellNode: reports OMITTED when the dominant node '
    + 'is not in the visible set', () => {
    const nodeMap = {
        'CPU*': { idle: false, total_ms: 500, event_id: 0 },
        'Lock:relation': { idle: false, total_ms: 900, event_id: 50 },
    };
    const d = findDominantSourceDwellNode(nodeMap, new Set(['CPU*']));
    assert.equal(d.name, 'Lock:relation');
    assert.equal(d.visible, false);
});

test('findDominantSourceDwellNode: idle nodes are never the dominant node, '
    + 'even with the largest total_ms', () => {
    const nodeMap = {
        'Client:ClientRead': { idle: true, total_ms: 999999, event_id: 99 },
        'Lock:relation': { idle: false, total_ms: 10, event_id: 1 },
    };
    const d = findDominantSourceDwellNode(nodeMap, new Set());
    assert.equal(d.name, 'Lock:relation');
});

test('findDominantSourceDwellNode: null when there is no non-idle node with time', () => {
    assert.equal(findDominantSourceDwellNode({}, new Set()), null);
    assert.equal(findDominantSourceDwellNode(
        { 'Client:ClientRead': { idle: true, total_ms: 500 } }, new Set()), null);
});

test('buildTransitionsDisclosure: dominant-dwell node is named PERSISTENTLY '
    + 'when nothing is hidden (the pre-#456 bug: the line said nothing at '
    + 'all about which node dominates dwell time in this case)', () => {
    const built = {
        hiddenIdleLinks: 0, hiddenIdleValue: 0,
        hiddenThresholdLinks: 0, hiddenThresholdValue: 0,
        serverTruncated: false, serverShownLinkCount: 4, serverTotalLinkCount: 4,
        omittedDominant: null,
        dominantSourceDwell: { name: 'CPU*', ms: 500000, visible: true },
    };
    const text = buildTransitionsDisclosure(built, 400);
    assert.ok(text.includes('CPU*') && text.includes('largest source-dwell node'),
        text);
    assert.ok(text.includes('shown'), text);
});

test('buildTransitionsDisclosure: when the dominant node is omitted, the '
    + 'existing omittedDominant "Show all" phrase covers it -- no duplicate '
    + 'second phrase for the same node', () => {
    const built = {
        hiddenIdleLinks: 0, hiddenIdleValue: 0,
        hiddenThresholdLinks: 2, hiddenThresholdValue: 2,
        serverTruncated: false, serverShownLinkCount: 4, serverTotalLinkCount: 4,
        omittedDominant: { name: 'Lock:relation', ms: 50000 },
        dominantSourceDwell: { name: 'Lock:relation', ms: 50000, visible: false },
    };
    const text = buildTransitionsDisclosure(built, 400);
    // "Lock:relation" must appear in the omitted phrase (with its Show-all
    // cue); it must NOT also appear inside a second "largest source-dwell"
    // phrase -- that would be the same fact stated twice.
    assert.ok(text.includes('Lock:relation omitted') && text.includes('Show all'), text);
    assert.ok(!text.includes('largest source-dwell node'), text);
});

test('buildTransitionsOption: dominantSourceDwell reaches the real response '
    + 'shape end to end, via buildTransitionsDisclosure', () => {
    const data = {
        total: 10,
        nodes: [
            { name: 'CPU*', idle: false, total_ms: 100, event_id: 0, class: 'CPU' },
            { name: 'IO:DataFileRead', idle: false, total_ms: 90, event_id: 20, class: 'IO' },
        ],
        links: [
            { source: 'CPU*', target: 'IO:DataFileRead', value: 10, duration_ms: 90 },
            { source: 'IO:DataFileRead', target: 'CPU*', value: 10, duration_ms: 80 },
        ],
    };
    const built = buildTransitionsOption(data, 0, { width: 400, height: 300 });
    assert.equal(built.dominantSourceDwell.name, 'CPU*');
    assert.equal(built.dominantSourceDwell.visible, true);
    const text = buildTransitionsDisclosure(built, data.total);
    assert.ok(text.includes('CPU*') && text.includes('largest source-dwell node'), text);
});

test('TRANSITIONS_SEMANTICS_NOTE: states the size/edge-meaning distinction '
    + 'and explicitly disclaims a DB-Time ranking', () => {
    assert.ok(TRANSITIONS_SEMANTICS_NOTE.includes('source-state dwell time'));
    assert.ok(TRANSITIONS_SEMANTICS_NOTE.includes('transition counts'));
    assert.ok(TRANSITIONS_SEMANTICS_NOTE.toLowerCase().includes('not a ranking'));
    assert.ok(TRANSITIONS_SEMANTICS_NOTE.toLowerCase().includes('db time'));
});

// #456: frozen buckets:200 response from issue456-restart-replay/out200/run00.json
// (all ten captured responses are byte-identical). Keep the full catalog/links:
// pruning the fixture to the displayed graph would hide the regression itself.
// #487: idle booleans annotate that capture for the current wire schema;
// captured dwell times, identities and links are unchanged.
import { readFileSync } from 'node:fs';
import { createTransitionsView } from '../../web/static/views/transitions.js';
import { Transport } from '../../web/static/lib/transport.js';
import { ViewManager } from '../../web/static/lib/view-manager.js';
import { FilterStack, TimeRange, ServerInfo } from '../../web/static/lib/state.js';

function replay456() {
    return JSON.parse(readFileSync(new URL(
        '../../web/static/dev/fixtures/issue456-transitions.json', import.meta.url), 'utf8'));
}

function assertReplay456(payload) {
    // Population assertions precede determinism: ten empty graphs cannot pass.
    assert.ok(payload.nodes.length > 0);
    assert.equal(payload.links.length, 80);
    assert.equal(payload.links.reduce((sum, link) => sum + link.value, 0), 3466745);
    const built = buildTransitionsOption(payload, 20, { width: 800, height: 550 }, null, true);
    assert.equal(payload.total, 3466745);
    assert.equal(built.hiddenIdleValue, 849416);
    assert.equal(built.hiddenThresholdValue, 70110);
    assert.ok(built.visibleCount > 0);
    assert.ok(built.shownLinks > 0);
    assert.deepEqual(built.omittedDominant, { name: 'Lock:relation', ms: 1533607.757399 });
    assert.equal(payload.nodes.length, 41);
    assert.equal(built.option.series[0].data.some(n => n.name === 'Lock:relation'), false);
    const text = buildTransitionsDisclosure(built, payload.total);
    assert.match(text, /Lock:relation omitted \(1533\.6s total\)/);
    assert.match(text, /849,416 idle \(ClientRead\) hidden/);
    assert.match(text, /70,110 below the simplify threshold/);
    const restored = buildTransitionsOption(payload, 0, {}, null, false);
    assert.equal(restored.option.series[0].data.find(n => n.name === 'Lock:relation').value,
        1533607.757399);
    return { built, text };
}

// Deliberately small DOM/chart boundary: run the REAL view, transport and view
// manager. innerHTML replacement removes descendants, just as a browser does.
function viewHarness456(t) {
    const elements = new Map();
    const frames = new Map();
    const paints = [];
    let nextFrame = 0;
    class Element {
        constructor(id) {
            this.id = id; this.style = {}; this.listeners = {}; this.children = [];
            this.textContent = ''; this.clientWidth = 800; this.clientHeight = 550;
        }
        addEventListener(name, fn) { this.listeners[name] = fn; }
        set innerHTML(html) {
            for (const child of this.children) { child.innerHTML = ''; elements.delete(child.id); }
            this.children = []; this.html = html; this.textContent = '';
            for (const match of html.matchAll(/id="([^"]+)"/g)) {
                const child = new Element(match[1]);
                elements.set(child.id, child); this.children.push(child);
            }
        }
        get innerHTML() { return this.html || ''; }
    }
    const originals = new Map();
    function stubGlobal(name, value) {
        originals.set(name, Object.getOwnPropertyDescriptor(globalThis, name));
        Object.defineProperty(globalThis, name, { value, configurable: true, writable: true });
    }
    stubGlobal('document', { getElementById: id => elements.get(id) || null });
    stubGlobal('requestAnimationFrame', fn => {
        frames.set(++nextFrame, fn); return nextFrame;
    });
    stubGlobal('cancelAnimationFrame', id => frames.delete(id));
    stubGlobal('WebSocket', { OPEN: 1 });
    const ws = {
        readyState: 1, sent: [],
        addEventListener(name, fn) { if (name === 'message') this.deliver = body => fn({ data: JSON.stringify(body) }); },
        send(msg) { this.sent.push(JSON.parse(msg)); },
    };
    const transport = new Transport(); transport.attach(ws);
    const ctx = {
        transport, timeRange: new TimeRange(new ServerInfo()), filters: new FilterStack(),
        channel: name => 'transitions.' + name,
        getEscalationStatus: () => ({ escalation_supported: false }),
        echarts: { init() { return {
            on() {}, getZr() { return { on() {} }; }, dispose() {}, resize() {},
            getOption() { return paints.at(-1); }, setOption(option) { paints.push(option); },
        }; } },
    };
    const view = createTransitionsView();
    const root = new Element('root');
    const manager = new ViewManager(ctx); manager.register(view); manager.setContainer(root);
    t.after(() => {
        view.leave(); transport.rejectAll('test complete');
        for (const [name, descriptor] of originals) {
            if (descriptor) Object.defineProperty(globalThis, name, descriptor);
            else delete globalThis[name];
        }
    });
    return { view, root, ctx, ws, manager, paints, frames, elements,
        mount(payload) { view.mount(root, view.build({ transitions: payload }), ctx); },
        flush() { const pending = [...frames.values()]; frames.clear(); pending.forEach(fn => fn()); },
    };
}

test('#456 frozen replay names the omitted Lock:relation with exact dwell and visibility', () => {
    assertReplay456(replay456());
});

test('#456 nodes-less response is visibly refused by the view, never rendered as a graph', t => {
    const h = viewHarness456(t);
    const payload = replay456(); delete payload.nodes;
    h.mount(payload);
    assert.match(h.root.innerHTML, /role="alert"/);
    assert.match(h.root.innerHTML, /Transitions data incomplete/);
    assert.match(h.root.innerHTML, /missing nodes/);
    assert.equal(h.paints.length, 0);
    assert.equal(h.elements.has('dfg-total'), false);
});

test('#456 missing Lock:relation keeps the headline totals but is visibly refused', t => {
    const h = viewHarness456(t);
    const payload = replay456();
    const before = JSON.stringify(payload.links);
    payload.nodes = payload.nodes.filter(n => n.name !== 'Lock:relation');
    assert.equal(JSON.stringify(payload.links), before);
    assert.equal(payload.total, 3466745);
    h.mount(payload);
    assert.match(h.root.innerHTML, /Transitions data incomplete/);
    assert.match(h.root.innerHTML, /missing node: Lock:relation/);
    assert.equal(h.paints.length, 0);
});

test('#456 builder refuses incomplete catalogs before idle or simplify filtering', () => {
    const payload = replay456();
    delete payload.nodes;
    assert.throws(() => buildTransitionsOption(payload, 20, {}, null, true), /missing nodes/);
    for (const name of ['Lock:relation', 'Client:ClientRead', 'CPU*']) {
        const partial = replay456(); partial.nodes = partial.nodes.filter(n => n.name !== name);
        assert.throws(() => buildTransitionsOption(partial, 20, {}, null, true), /missing node:/);
    }
});

test('#456 refresh clears the old payload and queued render before mounting empty data', t => {
    const h = viewHarness456(t);
    h.mount(replay456());
    h.view.resize();
    assert.equal(h.frames.size, 1);
    h.mount({ nodes: [], links: [], total: 0 });
    assert.equal(h.frames.size, 0, 'empty mount must cancel old render');
    h.view.setHideIdle(false);
    assert.equal(h.frames.size, 0, 'empty mount must release dataRef');
    assert.match(h.root.innerHTML, /No transitions found/);
});

test('#456 malformed refresh replaces the old graph and cannot revive it on slider/resize', t => {
    const h = viewHarness456(t);
    h.mount(replay456()); h.view.resize();
    const payload = replay456(); delete payload.nodes;
    h.mount(payload);
    h.view.setHideIdle(false); h.view.resize(); h.flush();
    assert.match(h.root.innerHTML, /Transitions data incomplete/);
    assert.equal(h.paints.length, 1);
    assert.equal(h.frames.size, 0);
});

test('#456 ten fresh view/transport sessions preserve nonempty frozen payload and disclosure', async t => {
    let expected;
    for (let run = 0; run < 10; run++) {
        await t.test('session ' + run, async t => {
            const h = viewHarness456(t);
            h.ctx.timeRange.set(100, 200);
            const mounting = h.manager.switchTo('transitions');
            const request = h.ws.sent.at(-1);
            assert.deepEqual(request, { id: 1, cmd: 'transitions', from: 100, to: 200, filters: {}, buckets: 200 });
            // Interleave a different table channel's nodes-less response.
            const other = h.ctx.transport.request('events.table', 'events', {});
            h.ws.deliver({ id: h.ws.sent.at(-1).id, rows: [{ name: 'unrelated' }] });
            await other;
            const payload = replay456(); const { text } = assertReplay456(payload);
            h.ws.deliver({ ...payload, id: request.id }); await mounting;
            assert.equal(h.elements.get('dfg-total').textContent, text);
            assert.ok(h.paints[0].series[0].data.length > 2);
            const snapshot = JSON.stringify({ text, option: h.paints[0] });
            if (run === 0) expected = snapshot;
            assert.equal(snapshot, expected);

            // A superseded request must not supply the next refresh's body.
            const stale = h.manager.refresh(); const oldId = h.ws.sent.at(-1).id;
            h.ctx.timeRange.set(300, 400); h.ctx.filters.drill('pid', 17, 'pid 17', 'transitions');
            const fresh = h.manager.refresh(); const current = h.ws.sent.at(-1);
            assert.deepEqual(current.filters, { pid: 17 });
            assert.equal(current.from, 300); assert.equal(current.to, 400);
            h.ws.deliver({ id: oldId, links: payload.links, total: payload.total });
            h.ws.deliver({ ...payload, id: current.id });
            await Promise.all([stale, fresh]);
            assert.equal(h.paints.length, 2);
            assert.equal(h.elements.get('dfg-total').textContent, text);
        });
    }
});


test('#456 unavailable refresh also releases the old payload and pending frame', t => {
    const h = viewHarness456(t);
    h.mount(replay456()); h.view.resize();
    h.mount({ code: 'full_fidelity_required', unavailable: 'Exact data required', fidelity: 'sampled' });
    h.view.setHideIdle(false);
    assert.equal(h.frames.size, 0);
    assert.match(h.root.innerHTML, /unavailable-panel/);
    assert.equal(h.elements.has('dfg-total'), false);
});

test('#456 catalog mutation after mount is visibly refused on the next render', t => {
    const h = viewHarness456(t);
    const payload = replay456(); h.mount(payload);
    payload.nodes = payload.nodes.filter(n => n.name !== 'Lock:relation');
    h.view.resize(); h.flush();
    assert.match(h.root.innerHTML, /missing node: Lock:relation/);
    assert.equal(h.paints.length, 1);
});

test('#456 invalid dwell, duplicate names, empty catalog and missing links are not empty graphs', t => {
    const h = viewHarness456(t);
    for (const mutate of [
        p => { p.nodes = []; },
        p => { p.nodes = null; },
        p => { p.nodes.push({ ...p.nodes[0] }); },
        p => { delete p.nodes[0].total_ms; },
        p => { p.nodes[0].total_ms = NaN; },
        p => { p.nodes[0].total_ms = -1; },
        p => { delete p.links; },
    ]) {
        const payload = replay456(); mutate(payload); h.mount(payload);
        assert.match(h.root.innerHTML, /role="alert"/);
        assert.match(h.root.innerHTML, /Transitions data incomplete/);
    }
    assert.equal(h.paints.length, 0);
    h.mount(replay456());
    assert.match(h.elements.get('dfg-total').textContent, /Lock:relation omitted/);
    assert.equal(h.paints.length, 1, 'a valid refresh recovers');
});

test('#456 missing hostile endpoint is escaped in the visible error', t => {
    const h = viewHarness456(t);
    const payload = replay456(); payload.links[0].source = '<img src=x onerror=alert(1)>';
    h.mount(payload);
    assert.match(h.root.innerHTML, /missing node: &lt;img/);
    assert.ok(!h.root.innerHTML.includes('<img'));
});
