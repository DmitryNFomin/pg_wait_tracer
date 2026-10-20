# 2026-10-20 — #481: fifteen viewer-visible defects, thirteen baselines regenerated

THIRTEEN baselines regenerated. THREE further cells in the same CI artifact
differ and are **deliberately NOT regenerated here** — see "Left alone" below.

PR #481 fixes 15 viewer-visible defects; several deliberately change element
geometry. The `snapshots` job compared against stale baselines and failed 13
of 47. A ui-reviewer verified every failure against the diff PNGs at pixel
level before this regeneration (see the PR body): no unintended cell moved.

| baseline | change | issue |
|---|---|---|
| `transitions_dfg.png` | 1240x550 -> 1240x**302** | #475-1, viewport-derived DFG height clamp |
| `concurrency_chart.png` | 1240x350 -> 1240x**270** | #473-5, chart shrunk so the Peaks row clears the fold |
| `execution_scatter.png` | 1240x543 -> 1240x**579** | #475-3 / #476, notes moved below the chart |
| `transition_matrix.png` | 1240x692 -> 1240x**740** | #475-3 / #476, same |
| `waterfall_execution.png` | 1280x507 -> 1280x**507** (content only, ratio 0.0134 of 0.02 budget) | #473-4 grid.right 24->64 + #478-axis precision |
| `gallery/waterfall-dense-plan-3lanes.png` | 650x359 -> 650x**359** (content, ratio 0.0583 of 0.002 budget) | #478-axis + #478-planmarker + #473-4 |
| `gallery/uplot-aas-dense.png` | 650x377, ratio 0.0060 of 0.002 | #473-1, N-CPUs chip moved to the left plot edge |
| `gallery/uplot-aas-dense-events.png` | 650x377, ratio 0.0060 of 0.002 | same |
| `gallery/uplot-aas-sampled.png` | 650x377, ratio 0.0060 of 0.002 | same |
| `gallery/uplot-aas-compare-ghostdiff.png` | 650x377, ratio 0.0060 of 0.002 | same |
| `gallery/uplot-aas-mixed-escalation.png` | 650x377, ratio 0.0053 of 0.002 | same |
| `gallery/uplot-aas-escalated-live-edge.png` | 650x393, ratio 0.0052 of 0.002 | same |
| `gallery/uplot-aas-live-ticks-tick4.png` | 650x422, ratio 0.0052 of 0.002 | same |

## How these were produced

CI `snapshots` job via `gh workflow run ci.yml --ref agent/viewer-defects-web
-f update_snapshots=true`, run **39455554628** (snapshots job `118741367362`,
conclusion success), against commit `06eae12` (branch tip plus the
already-landed `Merge branch 'master' into agent/viewer-defects-web`). Never
a local Playwright run, per RULE 2.

Every one of the artifact's 47 PNGs was compared byte-for-byte against the
committed baseline before anything was copied: 31 identical, 16 differing, 0
new, 0 missing. Of the 16 differing, exactly these 13 are taken.

## waterfall_execution.png: reproducibility note, not a regeneration decision

The one CI `pull_request`-triggered compare run that originally failed this
PR (run `39448850542`) recorded `FAIL: waterfall_execution (size (1280, 506)
!= baseline (1280, 507))` — an actual height of 506px. A second, independent
workflow_dispatch regeneration (run **39456603314**, dispatched purely to
cross-check this one cell) produced a byte-identical artifact to the first
(`33e3a976e7f5` both times) at height **507px** — matching the OLD
baseline's size exactly, content ratio 0.0134 under the 0.02 pane budget.

Two isolated solo-dispatch runs agree deterministically; the one run
embedded in the full gating job matrix (`pull_request` event, every other CI
job scheduled concurrently) disagreed by 1px. That pattern — agreement
under isolation, disagreement under full-matrix load — is consistent with a
settle-timing flake in the chart's resize-observer against the capture
helper's fixed 300ms wait (`tests/test_web_ui_snapshots.py`'s `snapshot()`),
not a baseline problem. Recorded here rather than silently resolved: if the
`snapshots` check flaps again on a future `pull_request`-triggered run for
this specific cell, this is the first thing to check, and `make check`/the
test harness should not be "fixed" by widening its tolerance (CLAUDE.md's
"never harden a test against runner noise" rule) without first confirming
with a few more full-load runs.

## Left alone: three cells NOT in the 13, found differing but not regenerated

`aas_chart_overview.png`, `fidelity_sampled_shading.png` and
`table_queries.png` are also byte-different from their committed baselines
in the same artifact (ratios 0.0036, 0.0036 and 0.00035 of the 0.02 pane
budget respectively — all comfortably inside tolerance, which is why none of
the three ever showed up in the `snapshots` job's 13-failure list). Both
regeneration runs (39455554628 and 39456603314) agree on their content
byte-for-byte, so this is not render noise.

Circumstantial evidence ties two of the three to this PR's own fixes rather
than to an unrelated regression: `aas_chart_overview` and
`fidelity_sampled_shading` both render AAS-family charts, and #473-1 (the
N-CPUs chip moved to the left plot edge) plus #472 (AAS KPI window-captured
disclosure) are exactly the kind of change that would nudge them — the same
root cause as 7 of the 13 cells taken above, just below the looser
28/0.02 pane threshold instead of the gallery's tight 8/0.002 one.
`table_queries` plausibly traces to #475-2 (sub-millisecond spans read
`300µs` instead of `0.0 s`), which changes displayed text width in that
table. This project has hit the adjacent pattern four times before under
issue #263 (see `2026-10-20-466-execution-boundary-and-web-fit.md` and its
backward references) — there, the standing policy was to leave matching
cells unregenerated because *that* PR touched no AAS/overlay-chip code.
Here, #481 explicitly does touch that code (#473-1), so the #263 rationale
for leaving `aas_chart_overview`/`fidelity_sampled_shading` alone may no
longer apply — but attribution was not independently verified at the pixel
level the way the 13 above were, so they are reported rather than folded
in. Not regenerating here is the conservative default per the task's own
guard rail: a baseline change for a cell outside the documented list gets
reported, not committed, so a real regression never rides along inside
mechanical churn.

Pins unchanged: `playwright==1.60.0`, container
`mcr.microsoft.com/playwright:v1.60.0-noble`. No `VERSION` change.
