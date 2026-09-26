/* pgwt — pure builder: session_timeline data -> ECharts option.
 *
 * The per-session wait timeline (Gantt-style bars, one row per PID, each wait a
 * colored bar). This builder is library-shaped only at the edge (it emits an
 * ECharts custom-series option); the data->bar mapping is pure and Node-testable.
 * It owns NO chart instance and touches NO DOM.
 *
 * Mapping matches the old legacy adapter in app.js (renderTimeline), except the
 * drawn interval is clamped to the view window (fixed in U0, see buildTimelineOption):
 *   - one y-category per PID ("PID <n>")
 *   - bar data = [startNs, endNs, pidIdx, name, classIdx, query, durNs, rawStartNs]
 *     (start/end clamped to the window; the raw start rides along for the tooltip)
 *   - custom renderItem draws a class-colored rect per wait (60% of band height)
 *   - x-axis spans the current view window [from, to]
 *
 * DENSITY (issue #106). Above TIMELINE_AGG_SPANS_PER_PX spans per pixel column
 * the per-span mapping above is abandoned for a per-pixel-column aggregation —
 * see aggregateTimelineColumns / timelineAggRenderItem below. Drawing 3000-5000
 * individual rects across ~1100 px fused them into one solid block (OCCLUSION)
 * whose alpha-composited pixels rendered rgb(181,199,48), a yellow-green that
 * belongs to NO wait class (SEMANTICS: the pixels lied about the class), and
 * cost 1.8-3.0 s per live tick where every other tab lands at ~1.21 s (#100).
 */

import { WAIT_CLASSES, fmtTime, fmtUs, esc } from '../format.js';

/* Aggregate when a PID row carries MORE than this many spans per pixel column.
 * At exactly 1/px every span can still own a pixel; past 2/px over-paint is
 * arithmetically guaranteed, so individual rects can only lie. Exported so the
 * threshold boundary is pinned by a test, not by a magic number. */
export const TIMELINE_AGG_SPANS_PER_PX = 2;

/* ...on at least this many painted columns of that row, AND only once the row
 * carries TIMELINE_AGG_MIN_SPANS spans at all.
 *
 * The mean spans-per-painted-column alone is not enough, because a mean is
 * dragged over the threshold by one bad pixel: a row of 50 waits stacked in
 * ONE pixel plus 20 legible spread bars means 70/21 = 3.3 per painted column,
 * and aggregating the whole row on that basis would take identity away from
 * 20 bars that master drew perfectly well — a regression for that shape.
 * Requiring the over-dense condition on a RUN of columns describes the thing
 * the fix is for: a BLOCK of fused pixels, not a spike. 16 columns is a block
 * you can see; one is a bar.
 *
 * Note this also subsumes TIMELINE_AGG_MIN_SPANS (16 columns of >2 spans is
 * at least 48 spans). The span floor is kept as an explicit, independently
 * meaningful statement — it is the guard that still holds if #123's per-row
 * aggregation lowers the column requirement. */
export const TIMELINE_AGG_MIN_DENSE_COLS = 16;

/* Density may not fire on a row below this many spans at all: a handful of
 * legible waits sharing one busy pixel column can already exceed
 * TIMELINE_AGG_SPANS_PER_PX, yet trading that chart's per-wait identity (this
 * wait, this duration, this query) for one summarised pixel is a bad deal
 * until the row is genuinely dense. (Before #121, waits that ended before the
 * window clamped to the same left-edge pixel and could fake this density —
 * such waits are now dropped by the builder before aggregation ever sees
 * them, see buildTimelineOption.) */
export const TIMELINE_AGG_MIN_SPANS = 32;

/* Fallback host width (px) when the view cannot measure one (first paint,
 * gallery/Node). The app's timeline panel is ~1240 px at the pinned viewport. */
export const TIMELINE_DEFAULT_WIDTH = 1200;

/* Kept in one place because the plot width (and therefore the pixel-column
 * count) is the host width MINUS this grid's horizontal margins. */
export const TIMELINE_GRID = { left: 100, right: 20, top: 20, bottom: 40 };

