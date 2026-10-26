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
/* #438/#591: a reported link count corroborates `truncated: false`. Absent
 * (undefined/null) is "not reported", which `truncated: false` already
 * covers on its own; PRESENT means it must be a finite number equal to the
 * number of links that actually arrived. A present-but-unparseable count is
 * a refusal, not a pass -- NaN comparisons are false either way, but saying
 * so explicitly keeps the next reader from "simplifying" it into a loose
 * coercion that approves. */
function linkCountAgrees(reported, arrived) {
    if (reported === undefined || reported === null) return true;
    return typeof reported === 'number' && Number.isFinite(reported) &&
        reported === arrived;
}

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
    //
    // #438 (owner decision): when CPU* is incident to EVERY link in the
    // full response -- not just the shown top-N labels, which could make
    // this true by truncation alone -- the matrix is the "degenerate L"
    // the issue names: only the CPU* row/column carry values because this
    // workload never has a direct wait -> wait transition, only
    // wait -> CPU -> wait. State that fact explicitly rather than leaving
    // an empty-looking grid to read as broken; keep the literal
    // transitions unchanged (no wait -> wait edges are synthesized).
    //
    // #438 follow-up (#591): the sentence is a UNIVERSAL claim, so it may
    // only be made from a COMPLETE link set -- and `links` is the
    // SERVER-CAPPED set. emit_transitions_response() in src/server.c fills
    // `links` with `shown` entries and emits
    // `truncated = shown < total_links`; the payload this very snapshot
    // bakes (tests/mock_server.py's canned transitions reply) is 5 of 8,
    // truncated, with every RETURNED link incident to CPU*. Asserting
    // "every transition" there examined 5 links and ignored 3, and a capped
    // direct wait -> wait edge would have made the sentence the exact
    // opposite of the truth.
    //
    // SUPPRESSED rather than weakened, deliberately. The weakened form
    // ("...across the links the server returned") is read in ISOLATION: the
    // cap count lives in `notes`, which views/matrix.js renders BELOW the
    // canvas -- below the fold at the demo viewport, which is the whole
    // reason #473-1 moved this sentence above it in the first place. A
    // reader would get a universal-sounding claim with no visible way to
    // learn it covers 5 of 8. Nothing is lost by dropping it: the base
    // explanation still removes the #438 misreading (that the matrix
    // measures running CPU), and the link-cap footer already explains why
    // the grid is sparse. A disclosure that might be false is worse than one
    // that is merely absent.
    //
    // Completeness is established POSITIVELY, never inferred from a missing
    // field: the real server emits `truncated` unconditionally, so a payload
    // without it is one whose provenance cannot be seen, and a check that
    // cannot see must refuse. Counts that contradict the flag (declaring
    // more links than arrived, or disagreeing with the array that did)
    // refuse too -- that is a payload something trimmed after the flag was
    // computed.
    const linkSetComplete = !!data && data.truncated === false &&
        linkCountAgrees(data.link_count, links.length) &&
        linkCountAgrees(data.total_link_count, links.length);
    const allLinksTouchCpu = linkSetComplete && links.length > 0 &&
        links.every(l => l.source === 'CPU*' || l.target === 'CPU*');
    const cpuNote = labels.includes('CPU*')
        ? 'CPU* is a literal intermediate state between waits, not ' +
            'necessarily measured running CPU' +
            (allLinksTouchCpu
                ? '. Every transition in this workload passes through CPU* ' +
                    '(wait -> CPU -> wait, never wait -> wait) -- the L shape is ' +
                    'the data, not a rendering bug.'
                : '')
        : null;
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

/* #473 item 1 (live-walk evidence, frames 2591-2824 of
 * tests/results/demo_walk/mac-walk1-live-8112c71 at the demo viewport
 * 1710x981): #matrix-chart has a flat CSS height of 660px (style.css)
 * regardless of how much vertical room is actually left under the title,
 * cpu-note and tab bar. At that viewport the chrome above the chart leaves
 * roughly 430px, so the chart's own bottom -- where the rotated x-axis tick
 * labels (grid.bottom: 150) and the visualMap legend live -- renders past
 * y=981 and is never on screen (the full-page snapshot gallery never shows
 * this: test_web_ui_snapshots.py captures `full_page=True`, which is
 * exactly why this escaped the gallery for four PR gates, same mechanism
 * as #252).
 *
 * Same engineering shape as waterfallChartFitHeight (#462): shrink the
 * chart from its CSS natural height only by however much it would
 * otherwise overflow the viewport, plus a fold reserve, never below a
 * floor that still leaves a usable (if cramped) grid. A page with room to
 * spare (overflow <= 0) returns the natural height unchanged -- no
 * snapshot-gallery cell moves, since the gallery's full-page capture never
 * has a viewport to overflow. */
export const MATRIX_CHART_NATURAL_PX = 660;
export const MATRIX_CHART_MIN_PX = 360;
export const MATRIX_FOLD_RESERVE_PX = 16;

export function matrixChartFitHeight(naturalHeight, minimumHeight, overflow) {
    return Math.max(minimumHeight,
        naturalHeight - Math.max(0, Math.ceil(overflow) + MATRIX_FOLD_RESERVE_PX));
}
