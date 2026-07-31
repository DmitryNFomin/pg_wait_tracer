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

KNOWN-DEFECT PIN: the `gallery/aas-*` chart cells currently pin the
"First catch" render defect documented in `docs/VISUAL_CHECKLIST.md` (ECharts
stacked areas on a `type:'value'` x-axis paint only `series[0]`'s area at
epoch-magnitude x values — same defect `aas_chart_overview.png` pins). When
the axis fix lands, regenerate every `gallery/aas-*` baseline (and
`aas_chart_overview.png`) in the same PR.

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