/* Plottable width in px for a given host width — i.e. the number of pixel
 * columns the aggregation buckets into. */
export function timelinePlotWidth(width) {
    const w = Number(width);
    const host = Number.isFinite(w) && w > 0 ? w : TIMELINE_DEFAULT_WIDTH;
    return Math.max(1, Math.round(host - TIMELINE_GRID.left - TIMELINE_GRID.right));
}

/* Pure renderItem for the custom timeline series. Hoisted so the emitted option
 * is a plain (serializable-shaped) structure and the function is testable. */
export function timelineRenderItem(params, api) {
    const startVal = api.value(0), endVal = api.value(1);
    const catIdx = api.value(2), classIdx = api.value(4);
    const start = api.coord([startVal, catIdx]);
    const end = api.coord([endVal, catIdx]);
    const bandWidth = api.size([0, 1])[1];
    const color = WAIT_CLASSES[classIdx] ? WAIT_CLASSES[classIdx].color : '#888';
    const rectHeight = bandWidth * 0.6;
    const width = Math.max(end[0] - start[0], 1);
    return {
        type: 'rect',
        shape: { x: start[0], y: start[1] - rectHeight / 2, width, height: rectHeight },
        style: { fill: color },
        styleEmphasis: { fill: color, opacity: 0.8, lineWidth: 1, stroke: '#fff' },
    };
}

/* Pure tooltip renderer (exported for testing). The query text (d[5]) comes
 * from arbitrary user SQL and MUST be escaped — it is injected into the DBA's
 * browser otherwise (UI-6). Start shows the RAW start (d[7]), not the drawn
 * start (d[0]) — d[0] is clamped to the window and analysts must see when the
 * wait truly began. */
export function timelineTooltipFormatter(params) {
    const d = params.data;
    let s = '<b>' + esc(d[3]) + '</b><br>';
    s += 'Duration: <b>' + fmtUs(d[6] / 1000) + '</b><br>';
    s += 'Start: ' + fmtTime(d[7]) + '<br>';
    if (d[5] && d[5] !== '0') s += 'Query: ' + esc(d[5]);
    return s;
}

// ── Density aggregation (#106) ───────────────────────────────────────────────
//
// One pixel column of one PID row is summarised as a STACK of class-colored
// segments whose heights are that class's share of the WAIT TIME inside the
// column. Why stacked-by-share and not "paint the column in its dominant
// class's color" (the issue offered both):
//
//   SEMANTICS — the checklist's rule is "a pixel proportion that means share of
//     time must actually be that share". A 57 % Timeout / 43 % CPU column
//     becomes 57 % orange over 43 % green; dominant-only would erase the CPU
//     43 % entirely and claim a pure-Timeout pixel. Every fill is a literal
//     WAIT_CLASSES color, so no pixel can ever show a composited hue again.
//   STABILITY — segments stack in canonical WAIT_CLASSES order, so a class
//     keeps BOTH its hue and its slot in the stack across ticks, and a class
//     entering or leaving a column does not reshuffle the others. Under
//     dominant-only a 50.1/49.9 column would flip its entire color on a 0.2 %
//     data change; here that moves one boundary by a sub-pixel.
//   HIERARCHY — the dominant class still owns most of the column's height, so
//     dominance reads at a glance without erasing the minority.
//
// Segment heights are EXACT proportions: no min-height clamp, because clamping
// silently renormalizes the rest of the stack (P11 — "min-width clamps lie").
// A class below ~1/bandHeight share therefore renders sub-pixel; that is the
// honest rendering of a sub-1 % share, and the tooltip carries the numbers.
//
// The window is bucketed ONCE per build; a span contributes to every column it
// overlaps, weighted by its ns of overlap with that column, so a wide wait
// paints its whole extent instead of only its start pixel.

const NCLASS = WAIT_CLASSES.length;
const UNKNOWN_CLASS = NCLASS - 1;   // WAIT_CLASSES[10] = "Unknown"

