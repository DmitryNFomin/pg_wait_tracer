# 2026-07-31 — Track U U1: table_queries identity color + gallery cells added

2026-07-31 (Track U U1): table_queries.png REGENERATED (deliberate P2
identity-color change, see README "Baseline history") and the 13 gallery/
cell baselines ADDED. These were generated LOCALLY (Linux 6.8, playwright
1.58.0, chromium 145.0.7632.6) — a deviation from the CI-only rule above,
taken because this environment demonstrably reproduces the committed CI
baselines: a pristine-tree compare immediately before the U1 changes matched
12/13 pane baselines at diff ratio 0.0000 (session_timeline 0.0003), the
sole failure being the deliberate table_queries change itself. If the CI
compare job nevertheless churns on any of these (esp. the tight-tolerance
gallery/ cells), regenerate them in CI via the README workflow and update
this file.

gallery/ local generation environment:
playwright==1.58.0
chromium-version=145.0.7632.6
