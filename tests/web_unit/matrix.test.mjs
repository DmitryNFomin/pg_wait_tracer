import { test } from 'node:test';
import assert from 'node:assert/strict';
import {
    MATRIX_COLORS, buildMatrixOption, matrixCellIntent, matrixTooltipFormatter,
    matrixLegendBucketMax, matrixChartFitHeight,
    MATRIX_CHART_NATURAL_PX, MATRIX_CHART_MIN_PX, MATRIX_FOLD_RESERVE_PX,
} from '../../web/static/lib/builders/matrix.js';
import { eventColor } from '../../web/static/lib/format.js';

function payload(n = 4) {
    const nodes = Array.from({ length: n }, (_, i) => ({
        name: (i % 2 ? 'IO:E' : 'Lock:E') + i,
        class: i % 2 ? 'IO' : 'Lock', event_id: i + 1, total_ms: 10 + i,
    }));
    const links = [];
    for (let i = 0; i < n; i++) {
        links.push({ source: nodes[i].name, target: nodes[(i + 1) % n].name,
            value: n - i, duration_ms: (n - i) * 2.5 });
    }
    return { total: 100, nodes, links };
}

test('matrix axes are source x / target y and cell carries log + raw + target id', () => {
    const m = buildMatrixOption(payload(), { limit: 20 });
    assert.equal(m.option.xAxis.name, 'Source event');
    assert.equal(m.option.yAxis.name, 'Target event');
    const cell = m.cells[0];
    assert.equal(cell[2], Math.log1p(cell[3]));
    assert.equal(cell[5], payload().nodes.find(n => n.name === m.labels[cell[1]]).event_id);
});

test('top-N is incident-link ranked, deterministic, capped at 20 with honest N-more note', () => {
    const m = buildMatrixOption(payload(23), { limit: 99 });
    assert.equal(m.visibleCount, 20);
    assert.equal(m.hiddenCount, 3);
    assert.ok(m.notes.includes('3 linked events outside the top 20'));
    assert.equal(m.option.xAxis.data.length, 20);
});

test('visualMap is piecewise over log1p(count) and uses one hue ramp', () => {
    const m = buildMatrixOption(payload());
    assert.equal(m.option.visualMap.type, 'piecewise');
    assert.equal(m.option.visualMap.dimension, 2);
    assert.equal(m.option.visualMap.max, Math.log1p(m.bucketMax));
    assert.deepEqual(m.option.visualMap.inRange.color, MATRIX_COLORS);
    assert.equal(m.option.visualMap.formatter(Math.log1p(4)), '4');
    assert.equal(m.option.visualMap.formatter(Math.log1p(4), Math.log1p(9)), '4–9');
});

test('event identity appears only on axis-label text colors', () => {
    const p = payload();
    const m = buildMatrixOption(p);
    for (const name of m.labels) {
        const n = p.nodes.find(x => x.name === name);
        assert.equal(m.labelColors[name], eventColor(n.class, name));
        assert.equal(m.option.xAxis.axisLabel.color(name), eventColor(n.class, name));
    }
    assert.deepEqual(m.option.visualMap.inRange.color, MATRIX_COLORS);
});

test('tooltip direction, count and duration are raw analyst values', () => {
    const labels = ['IO:Read', 'CPU*'];
    const html = matrixTooltipFormatter(labels,
        { data: [0, 1, Math.log1p(500), 500, 123.4, 0] });
    assert.ok(html.includes('IO:Read</b> → <b>CPU*'));
    assert.ok(html.includes('Count: <b>500</b>'));
    assert.ok(html.includes('123.4ms'));
});

test('duplicate source-target links aggregate both count and duration', () => {
    const p = payload(2);
    p.links.push({ ...p.links[0], value: 6, duration_ms: 9 });
    const m = buildMatrixOption(p);
    const x = m.labels.indexOf(p.links[0].source), y = m.labels.indexOf(p.links[0].target);
    const c = m.cells.find(v => v[0] === x && v[1] === y);
    assert.equal(c[3], p.links[0].value + 6);
    assert.equal(c[4], p.links[0].duration_ms + 9);
});

