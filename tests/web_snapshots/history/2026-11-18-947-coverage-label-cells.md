# 2026-11-18 — issue 947 — coverage label U3: nine gallery baselines added

**Added (gallery):** `aas-coverage-capture-loss.png`, `uplot-aas-coverage-capture-loss.png`,
`histogram-coverage-capture-loss.png`, `histogram-coverage-not-recorded.png`,
`fidelity-coverage-capture-loss.png`, `fidelity-coverage-restart-postgres.png`,
`fidelity-pane-in-flight-excluded.png`, `fidelity-pane-idle-unverified.png` and
`fidelity-pane-live-edge.png`. They are the new cells of the coverage-label web unit:
an incomplete window shows "Coverage unverified — whole-window metrics unavailable"
with its reason and never a number, uncovered buckets are missing, not zero, and the
live edge and in-flight marks are labelled.

**Where they came from:** the CI `snapshots` job's `snapshot-baselines` artifact on
the branch `agent/947-coverage-web` at 71ee2f63 (Actions run 44218258285, a
`workflow_dispatch` with `update_snapshots=true`). They were not rendered locally.

**No existing baseline was replaced.** The same run regenerated the 79 existing
baselines; compared with the committed files, 73 were byte-identical and 6 differed.
The 6 are the files the 2026-11-14 and 2026-11-17 entries recorded as within
tolerance (`table_queries.png`, `table_sessions.png`, `aas_chart_overview.png`,
`transition_matrix.png`, `fidelity_sampled_shading.png`,
`gallery/uplot-aas-live-ticks-tick4.png`). They were left as committed.
