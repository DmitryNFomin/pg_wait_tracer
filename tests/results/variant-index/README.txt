Evidence for the two commits on top of the variant-index module
===============================================================

Branch: agent/variant-sequence-index   PR #344
Rebased onto: 2fbf8fb ("Bound every unbounded probe loop in compute.c by
growing, never dropping (#327, #347)", PR #358).

  commit A  "Bound the variant index's merge probe loop by growing it
             (#327, 7th site)"
  commit B  "Collect variants in first-execution order, matching the oracle
             after #358"
  this one  generated artifacts only, nothing else.

Parent module = src/variant_index.c as it stood at the branch's second
commit ("Mutation harness for the step-sequence index..."), i.e. without
either fix.

WHAT WAS WRONG, TWICE
---------------------

A. #327's SEVENTH probe site. pgwt_variant_index_query() merged executions
   into a FIXED 4096-slot array probed with no fullness test:

       while (ht[slot].used && ht[slot].hash != e->hash)
           slot = (slot + 1) & (PGWT_VARIANT_INDEX_HT_SIZE - 1);

   copied verbatim from pgwt_compute_variants. With 4096 distinct pattern
   hashes in the table, the 4097th never terminates. pgwt-server is
   single-threaded, so the request never answers and every tab stops at
   once. The table now GROWS and never drops a key; a growth that cannot
   allocate refuses the WHOLE result, with the counts poisoned AND
   res.failed = 1, because -1 is truthy in C and a poison value alone stops
   no caller.

B. #358 changed the ORACLE under this branch, mid-gate-run. It replaced
   compute.c's fixed variant table with an index over a growable accumulator
   array indexed by INSERTION order, so the oracle now collects variants in
   first-execution order rather than hash-slot order. qsort() breaks ties by
   input order, and tied variants are common, so the index -- still
   collecting by walking slots -- returned the same set of variants in a
   different arrangement. The index now collects by vi_accum.seq, which
   reproduces the oracle's order exactly.

EVIDENCE TABLE
--------------

claim                                       value          source
------------------------------------------- -------------- --------------------------
unit checks, both fixes, this Mac           174 / 0 fail   unit-test-mac.txt
  pre-existing (144) + #327 saturation (24)
  + 2 new ledger rows + 4 oracle
  differentials at saturation                = 174         unit-test-mac.txt
bit-exact differential quadruples            42,266        unit-test-mac.txt (ledger)
differential disagreements                   0             unit-test-mac.txt (ledger)
RED for B: parent ORDER vs post-#358 oracle  2 failed       order-358-red.txt
  the differing field                        3 = hash       order-358-red.txt
  total_ns of all five big-trace variants    940800000 each order-358-red.txt
                                             (so the sort key cannot separate them)
RED for A: parent module, macOS, timeout 240 exit 124       prefix-hang-mac.txt
  last line printed                          "4097 distinct patterns: index
                                               built over 8 blocks, 16388 records"
  B's two failures also visible in that run   2 FAIL lines  prefix-hang-mac.txt
mutations injected / not detected            46 / 0        mutations.txt
  pristine baseline                          174 / 0 fail  mutations.txt
  added for A (G17, G18, G19)                3             mutations.txt
  added for B (G20, G21)                     2             mutations.txt
G21 under AddressSanitizer: refuses, does    37 failed,    g21-oob-guard-asan.txt
  not write past the result array            0 asan reports
ledger falsification, re-run on this tree    exit 2        oneblock-falsification-mac.txt
windows answered above 4096 variants          1 (ledger)   unit-test-mac.txt
growth-allocation refusals at saturation      1 (ledger)   unit-test-mac.txt
saturated windows the ORACLE also answered    1 (ledger)   unit-test-mac.txt

The Linux tier (`make box-check` on an Ubuntu VM: build, C units at -O2,
synthetic data, protocol drift, real-PG capture, live-UI smoke) and the
ASan + LeakSanitizer run of probe327.c are in the PR body, because their
logs are gitignored and the VM is deleted at the end of the run.

WHAT EACH FILE IS
-----------------

unit-test-mac.txt
    tests/test_variant_index.c against the committed module (cc -O1, the same
    command tests/variant_index_mutations.py uses). 174 checks, 0 failed. The
    "--- #327 seventh site ---" block is the saturation suite; the
    "non-vacuity ledger" below it holds the three rows that FAIL the test if
    no window ever exceeded 4096 variants, if no growth refusal was observed,
    or if no saturated window was ever compared against the oracle.

prefix-hang-mac.txt
    THE RED FOR BOTH FIXES, in one run: the committed test file built against
    the PARENT module and run under `timeout 240 stdbuf -oL`. Two FAIL lines
    from B (the ordering divergence), then exit 124 -- it did not terminate --
    inside A's 4097-pattern query. 4095 and 4096 PASS pre-fix, because a table
    that still has a free slot always stops; 4097 is the first count that
    cannot. A's pre-fix symptom is a HANG, not a failed assertion, which is
    why it is recorded as a timeout rather than as FAIL lines.

order-358-red.txt
    B's red, at field resolution. A scratch copy of the test, instrumented to
    print which field variants_diff() stopped on and to dump both sides, built
    against a slot-order module. field=3 is the hash; every variant's total_ns
    is identical, so the sort key cannot order them and the arrangement is
    whatever qsort() received. Rows 0 and 4 are swapped between the two sides.
    The instrumentation is NOT committed -- it exists to name the field, and
    the committed test fails on the same two checks without it.

g21-oob-guard-asan.txt
    Mutation G21 breaks the seq invariant by one. Collecting by seq means an
    indexed write, so the module checks the index and refuses the whole result
    (PGWT_VARIANT_INDEX_REFUSE_INTERNAL, "internal-invariant") instead of
    writing one variant past the end of the array. Run under
    AddressSanitizer: 37 failing checks and ZERO sanitizer reports -- it
    refuses rather than scribbling.

oneblock-falsification-mac.txt
    PGWT_VARIANT_INDEX_ONE_BLOCK=1 re-runs the sweeps with no block cuts and
    REQUIRES the carry-over counters to be zero, which is what proves the
    ledger is a real gate rather than decoration. Exit 2; that mode can never
    exit 0.

mutations.txt + mutations/m01..m46.txt
    46 silent-wrong risks injected into a copy of src/variant_index.c one at a
    time, each requiring the unit test to go RED. 46 RED, 0 undetected,
    pristine baseline green. BUILD-FAILED, NOT-APPLIED, STAYED-GREEN and a
    hung mutant are each scored as FAILURES, so the harness cannot report
    "red" for a mutation that never reached a binary.

    Most ways to break site A HANG rather than answer wrongly -- that IS the
    defect -- and a hung mutant is a failure here, so only mutations that are
    wrong WHILE STILL TERMINATING could be listed:

      G17 the rehash drops half the keys        -> 17 RED. Since commit B this
                                                   now trips the seq/count
                                                   invariant and REFUSES, which
                                                   is the fail-safe outcome,
                                                   and the test still sees it
      G18 the growth hook is renamed            -> 1 RED (the refusal case
                                                   silently went green)
      G19 the rehash loses the p95 sample       -> 6 RED (p95 = 0 on 3072 of
                                                   the grown-over variants)
      G20 collect in SLOT order (B's pre-fix)   -> 2 RED, both differentials
      G21 seq off by one                        -> 37 RED, all refusals

    M25's needle was re-pointed at the current `int nr = nv < max_variants`
    line. Left alone it would have been reported NOT-APPLIED, which is the
    harness's own false-negative guard working.

probe327.c
    A 90-line standalone probe: 4097 distinct patterns, one query, plus the
    growth-refusal path. Used for the Linux hang/ASan runs quoted in the PR
    body. Evidence, not a gate -- deliberately not in tests/unit_tests.list,
    because the gate for this site is the saturation suite inside
    test_variant_index.c.

WHERE THE ORDERING GATE ACTUALLY LIVES
--------------------------------------
In measure_complexity()'s big-trace and narrow-window differentials, where
all five variants are tied on total_ns. The saturation suite's generator gives
each pattern a DISTINCT total, deliberately, so its count assertions never
depend on order -- which also means it cannot see an ordering bug. G20 going
red on exactly those two checks, and nowhere else, is the measurement of that
split rather than an assumption about it.

NOT MEASURED HERE
-----------------
- Whether a real 900 s capture reaches 4097 distinct flow patterns. #327's
  empirical probe over a 30-minute rehearsal trace is still open. This fix
  does not depend on the answer: "the workload we rehearsed stays under it" is
  not a bound.
- src/variant_index.c is still NOT in SERVER_SRCS, so neither site is
  reachable from the live binary yet. The wiring step is what makes them
  reachable, which is exactly when nobody would be looking at #327.
- The 144 pre-existing checks are no longer byte-identical to the parent
  tree's output, and cannot be: #358 changed the oracle's ordering, so
  matching it is a behaviour change by construction. An earlier revision of
  this directory claimed that identity; it was true only against the
  pre-#358 oracle and has been removed rather than reworded.
