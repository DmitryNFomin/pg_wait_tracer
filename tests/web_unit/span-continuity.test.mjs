import { test } from 'node:test';
import assert from 'node:assert/strict';
import { makeQueriesConfig } from '../../web/static/lib/builders/table-configs.js';
import { createWaterfallView } from '../../web/static/views/waterfall.js';

const span = { pid: 661380, measurement: 'measured', completion: 'closed',
    start_ns: '1000000000', end_ns: '10900000000', completed_duration_ms: 9900,
    observed_elapsed_ms: 9900 };
const render = s => makeQueriesConfig('exact').columns.map(c => c.format?.({
    query_id: '42', text: 'select 1', backend_activity: { span: s },
})).join(' ').replace(/<[^>]*>/g, ''); // text nodes only; titles are not visible copy

test('Queries measured closed: visible longest selection, PID, boundary scope', () => {
    assert.match(render(span), /Longest measured closed backend span: 9\.9s/);
    assert.match(render(span), /PID 661380/);
});
test('Queries in flight: observed elapsed even with a completed duration populated', () => {
    const html = render({ ...span, completion: 'in_flight', completed_duration_ms: 555000 });
    assert.match(html, /Longest observed in-flight backend span: 9\.9s so far/);
    assert.doesNotMatch(html, /555\.0|measured closed/);
});
test('Queries partial and unavailable remain explicit', () => {
    assert.match(render({ ...span, measurement: 'partial', completion: 'unknown' }), /Partial.*9\.9s observed/);
    assert.match(render(null), /Backend span unavailable: Not measured/);
});
test('Queries span occupies identity cell, never its own column', () => {
    const cols = makeQueriesConfig('exact').columns;
    assert.ok(!cols.some(c => c.key === 'backend_activity_span'));
    assert.match(cols.find(c => c.key === 'text').format({ text: 'select 1', backend_activity: { span } }), /Longest/);
});
test('Waterfall selects only the requested PID/query and requests containing-activity evidence', async () => {
    const requests = [];
    const view = createWaterfallView();
    const rows = [
        { pid: 999, query_id: '99', start_ns: '10000000000', end_ns: '10900000000', duration_ms: 900, n_events: 1 },
        { pid: 661380, query_id: '42', start_ns: '10800000000', end_ns: '10900000000', duration_ms: 100, n_events: 1 },
    ];
    const ctx = { timeRange: { from: 1e9, to: 11e9 }, filters: { snapshot: () => ({ pid: 661380, query_id: '42', event_id: 1 }) },
        channel: x => x, transport: { request: async (ch, cmd, params) => {
            requests.push({ cmd, ...params });
            if (cmd === 'executions') return { rows };
            if (cmd === 'execution_detail') return { backend_activity_span: span };
            return { fidelity: 'exact', truncated: false, events: [{ p: 661380, n: 'Lock:relation', s: '1000000000', d: '9800000000' }] };
        } } };
    const data = await view.requests(ctx);
    assert.equal(data.selected.pid, 661380);
    const evidence = requests.find(r => r.cmd === 'session_timeline');
    assert.ok(evidence, 'must fetch wait evidence');
    assert.deepEqual(evidence.filters, { pid: 661380 }, 'activity scope must not drop pre-query or other waits');
    assert.equal(String(evidence.from), span.start_ns);
    assert.equal(String(evidence.to), span.end_ns);
    assert.match(view.build(data).context, /Lock:relation/);
    assert.match(view.build(data).context, /before execution/);
});

import { waterfallContextHtml, containingActivityWindow, buildWaterfallOption } from '../../web/static/lib/builders/waterfall.js';

