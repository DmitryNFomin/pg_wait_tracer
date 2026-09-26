# 2026-09-25 — #121: inverted timeline intervals

2026-09-25 (#121, inverted timeline intervals): ONE baseline REGENERATED,
by the CI snapshots job (workflow_dispatch update_snapshots=true on
agent/timeline-inverted-intervals, run 36082814005).
  REGEN  session_timeline (ratio 0.0470) — buildTimelineOption no longer
         clamps a wait with no overlap with the view window to an inverted
         (start>end) interval pinned to the left edge; such waits are now
         dropped entirely. The mock's canned session_timeline fixture
         (tests/mock_server.py) was also re-anchored to the client's
         default 15-min live window (it previously anchored all 8 events
         ~55-58 min in the past, i.e. entirely before that window — exactly
         the shape this bug produced), so the pane now draws the same 8
         waits at their correct, non-inverted positions instead of eight
         spurious left-edge slivers.
  The artifact's other 6 non-zero-ratio baselines (table_overview 0.0021,
  fidelity_sampled_shading 0.0006, gallery/uplot-aas-live-ticks-tick4
  0.0021, gallery/exec-scatter-dense-downsampled 0.0005,
  gallery/aas-live-ticks-tick4 0.0021, gallery/histogram-dense 0.0019) are
  all inside the suite's own gallery/pane noise budget and touch no
  timeline builder or fixture — left untouched, matching the #103 entry's
  precedent above. `make ui-gallery` (this Mac, same before/after
  environment) independently confirms: only session_timeline changed
  (4.70% pixels), every gallery/timeline-* cell at 0.00%.

#121 regen environment: the CI snapshots job (ubuntu-latest) at the pins
already recorded at the top of this file — no local PNG was committed.
