# 2026-08-04 — Track U U2: OEM-loop visuals

2026-08-04 (Track U U2, OEM-loop visuals): histogram_heatmap.png,
transitions_dfg.png, table_events.png, table_queries.png REGENERATED +
gallery/histogram-dense.png REGENERATED (P8 violet log1p heatmap ramp, F4
eventColor DFG/variant tints, wire-6 percentile drill affordance + F4 %DB
bar tints, P11 bar-truth segments; see README "Baseline history").
gallery/table-configs-queries-hostile-sql.png intentionally kept
CI-authoritative: a trial regen was pixel-identical (bar changes fall
outside the cell's 650px clip). Generated LOCALLY via --update-snapshots
--only=…; follow-up --only compare matched all five at 0.0000. NOTE:
gallery/histogram-dense was one of the six 2026-08-04 CI-authoritative
cells — its provenance is local again; re-arbitrate in CI via the README
workflow, same PR. Local FULL-run failure set after this regen is exactly
the six CI-authoritative cells (README entry has the per-cell ratios and
the capture-phase attribution).

U2 regen environment:
playwright==1.58.0
chromium-version=145.0.7632.6
