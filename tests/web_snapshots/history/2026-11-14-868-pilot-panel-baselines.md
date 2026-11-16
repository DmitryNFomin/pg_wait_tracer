# 2026-11-14 — issue 868 — evidence-v1 pilot panel: two baselines added

**Added:** `pilot_complete.png` and `pilot_unavailable.png`, the two states of the
pilot panel in the Events view (`?pilot=1`).

**Where they came from:** the CI `snapshots` job's `snapshot-baselines` artifact on
the branch `agent/868-pilot-a-route` (Actions run 43601351584, a `workflow_dispatch`
with `update_snapshots=true`). They were not rendered locally.

**No existing baseline was replaced.** The same run regenerated all 70 existing
baselines as well; compared with the committed files:

- 64 were byte-identical;
- 6 differed within the compare tolerance and were left as committed:
  `table_queries.png` (0.0004 of pixels beyond the channel threshold),
  `table_sessions.png` (0.0047), `aas_chart_overview.png` (0.0036),
  `transition_matrix.png` (0.0042), `fidelity_sampled_shading.png` (0.0036), against
  the pane limit of 0.02; and `gallery/uplot-aas-live-ticks-tick4.png` (0 pixels
  beyond the gallery threshold; 14 pixels differing by one level) against 0.002.

So the stylesheet change that came with the panel did not move any existing view
beyond rendering noise.
