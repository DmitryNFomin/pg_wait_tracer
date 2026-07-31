/* pgwt — pure builder: heatmap data -> ECharts option.
 *
 * The histogram view's latency-over-time heatmap. Buckets on x (time) and
 * latency-band on y, color = event count. This builder is library-shaped only
 * at the edge (it emits an ECharts option); the data->cell mapping is pure and
 * Node-testable. It owns NO chart instance and touches NO DOM — the view's
 * mount() feeds the option to its ECharts instance.
 *
 * Mapping is byte-identical to the old legacy adapter in app.js (renderHeatmap):
 *   - x category labels = formatted bucket times (fmtTime with bucket_ns)
 *   - y category labels = the server-supplied latency-band labels
 *   - series data = [timeIdx, latIdx, count] triples
 *   - visualMap min 0 / max = max_count, with the original 6-stop color ramp
 */

import { fmtTime, esc } from '../format.js';

const HEATMAP_COLORS =
    ['#1a5276', '#2196F3', '#4CAF50', '#FFEB3B', '#FF9800', '#F44336'];

/* data: heatmap response { bucket_ns, max_count, times[], labels[], cells[] }
 * Returns { option, hasData }. hasData is false for an empty/absent grid so the
 * view can paint its "No data" placeholder instead of an empty chart. */
export function buildHeatmapOption(data) {
    if (!data || !data.cells || data.cells.length === 0) {
        return { option: null, hasData: false };
    }
    const hbns = data.bucket_ns || 0;
    const timeLabels = (data.times || []).map(t => fmtTime(t, hbns));
    const latLabels = data.labels || [];
    const hmData = data.cells.map(c => [c[0], c[1], c[2]]);

    const option = {
        backgroundColor: 'transparent',
        animation: false,
        tooltip: {
            position: 'top', backgroundColor: '#1e1e3a', borderColor: '#333',
            textStyle: { color: '#e0e0e0', fontSize: 12 },
            formatter: (p) => '<b>' + esc(timeLabels[p.data[0]]) + '</b><br>' +
                'Latency: ' + esc(latLabels[p.data[1]]) + '<br>' +
                'Count: <b>' + p.data[2].toLocaleString() + '</b>',
        },
        grid: { left: 90, right: 40, top: 10, bottom: 60 },
        xAxis: {
            type: 'category', data: timeLabels,
            axisLabel: { color: '#888', fontSize: 10 },
            axisLine: { lineStyle: { color: '#333' } }, splitArea: { show: false },
        },
        yAxis: {
            type: 'category', data: latLabels,
            axisLabel: { color: '#888', fontSize: 10 },
            axisLine: { lineStyle: { color: '#333' } }, splitArea: { show: false },
        },
        visualMap: {
            min: 0, max: data.max_count || 1, calculable: false,
            orient: 'horizontal', left: 'center', bottom: 0,
            itemWidth: 12, itemHeight: 120,
            textStyle: { color: '#888', fontSize: 10 },
            inRange: { color: HEATMAP_COLORS },
        },
        series: [{
            type: 'heatmap', data: hmData,
            emphasis: { itemStyle: { borderColor: '#fff', borderWidth: 1 } },
            itemStyle: { borderWidth: 0 },
        }],
    };
    return { option, hasData: true };
}

/* PURE: derive the class/event selector model from a top_events row list.
 *
 * Returns { classes: [string], eventsByClass: { class: [{event_id,label}] },
 *   allEvents: [{event_id,label}] }. Mirrors the old populateSelectors /
 *   populateEventSelect logic (distinct classes, up to 50 events, name+count
 *   label) without touching the DOM. */
export function buildSelectorModel(events, fmtCount) {
    const evs = events || [];
    const classes = [];
    const seen = new Set();
    for (const r of evs) {
        if (r.class && !seen.has(r.class)) { seen.add(r.class); classes.push(r.class); }
    }
    const label = (ev) => ev.name + ' (' + fmtCount(ev.count) + ')';
    const toOpt = (ev) => ({ event_id: ev.event_id, label: label(ev) });
    const allEvents = evs.slice(0, 50).map(toOpt);
    const eventsByClass = {};
    for (const cls of classes) {
        eventsByClass[cls] = evs.filter(r => r.class === cls).slice(0, 50).map(toOpt);
    }
    return { classes, eventsByClass, allEvents };
}