test('notes account for hidden transition volume and the server link cap', () => {
    const p = {
        total: 2916, link_count: 72, total_link_count: 90, truncated: true,
        nodes: [
            { name: 'CPU*', class: 'CPU', event_id: 0 },
            { name: 'IO:A', class: 'IO', event_id: 1 },
            { name: 'Lock:B', class: 'Lock', event_id: 2 },
        ],
        links: [
            { source: 'CPU*', target: 'IO:A', value: 2000, duration_ms: 1 },
            { source: 'IO:A', target: 'Lock:B', value: 350, duration_ms: 1 },
        ],
    };
    const m = buildMatrixOption(p, { limit: 2 });
    assert.equal(m.shownVolume, 2000);
    assert.equal(m.hiddenVolume, 916);
    assert.ok(m.notes.includes('Showing 2,000 of 2,916 transitions; 916 not shown'));
    assert.ok(m.notes.some(n => n.includes('Server returned 72 of 90 transition links')));
    assert.equal(m.serverTruncated, true);
});

test('zero-link catalog nodes do not inflate hidden linked-event count', () => {
    const p = payload(2);
    p.nodes.push({ name: 'IO:Unlinked', class: 'IO', event_id: 99 });
    const m = buildMatrixOption(p, { limit: 2 });
    assert.equal(m.hiddenCount, 0);
});

test('CPU target cells drill through the CPU class instead of going dead', () => {
    assert.deepEqual(matrixCellIntent([0, 1, 1, 2, 3, 0], ['IO:Read', 'CPU*']), {
        pivot: 'matrix-cell', filterKey: 'class', filterValue: 'CPU', label: 'CPU*',
    });
    assert.equal(matrixCellIntent([0, 1, 1, 2, 3, null], ['IO:Read', 'Unknown']), null);
});

test('empty matrix is explicit and animation is disabled for data', () => {
    assert.equal(buildMatrixOption({ nodes: [], links: [] }).hasData, false);
    assert.equal(buildMatrixOption({ nodes: [], links: [] }).option, null);
    assert.equal(buildMatrixOption(payload()).option.animation, false);
});

test('row/column order is stable across ticks even when scores reshuffle (#104)', () => {
    const nodes = [
        { name: 'Timeout:VacuumDelay', class: 'Timeout', event_id: 1 },
        { name: 'LWLock:BufferContent', class: 'LWLock', event_id: 2 },
        { name: 'CPU*', class: 'CPU', event_id: 0 },
    ];
    const tick1 = {
        total: 100, nodes,
        links: [
            { source: 'CPU*', target: 'Timeout:VacuumDelay', value: 50, duration_ms: 1 },
            { source: 'CPU*', target: 'LWLock:BufferContent', value: 40, duration_ms: 1 },
        ],
    };
    // Tick 2: no user action, but the two events' incident-link scores swap
    // rank (the real defect's shape — see docs/VISUAL_CHECKLIST.md STABILITY).
    const tick2 = {
        total: 100, nodes,
        links: [
            { source: 'CPU*', target: 'Timeout:VacuumDelay', value: 40, duration_ms: 1 },
            { source: 'CPU*', target: 'LWLock:BufferContent', value: 50, duration_ms: 1 },
        ],
    };
    const m1 = buildMatrixOption(tick1, { limit: 20 });
    // Without prevLabels, rank order alone WOULD reorder the two rows —
    // confirms the fixture actually exercises the reshuffle.
    const mNaive = buildMatrixOption(tick2, { limit: 20 });
    assert.notDeepEqual(mNaive.labels, m1.labels);
    // With prevLabels (what the view now always passes), order is held.
    const m2 = buildMatrixOption(tick2, { limit: 20, prevLabels: m1.labels });
    assert.deepEqual(m2.labels, m1.labels);
});

test('a newly-entering event is appended, not inserted, holding old positions', () => {
    const p1 = payload(3);
    const m1 = buildMatrixOption(p1, { limit: 3 });
    const p2 = payload(4); // adds a 4th event with the highest score
    const m2 = buildMatrixOption(p2, { limit: 4, prevLabels: m1.labels });
    assert.deepEqual(m2.labels.slice(0, m1.labels.length), m1.labels);
    assert.equal(m2.labels.length, 4);
});

