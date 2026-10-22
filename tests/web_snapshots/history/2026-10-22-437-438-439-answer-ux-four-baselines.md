# 2026-10-22 — #437/#438/#439: four baselines regenerated for the answer-ux fixes (PR #491)

PR #491 ("Five answer-quality fixes") deliberately changes element geometry
and text in four places. The `snapshots` job (run `39704592533`) compared
against stale baselines and failed exactly these 4 of 47 cells. A
ui-reviewer verified every failure against the diff images at pixel level
before this regeneration (see the PR body): no unintended cell moved.

| baseline | change | cause |
|---|---|---|
| `table_sessions.png` | 1280x221 -> 1280x**247** | #439 — new cross-tab scope caption above the table |
| `gallery/table-configs-events-overflow-pctl.png` | 650x393 -> 650x**409** | #437 — "Unresolved" is longer than "≥16.4ms", rewrapping a row |
| `transition_matrix.png` | 1240x740 -> 1240x**754** | #438 — CPU* explanation moved above the canvas |
| `gallery/matrix-dense-top20.png` | 650x613 (unchanged), content ratio 0.0134 of the 0.002 gallery budget | #438 — CPU* note restored to the fixture's footer line, now rendered first |

## How these were produced

CI `snapshots` job via `gh workflow run ci.yml --ref fix/web-answer-ux-1
-f update_snapshots=true`, run **39732278654** (job `119669819467`,
conclusion success), against commit `dbc4f6a53ba912b034b5f07503da11cd92a6f403`
— PR #491's head at dispatch time (the artifact's own `workflow_run.head_sha`
confirms it). Artifact `snapshot-baselines`, id `12404834189`, digest
`sha256:ed82315870346ece53ca9c4f110c0560b4224200aa4ad9dd395879d80e111208`.
Never a local Playwright run, per RULE 2.

Every one of the artifact's 47 PNGs was compared byte-for-byte against the
committed baseline before anything was copied: 40 identical, 7 differing, 0
new, 0 missing. Of the 7 differing, exactly these 4 are taken. That diff was
run twice by two independent parties (the regenerating agent and the
orchestrator) with identical counts.

## Left alone: three cells NOT regenerated here (tracked in #483)

`aas_chart_overview.png`, `fidelity_sampled_shading.png` and
`table_queries.png` are also byte-different from their committed baselines
in the same artifact (ratios 0.0036, 0.0036 and 0.0004 of the 0.02 pane
budget respectively — all comfortably inside tolerance, which is why none
of the three ever showed up in the `snapshots` job's failure list). These
are the same pre-existing sub-threshold drift already recorded in
`2026-10-20-481-viewer-defects-thirteen-cells.md`'s "Left alone" section
(ratios 0.0036 / 0.0036 / 0.00035 there — effectively identical), tracked
under #483 and deliberately left un-regenerated: a cell outside the
documented failure list gets reported, not folded in.

Pins unchanged: `playwright==1.60.0`, container
`mcr.microsoft.com/playwright:v1.60.0-noble`. No `VERSION` change.
