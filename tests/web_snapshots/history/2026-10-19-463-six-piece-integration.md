# 2026-10-19 — #463: integrating the #455 backend active span, #454/#456 tab fixes, #459 nested executions and footage mode

EIGHT baselines REGENERATED, each traced to a specific piece of the
six-piece integration (PR #463). SEVEN further cells in the same CI artifact
differ and are **deliberately NOT regenerated** — they are #263 again, see
the last section.

| baseline | change | piece / issue |
|---|---|---|
| `table_queries.png` | 1280x175 -> 1280x**325** (+150px) | span UI wiring, #455 (the `Backend activity (observed span)` column, plus the collapsed backend-activity panel) |
| `table_events.png` | 1280x277 -> 1280x**305** (+28px) | web tab fixes, #454 (the `≤`/`≥` percentile qualification note) |
| `concurrency_burst_table.png` | 1240x525 -> **602x130** | web tab fixes, review finding adjacent to #442 (Peaks and Bursts split into independent panels, so the burst table is its own narrower cell) |
| `waterfall_execution.png` | 1280x444 -> 1280x**460** (+16px) | integration finding B, #455 (the collapsed backend-activity line, moved BELOW the per-bar readout) |
| `execution_scatter.png` | ratio 0.0072 (passed its own gate) | web tab fixes, #455 UI (axis renamed to Execution duration; pre-execution gap disclosed unconditionally) |
| `gallery/table-configs-events-overflow-pctl.png` | 650x345 -> 650x**393** (+48px) | web tab fixes, #454 (same percentile qualification, fixture view) |
| `gallery/transitions-idle-loop-hidden.png` | 650x573 -> 650x**589** (+16px) | web tab fixes, #456 (the persistent dominant-source-dwell disclosure line) |
| `gallery/exec-scatter-dense-downsampled.png` | 650x453 -> 650x**469** (+16px) | web tab fixes, #455 UI (the unconditional pre-execution gap disclosure) |

Seven of the eight **failed the CI compare job outright, every one of them on
a SIZE change rather than pixel drift** (the job's own lines, run
`39173217295`, e.g. `FAIL: table_queries (size (1280, 656) != baseline (1280,
175))`). `execution_scatter` did **not** fail — it is regenerated anyway
because its 0.72 % is this branch's own axis rename, and leaving a knowingly
stale baseline for a cell this branch changed is precisely how the #263
staleness below came to be tabulated four times.

Attribution came from `make ui-gallery BASE=907e070` on the integration
branch **before** regeneration, not from reading the artifact afterwards:
its report was `8 changed, 0 added, 0 removed, 39 unchanged`, exactly these
eight rows.

`table_queries` is worth a note. It was **1280x656** when the compare job
first failed, i.e. +481 px over the base. A reviewer finding on this branch
then made the Queries backend-activity panel ONE collapsed `<details>`
instead of one force-expanded `<details open>` per entry — the same default
state the Waterfall surface uses for the identical concept. The regenerated
baseline here is the post-fix **1280x325**, so 331 px of that growth is gone
and the remaining +150 px is the column's two-line cell content across three
rows plus the single collapsed summary line.

## How these were produced

CI `snapshots` job via `gh workflow run ci.yml --ref
agent/release-integration-2 -f update_snapshots=true` (run **39177589710**,
snapshots job conclusion success), dispatched **after** the reviewer-fix
commit `f3ae64e` was pushed, so the artifact reflects this branch's final
code. Never a local Playwright run, per RULE 2. Only the eight explained
cells were copied out of the `snapshot-baselines` artifact; sha256 of the
committed files:

    table_events.png                               26837dd3d33aa7c9c76850bf2e1b324d73df49bab843e0373f16d1e269937d07
    table_queries.png                              0732c330659362a63f831111032d3e3a9c361a88d6fd452cb048e1ce97641a967
    concurrency_burst_table.png                    1eaf8ec4f834b6f75c638e888366493571d618be9156db8b0847a9ab450e229a
    waterfall_execution.png                        32fb06d25383d418be9a60c2988df4ed850e486cfe3453a754ec9c11b85d65f3
    execution_scatter.png                          ac88dd644a64786fd288f0d67fd52efb8342b5479484734b2b7dcb309e2712fc
    gallery/exec-scatter-dense-downsampled.png     73e40ec6ae18e5e6734a18513d5a11ff6d5c0df94b2a7912a4bc26f74f72ac34
    gallery/transitions-idle-loop-hidden.png       c7620cc51e5f52548642bfdfe7818ae81a71e0588e8b23b7fc79b9ee55ee5b53
    gallery/table-configs-events-overflow-pctl.png 4d4938f07bdbee1e139571ab13ac8e72cbdb8229d4079554a1b2aacb06a187d9

Pins unchanged: `playwright==1.60.0`, chromium build 1223, container
`mcr.microsoft.com/playwright:v1.60.0-noble`. No `VERSION` change.

## Seven cells left on the pre-#213 rendering — #263 for the fourth time

The artifact's other differing PNGs are `aas_chart_overview`,
`fidelity_sampled_shading`, and `gallery/uplot-aas-{compare-ghostdiff,
dense-events,dense,live-ticks-tick4,sampled}` — the same seven, in the same
rect, that this directory has already tabulated in
`2026-10-01-222-waterfall-slowest-default.md`,
`2026-10-13-384-concurrency-panel-labels.md` and
`2026-10-17-449-integration-baselines.md`. All seven **PASSED** the compare
job inside tolerance, and `make ui-gallery BASE=907e070` reports every one
of them **unchanged** against the base.

This is **#263** (the "4 CPUs" overlay chip rendering outside its chip;
#213's fix at `801793f` never reached these baselines because #213
regenerated only the two cells that broke its own gate). None of this PR's
six pieces touches AAS, overlay-chip or uPlot code. Not regenerated here,
same standing policy as all three prior entries: copying them would smuggle
#213's fix into this PR under #455/#454/#456/#459's issue numbers, which is
how a real regression hides inside mechanical churn.

Reading this directory first is what kept that straight: the baseline
commit's first draft attributed these seven to **#159** (font/antialias
session drift), repeating the exact mistake `2026-10-17-449` records — that
agent filed duplicate issue #450 before reading the history in full. RULE 3
is not decoration.