/* Aggregate clamped bar tuples into per-row, per-pixel-column class stacks.
 *
 * bars: [startNs, endNs, pidIdx, name, classIdx, query, durNs, rawStartNs][]
 *       (start/end already clamped to [from, to])
 * Returns { segments, occupiedColumns, columnSpanTotal, maxColumnSpans,
 * triggerDensity }, where triggerDensity is the spans per PAINTED pixel column
 * of the busiest row that QUALIFIES for aggregation (enough spans, and enough
 * over-dense columns to be a block — 0 when no row qualifies), and each
 * segment is
 *   [colStartNs, colEndNs, pidIdx, classIdx, frac0, frac1,
 *    classSpans, columnSpans, classWaitNs, columnWaitNs, columnBreakdown]
 * frac0/frac1 are cumulative bottom-up fractions of the band rect in [0,1];
 * columnBreakdown is the whole column's [classIdx, spans, waitNs][] in
 * canonical class order, shared by reference between that column's segments. */
export function aggregateTimelineColumns(bars, rows, cols, from, to) {
    const span = to - from;
    const segments = [];
    if (!(span > 0) || !(cols > 0) || !(rows > 0)) {
        return { segments, occupiedColumns: 0, columnSpanTotal: 0,
            maxColumnSpans: 0, triggerDensity: 0 };
    }
    const colNs = span / cols;
    // Bucket ROW BY ROW into one reused (cols x class) pair of buffers: a
    // rows*cols*11 allocation would be tens of MB per tick on a many-PID
    // window, and this is on the live-refresh path.
    const byRow = new Array(rows);
    for (let i = 0; i < bars.length; i++) {
        const b = bars[i];
        // A wait that STARTS after the window never paints: the per-span path
        // hands the rect an x past the plot and clip:true drops it. Counting
        // it here would both tint the last column and inflate the density.
        if (b[0] > to) continue;
        // Defensive (#121): the builder drops any wait with no overlap with
        // the window before it ever reaches here, so an inverted bar
        // (end < start — the P6 shape of a wait that ended before `from`)
        // should never arrive. If one does anyway, it describes nothing real
        // on the timeline and must not be counted or painted.
        if (b[1] < b[0]) continue;
        const row = b[2] >= 0 && b[2] < rows ? b[2] : 0;
        if (byRow[row]) byRow[row].push(b); else byRow[row] = [b];
    }
    const wait = new Float64Array(cols * NCLASS);   // ns of wait per (col,class)
    const hits = new Uint32Array(cols * NCLASS);    // spans touching (col,class)

    let occupiedColumns = 0, columnSpanTotal = 0, maxColumnSpans = 0;
    let triggerDensity = 0;
    for (let r = 0; r < rows; r++) {
        const rowBars = byRow[r];
        if (!rowBars) continue;
        let rowOccupied = 0, rowSpanTotal = 0, rowDenseCols = 0;
        wait.fill(0);
        hits.fill(0);
        for (let i = 0; i < rowBars.length; i++) {
            const b = rowBars[i];
            // An out-of-range class becomes the real "Unknown" class rather
            // than the per-span renderer's #888: an aggregated column must
            // only ever be painted in palette colors.
            const cls = b[4] >= 0 && b[4] < NCLASS ? b[4] : UNKNOWN_CLASS;
            let c0 = Math.floor((b[0] - from) / colNs);
            // A span that ENDS exactly on a column boundary stops at the
            // previous column — otherwise every wait would tint one pixel it
            // never covers.
            let c1 = Math.ceil((b[1] - from) / colNs) - 1;
            if (c0 < 0) c0 = 0; if (c0 > cols - 1) c0 = cols - 1;
            if (c1 < c0) c1 = c0; if (c1 > cols - 1) c1 = cols - 1;
            for (let c = c0; c <= c1; c++) {
                const cs = from + colNs * c;
                let overlap = Math.min(b[1], cs + colNs) - Math.max(b[0], cs);
                // A zero-length (or sub-ns) wait happened, so it must be
                // PRESENT in its column's counts and share — a nominal 1 ns of
                // weight puts it there. This buys it no pixels: with a real
                // wait beside it, its share (and so its slice of the band) is
                // the sub-pixel sliver the numbers say it is.
                if (!(overlap > 0)) overlap = 1;
                const idx = c * NCLASS + cls;
                wait[idx] += overlap;
                hits[idx] += 1;
            }
        }
        for (let c = 0; c < cols; c++) {
            const base = c * NCLASS;
            let total = 0, count = 0, last = -1;
            for (let k = 0; k < NCLASS; k++) {
                if (hits[base + k] === 0) continue;
                total += wait[base + k];
                count += hits[base + k];
                last = k;
            }
            if (count === 0) continue;
            occupiedColumns++;
            columnSpanTotal += count;
            rowOccupied++;
            rowSpanTotal += count;
            if (count > TIMELINE_AGG_SPANS_PER_PX) rowDenseCols++;
            if (count > maxColumnSpans) maxColumnSpans = count;
            const x0 = from + colNs * c;
            const x1 = x0 + colNs;
            // ONE breakdown per column, shared by reference with every segment
            // of that column: the tooltip lists the whole column whichever
            // slice the pointer actually hit. A 1 % class is a sub-pixel hit
            // target, so it must be readable from its neighbours' pixels.
            const breakdown = [];
            let acc = 0;
            for (let k = 0; k < NCLASS; k++) {
                if (hits[base + k] === 0) continue;
                const w = wait[base + k];
                // The topmost present class closes the stack at exactly 1 so
                // float drift can never leave a hairline gap at the band top.
                const f1 = k === last ? 1 : acc + (total > 0 ? w / total : 0);
                segments.push([x0, x1, r, k, acc, f1,
                    hits[base + k], count, w, total, breakdown]);
                breakdown.push([k, hits[base + k], w]);
                acc = f1;
            }
        }
        // A row only gets a vote once it is big enough to aggregate
        // (TIMELINE_AGG_MIN_SPANS) and its over-dense pixels form a BLOCK
        // rather than a spike (TIMELINE_AGG_MIN_DENSE_COLS) — see the
        // constants. Only qualifying rows contribute the density that decides
        // the chart, so one stacked pixel can never drag a legible row in.
        if (rowOccupied && rowBars.length >= TIMELINE_AGG_MIN_SPANS &&
                rowDenseCols >= TIMELINE_AGG_MIN_DENSE_COLS) {
            const d = rowSpanTotal / rowOccupied;
            if (d > triggerDensity) triggerDensity = d;
        }
    }
    return { segments, occupiedColumns, columnSpanTotal, maxColumnSpans,
        triggerDensity };
}

