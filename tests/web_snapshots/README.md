# Visual-regression baselines (Phase B4 + U1 gallery cells)

The `*.png` files in this directory are the committed baseline screenshots for
`tests/test_web_ui_snapshots.py`. The CI `snapshots` job screenshots each web
view against the fixed `mock_server.py` dataset and diffs against these.

## Gallery-cell baselines (`gallery/`, Track U U1)

`gallery/*.png` are single-cell captures of the fixture gallery
(`web/static/dev/gallery.html` — every pure builder × deterministic fixture
state), screenshotted by their stable `#gallery-<builder>-<state>` cell ids.
Because each cell is one chart at fixed CSS pixels + devicePixelRatio 1 on
fully deterministic data, they gate at a much TIGHTER tolerance
(pixel delta 8, ratio 0.002) than the 28/0.02 full-pane budget — a one-series
recolor fails here even if it would drown inside a full-pane diff. The
`gallery/aas-live-ticks-tick4` baseline is the recorded live-replay cell
stepped to tick index 4 via the deterministic step button (never the ▶ timer).
Failure vocabulary: `docs/VISUAL_CHECKLIST.md` ("<WORD> violation, cell <id>").

FORMER KNOWN-DEFECT PIN (resolved 2026-07-31, Track U U2a): the
`gallery/aas-*` cells and `aas_chart_overview.png` used to pin the
"First catch" render defect (`docs/VISUAL_CHECKLIST.md` — ECharts stacked
areas on a `type:'value'` x-axis paint only `series[0]`'s area). The U2a
camera wiring switched the AAS axis to `type:'time'`, and all nine affected
baselines were regenerated in the same PR (see Baseline history below): they
now pin the CORRECT full multi-layer stacked render with UTC time-axis
labels.

## Regenerating a SUBSET (intentional single-baseline change)

`--only=<patterns>` (or `PGWT_SNAP_ONLY=<patterns>`, comma-separated fnmatch)
restricts both compare and update to matching snapshot names, so an intentional
change regenerates ONLY its baseline without silently rewriting — and thereby
un-gating — every other one:

```
python3 tests/test_web_ui_snapshots.py --update-snapshots --only=table_queries
python3 tests/test_web_ui_snapshots.py --update-snapshots --only='gallery/*'
```

## Baseline history

- 2026-06-16: initial 13 pane baselines, CI snapshots job (see `VERSION`).
- 2026-07-31 (Track U U1): `table_queries.png` regenerated — deliberate P2
  identity change (per-event tints from the `eventColor()` color service in the
  queries-view Wait Profile stacked bars, replacing flat class hues; gallery
  evidence cells `gallery-aas-dense-events`,
  `gallery-table-configs-queries-hostile-sql`). All 12 other pane baselines
  measured unchanged (<= 0.0003) against the U1 tree. Added the 13 `gallery/`
  cell baselines. See `VERSION` for the generation environment.
- 2026-08-04 (Track U U2b): TWO pane baselines regenerated + SIX `gallery/`
  cell baselines added for the AAS renderer swap (the pane's default renderer
  is now uPlot behind the `?renderer=echarts|uplot` seam).
  `aas_chart_overview.png` and `fidelity_sampled_shading.png` both screenshot
  `#aas-chart-container`, which now renders via `lib/uplot-aas.js` + the
  vendored uPlot bundle — pre-regen diff ratios 0.082/0.089, the priced U2b
  rebaseline (tick placement/density, font raster, area-edge antialiasing;
  series colors, 0.85 area alpha, and honesty-overlay coordinates are
  parity-pinned against the ECharts builder in
  `tests/web_unit/uplot-aas.test.mjs`). Added `gallery/uplot-aas-{dense,
  dense-events, sampled, mixed-escalation, escalated-live-edge,
  live-ticks-tick4}.png` — renderer-swap twins of the aas cells (same fixture
  states by reference, real uPlot mounts; the tick cell rebuilds its instance
  per tick, the app's series-set-change path). The ECharts `gallery/aas-*`
  cells are RETAINED and measured 0.0000 against the U2b tree — the
  `?renderer=echarts` rollback path still renders through `buildAasOption`,
  so both pixel sets gate. Generated LOCALLY (playwright 1.58.0 / chromium
  145.0.7632.6 — same environment and justification as the U1/U2a entries)
  via `--update-snapshots --only=…` so the 24 untouched baselines were never
  rewritten; follow-up compare matched 28/28 excluding exactly the four
  2026-07-31 CI-chromium-authoritative cells below. Full-run caveat: those
  four cells' `capture_failure()` full-page screenshots trigger the
  documented neighbor drift on BOTH `*live-ticks-tick4` cells (captured
  last), 0.0106/0.0135 in a full local run — both verified 0.0000 in
  isolation (`PGWT_SNAP_ONLY='gallery/*live-ticks*'`). If the CI compare
  churns on the tight-tolerance `gallery/uplot-aas-*` cells, regenerate them
  in CI via the workflow below — same PR.
  Layout-phase note (measured 2026-08-04): gallery cells have always sat at
  FRACTIONAL page offsets (heights like 376.375), so an element screenshot's
  rasterization depends on the layout phase above the cell. Inserting the
  `uplot-aas` section mid-page shifts every cell below it by +2400.125 CSS
  px (local chromium 145). A same-browser A/B against the pre-U2b tree shows
  this phase change alone re-rasterizes exactly three of the four
  CI-authoritative cells — `histogram-dense` (header glyph AA),
  `concurrency-dense-bursts` (footer glyph AA), and
  `table-configs-queries-hostile-sql` (screenshot clip widens 245→246 px:
  top fraction .672→.797 under floor(top)/ceil(bottom) clip rounding) —
  while `timeline-single-point` and every other below-section cell are
  pixel-identical across trees. Consequence: expect CI churn on those three
  cells with this change; that churn is the layout shift, not chromium
  drift — regenerate them in CI, same PR. (Root-cause hardening — integer-
  quantized gallery cell heights so captures are phase-independent — would
  itself rebaseline every gallery cell, so it is deliberately not bundled
  into the U2b PR.)
- 2026-08-04 (Track U U2, OEM-loop visuals): FOUR pane baselines + ONE
  gallery cell regenerated, each justified by a U2 review fix:
  `histogram_heatmap.png` + `gallery/histogram-dense.png` — the P8 heatmap
  recolor (linear 6-stop rainbow → log1p-domain single-hue violet ramp
  `['#763e84'…'#eac3f4']`, visualMap labels now raw counts via the expm1
  formatter); pre-regen ratios 0.1300 / 0.2375 — the intended SEMANTICS
  change, unit-pinned in `tests/web_unit/histogram.test.mjs`.
  `transitions_dfg.png` — F4 identity tints (DFG node fills/borders + variant
  flow bars move from flat class hues to `eventColor()` per-event tints;
  0.0343). `table_events.png` — %DB bars adopt eventColor tints + the wire-6
  drill affordance on the P50/P95/P99 cells (0.0009 — inside the 0.02 pane
  gate, but the baseline now pins the intended pixels). `table_queries.png` —
  the P11 bar-truth model (1px inset separators, below-threshold vs
  other/unattributed split segments, pre-normalized flex widths; 0.0013).
  `gallery/table-configs-queries-hostile-sql.png` was deliberately NOT
  regenerated: the bar changes land outside that cell's 650px clip — a trial
  local regen produced pixels IDENTICAL to the committed CI baseline (0.0000
  at channel threshold 8, same 650×246 size), so the CI-authoritative file is
  kept. Generated LOCALLY (playwright 1.58.0 / chromium 145.0.7632.6 — same
  environment and justification as the U1/U2a/U2b entries) via
  `--update-snapshots --only=…`, so the 26 untouched baselines were never
  rewritten; a follow-up `--only` compare matched all five regens at 0.0000.
  NOTE: `gallery/histogram-dense` was one of the six CI-authoritative cells —
  its provenance is local again and it must be re-arbitrated in CI via the
  workflow below, same PR. Local full-run expectation after this regen: the
  failure set is EXACTLY the six 2026-08-04 CI-authoritative cells
  (timeline-single-point 0.0033, histogram-dense 0.0066,
  concurrency-dense-bursts 0.0053, table-configs-queries-hostile-sql the
  650×250-vs-246 clip flap, aas-live-ticks-tick4 0.0106,
  uplot-aas-live-ticks-tick4 0.0135) — all capture-phase/cross-chromium
  drift, not content: histogram-dense verifies 0.0000 in isolation
  (`PGWT_SNAP_ONLY='gallery/histogram-dense'`), and the concurrency/
  table-configs diff masks were inspected — glyph antialiasing only, zero
  chart/table content change from the U2 non-painting row attributes.
- 2026-07-31 (Track U U2a): NINE baselines regenerated for the First-catch
  axis fix (`builders/aas.js` x-axis → `type:'time'`, all stacked layers now
  paint, UTC time-tick labels): `aas_chart_overview.png`,
  `fidelity_sampled_shading.png`, and `gallery/aas-{dense, dense-events,
  sampled, mixed-escalation, escalated-live-edge, unicode-names,
  live-ticks-tick4}.png`. Pre-regen diff ratios 0.13–0.47 — the intended
  ALIGNMENT/SEMANTICS fix, not drift. All 17 other baselines verified
  byte-stable in the same full update run and restored untouched. Generated
  LOCALLY (playwright 1.58.0 / chromium 145.0.7632.6 — same environment and
  justification as the U1 entry in `VERSION`); if the CI compare churns on
  the seven tight-tolerance `gallery/aas-*` cells (or `aas-live-ticks-tick4`,
  which was previously CI-chromium-authoritative), regenerate them in CI via
  the workflow below — same PR.
  Suite-hygiene note discovered during this regen: a FAILING cell's
  `capture_failure()` full-page screenshot perturbs text-glyph antialiasing
  of cells captured AFTER it in the same run (element screenshots re-scroll
  the page). That was the entire "neighbor drift" on `gallery/aas-empty` /
  `gallery/timeline-dense-50pids` while the aas cells were still failing —
  both match their ORIGINAL baselines at 0.0000 again now — and it is why
  the five README-documented local-vs-CI cells can report slightly elevated
  ratios in a full local run while earlier cells are red.

## Do NOT regenerate baselines locally

Playwright screenshots are environment-sensitive (font rendering, antialiasing,
chromium build). Baselines must be produced in the **same** environment that
compares them — `ubuntu-latest` + the chromium build bundled with the
**pinned** playwright version. Note that `playwright install` pins the browser
per playwright *version* only (each playwright release bundles one specific
chromium build; the command itself follows whatever playwright is installed),
so playwright must be — and is — pinned in `.github/workflows/ci.yml`, with
the exact playwright + chromium versions recorded in `VERSION` next to this
file. Local rendering will not match and will churn diffs.

## Regenerating baselines (intentional UI change or playwright bump)

When a UI change legitimately alters appearance, regenerate the baselines in CI:

```
gh workflow run CI --ref <your-branch> -f update_snapshots=true
gh run watch                              # wait for the snapshots job
gh run download <run-id> -n snapshot-baselines -D tests/web_snapshots/
git add tests/web_snapshots/*.png
git commit -m "B4: update visual-snapshot baselines (<why>)"
```

Then the normal `snapshots` job (compare mode) on the PR validates them.

When bumping playwright: update BOTH pins in `.github/workflows/ci.yml` (the
web-ui and snapshots jobs), regenerate the baselines as above, and update
`VERSION` — all in the same PR.

## Transient artifacts

On a compare failure the suite writes `<name>-actual.png` and `<name>-diff.png`
next to the baseline (uploaded by CI as the `snapshot-diffs` artifact). These
are git-ignored — only the baselines are tracked.

### 2026-07-31 — five gallery cells re-baselined from CI chromium

`gallery/{timeline-single-point,histogram-dense,concurrency-dense-bursts,
table-configs-queries-hostile-sql,aas-live-ticks-tick4}.png` drifted
0.003-0.011 (one +4px height) between local chromium 145 (playwright 1.58)
and the CI-pinned chromium 148 (playwright 1.60) — above the tight 0.002
gallery gate, invisible at the 0.02 pane gate. Regenerated via the
update_snapshots workflow dispatch (run 30653754206) so these five are
CI-chromium-authoritative. Consequence for local runs on playwright 1.58:
exactly these cells may show sub-0.011 drift locally; either install the
pinned playwright (1.60.0) locally or exclude them with PGWT_SNAP_ONLY
when iterating. The panes and the remaining gallery cells match on both
chromium builds.

### 2026-08-04 — six tight-gate gallery cells re-arbitrated from CI chromium (U2b)

U2b's style additions shifted the four previously CI-authoritative cells by
~1px on CI, and the two live-tick cells (echarts + uplot twins) carry the
usual cross-chromium drift from local generation. All six regenerated on
CI's own chromium via the update_snapshots dispatch (run 30915354745):
`gallery/{timeline-single-point,histogram-dense,concurrency-dense-bursts,
table-configs-queries-hostile-sql,aas-live-ticks-tick4,
uplot-aas-live-ticks-tick4}.png`. These six are CI-authoritative; local
runs on the unpinned playwright may show sub-0.014 drift on exactly these
cells (use PGWT_SNAP_ONLY to exclude them when iterating, or install the
pinned playwright).
