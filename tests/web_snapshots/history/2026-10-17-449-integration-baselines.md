# 2026-10-17 — #449: integrating gate-integrity, semantics-backend, 430-rotation, web-tables and web-graph

TEN baselines REGENERATED, each explained by a specific branch in the
five-branch integration (PR #449). SEVEN further cells in the same CI
artifact differ but are **deliberately NOT regenerated** — see "Seven cells
left on the pre-#213 rendering — not #449's, already #263" below.

| baseline | change | branch / issue |
|---|---|---|
| `gallery/matrix-dense-top20.png` | 650x629 -> 650x**645** (+16px) | web-graph, #436/#438 (Matrix CPU* labeling/layout) |
| `gallery/table-configs-events-overflow-pctl.png` | 650x287 -> 650x**361** (+74px) | web-tables, #437 (overflow-percentile rows) |
| `gallery/transitions-idle-loop-dominant.png` | 650x589 -> 650x**605** (+16px) | web-graph, #436 (omitted-dominant-wait disclosure line) |
| `gallery/transitions-idle-loop-hidden.png` | ratio 0.1506 (own gate 0.002) | web-graph, #436 (omission-cause disclosure) |
| `table_queries.png` | 1280x141 -> 1280x**160** (+19px) | web-tables, #439/#441 (empty-state disclosure, "% attributed DB Time" label) |
| `table_overview.png` | ratio 0.0193 (own gate 0.02) | web-tables, #440 (near-tie hysteresis + Idle-children routing fix) |
| `table_events.png` | ratio 0.0096 (own gate 0.02) | web-tables, #440 (hysteresis wired into Events, previously Overview-only) |
| `waterfall_execution.png` | 1280x508 -> 1280x**445** (-63px) | web-tables, #415 (drilled readout stays above the fold) |
| `transitions_dfg.png` | ratio 0.0906 (own gate 0.02) | web-graph's rendered-geometry fix itself (nodes move to correct, identity-transform pixel coordinates) |
| `transition_matrix.png` | ratio 0.0049 (own gate 0.02) | semantics-backend, #436-backend (node/edge population fix changes which nodes the Matrix draws) |

Every one of these ten was independently traced to its branch's own diff
*before* regeneration, via `make ui-gallery` (BASE=`origin/master`=`97dca0c`
vs HEAD) on the integration branch — not inferred from the regenerated
artifact after the fact. `make ui-gallery`'s own report: 10 changed, 0
added, 0 removed, 37 unchanged, exactly matching this table's ten rows.

## How these were produced

CI `snapshots` job via `gh workflow run CI --ref agent/integration-12h -f
update_snapshots=true` (run **38958282438**, snapshots job conclusion
success) — never a local Playwright run, per RULE 2. Only the ten explained
cells were copied out of the `snapshot-baselines` artifact; sha256 verified
identical to the artifact after copying:

    gallery/matrix-dense-top20.png                24f5b11def49341c58b897699ce2f4e44a3939bcec99562bd394a5bf747c3f6d
    gallery/table-configs-events-overflow-pctl.png 36a0db19a5533bb30fec3efb40029e9a5f0878d414e904ced334727edb957cd8
    gallery/transitions-idle-loop-dominant.png     ec62075f17d50233cb1bc827a4ca1aa7311a41492e7148dc608cbfc429ec972d
    gallery/transitions-idle-loop-hidden.png       23ae1cb712eb094e0fe25b4b3bc420d7378d53a5ded5d1e137940b15e127c1fc
    table_queries.png                              5a20403db62d98af320a23f27ac6c1b6e36731d246d581b83cae517c7735f49a
    waterfall_execution.png                        68316dc91a00d89103653e42cdf4402e1ac5859aff4cd3ce43750b31b1b30699
    transitions_dfg.png                            1e73a17cd151061f3cb967f25777fc90ffe2cbc57fcdbb3044c4aba4c4806bc1
    table_overview.png                             5511e2f2e62e9c981bbe934b8d86f17f22281f3c5ac6429a520fe0cb83a61681
    table_events.png                               e1c7835c2c4cf7a8a89a5a582b78df15a0d265d6c1794f740f8b336f05c6a5cf
    transition_matrix.png                          53df720e25705795579055df80b189ed5ab9e81bd153943e805fead1f4f3215b

Pins unchanged: `playwright==1.60.0`, chromium build 1223, container
`mcr.microsoft.com/playwright:v1.60.0-noble`.

## Seven cells left on the pre-#213 rendering — not #449's, already #263

The same artifact's other 37 baselines: 30 byte-identical, 7 differing in
the identical rect this directory has already tabulated twice before
(`history/2026-10-01-222-waterfall-slowest-default.md`,
`history/2026-10-13-384-concurrency-panel-labels.md`):

| cell | ratio | max channel diff | bbox (approx) |
|---|---|---|---|
| `aas_chart_overview` | 0.00068 | 225 | (1164,88)-(1233,96) |
| `fidelity_sampled_shading` | 0.00068 | 209 | (1164,88)-(1233,96) |
| `gallery/uplot-aas-compare-ghostdiff` | 0.00122 | 229 | (535,104)-(604,112) |
| `gallery/uplot-aas-dense-events` | 0.00122 | 229 | (535,122)-(604,130) |
| `gallery/uplot-aas-dense` | 0.00122 | 229 | (535,122)-(604,130) |
| `gallery/uplot-aas-live-ticks-tick4` | 0.00105 | 229 | (535,122)-(604,130) |
| `gallery/uplot-aas-sampled` | 0.00122 | 213 | (535,122)-(604,130) |

This is **#263** (the "4 CPUs" overlay chip rendering outside its chip
instead of inside — #213's fix, merged at `801793f`, never reaching these
seven baselines because they stayed under tolerance when #213 regenerated
only the two cells that broke its own gate). None of #449's five merged
branches touch AAS, overlay-chip or uPlot code, so this is the same
pre-existing staleness #222 and #384 already found and left alone, not
anything new.

I initially mis-attributed this to #159 (font/antialiasing session drift)
and filed a duplicate issue (#450) before reading this directory's existing
history in full, as `VERSION` RULE 3 instructs. Closed #450 as a duplicate
of #263 once the mistake was found; commented on #263 with this run's
measurements as a third independent confirmation. Not regenerated here,
same standing policy as every prior entry in this directory: copying them
would smuggle #213's fix into this PR's history entry under #449's issue
numbers.