/* renderItem for an aggregated column stack. Same 60%-of-band rect as the
 * per-span renderer, sliced vertically by class share (frac0 = bottom). */
export function timelineAggRenderItem(params, api) {
    const catIdx = api.value(2);
    const start = api.coord([api.value(0), catIdx]);
    const end = api.coord([api.value(1), catIdx]);
    const bandWidth = api.size([0, 1])[1];
    const rectHeight = bandWidth * 0.6;
    const classIdx = api.value(3);
    const color = WAIT_CLASSES[classIdx]
        ? WAIT_CLASSES[classIdx].color : WAIT_CLASSES[UNKNOWN_CLASS].color;
    const f0 = api.value(4), f1 = api.value(5);
    const top = start[1] - rectHeight / 2;
    return {
        type: 'rect',
        shape: {
            x: start[0],
            // y grows downward; frac 0 is the BOTTOM of the band rect.
            y: top + rectHeight * (1 - f1),
            width: Math.max(end[0] - start[0], 1),
            // No min-height clamp on purpose (P11): clamping renormalizes the
            // rest of the stack and the proportions stop meaning share.
            height: rectHeight * (f1 - f0),
        },
        style: { fill: color },
        styleEmphasis: { fill: color, opacity: 0.8, lineWidth: 1, stroke: '#fff' },
    };
}

/* Tooltip for an aggregated column. Hovering ANY slice describes the WHOLE
 * column — every class present, with its share, wait time and span count, in
 * the same canonical order the stack is drawn in, with the hovered class
 * marked. A 1 % class is a sub-pixel hit target, so the only way to read it is
 * from its neighbours' pixels. Carries NO query text: a column mixes many
 * executions, so there is nothing to attribute — which also keeps the
 * untrusted-SQL surface (UI-6) out of this path entirely. */
