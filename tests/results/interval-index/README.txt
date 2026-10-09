tests/results/interval-index/ — raw evidence for paint-latency Phase 4
(concurrency interval index, src/interval_index.{c,h}).

Branch agent/concurrency-interval-index, commit 86ef537 (module + test +
mutation harness + build wiring). Mac = darwin 24.6.0 / Apple clang.

FILES
  adversarial.txt                  the three adversarial questions, answered
                                   BEFORE any code was written
  unit-test-mac.txt                ./test_interval_index — 121 checks, 0 failed
  oneblock-falsification-mac.txt   ./test_interval_index oneblock — exit 1,
                                   5 of 15 ledger rows RED. This is the
                                   non-vacuity ledger's own falsification:
                                   every fixture fed as ONE block. Note that
                                   "no disagreement in 8412 comparisons"
                                   stays GREEN in that run — the raw
                                   differential alone cannot see a block
                                   boundary, which is exactly why the ledger
                                   exists.
  mutations.txt                    27 injected silent-wrong risks, 27 RED,
                                   0 STAYED-GREEN / BUILD-FAILED /
                                   NOT-APPLIED / TOOL-MISSING / BASELINE-RED
  mutations/m01..m27.txt           each mutant's full test output
  mutations/oneblock.txt           the ledger falsification run the harness
                                   itself requires to be red before it will
                                   score any mutant
  mutation-selftest.txt            interval_index_mutations.py --self-test —
                                   8 deliberate harness false-negative paths,
                                   0 wrongly approved
  unit-test-linux.txt              the same test as run by the live tier on
                                   gate-1 (gcc 13, x86_64) — 121 checks,
                                   0 failed
  box-check-summary.txt            the box run's last 30 lines: 102 executed,
                                   102 passed, 0 failed, 0 known-failing,
                                   Live UI smoke overall=PASS
  make-check-tail.txt              make check (full) verdict on this Mac

  The two multi-hundred-kilobyte logs are deliberately NOT committed:
  tests/results/box-check-*.log is gitignored (.gitignore:69) and the full
  make check log is 142 KB of pass lines. Both stay in this worktree for the
  reviewer:
     tests/results/box-check-ubuntu-20261009-083659.log   (exit=0)
     tests/results/ui_live/summary.json                   (ok: true)
  Target was root@2.28.45.47 = gate-1 (remote hostname pgwt-gate), NOT
  gate-2 (#331). EPHEMERAL=0, so no Hetzner VM was created; the
  hetzner-sweep line at the top of the run reports deleted=0 warned=0.
  The run waited ~1h on /tmp/pgwt-box-check.lock rather than switching
  boxes, as instructed.

EVIDENCE TABLE (one row per claim)

claim | value | unit | num/den | source | n | commit
------+-------+------+---------+--------+---+-------
unit test green                 | 121 passed / 0 failed | checks | 121/121 | unit-test-mac.txt | 1 | 86ef537
bit-exact answers compared      | 8412 | (cut,window,resolution) answers | 8412/8412 agree | unit-test-mac.txt | 1 | 86ef537
block-cut sets built            | 794 | cut masks | - | unit-test-mac.txt | 1 | 86ef537
intervals compared              | 44074 | intervals | - | unit-test-mac.txt | 1 | 86ef537
cross-block intervals exercised | 2697 | intervals | - | unit-test-mac.txt | 1 | 86ef537
intervals starting before cover | 545  | intervals | - | unit-test-mac.txt | 1 | 86ef537
open-past-`to` exclusions hit   | 3052 | intervals | - | unit-test-mac.txt | 1 | 86ef537
same pid, two blocks, one bucket| 1058 | answers | - | unit-test-mac.txt | 1 | 86ef537
bursts spanning >= 2 blocks     | 4666 | answers | - | unit-test-mac.txt | 1 | 86ef537
burst onsets detected           | 6214 | onsets | - | unit-test-mac.txt | 1 | 86ef537
GATE WENT RED (ledger)          | exit 1, 5 red rows | ledger rows | 5/15 | oneblock-falsification-mac.txt | 1 | 86ef537
GATE WENT RED (mutations)       | 27 detected | mutants | 27/27 | mutations.txt | 1 | 86ef537
harness false-negative paths    | 0 wrongly approved | cases | 0/8 | mutation-selftest.txt | 1 | 86ef537
storage compaction              | 6.0 | x | 960000 B index / 5760000 B raw | unit-test-mac.txt | 1 | 86ef537
materialised compaction         | 3.0 | x | 960048 B / 2880048 B in window | unit-test-mac.txt | 1 | 86ef537
blocks prefiltered away         | 19 of 40 | blocks | 19/40 | unit-test-mac.txt | 1 | 86ef537
index query vs raw recompute    | 803 vs 1051 | microseconds | - | unit-test-mac.txt | 1 | 86ef537
                                  (REPORTED, not asserted; one run, one
                                   loaded laptop — do not quote as a speedup)
make check (full)               | PASSED, stamp df0e1433 | - | - | make-check-tail.txt | 1 | 86ef537
box-check gate-1 (live tier)    | exit 0 | tests | 102 passed / 102 executed, 0 failed | box-check-summary.txt | 1 | 86ef537
live UI smoke                   | overall=PASS | - | - | box-check-summary.txt + tests/results/ui_live/summary.json | 1 | 86ef537
unit test on gate-1 (gcc, x86)  | 121 passed / 0 failed | checks | 121/121 | unit-test-linux.txt | 1 | 86ef537
index query vs raw, on gate-1   | 8888 vs 9340 | microseconds | - | unit-test-linux.txt | 1 | 86ef537
                                  (REPORTED, not asserted)

WHAT IS NOT CLAIMED
- No end-to-end paint-latency improvement is claimed: this phase ships a
  MODULE with no wiring. `concurrency` still runs the raw path until the
  wiring step lands.
- The 6.0x / 3.0x figures are compaction ratios on a synthetic 120k-record
  fixture, not a measured request latency. The plan predicted "roughly
  3-6x" for this phase and explicitly NOT the O(blocks) collapse Phases 1
  and 2 get; nothing here claims otherwise.
- The microsecond timings above are one run on a Mac under concurrent agent
  load. They are printed by the test and asserted by nothing.