test('legend bucket max is quantized to a fixed log grid and stable for a modest range change', () => {
    assert.equal(matrixLegendBucketMax(4), 5);
    assert.equal(matrixLegendBucketMax(53971), 100000);
    assert.equal(matrixLegendBucketMax(57811), 100000); // same grid point as 53,971
    assert.equal(matrixLegendBucketMax(600000), 1000000); // real order-of-magnitude jump

    // The issue's own real-data numbers: rawMax drifted 53,971 -> 57,811
    // between ticks (~7% sampling jitter) and the OLD code re-quantized the
    // legend from top-bucket "6,105-53,971" to "6,450-57,811" for it.
    const nodesFixed = [
        { name: 'CPU*', class: 'CPU', event_id: 0 },
        { name: 'IO:DataFileRead', class: 'IO', event_id: 1 },
    ];
    const linkAt = (value) => ({ total: value, nodes: nodesFixed,
        links: [{ source: 'CPU*', target: 'IO:DataFileRead', value, duration_ms: 1 }] });
    const mA = buildMatrixOption(linkAt(53971), { limit: 20 });
    const mB = buildMatrixOption(linkAt(57811), { limit: 20 });
    assert.equal(mA.bucketMax, mB.bucketMax);
    assert.equal(mA.option.visualMap.max, mB.option.visualMap.max);
});

// #438: CPU* is a literal intermediate state, not necessarily measured
// running CPU -- the fix is explaining that wherever CPU* is actually
// shown, not re-deriving a different (inferred wait-to-wait) metric.
//
// #438/#473-1 (video review): the explanation used to be appended into the
// SAME `notes` array as the hidden-count/link-cap disclosures, which the
// view renders BELOW the chart canvas (#matrix-notes) -- below the fold at
// the demo viewport, so a viewer never saw why the matrix looks like a
// degenerate L. It is now its own `cpuNote` field, kept OUT of `notes`
// entirely, so the view can place it ABOVE the canvas while the volume/
// link-cap disclosures stay in their existing below-canvas slot.
test('#438: cpuNote is set exactly when CPU* is a shown label, and is NEVER folded into notes', () => {
    const withCpu = buildMatrixOption(payload(4)); // payload() alternates Lock:E/IO:E, no CPU*
    assert.equal(withCpu.cpuNote, null, 'no CPU* note when CPU* never appears on an axis');
    assert.ok(!withCpu.notes.some(n => n.includes('CPU*')));

    const p = {
        total: 10,
        nodes: [
            { name: 'CPU*', class: 'CPU', event_id: 0 },
            { name: 'IO:A', class: 'IO', event_id: 1 },
        ],
        links: [{ source: 'CPU*', target: 'IO:A', value: 5, duration_ms: 1 }],
    };
    const m = buildMatrixOption(p);
    assert.ok(m.labels.includes('CPU*'));
    assert.ok(typeof m.cpuNote === 'string' && m.cpuNote.includes('CPU*') &&
        m.cpuNote.includes('literal intermediate state'),
        'cpuNote must appear once CPU* is a shown label: ' + JSON.stringify(m.cpuNote));
    // Negative control: a note that merely says "CPU-centered" without the
    // "literal intermediate state, not necessarily measured running CPU"
    // wording would re-introduce the exact misreading #438 reports (that
    // the matrix measures running CPU time).
    assert.ok(!(m.cpuNote.includes('CPU*') && !m.cpuNote.includes('literal')),
        'the CPU* note must carry the explanation, not just the name');
    // The volume/link-cap `notes` array must never carry the CPU* text --
    // it stays a SEPARATE field the view can position independently.
    assert.ok(!m.notes.some(n => n.includes('CPU*')),
        'notes (rendered below the chart) must not duplicate cpuNote: ' + JSON.stringify(m.notes));
});

test('#438 cpuNote coexists with an unrelated notes entry (hidden-count disclosure)', () => {
    const p = {
        total: 10,
        nodes: [
            { name: 'CPU*', class: 'CPU', event_id: 0 },
            { name: 'IO:A', class: 'IO', event_id: 1 },
            { name: 'Lock:B', class: 'Lock', event_id: 2 },
        ],
        links: [
            { source: 'CPU*', target: 'IO:A', value: 5, duration_ms: 1 },
            { source: 'IO:A', target: 'Lock:B', value: 3, duration_ms: 1 },
        ],
    };
    const m = buildMatrixOption(p, { limit: 2 });
    assert.ok(m.cpuNote && m.cpuNote.includes('literal intermediate state'));
    assert.ok(m.notes.some(n => n.includes('outside the top 2')),
        'the unrelated hidden-count note must still be in notes: ' + JSON.stringify(m.notes));
    assert.ok(!m.notes.some(n => n.includes('CPU*')));
});

