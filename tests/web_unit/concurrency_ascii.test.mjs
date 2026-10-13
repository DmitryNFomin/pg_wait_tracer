/* #388 — the concurrency panels' own text is ASCII-only, and the gate that
 * says so can actually see.
 *
 * THE DEFECT THIS PINS. #384 added two sentences naming the panel's two
 * measures; they carried "▲" and "—". In CI's container (image digest
 * identical to the passing run's) that forced a fallback-font load, which
 * flipped Chromium's `monospace` generic for everything laid out afterwards:
 * the `.query-id` span rendered "100" as 20x9 px instead of 16x10, four rows
 * of the waterfall table each lost 1 px, and `waterfall_execution` came out
 * 505 px against its 509 px baseline. The snapshot suite captures the
 * waterfall cell immediately AFTER the concurrency tab
 * (tests/test_web_ui_snapshots.py), so panel text in one view resized the
 * typeface of the next. Nothing about the layout of either view changed.
 *
 * CHROME, NOT DATA. The rule is about text the UI AUTHORS. A wait-event name
 * is data: PostgreSQL (or an extension) may legitimately send
 * "Extension:日本語..." and the UI must render it, which is exactly what the
 * `hostile-event-names` gallery state asserts elsewhere. So the breadth test
 * masks every value that came from the response before auditing — and a value
 * the mask fails to account for makes the audit go RED (a loud false positive),
 * never silently clean.
 *
 * THE GATE. Audit every string the builder emits — the whole ECharts option
 * (walked), the table HTML, and the tooltip formatter's OUTPUT for each branch
 * that produces text — not just the two exported constants: a glyph can enter
 * through any of them, and the two-constant version of this check would have
 * passed while the tooltip still shipped "▲".
 *
 * THE BYPASS SUITE (second half of this file). Every way the audit could stop
 * detecting while still printing a pass: nothing to inspect, a source absent
 * rather than wrong, an unwalkable source, a required source silently dropped
 * from the call site, a detector that does not recurse, a detector blind to
 * the exact pre-fix strings, and a mask so greedy it erases the chrome it was
 * meant to leave. Each must come out 'unverifiable' or 'non-ascii' — never
 * 'ascii-only'.
 */
import { test } from 'node:test';
import assert from 'node:assert/strict';
import {
    buildConcurrencyOption, buildConcurrencyTables,
    BURST_MARKER_LEGEND, TOP_PEAKS_CAPTION,
} from '../../web/static/lib/builders/concurrency.js';
import { esc } from '../../web/static/lib/format.js';
import { states as galleryStates } from '../../web/static/dev/fixtures/concurrency.mjs';
import { asciiAudit, collectStrings, describeAudit } from './helpers/ascii_only.mjs';

const FROM = 1_774_000_000_000_000_000;
const BUCKET = 3_000_000_000;

function liveShape() {
    const maxes = [4, 5, 4, 6, 8, 5, 4, 13, 9, 7, 6, 9, 5, 4, 8, 6, 9, 4, 5, 7];
    const peaks = maxes.map((max, i) => {
        const t = FROM + i * BUCKET;
        return { t, t_ms: Math.floor(t / 1e6), max,
            event: i === 7 ? 'IO:RelationMapRead' : (i % 2 ? 'IO:DataFileRead' : 'LWLock:LockManager') };
    });
    const burst = (i, offset, event, sessions) => {
        const ts = FROM + i * BUCKET + offset;
        return { timestamp_ns: ts, timestamp_ms: Math.floor(ts / 1e6), event, sessions,
            pids: Array.from({ length: Math.min(sessions, 8) }, (_, k) => 1001 + k) };
    };
    return {
        bucket_ns: BUCKET, peaks,
        bursts: [burst(7, 1_000_000, 'IO:RelationMapRead', 6),
                 burst(4, 500_000, 'LWLock:LockManager', 8),
                 burst(16, 2_000_000, 'LWLock:LockManager', 5)],
        bursts_total: 3, bursts_truncated: false,
    };
}