for (const [name, preferredPid, filters, expectedPid] of [
    ['prefers a span PID that executed this query', 1002, { query_id: '42' }, 1002],
    ['falls back when span PID has no execution of this query', 1001, { query_id: '42' }, 1000],
    ['keeps an explicit session filter authoritative', 1002, { query_id: '42', pid: 1000 }, 1000],
    ['does not fall back to another query or explicit session', 1002, { query_id: '42', pid: 1001 }, null],
]) {
    test('query pivot ' + name, async () => {
        const view = createWaterfallView();
        view.selectQuery({ pid: preferredPid });
        const rows = [
            { pid: 1001, query_id: '99', start_ns: '1', end_ns: '900', duration_ms: 900, n_events: 9 },
            { pid: 1000, query_id: '42', start_ns: '1', end_ns: '800', duration_ms: 800, n_events: 8 },
            { pid: 1002, query_id: '42', start_ns: '1', end_ns: '100', duration_ms: 100, n_events: 1 },
        ];
        const calls = [];
        const data = await view.requests({
            timeRange: { from: 0, to: 1000 }, filters: { snapshot: () => filters }, channel: x => x,
            transport: { request: async (ch, cmd, params) => {
                calls.push({ cmd, ...params });
                return cmd === 'executions' ? { rows } : {};
            } },
        });
        assert.deepEqual(calls[0].filters, filters, 'span preference must not become a server filter');
        assert.equal(data.selected?.pid ?? null, expectedPid);
        const detail = calls.find(c => c.cmd === 'execution_detail');
        if (expectedPid == null) assert.equal(detail, undefined);
        else assert.deepEqual(detail.filters, { pid: expectedPid, query_id: '42' });
    });
}

const execution = { pid: 661380, query_id: '42', start_ns: '10800000000', end_ns: '10900000000', duration_ms: 100 };

test('empty executions and unmatched filters explain why no chart is shown', () => {
    const previous = globalThis.document;
    try {
        for (const rows of [[], [{ ...execution, query_id: '999' }]]) {
            const elements = new Map();
            const element = id => {
                if (!elements.has(id)) elements.set(id, { innerHTML: '', textContent: '', style: {},
                    classList: { remove() {} } });
                return elements.get(id);
            };
            globalThis.document = { getElementById: element, querySelector: element };
            const view = createWaterfallView();
            const model = view.build({ executions: { rows }, selected: null, detail: null, windowTo: 11e9 });
            view.mount({}, model, { mountTable() {} });
            const target = rows.length ? 'waterfall-chart' : 'executions-table';
            assert.match(element(target).innerHTML, /No executions? match(?:es)? the selected query\/session filters and time range/);
            if (rows.length) assert.equal(element('.waterfall-pane').style.display, '');
        }
    } finally {
        globalThis.document = previous;
    }
});

const detail = { backend_activity_span: span, leader: { pid: 661380,
    events: [{ name: 'CPU*', we: 0, start_ns: execution.start_ns, dur_ns: '100000000' }] }, workers: [] };
const timeline = { fidelity: 'exact', truncated: false,
    events: [{ p: 661380, n: 'Lock:relation', s: '1000000000', d: '9700000000', q: '0' }] };

test('containing activity evidence never stretches the executor chart', () => {
    const context = waterfallContextHtml(detail, execution, timeline);
    assert.match(context, /Relation wait occurred before execution/);
    assert.match(context, /Executor interval: 100/);
    assert.doesNotMatch(context, /Longest/);
    const chart = buildWaterfallOption(detail, { executionStart: execution.start_ns, executionEnd: execution.end_ns });
    assert.equal(chart.fullFrom, execution.start_ns);
    assert.equal(chart.fullTo, execution.end_ns);
});

for (const [label, change] of [
    ['sampled', { fidelity: 'sampled' }], ['mixed', { fidelity: 'mixed' }],
    ['unknown fidelity', { fidelity: undefined }], ['truncated', { truncated: true }],
    ['wait overlaps executor', { events: [{ ...timeline.events[0], d: '9900000000' }] }],
]) test('refuses pre-execution claim when ' + label, () => {
    const html = waterfallContextHtml(detail, execution, { ...timeline, ...change });
    assert.match(html, /Lock:relation/);
    assert.doesNotMatch(html, /occurred before execution/);
    assert.match(html, /not represented by these executor bars; pre-execution timing is not established/);
});