// #438 (owner decision, issue #438: "Transition Matrix is a degenerate L on
// the demo workload: only the CPU* row and column populate, because every
// transition routes through CPU*"): state that fact above the grid rather
// than leaving an L-shaped grid to read as broken. GATED BOTH WAYS -- the
// extra sentence must appear exactly when the full link set is truly
// CPU*-incident-only (the degenerate case), and must NOT appear merely
// because CPU* is one of the shown axis labels (the "coexists" test above
// already covers a CPU*-containing but non-degenerate link set).
test('#438: a truly degenerate matrix (every link touches CPU*) states the L-shape fact', () => {
    const p = {
        total: 10,
        // #438/#591: the fidelity of the claim depends on the link set being
        // COMPLETE, which only the server can attest (`truncated`). This
        // fixture says so explicitly -- see the capped-response tests below
        // for why an ABSENT flag is not read as "complete".
        truncated: false, link_count: 3, total_link_count: 3,
        nodes: [
            { name: 'CPU*', class: 'CPU', event_id: 0 },
            { name: 'IO:A', class: 'IO', event_id: 1 },
            { name: 'Lock:B', class: 'Lock', event_id: 2 },
        ],
        links: [
            { source: 'CPU*', target: 'IO:A', value: 5, duration_ms: 1 },
            { source: 'CPU*', target: 'Lock:B', value: 3, duration_ms: 1 },
            { source: 'Lock:B', target: 'CPU*', value: 2, duration_ms: 1 },
        ],
    };
    const m = buildMatrixOption(p);
    assert.ok(m.cpuNote.includes('Every transition in this workload passes through CPU*'),
        'degenerate-L fixture must state the fact: ' + JSON.stringify(m.cpuNote));
    assert.ok(m.cpuNote.includes('the L shape is the data, not a rendering bug'),
        m.cpuNote);
    // Still carries the base explanation too (it is an addition, not a
    // replacement) -- the "coexists" test above pins that substring too.
    assert.ok(m.cpuNote.includes('literal intermediate state'));
});

test('#438: a non-degenerate matrix (a real wait->wait link exists) does NOT claim every transition passes through CPU*', () => {
    const p = {
        total: 10,
        nodes: [
            { name: 'CPU*', class: 'CPU', event_id: 0 },
            { name: 'IO:A', class: 'IO', event_id: 1 },
            { name: 'Lock:B', class: 'Lock', event_id: 2 },
        ],
        links: [
            { source: 'CPU*', target: 'IO:A', value: 5, duration_ms: 1 },
            // A genuine wait -> wait link that never touches CPU* -- the
            // claim "every transition passes through CPU*" would be FALSE
            // here, so the builder must not assert it.
            { source: 'IO:A', target: 'Lock:B', value: 3, duration_ms: 1 },
        ],
    };
    const m = buildMatrixOption(p);
    assert.ok(m.cpuNote.includes('literal intermediate state'));
    assert.ok(!m.cpuNote.includes('Every transition in this workload passes through CPU*'),
        'non-degenerate fixture must not claim the L-shape fact: ' + JSON.stringify(m.cpuNote));
});

// ── #438 follow-up: the "every transition" claim vs the SERVER link cap ────
//
// Found while diagnosing #591's transition_matrix snapshot failure. The
// sentence above is asserted from `data.links`, which is the SERVER-CAPPED
// link set: emit_transitions_response() in src/server.c emits
// `truncated = shown < total_links` and fills `links` with `shown` entries
// only. The payload the snapshot actually bakes -- tests/mock_server.py's
// canned transitions reply -- is `truncated: true, link_count: 5,
// total_link_count: 8`, so "Every transition in this workload passes
// through CPU*" was being stated from 5 links with 3 never examined. If one
// of the 3 omitted links were a direct wait -> wait edge, the UI would state
// the exact opposite of the truth.
//
// #438's own guard was written for client-side top-N LABEL truncation (that
// is why it reads `links` rather than the shown labels) and does not see the
// server cap at all.
//
// THE VACUITY TRAP these tests exist to close: an UNCAPPED fixture makes
// `links` complete, so the claim is sound and ANY wiring -- including none
// -- passes. Every case below therefore pins a capped or unprovable payload
// against an uncapped control, and each absence assertion is paired with a
// POSITIVE assertion that the base note is still there, so an early return
// (links: []) or a null cpuNote cannot satisfy it vacuously.