/* Everything one render of the panels puts on screen. The tooltip is a
 * function on the option, so each of its text-producing branches is invoked
 * here and its OUTPUT audited as a source of its own; the indices come from
 * the built model, so a fixture with three buckets is probed at a bucket it
 * actually has (an out-of-range index would quietly audit "none"). */
function renderedText(data, opts) {
    const model = buildConcurrencyOption(data, opts);
    const html = buildConcurrencyTables(model);
    const option = model.option;
    const fmt = option.tooltip.formatter;
    const labels = option.xAxis.data;
    const series = option.series[0].data;
    const markers = (option.series[0].markPoint && option.series[0].markPoint.data) || [];
    const burstIdx = markers.length ? markers[0].coord[0] : null;
    let plainIdx = null;
    for (let i = labels.length - 1; i >= 0; i--) {
        if (i !== burstIdx && series[i] !== null) { plainIdx = i; break; }
    }
    const params = (idx) => [{ dataIndex: idx, axisValue: labels[idx], value: series[idx] }];
    const sources = {
        'option': option,
        'tables.html': html,
        'legend.const': BURST_MARKER_LEGEND,
        'caption.const': TOP_PEAKS_CAPTION,
    };
    if (plainIdx !== null) sources['tooltip.plain'] = fmt(params(plainIdx));
    if (burstIdx !== null) sources['tooltip.burst'] = fmt(params(burstIdx));
    if (model.notCapturedCount > 0) sources['tooltip.notcaptured'] = fmt(params(0));
    return { model, sources, burstIdx, plainIdx };
}

/* Values that came from the RESPONSE, in both raw and HTML-escaped form.
 * Longest first so a short name cannot shadow a longer one containing it. */
function dataValues(data) {
    const out = new Set();
    for (const p of data.peaks || []) if (p.event) out.add(p.event);
    for (const b of data.bursts || []) if (b.event) out.add(b.event);
    const both = [];
    for (const v of out) { both.push(v); if (esc(v) !== v) both.push(esc(v)); }
    return both.sort((a, b) => b.length - a.length);
}

/* Replace pass-through data with a marker, leaving the UI's own text intact.
 * Functions are dropped (the audit skips them anyway; their output is audited
 * separately above). */
function maskData(sources, data) {
    const vals = dataValues(data);
    const strip = (s) => vals.reduce((acc, v) => acc.split(v).join('{data}'), s);
    const walk = (v) => {
        if (typeof v === 'string') return strip(v);
        if (Array.isArray(v)) return v.map(walk);
        if (v && typeof v === 'object') {
            const o = {};
            for (const [k, x] of Object.entries(v)) if (typeof x !== 'function') o[k] = walk(x);
            return o;
        }
        return v;
    };
    const out = {};
    for (const [k, v] of Object.entries(sources)) out[k] = walk(v);
    return out;
}

const REQUIRED = ['option', 'tables.html', 'tooltip.burst', 'tooltip.plain',
                  'legend.const', 'caption.const'];

/* ── The deliverable ──────────────────────────────────────────────────────── */

test('#388: every string the concurrency panels render is ASCII-only', () => {
    const { sources } = renderedText(liveShape());
    // No masking here: this fixture's own event names are ASCII, so the panel
    // is audited exactly as it ships, data included.
    const r = asciiAudit(sources, { require: REQUIRED, minStrings: 30 });
    assert.equal(r.verdict, 'ascii-only', describeAudit(r));
    // The audit must have looked at the panel's real text, not a husk: the
    // live shape yields 35+ strings (title + legend + 20 bucket labels + event
    // names + both tooltips + the table HTML + the two constants).
    assert.equal(r.checked, REQUIRED.length);
    assert.ok(r.strings >= 30, `only ${r.strings} strings inspected`);
    // ...and the tooltip branches really produced their text, rather than
    // auditing an empty string that can never be non-ASCII.
    assert.match(sources['tooltip.burst'], /Burst: <b>6 sessions<\/b>/);
    assert.match(sources['tooltip.plain'], /Peak: <b>\d+ sessions<\/b>/);
});

