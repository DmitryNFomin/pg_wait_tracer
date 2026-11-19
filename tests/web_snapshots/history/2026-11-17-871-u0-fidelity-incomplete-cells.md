# 2026-11-17 — issue 871 — U0 fidelity-incomplete: seven gallery baselines added

**Added (gallery):** `aas-missing-bucket-zero.png`, `aas-not-captured-range.png`,
`uplot-aas-missing-bucket-zero.png`, `uplot-aas-not-captured-range.png`,
`fidelity-pane-unknown.png`, `fidelity-compare-unknown.png` and
`fidelity-compare-unknown-unavailable.png`. They are the new cells of the U0 web unit
(an unknown fidelity token is never shown as exact; uncaptured ranges are shaded;
missing AAS values stay missing).

**Where they came from:** the CI `snapshots` job's `snapshot-baselines` artifact on
the branch `agent/871-u0-fidelity-incomplete` at 6044446c (Actions run 44045365655,
a `workflow_dispatch` with `update_snapshots=true`). They were not rendered locally.

**No existing baseline was replaced.** The same run regenerated the 72 existing
baselines; compared with the committed files, 66 were byte-identical and 6 differed.
The 6 are the same files the 2026-11-14 entry (868) recorded as within tolerance:
`table_queries.png`, `table_sessions.png`, `aas_chart_overview.png`,
`transition_matrix.png`, `fidelity_sampled_shading.png` and
`gallery/uplot-aas-live-ticks-tick4.png` (14 pixels differing by one level). They
were left as committed.
