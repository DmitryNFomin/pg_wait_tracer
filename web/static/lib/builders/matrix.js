/* pgwt — pure builder: transitions payload -> top-N transition heatmap. */

import { eventColor, fmtMs, esc } from '../format.js';

export const MATRIX_COLORS =
    ['#763e84', '#a04ab5', '#bb6ecf', '#d496e3', '#eac3f4'];

function nodeCatalog(data) {
    const map = {};
    for (const n of (data && data.nodes) || []) map[n.name] = n;
    return map;
}

export function matrixTooltipFormatter(labels, params) {
    const d = params.data;
    return '<b>' + esc(labels[d[0]]) + '</b> → <b>' + esc(labels[d[1]]) +
        '</b><br>Count: <b>' + Number(d[3]).toLocaleString() + '</b><br>' +
        'Duration: ' + fmtMs(d[4]);
}

export function matrixCellIntent(tuple, labels) {
    if (!tuple) return null;
    const targetId = tuple[5];
    const targetName = labels && labels[tuple[1]];
    if (targetId == null || !targetName) return null;
    if (Number(targetId) === 0) {
        return { pivot: 'matrix-cell', filterKey: 'class',
            filterValue: 'CPU', label: targetName };
    }
    return { pivot: 'matrix-cell', filterKey: 'event_id',
        filterValue: targetId, label: targetName };
}

/* `limit` is clamped to 1..20. Events are ranked by total incident link
 * count (source + target), with a lexical tie-break for deterministic output.
 * Color maps log1p(count) through a piecewise single-hue ramp; class identity
 * appears only in axis-label text colors. */
export function buildMatrixOption(data, opts) {
    opts = opts || {};
    const links = (data && data.links) || [];
    if (!links.length) {
        return { option: null, hasData: false, labels: [], hiddenCount: 0,
            visibleCount: 0, notes: [], cells: [] };
    }
    const limit = Math.max(1, Math.min(20, opts.limit || 20));
    const nodes = nodeCatalog(data);
    const scores = {};
    for (const l of links) {
        const v = Number(l.value) || 0;
        scores[l.source] = (scores[l.source] || 0) + v;
        scores[l.target] = (scores[l.target] || 0) + v;
    }
    const allNames = Object.keys(scores).sort((a, b) =>
        scores[b] - scores[a] || a.localeCompare(b));
    const labels = allNames.slice(0, limit);
    const hiddenCount = Math.max(0, allNames.length - labels.length);
    const index = {};
    labels.forEach((name, i) => { index[name] = i; });

    const byCell = {};
    let returnedVolume = 0;
    for (const l of links) {
        returnedVolume += Number(l.value) || 0;
        if (!(l.source in index) || !(l.target in index)) continue;
        const key = index[l.source] + ':' + index[l.target];
        if (!byCell[key]) byCell[key] = {
            x: index[l.source], y: index[l.target], count: 0, duration: 0,
            targetId: nodes[l.target] && nodes[l.target].event_id != null
                ? nodes[l.target].event_id : null,
        };
        byCell[key].count += Number(l.value) || 0;
        byCell[key].duration += Number(l.duration_ms) || 0;
    }
    const cells = Object.values(byCell).map(c =>
        [c.x, c.y, Math.log1p(c.count), c.count, c.duration, c.targetId]);
    const rawMax = cells.length ? Math.max(...cells.map(c => c[3])) : 1;
    const labelColors = {};
    labels.forEach(name => {
        const n = nodes[name] || {};
        labelColors[name] = eventColor(n.class != null ? n.class : null, name);
    });
    const shownVolume = Object.values(byCell).reduce((sum, c) => sum + c.count, 0);
    const declaredTotal = Number(data && data.total);
    const totalVolume = Number.isFinite(declaredTotal) && declaredTotal >= 0
        ? declaredTotal : returnedVolume;
    const hiddenVolume = Math.max(0, totalVolume - shownVolume);
    const notes = [];
    if (hiddenVolume > 0) {
        notes.push('Showing ' + shownVolume.toLocaleString() + ' of ' +
            totalVolume.toLocaleString() + ' transitions; ' +
            hiddenVolume.toLocaleString() + ' not shown');
    }
    if (hiddenCount) {
        notes.push(hiddenCount.toLocaleString() + ' linked event' +
            (hiddenCount === 1 ? '' : 's') + ' outside the top ' + limit);
    }
    if (data && data.truncated === true) {
        const keptLinks = Number.isFinite(Number(data.link_count))
            ? Number(data.link_count) : links.length;
        const totalLinks = Number.isFinite(Number(data.total_link_count))
            ? ' of ' + Number(data.total_link_count).toLocaleString() : '';
        notes.push('Server returned ' + keptLinks.toLocaleString() + totalLinks +
            ' transition links (link cap); transition totals include omitted links');
    }
    const option = {
        backgroundColor: 'transparent', animation: false,
        tooltip: {
            position: 'top', backgroundColor: '#1e1e3a', borderColor: '#333',
            textStyle: { color: '#e0e0e0', fontSize: 12 },
            formatter: (p) => matrixTooltipFormatter(labels, p),
        },
        grid: { left: 170, right: 40, top: 22, bottom: 150 },
        xAxis: {
            type: 'category', name: 'Source event', data: labels,
            axisLabel: { interval: 0, rotate: 48, fontSize: 10,
                color: (v) => labelColors[v] || '#aaa' },
            axisLine: { lineStyle: { color: '#333' } }, splitArea: { show: false },
        },
        yAxis: {
            type: 'category', name: 'Target event', data: labels,
            inverse: true,
            axisLabel: { interval: 0, fontSize: 10,
                color: (v) => labelColors[v] || '#aaa' },
            axisLine: { lineStyle: { color: '#333' } }, splitArea: { show: false },
        },
        visualMap: {
            type: 'piecewise', min: 0, max: Math.log1p(rawMax), splitNumber: 5,
            dimension: 2, orient: 'horizontal', left: 'center', bottom: 4,
            textStyle: { color: '#888', fontSize: 10 },
            formatter: (min, max) => {
                const lo = Math.round(Math.expm1(min)).toLocaleString();
                if (max == null || !Number.isFinite(Number(max))) return lo;
                const hi = Math.round(Math.expm1(max)).toLocaleString();
                return lo === hi ? lo : lo + '–' + hi;
            },
            inRange: { color: MATRIX_COLORS },
        },
        series: [{
            type: 'heatmap', data: cells,
            itemStyle: { borderWidth: 1, borderColor: '#1a1a2e' },
            emphasis: { itemStyle: { borderColor: '#fff', borderWidth: 1 } },
        }],
    };
    return { option, hasData: cells.length > 0, labels, labelColors,
        hiddenCount, visibleCount: labels.length, notes, cells, rawMax,
        shownVolume, hiddenVolume, totalVolume,
        serverTruncated: !!(data && data.truncated) };
}
