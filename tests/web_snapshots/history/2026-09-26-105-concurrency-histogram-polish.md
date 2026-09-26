# 2026-09-26 — #105: concurrency/histogram axis+legend+pre-capture polish

2026-09-25 (#105 concurrency/histogram axis+legend+pre-capture polish):
FIVE baselines REGENERATED, all by the CI snapshots job (workflow_dispatch
update_snapshots=true on agent/concurrency-histogram-polish, run
36100685301, head 2c123992372e076ee8384e9298861ee068a34e12):
  concurrency_chart.png, concurrency_burst_table.png, histogram_heatmap.png
    — y-axis name moved to nameLocation 'middle' (fixed the "Simultaneous
    Sessions" clip at 1280px, the matrix/#104 fix shape) and histogram's
    visualMap switched continuous -> piecewise (numeric bucket legend,
    mirroring Matrix).
  gallery/histogram-dense.png, gallery/concurrency-dense-bursts.png
    — same axis/legend fix, PLUS both cells' fixtures now carry a small,
    non-overlapping opts.captureFromNs so the new "Not captured" markArea
    (buckets before the server's earliest captured timestamp are excluded
    from the plotted series, never painted as a measured zero) is visible
    in the screenshotted evidence.
Downloaded PNGs' sha256 verified byte-identical to the local copies before
commit. No other baseline in the artifact was touched (verified against
the unrelated cells' own make-ui-gallery diffs, which were pixel-noise from
the pre-existing #122 flexbox-reflow, not sourced from this artifact).

#105 regen environment: the CI snapshots job (ubuntu-latest) at the pins
already recorded at the top of this file — no local PNG was committed.

2026-09-25 (#105 PR #152, post-merge-with-master gallery-diagnosis): the
`snapshots` job on the merge commit (8ec7d02, run 36106352365) FAILED on
FIFTEEN gallery cells, thirteen of them never touched by #105's own fixture
change. Diagnosis before touching anything:
  1. `gh workflow run ci.yml --ref master -f update_snapshots=true` (run
     36117913812, head 8f8c387) downloaded and pixel-compared (same
     threshold/ratio the test uses) against master's own committed
     tests/web_snapshots/: 0/44 cells differ (three gallery cells sit close
     to the tolerance — timeline-single-point 0.0020, aas/uplot
     live-ticks-tick4 0.0019, histogram-dense 0.0017 — but none exceed it).
     Plain master is NOT churning today: no live #122 regression to report
     on the issue.
  2. `gh workflow run ci.yml --ref agent/concurrency-histogram-polish
     -f update_snapshots=true` (run 36117939349, same head 8ec7d02 as the
     failing PR run) was compared two ways:
       a. against the branch's committed baselines: only TWO cells differ
          — gallery/fidelity-compare-mismatch.png (0.0434) and
          gallery/table-configs-compare-delta.png (0.0076, but 0.0234 in
          the original failing PR run — see below).
       b. cell-by-cell against the ACTUAL renders the failing PR run
          (36106352365) saved as CI artifacts (`snapshot-diffs`): of the
          14 cells with usable "-actual" artifacts, 13 differ from this
          second same-commit run's own actual render by 0.0074-0.0317 —
          i.e. two independent CI job executions of the IDENTICAL commit
          render those 13 cells differently from EACH OTHER, not just from
          the committed baseline. That is render-timing noise (dense
          ECharts/uPlot cells not fully settled at screenshot time under
          variable runner load), not a deterministic code effect, and
          NOT #122's cell-insertion/reflow mechanism (no cell was
          inserted between the two runs of the same commit). None of these
          13 were regenerated.
     gallery/fidelity-compare-mismatch.png was the one exception: BOTH
     independent runs (36106352365's actual and 36117939349's artifact)
     differ from the committed baseline by the exact same 3839/88400 px
     (0.0434) and are pixel-identical (0 diff at the test's own threshold)
     to each other — a real, reproducible difference, consistent with
     #122's grid-reflow (this cell's caption row shifted vertically,
     matching a sibling cell growing from #105's captureFromNs change).
     REGENERATED from run 36117939349's snapshot-baselines artifact; sha256
     213bf30113d8af72fd2484b2be3073cdca3073e07e18c843e735c3cc39f4bed5
     verified identical before and after copying into this directory.
     gallery/table-configs-compare-delta.png was NOT regenerated: its own
     diff ratio against the (unchanged) baseline varies between runs
     (0.0234 in 36106352365, 0.0076 in 36117939349) the same way the noisy
     13 do, so it is the same render-timing noise, not a real regression.

2026-09-26 (#105 x #159 merge, re-pin under the container): merging
master (which carries #159's container-pinned rendering environment,
entry above) into this branch re-rendered this branch's own six changed
cells via workflow_dispatch update_snapshots=true on
agent/concurrency-histogram-polish (run 36241234904):
  concurrency_chart.png (sha256 c6d21d13...b3fb8ff) and
  concurrency_burst_table.png (sha256 6d8af3fd...44f8aa5) came back
  BYTE-IDENTICAL to this branch's pre-merge (pre-#159) committed bytes —
  like the #159 entry's own "5 byte-identical" list, neither cell has any
  monospace-styled text, so the container font pin changes nothing here.
  histogram_heatmap.png (57ae266e...c84d397c), gallery/histogram-dense.png
  (325502ef...dcb1f688), gallery/concurrency-dense-bursts.png
  (9bc03783...dcef361e) and gallery/fidelity-compare-mismatch.png
  (c93a94dd...48b0cee5) DID change bytes; diffed against master's current
  (pre-#105) baselines: concurrency_chart 10.60%, histogram_heatmap
  13.43%, gallery/histogram-dense 30.47%, gallery/concurrency-dense-bursts
  9.68%, concurrency_burst_table 0.10%, gallery/fidelity-compare-mismatch
  1.51%. The four large-ratio cells' diff bounding boxes span nearly the
  whole plot area, exactly as expected for this branch's own change (the
  y-axis nameLocation move off the default clipped position reflows the
  grid's height, shifting every plotted point, and the piecewise
  visualMap requantizes every heatmap cell's colour) — not any new,
  unexplained geometry or colour shift. The two small-ratio cells
  (burst_table, fidelity-compare-mismatch) match the font-metric/reflow
  order of magnitude documented elsewhere in this file. All 6 files are
  exactly this branch's own changed set (git diff against its merge-base
  confirms no other cell was ever touched by this branch's commits); the
  other 38 baselines in the artifact were left at master's container-
  pinned bytes already in the merge.
