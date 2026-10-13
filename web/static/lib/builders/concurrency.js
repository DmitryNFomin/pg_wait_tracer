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
 *   - burst markers placed in the CONTAINING bucket (arithmetic, clamped),
 *     drawn as an unlabelled annotation ABOVE the line (#384 — see
 *     burstPoints: the marker used to print its own, different count on top
 *     of the line's peak)
 *   - top-peaks table: peaks with max>1, sorted desc, top 10 — the SAME
 *     series the line plots, which is why the two are pinned against each
 *     other in tests/web_unit/concurrency_panels.test.mjs
 *   - bursts table: every burst (4+ sessions within 10ms)
 */

import { fmtTime, fmtTimeMs, esc } from '../format.js';

/* #105: a bucket that starts before capture began has no real sample — the
 * old code fell through to `p.max || 0` and painted a solid "0 sessions"
 * line across it, indistinguishable from a bucket that WAS captured and
 * genuinely idle (FEEDBACK: idle-but-captured != no data). Subtle, not loud:
 * a quiet gray wash + dashed edge, distinct from the fidelity/escalation
 * bands (those mean "captured but estimated"; this means "not captured at
 * all"). */
export const NOT_CAPTURED_BAND_COLOR = 'rgba(136, 136, 136, 0.10)';
export const NOT_CAPTURED_BORDER = 'rgba(136, 136, 136, 0.45)';

/* #384: the panel shows TWO measures and used to name neither. The line is
 * peak simultaneous sessions per bucket (and the "Top Peak Moments" table is
 * that same series, ranked); the triangles are burst ONSETS, a count of pids
 * entering one wait event inside 10 ms — a different question over a 10 ms
 * window, not the bucket. Both sentences are exported so the chart subtitle
 * and the table caption cannot drift apart from each other.
 *
 * The legend is two short lines (an ECharts subtext does not wrap): long
 * enough to name both measures, short enough to fit a narrow gallery cell. */
export const BURST_MARKER_LEGEND =
    'Line = peak simultaneous sessions per bucket\n' +
    '▲ = burst onset, a different count (sessions entering one wait ' +
    'within 10 ms) — listed under Burst Events below';
export const TOP_PEAKS_CAPTION =
    'The highest points of the line above, ranked — peak simultaneous ' +
    'sessions in a single bucket. The ▲ markers on the chart count ' +
    'something else and are listed separately under Burst Events.';

/* data: concurrency response { peaks[], bursts[], bucket_ns }
 * opts.captureFromNs (optional): the server's earliest captured timestamp
 * (ctx.server.fromNs — "earliest ts in the trace"). Buckets that START
 * before it are EXCLUDED from the plotted series (null, not 0 — ECharts
 * leaves a gap rather than drawing through it) and covered by a labeled
 * "Not captured" markArea, so the pre-capture span never reads as measured
 * idle time.
 * Returns { option, hasData, topPeaks, bursts, bucketNs, notCapturedCount,
 * burstNote } -- burstNote is the #276 completeness sentence (empty when the
 * server declared nothing to withhold).
 * The HTML tables are left to the view's mount (cheap string templating);
 * the row models here are the pure data the templates iterate. */
export function buildConcurrencyOption(data, opts) {
    opts = opts || {};
    if (!data || !data.peaks || data.peaks.length === 0) {
        return { option: null, hasData: false, topPeaks: [], bursts: [], bucketNs: 0,
            notCapturedCount: 0, burstNote: '' };
    }
    const bns = data.bucket_ns || 0;
    const times = data.peaks.map(p => p.t);
    const captureFromNs = Number.isFinite(opts.captureFromNs) ? opts.captureFromNs : null;
    const notCapturedCount = captureFromNs != null
        ? times.filter(t => t < captureFromNs).length : 0;
    const peakData = data.peaks.map((p, i) => i < notCapturedCount ? null : (p.max || 0));
    const peakEvents = data.peaks.map(p => p.event || '');

    const burstsWithBucket = (data.bursts || []).map(b => {
        // Containing bucket by arithmetic (P6): peak t values are bucket
        // STARTS, so the old findIndex(p.t >= ts) was off-by-one for in-range
        // bursts (bucket AFTER the containing one) and returned -1 for any
        // burst inside the FINAL bucket, clamping the marker to bucket 0.
        const idx = bns > 0
            ? Math.floor((b.timestamp_ns - data.peaks[0].t) / bns) : 0;
        const at = Math.min(Math.max(idx, 0), data.peaks.length - 1);
        return { b, at };
    });
    // #105: a burst can only ever come from a CAPTURED bucket — a burst needs
    // real events, so one algebraically landing in the not-captured prefix
    // would be a self-contradiction (a marker sitting inside the "Not
    // captured" band, or a Burst Events row for a moment the chart says has
    // no data). Drop it from BOTH the markPoint and the table below.
    const capturedBursts = burstsWithBucket.filter(({ at }) => at >= notCapturedCount);
    /* #384: the burst marker is an ANNOTATION on the line, never a second
     * value drawn in the line's own place.
     *
     * What it used to do, and why it read as a contradiction: the marker was
     * planted AT the bucket's peak (coord y = the line's value) and labeled
     * with `b.sessions` — a DIFFERENT quantity (pids entering one wait inside
     * 10 ms) — in white, on an opaque triangle up to 30 px tall. On the live
     * 15-minute walk the tallest bucket was 13 simultaneous sessions and the
     * triangle sitting on its apex read "6": measured on
     * shots_unfiltered/tab_concurrency_UNFILTERED.png, the triangle spanned
     * y-values 13.5 down to 8.3 — it covered the apex AND 5 units of the line
     * under it, so the only number legible at that peak was the burst's. The
     * "Top Peak Moments" table below, which reads the SAME peaks array, said
     * 13. Two numbers, one pixel, different units.
     *
     * So: no numeric label on the chart (the count lives in the tooltip and
     * in the Burst Events table, both of which name its unit), a small
     * down-pointing triangle floated ABOVE the data point rather than over
     * it, and y taken strictly from the plotted series — never `|| b.sessions`,
     * which planted a burst COUNT at a line COORDINATE whenever the bucket's
     * peak was 0. */
    const burstPoints = capturedBursts.map(({ b, at }) => ({
        coord: [at, peakData[at]],
        value: b.sessions, symbol: 'triangle', symbolRotate: 180,
        symbolSize: Math.min(8 + b.sessions, 18),
        symbolOffset: [0, -12],
        itemStyle: { color: '#f44' },
        label: { show: false },
    }));
    /* Bucket -> the burst to describe in that bucket's tooltip (the largest,
     * matching the server's one-per-bucket selection). */
    const burstByBucket = new Map();
    capturedBursts.forEach(({ b, at }) => {
        const cur = burstByBucket.get(at);
        if (!cur || b.sessions > cur.sessions) burstByBucket.set(at, b);
    });

    const option = {
        animation: false,
        title: {
            text: 'Peak Concurrent Sessions per Wait Event', left: 'center',
            textStyle: { color: '#ccc', fontSize: 14 },
            /* #384: the panel draws two different measures; say which is
             * which, in the panel, rather than leaving a red triangle to be
             * read as a point on the line. */
            subtext: BURST_MARKER_LEGEND,
            subtextStyle: { color: '#888', fontSize: 11 },
        },
        tooltip: {
            trigger: 'axis', axisPointer: { type: 'cross' },
            backgroundColor: '#1e1e3a', borderColor: '#333',
            textStyle: { color: '#e0e0e0', fontSize: 12 },
            formatter: (params) => {
                if (!params.length) return '';
                const idx = params[0].dataIndex;
                const time = fmtTime(params[0].axisValue, bns);
                if (idx < notCapturedCount) return time + '<br/>Not yet captured';
                const ev = peakEvents[idx] || 'none';
                let out = time + '<br/>' +
                    'Peak: <b>' + params[0].value + ' sessions</b><br/>Event: ' + esc(ev);
                /* #384: the burst count the marker used to print as a bare
                 * number, restored here WITH its unit so it cannot be read as
                 * the line's value. */
                const br = burstByBucket.get(idx);
                if (br) {
                    out += '<br/>▲ Burst: <b>' + br.sessions + ' sessions</b>' +
                        ' entered ' + esc(br.event || '') + ' within 10 ms';
                }
                return out;
            },
        },
        // #105: nameLocation 'middle' + a wider left margin (the matrix fix,
        // PR #147) — the default 'end' location anchored the name text so it
        // extended PAST the canvas left edge at 1280px, clipping the leading
        // "S" of "Simultaneous". A vertical strip inside grid.left never
        // collides with the tick-label column.
        /* top 80 (was 50): the title now carries a two-line legend (#384) and
         * the plot must start below it, not under it. */
        grid: { left: 66, right: 20, top: 80, bottom: 40 },
        xAxis: {
            type: 'category', data: times,
            axisLabel: { color: '#888', fontSize: 10, formatter: (v) => fmtTime(v, bns) },
            axisLine: { lineStyle: { color: '#333' } },
        },
        yAxis: {
            type: 'value', name: 'Simultaneous Sessions', nameLocation: 'middle',
            nameGap: 44, nameRotate: 90,
            nameTextStyle: { color: '#888', fontSize: 11 }, min: 0,
            axisLabel: { color: '#888' }, splitLine: { lineStyle: { color: '#2a2a4a' } },
        },
        series: [{
            type: 'line', data: peakData,
            areaStyle: { color: 'rgba(248,170,170,0.15)' },
            lineStyle: { color: '#f8a', width: 2 }, itemStyle: { color: '#f8a' },
            symbol: 'none',
            markPoint: burstPoints.length > 0 ? { data: burstPoints } : undefined,
            markArea: notCapturedCount > 0 ? {
                silent: true,
                label: { show: true, position: 'insideTop', color: '#999',
                    fontSize: 10, formatter: 'Not captured' },
                data: [[
                    { xAxis: 0, itemStyle: { color: NOT_CAPTURED_BAND_COLOR,
                        borderColor: NOT_CAPTURED_BORDER, borderWidth: 1,
                        borderType: 'dashed' } },
                    { xAxis: notCapturedCount - 1 },
                ]],
            } : undefined,
        }],
    };

    // #105: a not-captured bucket can never be a "top peak" — excluding it
    // from the line but leaving it eligible for this table would have the
    // chart and the table disagree about whether that moment was measured.
    const topPeaks = data.peaks.filter((p, i) => i >= notCapturedCount && p.max > 1)
        .sort((a, b) => b.max - a.max).slice(0, 10);

    const shownBursts = capturedBursts.map(({ b }) => b);
    return { option, hasData: true, topPeaks,
        bursts: shownBursts, bucketNs: bns, notCapturedCount,
        burstNote: burstSelectionNote(data, shownBursts.length) };
}

/* PURE: the panel-level completeness sentence for the burst list (#276).
 *
 * The server detects every burst ONSET in the window and returns the LARGEST
 * one in each bucket, declaring the onset count in `bursts_total`. Before #276
 * it returned the first 20 of a list built in arrival order, so on a 900 s
 * window every marker sat in the first minute and nothing said so.
 *
 * Returns '' when the server sent no `bursts_total` (older server: render
 * exactly as before), when nothing was detected, or when the markers ARE every
 * onset — a note that says "showing 3 of 3" is noise. Driven entirely by
 * bursts_total vs the bursts actually rendered; no bound is hard-coded here. */
export function burstSelectionNote(data, shownCount) {
    if (!data || typeof data.bursts_total !== 'number') return '';
    const total = data.bursts_total;
    const shown = Number(shownCount) || 0;
    if (!(total > 0) || shown <= 0 || shown >= total) return '';
    return 'Showing the largest burst in each of ' + shown.toLocaleString() +
        ' bucket' + (shown === 1 ? '' : 's') + ', out of ' +
        total.toLocaleString() + ' burst onsets detected across this window.';
}

/* PURE: the terse per-row marker for the server's per-burst PID sample (#276).
 *
 * `sessions` is the exact distinct-session count; `pids` carries at most
 * PGWT_BURST_PID_SAMPLE (64) of them. A shorter list than the count is the
 * only bound left in the concurrency response, so it is stated on the row
 * rather than implied by the list's length. '' when the list is complete, or
 * when either field is missing (older server renders exactly as before). */
export function burstPidSampleMarker(b) {
    if (!b || !Array.isArray(b.pids) || typeof b.sessions !== 'number') return '';
    if (b.pids.length >= b.sessions) return '';
    return ' <span style="color:#d9a441;font-size:10px" title="The server returns ' +
        b.pids.length.toLocaleString() + ' of this burst\u2019s ' +
        b.sessions.toLocaleString() + ' PIDs">(' + b.pids.length.toLocaleString() +
        ' of ' + b.sessions.toLocaleString() + ')</span>';
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
        // #384: this table and the chart's line are ONE series; the caption
        // says so, so a reader who finds 13 here looks for 13 on the line
        // instead of comparing it with a burst marker. .panel-caption, not
        // .chart-notes: this explains a column, it does not declare a data
        // limit, and the amber note style is for the latter only.
        '<div class="panel-caption">' + esc(TOP_PEAKS_CAPTION) + '</div>' +
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
            // #276: the panel-level completeness sentence. `.chart-notes:empty`
            // collapses, so a response with nothing to declare renders exactly
            // as it did before.
            '<div class="chart-notes">' + esc(model.burstNote || '') + '</div>' +
            '<table class="data-table"><thead><tr>' +
            '<th>Time</th><th>Wait Event</th><th>Sessions</th><th>PIDs</th></tr></thead><tbody>';
        model.bursts.forEach(b => {
            const time = fmtTimeMs(b.timestamp_ms);
            const pids = (b.pids || []).slice(0, 8).join(', ') +
                (b.pids && b.pids.length > 8 ? '...' : '');
            html += '<tr' + zoomAttrs(b.timestamp_ns, model.bucketNs) + '><td>' + time +
                '</td><td>' + esc(b.event) + '</td><td><b>' +
                b.sessions + '</b></td><td>' + pids +
                burstPidSampleMarker(b) + '</td></tr>';
        });
        html += '</tbody></table>';
    } else {
        html += '<p style="color:#666;margin-top:15px">No burst events detected</p>';
    }
    return html;
}
