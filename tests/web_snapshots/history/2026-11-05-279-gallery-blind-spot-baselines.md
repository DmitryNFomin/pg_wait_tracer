# 2026-11-05 — #279: 23 new gallery baselines (checklist-cited cells + the truncation fixtures)

TWENTY-THREE baselines ADDED, none regenerated: every file below is new. The 47
pre-existing baselines were byte-identical in the CI artifact and were not
touched (checked per file with `cmp` before copying; only absent files were
copied).

Why these exist: `docs/VISUAL_CHECKLIST.md` cited gallery cells that
`GALLERY_STATIC_CELLS` never captured, and no captured fixture carried data
that triggers the truncation disclosures, so a visible change produced
`0 changed / 0 added / 0 removed` (#273, #276). #279 captures every cited cell,
adds `concurrency/truncated-bursts`, and adds `GALLERY_EVIDENCE`: each captured
cell declares what its checklist citation claims is visible and the suite
asserts, before the screenshot, that it lies inside `.cell-body`'s 520px fold.
Two cited cells (`aas-hostile-names`, `timeline-hostile-sql`) were dropped
instead of captured: their hostile text appears only in tooltips, so no pixel
can gate their stated property.

| baseline | size |
|---|---|
| `gallery/aas-all-zero.png` | 650x377 |
| `gallery/aas-one-huge-value.png` | 650x377 |
| `gallery/concurrency-burst-final-bucket.png` | 650x595 |
| `gallery/concurrency-empty.png` | 650x178 |
| `gallery/concurrency-hostile-event-names.png` | 650x572 |
| `gallery/concurrency-one-huge-peak.png` | 650x617 |
| `gallery/concurrency-truncated-bursts.png` | 650x597 |
| `gallery/fidelity-annotation-mid-escalation.png` | 650x359 |
| `gallery/fidelity-annotation-observed-anomaly.png` | 650x359 |
| `gallery/fidelity-metrics-escalated.png` | 650x364 |
| `gallery/fidelity-unavailable-escalatable.png` | 650x183 |
| `gallery/fidelity-unavailable-no-daemon.png` | 650x199 |
| `gallery/histogram-empty.png` | 650x178 |
| `gallery/histogram-one-hot-cell.png` | 650x437 |
| `gallery/table-configs-events-null-latency.png` | 650x261 |
| `gallery/timeline-empty.png` | 650x178 |
| `gallery/timeline-micro-vs-macro.png` | 650x297 |
| `gallery/timeline-pre-window-start.png` | 650x297 |
| `gallery/transitions-dense-dfg.png` | 650x557 |
| `gallery/transitions-empty.png` | 650x196 |
| `gallery/transitions-hostile-names.png` | 650x557 |
| `gallery/transitions-threshold-simplified.png` | 650x621 |
| `gallery/transitions-variants-html.png` | 650x494 |

`transitions-variants-html` now holds the exec section only (the plan section
was trimmed to bring the cell inside the fold; it was 67px clipped), and the
concurrency cells use a smaller chart (`chartHeight` on the state) for the same
reason. No existing cell's pixels depend on either change: `chartHeight`
defaults to the old 280.

## How this was produced

CI `snapshots` job in UPDATE mode via
`gh workflow run ci.yml --ref agent/279-gallery-blind-spots -f update_snapshots=true`,
run 42084145597, artifact `snapshot-baselines`, on head `37eb75f6`, which already
contains the evidence-geometry fix (baselines were generated AFTER the fixtures
were reshaped, as fixing the fold changes the pixels). No local capture was
committed. Playwright/chromium pins are unchanged (`tests/web_snapshots/VERSION`).

`transitions-hostile-names` renders through ECharts SVG renderer (a per-state
`renderer` field; every other cell stays canvas) so the hostile node labels are
real text the geometry assertion can measure.
