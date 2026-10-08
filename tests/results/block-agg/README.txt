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

  make-check-full.log
      `make check` (full, not --fast) on this Mac. Last line:
      "CHECK PASSED (full) — stamp 05a2eef5ecdeec888a69a7460ae248a4fea9524a",
      which equals `scripts/tree-hash.sh` on the committed tree, so the stamp
      describes the tree that was actually tested.

  box-check-summary.txt
      The Linux tier, on gate-2 (pgwt-gate-2 / root@142.132.191.18, NOT an
      ephemeral VM): test_block_agg's own output inside the C-unit suite, the
      cross-validation rate sweep (all four rates in tolerance -- #323's
      stricter gate), and the run_all summary: 101 executed, 101 passed,
      0 failed, 0 known-failing, 0 xpass, 4 skipped, live UI smoke PASS,
      exit 0. The ledger numbers are IDENTICAL to the macOS run (378 windows,
      159 seams, 480/335/1075 merge/decode/skip, 7 probes red, 43 refusals),
      which is the point: the aggregate's arithmetic is not platform
      dependent. The full box log is gitignored
      (tests/results/box-check-*.log) and stays in the worktree:
      tests/results/box-check-ubuntu-20261008-224008.log

      Two earlier attempts are NOT evidence and are not claimed as such:
      the first was killed while queued (it had rsynced a tree that was one
      commit stale, so it would have tested the wrong thing); the second died
      at exit 255 on an ssh transport drop about a minute into the run, and
      because scripts/box-check.sh runs the remote suite over a live ssh
      channel with no detachment, the REMOTE command kept going and held the
      box's flock for a full suite duration. tests/run_all.sh has no cleanup
      traps, so killing it mid-live-tier risks orphaned PostgreSQL clusters;
      it was left to drain and this run queued behind it.

WHAT IS NOT HERE
  No `transitions` end-to-end numbers, because nothing is wired into
  src/compute.c / src/server.c yet — those three files were held by another
  branch (the #316/#317/#318 re-cut) for the whole of this task. Phase 1 step
  1 is the standalone module and its proofs; step 2 is the wiring.
