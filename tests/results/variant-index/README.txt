Evidence for #327's SEVENTH probe site: src/variant_index.c's merge table
=========================================================================

Branch: agent/variant-sequence-index   PR #344
Fix commit: "Bound the variant index's merge probe loop by growing it
(#327, 7th site)" -- the commit immediately before this one.
Parent (pre-fix) module: the same file at the commit "Mutation harness for
the step-sequence index, plus the two fixtures it found missing".

THE DEFECT. pgwt_variant_index_query() merged executions into a FIXED
4096-slot array (PGWT_VARIANT_INDEX_HT_SIZE) probed with no fullness test:

    while (ht[slot].used && ht[slot].hash != e->hash)
        slot = (slot + 1) & (PGWT_VARIANT_INDEX_HT_SIZE - 1);

copied verbatim from pgwt_compute_variants (src/compute.c:4737). With 4096
distinct pattern hashes already in the table, the 4097th never terminates.
pgwt-server is single-threaded, so the request never answers and every tab
stops at once.

THE FIX. The table GROWS (doubling + rehash) and NEVER drops a key; a growth
that cannot allocate refuses the WHOLE result through vi_query_refuse(),
which poisons both counts AND sets res.failed / sequences_indexed = 0,
because -1 is truthy in C and a poison value alone stops no caller.

EVIDENCE TABLE
--------------

claim                                    value        source
---------------------------------------- ------------ ----------------------------
unit checks, post-fix, this Mac          170 / 0 fail unit-test-mac.txt
  of which pre-existing                  144          identical-144-checks.txt
  new: saturation-suite checks            24          unit-test-mac.txt
  new: non-vacuity ledger rows             2          unit-test-mac.txt
bit-exact differential quadruples         42,266       unit-test-mac.txt (ledger)
differential disagreements                0            unit-test-mac.txt (ledger)
pre-fix: new test vs parent module, macOS exit 124     prefix-hang-mac.txt
  PASS lines printed before the hang      138          prefix-hang-mac.txt
  last line printed                       "4097 distinct patterns: index
                                           built over 8 blocks, 16388 records"
pre-fix: probe vs parent module, Linux    exit 124     prefix-hang-linux.txt
post-fix: same probe, Linux, ASan+LSan    exit 0,      prefix-hang-linux.txt
                                          nv=4097
mutations injected / not detected         44 / 0       mutations.txt
  of which new for this site               3 (G17-G19) mutations.txt
windows answered above 4096 variants       1 (ledger)  unit-test-mac.txt
growth-allocation refusals observed        1 (ledger)  unit-test-mac.txt

WHAT EACH FILE IS
-----------------

unit-test-mac.txt
    tests/test_variant_index.c built against the fixed module and run on this
    Mac (cc -O1, the same command tests/variant_index_mutations.py uses).
    170 checks, 0 failed. The "--- #327 seventh site ---" block is the new
    suite; the "non-vacuity ledger" block below it includes the two new rows
    that FAIL the test if no window ever exceeded 4096 variants and if no
    growth refusal was ever observed.

prefix-hang-mac.txt
    THE RED. The SAME test file, built against the PARENT module, run under
    `timeout 180 stdbuf -oL`. Exit 124 -- it did not terminate. The 4095 and
    4096 cases pass pre-fix (a table that still has a free slot always stops);
    the output ends at "4097 distinct patterns: index built over 8 blocks",
    i.e. inside the first query that cannot. The pre-fix symptom is a HANG,
    not a failed assertion, which is why this is recorded as a timeout rather
    than as FAIL lines.

prefix-hang-linux.txt + probe327.c
    The same thing on the platform that actually runs pgwt-server: Ubuntu
    24.04, kernel 6.8, gcc 13.3, x86_64 (the gate box). probe327.c is a
    standalone 90-line probe -- 4097 distinct patterns, one query -- built
    against the parent module (exit 124 after 120 s) and against the fixed
    module under ASan + LeakSanitizer (exit 0, num_variants=4097, no
    sanitizer report). The second run also exercises the growth-refusal path
    (PGWT_TEST_ALLOC_FAIL=variant_index_ht_grow), so the refusal is leak- and
    bounds-checked too. probe327.c is evidence, not a gate: it is not in
    tests/unit_tests.list, because the gate for this site is the saturation
    suite inside test_variant_index.c.

mutations.txt + mutations/m01..m44.txt
    tests/variant_index_mutations.py: 44 silent-wrong risks injected into a
    copy of src/variant_index.c one at a time, each requiring the unit test
    to go RED. 44 RED, 0 undetected, pristine baseline green at 170 checks.
    The branch's previous run was 41 RED / 0 undetected; the three added here
    are the ones that can be WRONG WHILE STILL TERMINATING at this site:

      G17 the rehash drops half the keys      -> num_variants 2049, not 4097
      G18 the growth hook is renamed          -> the refusal case goes green
      G19 the rehash loses the p95 sample     -> 4096 variants with p95 = 0

    Most other ways to break this site HANG, and this harness scores a hung
    mutant as a FAILURE, so a hanging mutation could not be listed honestly.

WHY THE GROWTH TRIGGER HAS TWO REGIMES
--------------------------------------
At or below the oracle's own 4096 the table grows only when genuinely FULL,
so every input pgwt_compute_variants can answer at all keeps the oracle's
capacity, slot layout and therefore slot ORDER -- load-bearing, because the
collection loop walks slots and qsort() breaks equal-total_ns ties by input
order. Checked rather than argued: with the fix applied, the 144 pre-existing
checks print output byte-identical to the parent tree's -- see
identical-144-checks.txt (same test file, the two modules, `diff` empty with
the one REPORTED timing line removed).
Above 4096 there is no oracle to be exact with -- it spins -- so the table is
kept at most 3/4 full, because filling a linear-probe table to 100% costs
O(cap^2) probes, tens of seconds at cap 131072, which would be a different
freeze rather than a fix.

NOT MEASURED HERE
-----------------
- Whether a real 900 s capture reaches 4097 distinct flow patterns. #327's
  open empirical probe over a 30-minute rehearsal trace is still open; this
  fix does not depend on the answer, because "the workload we rehearsed stays
  under it" is not a bound.
- src/variant_index.c is still NOT in SERVER_SRCS, so this site is not
  reachable from the live binary yet. The wiring step is what makes it
  reachable, which is exactly when nobody would be looking at #327.
