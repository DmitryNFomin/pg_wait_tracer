# 2026-08-04 — Track U U2b: AAS pane renderer swap to uPlot

2026-08-04 (Track U U2b): aas_chart_overview.png + fidelity_sampled_shading
.png REGENERATED (the AAS pane's default renderer swapped to uPlot — the
priced U2b rebaseline; see README "Baseline history") and SIX
gallery/uplot-aas-* cell baselines ADDED (renderer-swap twins of the aas
cells; the echarts aas cells are retained and still match at 0.0000).
Generated LOCALLY (Linux 6.8, playwright 1.58.0, chromium 145.0.7632.6) via
--update-snapshots --only=aas_chart_overview,fidelity_sampled_shading,
gallery/uplot-aas-* — the 24 untouched baselines were never rewritten. A
follow-up compare matched all 28 baselines outside the four 2026-07-31
CI-chromium-authoritative cells at diff ratio 0.0000 (session_timeline
0.0003); in an unfiltered local run those four cells' failure captures
additionally perturb the two *live-ticks-tick4 cells (README neighbor-drift
note) — both match 0.0000 in isolation.

EXPECTED CI CHURN (measured 2026-08-04, README "layout-phase" note): the
uplot-aas gallery section is inserted mid-page, shifting every cell below
it by a FRACTIONAL page offset (+2400.125 CSS px on local chromium 145 —
gallery cell heights have always been fractional, e.g. 376.375). A same-
browser A/B against the pre-U2b tree (git-archive HEAD, identical context)
shows that phase shift alone re-rasterizes THREE of the four CI-
authoritative cells: gallery/histogram-dense (header glyph antialiasing),
gallery/concurrency-dense-bursts (footer glyph antialiasing), and
gallery/table-configs-queries-hostile-sql (element-screenshot clip widens
245->246 px: top fraction .672->.797 under floor(top)/ceil(bottom)
rounding); gallery/timeline-single-point is pixel-identical across trees.
So a CI compare failure on those three cells after this change is the
layout shift, NOT chromium drift — regenerate them in CI via the README
workflow, same PR, alongside any churn on the tight-tolerance
gallery/uplot-aas-* cells.

U2b regen environment:
playwright==1.58.0
chromium-version=145.0.7632.6
