# 2026-09-18 — #106: timeline density aggregation

2026-09-18 (#106 timeline density aggregation): ONE gallery baseline ADDED
and FOUR REGENERATED, all by the CI snapshots job (workflow_dispatch
update_snapshots=true on agent/timeline-density, run 35369391705) — the
artifact's other 38 baselines were compared at the suite's own tolerances
(gallery 8/0.002, panes 28/0.02) and left untouched.
  ADDED  gallery/timeline-dense-one-row — the new #106 fixture cell (3036
         spans on one PID row, 57% Timeout / 43% CPU): per-pixel-column
         class-share stacks plus the density banner.
  REGEN  gallery/timeline-dense-50pids (650x578 -> 650x602) — the gallery's
         timeline renderer now paints the model's banner, so this cell
         finally SHOWS the truncation it is tagged FEEDBACK for ("Showing
         400 of 1,200 events"): +24 px of banner above the same chart.
  REGEN  gallery/concurrency-dense-bursts (ratio 0.0033, rows 596-600),
         gallery/fidelity-compare-predates (650x133 -> 650x132),
         gallery/table-configs-queries-hostile-sql (ratio 0.0060, rows
         10-23) — none of these builders changed. #grid is a wrapping
         flexbox, so inserting one cell re-wraps its row: row-mates restretch
         by a pixel and header/footer glyphs re-rasterize at a new subpixel
         offset (the same layout-phase effect the U2b entry above measured).
         That is issue #122's known cost of adding any gallery cell.

#106 regen environment: the CI snapshots job (ubuntu-latest) at the pins
already recorded at the top of this file — no local PNG was committed.
