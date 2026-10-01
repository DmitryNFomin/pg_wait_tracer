# 2026-10-01 — #222: Waterfall defaults to the slowest executions

TWO baselines REGENERATED, both top-level panes, both explained by this
branch's own diff. **Neither is #122 flexbox neighbour drift** — see "No
neighbour drift this time, and why" below, because the absence is the part
a reader would otherwise have to re-derive.

| baseline | change | intended, or collateral? |
|---|---|---|
| `waterfall_execution.png` | 1280x479 -> 1280x**509** (+30px) | **INTENDED** — the whole point of #222 |
| `execution_scatter.png` | ratio 0.00183 (1230/673320 px), bbox (100,454)-(1149,468) | **COLLATERAL, same branch** — x-axis tick labels only, from this branch's own mock-fixture re-anchoring. Not neighbour drift: this cell shares a fixture with the Waterfall table, it does not share a layout row with it |

The `snapshots` gate on PR #262 (run 36842914974) failed on exactly ONE
cell — `waterfall_execution`, on the size mismatch. `execution_scatter`
PASSED at 0.00183 against the pane tier's own 0.02 budget and is refreshed
anyway, so the committed baseline pins what HEAD actually draws (the same
reasoning as the #103 entry's review round 2, which refreshed a cell at
0.00013).

## `waterfall_execution` — four changes, all intended, all from this branch

Read off the failing run's own `-actual` render against the committed
baseline (`snapshot-diffs` artifact), not inferred from the ratio:

1. **Status line.** `Executions · latest first` becomes `Executions ·
   longest running first · [Show latest first] · 1 running, 3 completed ·
   * = inferred end`. Three separate commits land here: the
   duration-descending UI default (`EXECUTIONS_SORT_DEFAULT`), the
   open/completed split from `open_count`/`completed_count`, and review
   round 3's inline `* = inferred end` legend. The new
   `.view-title .link-button` and `.execution-inferred` rules in
   `style.css` are the only CSS this branch adds, and both are scoped to
   this header/table.
2. **Row order.** Latest-first (1004, 1002, 1000) becomes slowest-first
   (1004 at ">= 1.0s (running)", 1002 at 80.0ms, 1010 at "80.0ms *", 1000
   at 30.0ms). This is the issue.
3. **One extra table row, and that is the whole +30px.** `tests/
   mock_server.py` gained PID 1010 — `end_inferred: True`, 80.0ms — so the
   fixture exercises the inferred-end `*` and its dotted underline. Top 15
   rows and bottom 86 rows of the image are byte-identical to the old
   baseline; the growth is one table row, not a wrapped header.
4. **In-progress duration text.** PID 1004 reads ">= 1.0s (running)"
   instead of "In progress" — the elapsed-so-far ranking that lets an open
   row sort by how long it has actually been running.

Timestamps also move (02:46:40.x -> 09:46:39.x) because the executions
fixture is now anchored at `_EXEC_BASE_NS = _TO_NS - 950_000_000`.
`_TO_NS` is the fixed constant `1_774_000_000_000_000_000`, **not** a
wall-clock read, so the rendered times stay deterministic across runs —
checked in `tests/mock_server.py` before regenerating, because a
now-relative anchor would have made this baseline churn on every single CI
run and no amount of regeneration would have fixed that.

## `execution_scatter` — the axis labels, and nothing else