export function timelineAggTooltipFormatter(params) {
    const d = params.data;
    const total = d[9];
    const rows = d[10] && d[10].length ? d[10] : [[d[3], d[6], d[8]]];
    let s = '<b>Aggregated column</b> · ' + fmtTime(d[0], 1_000_000) +
        ' – ' + fmtTime(d[1], 1_000_000) + '<br>';
    s += d[7] + ' span' + (d[7] === 1 ? '' : 's') + ', ' +
        fmtUs(total / 1000) + ' of wait<br>';
    for (let i = 0; i < rows.length; i++) {
        const k = rows[i][0], spans = rows[i][1], w = rows[i][2];
        const cls = WAIT_CLASSES[k] || WAIT_CLASSES[UNKNOWN_CLASS];
        const pct = total > 0 ? (w / total) * 100 : 0;
        const hovered = k === d[3];
        s += '<span style="display:inline-block;width:8px;height:8px;' +
            'background:' + cls.color + ';margin-right:6px"></span>';
        s += (hovered ? '<b>' : '') + esc(cls.label) + (hovered ? '</b>' : '');
        s += ' ' + pct.toFixed(1) + '% · ' + fmtUs(w / 1000) + ' · ' +
            spans + ' span' + (spans === 1 ? '' : 's') + '<br>';
    }
    s += 'Zoom in for individual waits and their queries';
    return s;
}

/* The banner: ONE line, one number format, one zoom instruction, covering
 * truncation and density together (they are different facts, but two yellow
 * lines above one chart read as two problems). Pure so the text is
 * unit-tested; null when there is nothing to say.
 *
 * COUPLED TEXT: tests/ui_live_smoke.py detects the aggregated render path by
 * the substring "aggregated at " in this line (it reads the painted banner —
 * the series length alone cannot tell the two paths apart). That substring is
 * load-bearing: reword the rest of the sentence freely, but keep it, or update
 * the smoke's check in the same commit. */
export function timelineBannerNote(count, totalCount, truncated, spansPerPx) {
    const shown = count.toLocaleString();
    const of = truncated && totalCount > count
        ? ' of ' + totalCount.toLocaleString() : '';
    if (spansPerPx) {
        return 'Showing ' + shown + of + ' events, aggregated at ' +
            spansPerPx.toLocaleString() + ' per px into per-pixel class shares' +
            ' — individual waits and their queries appear when you drag to zoom in.';
    }
    if (truncated) {
        return 'Showing ' + shown + of +
            ' events — drag to zoom in for the rest of the window.';
    }
    return null;
}

/* data: session_timeline response { events[], pids[], truncated, total_count }
 * opts: { from, to, width }  (view window for the x-axis bounds; width = the
 *       chart host's pixel width, used to size the density aggregation)
 * Returns { option, hasData, chartHeight, truncated, total_count, count,
 *           aggregated, bannerNote, spansPerPx, columns, segmentCount }.
 * bannerNote is the single banner line (truncation and density in one
 * sentence), null when there is nothing to say. */
