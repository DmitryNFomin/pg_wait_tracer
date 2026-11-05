/* #279: every checklist citation must name a real, captured gallery cell;
 * the truncation fixtures must actually fire the real builders' disclosures. */
import { test } from 'node:test';
import assert from 'node:assert/strict';
import { readFileSync } from 'node:fs';
import { MANIFEST, FIXTURES } from '../../web/static/dev/fixtures/manifest.mjs';
import { p95TruncationMarker } from '../../web/static/lib/builders/transitions.js';
import {
    buildConcurrencyOption, buildBurstsPanelHtml, burstPidSampleMarker,
} from '../../web/static/lib/builders/concurrency.js';

const checklist = readFileSync(new URL('../../docs/VISUAL_CHECKLIST.md', import.meta.url), 'utf8');
const snapshots = readFileSync(new URL('../test_web_ui_snapshots.py', import.meta.url), 'utf8');
const captureLists = [...snapshots.matchAll(/^GALLERY_(?:STATIC|TICK)_CELLS = \[([\s\S]*?)^\]/gm)];
assert.equal(captureLists.length, 2, 'must parse both snapshot capture lists');
const captured = new Set(captureLists.flatMap(([, body]) =>
    [...body.matchAll(/["'](gallery-[a-z0-9-]+)["']/g)].map(m => m[1])));
const staticList = snapshots.match(/^GALLERY_STATIC_CELLS = \[([\s\S]*?)^\]/m);
const staticCaptured = new Set([...staticList[1].matchAll(/"(gallery-[a-z0-9-]+)"/g)]
    .map(m => m[1]));
// The Python declaration uses JSON string literals and ("text", regex)
// tuples. Convert only those tuples, preserving parentheses inside strings.
const evidenceBlock = snapshots.match(/^GALLERY_EVIDENCE = (\{[\s\S]*?^\})/m);
assert.ok(evidenceBlock, 'must parse the evidence declaration');
const evidence = JSON.parse(evidenceBlock[1]
    .replace(/\("text", ("(?:\\.|[^"\\])*")\)/g, '["text", $1]')
    .replace(/,\s*\}$/, '}'));

test('#279: every static capture declares non-empty, valid evidence and vice versa', () => {
    assert.deepEqual(Object.keys(evidence).sort(), [...staticCaptured].sort());
    for (const [id, items] of Object.entries(evidence)) {
        assert.ok(Array.isArray(items) && items.length > 0, `${id}: evidence required`);
        for (const item of items) {
            if (typeof item === 'string') {
                assert.ok(item.trim(), `${id}: empty selector`);
            } else {
                assert.ok(Array.isArray(item) && item.length === 2 && item[0] === 'text' &&
                    typeof item[1] === 'string' && item[1].trim(), `${id}: invalid text evidence`);
                assert.doesNotThrow(() => new RegExp(item[1]), `${id}: invalid regex`);
            }
        }
    }
});

function coverageErrors(doc, captures, manifest = MANIFEST) {
    const ids = new Set(manifest.map(entry => entry.cellId));
    const citations = new Set(doc.match(/\bgallery-[a-z0-9]+(?:-[a-z0-9]+)*(?:-\*)?/g) || []);
    const errors = [];
    for (const citation of citations) {
        const matches = citation.endsWith('-*')
            ? [...ids].filter(id => id.startsWith(citation.slice(0, -1)))
            : ids.has(citation) ? [citation] : [];
        if (!matches.length) errors.push(`${citation}: not in fixture manifest`);
        for (const id of matches) {
            if (!captures.has(id)) errors.push(`${id}: not captured by snapshot suite`);
        }
    }
    return errors;
}

test('#279: every checklist gallery citation exists and is captured (including globs)', () => {
    assert.deepEqual(coverageErrors(checklist, captured), []);
    for (const id of captured) {
        assert.ok(MANIFEST.some(entry => entry.cellId === id), `${id}: captured cell not in manifest`);
    }
});

test('#279: coverage guard detects a missing cited capture (negative control)', () => {
    const missing = 'gallery-synthetic-removed';
    const kept = 'gallery-synthetic-kept';
    const doc = `Check ${missing} and ${kept}.`;
    const manifest = [missing, kept].map(cellId => ({ cellId }));
    const incomplete = new Set([missing, kept]);
    assert.deepEqual(coverageErrors(doc, incomplete, manifest), []);
    assert.ok(incomplete.delete(missing));
    assert.deepEqual(coverageErrors(doc, incomplete, manifest), [
        `${missing}: not captured by snapshot suite`,
    ]);
});

test('#279: coverage guard detects unknown cells and expands wildcard citations', () => {
    assert.deepEqual(coverageErrors('`gallery-missing-state`', captured), [
        'gallery-missing-state: not in fixture manifest',
    ]);
    const missing = 'gallery-fidelity-unavailable-no-daemon';
    const incomplete = new Set(captured);
    assert.ok(incomplete.delete(missing));
    assert.deepEqual(coverageErrors('`gallery-fidelity-unavailable-*`', incomplete), [
        `${missing}: not captured by snapshot suite`,
    ]);
    assert.deepEqual(coverageErrors('`gallery-missing-*`', captured), [
        'gallery-missing-*: not in fixture manifest',
    ]);
});

test('#279: variants-html fires p95 truncation markers and includes a complete sample', () => {
    const variants = Object.values(FIXTURES.transitions['variants-html'].variants)
        .flatMap(section => section.variants);
    const markers = variants.map(p95TruncationMarker);
    assert.ok(markers.some(marker => marker !== ''), 'must disclose at least one truncated p95 sample');
    assert.ok(markers.some(marker => marker === ''), 'must leave at least one complete sample unmarked');
});

test('#279: truncated-bursts fires the selection note and only marks sampled PID lists', () => {
    const fixture = FIXTURES.concurrency['truncated-bursts'];
    assert.equal(typeof fixture.data.bursts_total, 'number');
    assert.ok(fixture.data.bursts.length > 0);
    assert.ok(fixture.data.bursts_total > fixture.data.bursts.length);
    const model = buildConcurrencyOption(fixture.data, fixture.opts);
    assert.ok(model.burstNote.includes('Showing the largest burst'), 'must disclose burst-onset selection');
    const markers = model.bursts.map(burstPidSampleMarker);
    assert.ok(markers.some(marker => marker !== ''), 'must disclose at least one PID sample');
    assert.ok(markers.some(marker => marker === ''), 'must leave at least one complete PID list unmarked');
    const html = buildBurstsPanelHtml(model);
    assert.ok(html.includes(model.burstNote), 'selection note must reach the rendered panel');
    for (const marker of markers.filter(Boolean)) {
        assert.ok(html.includes(marker), 'PID sample marker must reach the rendered panel');
    }
});

test('#279: dense-bursts was blind to the burst selection disclosure', () => {
    const fixture = FIXTURES.concurrency['dense-bursts'];
    // No bursts_total and sparse chart markers: this previously captured
    // fixture cannot exercise the onset-selection note or the noise note.
    assert.equal(buildConcurrencyOption(fixture.data, fixture.opts).burstNote, '');
});
