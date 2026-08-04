import { test } from 'node:test';
import assert from 'node:assert/strict';
import {
    MATRIX_COLORS, buildMatrixOption, matrixCellIntent, matrixTooltipFormatter,
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
    assert.equal(m.option.visualMap.max, Math.log1p(m.rawMax));
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
