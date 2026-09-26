# 2026-09-25 — #101: Waterfall default execution selection


2026-09-25 (#101, Waterfall default execution selection): ONE baseline
regenerated — waterfall_execution.png (1280x445 -> 1280x473). NO pixel
logic changed: buildWaterfallOption, the executions table config and every
renderer are untouched. The FIXTURE legitimately gained one row.
tests/mock_server.py's executions response now carries the shape a real
--mode full capture actually returns — an undrawable NEWEST execution
(PID 1004, in progress, 0 leader events, 0 workers, no plan) whose
execution_detail is {leader:{events:[]}, workers:[], plan:null}. Measured
on the gate box, rows[0] was undrawable in 40 of 40 simulated live ticks,
which is why the tab used to mount no chart at all. The +28 px is that one
extra table row; the cell also now PINS the fix: the newest row is present
but NOT highlighted, and the selected row is PID 1002, the newest execution
that actually has a waterfall.
Regenerated via CI run 36152439448 (workflow_dispatch update_snapshots=true
on agent/waterfall-query-latency, ubuntu-latest, pins already recorded at
the top of this file) — no local PNG committed. The artifact's other 43
baselines were compared and left untouched: 15 byte-identical, the rest
inside the suite's own tolerances (gallery 0.002, panes 0.02) at the usual
re-encode/antialias churn — worst gallery/timeline-single-point 0.00196,
gallery/uplot-aas-live-ticks-tick4 0.00189, gallery/aas-live-ticks-tick4
0.00188, gallery/histogram-dense 0.00169, table_overview 0.00159; every
other cell <= 0.0004.
