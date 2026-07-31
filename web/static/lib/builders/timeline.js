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
 */

import { WAIT_CLASSES, fmtTime, fmtUs, esc } from '../format.js';

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

/* data: session_timeline response { events[], pids[], truncated, total_count }
 * opts: { from, to }  (current view window, for the x-axis bounds)
 * Returns { option, hasData, chartHeight, truncated, total_count, count }. */
export function buildTimelineOption(data, opts) {
    opts = opts || {};
    const events = (data && data.events) || [];
    const pids = (data && data.pids) || [];
    if (events.length === 0) {
        return { option: null, hasData: false, chartHeight: 0,
            truncated: false, total_count: 0, count: 0 };
    }

    const pidLabels = pids.map(p => 'PID ' + p);
    const pidIndexMap = {};
    pids.forEach((p, i) => { pidIndexMap[p] = i; });
    // Clamp the drawn interval to the view window (P6): the server emits
    // start_ns = timestamp - duration with NO clamp to from_ns, so long waits
    // routinely start before the window and the bars bled left across the PID
    // axis labels. The raw start rides along at [7] for the tooltip.
    const from = opts.from, to = opts.to;
    const barData = events.map(ev => {
        const rawEnd = ev.s + ev.d;
        const s = from != null ? Math.max(ev.s, from) : ev.s;
        const e = to != null ? Math.min(rawEnd, to) : rawEnd;
        return [s, e, pidIndexMap[ev.p] || 0, ev.n, ev.c, ev.q, ev.d, ev.s];
    });

    const option = {
        backgroundColor: 'transparent',
        animation: false,
        tooltip: {
            trigger: 'item', backgroundColor: '#1e1e3a', borderColor: '#333',
            textStyle: { color: '#e0e0e0', fontSize: 12 },
            formatter: timelineTooltipFormatter,
        },
        grid: { left: 100, right: 20, top: 20, bottom: 40 },
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
            renderItem: timelineRenderItem,
            // Custom series default clip:false in this ECharts bundle — combined
            // with unclamped starts the rects painted over the axis labels (P6).
            clip: true,
            encode: { x: [0, 1], y: 2 },
            data: barData,
        }],
    };

    return {
        option, hasData: true,
        chartHeight: Math.max(200, pids.length * 50 + 80),
        truncated: !!(data && data.truncated),
        total_count: (data && data.total_count) || 0,
        count: events.length,
    };
}
