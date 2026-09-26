# 2026-09-25 — #155: render-settle fix

2026-09-25 (#155, render-settle fix): TWO baselines regenerated —
gallery/aas-live-ticks-tick4.png and gallery/uplot-aas-live-ticks-tick4.png.
NOT because pixels are wrong: the old committed baselines had themselves
been captured under the pre-fix racy capture and sat at 0.00188/0.00189
against the cells' own 0.002 gate — 95% of budget, effectively zero
margin. Regenerated to remove that old-race drift now that the capture
waits for echarts 'finished'/uPlot 'draw' instead of racing the paint.
Determinism proof: CI runs 36136702565 and 36137060705 (workflow_dispatch
update_snapshots=true, ubuntu-latest, both on this PR's head after the
render-settle fix) produced BYTE-IDENTICAL PNGs for all 44 baselines
(sha256 match, not just under-tolerance) — including these two and the
other 22 gallery/ cells. The committed bytes here are sha256-verified
copies straight from the 36136702565 artifact
(aas-live-ticks-tick4.png c01bd39c1466..b0e2e,
uplot-aas-live-ticks-tick4.png 11c0ce473ac8..c06); no local PNG committed.
