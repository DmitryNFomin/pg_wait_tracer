/* #384 — the Concurrency tab's two panels, pinned against each other.
 *
 * THE BUG. On the 2026-10-13 unfiltered walk the "Top Peak Moments" table read
 * 13 simultaneous sessions at 13:38:39.463 while the chart above it looked
 * like it never passed 8. Both panels are built by ONE builder from ONE
 * response, and the numbers in the model were identical — what differed was
 * what the chart DREW there: the burst markPoint was planted at the bucket's
 * peak and labeled with a different quantity (6 pids entering one wait within
 * 10 ms) on an opaque triangle. Pixel-measured on that screenshot, the
 * triangle covered y-values 13.5 down to 8.3 — the apex and five units of the
 * line beneath it — so "6" was the only number legible at the 13-session peak.
 *
 * THE PIN. tests/web_unit/helpers/concurrency_panel_agreement.mjs compares the
 * two RENDERED artifacts (the ECharts option and the table HTML) against the
 * response that produced both, and refuses ("unverifiable") on every path
 * where it would otherwise iterate nothing. The model-level tests in
 * concurrency.test.mjs could not have caught #384 — the model was right.
 */
import { test } from 'node:test';
import assert from 'node:assert/strict';
import {
    buildConcurrencyOption, buildConcurrencyTables,
    BURST_MARKER_LEGEND, TOP_PEAKS_CAPTION,
} from '../../web/static/lib/builders/concurrency.js';
import { concurrencyPanelAgreement, describe }
    from './helpers/concurrency_panel_agreement.mjs';
import { states as galleryStates } from '../../web/static/dev/fixtures/concurrency.mjs';

/* Epoch and bucket width chosen so every t and t_ms below is EXACT in a
 * double (ns epochs quantize to 256 ns steps; 1.774e18 and 3e9 are both
 * multiples of 256), i.e. nothing here depends on rounding. */
const FROM = 1_774_000_000_000_000_000;
const BUCKET = 3_000_000_000;

/* The live shape from the issue: a floor around 4-8 with ONE 13-session peak,
 * and a 6-session burst inside that same peak bucket — the exact
 * configuration that produced "13 in the table, 6 on the chart". */
function liveShape() {
    const maxes = [4, 5, 4, 6, 8, 5, 4, 13, 9, 7, 6, 9, 5, 4, 8, 6, 9, 4, 5, 7];
    const events = maxes.map((_, i) =>
        i === 7 ? 'IO:RelationMapRead' : (i % 2 ? 'IO:DataFileRead' : 'LWLock:LockManager'));
    const peaks = maxes.map((max, i) => {
        const t = FROM + i * BUCKET;
        return { t, t_ms: Math.floor(t / 1e6), max, event: events[i] };
    });
    const burst = (i, offset, event, sessions) => {
        const ts = FROM + i * BUCKET + offset;
        return { timestamp_ns: ts, timestamp_ms: Math.floor(ts / 1e6), event,
            sessions, pids: Array.from({ length: Math.min(sessions, 8) }, (_, k) => 1001 + k) };
    };
    return {
        bucket_ns: BUCKET,
        peaks,
        bursts: [
            burst(7, 1_000_000, 'IO:RelationMapRead', 6),   // inside the 13-session peak
            burst(4, 500_000, 'LWLock:LockManager', 8),
            burst(16, 2_000_000, 'LWLock:LockManager', 5),
        ],
        bursts_total: 3, bursts_truncated: false,
    };
}

function panels(data, opts) {
    const model = buildConcurrencyOption(data, opts);
    return { model, html: buildConcurrencyTables(model) };
}
function agreement(data, opts) {
    const { model, html } = panels(data, opts);
    return { model, html, result: concurrencyPanelAgreement(data, model, html) };
}

/* ── The deliverable: the two panels agree, on the issue's own data ──────── */

