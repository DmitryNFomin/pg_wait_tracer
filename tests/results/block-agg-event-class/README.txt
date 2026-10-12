Evidence for paint-latency Phase 2 — per-event totals and the per-class fold
branch agent/block-agg-event-class-totals, module only (no src/compute.c,
no src/server.c change, nothing wired yet)

FILES
  ADVERSARIAL.txt                     the three adversarial questions, answered
                                      before any code was written
  unit-green-macos.txt                tests/test_block_agg.c, full green run on
                                      this Mac, with the §0 vacuity ledger
  mutations-summary.txt               the current mutation/bypass verdict table
  mutations-run1-N6-GREEN-HOLE.log    FIRST sweep: 22 RED, 1 GREEN-HOLE (N6).
                                      Kept on purpose — see "N6" below
  mutations-run2-all-red.log          second sweep after the hole was closed:
                                      23 RED, 0 NOT RED
  mutations-summary-selftest.txt      the summary the --selftest run writes
                                      (separate file so it cannot clobber the
                                      real one)
  selftest.txt                        the DRIVER's own blind-spot check
  mutation-N*.log                     one log per mutation
  build.sh                            out-of-tree build of the suite on macOS
                                      (src/spawn.c needs Linux pipe2)

WHAT THE ORACLE IS
  pgwt_compute_top_events() in src/compute.c — the SHIPPING function the Events
  tab is drawn from — compiled into the suite with -DPGWT_SERVER and called on
  exactly the records a raw loader would have admitted for the window (C7,
  inclusive at both ends). Not a restatement. Comparisons are bit-exact: no
  epsilon, no tolerance, no retry, no sleep anywhere in this phase.

  The ORACLE IS MUTATED TOO (N20-N23). Mutating only the module under test can
  prove the two sides AGREE; it cannot prove either is right, because a bug
  shared by aggregate and oracle is invisible to a differential. N20 removes
  the oracle's clip, N21 removes event_window_ns()'s start clamp, N22 removes
  its io_worker exclusion, N23 moves its own latency-bucket edge. All four RED.

N6 — THE HOLE THE FIRST SWEEP FOUND, AND WHY THE LOG IS KEPT
  Run 1: 22 RED, 1 GREEN-HOLE. N6 drops the `|| is_new` clause from the
  SAMPLE_CONT count rule and the suite stayed GREEN, because §11 E12's fixture
  fed a plain record first — so the continuation fragment was never the FIRST
  record for its event and the clause was never exercised. A surviving mutant
  is this suite failing, not a passing build. §11 E12b now feeds a LONE
  fragment, which is the case where the missing clause leaves the slot at
  count 0 and the whole row vanishes from the answer. Run 2: 23 RED.

THE DRIVER IS ITSELF A GATE (selftest.txt)
  `run_mutations.py --selftest` checks the three ways the driver could approve
  without having seen anything:
    - a missing compiler (OSError / exit 127): the driver must REFUSE with
      "CONTROL IS RED" and exit 2, not print a tidy row of GREENs. It used to
      die with a traceback and no verdict; it now reports COMPILER UNAVAILABLE;
    - a mutation whose anchor text is gone: NOT-APPLIED, counted as a hole,
      nonzero exit;
    - an EMPTY mutation set: exit 3, "a driver that proved nothing must not
      report success".
  Current verdict: PASS.

NON-VACUITY, AND IT IS FALSIFIED RATHER THAN ASSERTED
  §0 carries twelve Phase-2 counters that FAIL the run at zero. The two that
  matter most are ev_clipped_records (4224) and ev_merge_refused_clip (1938):
  the only interesting record for C10 is one whose wait STARTED before the
  window opened, and without such a record the clip could be deleted entirely
  and the differential would stay green.

  §10f falsifies them: the same fixture, the same boundary generator, the same
  comparison, with every duration set to ZERO — both counters must then read 0,
  and they do. The FIRST lever tried was 1 ns durations and it did NOT falsify
  anything (the boundary set contains every record's END timestamp, so a window
  opening at `ts` still clips a 1 ns record that ended there). That rejection is
  the falsification working, and it is recorded in the code so the next reader
  does not re-derive it.

  §9e exists for the same reason one layer up: §10b drives a MIRROR of
  pgwt_block_agg_window_from_reader()'s REFUSED_CLIP -> DECODE downgrade, and a
  mirror cannot pin the function it mirrors. Mutation N12 makes the shipped
  builder SKIP a clip-refused block instead of decoding it; §10b stays green
  (it never calls the shipped function) and only §9e catches it.

THE TRIPWIRE
  window_from_reader()'s non-TRANSITIONS refusal used a hand-written half-open
  `bi.first_timestamp_ns < to_mono_ns` while C7 is inclusive — unreachable in
  Phase 1 only because the server refused any window a SAMPLES block touched,
  which is the refusal Phase 2 lifts. It now goes through the shared
  pgwt_block_agg_file_can_contribute(). §9c-bis pins the exact window that
  tells the two readings apart (a SAMPLES block whose first record ends exactly
  at `to`: inclusive REFUSES, half-open silently SKIPS and answers a MIXED
  window as EXACT), plus the window one tick earlier that must still answer, so
  the fix cannot be a blanket refusal wearing the shape of a boundary rule.
  Mutation N13 puts the old comparison back; §9c-bis goes red.

HOW TO REPRODUCE
  sh  tests/results/block-agg-event-class/build.sh   # prints the binary path
  python3 tests/results/block-agg-event-class/run_mutations.py
  python3 tests/results/block-agg-event-class/run_mutations.py --selftest

REVIEW QUESTION: THE LARGE MUTATION LOGS
  The original commits deliberately kept machine-generated mutation output as
  evidence, including the first sweep's N6 GREEN-HOLE. Much of the volume was
  the same failed differential assertion repeated for different rows/windows.
  Before this landing pass, de11ead (rebased as bb0b733) added an explicitly
  labelled digest and ec30211 (rebased as ac64cc9) regenerated the logs through
  it. Those commits already replaced the
  bulk at the branch tip; this landing pass removes no evidence and does not
  rewrite those historical commits to purge their full logs.

  A reviewer can start with mutations-summary.txt, mutation-control.log,
  unit-green-macos.txt, selftest.txt and mutations-run1-N6-GREEN-HOLE.log, then
  inspect the per-mutant digests (especially N20-N23 for the oracle). Long-run
  digests retain the original line/FAIL counts, assertion examples, first FAIL
  lines and final non-vacuity ledger. They are summaries, not full transcripts.
  Re-run the commands above to independently verify the claims on the current
  tree; historical full transcripts remain accessible with git show at commits
  preceding ac64cc9. The box-check digest is separately labelled and names its
  full, gitignored log; that file's availability depends on the original
  worktree, so a new box run is required for current-tree evidence.
