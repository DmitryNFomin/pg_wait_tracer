tests/results/issue424-index/ — evidence for #424: repairing the interval
index's selection so it agrees with the corrected raw-path semantics.

WHAT THIS IS, AND WHAT IT IS NOT
--------------------------------
This is a CONSISTENCY REPAIR for a regression introduced tonight, not a new
capability. src/interval_index.{c,h} has no product callers — nothing in
src/ references it and it is not in the Makefile's daemon build — so no
shipped behaviour changes here.

The regression: #424 round 2 made compute.c's concurrency_qualifies()
OVERLAP-selected on the raw path, while the interval index still selected by
CLOSE. tests/test_interval_index.c cross-checks the two against each other,
so they stopped agreeing and the test went red on Linux. It passed at
a9b5c98 (round 1) and failed at 6206dd1 (round 2).

Reproduced here on the Mac against agent/424-integration (tip 0a12c7f),
exactly: 122 checks / 2 failed, 6 disagreements, first
`fx_main mask=0x0 w=[240,520] nb=8: peak_sessions[7] 4 vs 0`.

The repair makes the index overlap-selected too. Concurrency occupancy is an
INTERVAL quantity; counting distinct sessions does not make it close-
selected.

Branch agent/424-index, off 304a772. Code commit 6e78861
(src/interval_index.{c,h}, tests/test_interval_index.c,
tests/interval_index_mutations.py, tests/unit_tests.list). This directory is
a separate, artifacts-only commit so no behaviour change rides inside
regenerated output.

Mac = darwin 24.6.0, Apple clang. Built the same way tests/Makefile builds
it (test_interval_index.c + src/interval_index.c + src/compute.c +
wait_event/idle_rule/cJSON, -DPGWT_SERVER).


WHAT CHANGED, IN ONE PARAGRAPH
------------------------------
The old rule selected an interval when `from <= end_ns <= to`, so a wait
still OPEN at `to` was dropped. The new rule selects on OVERLAP — `end_ns >=
from_ns && start_ns <= to_ns`, inclusive at both ends — which is literally
the corrected concurrency_qualifies()'s own test (`timestamp_ns < from_ns`
reject; `timestamp_ns > to_ns && timestamp_ns - duration_ns > to_ns`
reject). Removing the row-level exclusion alone is NOT the fix: the chunk
prefilter's high side was keyed on `min_end_ns`, and a chunk whose intervals
all END past `to` can still hold one that BEGAN before it, so the same
records are dropped one level up with no row-level test ever running. The
chunk bounds therefore became overlap-aware: an exact `min_start_ns` for the
high side, plus `max_duration_ns` to bound the in-chunk scan, which can no
longer stop at the first row ending past `to` because rows ascend in end_ns,
not in start_ns.

WHY THE INDEX ALSO CLIPS, WHEN compute.c NOW CLIPS TOO
------------------------------------------------------
The corrected compute.c clips by clamping b_hi to the last bucket, and keeps
the burst onset at the true start. The index hands a selected interval whose
end is past `to` over as `timestamp_ns = to_ns, duration_ns = to_ns -
start_ns`, which preserves `end - duration` = the true start, so b_lo and
the burst onset are untouched and b_hi lands on the same last bucket. The
two are therefore the SAME answer, which is checked and not argued: the
whole suite was run against agent/424-integration's compute.c and is
164/0 there (regression-after-integration-compute.txt).

The clip is kept rather than dropped for one reason: it makes the index's
answer independent of WHICH compute.c is linked. This branch is based on
304a772, whose compute.c is still close-selected, and without the clip the
differential there could not see a selection bug at all — compute would drop
the extra records itself and index and oracle would agree while both were
wrong. That is the false negative this whole exercise is about. With the
clip, the branch is green on its own base AND against the corrected
compute, and the mutation suite still goes red on both halves of the fix.

PGWT_INTERVAL_INDEX_VERSION 1 -> 2 in the same commit as the header change,
as that header's own rule requires. The bump is not a feature claim: it is
the stamp saying "selection changed", and it makes an index built by
version-1 code refuse rather than answer under version-2 rules.


EVIDENCE TABLE
--------------
Every row is one claim. n = number of runs of that command on this tree.
All at commit 6e78861 unless the row says otherwise.

