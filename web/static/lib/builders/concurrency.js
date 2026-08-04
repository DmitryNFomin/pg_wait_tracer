/* pgwt — pure builder: concurrency data -> ECharts option + table models.
 *
 * The "peak concurrent sessions per wait event" overlay (an area line of peak
 * concurrency over time, with burst markers) plus the top-peaks and burst
 * tables below it. This builder is library-shaped only at the edge (it emits an
 * ECharts option); the data->series/markPoint/table mapping is pure and
 * Node-testable. It owns NO chart instance and touches NO DOM.
 *
 * Mapping matches the old legacy adapter in app.js (render), except the burst
 * markers (fixed in U0, see below):
 *   - line series of per-bucket peak max, area fill, x = bucket time
 *   - burst markers placed in the CONTAINING bucket (arithmetic, clamped)
 *   - top-peaks table: peaks with max>1, sorted desc, top 10
 *   - bursts table: every burst (4+ sessions within 10ms)
 */

import { fmtTime, fmtTimeMs, esc } from '../format.js';

/* data: concurrency response { peaks[], bursts[], bucket_ns }
 * Returns { option, hasData, topPeaks, bursts, bucketNs }. The HTML tables are
 * left to the view's mount (cheap string templating); the row models here are
 * the pure data the templates iterate. */
export function buildConcurrencyOption(data) {
    if (!data || !data.peaks || data.peaks.length === 0) {
        return { option: null, hasData: false, topPeaks: [], bursts: [], bucketNs: 0 };
    }
    const bns = data.bucket_ns || 0;
    const times = data.peaks.map(p => p.t);
    const peakData = data.peaks.map(p => p.max || 0);
    const peakEvents = data.peaks.map(p => p.event || '');

    const burstPoints = (data.bursts || []).map(b => {
        // Containing bucket by arithmetic (P6): peak t values are bucket
        // STARTS, so the old findIndex(p.t >= ts) was off-by-one for in-range
        // bursts (bucket AFTER the containing one) and returned -1 for any
        // burst inside the FINAL bucket, clamping the marker to bucket 0.
        const idx = bns > 0
            ? Math.floor((b.timestamp_ns - data.peaks[0].t) / bns) : 0;
        const at = Math.min(Math.max(idx, 0), data.peaks.length - 1);
        return {
            coord: [at, peakData[at] || b.sessions],
            value: b.sessions, symbol: 'triangle',
            symbolSize: Math.min(10 + b.sessions * 2, 30),
            itemStyle: { color: '#f44' },
            label: { show: true, formatter: b.sessions + '', color: '#fff', fontSize: 10 },
        };
    });

    const option = {
        animation: false,
        title: {
            text: 'Peak Concurrent Sessions per Wait Event', left: 'center',
            textStyle: { color: '#ccc', fontSize: 14 },
        },
        tooltip: {
            trigger: 'axis', axisPointer: { type: 'cross' },
            backgroundColor: '#1e1e3a', borderColor: '#333',
            textStyle: { color: '#e0e0e0', fontSize: 12 },
            formatter: (params) => {
                if (!params.length) return '';
                const idx = params[0].dataIndex;
                const ev = peakEvents[idx] || 'none';
                return fmtTime(params[0].axisValue, bns) + '<br/>' +
                    'Peak: <b>' + params[0].value + ' sessions</b><br/>Event: ' + esc(ev);
            },
        },
        grid: { left: 50, right: 20, top: 50, bottom: 40 },
        xAxis: {
            type: 'category', data: times,
            axisLabel: { color: '#888', fontSize: 10, formatter: (v) => fmtTime(v, bns) },
            axisLine: { lineStyle: { color: '#333' } },
        },
        yAxis: {
            type: 'value', name: 'Simultaneous Sessions',
            nameTextStyle: { color: '#888', fontSize: 11 }, min: 0,
            axisLabel: { color: '#888' }, splitLine: { lineStyle: { color: '#2a2a4a' } },
        },
        series: [{
            type: 'line', data: peakData,
            areaStyle: { color: 'rgba(248,170,170,0.15)' },
            lineStyle: { color: '#f8a', width: 2 }, itemStyle: { color: '#f8a' },
            symbol: 'none',
            markPoint: burstPoints.length > 0 ? { data: burstPoints } : undefined,
        }],
    };

    const topPeaks = data.peaks.filter(p => p.max > 1)
        .sort((a, b) => b.max - a.max).slice(0, 10);

    return { option, hasData: true, topPeaks, bursts: data.bursts || [], bucketNs: bns };
}

/* P3 wire 2: the row's zoom-intent attributes. Bursts are 4+ sessions inside
 * 10ms — at a 15-min window drag-select resolves ~1s/pixel, so the moment is
 * genuinely unreachable by gesture; the row IS the drill. The target window
 * is ts ± 5 buckets (the burst centered with context on both sides); the ns
 * values ride as data attributes the view feeds straight to ctx.onZoom.
 * cursor/title are the affordance (rows also carry .row-zoom for styling). */
function zoomAttrs(tsNs, bucketNs) {
    const pad = 5 * (bucketNs || 0);
    if (!(pad > 0) || !(tsNs > 0)) return '';
    return ' class="row-zoom" data-from="' + (tsNs - pad) + '" data-to="' +
        (tsNs + pad) + '" style="cursor:pointer"' +
        ' title="Zoom to ±5 buckets around this moment"';
}

/* PURE: the two HTML tables under the chart. Exported for testing — same
 * content as the old adapter's #burst-table, plus the wire-2 zoom attributes
 * on every peak/burst row. */
export function buildConcurrencyTables(model) {
    let html = '<h3 style="color:#ccc;margin:10px 0 5px">Top Peak Moments</h3>' +
        '<table class="data-table"><thead><tr>' +
        '<th>Time</th><th>Wait Event</th><th>Simultaneous Sessions</th></tr></thead><tbody>';
    model.topPeaks.forEach(p => {
        html += '<tr' + zoomAttrs(p.t, model.bucketNs) + '><td>' + fmtTimeMs(p.t_ms) +
            '</td><td>' + esc(p.event || 'CPU*') +
            '</td><td><b>' + p.max + '</b></td></tr>';
    });
    html += '</tbody></table>';

    if (model.bursts.length > 0) {
        html += '<h3 style="color:#ccc;margin:15px 0 5px">Burst Events ' +
            '<span style="color:#666;font-size:12px">(4+ sessions within 10ms)</span></h3>' +
            '<table class="data-table"><thead><tr>' +
            '<th>Time</th><th>Wait Event</th><th>Sessions</th><th>PIDs</th></tr></thead><tbody>';
        model.bursts.forEach(b => {
            const time = fmtTimeMs(b.timestamp_ms);
            const pids = (b.pids || []).slice(0, 8).join(', ') +
                (b.pids && b.pids.length > 8 ? '...' : '');
            html += '<tr' + zoomAttrs(b.timestamp_ns, model.bucketNs) + '><td>' + time +
                '</td><td>' + esc(b.event) + '</td><td><b>' +
                b.sessions + '</b></td><td>' + pids + '</td></tr>';
        });
        html += '</tbody></table>';
    } else {
        html += '<p style="color:#666;margin-top:15px">No burst events detected</p>';
    }
    return html;
}
