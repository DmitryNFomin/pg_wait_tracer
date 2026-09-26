# 2026-09-26 — #107: transitions DFG hideIdle default + real-workload fixture

2026-09-25 (#107, transitions DFG hideIdle default + gallery-gate blocker
fix): TWO baselines REGENERATED and TWO gallery baselines ADDED —
transitions_dfg.png, transition_matrix.png,
gallery/transitions-idle-loop-dominant.png,
gallery/transitions-idle-loop-hidden.png. tests/mock_server.py's canned
"transitions" payload gained a dominant Client:ClientRead node/edge pair
(900/560 vs 100/80/30 for everything else) so transitions_dfg actually
exercises the new hideIdle-default-on behavior end to end (previously it
sat at 0.0% unchanged regardless of the default, a reviewer-flagged gate
hole); transition_matrix renders the same "transitions" payload via a
different builder and shifts as a same-cause side effect (no matrix.js
change). The two new dev/gallery.html fixture cells (idle-loop-dominant =
pre-fix shape with hideIdle off, idle-loop-hidden = the shipped default)
are now in GALLERY_STATIC_CELLS so they gate going forward instead of
only rendering in the interactive gallery page.
`make ui-gallery BASE=$(git merge-base HEAD origin/master)`: 12 changed, 2
added, 0 removed, 32 unchanged. Of the 12 changed, only transitions_dfg
(10.25%) and transition_matrix (10.19%) are from this branch; the other
10 (concurrency-dense-bursts, exec-scatter-dense-downsampled,
fidelity-compare-mismatch, matrix-dense-top20, waterfall-dense-plan-
3lanes, table-configs-{compare-delta,events-overflow-pctl,queries-
hostile-sql}, fidelity-compare-predates, uplot-aas-compare-ghostdiff) are
untouched by any commit on this branch (git diff confirms none of their
builders/fixtures changed) — this branch predates the render-settle fix
(#158), which is why they still show run-to-run rasterization drift on a
local before/after render. Regenerated via CI run 36155152277
(workflow_dispatch update_snapshots=true on agent/transitions-real-
workload, ubuntu-latest, pins already recorded above); sha256 verified
against the downloaded artifact before committing; no local PNG
committed. The artifact's other 33 baselines byte-differ from what's
committed (expected non-reproducible PNG encoding across separate CI
runs per the note at the top of this file) but were left untouched —
only the four files above, which this branch's code/fixture changes
actually explain, were copied in.

2026-09-26 (#107 x #159 merge, re-pin under the container): merging
master (which carries #159's container-pinned rendering environment,
entry above) into this branch re-rendered this branch's own four changed
cells via workflow_dispatch update_snapshots=true on
agent/transitions-real-workload (run 36241695209):
  transition_matrix.png (sha256 1a3658ab...9d9f499e) and
  transitions_dfg.png (sha256 0fc88cb1...97a4097e) came back
  BYTE-IDENTICAL to this branch's pre-merge (pre-#159) committed bytes —
  no font-metric drift touches either cell under the container.
  gallery/transitions-idle-loop-dominant.png (sha256 f911535f...03b09a78)
  and gallery/transitions-idle-loop-hidden.png (sha256 21b3101a...b46ac5db)
  are the two ADDED cells; sha256-verified against the artifact before
  committing. Diffed transition_matrix and transitions_dfg against
  master's current (pre-#107) baselines: 10.93% and 11.10% respectively —
  matching this branch's own documented `make ui-gallery` ratios (10.19%,
  10.25%) closely enough to confirm the diff is exactly the #107 mock
  fixture change (the new dominant Client:ClientRead node/edge), nothing
  else moved. All 4 files are exactly this branch's own changed/added set
  (git diff against its merge-base confirms it); the other 42 baselines in
  the artifact were left at master's container-pinned bytes already in
  the merge.

2026-09-26 (#107 x #122 merge, re-regenerate the two new gallery cells
under isolated capture): merging master's #122 gallery-cell-isolation fix
(see history/2026-09-26-122-gallery-cell-isolation.md, PR #179) produced
no binary conflict on this branch's two ADDED cells (git has no prior
version of them to conflict with), so they stayed at this branch's
pre-#122 bytes after the merge — but the real `snapshots` CI run on the
merged commit (run 36267887359) then FAILED the gate on exactly those two
cells: gallery/transitions-idle-loop-dominant (0.0097 > 0.002, 3703/382850
px) and gallery/transitions-idle-loop-hidden (0.0094 > 0.002, 3490/372450
px). Both cells' bytes had been captured by the OLD, pre-isolation
mechanism (cropped out of the shared, every-fixture gallery page) at
regen time, before #122's per-cell isolated-page capture existed; every
OTHER cell in the same failing run passed at an exact 0.0000, confirming
this is specifically the capture-mechanism change, not fixture drift.
Regenerated both via workflow_dispatch update_snapshots=true on this
branch, now merged with master (run 36268854503), sha256-verified against
the downloaded artifact before committing:
  gallery/transitions-idle-loop-dominant.png (sha256
  e70ae534...9f42313d3): diffs 0.97% against the pre-merge bytes above
  (3703/382850 px), localized to a bottom band of the cell (bbox y=534-588
  of 589 total rows) — not a whole-image shift or any change to the DFG
  content itself.
  gallery/transitions-idle-loop-hidden.png (sha256 7f5f4667...bbdd63068bf2):
  diffs 0.94% (3490/372450 px), same bottom-band localization (bbox
  y=534-572 of 573 total rows).
Both localized-to-the-bottom-edge diffs match the isolated-capture
mechanism's own documented effects (auto-scroll-into-view landing and
absolute-page-Y raster hinting, both edge-of-cell effects) rather than any
new, unexplained geometry or DFG-node/edge change. transitions_dfg.png,
transition_matrix.png (this branch's other two, pre-existing cells) are
untouched by #122 and keep the bytes from the entry above.
