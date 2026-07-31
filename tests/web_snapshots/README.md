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
