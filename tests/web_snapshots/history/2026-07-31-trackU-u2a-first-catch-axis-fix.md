# 2026-07-31 — Track U U2a: First-catch type:'time' axis fix

2026-07-31 (Track U U2a): NINE baselines REGENERATED for the First-catch
type:'time' axis fix (see README "Baseline history"): aas_chart_overview,
fidelity_sampled_shading, gallery/aas-{dense, dense-events, sampled,
mixed-escalation, escalated-live-edge, unicode-names, live-ticks-tick4}.
Generated LOCALLY (Linux 6.8, playwright 1.58.0, chromium 145.0.7632.6) via
a full --update-snapshots run; the 17 untouched baselines came out
byte-identical and were restored from the pre-run copies, and a follow-up
full compare matched all 9 at diff ratio 0.0000 (the only remaining local
failures are the five 2026-07-31 CI-chromium-authoritative cells documented
in README.md, at their pristine-HEAD ratios). NOTE: gallery/aas-live-ticks-
tick4 was one of those five CI-authoritative cells; its provenance is now
local again. If the CI compare churns on it or on the other tight-tolerance
gallery/aas-* cells, regenerate in CI via the README workflow — same PR.

U2a regen environment:
playwright==1.58.0
chromium-version=145.0.7632.6
