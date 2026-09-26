# 2026-08-04 — Track U U3: execution-analysis baselines + adversarial-review fixes

2026-08-04 (Track U U3 + adversarial-review fixes): SIX execution-analysis
baselines ADDED (waterfall_execution, execution_scatter, transition_matrix,
gallery/{waterfall-dense-plan-3lanes,exec-scatter-dense-downsampled,
matrix-dense-top20}) and TWO baselines REGENERATED (table_queries,
gallery/table-configs-queries-hostile-sql). The existing hostile-SQL cell is
intentionally no longer CI-authoritative: M5 moves the Waterfall action into
its 650px clip, so retaining the U2 file would hide the new visible pivot.
README "Baseline history" records every pre-regen ratio/size and the
semantics behind the waterfall/scatter/matrix changes. Generated LOCALLY via
one exact eight-name --update-snapshots --only run; unrelated baselines were
never opened for update, and the same --only compare then matched 8/8.
Re-arbitrate in CI via README's workflow if Chromium rasterization differs.

U3 regen environment:
playwright==1.58.0
chromium-version=145.0.7632.6