test('#388: the two named sentences are intact as well as ASCII', () => {
    // The fix removes glyphs, never the sentences: #384 exists because the
    // panels showed two quantities and named neither.
    for (const s of [BURST_MARKER_LEGEND, TOP_PEAKS_CAPTION]) {
        assert.ok(s.length > 60, `caption/legend too short to name a measure: ${s}`);
    }
    assert.equal(BURST_MARKER_LEGEND.split('\n').length, 2, 'subtext stays two lines');
    assert.match(BURST_MARKER_LEGEND, /peak simultaneous sessions per bucket/);
    assert.match(BURST_MARKER_LEGEND, /burst onset/);
    assert.match(BURST_MARKER_LEGEND, /within 10 ms/);
    assert.match(TOP_PEAKS_CAPTION, /peak simultaneous[\s\S]*single bucket/);
    assert.match(TOP_PEAKS_CAPTION, /Burst Events/);   // still points at the other panel
    const model = buildConcurrencyOption(liveShape());
    assert.equal(model.option.title.subtext, BURST_MARKER_LEGEND);
    assert.ok(buildConcurrencyTables(model).includes(TOP_PEAKS_CAPTION),
        'caption must still reach the DOM');
});

test('#388: every gallery concurrency state renders ASCII-only chrome', () => {
    let full = 0, chartless = 0, withBursts = 0;
    for (const [id, st] of Object.entries(galleryStates)) {
        const model = buildConcurrencyOption(st.data, st.opts);
        if (model.option === null) {
            // The deliberately empty state draws no chart; it still renders
            // the tables (with the caption), so audit exactly that and say so,
            // rather than letting a null option read as a clean pass.
            assert.equal(model.hasData, false, `${id}: null option but hasData true`);
            const r = asciiAudit({ 'tables.html': buildConcurrencyTables(model),
                                   'caption.const': TOP_PEAKS_CAPTION,
                                   'legend.const': BURST_MARKER_LEGEND },
                                 { require: ['tables.html', 'caption.const', 'legend.const'],
                                   minStrings: 3 });
            assert.equal(r.verdict, 'ascii-only', `${id}: ${describeAudit(r)}`);
            chartless++;
            continue;
        }
        const { sources, burstIdx } = renderedText(st.data, st.opts);
        // Every tooltip branch this state can reach must be in the audit (the
        // single-bucket state has only a burst bucket, so it has no "plain"
        // one) — and it must reach at least one, or the tooltip went
        // unaudited without anything saying so.
        const tips = Object.keys(sources).filter(k => k.startsWith('tooltip.'));
        assert.ok(tips.length >= 1, `${id}: no tooltip branch audited`);
        const need = ['option', 'tables.html', 'legend.const', 'caption.const', ...tips];
        const r = asciiAudit(maskData(sources, st.data), { require: need, minStrings: 8 });
        assert.equal(r.verdict, 'ascii-only', `${id}: ${describeAudit(r)}`);
        if (burstIdx !== null) withBursts++;
        full++;
    }
    // Every arm must have run: a fixture set that lost its chart states would
    // otherwise leave this test auditing captions only, and a set with no
    // bursts would never reach the burst tooltip or the markers.
    assert.ok(full >= 4, `expected several chart states, audited ${full}`);
    assert.ok(withBursts >= 2, `expected burst states, audited ${withBursts}`);
    assert.equal(chartless, 1, `expected exactly one chartless state, got ${chartless}`);
});

/* ── Bypass suite: the ways this gate could stop seeing ───────────────────── */