/* The mock's canned transitions payload, copied literally from
 * tests/mock_server.py (cmd == "transitions"): 5 of 8 links, truncated,
 * every returned link incident to CPU*. This is the response the
 * transition_matrix snapshot cell renders, so this fixture IS the regression. */
function cappedAllCpuPayload() {
    return {
        total: 1800, link_count: 5, total_link_count: 8, truncated: true,
        idle_classification_complete: true,
        nodes: [
            { name: 'CPU*', total_ms: 4800, class: 'CPU', event_id: 0, idle: false },
            { name: 'Client:ClientRead', total_ms: 9153, class: 'Client',
              event_id: 0x06000000, idle: true },
            { name: 'IO:DataFileRead', total_ms: 2100, class: 'IO',
              event_id: 0x01000015, idle: false },
            { name: 'LWLock:WALInsert', total_ms: 900, class: 'LWLock',
              event_id: 0x04000007, idle: false },
        ],
        links: [
            { source: 'Client:ClientRead', target: 'CPU*', value: 900, duration_ms: 9000.0 },
            { source: 'CPU*', target: 'Client:ClientRead', value: 560, duration_ms: 5000.0 },
            { source: 'CPU*', target: 'IO:DataFileRead', value: 100, duration_ms: 500.0 },
            { source: 'IO:DataFileRead', target: 'CPU*', value: 80, duration_ms: 320.0 },
            { source: 'CPU*', target: 'LWLock:WALInsert', value: 30, duration_ms: 90.0 },
        ],
    };
}

const EVERY = 'Every transition in this workload passes through CPU*';

test('#438/#591: a SERVER-CAPPED response must not claim every transition passes through CPU*', () => {
    const m = buildMatrixOption(cappedAllCpuPayload());
    // Positive half first: the note exists and carries the base explanation,
    // so the absence assertion below cannot be satisfied by a null cpuNote.
    assert.ok(m.labels.includes('CPU*'), 'CPU* is on the axes for this payload');
    assert.ok(typeof m.cpuNote === 'string' &&
        m.cpuNote.includes('literal intermediate state'),
        'the base CPU* explanation must survive: ' + JSON.stringify(m.cpuNote));
    // The claim itself: unsupportable from 5 of 8 links.
    assert.ok(!m.cpuNote.includes(EVERY),
        'a 5-of-8 capped response must not assert a claim about EVERY ' +
        'transition: ' + JSON.stringify(m.cpuNote));
    assert.ok(!m.cpuNote.includes('the L shape is the data'),
        'nor the conclusion drawn from it: ' + JSON.stringify(m.cpuNote));
    // And the reader is still told why the grid is sparse -- the disclosure
    // is relocated to the cap footer, not removed from the UI.
    assert.ok(m.notes.some(n => n.includes('5 of 8') && n.includes('link cap')),
        'the cap footer must still disclose the omission: ' + JSON.stringify(m.notes));
    assert.equal(m.serverTruncated, true);
});

test('#438/#591 CONTROL: an UNCAPPED all-CPU response still states the L-shape fact', () => {
    // Same link shape, but the server says it returned everything. Without
    // this control the test above would be satisfied by deleting the feature.
    const p = cappedAllCpuPayload();
    p.truncated = false;
    p.link_count = p.links.length;
    p.total_link_count = p.links.length;
    const m = buildMatrixOption(p);
    assert.ok(m.cpuNote.includes(EVERY),
        'an uncapped all-CPU response must keep the #438 sentence: ' +
        JSON.stringify(m.cpuNote));
    assert.ok(m.cpuNote.includes('the L shape is the data, not a rendering bug'),
        m.cpuNote);
    assert.ok(!m.notes.some(n => n.includes('link cap')),
        'no cap footer when nothing was capped: ' + JSON.stringify(m.notes));
});