The differing pixels are confined to one 15px band, y=454..468: the x-axis
tick-label row, which now reads `09:46:39` five times instead of
`02:46:40` once. Cause is the same `_EXEC_BASE_NS` re-anchoring (commit
"exec_scatter's pid 1002 point must match the executions fixture's
start_ns"). **The plot area is byte-identical**: every pixel above y=448
is unchanged, verified pixel-for-pixel against the pre-regen file — the
point did not move within the plot, the axis rescaled with it, so no
plotted value, dot position, downsampling decision or colour differs.

## No neighbour drift this time, and why

`VERSION` points at the #122 flexbox cascade — one cell resizing re-wraps
its row and re-rasterizes its row-mates — and a +30px cell is exactly the
shape that used to trigger it. It did not here, for two independent
reasons, and both were checked rather than assumed:

- `waterfall_execution` is a **top-level pane**, not a gallery cell. The
  #122 mechanism lives in `web/static/dev/gallery.{html,js}`, which the
  app pages never load (see `history/2026-09-26-122-gallery-cell-
  isolation.md`: "No other baseline (the 16 non-gallery panes) changed").
- Even for gallery cells the mechanism is gone: #122's follow-up gave each
  cell its own `?isolate=<cellId>` page, so a cell's absolute paint
  position can no longer depend on the rest of the manifest. That was
  verified there with two probe insertions leaving all 28 cells
  byte-identical.

Consistent with both: the failing CI run flagged one cell, and a full
47-cell compare of this regeneration's artifact against the committed
tree shows no gallery cell changed for any layout reason.

## Seven cells left untouched — and a pre-existing staleness worth filing

The artifact's other 45 baselines: 38 byte-identical, 7 differing. All
seven differ in the **identical 68x8 px rectangle** — the "4 CPUs" overlay
chip at the top right of the AAS plot — and in all seven the committed
byte renders the label *outside* its chip while the fresh render puts it
*inside*:

| cell | ratio | px | bbox | own gate |
|---|---|---|---|---|
| `aas_chart_overview` | 0.00078 | 300 | (1164,88)-(1232,95) | 0.02 |
| `fidelity_sampled_shading` | 0.00078 | 300 | (1164,88)-(1232,95) | 0.02 |
| `gallery/uplot-aas-compare-ghostdiff` | 0.00131 | 321 | (535,104)-(603,111) | 0.002 |
| `gallery/uplot-aas-dense-events` | 0.00131 | 321 | (535,122)-(603,129) | 0.002 |
| `gallery/uplot-aas-dense` | 0.00131 | 321 | (535,122)-(603,129) | 0.002 |
| `gallery/uplot-aas-live-ticks-tick4` | 0.00109 | 300 | (535,122)-(603,129) | 0.002 |
| `gallery/uplot-aas-sampled` | 0.00131 | 321 | (535,122)-(603,129) | 0.002 |

That is **#213's chip-label fix** (`history/2026-09-28-213-chip-label-
alignment.md`, merged at 801793f, already in this branch's base), not
anything #222 does — this branch touches no overlay, AAS or uPlot code at
all (`git diff origin/master -- web/` is three files: the waterfall
builder, the waterfall view, and the two scoped CSS rules above). #213
regenerated only the two cells that broke its gate, because "45 of 47
snapshots matched unchanged"; these seven were under tolerance then and
are still under tolerance now, so they have quietly carried the *pre*-#213
rendering ever since.

They are deliberately NOT regenerated here: copying them would smuggle
another branch's visual change into this PR under a #222 history entry,
and the standing policy in every entry in this directory is to copy only
the cells the branch's own diff explains. Flagged for its own issue
instead — it is a real, if cosmetic, baseline staleness on master, and
nobody will notice it from a green gate.

## How these were produced

CI `snapshots` job via `gh workflow run CI --ref agent/waterfall-slowest -f
update_snapshots=true` (run **36852014703**, snapshots job conclusion
success) — never a local Playwright run, per RULE 2. Only the two changed
cells were copied out of the `snapshot-baselines` artifact; sha256 verified
identical to the artifact after copying:

    waterfall_execution.png  937a9bb9f5acc74ce15f247f64e356191be5fa7a67d199cedefe5cec21d44ee3
    execution_scatter.png    f4b16791a398ecaba89bbe41ba618884c83306e8ba2542a29992a26594e3b5dc

Pins unchanged: `playwright==1.60.0`, chromium build 1223, container
`mcr.microsoft.com/playwright:v1.60.0-noble`.
