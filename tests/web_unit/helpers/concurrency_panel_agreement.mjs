/* The #384 cross-panel gate: do the Concurrency tab's two panels, built from
 * ONE response, show the same numbers?
 *
 * WHY IT EXISTS. On the 2026-10-13 unfiltered walk the "Top Peak Moments"
 * table reported 13 simultaneous sessions at 13:38:39.463 while the chart
 * directly above it appeared to top out at 8 — two numbers, same window, both
 * on screen at once. They came from the same `peaks` array (so the DATA
 * agreed); what disagreed was what the chart DREW: an opaque burst triangle
 * planted on the peak and labeled with a different quantity (6 pids entering
 * one wait inside 10 ms), covering the apex and 5 units of line beneath it.
 * A unit test over the model alone could never see that, because the model was
 * right. So this gate compares the two RENDERED artifacts — the ECharts option
 * and the table HTML string — against the response that produced both.
 *
 * WHAT IT REFUSES TO DO. Approve blind. Every way it could lose sight of the
 * thing it checks returns `unverifiable`, never `agree`:
 *   - no chart option / no series / no axis            -> unverifiable
 *   - zero table rows parsed out of the HTML           -> unverifiable
 *   - fewer/more rows parsed than the model holds      -> unverifiable (markup
 *     drift: the regex stopped matching and would otherwise iterate nothing)
 *   - a bucket time that formats to '--' (toy data)    -> unverifiable
 *   - two buckets formatting to the same time          -> unverifiable
 *   - a sessions cell that is not a bare integer       -> unverifiable
 * A caller asserts `verdict === 'agree'`, so every one of those is a test
 * failure rather than a silent pass.
 *
 * WHAT IT CANNOT SEE. Pixels. If the line were drawn in the background colour
 * the numbers would still agree; the gallery snapshot cells cover that.
 */
import { fmtTimeMs, esc } from '../../../web/static/lib/format.js';

/* Rows of one <tbody>, as arrays of raw cell HTML.
 *
 * The section is located by its HEADING, not by the first occurrence of its
 * name: the panel captions name each other ("...listed separately under Burst
 * Events"), and anchoring on bare text made this read the Top Peak Moments
 * tbody as the burst table — a parser that silently answers about the wrong
 * panel is the failure mode this whole file is built to refuse. */
function tbodyRows(html, headingText) {
    const h = html.search(new RegExp('<h3[^>]*>' + headingText));
    if (h < 0) return null;
    const open = html.indexOf('<tbody>', h);
    const close = html.indexOf('</tbody>', open);
    if (open < 0 || close < 0) return null;
    const body = html.slice(open + '<tbody>'.length, close);
    const rows = [];
    const re = /<tr[^>]*>([\s\S]*?)<\/tr>/g;
    let m;
    while ((m = re.exec(body)) !== null) {
        const cells = [];
        const cre = /<td[^>]*>([\s\S]*?)<\/td>/g;
        let c;
        while ((c = cre.exec(m[1])) !== null) cells.push(c[1]);
        rows.push(cells);
    }
    return rows;
}

