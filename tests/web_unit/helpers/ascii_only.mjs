/* ASCII-only audit for strings a builder RENDERS (#388).
 *
 * WHY THIS EXISTS. PR #388's snapshot job failed `waterfall_execution` at
 * 505 px against a 509 px baseline, in a container whose digest was identical
 * to the passing run's. The cause was not layout: #384's new panel text
 * carried "▲" and "—", and a non-ASCII glyph forces Chromium to load a
 * fallback font, which flips the `monospace` generic for everything laid out
 * AFTER it in the same page session. `.query-id` ('SF Mono','Fira Code',
 * monospace) then rendered "100" as 20x9 instead of 16x10, and four table
 * rows each lost a pixel. The snapshot suite captures the waterfall cell
 * immediately after the concurrency tab, so the panel text of one view
 * silently resized the typeface of the next.
 *
 * So: a glyph outside ASCII in text the UI renders is a layout hazard, not a
 * typography preference. This audit is the gate.
 *
 * IT MUST REFUSE RATHER THAN APPROVE when it cannot see: an empty source set,
 * a source that is absent (undefined/null), a source that holds no strings at
 * all, a source of a type it cannot walk, or a required label missing from
 * the audit all return verdict 'unverifiable'. A gate that silently inspects
 * nothing reads exactly like a clean one, which is the false negative this
 * whole file is about.
 */

/* Every non-ASCII code point, matched as a code POINT (the /u flag) so an
 * astral character is reported once, not as two lone surrogates. Global: a
 * string carrying BOTH "▲" and "—" must report both — a report that stops
 * at the first glyph sends the fixer back for a second round. */
const NON_ASCII = /[^\x00-\x7F]/u;
const NON_ASCII_ALL = /[^\x00-\x7F]/gu;

/* Recursively collect every string reachable from `value`, each with the path
 * it was found at. Numbers, booleans, null/undefined and FUNCTIONS are not
 * strings and are skipped (an ECharts option is mostly formatters; their
 * OUTPUT is audited by passing it in as its own source). */
export function collectStrings(value, path = '', out = []) {
    if (typeof value === 'string') { out.push({ path, value }); return out; }
    if (Array.isArray(value)) {
        value.forEach((v, i) => collectStrings(v, `${path}[${i}]`, out));
        return out;
    }
    if (value && typeof value === 'object') {
        for (const k of Object.keys(value)) {
            collectStrings(value[k], path ? `${path}.${k}` : k, out);
        }
        return out;
    }
    return out;
}

/* sources: { label -> string | object }  (objects are walked recursively)
 * opts.require: labels that MUST be present; a missing one is a refusal, so
 *   dropping a source from the call site cannot quietly shrink the gate.
 * opts.minStrings: floor on how many strings were actually inspected; an
 *   emptied builder output cannot pass by having nothing to say.
 *
 * Returns { verdict, checked, strings, findings[], refused[] } where verdict
 * is 'unverifiable' (could not see) | 'non-ascii' (found some) | 'ascii-only'.
 */
export function asciiAudit(sources, opts = {}) {
    const required = opts.require || [];
    const minStrings = opts.minStrings || 1;
    const refused = [];
    if (!sources || typeof sources !== 'object') {
        return { verdict: 'unverifiable', checked: 0, strings: 0, findings: [],
                 refused: ['no sources given'] };
    }
    const entries = Object.entries(sources);
    if (entries.length === 0) refused.push('no sources given');
    const strings = [];
    for (const [label, v] of entries) {
        if (v === undefined || v === null) { refused.push(`${label}: absent`); continue; }
        if (typeof v !== 'string' && typeof v !== 'object') {
            refused.push(`${label}: not inspectable (${typeof v})`);
            continue;
        }
        const got = collectStrings(v, label, []);
        if (got.length === 0) { refused.push(`${label}: nothing to inspect`); continue; }
        strings.push(...got);
    }
    for (const label of required) {
        if (!Object.prototype.hasOwnProperty.call(sources, label)) {
            refused.push(`${label}: missing from audit`);
        }
    }
    if (strings.length < minStrings) {
        refused.push(`only ${strings.length} string(s) inspected, need ${minStrings}`);
    }
    const findings = [];
    for (const s of strings) {
        if (!NON_ASCII.test(s.value)) continue;
        const seen = new Set();
        for (const m of s.value.matchAll(NON_ASCII_ALL)) {
            const ch = m[0];
            if (seen.has(ch)) continue;   // one finding per (path, glyph)
            seen.add(ch);
            findings.push({
                path: s.path, char: ch,
                codePoint: 'U+' + ch.codePointAt(0).toString(16).toUpperCase().padStart(4, '0'),
                value: s.value,
            });
        }
    }
    const verdict = refused.length ? 'unverifiable'
        : (findings.length ? 'non-ascii' : 'ascii-only');
    return { verdict, checked: entries.length, strings: strings.length, findings, refused };
}

/* One-line explanation for an assertion message. */
export function describeAudit(r) {
    if (r.verdict === 'unverifiable') return `unverifiable: ${r.refused.join('; ')}`;
    if (r.verdict === 'non-ascii') {
        return 'non-ASCII in rendered text: ' + r.findings
            .map(f => `${f.path} has ${f.codePoint} (${JSON.stringify(f.char)}) in ${JSON.stringify(f.value)}`)
            .join(' | ');
    }
    return `ascii-only across ${r.strings} string(s) from ${r.checked} source(s)`;
}
