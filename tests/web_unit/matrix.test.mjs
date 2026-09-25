import { test } from 'node:test';
import assert from 'node:assert/strict';
import {
    MATRIX_COLORS, buildMatrixOption, matrixCellIntent, matrixTooltipFormatter,
    matrixLegendBucketMax,
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
