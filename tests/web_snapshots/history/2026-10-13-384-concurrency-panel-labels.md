# 2026-10-13 — #384: Concurrency markers stop printing a second number on the line

THREE baselines REGENERATED, all three explained by this branch's own diff.
A fourth cell (`waterfall_execution`) failed the same CI gate and is
**deliberately NOT regenerated** — see "The cell that failed and was left
alone" below, which is the part of this entry worth reading.

| baseline | change | intended, or collateral? |
|---|---|---|
| `concurrency_chart.png` | 1240x350, ratio 0.0848 (36823/434000 px), own gate 0.02 | **INTENDED** — the issue |
| `concurrency_burst_table.png` | 1240x499 -> 1240x**525** (+26px) | **INTENDED** — one new caption line |
| `gallery/concurrency-dense-bursts.png` | 650x601, ratio 0.1288 (50299/390650 px), own gate 0.002 | **INTENDED** — the same builder on the dense fixture |

## What changed in the pixels, and why

#384's defect was not a computation disagreement — both panels were
numerically right. A burst `markPoint` was planted at the bucket's peak
coordinate and labelled with a *different quantity* (pids entering one wait
within 10 ms) on an opaque triangle up to 30 px tall. Over a 13-session
peak the only legible number on the canvas read "6".

So the chart cell changes in exactly the ways the fix changes:

1. markers carry **no numeric label** (the burst count moved into the
   tooltip, with its unit);
2. `symbolSize` 30 -> **18**, rotated and offset to float *above* the point
   rather than cover it;
3. `coord[1]` is strictly the plotted series value — it was
   `peakData[at] || b.sessions`, which planted a burst **count** on a line
   **coordinate** whenever a bucket's own peak was 0;
4. a two-line chart legend naming which quantity each panel reports.

The table cell's +26 px is **one caption line** over the peaks table, in the
neutral style, naming that table's quantity. Not a wrapped header, not a
row-count change: the fixture is untouched.

`gallery/concurrency-dense-bursts` is the same builder rendered on the dense
fixture, so it moves for reasons 1-4 and nothing else.

A pure-builder test landed with the fix and is the actual deliverable:
`tests/web_unit/concurrency_panels.test.mjs` compares the two **rendered**
artifacts — the ECharts option and the table HTML — against the response
that produced both. Model-level tests could not have caught this, because
the model was right and only the rendering lied.

## The cell that failed and was left alone

`waterfall_execution` failed the same gate on a size mismatch — 1280x**505**
against the 1280x509 baseline that #222 established (see
`2026-10-01-222-waterfall-slowest-default.md`). It is **not** regenerated
here, and the reason is recorded rather than guessed at:

- **It is reproducible, not flaky.** Three independent compare-mode runs on
  this code report all four failures with identical pixel counts: the
  `pull_request` run on the merge ref plus two `workflow_dispatch` runs on
  `256ec82` itself.
- **The same code renders 509 in update mode.** This regeneration's own
  artifact contains a `waterfall_execution.png` byte-identical to the
  committed baseline. Update and compare are the same job in the same
  container, so this is a mode difference, not an environment one.
- **The only mode-dependent code after a capture is the failure path.**
  `snapshot()` runs identically in both modes up to
  `png = el.screenshot(...)`; on a mismatch it then calls
  `capture_failure()`, which takes a `full_page=True` screenshot inline.
  The suite captures `concurrency_chart` and `concurrency_burst_table`
  immediately before `waterfall_execution` — exactly the two cells that
  were failing.
- **The 4 px is four table rows one pixel shorter each** (150 px vs 154 px
  across the band), a row-height change — which is the same symptom
  `4cc5c33` on this branch attributed to a font fallback.

The mechanism is therefore **open**, with two live candidates: the full-page
capture perturbs the page, or there is no persistent perturbation at all and
the extra seconds simply let a fallback font land before the waterfall tab
paints — in which case #388's font coupling is not fixed and any future red
cell re-triggers this. A scroll-mutation story is ruled out: Playwright 1.60
captures beyond the viewport rather than scrolling, and scroll position
cannot change a row's height.

