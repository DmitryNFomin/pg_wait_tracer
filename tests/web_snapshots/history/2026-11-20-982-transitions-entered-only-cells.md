# 2026-11-20 — issue 982 — Transitions entered-only node: two gallery baselines added

**Added (gallery):** `transitions-entered-only.png` and `transitions-entered-only-800x550.png`. These are the new cells of the #982 fix. A Transitions node that was only entered in the window (it is the target of a counted link and has no closed dwell, for example under an event or class drill-down) is drawn as a small node labelled "entered N×", never "0 ms". The full sentence is in the tooltip, which a web unit test covers.

**Where they came from:** the CI `snapshots` job's `snapshot-baselines` artifact on the branch `agent/947-coverage-writer` at 3eea99a8 (Actions run 44682287907, a `workflow_dispatch` with `update_snapshots=true`). They were not rendered locally.

**No existing baseline was replaced.** The same run regenerated the 88 existing baselines. Compared with the committed files, 82 were byte-identical and 6 differed: the 6 files the 2026-11-14, 2026-11-17 and 2026-11-18 entries record as within tolerance (`table_queries.png`, `table_sessions.png`, `aas_chart_overview.png`, `transition_matrix.png`, `fidelity_sampled_shading.png`, `gallery/uplot-aas-live-ticks-tick4.png`). They were left as committed.
