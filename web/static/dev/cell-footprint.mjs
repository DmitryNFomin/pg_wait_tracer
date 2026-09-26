/* pgwt — cell-footprint quantization for the dev gallery (issue #122: the
 * grid reflows when a cell changes size or a cell is inserted, so unrelated
 * baselines shift by a pixel or two and their snapshot checks go red).
 *
 * Pure arithmetic, no DOM reference, so it is unit-testable in plain Node
 * (tests/web_unit/cell-footprint.test.mjs). gallery.js wires this to a real
 * cell's measured height (Element.getBoundingClientRect().height) and pins
 * it back via cell.style.height.
 *
 * Why this exists: a gallery cell's NATURAL height is a fractional CSS px
 * (font/line-height metrics), and in normal document flow those fractions
 * ACCUMULATE across every cell above a given one — so inserting, removing or
 * resizing any single cell shifts every later cell's absolute page position
 * by a sub-pixel remainder. The renderer then snaps that remainder to a
 * DIFFERENT device pixel than before, repainting canvases/borders a pixel
 * off and failing that cell's snapshot even though its own CSS content never
 * changed (see `web/static/dev/gallery.html`'s #grid comment, and
 * `tests/web_snapshots/VERSION`'s "flex-wrap cascade" note — the same
 * mechanism, previously undiagnosed).
 *
 * Rounding every cell's own footprint UP to a whole pixel makes each cell's
 * contribution to that running sum exact, so the remainder is always zero:
 * a cell's rendered bytes depend only on its own content, never on its
 * neighbours' sizes or on how many cells precede it. Rounding UP (not down
 * or to nearest) matters: `.cell{overflow:hidden}` means a value that came
 * out too SMALL would clip real content, while too LARGE only adds inert
 * background — never visible, and no less deterministic (the cell's true
 * natural height for the same content is itself deterministic run to run;
 * see #155). Idempotent: quantizing an already-integer height is a no-op.
 */

/** Given a cell's current natural (possibly fractional) height in CSS px,
 * return the height to PIN it to so it always contributes a whole number of
 * pixels to the page's cumulative vertical offset. */
export function quantizeCellHeight(naturalHeightPx) {
    return Math.ceil(naturalHeightPx);
}
