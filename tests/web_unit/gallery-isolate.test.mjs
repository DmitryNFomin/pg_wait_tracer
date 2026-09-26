/* Node unit tests for web/static/dev/gallery.js's selectDisplayEntries —
 * the #122 follow-up isolation mode (?isolate=<cellId>): a gallery capture
 * page renders ONLY that one cell instead of the whole fixture corpus, so a
 * cell's rendered bytes can never depend on what else the manifest
 * contains (closing the residual coupling pinning heights alone did not:
 * see gallery.js's doc comment and tests/test_web_ui_snapshots.py's
 * snap_gallery_suite). Pure (no DOM) — gallery.js's main() wires this to
 * `new URLSearchParams(location.search).get('isolate')` and #grid.
 */
import { test } from 'node:test';
import assert from 'node:assert/strict';
import { selectDisplayEntries } from '../../web/static/dev/gallery.js';

const ENTRIES = [
    { cellId: 'gallery-aas-empty' },
    { cellId: 'gallery-aas-dense' },
    { cellId: 'gallery-aas-dense-events' },
];

test('no isolateId returns every entry, unchanged order', () => {
    assert.deepEqual(selectDisplayEntries(ENTRIES, null), ENTRIES);
    assert.deepEqual(selectDisplayEntries(ENTRIES, ''), ENTRIES);
});

test('a matching isolateId returns ONLY that one entry', () => {
    const got = selectDisplayEntries(ENTRIES, 'gallery-aas-dense');
    assert.deepEqual(got, [{ cellId: 'gallery-aas-dense' }]);
});

test('a non-matching isolateId returns an EMPTY array, never the full set', () => {
    // A typo'd cellId must render a visibly empty page (loud), not silently
    // fall back to "show everything" (which would defeat the isolation the
    // caller asked for and could quietly reintroduce the #122 coupling).
    const got = selectDisplayEntries(ENTRIES, 'gallery-does-not-exist');
    assert.deepEqual(got, []);
});
