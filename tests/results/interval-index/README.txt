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
                                   "no disagreement in 8476 comparisons"
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
                                   0 approved and 0 refused for the wrong
                                   reason (each case now asserts WHICH tally
                                   bucket must be 1, not merely that some
                                   failure was reported)
  selftest-assertion-red.txt       DEMONSTRATED RED for that new assertion:
                                   a copy of the harness with case 7's needle
                                   broken (the exact regression the nit
                                   described) is reported FAIL — "refused for
                                   the WRONG reason: STAYED-GREEN is 0, not
                                   1, so this path was never exercised".
                                   Under the old `failures > 0` form the same
                                   input reported PASS.
  unit-test-linux.txt              the same test as run by the live tier on
                                   gate-1 (gcc 13, x86_64) — 121 checks,
                                   0 failed
  box-check-summary.txt            the box run's last 30 lines: 102 executed,
                                   102 passed, 0 failed, 0 known-failing,
                                   Live UI smoke overall=PASS
  make-check-tail.txt              make check (full) verdict on this Mac
  verify_table.py                  checks the evidence table below against
                                   the logs it cites, BOTH ways: every quoted
                                   phrase must be in the named log, AND every
                                   published value must be in the table. It
                                   refuses (nonzero) on a missing log, an
                                   empty log or an unparseable table rather
                                   than reporting nothing wrong.
  verify-table.txt                 its output: 31 claims, 0 log-side and
                                   0 table-side mismatches, 0 stale values
  verify-table-red.txt             DEMONSTRATED RED, six probes: a stale
                                   value put back, a value changed to one
                                   that is neither right nor stale (the case
                                   the first version of the script MISSED,
                                   because it only checked the log side), the
                                   timing pair flipped to read as a speedup,
                                   a missing log, an empty log and an
                                   unparseable table header. All six exit 1;
                                   the unmodified directory exits 0.

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

REFRESHED 2026-10-09 after review. Six rows had been written BEFORE the
fx_cover fixture was added; its 4 records shift every sweep counter, and the
table was not refreshed when the logs were. The logs below are the committed,
authoritative ones and every number here is now copied from them verbatim.
The corrections were: 8412->8476 comparisons, 794->802 cut masks (794 of
them multi-block), 44074->44266 intervals, 2697->2714 cross-block,
4666->4694 bursts spanning two or more blocks, and the Mac timing pair,
which had read "803 vs 1051" (an apparent speedup) when the committed log
says 1475.0 vs 1266.0 — i.e. the index was SLOWER than raw on that run.
Five of the six understated coverage; the timing one would have read as a
performance claim and is the reason this table is now transcribed, not
retyped.

claim | value | unit | num/den | source | n | commit
------+-------+------+---------+--------+---+-------
unit test green                 | 121 passed / 0 failed | checks | 121/121 | unit-test-mac.txt | 1 | 86ef537
bit-exact answers compared      | 8476 | (cut,window,resolution) answers | 8476/8476 agree | unit-test-mac.txt | 1 | 86ef537
block-cut sets built            | 802 | cut masks | 794 of the 802 were multi-block | unit-test-mac.txt | 1 | 86ef537
intervals compared              | 44266 | intervals | - | unit-test-mac.txt | 1 | 86ef537
cross-block intervals exercised | 2714 | intervals | - | unit-test-mac.txt | 1 | 86ef537
intervals starting before cover | 545  | intervals | - | unit-test-mac.txt | 1 | 86ef537
open-past-`to` exclusions hit   | 3052 | intervals | - | unit-test-mac.txt | 1 | 86ef537
end-before-`from` exclusions hit| 4020 | intervals | - | unit-test-mac.txt | 1 | 86ef537
same pid, two blocks, one bucket| 1058 | answers | - | unit-test-mac.txt | 1 | 86ef537
answers with a bucket peak > 1  | 5286 | answers | - | unit-test-mac.txt | 1 | 86ef537
bursts spanning >= 2 blocks     | 4694 | answers | - | unit-test-mac.txt | 1 | 86ef537
burst onsets detected           | 6214 | onsets | - | unit-test-mac.txt | 1 | 86ef537
windows that selected nothing   | 1148 | windows, all REFUSED | - | unit-test-mac.txt | 1 | 86ef537
prefilter skips across the sweep| 20134 | block skips | - | unit-test-mac.txt | 1 | 86ef537
GATE WENT RED (ledger)          | exit 1, 5 red ledger rows | ledger rows | 5/15 checks | oneblock-falsification-mac.txt | 1 | 86ef537
GATE WENT RED (mutations)       | 27 detected | mutants | 27/27 | mutations.txt | 1 | 86ef537
harness false-negative paths    | 0 wrongly approved, 0 wrong-reason | cases | 0/8 | mutation-selftest.txt | 1 | (post-review)
GATE WENT RED (self-test assert)| exit 1, case 7 FAIL | cases | 1/8 red on the injected regression | selftest-assertion-red.txt | 1 | (post-review)
table transcription verified    | 31 claims, 0 mismatches | claims | 31/31 log-side AND table-side | verify-table.txt | 1 | (post-review)
GATE WENT RED (table verifier)  | 6 probes, all exit 1 | probes | 6/6 red, control 0 | verify-table-red.txt | 1 | (post-review)
storage compaction              | 6.0 | x | 960000 B index / 5760000 B raw | unit-test-mac.txt | 1 | 86ef537
materialised compaction         | 3.0 | x | 960048 B / 2880048 B in window | unit-test-mac.txt | 1 | 86ef537
blocks prefiltered away         | 19 of 40 | blocks | 19/40 | unit-test-mac.txt | 1 | 86ef537
index query vs raw, on this Mac | 1475.0 vs 1266.0 | microseconds | - | unit-test-mac.txt | 1 | 86ef537
                                  The index was SLOWER than raw on this run.
                                  REPORTED, asserted by nothing. One run on
                                  a laptop under concurrent agent load, and
                                  the in-memory raw path it is measured
                                  against does NO disk decode, which is where
                                  the real cost is. NOT a speedup claim in
                                  either direction.
