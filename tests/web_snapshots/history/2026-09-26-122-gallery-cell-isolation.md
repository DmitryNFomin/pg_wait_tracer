# 2026-09-26 — #122: gallery cell isolation

2026-09-26 (issue #122): ALL 28 gallery/*.png REGENERATED — root cause and
fix, not a content change. web/static/dev/gallery.html's #grid stacked
cells in normal document flow with each cell's fractional (font-metric)
natural height accumulating into every LATER cell's absolute page
position; el.screenshot()'s crop rect is computed from that fractional
top/bottom, so inserting, removing or resizing any ONE cell shifted the
sub-pixel remainder for every cell below it and re-snapped its canvas/
borders to a different device pixel — the "flex-wrap cascade" already
named (undiagnosed) in the #159 note above and the literal #122 repro (one
new fixture cell perturbing gallery/matrix-dense-top20 and four
table-configs/aas-live-ticks-tick4 baselines by 0.5-2.1%). Fixed by
pinning every cell to an integer CSS px height once its content settles
(gallery.js snapCellFootprint / cell-footprint.mjs quantizeCellHeight) so
the running sum of preceding heights is always a whole number — verified
with a temporary probe cell: on master, inserting/removing it changed 8
unrelated cells' rendered sha256 (matrix, all three table-configs,
histogram, concurrency, waterfall, exec-scatter) despite unchanged
reported dimensions; with the fix, the same experiment leaves every one
of them byte-identical (see PR for the full before/after).

Consequence: EVERY gallery cell's OWN reported height is now the honest
ceil() of its true content height instead of the old accidental
round(bottom)-round(top) value, so all 28 changed — 9 shrank by EXACTLY
1px (aas-dense, aas-live-ticks-tick4, aas-sampled, histogram-dense,
table-configs-compare-delta, timeline-dense-50pids, uplot-aas-compare-
ghostdiff, uplot-aas-dense-events, uplot-aas-escalated-live-edge — the old
accidental rounding had gone up, never down, at these 9) and the other 19
are the SAME dimensions, sub-pixel-antialiasing only (0.008-7.2% at that
SAME size — text/border/canvas edges, diffed pixel-for-pixel against the
pre-regen file: identical content, no row/column/series/label ever moved,
added or dropped; spot-checked visually for fidelity-compare-predates
(7.2%, highest of the 19), table-configs-queries-hostile-sql (4.0%) and
matrix-dense-top20 (1.2%) — all purely glyph-hinting/AA shifts from the
corrected sub-pixel offset, same cells and same shrink-only direction the
#122 repro (one inserted fixture cell) already showed on master).
CORRECTION (found during the #122 follow-up review below): a reader
comparing one of the OTHER 9 — the ones whose HEIGHT changed by 1px —
against the pre-regen file directly will see a much bigger raw diff
ratio than 7.2%, because a full-image byte/pixel diff over two images
offset by one row is comparing almost every row against its neighbour,
not against itself; table-configs-compare-delta measured ~10.7% that way.
That is not a bigger antialiasing effect, it is the expected shape of
"the whole image moved one row" — the 0.008-7.2% range above was, and
remains, only the 19 SAME-dimension cells; it was never meant to bound
the 9 dimension-changed ones, but did not say so explicitly enough not to
read as if it did. No other baseline (the 16 non-gallery panes) changed:
the fix touched only web/static/dev/gallery.{html,js}, which the app
pages never load.

Generated via workflow_dispatch update_snapshots=true on
agent/gallery-grid-stable (commit 8fce6f546a381fbc068113f69d6b080a8819e9a8,
run 36243882497, snapshots job 108409339734); sha256-verified from the
downloaded snapshot-baselines artifact before committing, same pinned
environment as above (playwright==1.60.0 /
mcr.microsoft.com/playwright:v1.60.0-noble — unchanged, no version bump).

2026-09-26 (issue #122 follow-up, reviewer blocker): 15 gallery/*.png
REGENERATED — the height-quantization fix above closed the DOCUMENT-
position coupling but not all of it. A fresh reviewer probe (duplicate
'single-point' before 'dense' in aas.mjs) found gallery/aas-dense and
gallery/aas-dense-events still moved for a REAL gate failure (644/245,050
px, 0.263%, over the 0.002% limit) with the height-only fix, plus three
more cells (uplot-aas-dense, fidelity-compare-mismatch, fidelity-compare-
predates) moving a handful of pixels below the gate's tolerance but still
not byte-identical. Diagnosed via getBoundingClientRect()/scrollY
(tests/test_web_ui_snapshots.py had no unit-testable component here, so
this was measured directly, not asserted): every document-relative
top/height was already an exact integer in both cases — the residual
had TWO further causes, neither a leftover fractional CSS pixel:
  1. el.screenshot()'s own auto-scroll-into-view can pick a FRACTIONAL
     scrollTop for an odd-height cell against the even 800px viewport (an
     alignment-math artifact). Fixed: snapshot() now scrollIntoView
     ({block:'start'}) itself first, at the cell's own integer document Y.
  2. Text/canvas content painted at a DIFFERENT absolute page Y — even a
     whole-pixel one — can still rasterize a handful of pixels a shade
     differently (a browser raster-tile/hinting effect, not a CSS bug).
     Fixed at the root: gallery.html/gallery.js gained ?isolate=<cellId>,
     rendering ONLY that one cell; snap_gallery_suite now loads each
     gallery cell from its own isolated page (28 small local loads)
     instead of cropping it out of the shared, every-fixture page, so a
     cell's absolute paint position can never depend on the rest of the
     manifest again.

Re-verified BOTH the reviewer's aas.mjs probe and the original #122
timeline.mjs probe with the isolated-capture code: inserting/removing
either probe now leaves all 28 gallery cells byte-identical (sha256),
confirmed twice per probe, not the gate's tolerance — the actual bar this
entry's title claims.

Of the 15: all 15 kept their EXISTING dimensions (isolation only changes
which pixels a cell's own content rasterizes to, never its size).
aas-dense/aas-mixed-escalation/uplot-aas-dense-events/uplot-aas-mixed-
escalation are near the 0.263% mark seen in the reviewer's repro (0.26%,
same order as the bug that was fixed — the isolated capture simply
differs from the OLD shared-page capture by about that much, which is
the point); the rest are 0.03% or under. Spot-checked visually
(aas-dense, table-configs-events-overflow-pctl, timeline-dense-one-row):
identical chart geometry/data/table values/bar-column counts in every
case, AA-edge pixels only — see PR for the side-by-side crops. No
non-gallery baseline changed.

Generated via workflow_dispatch update_snapshots=true on
agent/gallery-grid-stable (commit ab5f01e513204170e2bb219bea47572eda9fcdf1,
run 36249340836, snapshots job 108424319552); sha256-verified from the
downloaded snapshot-baselines artifact before committing, same pinned
environment as above (unchanged, no version bump).
