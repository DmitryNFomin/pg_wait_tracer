/* pgwt — pure builder: transitions payload -> top-N transition heatmap. */

import { eventColor, fmtMs, esc, rgbToHsl, hslToRgb } from '../format.js';

export const MATRIX_COLORS =
    ['#763e84', '#a04ab5', '#bb6ecf', '#d496e3', '#eac3f4'];

const MATRIX_BG = [0x1a, 0x1a, 0x2e];
function srgbChannel(c) {
    c /= 255;
    return c <= 0.03928 ? c / 12.92 : Math.pow((c + 0.055) / 1.055, 2.4);
}
function relLuminance(r, g, b) {
    return 0.2126 * srgbChannel(r) + 0.7152 * srgbChannel(g) + 0.0722 * srgbChannel(b);
}
function contrastRatio(l1, l2) {
    const hi = Math.max(l1, l2), lo = Math.min(l1, l2);
    return (hi + 0.05) / (lo + 0.05);
}
const MATRIX_BG_LUM = relLuminance(...MATRIX_BG);
const MATRIX_CONTRAST_FLOOR = 3.0;

/* #473 item 2: several per-event tints (IO's darker steps especially)
 * fall under 2:1 contrast against the app's #1a1a2e background --
 * effectively invisible axis labels. Lifts ONLY lightness (hue and
 * saturation untouched, so a class's identity hue is preserved) just
 * far enough to clear a 3:1 floor; a color that already clears it is
 * returned completely unchanged. */
export function contrastFloorColor(rgbString, minRatio = MATRIX_CONTRAST_FLOOR) {
    const m = /^rgb\((\d+),\s*(\d+),\s*(\d+)\)$/.exec(rgbString);
    if (!m) return rgbString;
    const r = +m[1], g = +m[2], b = +m[3];
    if (contrastRatio(relLuminance(r, g, b), MATRIX_BG_LUM) >= minRatio) return rgbString;
    const [h, s, l0] = rgbToHsl(r, g, b);
    let lo = l0, hi = 1.0;
    for (let i = 0; i < 30; i++) {
        const mid = (lo + hi) / 2;
        const [rr, gg, bb] = hslToRgb(h, s, mid);
        if (contrastRatio(relLuminance(rr, gg, bb), MATRIX_BG_LUM) >= minRatio) hi = mid;
        else lo = mid;
    }
    const [rr, gg, bb] = hslToRgb(h, s, hi);
    return 'rgb(' + rr + ',' + gg + ',' + bb + ')';
}

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

/* Legend buckets are quantized to a fixed 1-2-5-10 log grid,
 * never to the raw per-tick max. A tick's rawMax snaps UP to the nearest
 * grid point at or above it; ticks whose rawMax stays under the same grid
 * point (ordinary sampling jitter, e.g. 53,971 -> 57,811 both round up to
 * 100,000) keep byte-identical visualMap pieces, so cells never change
 * shade without a change in meaning (STABILITY, #104 / P8 "heatmap global
 * recolor per tick"). Only a rawMax that crosses into the next grid point
 * (a real order-of-magnitude change in transition volume) moves the legend. */
export function matrixLegendBucketMax(x) {
    if (!(x > 1)) return 1;
    const exp = Math.floor(Math.log10(x));
    const base = Math.pow(10, exp);
    for (const step of [1, 2, 5, 10]) {
        if (x <= step * base) return step * base;
    }
    return 10 * base;
}