export function buildTimelineOption(data, opts) {
    opts = opts || {};
    const events = (data && data.events) || [];
    const pids = (data && data.pids) || [];
    if (events.length === 0) {
        return { option: null, hasData: false, chartHeight: 0,
            truncated: false, total_count: 0, count: 0,
            aggregated: false, bannerNote: null, spansPerPx: 0,
            columns: 0, segmentCount: 0 };
    }

    const pidLabels = pids.map(p => 'PID ' + p);
    const pidIndexMap = {};
    pids.forEach((p, i) => { pidIndexMap[p] = i; });
    // Clamp the drawn interval to the view window (P6): the server emits
    // start_ns = timestamp - duration with NO clamp to from_ns, so long waits
    // routinely start before the window and the bars bled left across the PID
    // axis labels. The raw start rides along at [7] for the tooltip.
    //
    // A wait with NO overlap with the window at all — it ended before `from`,
    // or started at/after `to` — is dropped rather than clamped (#121): naive
    // clamping (start=max(s,from), end=min(rawEnd,to)) on such a wait yields
    // start > end, an inverted interval that renders as a phantom zero/negative
    // -width rect pinned to whichever edge is nearer. There is nothing true to
    // draw for a wait the window never touched, so it is excluded from the
    // drawn set entirely — never emitted as start>end.
    const from = opts.from, to = opts.to;
    const barData = [];
    for (let i = 0; i < events.length; i++) {
        const ev = events[i];
        const rawEnd = ev.s + ev.d;
        if (from != null && rawEnd <= from) continue;   // ended before the window
        if (to != null && ev.s >= to) continue;          // starts at/after the window
        const s = from != null ? Math.max(ev.s, from) : ev.s;
        const e = to != null ? Math.min(rawEnd, to) : rawEnd;
        barData.push([s, e, pidIndexMap[ev.p] || 0, ev.n, ev.c, ev.q, ev.d, ev.s]);
    }

    // Density decision (#106). Measured on the bucketing itself, as spans per
    // PAINTED pixel column of the busiest ROW — never events/plotWidth:
    //   - the row is the unit that fuses (3000 spans across 50 rows is 8 bars a
    //     row; 3000 on ONE row is a solid block);
    //   - the painted column is the unit that occludes. The live smoke's real
    //     shape is 519 spans packed into ~185 px at the right edge of an
    //     otherwise empty 1100 px window: 0.5 spans per plot pixel, but 2.8
    //     per PAINTED pixel — an averaged trigger would leave that block fused;
    //   - and the row must carry TIMELINE_AGG_MIN_SPANS spans before density
    //     can fire at all, so a handful of window-clamped waits never loses
    //     its tooltips.
    // The bucketing pass is ~0.1 ms on 5000 spans, so it always runs and the
    // decision uses its real numbers instead of an estimate. The switch is
    // all-or-nothing across rows, so one banner describes the whole chart.
    const rows = Math.max(pidLabels.length, 1);
    const cols = timelinePlotWidth(opts.width);
    const bucketable = from != null && to != null && to > from;
    const agg = bucketable ? aggregateTimelineColumns(barData, rows, cols, from, to) : null;
    const density = agg ? agg.triggerDensity : 0;
    const aggregated = density > TIMELINE_AGG_SPANS_PER_PX;

    const spansPerPx = aggregated ? Math.max(1, Math.round(density)) : 0;
    const truncated = !!(data && data.truncated);
    const total_count = (data && data.total_count) || 0;
    // ONE banner line for both facts (truncation and density).
    const bannerNote = timelineBannerNote(events.length, total_count, truncated,
        spansPerPx);

    const option = {
        backgroundColor: 'transparent',
        animation: false,
        tooltip: {
            trigger: 'item', backgroundColor: '#1e1e3a', borderColor: '#333',
            textStyle: { color: '#e0e0e0', fontSize: 12 },
            formatter: aggregated ? timelineAggTooltipFormatter : timelineTooltipFormatter,
        },
        grid: { ...TIMELINE_GRID },
        xAxis: {
            type: 'value', min: opts.from, max: opts.to,
            axisLabel: { color: '#888', fontSize: 10, formatter: (v) => fmtTime(v) },
            axisLine: { lineStyle: { color: '#333' } },
        },
        yAxis: {
            type: 'category', data: pidLabels,
            axisLabel: { color: '#aaa', fontSize: 11 },
            axisLine: { lineStyle: { color: '#333' } },
        },
        series: [{
            type: 'custom',
            renderItem: aggregated ? timelineAggRenderItem : timelineRenderItem,
            // Custom series default clip:false in this ECharts bundle — combined
            // with unclamped starts the rects painted over the axis labels (P6).
            clip: true,
            encode: { x: [0, 1], y: 2 },
            data: aggregated ? agg.segments : barData,
        }],
    };

    return {
        option, hasData: true,
        chartHeight: Math.max(200, pids.length * 50 + 80),
        truncated, total_count,
        count: events.length,
        aggregated, bannerNote, spansPerPx,
        columns: aggregated ? cols : 0,
        segmentCount: aggregated ? agg.segments.length : barData.length,
    };
}
