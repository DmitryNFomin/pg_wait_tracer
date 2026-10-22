# 2026-10-22 — #462 (branch `agent/web-answer-ux-2`): one baseline regenerated

ONE baseline regenerated: `waterfall_execution.png`.

| baseline | change | issue |
|---|---|---|
| `waterfall_execution.png` | 1280x507 -> 1280x**483** | #462: the Waterfall chart's flat 240px `min-height` is now viewport-conditional — `@media (max-height:1000px){ min-height:216px }` — so at this fixture's render height the chart floor drops 240px -> 216px, a 24px delta, and the cell shrinks by exactly 24px (507 -> 483) |

`make ui-gallery BASE=af93c35` on this branch's tip reported the same 24px
delta against the pre-#462 local Playwright render (1280x511 -> 1280x487);
the 4px offset between that number and the committed-baseline numbers above
is the known local-Mac-vs-CI-pinned-chromium font/raster difference (RULE 2
in `../VERSION` — local Playwright is never committed for exactly this
reason). Both measurements agree on the delta that matters: 24px, matching
the 240->216 arithmetic exactly.

## How this was produced

CI `snapshots` job via `gh workflow run ci.yml --ref agent/web-answer-ux-2
-f update_snapshots=true`, run **39751589109** (snapshots job
`119733225293`, conclusion success), against commit `8f3643d` (branch tip —
the artifact's own `workflow_run.head_sha`,
`8f3643da137a6bceceb36df8158669c2ecb42146`, was checked against
`git rev-parse HEAD` before anything was copied, and matches exactly). Never
a local Playwright run, per RULE 2. Artifact digest:
`sha256:772ed75ee81319575167095f6029f1ea250f785123dac630313dd530e50f7656`.

Every one of the artifact's 47 PNGs was byte-compared (`cmp`) against the
committed baseline before anything was copied: 43 identical, 4 differing, 0
new, 0 missing. Of the 4 differing, exactly `waterfall_execution.png` is
taken.

## Left alone: three cells found differing but not regenerated

`aas_chart_overview.png` (diff-pixel ratio 0.0036), `fidelity_sampled_shading.png`
(ratio 0.0036) and `table_queries.png` (ratio 0.0004) are byte-different from
their committed baselines in the same artifact — all comfortably inside the
0.02 full-pane tolerance, and matching (to four decimal places) the exact
ratios already documented as "Left alone" in
`2026-10-21-485-scatter-chart-380px.md` the day before. This is the same
known sub-threshold drift tracked in **#483**, not a regression introduced by
this branch, which touches neither the AAS overlay-chip code, the sampled-
shading fixture, nor the table row text width. Not regenerating here is the
conservative default from that precedent: a baseline change for a cell
outside the one documented cell gets reported, not committed, so a real
regression never rides along inside mechanical churn.

Pins unchanged: `playwright==1.60.0`, container
`mcr.microsoft.com/playwright:v1.60.0-noble`. No `VERSION` change.