make check (full), FINAL tree    | PASSED (full) | verdict | 29/29 python checks | make-check-tail.txt | 2 | (post-review)
                                  No stamp is quoted anywhere in this
                                  directory, on purpose: check.sh hashes
                                  the whole tree, so any file written
                                  after the run (this one included) makes
                                  a transcribed stamp wrong. The push
                                  guard recomputes it live and refuses a
                                  mismatch; `make check` is run LAST on
                                  this branch so that comparison is live.
                                  verify_table.py treats a stamp in any
                                  evidence file as a STALE value and
                                  refuses it.
box-check gate-1 (live tier)    | exit 0 | tests | 102 passed / 102 executed, 0 failed | box-check-summary.txt | 1 | 86ef537
live UI smoke                   | overall=PASS | - | - | box-check-summary.txt + tests/results/ui_live/summary.json | 1 | 86ef537
unit test on gate-1 (gcc, x86)  | 121 passed / 0 failed | checks | 121/121 | unit-test-linux.txt | 1 | 86ef537
index query vs raw, on gate-1   | 8887.9 vs 9340.0 | microseconds | - | unit-test-linux.txt | 1 | 86ef537
                                  (REPORTED, asserted by nothing; same
                                   caveat as the Mac row above)

WHAT IS NOT CLAIMED
- No end-to-end paint-latency improvement is claimed: this phase ships a
  MODULE with no wiring. `concurrency` still runs the raw path until the
  wiring step lands.
- The 6.0x / 3.0x figures are compaction ratios on a synthetic 120k-record
  fixture, not a measured request latency. The plan predicted "roughly
  3-6x" for this phase and explicitly NOT the O(blocks) collapse Phases 1
  and 2 get; nothing here claims otherwise.
- The microsecond timings above are one run each, and on this Mac the index
  came out SLOWER than raw (1475 vs 1266 us). That is expected and is not a
  regression: both sides of that comparison read an array already in
  memory, so the measurement excludes the only cost this phase removes —
  decoding 48-byte records off disk. No latency claim is made in either
  direction from these numbers; the defensible figures are the compaction
  ratios and the prefilter skip count, which are deterministic and
  asserted.

REVIEWER NITS DELIBERATELY NOT FIXED HERE (they are in src/)
Two comment-only corrections were raised and are NOT applied on this branch,
because any edit under src/ costs a fresh ~40-minute live-tier box run for a
sentence, and the review's exemption covered evidence text only:
  - src/interval_index.c:366-368  REFUSE_BUILD_FAILED is unreachable;
    fail_build() always records a specific reason. It is defence in depth
    against a future build path that latches `failed` without one.
  - src/interval_index.h:238      rows_examined's comment says "rows the
    prefilter let through"; it counts only SELECTED rows, so it always
    equals intervals_used. The rows the prefilter skipped by binary search
    are in excluded_end_before_from, and those skipped past `to` are in
    excluded_open_past_to.
Both should be folded into the WIRING commit, which touches src/ and pays
for that box run anyway.