/* DEFINITIONS (contract 5, issue #438 — RELEASE-PLAN.md "Transitions and
 * simplification"): CPU* is a LITERAL intermediate state the sampler
 * observed between two off-CPU waits — not necessarily measured running CPU
 * time (it can include scheduling gaps the sampler cannot distinguish from
 * CPU). A CPU-centered matrix (most rows/columns funnel through the CPU*
 * cell) is therefore the correct shape of the literal transition data for a
 * CPU-mediated workload, not a bug to be fixed by re-deriving a different
 * (inferred wait-to-wait) metric — the adviser named that scope explicitly
 * as a different product. #438's actual fix is EXPLANATION: see the CPU*
 * note appended below when it is one of the shown labels. The topology
 * itself, the top-20 selection by summed incident count, and the existing
 * omission accounting (hiddenCount/hiddenVolume/serverTruncated) are
 * unchanged.
 *
 * `limit` is clamped to 1..20. Events are ranked by total incident link
 * count (source + target), with a lexical tie-break for deterministic output
 * — this ranking decides which events make the top-N, tick to tick. Display
 * ORDER is a separate, stickier concern (STABILITY, #104): with no
 * `opts.prevLabels`, order is the rank order (first build / explicit
 * re-sort). With `opts.prevLabels` (the `labels` this builder returned last
 * tick), already-shown events keep their previous relative position; only
 * events newly entering the top-N are appended, in rank order, at the end —
 * so two events already on screen never swap rows/columns just because their
 * scores nudged past each other. Color maps log1p(count) through a piecewise
 * single-hue ramp; class identity appears only in axis-label text colors. */
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
    const topNames = allNames.slice(0, limit);
    const hiddenCount = Math.max(0, allNames.length - topNames.length);
    let labels;
    if (Array.isArray(opts.prevLabels) && opts.prevLabels.length) {
        const topSet = new Set(topNames);
        const kept = opts.prevLabels.filter(name => topSet.has(name));
        const keptSet = new Set(kept);
        const added = topNames.filter(name => !keptSet.has(name));
        labels = kept.concat(added);
    } else {
        labels = topNames;
    }
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
    const bucketMax = matrixLegendBucketMax(rawMax);
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
    // #438: explain the CPU* cell wherever it is actually shown, rather than
    // leaving a CPU-saturated-looking row/column unexplained. Conditioned on
    // `labels` (what is ON the axes right now), not on the raw node catalog,
    // so the note never appears for a window where CPU* did not make top-N.
    const cpuNote = labels.includes('CPU*')
        ? 'CPU* is a literal intermediate state between waits, not ' +
            'necessarily measured running CPU' : null;
    const option = {
        backgroundColor: 'transparent', animation: false,
        tooltip: {
            position: 'top', backgroundColor: '#1e1e3a', borderColor: '#333',
            textStyle: { color: '#e0e0e0', fontSize: 12 },
            formatter: (p) => matrixTooltipFormatter(labels, p),
        },
        /* Axis names sit clear of tick labels at 1280px: the x-axis name is
         * centered ('middle') below the rotated tick labels instead of at
         * the axis end (which clipped "Source event" to "Sour" against the
         * panel's right edge); the y-axis name is a vertical strip to the
         * LEFT of the tick-label column (nameGap pushes it past the
         * longest label) instead of at the axis end (which, with
         * `inverse: true`, landed at the origin and overstruck the first
         * x tick label "CPU*"). */
        grid: { left: 205, right: 40, top: 22, bottom: 150 },
        xAxis: {
            type: 'category', name: 'Source event', nameLocation: 'middle',
            nameGap: 95, nameTextStyle: { color: '#888' }, data: labels,
            axisLabel: { interval: 0, rotate: 48, fontSize: 10,
                color: (v) => contrastFloorColor(labelColors[v] || '#aaa') },
            axisLine: { lineStyle: { color: '#333' } }, splitArea: { show: false },
        },
        yAxis: {
            type: 'category', name: 'Target event', nameLocation: 'middle',
            nameGap: 190, nameRotate: 90, nameTextStyle: { color: '#888' },
            data: labels, inverse: true,
            axisLabel: { interval: 0, fontSize: 10,
                color: (v) => contrastFloorColor(labelColors[v] || '#aaa') },
            axisLine: { lineStyle: { color: '#333' } }, splitArea: { show: false },
        },
        visualMap: {
            type: 'piecewise', min: 0, max: Math.log1p(bucketMax), splitNumber: 5,
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
        hiddenCount, visibleCount: labels.length, notes, cpuNote, cells, rawMax, bucketMax,
        shownVolume, hiddenVolume, totalVolume,
        serverTruncated: !!(data && data.truncated) };
}
