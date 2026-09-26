# 2026-09-19 — #103: overflow-bucket percentiles

2026-09-19 (#103 overflow-bucket percentiles): ONE gallery baseline ADDED
and FOUR REGENERATED, all by the CI snapshots job (workflow_dispatch
update_snapshots=true on agent/events-overflow-percentiles, run
35455242054) — the artifact's other 39 baselines were compared at the
suite's own tolerances (gallery 8/0.002, panes 28/0.02) and left untouched
(worst non-churn ratios: gallery/timeline-single-point 0.00196,
gallery/uplot-aas-live-ticks-tick4 0.00189, gallery/aas-live-ticks-tick4
0.00188 — all inside budget; table_events 0.00000).
  ADDED  gallery/table-configs-events-overflow-pctl — the new #103 fixture
         cell: percentiles that landed in the latency histogram's
         open-ended top bucket render ">=16.4ms" with a disclosure tooltip,
         next to Avg/Max in seconds; the last row (inside the histogram) and
         CPU* (overflow, no histogram pivot) pin the two control cases.
  REGEN  gallery/fidelity-compare-mismatch (0.0558),
         gallery/fidelity-compare-predates (0.0285),
         gallery/table-configs-compare-delta (650x227 -> 650x228),
         gallery/uplot-aas-compare-ghostdiff (650x377 -> 650x378) — all four
         numbers are the CI job's own (master -> HEAD; the local
         make ui-gallery render disagrees on the ghostdiff direction, which
         is exactly why CI is authoritative). None of those builders
         changed. #grid is a wrapping flexbox: adding one
         fixture cell re-wraps a row, row-mates restretch by a pixel and
         their glyphs re-rasterize at a new subpixel offset (issue #122, the
         same cost the #106 entry above measured). Attribution verified
         locally: rendering THIS branch's code with the new fixture state
         temporarily removed reproduced all four cells pixel-identically to
         master (make ui-gallery equivalent: 0 changed). Appending the state
         at the END of dev/fixtures/table-configs.mjs additionally spared
         gallery/table-configs-queries-hostile-sql (0.00000), which a
         mid-file insertion had churned at 4.06%.

Review round 2 (same PR, CI run 35460525765, workflow_dispatch
update_snapshots=true): the owner-taste .pctl-overflow underline on
non-drillable bounds re-rendered gallery/table-configs-events-overflow-pctl
at ratio 0.00013 — BELOW the cell's own 0.002 gate, so the compare would
have passed either way; the file is refreshed anyway so the committed
baseline pins what HEAD actually draws. The other 43 baselines came back
at 0.00000 (including all four REGEN cells above and table_events), so
nothing else was touched.

#103 regen environment: the CI snapshots job (ubuntu-latest) at the pins
already recorded at the top of this file — no local PNG was committed.