// ── Bypass suite: each way the new guard's detection could be made
// unreachable, so it approves without having checked anything. ────────────

test('#438/#591 bypass: an ABSENT cap flag is not evidence of completeness', () => {
    // A payload with no `truncated` key at all -- an older/other producer, a
    // hand-written fixture, a bridge that dropped the field. The real server
    // emits `truncated` unconditionally, so its absence means the link set's
    // provenance cannot be established. A check that cannot see must refuse.
    const p = cappedAllCpuPayload();
    delete p.truncated;
    delete p.link_count;
    delete p.total_link_count;
    const m = buildMatrixOption(p);
    assert.ok(m.cpuNote.includes('literal intermediate state'));
    assert.ok(!m.cpuNote.includes(EVERY),
        'no cap flag => completeness unproven => no universal claim: ' +
        JSON.stringify(m.cpuNote));
});

test('#438/#591 bypass: counts that CONTRADICT truncated:false refuse the claim', () => {
    // `truncated: false` with total_link_count (8) above the number of links
    // that actually arrived (5) is a self-contradictory payload: something
    // trimmed the array after the flag was computed. Trusting the flag alone
    // would restate the claim from an incomplete set.
    const p = cappedAllCpuPayload();
    p.truncated = false;           // claims complete ...
    p.link_count = 5;
    p.total_link_count = 8;        // ... while declaring 8 exist
    const m = buildMatrixOption(p);
    assert.ok(m.cpuNote.includes('literal intermediate state'));
    assert.ok(!m.cpuNote.includes(EVERY),
        'contradictory counts must refuse the claim: ' + JSON.stringify(m.cpuNote));

    // Mirror: link_count above the array length (the array was trimmed).
    const q = cappedAllCpuPayload();
    q.truncated = false;
    q.link_count = 9;
    q.total_link_count = 9;
    const n = buildMatrixOption(q);
    assert.ok(!n.cpuNote.includes(EVERY),
        'link_count above the links that arrived must refuse: ' +
        JSON.stringify(n.cpuNote));
});

test('#438/#591 bypass: UNPARSEABLE cap fields refuse rather than approve', () => {
    // A string "false" is truthy, and a non-numeric count is NaN. Neither may
    // read as "complete" through a loose coercion.
    const p = cappedAllCpuPayload();
    p.truncated = 'false';
    const m = buildMatrixOption(p);
    assert.ok(m.cpuNote.includes('literal intermediate state'));
    assert.ok(!m.cpuNote.includes(EVERY),
        'truncated as a string must not read as false: ' + JSON.stringify(m.cpuNote));

    const q = cappedAllCpuPayload();
    q.truncated = false;
    q.link_count = 5;
    q.total_link_count = 'many';   // present but unparseable
    const n = buildMatrixOption(q);
    assert.ok(n.cpuNote.includes('literal intermediate state'));
    assert.ok(!n.cpuNote.includes(EVERY),
        'an unparseable total_link_count must refuse: ' + JSON.stringify(n.cpuNote));
});

test('#438/#591 bypass: the thing being checked ABSENT, not wrong', () => {
    // (a) No links at all: the builder returns early. cpuNote must be absent
    //     rather than a claim about a workload with no transitions.
    const empty = buildMatrixOption({ total: 0, nodes: [], links: [],
        truncated: false, link_count: 0, total_link_count: 0 });
    assert.equal(empty.hasData, false);
    assert.ok(!empty.cpuNote, 'no cpuNote for an empty response: ' +
        JSON.stringify(empty.cpuNote));

    // (b) A capped response in which CPU* is not on the axes at all: the
    //     whole note is absent, so there is nothing to weaken -- this pins
    //     that the new guard did not turn "no CPU*" into a partial sentence.
    const p = {
        total: 100, truncated: true, link_count: 1, total_link_count: 4,
        nodes: [
            { name: 'IO:A', class: 'IO', event_id: 1 },
            { name: 'Lock:B', class: 'Lock', event_id: 2 },
        ],
        links: [{ source: 'IO:A', target: 'Lock:B', value: 5, duration_ms: 1 }],
    };
    const m = buildMatrixOption(p);
    assert.ok(!m.labels.includes('CPU*'));
    assert.equal(m.cpuNote, null, 'no CPU* label => no cpuNote: ' +
        JSON.stringify(m.cpuNote));
    // ... and the cap footer is still rendered, so the user is not left with
    // a silently partial grid.
    assert.ok(m.notes.some(n => n.includes('link cap')), JSON.stringify(m.notes));
});

