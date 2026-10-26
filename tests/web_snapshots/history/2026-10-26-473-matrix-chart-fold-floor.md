# 2026-10-26 — #473 (branch `agent/591-snapshot-baselines`, off integration `agent/integration-20261026`): one baseline regenerated

ONE baseline regenerated: `transition_matrix.png`.

| baseline | change | issue |
|---|---|---|
| `transition_matrix.png` | 1240x754 -> 1240x**454** | #473: `matrixChartFitHeight` makes `#matrix-chart`'s flat 660px CSS height viewport-conditional. At the snapshot tier's 1280x800 viewport the chart's natural height overflows the fold by more than the fold reserve allows, so the fit clamps to the `MATRIX_CHART_MIN_PX` floor: 660 -> 360, and the captured `.matrix-shell` element shrinks by exactly that 300px delta (754 -> 454) |

The arithmetic is exact and is the check that this is the fix and not a
regression: `MATRIX_CHART_NATURAL_PX` (660) - `MATRIX_CHART_MIN_PX` (360) =
300 = 754 - 454, to the pixel. No other content in `.matrix-shell` changed
height: the #438 second sentence appended to `cpuNote` in the same
integration still wraps to a single line at this width, so it contributes 0px.

## Why this escaped every earlier gate on the #473 branch

`matrixChartFitHeight`'s own doc comment in `web/static/lib/builders/matrix.js`
asserts that no snapshot cell can move, because "test_web_ui_snapshots.py
captures `full_page=True`, which is exactly why this escaped the gallery for
four PR gates, same mechanism as #252". That is true of the *fixture-gallery*
cells — `gallery/matrix-dense-top20.png` is byte-identical here, and the
fixture renderer calls `buildMatrixOption` directly, never `views/matrix.js`'s
`mount()`. It is NOT true of the served-app cell: `transition_matrix` is an
**element** capture of `.matrix-shell` (`snapshot(page, "transition_matrix",
".matrix-shell")`), inside a real 1280x800 viewport, so it is precisely the
one cell the fit logic can move. Combined with `test_web_ui_snapshots` being
SKIPPED both locally and on the gate box ("CI snapshots job is
authoritative"), the CI `snapshots` job was the only instrument that could
see this, and it had not run on the integration tree until #591.

`make ui-gallery BASE=f6fdf8ec` on this tree independently reported the same
cell changing, 1240x757 -> 1240x472, at the gallery's 1710x981 DPR2 viewport.
The different absolute numbers are expected — a different viewport puts
`#matrix-chart` at a different `top`, so the fit resolves to 471px there
(above the floor) versus the floor's 360px here — and both instruments agree
the chart shrank and nothing else moved.

## How this was produced

CI `snapshots` job in UPDATE mode (`mode: UPDATE baselines`) via `gh workflow
run ci.yml --ref agent/integration-20261026 -f update_snapshots=true`, run
**40285271448** (snapshots job `121447325306`, conclusion success). Never a
local Playwright run, per RULE 2. The dispatch names the integration branch
rather than this one because this branch is deliberately unpushed; the
integration branch's remote tip is `ba60ab28`, identical to this branch's
HEAD, so the regeneration ran on exactly the tree being gated. Verified, not
assumed: the artifact's own `workflow_run.head_sha` is
`ba60ab2834fa43713066b8f316774906d1520abd` and `git rev-parse HEAD` is the
same. Artifact digest:
`sha256:5ec46c8d551d840fd0437e0ba9c38b11001f238b0fa846b41be311b8e265a8cb`.

Cross-run determinism check: the UPDATE run's `transition_matrix.png` is
**pixel-identical** (0 differing pixels, max per-channel delta 0) to the
`-actual.png` the independent COMPARE run `40282535860` uploaded on the same
tree. The two runs' PNG *bytes* differ only because `_compare` re-encodes the
actual through Pillow when it saves it.

Every one of the artifact's 47 PNGs was byte-compared (`cmp`) against the
committed baseline before anything was copied: 41 identical, 6 differing, 0
new, 0 missing. Of the 6 differing, exactly `transition_matrix.png` is taken.

## Left alone: five cells found differing but not regenerated

All five are inside their compare tolerance and all five PASSED in the
COMPARE run — `transition_matrix` was the single failure there (`46/47
snapshots matched`). Ratios below are measured against the committed
baseline with the suite's own metric (`pixel_threshold=2`, max per-channel
absolute difference per pixel):

| cell | ratio | tolerance | why left alone |
|---|---|---|---|
| `aas_chart_overview.png` | 0.0036 (1381/384000) | 0.02 | known sub-threshold drift tracked in **#483** — the same 0.0036 documented as "Left alone" in `2026-10-21-485-scatter-chart-380px.md` and `2026-10-22-462-waterfall-viewport-floor.md`. This branch touches no AAS overlay-chip code |
| `fidelity_sampled_shading.png` | 0.0036 (1381/384000) | 0.02 | same #483 drift, same ratio as both prior entries; no sampled-shading fixture change here |
| `table_queries.png` | 0.0004 (223/512000) | 0.02 | same #483 drift (prior entries record 0.0004); no table row-text change here |
| `gallery/uplot-aas-live-ticks-tick4.png` | 0.0000 (0/274300 at `pixel_threshold=2`; 14 px differ at threshold 0) | 0.002 | byte-different but visually identical — 14 pixels move by a single channel step. Below even the tight gallery tolerance |
| `table_sessions.png` | 0.0058 (1930/335360) | 0.02 | **this integration's own doing**, not #483: the #439 / #523 scope-note caption text. Confined to the caption, comfortably inside tolerance, and PASSED the COMPARE run at 0.0039. Note the committed `table_sessions.png` blob is already identical between the merge-base `f6fdf8ec` and this tree — #439's regeneration (`fd96b9a7`, entry `2026-10-25-439-sessions-scope-note.md`) is already on master — so this is post-#439 caption drift, not a missing regeneration |

Not regenerating these is the conservative default set by
`2026-10-22-462-waterfall-viewport-floor.md`: a baseline change outside the
one documented cell gets reported, not committed, so a real regression never
rides along inside mechanical churn. `table_sessions` is reported here rather
than silently refreshed for exactly that reason.

## Flexbox neighbour-drift (#122): not in play

`#matrix-chart` is not a gallery flex cell and has no row-mates to re-wrap;
the shrink is a direct `style.height` assignment on one element. The five
left-alone cells are not row-mates of it either — three are the independently
tracked #483 drift and one is a caption text change. Nothing unexpected moved.

## One thing this baseline now bakes in, flagged rather than buried

The regenerated PNG contains #438's second `cpuNote` sentence — "Every
transition in this workload passes through CPU* (wait -> CPU -> wait, never
wait -> wait) -- the L shape is the data, not a rendering bug." In
`buildMatrixOption`, `allLinksTouchCpu` is computed over `links`, which is
the link set the **server already capped**: this same capture's footer reads
"Server returned 5 of 8 transition links (link cap)". So the claim is
asserted from 5 of 8 links, and whether any of the 3 un-sent links is a
wait -> wait is not knowable client-side. #438's guard covers client-side
top-N *label* truncation, which is what its commit message describes; it does
not cover the server-side link cap. That is #438's scope, not this
regeneration's, and it is not what failed the gate (the failure was the size
change above) — recorded here so that committing this baseline does not
quietly become the thing that settles the question.

Pins unchanged: `playwright==1.60.0` (both runs print `Version 1.60.0`),
container `mcr.microsoft.com/playwright:v1.60.0-noble`. No `VERSION` change.