for (const [label, events] of [
    ['another PID', [{ ...timeline.events[0], p: 777777, n: 'Lock:advisory' }]],
    ['outside containing activity', [{ ...timeline.events[0], s: '0', d: '999' }]],
    ['invalid duration', [{ ...timeline.events[0], d: null }]],
]) test('rejects wait evidence from ' + label, () => {
    const html = waterfallContextHtml(detail, execution, { ...timeline, events });
    assert.match(html, /Wait evidence unavailable/);
    assert.doesNotMatch(html, /Lock:relation|Lock:advisory|occurred before/);
});

test('partial/missing/wrong-PID span cannot establish pre-execution timing', () => {
    for (const s of [null, { ...span, pid: 777777 }, { ...span, start_ns: null }]) {
        assert.equal(containingActivityWindow({ backend_activity_span: s }, execution), null);
        assert.doesNotMatch(waterfallContextHtml({ backend_activity_span: s }, execution, timeline), /occurred before/);
    }
    const html = waterfallContextHtml({ ...detail, backend_activity_span: { ...span, measurement: 'partial' } }, execution, timeline);
    assert.match(html, /Partial/);
    assert.doesNotMatch(html, /occurred before/);
});

test('contradictory relation evidence inside executor bars refuses a pre-execution-only claim', () => {
    const html = waterfallContextHtml({ ...detail, leader: { events: [{ name: 'Lock:relation' }] } }, execution, timeline);
    assert.doesNotMatch(html, /occurred before|not represented/);
});

// #523: three readers (two advisers plus the owner relaying their
// conclusion) misread "Observed waits: Lock:transactionid, IO:WalWrite,
// IO:WalSync" as describing the EXECUTOR BARS below -- sub-pixel bars that
// do not exist -- because the scope qualifier lived only in the THIRD
// context line ("Bars show executor phase only..."), and a GIF crop of
// this pane kept the first two lines while losing the third. Simulates
// that exact crop (drop the trailing scope sentence) and asserts the
// qualifier is still present where the wait names are.
test('#523: the backend-scope qualifier survives a GIF crop that drops the trailing scope sentence', () => {
    const html = waterfallContextHtml(detail, execution, timeline);
    const lines = [...html.matchAll(/<div data-context>([\s\S]*?)<\/div>/g)].map(m => m[1]);
    assert.equal(lines.length, 3, 'fixture must produce the full 3-line context block');
    assert.match(lines[2], /bars show executor phase only/i,
        'fixture sanity: the trailing scope sentence this crop drops must exist');
    // The crop the issue reports: keep only the PID/Query line and the
    // "Containing backend activity" line.
    const cropped = lines.slice(0, 2).join(' ');
    assert.match(cropped, /Lock:relation/, 'crop fixture sanity: wait names must survive');
    assert.match(cropped, /Observed waits \(containing backend, not these bars\)/,
        'the scope qualifier must be carried INSIDE the same line as the wait names, ' +
        'not only in the dropped third sentence');
});

test('#523 negative control: the pre-fix unscoped wording fails the same crop assertion', () => {
    // Reconstructs the OLD wording this fix replaced, to prove the crop
    // assertion above actually discriminates -- it must FAIL here exactly
    // as the three real readers were misled.
    const html = waterfallContextHtml(detail, execution, timeline)
        .replace('Observed waits (containing backend, not these bars): ', 'Observed waits: ');
    const lines = [...html.matchAll(/<div data-context>([\s\S]*?)<\/div>/g)].map(m => m[1]);
    const cropped = lines.slice(0, 2).join(' ');
    assert.match(cropped, /Lock:relation/);
    assert.doesNotMatch(cropped, /Observed waits \(containing backend, not these bars\)/,
        'negative control must reproduce the pre-#523 (unscoped) wording');
});

test('Query execution drill carries the selected span PID and releases wait filters, preserving query identity', () => {
    const intent = makeQueriesConfig('exact').columns.find(c => c.key === '_executions')
        .intent({ query_id: '42', backend_activity: { span } });
    assert.equal(intent.pid, 661380);
    assert.equal(intent.filterValue, '42');
    assert.deepEqual(intent.removeKey, ['event_id', 'class']);
});