// #473 item 2 (video review): several IO event names render dark blue on
// the #1a1a2e navy background and are effectively invisible (IO:DataFileWrite,
// IO:WalWrite, IO:WalSync, IO:DataFilePrefetch, IO:AioIoCompletion,
// IO:DataFileSync -- f010/f036). The fix is a contrast FLOOR applied to the
// axis label color, not a palette change -- identity (same class hue) must
// survive. GATE: assert the builder's axisLabel.color OUTPUT directly --
// ECharts draws labels to a <canvas>, so a pixel-level assertion on the
// rendered page would be reading the wrong element entirely (the DOM/canvas
// pixels are not inspectable the way a CSS color string is).
//
// The WCAG contrast-ratio math below is written independently of whatever
// matrix.js uses internally (it is the public, standard formula) -- an
// assertion that imported matrix.js's OWN ratio helper to check matrix.js's
// own output could pass a broken floor that still agreed with itself.
function srgbChannel(c) { c /= 255; return c <= 0.03928 ? c / 12.92 : Math.pow((c + 0.055) / 1.055, 2.4); }
function relLuminance(r, g, b) { return 0.2126 * srgbChannel(r) + 0.7152 * srgbChannel(g) + 0.0722 * srgbChannel(b); }
function contrastRatio(l1, l2) { const hi = Math.max(l1, l2), lo = Math.min(l1, l2); return (hi + 0.05) / (lo + 0.05); }
const MATRIX_BG_LUM = relLuminance(0x1a, 0x1a, 0x2e);
function parseRgb(s) {
    const m = /^rgb\((\d+),\s*(\d+),\s*(\d+)\)$/.exec(s);
    assert.ok(m, `not an rgb(...) string: ${s}`);
    return [+m[1], +m[2], +m[3]];
}
function contrastOf(rgbString) {
    const [r, g, b] = parseRgb(rgbString);
    return contrastRatio(relLuminance(r, g, b), MATRIX_BG_LUM);
}

function ioMatrixPayload() {
    return {
        total: 100,
        nodes: [
            { name: 'IO:DataFileWrite', class: 'IO', event_id: 1 },
            { name: 'IO:WalSync', class: 'IO', event_id: 2 },
            { name: 'Lock:relation', class: 'Lock', event_id: 3 },
        ],
        links: [
            { source: 'IO:DataFileWrite', target: 'IO:WalSync', value: 5, duration_ms: 1 },
            { source: 'IO:WalSync', target: 'Lock:relation', value: 3, duration_ms: 1 },
        ],
    };
}

test('#473-2 a low-contrast IO axis label is lifted to a readable floor (>= 3:1 vs #1a1a2e)', () => {
    const m = buildMatrixOption(ioMatrixPayload());
    const rawDataFileWrite = eventColor('IO', 'IO:DataFileWrite');
    const rawWalSync = eventColor('IO', 'IO:WalSync');
    // Confirms the fixture actually exercises the defect: the UNFLOORED
    // color really is below the floor (the negative control for this gate).
    assert.ok(contrastOf(rawDataFileWrite) < 3.0,
        `fixture choice is stale -- IO:DataFileWrite's raw color is no ` +
        `longer low-contrast (${contrastOf(rawDataFileWrite).toFixed(2)}:1)`);
    assert.ok(contrastOf(rawWalSync) < 3.0,
        `fixture choice is stale -- IO:WalSync's raw color is no longer ` +
        `low-contrast (${contrastOf(rawWalSync).toFixed(2)}:1)`);

    for (const name of ['IO:DataFileWrite', 'IO:WalSync']) {
        const floored = m.option.xAxis.axisLabel.color(name);
        assert.ok(contrastOf(floored) >= 3.0 - 1e-6,
            `${name}: ${floored} contrast ${contrastOf(floored).toFixed(2)}:1, below the 3:1 floor`);
        // Identity preserved: still a blue (IO) swatch, not reassigned to
        // another class's hue -- blue channel strictly dominant.
        const [r, g, b] = parseRgb(floored);
        assert.ok(b > r && b > g, `${name}: ${floored} is no longer blue-dominant (lost IO identity)`);
        // Y axis must apply the identical floor (shares labelColors).
        assert.equal(m.option.yAxis.axisLabel.color(name), floored);
    }
});

