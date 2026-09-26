/* Node unit tests for web/static/dev/cell-footprint.mjs — the gallery
 * cell-height quantization behind issue #122 (the grid reflows when a cell
 * changes size or is inserted, so unrelated baselines shift by a pixel or
 * two). Pure arithmetic, no DOM — gallery.js wires this to a real cell's
 * measured height (Element.getBoundingClientRect().height); these tests pin
 * the rounding policy only:
 *   - a fractional natural height rounds UP to the next integer px (never
 *     down or to nearest — `.cell{overflow:hidden}` means anything smaller
 *     than the true content height would clip it; larger only adds inert,
 *     invisible background)
 *   - an already-integer height is returned unchanged (idempotent — running
 *     this on a cell whose content did not change must not itself perturb
 *     the page's cumulative vertical offset)
 *   - the fix's actual claim: summing quantized (integer) heights can never
 *     produce a fractional running total, so a later cell's absolute page
 *     position never carries a sub-pixel remainder from an earlier cell's
 *     resize/insertion/removal.
 */
import { test } from 'node:test';
import assert from 'node:assert/strict';
import { quantizeCellHeight } from '../../web/static/dev/cell-footprint.mjs';

test('quantizeCellHeight rounds a fractional height UP to the next integer', () => {
    assert.strictEqual(quantizeCellHeight(628.2), 629);
    assert.strictEqual(quantizeCellHeight(600.4), 601);
    assert.strictEqual(quantizeCellHeight(0.001), 1);
});

test('quantizeCellHeight leaves an already-integer height unchanged (idempotent)', () => {
    assert.strictEqual(quantizeCellHeight(600), 600);
    assert.strictEqual(quantizeCellHeight(0), 0);
    assert.strictEqual(quantizeCellHeight(quantizeCellHeight(628.2)), 629);
});

test('quantizeCellHeight never rounds DOWN (would clip content under overflow:hidden)', () => {
    for (const h of [1.0001, 99.9999, 250.5]) {
        assert.ok(quantizeCellHeight(h) >= h, `${quantizeCellHeight(h)} < ${h}`);
    }
});

test('summing quantized heights is always an integer, however many cells precede a given one', () => {
    // The property the fix actually relies on: any run of fractional
    // natural heights (as real cells report — see gallery/*'s committed
    // baseline heights) sums to a whole number once each is quantized, so a
    // later cell's cumulative top offset is exact regardless of how many
    // cells were inserted/removed/resized above it.
    const naturalHeights = [628.2, 248.3, 286.6, 226.9, 436.4, 600.4, 358.4];
    for (let n = 1; n <= naturalHeights.length; n++) {
        const total = naturalHeights.slice(0, n)
            .map(quantizeCellHeight)
            .reduce((a, b) => a + b, 0);
        assert.strictEqual(total, Math.round(total));
    }
});