export function concurrencyPanelAgreement(data, model, html) {
    const problems = [];
    const unverifiable = [];
    let rowsChecked = 0, markersChecked = 0, burstRowsChecked = 0;

    const fail = (s) => problems.push(s);
    const blind = (s) => unverifiable.push(s);

    const opt = model && model.option;
    const series = opt && opt.series && opt.series[0];
    const sdata = series && series.data;
    const axis = opt && opt.xAxis && opt.xAxis.data;
    const peaks = data && data.peaks;

    if (!opt || !Array.isArray(sdata) || !Array.isArray(axis) || !Array.isArray(peaks)) {
        blind('no chart option/series/axis or no peaks in the response');
        return verdict();
    }
    if (sdata.length === 0) { blind('chart series is empty'); return verdict(); }

    /* The chart's own bucket axis must still be the response's buckets, in
     * order — everything below indexes the series by that identity. */
    if (axis.length !== peaks.length || sdata.length !== peaks.length)
        fail(`axis/series/peaks length mismatch: ${axis.length}/${sdata.length}/${peaks.length}`);
    for (let i = 0; i < Math.min(axis.length, peaks.length); i++)
        if (axis[i] !== peaks[i].t)
            fail(`chart bucket ${i} is at t=${axis[i]}, the response says ${peaks[i].t}`);

    /* Bucket time -> index, by the SAME string the table prints. */
    const byTime = new Map();
    for (let i = 0; i < peaks.length; i++) {
        const s = fmtTimeMs(peaks[i].t_ms);
        if (s === '--') { blind(`bucket ${i} has no printable t_ms (${peaks[i].t_ms})`); return verdict(); }
        if (byTime.has(s)) { blind(`two buckets print the same time ${s}`); return verdict(); }
        byTime.set(s, i);
    }

    /* ── Panel A: the "Top Peak Moments" table, as rendered ───────────── */
    const rows = tbodyRows(html, 'Top Peak Moments');
    if (rows === null) { blind('Top Peak Moments table not found in the HTML'); return verdict(); }
    const expectRows = (model.topPeaks || []).length;
    if (rows.length === 0 || rows.length !== expectRows) {
        blind(`parsed ${rows.length} Top Peak Moments rows, the model holds ${expectRows}` +
              ' — markup drift would make this check iterate nothing');
        return verdict();
    }

    let tableMax = 0;
    for (const cells of rows) {
        if (cells.length !== 3) { blind(`a peak row has ${cells.length} cells, expected 3`); return verdict(); }
        const [timeCell, eventCell, sessCell] = cells;
        const m = /^<b>(\d+)<\/b>$/.exec(sessCell);
        if (!m) { blind(`unparseable sessions cell ${JSON.stringify(sessCell)}`); return verdict(); }
        const shown = Number(m[1]);
        const idx = byTime.get(timeCell);
        if (idx === undefined) {
            fail(`table row at ${timeCell} matches no bucket on the chart's axis`);
            continue;
        }
        rowsChecked++;
        tableMax = Math.max(tableMax, shown);
        const drawn = sdata[idx];
        if (drawn === null)
            fail(`table reports ${shown} sessions at ${timeCell}, where the chart draws nothing (not captured)`);
        else if (Number(drawn) !== shown)
            fail(`table says ${shown} sessions at ${timeCell}, the chart's line draws ${drawn}`);
        const wantEvent = esc(peaks[idx].event || 'CPU*');
        if (eventCell !== wantEvent)
            fail(`table says event ${eventCell} at ${timeCell}, the response says ${wantEvent}`);
    }

    /* The single claim the #384 screenshot contradicted: the biggest number in
     * the table IS the top of the line. (The table is capped at 10 rows but
     * sorted descending, so its first row is the window's maximum.) */
    const drawnMax = sdata.reduce((a, v) => (v === null ? a : Math.max(a, Number(v))), 0);
    if (drawnMax > 1 && tableMax !== drawnMax)
        fail(`the chart's tallest point is ${drawnMax}, the table's largest row is ${tableMax}`);

    /* ── Panel B: burst markers vs the Burst Events table ─────────────── */
    const mp = (series.markPoint && series.markPoint.data) || [];
    markersChecked = mp.length;
    for (const p of mp) {
        const at = p.coord && p.coord[0];
        if (!Number.isInteger(at) || at < 0 || at >= sdata.length) {
            fail(`burst marker at bucket ${at}, outside the ${sdata.length}-bucket axis`);
            continue;
        }
        /* The marker is an annotation ON the line: its y is the line's own
         * value. Anything else plants one quantity at another's coordinate. */
        if (p.coord[1] !== sdata[at])
            fail(`burst marker at bucket ${at} sits at y=${p.coord[1]}, the line there is ${sdata[at]}`);
        /* #384 proper: a bare number drawn inside the plot that is NOT the
         * line's value is a second, unlabelled measure in the same pixels. */
        const lab = p.label;
        const text = lab && lab.show !== false ? lab.formatter : null;
        if (typeof text === 'string' && /^\s*\d+\s*$/.test(text) && Number(text) !== sdata[at])
            fail(`burst marker at bucket ${at} prints a bare "${text}" where the line reads ${sdata[at]}`);
    }
    const burstRows = tbodyRows(html, 'Burst Events');
    const shownBursts = (model.bursts || []).length;
    if (burstRows === null) {
        if (shownBursts > 0) fail(`${shownBursts} bursts in the model but no Burst Events table rendered`);
        if (markersChecked > 0) fail(`${markersChecked} burst markers on the chart, no Burst Events table below`);
    } else {
        burstRowsChecked = burstRows.length;
        if (burstRows.length !== markersChecked)
            fail(`Burst Events lists ${burstRows.length} bursts, the chart draws ${markersChecked} markers`);
    }

    return verdict();

    function verdict() {
        return {
            verdict: unverifiable.length ? 'unverifiable'
                : (problems.length ? 'disagree' : 'agree'),
            rowsChecked, markersChecked, burstRowsChecked,
            problems, unverifiable,
        };
    }
}

/* One-line summary for assertion messages. */
export function describe(r) {
    return `${r.verdict} (rows=${r.rowsChecked}, markers=${r.markersChecked}` +
        `, burstRows=${r.burstRowsChecked})` +
        (r.problems.length ? '\n  problem: ' + r.problems.join('\n  problem: ') : '') +
        (r.unverifiable.length ? '\n  blind: ' + r.unverifiable.join('\n  blind: ') : '');
}