test('real timeline numeric timestamps prove broad gaps but refuse rounded boundary ties', () => {
    const base = 1792316000000000000n;
    const s = { ...span, start_ns: String(base), end_ns: String(base + 9900000000n) };
    const exec = { ...execution, start_ns: String(base + 9800000000n), end_ns: s.end_ns };
    const wire = { ...timeline, events: [{ p: span.pid, n: 'Lock:relation',
        s: Number(base + 1000000n), d: 9699000000 }] };
    const d = { ...detail, backend_activity_span: s };
    assert.match(waterfallContextHtml(d, exec, wire), /occurred before execution/);
    wire.events[0].d = 9799000000;
    assert.doesNotMatch(waterfallContextHtml(d, exec, wire), /occurred before execution/);
    assert.doesNotMatch(waterfallContextHtml(d, exec, { ...timeline, right_edge_truncated: true }), /occurred before execution/);
});

test('no matching backend execution never falls back to a competing backend', async () => {
    const view = createWaterfallView();
    const data = await view.requests({ timeRange: { from: 1e9, to: 11e9 },
        filters: { snapshot: () => ({ pid: 661380, query_id: '42' }) }, channel: x => x,
        transport: { request: async (_ch, cmd) => {
            assert.equal(cmd, 'executions', 'must not request wrong-PID detail');
            return { rows: [{ ...execution, pid: 777777, query_id: '999', n_events: 1 }] };
        } } });
    assert.equal(data.selected, null);
    assert.equal(data.detail, null);
});

test('failed activity evidence preserves the selected executor and refuses timing claims', async () => {
    const view = createWaterfallView();
    const data = await view.requests({ timeRange: { from: 1e9, to: 11e9 },
        filters: { snapshot: () => ({ pid: 661380, query_id: '42' }) }, channel: x => x,
        transport: { request: async (_ch, cmd) => {
            if (cmd === 'executions') return { rows: [{ ...execution, n_events: 1 }] };
            if (cmd === 'execution_detail') return detail;
            throw new Error('timeline budget exceeded');
        } } });
    assert.equal(data.selected.pid, 661380);
    const model = view.build(data);
    assert.equal(model.waterfall.hasData, true);
    assert.match(model.context, /Wait evidence unavailable/);
    assert.doesNotMatch(model.context, /occurred before/);
});

// ── #477: the server already explains why a containing span is unavailable
// (src/server.c:5845-5849, backend_activity_unavailable_reason) -- the
// Waterfall context line discarded it, rendering a bare "Not measured" that
// reads as "this panel is broken" instead of "the span pass refused, for a
// stated reason". backendSpanEnvelope already extracts the reason
// (backend-span.js); waterfallContextHtml already destructures the same
// envelope for `span` -- it just never read `.reason`.

test('#477 waterfall context discloses the server reason when the containing span is unavailable', () => {
    const html = waterfallContextHtml({ backend_activity_span: null,
        backend_activity_unavailable_reason: 'the backend span pass refused' }, execution, timeline);
    assert.match(html, /Containing backend activity: Not measured — the backend span pass refused/);
});

test('#477 negative control: no reason on the payload -> plain "Not measured", no dangling separator', () => {
    const html = waterfallContextHtml({ backend_activity_span: null }, execution, timeline);
    assert.match(html, /Containing backend activity: Not measured/);
    assert.doesNotMatch(html, / — /);
});

test('#477 a long server reason is clamped, never blows the fixed-height Waterfall fold (#462)', () => {
    const html = waterfallContextHtml({ backend_activity_span: null,
        backend_activity_unavailable_reason: 'x'.repeat(200) }, execution, timeline);
    // Bound by exact substring, not a regex up to the next HTML tag -- a
    // clamp that still terminates only at the next markup boundary (not at
    // 80 real characters) would pass a tag-bounded check without actually
    // being bounded in TEXT length, which is what the fixed-height fold
    // (#462) cares about.
    assert.ok(html.includes('Not measured — ' + 'x'.repeat(80) + '…'), html);
    assert.ok(!html.includes('x'.repeat(81)),
        'the full 200-char reason must not appear unclamped: ' + html);
});