test('#384: chart line and Top Peak Moments agree on the same response', () => {
    const data = liveShape();
    const { result, model } = agreement(data);
    assert.equal(result.verdict, 'agree', describe(result));
    // The gate must actually have looked at something — a vacuous pass is the
    // failure mode this whole file exists to prevent.
    assert.equal(result.rowsChecked, model.topPeaks.length);
    assert.ok(result.rowsChecked >= 10, 'expected a full 10-row table to check');
    assert.equal(result.markersChecked, 3);
    assert.equal(result.burstRowsChecked, 3);
    // And the number the issue reported is the one both panels carry.
    assert.equal(model.topPeaks[0].max, 13);
    assert.equal(model.option.series[0].data[7], 13);
});

test('#384: the chart draws no second number on the line', () => {
    const { model } = panels(liveShape());
    const mp = model.option.series[0].markPoint.data;
    for (const p of mp) {
        assert.equal(p.label.show, false, 'burst count must not be printed on the line');
        // The annotation floats ABOVE its data point instead of covering it.
        assert.deepEqual(p.symbolOffset, [0, -12]);
        assert.equal(p.symbolRotate, 180);
        assert.ok(p.symbolSize <= 18, 'marker must not cover units of line under it');
    }
    // The count itself is still reachable, with its unit named.
    const tip = model.option.tooltip.formatter([{ dataIndex: 7, axisValue: FROM + 7 * BUCKET, value: 13 }]);
    assert.ok(tip.includes('Peak: <b>13 sessions</b>'));
    assert.ok(tip.includes('Burst: <b>6 sessions</b> entered IO:RelationMapRead within 10 ms'), tip);
});

test('#384: both panels say what their number means', () => {
    const { model, html } = panels(liveShape());
    assert.equal(model.option.title.subtext, BURST_MARKER_LEGEND);
    assert.ok(model.option.grid.top > 50, 'plot must start below the two-line title');
    assert.ok(html.includes(TOP_PEAKS_CAPTION));
});

/* The old `peakData[at] || b.sessions` fallback: when a bucket's peak was 0,
 * the marker was placed at the BURST COUNT's height — one quantity drawn at
 * another's coordinate, which is #384 in miniature. */
test('#384: a burst in a zero-peak bucket sits on the line, not at its own count', () => {
    const data = liveShape();
    data.peaks[7].max = 0;
    const { model, result } = agreement(data);
    const at7 = model.option.series[0].markPoint.data.find(p => p.coord[0] === 7);
    assert.equal(at7.coord[1], 0);
    assert.equal(result.verdict, 'agree', describe(result));
});

/* Breadth: every curated gallery state must pass the same cross-panel gate. */
test('#384: every gallery concurrency state passes the cross-panel gate', () => {
    let verified = 0;
    for (const [id, st] of Object.entries(galleryStates)) {
        const { result } = agreement(st.data, st.opts);
        if (result.verdict === 'unverifiable') {
            // Only the deliberately empty state may be unverifiable.
            assert.equal(id, 'empty', `${id}: ${describe(result)}`);
            continue;
        }
        assert.equal(result.verdict, 'agree', `${id}: ${describe(result)}`);
        assert.ok(result.rowsChecked > 0, `${id}: checked no rows`);
        verified++;
    }
    assert.ok(verified >= 4, `only ${verified} gallery states were actually verified`);
});

/* ── True positives: every way the panels could drift, the gate sees ─────── */

function mutate(fn) {
    const data = liveShape();
    const model = buildConcurrencyOption(data);
    const html = buildConcurrencyTables(model);
    const out = fn({ data, model, html });
    return concurrencyPanelAgreement(out.data || data, out.model || model, out.html || html);
}