test('bypass: the detector sees the exact pre-fix strings (true positive)', () => {
    // Verbatim #384 text, i.e. the tree this test was written against. If the
    // detector cannot flag these, it cannot flag anything.
    const legendBefore =
        'Line = peak simultaneous sessions per bucket\n' +
        '▲ = burst onset, a different count (sessions entering one wait ' +
        'within 10 ms) — listed under Burst Events below';
    const captionBefore =
        'The highest points of the line above, ranked — peak simultaneous ' +
        'sessions in a single bucket. The ▲ markers on the chart count ' +
        'something else and are listed separately under Burst Events.';
    const r = asciiAudit({ legend: legendBefore, caption: captionBefore,
                           tip: '<br/>▲ Burst: <b>6 sessions</b>' });
    assert.equal(r.verdict, 'non-ascii', describeAudit(r));
    // Every distinct glyph in every source, not merely the first one found:
    // the legend carries both, the caption carries both, the tooltip the
    // triangle. A report that stops at one glyph costs a second fix round.
    assert.deepEqual(r.findings.map(f => `${f.path}:${f.codePoint}`).sort(),
        ['caption:U+2014', 'caption:U+25B2', 'legend:U+2014', 'legend:U+25B2',
         'tip:U+25B2']);
});

test('bypass: a glyph anywhere in the real render is caught, not just in the constants', () => {
    // The two-constant version of this gate would pass here.
    const { sources } = renderedText(liveShape());
    const poisoned = { ...sources,
        'tooltip.burst': sources['tooltip.burst'].replace('Burst', '▲ Burst') };
    const r = asciiAudit(poisoned, { require: REQUIRED, minStrings: 30 });
    assert.equal(r.verdict, 'non-ascii', describeAudit(r));
    assert.deepEqual(r.findings.map(f => f.path), ['tooltip.burst']);
});

test('bypass: a glyph nested deep in the option is caught (the walk recurses)', () => {
    const opt = { title: { text: 'Peak Concurrent Sessions' },
                  series: [{ markPoint: { data: [{ label: { formatter: 'peak — 13' } }] } }] };
    const r = asciiAudit({ option: opt });
    assert.equal(r.verdict, 'non-ascii', describeAudit(r));
    assert.equal(r.findings[0].path, 'option.series[0].markPoint.data[0].label.formatter');
    // Astral plane: reported once as its own code point, not as two surrogates.
    const emoji = asciiAudit({ t: { a: { b: 'ok \u{1F600}' } } });
    assert.equal(emoji.findings[0].codePoint, 'U+1F600');
    assert.equal(emoji.findings[0].path, 't.a.b');
});

test('bypass: EMPTY input refuses, it does not approve', () => {
    for (const bad of [{}, null, undefined, 'a string, not a source map', 7]) {
        const r = asciiAudit(bad);
        assert.equal(r.verdict, 'unverifiable', `${JSON.stringify(bad)} -> ${r.verdict}`);
    }
});

test('bypass: a source that is ABSENT rather than wrong refuses', () => {
    // The thing being checked is gone: a refactor drops the caption export, so
    // the audited value is undefined. "Nothing there" must never read as
    // "nothing wrong".
    const { sources } = renderedText(liveShape());
    for (const label of REQUIRED) {
        const r = asciiAudit({ ...sources, [label]: undefined },
                             { require: REQUIRED, minStrings: 30 });
        assert.equal(r.verdict, 'unverifiable', `${label} absent -> ${r.verdict}`);
        assert.ok(r.refused.some(x => x === `${label}: absent`), r.refused.join('; '));
    }
    assert.equal(asciiAudit({ caption: null }, { require: ['caption'] }).verdict, 'unverifiable');
});

test('bypass: a source DROPPED from the call site refuses', () => {
    // The partial-coverage case: someone stops passing the tooltip in, so the
    // gate would still say ascii-only while auditing less than it claims.
    const { sources } = renderedText(liveShape());
    delete sources['tooltip.burst'];
    const r = asciiAudit(sources, { require: REQUIRED, minStrings: 30 });
    assert.equal(r.verdict, 'unverifiable', describeAudit(r));
    assert.ok(r.refused.includes('tooltip.burst: missing from audit'), r.refused.join('; '));
});

