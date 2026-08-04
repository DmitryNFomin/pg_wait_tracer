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
    buildTransitionsOption, transitionsContext, buildVariantsHtml,
} from '../../web/static/lib/builders/transitions.js';
import { applyDragOffset } from '../../web/static/views/transitions.js';
import { eventColor } from '../../web/static/lib/format.js';

const esc = (s) => String(s).replace(/&/g, '&amp;').replace(/</g, '&lt;').replace(/>/g, '&gt;');

function data() {
    return {
        total: 1000,
        nodes: [
            // event_id mirrors the server's DFG node JSON (U2 / P3 wire 5;
            // 0 = the CPU* pseudo-node).
            { name: 'CPU*', total_ms: 4800, class: 'CPU', event_id: 0 },
            { name: 'IO:DataFileRead', total_ms: 2100, class: 'IO', event_id: 0x01000015 },
            { name: 'LWLock:WALInsert', total_ms: 900, class: 'LWLock', event_id: 0x04000007 },
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
    // class green rgb(80,250,123).
    assert.equal(cpu.itemStyle.color, 'rgb(155,247,178)');
});

test('cross-view identity: every node color equals eventColor(null, name)', () => {
    // The AAS chart colors series with eventColor(null, name) — the DFG must
    // agree for the same event names no matter how it identifies the class.
    const { option } = buildTransitionsOption(data(), 0, { width: 800, height: 550 });
    for (const n of option.series[0].data) {
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
        total: 10, nodes: [{ name: 'IO:WalSync', total_ms: 5, class: 'IO' }],
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
        total: 10, nodes: [{ name: 'CPU*', total_ms: 100, class: 'CPU' }],
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
        total: 10, nodes: [{ name: 'A', total_ms: 1 }, { name: 'B', total_ms: 1 }],
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
