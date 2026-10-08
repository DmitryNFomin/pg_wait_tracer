tests/results/block-agg — raw evidence for paint-latency Phase 1
(src/block_agg.c, tests/test_block_agg.c; docs/PAINT_LATENCY_PLAN.md Phase 1)

Written BEFORE the report was composed, so every number in the report has a
file here to check it against.

FILES

  unit-green-macos.txt
      tests/test_block_agg.c, green, with its vacuity ledger. The ledger is
      the part to read: a green run that exercised nothing is not a pass, so
      the suite counts what it touched and main() fails if any counter is
      unmet. 378 windows compared, 159 of them at a merge/decode seam, 480
      block merges, 335 block decodes, 1075 skips, 7 of 7 mutation probes red,
      42 refusals observed.

  mutation-control.log, mutation-M1..M14.log, mutations-summary.txt
      Produced by run_mutations.py. Each log breaks ONE guard in
      src/block_agg.c and records the suite's output. Control GREEN, all 14
      mutations RED. A GREEN mutation would be a hole in the suite and the
      driver labels it GREEN-HOLE and exits nonzero — that happened once
      during development (M13, the readout's tie order), which is why
      test_block_agg.c §3b asserts the total order as a property instead of
      relying on two merge orders differing.

  run_mutations.py
      The driver. Re-runnable: `python3 tests/results/block-agg/run_mutations.py`
      from the repo root. A mutation whose anchor text no longer exists in
      src/block_agg.c is reported NOT-APPLIED rather than quietly skipped — a
      mutation that did not apply proves nothing.

  bench-macos.txt
      tests/bench_block_agg, 5 reps per cell. Phase 1's falsifiable
      prediction, O(events in window) -> O(blocks x distinct pairs), made
      falsifiable by holding the block count AND the distinct-pair count fixed
      while growing events per block 16x. Merge time is flat down each group
      (16 blocks: 0.021 / 0.027 / 0.023 ms; 64 blocks: 0.073 / 0.088 /
      0.082 ms; 256 blocks: 0.375 / 0.376 / 0.377 ms) while raw compute grows
      with events (0.036 -> 0.228 ms, 0.068 -> 0.774 ms, 0.198 -> 3.204 ms).
      Merge time DOES grow with block count, ~linearly, which is the
      prediction and not a defect.

      Two honesty notes on this bench:
        - At 256 blocks x 256 events/block the merge is SLOWER than raw
          (0.5x). The aggregate wins only when events per block is large,
          which is the real case (PGWT_BLOCK_EVENTS = 4096).
        - It measures COMPUTE only. The cost Phase 1 actually targets is the
          raw LOAD (~0.68 s per million records), which merging interior
          blocks avoids entirely. That saving is not in this table; it is the
          reason the table exists.

  box-*.log (if present)
      The Linux tier: `make box-check` output, including this test inside the
      C unit suite.

WHAT IS NOT HERE
  No `transitions` end-to-end numbers, because nothing is wired into
  src/compute.c / src/server.c yet — those three files were held by another
  branch (the #316/#317/#318 re-cut) for the whole of this task. Phase 1 step
  1 is the standalone module and its proofs; step 2 is the wiring.
