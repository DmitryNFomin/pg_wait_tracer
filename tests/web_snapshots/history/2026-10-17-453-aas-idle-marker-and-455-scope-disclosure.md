# 2026-10-17 — #453 AAS idle-exclusion marker + #455 DB-Time-vs-Exec scope disclosure

FOUR baselines regenerated, each explained by a specific fix on
`agent/gif-presentation`. SEVEN further cells in the same CI artifact
differ but are **deliberately NOT regenerated** — see "Seven cells left on
the known AAS-chart/uPlot drift" below.

| baseline | change | fix |
|---|---|---|
| `table_overview.png` | ratio 0.0305 (own gate 0.02 — exceeds it; content change, not noise) | #453: Idle row's AAS cell was a bare rate ("0.00") under the "AAS" header with only a hover tooltip; now the same visible em-dash `%DB Time` uses, with the occupancy figure printed labeled beside it ("— (idle occ. 0.00)") |
| `table_queries.png` | 1280x161 -> 1280x**175** (+14px) | #455: new static, always-visible scope-disclosure line beneath the table ("DB Time includes waits acquired before execution starts…") |
| `waterfall_execution.png` | ratio 0.0487 (own gate 0.02 — exceeds it; size unchanged, 1280x444, content shifted down) | #455: same disclosure added above the Executions list ("Duration and Plan measure executor/planner time only…") |
| `gallery/exec-scatter-dense-downsampled.png` | 650x437 -> 650x**453** (+16px) | reworded the downsample note from "(server-downsampled)" (read as a uniform sample) to "(outlier-preserving sample; point density does not represent frequency)" — the sampler itself (`src/server.c:5506`) is unchanged |

Every one of these four was independently traced to its own code diff
*before* regeneration, via `make ui-gallery` (BASE=`origin/master`=`ee26ae6`
vs HEAD) on this branch — not inferred from the regenerated artifact after
the fact. `make ui-gallery`'s own report: 4 changed, 0 added, 0 removed, 43
unchanged, exactly matching this table's four rows
(`tests/results/ui_gallery/summary.json`).

## How these were produced

CI `snapshots` job via `gh workflow run CI --ref agent/gif-presentation -f
update_snapshots=true` (run **39032637499**, snapshots job conclusion
success) — never a local Playwright run, per RULE 2. All 47 cells in the
`snapshot-baselines` artifact were diffed by sha256 against the committed
baselines; 11 differed in total (this table's four, plus the seven
pre-existing known-drift cells below). Only the four explained cells were
copied out of the artifact; sha256 verified identical to the artifact after
copying:

    table_overview.png                              d22a9448397050b543efdf3d71259baf5cd1ef6c592fc59295e0f6530c7635a6
    table_queries.png                               43ec1f1da09e6038b982cd911fc04d462531895d31487b2c8554d8a1b543a604
    waterfall_execution.png                         5bb21b3e367781f0f5b22badf0d66b5d82c2251061a6f00a3c1fd413353dfc7d
    gallery/exec-scatter-dense-downsampled.png      c99b4ee322208bfb6a7bd9f8d2e5130f15aab8d8ba5b04b1d3b230ed585fecd8

Pins unchanged: `playwright==1.60.0`, chromium build 1223, version
148.0.7778.96, container `mcr.microsoft.com/playwright:v1.60.0-noble`.

## Seven cells left on the known AAS-chart/uPlot drift — not regenerated

The same artifact's other seven differing baselines are the SAME
session-to-session drift this directory has already characterized (no code
change touches AAS/overlay-chip/uPlot rendering on this branch):

- `aas_chart_overview.png`
- `fidelity_sampled_shading.png`
- `gallery/uplot-aas-compare-ghostdiff.png`
- `gallery/uplot-aas-dense-events.png`
- `gallery/uplot-aas-dense.png`
- `gallery/uplot-aas-live-ticks-tick4.png`
- `gallery/uplot-aas-sampled.png`

These are left deliberately unrefreshed, same standing policy as
`history/2026-10-17-449-integration-baselines.md`'s "Seven cells left on
the pre-#213 rendering" section: refreshing them here would absorb a
pre-existing, independently-tracked drift (#263) into this change's history
entry, and leaving the gap open is what keeps a FUTURE real move on these
cells detectable against the last deliberately-set baseline. (Two other
`gallery/uplot-aas-*` cells — `escalated-live-edge` and
`mixed-escalation` — did NOT drift in this run; the known-drift set is not
all-or-nothing every run.)

No baseline outside these eleven (the four regenerated here, the seven
left alone) differed from the committed tree in this artifact.