test('bypass: a source holding NO strings, or an unwalkable one, refuses', () => {
    assert.equal(asciiAudit({ option: {} }).verdict, 'unverifiable');
    assert.equal(asciiAudit({ option: { grid: { top: 80 } } }).verdict, 'unverifiable');
    assert.equal(asciiAudit({ option: [] }).verdict, 'unverifiable');
    // A formatter FUNCTION is not text: refused as a source, never counted as
    // inspected (its output is passed in separately).
    const typed = asciiAudit({ caption: 42, flag: true, fn: () => 'x' });
    assert.equal(typed.verdict, 'unverifiable');
    assert.deepEqual(typed.refused, ['caption: not inspectable (number)',
        'flag: not inspectable (boolean)', 'fn: not inspectable (function)',
        'only 0 string(s) inspected, need 1']);
});

test('bypass: too few strings inspected refuses even when all are clean', () => {
    // The husk case: a builder returning an almost-empty option would
    // otherwise sail through on one clean title.
    const r = asciiAudit({ option: { title: { text: 'Peak Concurrent Sessions' } } },
                         { minStrings: 30 });
    assert.equal(r.verdict, 'unverifiable', describeAudit(r));
    assert.ok(r.refused[0].includes('need 30'), r.refused.join('; '));
    // ...and the floor is honoured, not advisory.
    assert.equal(asciiAudit({ s: 'clean' }, { minStrings: 1 }).verdict, 'ascii-only');
});

test('bypass: collectStrings skips non-strings instead of stringifying them', () => {
    // A walk that coerced numbers/null to text would both inflate the
    // inspected count and audit values nobody renders.
    assert.deepEqual(collectStrings({ a: 1, b: null, c: undefined, d: false, e: 'x', f: () => 'y' }),
        [{ path: 'e', value: 'x' }]);
});

test('bypass: the data mask hides data ONLY, so chrome glyphs stay visible', () => {
    // The mask is the one piece of this gate that could make it blind by
    // design: mask too much and a glyph in the UI's own text disappears with
    // the data. Pin both directions on the hostile fixture.
    const hostile = galleryStates['hostile-event-names'];
    assert.ok(hostile, 'the hostile-event-names fixture is what makes the mask necessary');
    const raw = renderedText(hostile.data, hostile.opts).sources;
    // Unmasked, the data's own glyphs ARE reported (the detector is not
    // ignoring the table HTML).
    const unmasked = asciiAudit(raw, { require: ['tables.html'] });
    assert.equal(unmasked.verdict, 'non-ascii');
    assert.ok(unmasked.findings.some(f => f.codePoint === 'U+1F427'), describeAudit(unmasked));
    // Masked, they are gone...
    assert.equal(asciiAudit(maskData(raw, hostile.data),
        { require: ['tables.html'], minStrings: 8 }).verdict, 'ascii-only');
    // ...but a glyph in the CHROME around them survives the mask.
    const poisoned = { ...raw, 'tables.html': raw['tables.html'].replace('Top Peak Moments', 'Top ▲ Peaks') };
    const r = asciiAudit(maskData(poisoned, hostile.data), { require: ['tables.html'], minStrings: 8 });
    assert.equal(r.verdict, 'non-ascii', describeAudit(r));
    assert.deepEqual(r.findings.map(f => f.codePoint), ['U+25B2']);
    // And a response whose event names the mask cannot account for goes RED
    // rather than clean: masking with the WRONG response leaves the data in.
    const wrong = asciiAudit(maskData(raw, { peaks: [], bursts: [] }), { require: ['tables.html'] });
    assert.equal(wrong.verdict, 'non-ascii', describeAudit(wrong));
});