test('gate RED: the chart draws a different value than the table reports', () => {
    const r = mutate(({ model }) => {
        model.option.series[0].data[7] = 8;      // the #384 screenshot's reading
        return { model };
    });
    assert.equal(r.verdict, 'disagree');
    assert.ok(r.problems.some(p => /table says 13 sessions .*the chart's line draws 8/.test(p)),
        describe(r));
});

test('gate RED: a burst marker prints a bare number over a line of another value', () => {
    const r = mutate(({ model }) => {
        // Exactly the pre-#384 marker: label = the burst's session count.
        model.option.series[0].markPoint.data[0].label =
            { show: true, formatter: '6', color: '#fff', fontSize: 10 };
        return { model };
    });
    assert.equal(r.verdict, 'disagree');
    assert.ok(r.problems.some(p => /prints a bare "6" where the line reads 13/.test(p)), describe(r));
});

test('gate RED: a burst marker placed at its own session count, not on the line', () => {
    const r = mutate(({ model }) => {
        const mp = model.option.series[0].markPoint.data[0];
        mp.coord = [mp.coord[0], 6];             // the `|| b.sessions` fallback
        return { model };
    });
    assert.equal(r.verdict, 'disagree');
    assert.ok(r.problems.some(p => /sits at y=6, the line there is 13/.test(p)), describe(r));
});

test('gate RED: the tallest point on the line is missing from the table', () => {
    const r = mutate(({ data }) => {
        // A peak the table would rank first, hidden from the table only.
        const model = buildConcurrencyOption(data);
        model.topPeaks = model.topPeaks.filter(p => p.max !== 13);
        return { model, html: buildConcurrencyTables(model) };
    });
    assert.equal(r.verdict, 'disagree');
    assert.ok(r.problems.some(p => /tallest point is 13, the table's largest row is 9/.test(p)),
        describe(r));
});

test('gate RED: the chart plots a bucket the response does not have', () => {
    const r = mutate(({ model }) => {
        model.option.xAxis.data[3] = model.option.xAxis.data[3] + BUCKET;
        return { model };
    });
    assert.equal(r.verdict, 'disagree');
    assert.ok(r.problems.some(p => /chart bucket 3 is at t=/.test(p)), describe(r));
});

test('gate RED: the Burst Events table and the chart markers list different bursts', () => {
    const r = mutate(({ model }) => {
        model.bursts = model.bursts.slice(1);
        return { model, html: buildConcurrencyTables(model) };
    });
    assert.equal(r.verdict, 'disagree');
    assert.ok(r.problems.some(p => /Burst Events lists 2 bursts, the chart draws 3 markers/.test(p)),
        describe(r));
});

test('gate RED: a table row for a moment the chart never plots', () => {
    const r = mutate(({ model }) => {
        const html = buildConcurrencyTables(model)
            .replace(/<td>([^<]*\.463|[0-9:.]+)<\/td>/, '<td>09:09:09.999</td>');
        return { html };
    });
    assert.equal(r.verdict, 'disagree');
    assert.ok(r.problems.some(p => /matches no bucket on the chart's axis/.test(p)), describe(r));
});

test('gate RED: a table row inside the not-captured band', () => {
    const data = liveShape();
    const model = buildConcurrencyOption(data, { captureFromNs: FROM + 10 * BUCKET });
    // Re-admit a pre-capture bucket to the table only — the chart leaves it null.
    model.topPeaks = [data.peaks[7]].concat(model.topPeaks);
    const r = concurrencyPanelAgreement(data, model, buildConcurrencyTables(model));
    assert.equal(r.verdict, 'disagree');
    assert.ok(r.problems.some(p => /where the chart draws nothing \(not captured\)/.test(p)),
        describe(r));
});

/* ── BYPASS SUITE: every way the gate could be made blind, it must refuse ──
 *
 * These are the false negatives, which is the failure mode that actually
 * ships: a gate that iterates zero rows, or cannot resolve what it is
 * comparing, must say "unverifiable" — never "agree". The caller asserts
 * `verdict === 'agree'`, so each of these is a loud test failure. */

test('gate BLIND-REFUSES: empty response (no peaks at all)', () => {
    const r = agreement({ bucket_ns: BUCKET, peaks: [], bursts: [] }).result;
    assert.equal(r.verdict, 'unverifiable');
    assert.equal(r.rowsChecked, 0);
});

test('gate BLIND-REFUSES: a response whose every bucket is max<=1 (empty table)', () => {
    const data = liveShape();
    data.peaks.forEach(p => { p.max = 1; });
    data.bursts = [];
    const { model, result } = agreement(data);
    assert.deepEqual(model.topPeaks, []);         // nothing to compare...
    assert.equal(result.verdict, 'unverifiable'); // ...so it refuses, not approves
});

test('gate BLIND-REFUSES: the model has no chart at all', () => {
    const data = liveShape();
    const model = buildConcurrencyOption(data);
    model.option = null;
    const r = concurrencyPanelAgreement(data, model, buildConcurrencyTables(model));
    assert.equal(r.verdict, 'unverifiable');
});

test('gate BLIND-REFUSES: markup drift — the sessions cell stops being parseable', () => {
    const r = mutate(({ html }) => ({ html: html.replace(/<b>(\d+)<\/b>/g, '$1') }));
    assert.equal(r.verdict, 'unverifiable');
    assert.ok(r.unverifiable.some(p => /unparseable sessions cell/.test(p)), describe(r));
});

test('gate BLIND-REFUSES: markup drift — rows stop matching the row regex', () => {
    const r = mutate(({ html }) => ({ html: html.replace(/<tr/g, '<TR').replace(/<\/tr>/g, '</TR>') }));
    assert.equal(r.verdict, 'unverifiable');
    assert.ok(r.unverifiable.some(p => /parsed 0 Top Peak Moments rows/.test(p)), describe(r));
});

test('gate BLIND-REFUSES: a partial table — fewer rows rendered than the model holds', () => {
    const r = mutate(({ html }) => {
        // Drop one DATA row (the first <tr> after <tbody>; the <thead> row is
        // not one the gate reads).
        const i = html.indexOf('<tr', html.indexOf('<tbody>'));
        const j = html.indexOf('</tr>', i) + '</tr>'.length;
        return { html: html.slice(0, i) + html.slice(j) };
    });
    assert.equal(r.verdict, 'unverifiable');
    assert.ok(r.unverifiable.some(p => /parsed 9 Top Peak Moments rows, the model holds 10/.test(p)),
        describe(r));
});

test('gate BLIND-REFUSES: the Top Peak Moments heading is gone (panel renamed/removed)', () => {
    const r = mutate(({ html }) => ({ html: html.replace('Top Peak Moments', 'Peaks') }));
    assert.equal(r.verdict, 'unverifiable');
    assert.ok(r.unverifiable.some(p => /table not found in the HTML/.test(p)), describe(r));
});

test('gate BLIND-REFUSES: toy timestamps that print as "--" (no resolvable bucket time)', () => {
    const data = {
        bucket_ns: 1000,
        peaks: [{ t: 1000, t_ms: 0, max: 2, event: 'A' }, { t: 2000, t_ms: 0, max: 8, event: 'B' }],
        bursts: [],
    };
    assert.equal(agreement(data).result.verdict, 'unverifiable');
});

test('gate BLIND-REFUSES: two buckets that print the same time (ambiguous mapping)', () => {
    const data = liveShape();
    data.peaks[8].t_ms = data.peaks[7].t_ms;
    assert.equal(agreement(data).result.verdict, 'unverifiable');
});

/* Found by this suite going red on itself: TOP_PEAKS_CAPTION names the other
 * panel ("...listed separately under Burst Events"), and a parser anchored on
 * that bare text read the PEAKS tbody as the burst table — 10 "bursts" against
 * 3 markers. A gate that answers about the wrong panel is the same false
 * negative in a different coat, so the two sections are pinned as distinct. */
test('gate reads two DISTINCT tables even though each caption names the other', () => {
    const { model, html } = panels(liveShape());
    assert.ok(html.indexOf('Burst Events') < html.indexOf('<h3 style="color:#ccc;margin:15px 0 5px">Burst Events'),
        'the caption must mention Burst Events before the heading does (the hazard)');
    const r = concurrencyPanelAgreement(liveShape(), model, html);
    assert.equal(r.verdict, 'agree', describe(r));
    assert.equal(r.rowsChecked, 10);
    assert.equal(r.burstRowsChecked, 3);
});

/* The "thing being checked is ABSENT rather than wrong" case: a response with
 * no bursts leaves the marker half of the gate with nothing to inspect. It
 * must report that honestly (markersChecked = 0) rather than let a caller read
 * "agree" as "the markers were checked" — which is why the headline test above
 * asserts markersChecked === 3 explicitly. */
test('gate reports zero markers inspected when the response carries no bursts', () => {
    const data = liveShape();
    data.bursts = [];
    const r = agreement(data).result;
    assert.equal(r.verdict, 'agree');
    assert.equal(r.markersChecked, 0);
    assert.equal(r.burstRowsChecked, 0);
    assert.ok(r.rowsChecked > 0);
});