| claim                                   | value   | unit        | numerator/denominator        | source file                                   | n |
|-----------------------------------------|---------|-------------|------------------------------|-----------------------------------------------|---|
| THE REGRESSION, BEFORE: integration's   | 2       | failed      | 2 FAIL / 122 checks          | regression-before-integration-compute.txt      | 1 |
|   own index+test vs corrected compute   |         | checks      |                              |                                                |   |
|   (= the Linux failure, reproduced)     |         |             |                              |                                                |   |
|   its differential                      | 6       | disagree-   | 6 bad / 8476 comparisons     | regression-before-integration-compute.txt      | 1 |
|                                         |         | ments       |                              |                                                |   |
| THE REGRESSION, AFTER: this branch's    | 0       | failed      | 0 FAIL / 164 checks          | regression-after-integration-compute.txt       | 1 |
|   index+test vs the SAME corrected      |         | checks      |                              |                                                |   |
|   compute (cross-build, 0a12c7f)        |         |             |                              |                                                |   |
|   its differential                      | 0       | disagree-   | 0 bad / 8940 comparisons     | regression-after-integration-compute.txt       | 1 |
|                                         |         | ments       |                              |                                                |   |
| unit test green on this branch's OWN    | 164 / 0 | checks/fail | 164 passed / 164 run         | unit-test-mac.txt                              | 1 |
|   base (304a772, close-selected         |         |             |                              |                                                |   |
|   compute) — green under BOTH computes  |         |             |                              |                                                |   |
| raw differential, index vs corrected    | 0       | mismatches  | 0 bad / 8940 comparisons     | unit-test-mac.txt                              | 1 |
|   loader, bit-exact                     |         |             | (10 fixtures x cuts x 6      |                                                |   |
|                                         |         |             |  windows x 2 resolutions)    |                                                |   |
| intervals open at `to`, clipped         | 7670    | intervals   | 7670 / 8940 answers          | unit-test-mac.txt (ledger)                     | 1 |
| intervals rejected for starting past to | 4       | intervals   | 4 / 8940 answers             | unit-test-mac.txt (ledger)                     | 1 |
| chunk scans a v1 prefilter would have   | 3876    | chunk scans | 3876 / 8940 answers          | unit-test-mac.txt (ledger)                     | 1 |
|   SKIPPED whole (the named trap)        |         |             |                              |                                                |   |
| scans stopped on to + max_duration      | 810     | chunk scans | 810 / 8940 answers           | unit-test-mac.txt (ledger)                     | 1 |
| ledger falsification goes RED           | exit 1  | exit code   | 5 red rows / 18 checks       | oneblock-falsification-mac.txt                 | 1 |
| mutants detected                        | 36      | mutants     | 36 RED / 36 injected         | mutations.txt                                  | 1 |
| mutants the test could NOT see          | 0       | mutants     | 0 STAYED-GREEN / 36          | mutations.txt                                  | 1 |
| non-detections (build/tool/baseline)    | 0       | outcomes    | 0 / 36                       | mutations.txt                                  | 1 |
| harness false-negative paths closed     | 8       | cases       | 8 correct / 8 driven         | mutation-selftest.txt                          | 1 |
| GATE RED, row rule reverted to v1       | 9       | failed      | 9 FAIL / 164 checks          | red-M06-row-rule-reverted.txt                  | 1 |
|   (mutation M06)                        |         | checks      |                              |                                                |   |
| GATE RED, chunk prefilter reverted to   | 7       | failed      | 7 FAIL / 164 checks          | red-M06b-chunk-prefilter-reverted.txt          | 1 |
|   min_end_ns, row rule left CORRECT     |         | checks      |                              |                                                |   |
|   (mutation M06b — the named trap)      |         |             |                              |                                                |   |
| box-check (Linux, live PG)              | NOT RUN | —           | —                            | — ($PGWT_BOX / _EL8 / _EL9 all unset for this  | 0 |
|                                         |         |             |                              |    agent; no live tier claimed)                |   |

`make check` was run on this tree; its result is recorded in
make-check-tail.txt in this directory (added in the same artifacts commit).


