# 2026-09-26 — #159: cross-session drift, pin the snapshots job to a container

2026-09-26 (#159, cross-session drift): the `snapshots` job now runs inside
the pinned container mcr.microsoft.com/playwright:v1.60.0-noble (ci.yml)
instead of bare ubuntu-latest + `playwright install --with-deps chromium`.
ROOT CAUSE, found by pixel-diffing PR #152's failing run (36202307855)
against the committed baselines region by region: every differing row in
all three over-tolerance cells (gallery/aas-live-ticks-tick4,
gallery/uplot-aas-live-ticks-tick4, gallery/table-configs-compare-delta)
was TEXT — the gallery caption, the tick-bar label, or a table row — never
a chart canvas, axis, color or value. web/static/style.css names only
macOS/Windows font families ('SF Mono', 'Segoe UI', ...) plus a bare
generic ('monospace'/'sans-serif'), so every glyph is rendered with
whatever font fontconfig resolves the generic alias to — and `--with-deps`
populated that from LIVE ubuntu-latest apt mirrors at job time, which can
differ day to day even though playwright/chromium are pinned. That matches
#155's proof that two runs in the SAME session (same ephemeral apt state)
are byte-identical, while #159 found up to 98% of the tolerance budget
drifting BETWEEN sessions on exactly the text-heaviest cells.

Fix verified with the required two-session proof: `--update-snapshots`
dispatched on this branch's head at 00:09:02Z (run 36203743187) and again
at 01:17:20Z (run 36207915979) — a 68-minute gap, each run pulling the
container image fresh (confirmed in the job log: a `docker pull` each
time, digest sha256:9bd26ad9...6b948 both times) — produced BYTE-IDENTICAL
PNGs for all 44 baselines (sha256 match).

39 of the 44 baselines' BYTES also differ from the PRE-fix (apt-based)
baselines, as expected for any font-package change. REAL mechanism for the
5 that came back byte-IDENTICAL to pre-fix (concurrency_burst_table,
concurrency_chart, fidelity_metrics_panel, table_sessions,
transition_matrix) — checked, not assumed, after a reviewer round found
the first cut of this note ("no on-canvas text") to be false (all five
plainly have titles/labels/table text): the two CI environments are both
Ubuntu 24.04 "noble" (confirmed in the run log), so the `sans-serif`
generic — the page body font AND ECharts' own default canvas text font —
resolves to the SAME actual font in both, regenerating byte-identical
pixels for any plain-text title/axis-label/table-cell. Only the
`monospace` generic (and the explicit `'SF Mono', 'Fira Code', monospace`
stack) resolves differently, because whichever extra font package
`--with-deps` pulled from the live apt mirror on the pre-fix run outranked
the base default for THAT generic only; the container's frozen dependency
set does not carry the same package. Confirmed by source: the five
byte-identical cells' modules (views/sessions.js, views+lib/builders/
matrix.js, lib/builders/fidelity.js, the concurrency view/builder) contain
no `.query-id`/`<code>`/`monospace` element at all, while every one of the
39 changed cells' diff pixels (isolated per-file into contiguous y-bands at
the suite's own 8/28 thresholds) lands exactly on a monospace-styled
region: a gallery cell's `<code>` header or `.facts`/`.tick-label` footer,
a `.query-id` snippet (table_queries, table_events, execution_scatter,
waterfall_execution, transitions_dfg), or a hostile-SQL cell body —
verified per file, not sampled, with ONE exception:

session_timeline.png's diff is NOT text: a single 1px-wide,
fully-opaque rgb(255,121,198) column (WAIT_CLASSES[3] "lwlock") at the
plot's left edge, where the pre-fix baseline had plain axis-splitline
grey. Root-caused and filed as #168 — a pre-existing, orthogonal renderer
fragility (the mock's session_timeline pid-filter reflects unrelated
out-of-window events onto the drilled row; every one of the resulting
window-clamped, zero-width bars is floored to a visible 1px sliver at the
same rounded pixel, so a sub-pixel host-width shift from #159's font
change flips which one — arbitrarily — wins the z-order), NOT a font
rendering difference and NOT introduced by this change. The new rendering
is deterministic (byte-identical across both #159 sessions above) but
fragile; committed anyway per #168, since the alternative is the old
baseline permanently failing the gate under the now-pinned environment for
a reason this PR cannot fix.

SEVEN baselines also changed DIMENSIONS (monospace metric drift
accumulating across a column or several lines — verified content-identical
for table_queries: the Query ID column just renders a few px wider,
pushing every later column right and adding a row of height):
  table_queries 140->143, waterfall_execution 445->449,
  gallery/aas-live-ticks-tick4 420->423,
  gallery/uplot-aas-live-ticks-tick4 419->422,
  gallery/table-configs-queries-hostile-sql 246->245.
gallery/matrix-dense-top20 (613->597) and gallery/timeline-dense-one-row
(397->381) both shrank by exactly 16px — the #106/#103-documented
flex-wrap cascade (issue #122): one gallery cell's own height change
re-wraps which row its neighbours land in, not a direct font effect on
either cell's own content.

The committed bytes here are sha256-verified copies straight from the
36203743187 artifact; no local PNG committed.

#159 regen environment:
playwright==1.60.0 (pip) inside mcr.microsoft.com/playwright:v1.60.0-noble

Merge note (#159 x #101): waterfall_execution.png was first kept at #101's
(1280x473, ubuntu-latest / apt-based) bytes when merging master into the
#159 branch, since #101's mock_server.py fixture change landed after the
#159 regen above and the container-regenerated file would have shown the
old, now-wrong table row count. That made it the one baseline of the 44
NOT proven byte-identical under the pinned container.

Follow-up (same merge, completing the #159 set): regenerated
waterfall_execution.png ONLY (1280x473 -> 1280x479) via workflow_dispatch
update_snapshots=true on agent/snapshot-session-drift (run 36234353054),
sha256 verified against the downloaded artifact (83f6b7fd...c67c60e)
before committing. Pixel-diffed against the pre-regen (#101, apt-based)
file first: the chart region (bars, axis ticks, colors) is byte-identical
to the old file at a uniform +6px vertical offset (diff sum 0 at that
shift); the table rows show the usual monospace line-height drift
accumulating from 0px at the header to +6px by the chart, matching every
other #159 cell's documented mechanism. No chart geometry, series color,
or plotted value changed — the size bump is font metrics only. This
baseline is now, like the other 43, sha256-verified from a container-run
artifact; no local PNG committed.