test('#473-2 negative control: a color that already clears the floor is left byte-identical', () => {
    const m = buildMatrixOption(ioMatrixPayload());
    const raw = eventColor('Lock', 'Lock:relation');
    assert.ok(contrastOf(raw) >= 3.0, `fixture choice is stale -- Lock:relation is now low-contrast`);
    assert.equal(m.option.xAxis.axisLabel.color('Lock:relation'), raw,
        'a color that already meets the floor must be returned unchanged, not nudged');
});

test('axis names are positioned clear of tick labels, not at the clipped default end', () => {
    const m = buildMatrixOption(payload());
    assert.equal(m.option.xAxis.name, 'Source event');
    assert.equal(m.option.xAxis.nameLocation, 'middle');
    assert.ok(m.option.xAxis.nameGap > 0);
    assert.equal(m.option.yAxis.name, 'Target event');
    assert.equal(m.option.yAxis.nameLocation, 'middle');
    assert.ok(m.option.yAxis.nameGap > 0);
    // The y-axis name gap must clear the reserved left label column (grid.left)
    // so it renders as its own strip, never over the first x tick label.
    assert.ok(m.option.yAxis.nameGap < m.option.grid.left,
        'yAxis name must stay inside the left grid margin, not spill onto the plot');
});

// #473 item 1 (live-walk evidence: tests/results/demo_walk/
// mac-walk1-live-8112c71/frames/002700.jpg and 002820.jpg at the pinned
// demo viewport 1710x981 -- #matrix-chart's own top measured 493.39px
// there, so its OLD fixed 660px CSS height put its bottom at 1153.39,
// 172px past the fold; the x-axis tick labels (grid.bottom:150, where they
// live) never rendered on screen). matrixChartFitHeight is the same
// "shrink only by the overflow plus a fold reserve" shape as #462's
// waterfallChartFitHeight -- see that test file's header comment for the
// identical reasoning, applied here to the Matrix tab.
test('#473-1: a page with plenty of room is untouched (no needless shrink)', () => {
    // overflow very negative (top sits high on a tall viewport) -> the
    // natural 660px height must come back unchanged, so no gallery
    // snapshot cell moves because of this fix.
    assert.equal(matrixChartFitHeight(MATRIX_CHART_NATURAL_PX, MATRIX_CHART_MIN_PX, -400),
        MATRIX_CHART_NATURAL_PX);
});

test('#473-1: the measured live-walk overflow (172px) shrinks the chart enough to clear the fold', () => {
    // Real geometry from the frames above: top=493.39, natural=660 ->
    // naturalBottom = 1153.39, viewport 981 -> overflow ~172.39.
    const top = 493.390625, innerHeight = 981;
    const overflow = (top + MATRIX_CHART_NATURAL_PX) - innerHeight;
    const px = matrixChartFitHeight(MATRIX_CHART_NATURAL_PX, MATRIX_CHART_MIN_PX, overflow);
    const finalBottom = top + px;
    assert.ok(finalBottom <= innerHeight - MATRIX_FOLD_RESERVE_PX + 1,
        `expected the fold reserve to survive: finalBottom=${finalBottom}, innerHeight=${innerHeight}`);
    assert.ok(px < MATRIX_CHART_NATURAL_PX, 'the chart must actually have shrunk');
    assert.ok(px >= MATRIX_CHART_MIN_PX, 'must never go below the CSS floor');
});

test('#473-1: an overflow larger than the available slack clamps at the CSS floor, never below it', () => {
    assert.equal(matrixChartFitHeight(MATRIX_CHART_NATURAL_PX, MATRIX_CHART_MIN_PX, 100000),
        MATRIX_CHART_MIN_PX);
});