THE TWO DEMONSTRATED REDS, AND WHY THERE ARE TWO
------------------------------------------------
src/interval_index.c is not new on this branch, but the parent tree's copy
does not compile against the new header (the retired `excluded_open_past_to`
field), and a build failure is not a detection. The equivalent evidence is
reverting the fix in place, one half at a time:

  red-M06-row-rule-reverted.txt      the row-level rule put back to version
                                     1 (`if (iv->end_ns > to_ns) continue;`),
                                     chunk bounds left correct. 9 red checks,
                                     including the raw differential:
                                       fx_main mask=0x0 w=[240,520] nb=8:
                                       peak_sessions[7] 4 vs 0

  red-M06b-chunk-prefilter-reverted  THE TRAP. The row-level rule left fully
                     .txt            correct and ONLY the chunk prefilter's
                                     high side put back to `min_end_ns >
                                     to_ns`. 7 red checks:
                                       fx_order: exactly 1 chunk would have
                                       been SKIPPED by a version-1 end-range
                                       prefilter and had to be scanned
                                       (got 0)
                                       fx_faredge: query answered
                                       (no-intervals-selected)
                                       no disagreement in 8844 comparisons —
                                       first: fx_main mask=0x80 w=[240,520]
                                       nb=8: peak_sessions[7] 4 vs 0

The second run is the point of the whole change: a correct row rule with a
version-1 chunk bound silently answers SHORT, and fx_faredge degrades all
the way to REFUSE_NO_INTERVALS rather than to a wrong number — which is the
module's fail-safe rule holding, but only because the window happened to
select nothing at all.

Full per-mutant output for all 36 is in
tests/results/interval-index/mutations/m01..m36.txt.


THE BYPASS SUITE FOR THE NEW GATES
----------------------------------
Each new gate is tested for the ways its detection can be made unreachable,
and each way is demonstrated failing by a mutation:

  gate: overlap-aware chunk high bound (min_start_ns)
    unreachable if   the suite never builds a chunk whose intervals all end
                     past `to`
    pinned by        ledger row "chunk scans happened ONLY because ..." —
                     FAILS at 0; fx_order asserts exactly 1 by value and
                     fx_faredge exactly 4
    red shown by     M06b (prefilter reverted), M10 (bound narrowed to
                     half-open), M06i (bound computed from the first row
                     only — seal's cross-check AGREES with that one, so only
                     the differential can see it)

  gate: in-chunk scan bound (to + max_duration)
    unreachable if   no chunk ever has a row past the bound
    pinned by        ledger row "chunk scans stopped on ..." — FAILS at 0
    red shown by     M06f (bound tightened back to `to`), M06j (max_duration
                     understated by the shared helper)

  gate: seal's chunk-bound re-derivation
    satisfied without checking if the bound is corrupted in a way the
                     re-derivation reproduces
    pinned by        four bypass cases, one per bound, each corrupted in the
                     direction that LOSES rows; and by the shared helper
                     being ONE function, so M06i/M06j prove that a bound
                     computed wrongly is caught by the differential and NOT
                     by this check — the two gates are not the same gate
                     wearing two hats
    red shown by     M06i, M06j

  gate: sorted-by-end_ns invariant at seal
    was being satisfied by the WRONG check: the bypass case swapped rows
                     ACROSS chunks, which the new bound re-derivation
                     catches, so M25 (sort check removed) STAYED GREEN on
                     the first run of this suite. The case now swaps two
                     rows WITHIN one chunk, where all four bounds are
                     order-independent and therefore still exactly right.
    red shown by     M25

  gate: inclusivity at the far edge (`start_ns <= to_ns`)
    unreachable if   no fixture has an interval starting exactly at `to`
    was unreachable: M07 and M10 both STAYED GREEN on the first run. A wait
                     starting exactly at `to` contributes to NO bucket, so
                     peaks cannot see it at all; fx_faredge makes it the
                     fourth member of a 4-pid burst, which disappears
                     without it.
    red shown by     M07, M10

  gate: materialisation order
    unreachable if   no bucket is decided by a tie between two events whose
                     records sort differently by end_ns and by start_ns
    pinned by        fx_order bucket 2 (literal peak_event == EV_A) and
                     fx_main bucket 7 (literal peak_event == EV_C)
    red shown by     M06h (array re-sorted by start_ns)

  absent rather than wrong: fx_order queried over [100, 1000], where nothing
  is open at `to`, must still ANSWER and report clipped_open_past_to == 0 and
  chunks_overlap_rescued == 0. A counter that is never zero measures nothing.