Filed as **#391**, with the discriminating experiment (force the full-page
capture at those two points with the concurrency baselines current; add a
delay-only control) and the fix direction, which is isolating the artifact
capture from the run it reports on.

Regenerating this baseline would have been the wrong move twice over: it
would bless whichever of the two renders the harness happened to produce,
and 509 is what the page draws when nothing has failed. Leaving it makes the
next CI run a test of the coupling claim — 47/47 supports it and leaves the
mechanism unconfirmed; a 505 with concurrency green refutes it and means
something else is changing row height.

## No #122 neighbour drift, and the check behind that claim

A full 47-cell compare of this regeneration's artifact against the tree as it
stood **before** this commit shows **exactly ten** cells differing: the three
above, and the seven of **#263**. Nothing else moved, so the +26 px cell
re-wrapped nothing. (Against the tree *after* this commit the same compare
shows seven, the three adopted cells now being identical to the artifact.)

Consistent with the mechanism being gone rather than merely quiet: #122's
follow-up gave each gallery cell its own `?isolate=<cellId>` page, so a
cell's paint position can no longer depend on the rest of the manifest
(`2026-09-26-122-gallery-cell-isolation.md`), and the two pane cells here
are not gallery cells at all.

## Seven cells untouched — still #263, measured not assumed

The same seven cells the #222 entry flagged, differing in the **same 68x8 px
rectangle** — the "4 CPUs" overlay chip — at the **same bounding boxes that
entry tabulated**, so this is #263's pre-#213 staleness carried forward, not
new drift.

The pixel *counts* below are not #222's numbers and must not be read as
matching them: #222 counted any non-zero difference (300/321), these count at
a per-channel threshold of >2 (298/318). Same pixel set, two thresholds — the
two or three pixels between them are differences of 1 or 2 levels. Compared
cell by cell:

| cell | differing px (>2/channel) | bbox | own gate |
|---|---|---|---|
| `aas_chart_overview` | 298 | (1164,88)-(1232,95) | 0.02 |
| `fidelity_sampled_shading` | 298 | (1164,88)-(1232,95) | 0.02 |
| `gallery/uplot-aas-compare-ghostdiff` | 318 | (535,104)-(603,111) | 0.002 |
| `gallery/uplot-aas-dense-events` | 318 | (535,122)-(603,129) | 0.002 |
| `gallery/uplot-aas-dense` | 318 | (535,122)-(603,129) | 0.002 |
| `gallery/uplot-aas-live-ticks-tick4` | 298 | (535,122)-(603,129) | 0.002 |
| `gallery/uplot-aas-sampled` | 318 | (535,122)-(603,129) | 0.002 |

Max per-channel difference 209-229 — a text pixel, not antialiasing — and
every one of them under its own threshold. Not regenerated here: copying
them would smuggle #213's visual change into this PR under a #384 history
entry, which is the standing policy in every entry in this directory. They
belong to #263.

## How these were produced

CI `snapshots` job in update mode — run **38434130943** on `4cc5c33`
(`256ec82` and later commits on top of it change only docs and this
directory), snapshots job conclusion success, artifact `snapshot-baselines`
(id 11831828790). Never a local Playwright run, per RULE 2. Only the three
changed cells were copied out of the artifact; sha256 verified identical to
the artifact after copying:

    concurrency_chart.png                bc9d471f2d9b17fbed7d558e38ca5f40eb30a1424894307be5273de72c4740de
    concurrency_burst_table.png          e43f6a21dfc76c68ab0eb6efe81f93f8df5a35c91fe9ce785d90d6f588c22b05
    gallery/concurrency-dense-bursts.png b59231d9d5e6574ddd6044560ad329ae62e899d7a728782086e202481d0d0430

Pins unchanged: `playwright==1.60.0`, chromium build 1223, container
`mcr.microsoft.com/playwright:v1.60.0-noble`.