THE THREE ADVERSARIAL QUESTIONS, ANSWERED BEFORE THE CODE WAS WRITTEN
---------------------------------------------------------------------
1. What input makes this check pass while the product is broken?
   A fixture set in which no interval is ever open at `to` for any swept
   window: v1 and v2 then agree and the whole differential is vacuous. Also
   an oracle that shares the clip helper with the module, which would make
   the clip arithmetic untestable by construction. Stopped by: the oracle
   being a standalone linear pass (corrected_loader_clip) that calls nothing
   in src/interval_index.c; the ledger rows above, each of which FAILS at
   zero; and the `oneblock` falsification.

2. What single component failing makes this hang or skip, rather than fail?
   `cc` absent (127) or not executable (126) turns every mutant into
   BUILD-FAILED, which would score as "36 of 36 detected" in a naive
   harness. interval_index_mutations.py already scores TOOL-MISSING,
   BUILD-FAILED, NOT-APPLIED, BASELINE-RED and STAYED-GREEN as five distinct
   FAILURES and `--self-test` drives all eight paths (mutation-selftest.txt).
   In the unit test itself, a build_index() failure would turn the sweep into
   zero comparisons; pinned by `c_cut_masks > 50` and `c_triples > 500`.
   Nothing in this file waits on anything, so there is no hang path.

3. What here depends on timing or ordering, and what pins it?
   Nothing wall-clock: every fixture is a literal array and the single
   timing number printed is REPORTED, never asserted. ORDER decides
   peak_event, and version 2 makes that sharper, because clipping rewrites
   the timestamp the array would otherwise be sorted by. Pinned by fx_order
   bucket 2 and fx_main bucket 7 as literal constants, by the index refusing
   a non-monotone feed instead of sorting it, and by mutation M06h.


OPEN QUESTIONS (for the loader agent / the owner)
-------------------------------------------------
1. INCLUSIVITY AT THE FAR EDGE — RESOLVED, no longer a question. This module
   admits `start_ns <= to_ns` (inclusive). The corrected concurrency_qualifies
   at 0a12c7f rejects only when `timestamp_ns - duration_ns > to_ns`, i.e. it
   admits `start == to` too. The conventions match, and fx_faredge (whose
   fourth burst member starts exactly at `to`) is the case that pins it on
   this side. Verified by the cross-build, not by reading alone.

2. THE v2 WIRING PRECONDITION. Overlap admission can only find a wait open
   at `to` if the END record closing it was FED, and that record lies past
   `to`. Coverage containment (`to <= cover_to_ns`) therefore no longer
   implies completeness: a window ending exactly at cover_to_ns cannot see
   waits closed past the coverage, and the index cannot tell whether such a
   record exists. The corrected loader carries the same obligation. Stated
   in the header as the caller's; whoever wires this must decide whether to
   refuse such a window or to guarantee the feed runs past it.

3. NO LINUX TIER. $PGWT_BOX, $PGWT_BOX_EL8 and $PGWT_BOX_EL9 were all unset
   for this agent, so `make box-check` was not run and nothing here claims a
   live pass. The change is pure C with no BPF, no kernel and no layout
   dependency, but that is an argument, not evidence. The regression this
   repairs was FOUND on Linux, so the repair should be confirmed there.

4. SCOPE JUDGEMENT FOR THE REVIEWER. Three pieces go slightly beyond the
   minimum the regression forces, and each is here deliberately:
   - seal() re-deriving the chunk bounds and refusing on a mismatch. New
     machinery guarding new bounds. A prefilter bound that narrows silently
     is this bug's exact failure class, so it is guarded; cut it if the
     reviewer prefers the smaller diff.
   - the four new counters (clipped_open_past_to, excluded_start_past_to,
     chunks_overlap_rescued, chunks_scan_limited). These are not features;
     they are how the non-vacuity ledger proves the new code paths were
     reached, and each FAILS the test at zero.
   - fx_faredge and fx_order. Without fx_faredge, mutations M07 and M10 both
     STAYED GREEN — the far edge was untestable.
   Nothing here adds a product capability, and the index still has no
   callers. Part 2 of the original brief (the summary seal edge) was NOT
   started.
