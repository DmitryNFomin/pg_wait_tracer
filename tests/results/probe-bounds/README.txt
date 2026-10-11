#327 / #347 — LANDING STATE, 2026-10-11
=======================================

Branch: agent/probe-bounds, rebased onto origin/master d84619e (Phase 4's
interval-index module, #335). The park below is HISTORY: this directory was
first written on 2026-10-09 when the branch was parked at 79e4b75 against
parent f78ab4c, and the park was lifted once the demo it was racing was
cancelled. Everything in sections 1-6 was re-verified on d84619e, not
carried over on trust; section 4 says exactly which runs were repeated.

Two things changed on the way in, both recorded here:
  - section 7, NEW: the COMPLETE src/ audit. Every earlier survey grepped
    src/compute.c alone, which is why #327 was enumerated three times and a
    seventh site was still found by accident in a different file. Section 7
    greps all of src/ and gives a verdict for every hit, negatives included.
  - section 8, NEW: what this branch does NOT close. src/variant_index.c:600
    carries the same unbounded probe and is NOT fixed here, because that
    file does not exist on origin/master — it arrives with PR #344, whose
    branch owns both the file and the fix.


1. WHAT IS DONE — all six reachable sites, all by GROWING
---------------------------------------------------------
The rule was grow where cheap -> refuse the whole result where growth is
impractical -> never drop a key. Growth turned out cheap at all six, so NO
site introduces a saturation refusal and no key is ever dropped.

  site (pre-fix line)        key            old cap  decision   state
  -------------------------  -------------  -------  ---------  -----------
  :1580 top sessions raw     pid               1024  GROW       done+tested
  :2517 top sessions summ.   pid               1024  GROW       done+tested
  :1707 top queries raw      query_id          2048  GROW       done+tested
  :2676 top queries summ.    query_id          2048  GROW       done+tested
  :2990 fingerprints         query_id          2048  GROW       done+tested
  :4721 variants             pattern hash      4096  GROW       done+tested

Mechanism: src/qid_index.h (NEW — the uint64-keyed twin of the existing
src/pid_index.h) plus one accum_grow() helper in compute.c for the
accumulator arrays those indexes point into. #275/#276 had already built
this plumbing and applied it to two call sites; this reuses it rather than
reinventing it.

`int failed` added to pgwt_sessions_result, pgwt_queries_result and
pgwt_fingerprint_result. Server wiring: new arms in handle_top_sessions and
handle_fingerprints; handle_top_queries JOINS the existing
`if (lcres.failed)` condition rather than adding a second arm;
pgwt_variants_result.failed already existed and server.c already handled it,
so variants needed zero server work. The --dump text report also prints
"COMPUTE FAILED" so a refusal cannot read as "no rows".

Deliberately NOT done, and why — do not "fix" these on resume:
  :637 AAS per-event (512 slots), :1460, :2344, :2373 — all event-keyed
       against a verified PG17 valid-id ceiling of ~409. Unreachable by
       construction; a fix would be unverifiable churn.
  :2915 transitions — needs 4097 ordered pairs from that same alphabet;
       the rehearsal's own tab_coverage.json shows ~50 distinct pairs.
  :3669 interference — reachable in production, NOT on the demo workload
       even with burst_load.sh (see section 5).
  :287 tag_pid_get and :164 wait_ht_find_or_insert — already bounded, but
       they DROP THE KEY. Pre-existing silent shortfalls, out of scope, and
       their pattern must not be copied.

No failure-driven summary->raw fallback was added, deliberately: raw holds
the same distinct keys over the same window behind the same ceiling, so such
a fallback converts a refusal into a hang. server.c:3596/:3777 choose
summary-vs-raw as an if/else preflight, never a retry. Keep it that way.


2. #347 — the issue names the WRONG SITE. Measured.
---------------------------------------------------
#347 names compute.c:638 (AAS per-event) and proposes guarding the key
assignment with `if (evt_ht[h].total_ns == 0)`. THAT GUARD IS A NO-OP and
the merge it describes does not happen there: the probe can only exit with
total_ns > 0 when the key already matches; a slot with total_ns == 0 carries
no time; the collector skips it; and pass 2 re-matches by event_id, not by
slot. Measured on the PARENT tree (repro347-parent.txt) with ids 0x01000001
and 0x01000201, which share home slot 1, the first zero-duration: two
distinct series, 0.000500000 and 0.000250000 AAS, both exact. So :638 is
UNCHANGED except for a comment recording this. A guard whose red case cannot
be constructed is not a fix.

#347's SHAPE is real at three other tables that used an accumulator as the
occupancy test while ALSO accumulating fields that test does not look at.
Two were measured wrong on the parent tree:

  fingerprints  a duration-0 event incremented total_transitions and
                top_trans[] but left total_ns at 0, so the slot read as free
                and the next query_id probing to it re-keyed it AND
                INHERITED those transitions.
                PARENT: ids 1 and 326 (both -> slot 1509) with 1 and 2
                transitions came back as ONE row, id 326, transitions=3,
                id 1 absent.  FIXED: two rows, 1 and 2.
  top sessions  `pid != 0` as occupancy let pid 0's DB time be inherited.
                PARENT: pid 1024 reported db_time_ms=6.000 where its own
                events totalled 1.000.  FIXED: pid 0 -> 5.000, pid 1024 ->
                1.000.
  tq_summary    a record carrying a query_id with count 0 claimed a slot
                that still read as free. NOT shown reachable from our own
                writer; fixed structurally because a summary file is a file,
                not a guarantee.

All three now use explicit `used`-byte occupancy.
Evidence: repro347-parent.txt vs repro347-fixed.txt (same binary source,
repro347.c, built against the parent and the fixed compute.c).


3. THE EMPIRICAL NUMBERS NOBODY ELSE HAS
----------------------------------------
Driven over the WHOLE window of the existing 30-minute rehearsal trace
(tests/results/demo_rehearsal/trace/current.trace — 1052 blocks, 4,307,056
transition records, 1851.0 s, captured 2026-10-02, i.e. BEFORE burst_load.sh
existed). Harness: probe327.c / probe_pids.c in this directory, built on the
Mac against the real compute.c (pgwt-server itself does not build on macOS:
st_mtim and SOCK_CLOEXEC).

  probe327-rehearsal.txt
    VARIANTS exec : num_variants = 18     total_executions = 323,930
    VARIANTS plan : num_variants = 2      total_executions = 230,918
    INTERFERENCE  : num_rows     = 3
    (all three returned in < 0.03 s; nothing hung)

  18 of 4096 = 0.4% of the variants ceiling. 3 of 30 requested interference
  rows. So variants does NOT saturate on this workload — which is why the
  commit says the table grows anyway rather than because it had to.

  probe-pids-rehearsal.txt — distinct pids, the key at :1580 / :2517
    WHOLE TRACE (1851 s)          : 100 distinct pids   (9.8% of 1024)
    900 s windows at 300 s steps  : 55, 56, 55, 55, 46
    WORST 900 s WINDOW            :  56 distinct pids   (5.5% of 1024)

  5.5% of the cap on the pre-burst workload. That is the baseline the
  burst-tier figures in section 5 should be read against, and it matches
  burst_load.sh's own "~67 before the burst".


4. TESTS AND RED-BEFORE-FIX EVIDENCE — complete
-----------------------------------------------
tests/test_probe_bounds.c (NEW, 87 checks, 0 failures), registered in
tests/unit_tests.list AND tests/Makefile TESTS. One section per site at
cap+1 distinct keys, interleaved round-robin, distinct per-key duration in
whole milliseconds so a shortfall moves a TOTAL and every expected value is
an exact double. No epsilon, retry, sleep or tolerance anywhere. The two
summary sections spread cap+1 keys across TWO records of <= cap each — the
assertion proving SUMMARY_MAX_SESSIONS/QUERIES bound one record, not the
window. pgwt_visit_summaries is a RECORD-SERVING stub here, not the sibling
tests' `return -1` stub, and every summary section asserts how many records
the visitor actually saw.

RED LOOKS LIKE A HANG, so the red evidence is non-termination, per site:

  redgreen-327.txt  (redprobe327.c built twice: against the PARENT
                     compute.c at origin/master d84619e and against this
                     branch's. REGENERATED on d84619e 2026-10-11; the
                     original run was against f78ab4c, and compute.c is
                     byte-identical between the two — `git diff f78ab4c
                     origin/master -- src/` is interval_index.{c,h} only —
                     so the figures did not move, but the file is this
                     base's own evidence rather than a stale copy.)
    parent: sessions / sessions_summary / queries / queries_summary /
            fingerprints / variants  — ALL SIX exit 124, i.e. killed after
            30 s with no result. The probe loop spins.
    fixed : all six exit 0, returning 1025 / 1025 / 2049 / 2049 / 2049 /
            4097 rows respectively.
  Reproduce: sh run_redgreen.sh <parent-bin> <fixed-bin> 30

  run_redgreen.sh NOW CHECKS WHAT IT SEES and exits 1 otherwise (it used to
  print whatever happened and exit 0 regardless). A page of "exit 127" is
  not red evidence, it is absent evidence, and absent evidence must not read
  as a clean run. Required: every parent site exactly 124, every fixed site
  exit 0 AND a RETURNED line. Its own refusal paths, each demonstrated
  2026-10-11 before being relied on:
    parent slot handed the FIXED binary  -> "NO RED: expected 124, got 0",
                                            exit 1  (the case where the
                                            thing being checked is absent
                                            rather than wrong)
    `timeout` not on PATH (exit 127)     -> "NO RED ... got 127",  exit 1
    fixed binary chmod -x (exit 126)     -> "NO GREEN ...",        exit 1
    all six red + all six green          -> exit 0

  at-cap CONTROL — run_atcap_control.sh, atcap-control.txt (NEW 2026-10-11)
  Exit 124 says "did not finish"; it does not say the probe loop is why. So
  the same driver is rebuilt against the SAME parent compute.c with the
  fixtures holding exactly CAP keys instead of CAP+1, and every site must
  RETURN:
    sessions 1024, sessions_summary 1024, queries 2048, queries_summary
    2048, fingerprints 2048, variants 4096 — all exit 0.
  cap+1 hangs and cap returns on one unchanged binary, so the cause is the
  missing fullness test and not the fixture, the stub or the driver.
  The control refuses rather than approving when it cannot see:
    the cap+1 -> cap sed silently not applying -> "CONTROL REFUSED: the
      cap+1 -> cap edit did not apply", exit 2 (otherwise it would be a
      second copy of the cap+1 run and prove nothing)
    parent sources absent / won't compile    -> "CONTROL REFUSED: the
      at-cap control did not build", exit 2
    `timeout` missing (127)                  -> "CONTROL FAILED", exit 1
    a tree that hangs at cap too (forced by  -> "CONTROL FAILED: hung at cap
      SESSION_HT_SIZE 1024 -> 512)              too", exit 1 at both
                                                sessions sites

Bypass suite (section 7 of the test), one case per way the file's own
detection could be made unreachable: a cap-sized fixture (green while
broken — pinned blind), a per-row assertion shape that cannot see a dropped
key, a summary stub that delivers NOTHING, one that delivers only PART of
the range, key 0, a dense consecutive key run, and allocation failure at all
eight new growth points where the answer must be ABSENT (failed==1,
rows==NULL) — plus an unknown injection point so a no-op injection cannot be
what made those pass. main() REFUSES outright if any fixture is trimmed back
under its cap.

Also run, all green, on this branch:
  test_probe_bounds 87/0, test_pid_cap 40/0, test_window_clip 51/0,
  test_agg_raw_crosscheck 173/0, test_idle_summary 269/0
  — those are every C unit that touches the five rewritten functions.
  All five ALSO green under -fsanitize=address,undefined (macOS clang),
  0 sanitizer reports.

GATES — the park-era state is superseded; see the PR for the numbers.
  The 2026-10-09 park recorded `make check` passed with stamp
  2cac1503…, `make box-check` NOT RUN (both boxes were then holding
  run_all.sh plus queued CI jobs — pgwt-gate at load 20.78 with 6 pids on
  the lock, pgwt-gate-2 at 4.11 with 6), no Linux build, no x86 C-unit run,
  and test_variants_p95 never executed because it needs GNU ld's
  `-Wl,--wrap`. That was the honest gap in the parked state.
  The landing run closes it on a gate box. Gate artefacts are not committed
  (tests/results/box-check-*.log and tests/results/ui_live/ are
  .gitignore'd), so the `make check` stamp, the run_all summary and the
  live-UI verdict live in the PR body, which is where a reviewer can match
  them against the final commit. Nothing in THIS file is a gate claim.

Re-verified on d84619e 2026-10-11 (not carried over from the park):
  - test_probe_bounds           87 checks, 0 failures, macOS clang
  - redgreen-327.txt            all six parent sites 124, all six fixed 0
  - atcap-control.txt           all six at-cap parent sites 0 (NEW control)
  - repro347 parent vs fixed    byte-identical to the committed .txt pair
Still macOS-only in this directory; the x86 Linux build, the full C-unit
run and test_variants_p95 are the box-check's job and are reported there.

ONE THING STILL NOT COVERED BY A TEST, and it is deliberate: there is no
end-to-end check that a `compute_failed` response renders sensibly in the
UI. web/static/lib/transport.js:79 turns any `data.error` into a
TransportError generically, so no web/ change was made and none is believed
necessary — but that is reasoning, not a screenshot.


5. DOES burst_load.sh CHANGE #327's REACHABILITY?
-------------------------------------------------
Read in full over ssh from /root/pgwt-paint-20261008/tests/burst_load.sh on
pgwt-gate (root@2.28.45.47, 20815 bytes, mtime Oct 8 09:30). It is not in
the repo. ANSWER: it does NOT reverse the park — but it changes the margin
by an order of magnitude at the sessions sites, and there is one
configuration worth watching.

INTERFERENCE (:3669, 4096 pid-PAIR slots) — NOT demo-reachable. Confirmed,
with a correction to the earlier "~20 backends" figure. The burst tier's
client counts are explicitly scale-independent, and the four bursts are
SEQUENTIAL except the last two:
    b_wal  -c 6  -T 8*scale   then a 32*scale pause
    b_lm   -c 4  -T 1*scale   then a 37*scale pause
    b_ior  -c 6  and  b_iow -c 3   run CONCURRENTLY  (the only overlap)
so peak simultaneous burst backends is 9, not 19. Add base
`pgbench -c 4 --rate=25` and ~14 live_loop_workload.py sessions: peak ~27
concurrent backends. 4097 distinct co-waiting pairs needs ~92 MUTUALLY
overlapping backends (92*91/2 = 4186). ~27 gives at most 351. And the pair
count cannot be driven by churn the way the session count is, because
pairs only form between backends whose waits overlap IN TIME on the same
event, and the burst generations are time-disjoint by construction — each
burst is followed by a pause 4 to 37 times its own duration. Measured
corroboration: interference returned num_rows = 3 over the whole 1851 s
rehearsal window. So ~13x under the threshold rather than 20x, still a
comfortable no.

SESSIONS (:1580 raw and :2517 summary, 1024 pid slots) — THIS is what burst
moves, and it is the number to watch. burst_load.sh's own header records,
from a 2026-10-08 measurement on the gate box:
    "537 distinct sessions in a 900 s trace against ~67 before the burst"
537 is 52% of the 1024 cap. My own pre-burst measurement of the rehearsal
trace is 56 in the worst 900 s window (5.5%), consistent with that "~67".
So the burst tier took the sessions sites from ~5% of the cap to a MEASURED
52% in one change — a 9.6x move, on the exact 900 s window that is the UI's
boot default, on the path with no reject_overload upstream.

That 537 was the churny rig. The BURST_SCALE=6 duration-scaling change in
this same file exists specifically to cut backend CREATION 6x at the same
tx/s. DERIVED (not measured): (537-67)/6 + 67 ~= 145 distinct sessions per
900 s at scale 6, i.e. ~14% of the cap. Treat 145 as an estimate; 537 and 56
are the two real measurements.

THE ONE THING TO WATCH, and it is a derivation off the script's own code,
not a measurement: BURST_SCALE is derived from the walk duration by
`burst_scale_for`, with BASE_CYCLE_S = 8+32+1+37+6+54 = 138. So
    walk 138-275 s  -> scale 1   (the churny regime that measured 537)
    walk 276-413 s  -> scale 2
    walk >= 828 s   -> scale 6   (the 1800 s rehearsal / demo walk)
The demo and rehearsal walks are 1800 s, so they get scale 6 and the
~14% estimate. A SHORT walk (138-275 s) automatically selects scale 1 and
the 537-session regime. 537 is not 1025, so still no hang either way — but
the headroom at :2517 is 1.9x in that regime, not the 18x the pre-burst
trace shows. Anything that adds churn on top of scale 1 (a wider window, a
reconnect storm, a pooler-less failover, a second burst tier) crosses 1024,
and crossing it HANGS every tab at once rather than returning a short
number.

Net: park stands. If the demo ever runs a sub-276 s walk, or if anything
raises per-window session churn again, re-read this section first — the
sessions sites are the half of #327 that burst_load.sh made plausible, and
they are also the two that are fully fixed and tested on this branch.


6. FILES ON THE BRANCH (commit 79e4b75, 7 files, +1698/-95)
-----------------------------------------------------------
  src/qid_index.h           NEW  uint64-keyed grow-only index
  src/compute.c                  six sites, accum_grow(), #347 comment at
                                 :638, dead hash64() and the six
                                 *_HT_SIZE/_MASK constants removed
  src/compute.h                  `int failed` x3
  src/server.c                   ~50 lines: two new refusal arms, one
                                 joined condition, two --dump notices
  tests/test_probe_bounds.c NEW  87 checks
  tests/Makefile                 build rule + TESTS
  tests/unit_tests.list          entry + comment block

Evidence in THIS directory:
  probe327.c, probe327-rehearsal.txt          variants/interference numbers
  probe_pids.c, probe-pids-rehearsal.txt      distinct pids per window
  repro347.c, repro347-parent.txt,
    repro347-fixed.txt                        the #347 before/after
  redprobe327.c, run_redgreen.sh,
    redgreen-327.txt                          the six-site hang / no-hang
  run_atcap_control.sh, atcap-control.txt     the at-cap control: cap+1
                                              hangs, cap returns, so the
                                              CAP is why
  bypass-suite.txt                            7 ways the two evidence
                                              scripts' own detection could
                                              be made unreachable, each run
                                              and each refusing
  spawn_stub.c                                macOS link stub (pipe2)


7. THE COMPLETE src/ AUDIT — every open-addressing table, 2026-10-11
--------------------------------------------------------------------
Why this section exists: #327 was enumerated THREE times (5 sites, then 11,
then 13) and a seventh was still found by accident, because every pass ran
`grep -n "+ 1) & \w*_MASK\|+ 1) % SESSION_HT_SIZE" src/compute.c` — one
file, and one spelling of the probe step. Two things that grep cannot see:

  (a) other files. src/block_agg.c, src/server.c, src/summary_writer.c,
      src/query_text.c, src/pgss_resolver.c, src/pid_index.h,
      src/triple_map.h and src/variant_index.c all contain probe loops.
  (b) other spellings of the step. compute.c:180 and :208 advance with
      `(slot + i) & MASK`, and block_agg.c/server.c use `for (;;)` with
      `h = (h + 1) & mask` on a VARIABLE mask, so even
      `grep -rn "+ 1) & \w*_MASK" src/` still misses some.

The enumeration below is keyed on the HOME-SLOT computation instead, which
every open-addressing table must have and which cannot be written without a
`& mask` or a `% size`:

  grep -rn ") *& *\(mask\|[A-Z_]*MASK\|(.*- 1)\)\|) *% *[A-Z_]*SIZE" src/*.c src/*.h
  grep -rn "+ *1) *[&%]" src/            # the step, both spellings
  grep -rn "while (.*used\|while (.*count\|while (.*pid" src/

30 home slots across 10 files; every one has a verdict below.
  BOUNDED-COUNTED   the probe is `for (i = 0; i < size; i++)` and cannot
                    spin; the table-full case is an explicit return.
  BOUNDED-INVARIANT an unbounded loop body entered only on a table held
                    below a load factor, with growth failure refusing
                    BEFORE the probe.
  BOUNDED-RELATION  an unbounded loop body entered only on a table that a
                    SEPARATE constant keeps under-full.
  GROWABLE          grows, and refuses the whole result on allocation
                    failure.
  UNBOUNDED         no fullness test: saturation spins forever.

src/compute.c
  :180 event_ht_find_or_insert   EVENT_HT 2048, event    BOUNDED-COUNTED
       Returns -1 when full and the caller then DROPS the key. Pre-existing
       silent shortfall; unreachable in practice (see the alphabet note).
  :208 wait_ht_find_or_insert    WAIT_HT 256, event      BOUNDED-COUNTED
       Same shape, same key-dropping. 256 slots is the tightest table in
       the tree; it is per-session and event-keyed, so the alphabet note
       applies. Not touched here: changing it is a behaviour change (short
       answer -> complete answer), not a hang fix.
  :324 tag_pid_get               TAG_HT 4096, pid        BOUNDED-COUNTED
       Returns NULL when full; untagged records stay foreground.
  :706 AAS per-event             AAS_EVT_HT 512, event   UNBOUNDED, unreachable
       UNFIXED DELIBERATELY. 512 slots against a PG17 valid-id ceiling of
       ~409 (~281 in-core maxima at pg_wait_tracer.h:250-281 plus <=128
       custom). Headroom 1.25x — the thinnest of any unreachable site, so
       it is the first to revisit when a PG release adds wait events. Also
       #347's named site, where the proposed guard was measured to be a
       no-op (section 2).
  :1529 top events (raw)         EVENT_HT 2048, event    UNBOUNDED, unreachable
  :2481 top events (summaries)   EVENT_HT 2048, event    UNBOUNDED, unreachable
  :2510 top events (summaries)   EVENT_HT 2048, event    UNBOUNDED, unreachable
       All three key on the same ~409-value alphabet; 2049 distinct wait
       event ids cannot be produced by a PostgreSQL we support.
  :3129 transitions              TRANS_HT 4096, (from,to) UNBOUNDED, unreachable
       4097 ordered pairs needs >=65 distinct ids all cross-transitioning;
       the rehearsal's own tab_coverage.json shows ~50 distinct non-idle
       pairs over 1851 s.
  :3919 interference             PAIR_HT 4096, pid pair  UNBOUNDED, REACHABLE
       *** THE ONE REAL SITE THIS BRANCH DOES NOT FIX. *** Measured 3 rows
       over the whole 1851 s rehearsal window, and not reachable on the
       demo workload even with burst_load.sh (section 5). But pairs
       ACCUMULATE ACROSS THE WINDOW — any two pids whose waits ever
       overlapped on the same event inside the range occupy a slot — so a
       production window with real lock contention reaches 4097 at ~92
       mutually overlapping backends on one event (92*91/2 = 4186), or
       sooner through churn. #327's own triage already labelled it
       "reachable in production, not on stage" and deliberately scoped it
       out of the six-site fix; that scoping decision is the owner's, so it
       is reported rather than silently widened. The same free parts would
       fix it: pgwt_triple_map_slot(pa, pb, 0) plus the accum_grow() this
       branch already adds. Follow-up issue requested in the PR body.
  :4556 lifecycle slot (rehash)  growable, query_id      GROWABLE (#275)
  :4576 lifecycle slot (find)    growable, query_id      GROWABLE (#275)
  :4593 lifecycle slot (insert)  growable, query_id      GROWABLE (#275)
  the six #327 sites                                     FIXED, this branch
       top sessions raw + summary (pid, was 1024 each), top queries raw +
       summary and fingerprints (query_id, was 2048 each), variants
       (pattern hash, was 4096) — all now pid_index.h / qid_index.h.

src/qid_index.h  :83 find, :97 put_nogrow                GROWABLE (NEW here)
src/pid_index.h  :75 find, :89 put_nogrow                GROWABLE (#275)
src/triple_map.h :84, :141, :152                          GROWABLE (#276)
       All three grow at 70% load and return -1/NULL on allocation failure,
       which callers must propagate as an ABSENT answer. The put_nogrow
       loops are unbounded by construction and safe only because the caller
       guarantees spare capacity first — that is the invariant to preserve.

src/block_agg.c
  :184 pair_slot   (from,to)   for(;;)                   BOUNDED-INVARIANT
  :198 node_slot   event_id    for(;;)                   BOUNDED-INVARIANT
       pair_add/node_add grow at 70% BEFORE probing and return
       PGWT_BAGG_REFUSED_NOMEM if growth fails, so the probe is never
       entered on a full table; the rehash probes a table at <=35%. Correct,
       but the loop itself has no fullness test, so the invariant is
       load-bearing: a second insert path must grow first.

src/server.c
  :588 qt_map_lookup_entry    qt_capacity               BOUNDED-COUNTED
  :639 qt_map_insert          qt_capacity               BOUNDED-COUNTED
  :850 bm_map_insert          bm_capacity               BOUNDED-COUNTED
  :864 bm_map_lookup          bm_capacity               BOUNDED-COUNTED
  :4209 timeline pid index (insert)  pid_idx_ht[1024]   BOUNDED-RELATION
  :4226 timeline pid index (lookup)  pid_idx_ht[1024]   BOUNDED-RELATION
       Both are unbounded `while (pid_idx_ht[slot] >= 0)` loops over a
       fixed 1024-slot table, and both are safe ONLY because num_pids is
       capped at TIMELINE_MAX_PIDS = 256 by server.c:4133 — 25% load, so an
       empty slot is always reachable and even a lookup for an ABSENT pid
       terminates. That is a relation between two constants ~100 lines
       apart, with the table size written as the bare literal 1023 in three
       places and no assertion tying them. Raising TIMELINE_MAX_PIDS to
       1024 would hang the timeline tab with no other code change and no
       warning. Latent, not a defect today: flagged in the PR body as a
       one-line `_Static_assert` opportunity, not taken here because a new
       compile-time gate needs its own bypass-suite round.
  :4952 bagg_slot   (trace, block)  for(;;)             BOUNDED-INVARIANT
       Same 70%-grow discipline as block_agg.c; bagg_cache_put declines to
       cache (and counts it) when growth fails, so a full table is never
       probed.

src/summary_writer.c
  :110 find_or_insert_event    SUMMARY_MAX_EVENTS       BOUNDED-COUNTED
  :131 find_or_insert_session  SUMMARY_MAX_SESSIONS     BOUNDED-COUNTED
  :151 find_or_insert_query    SUMMARY_MAX_QUERIES      BOUNDED-COUNTED
       Table-full returns NULL and the caller skips. These are the PER-
       SECOND accumulators, and their caps are exactly what #327's two
       summary sites proved do NOT bound a window: the writer bounds one
       record, the reader then accumulates across records.
  :359 qattr_find_or_insert    SUMMARY_QATTR_SLOTS      BOUNDED-COUNTED
       Full -> the record keeps its emission-time id and
       summary_query_attr_table_full_total counts it. Never silent.
  :395 qattr_slot_free         backward-shift deletion  BOUNDED-COUNTED
       One lap of SLOTS-1 steps, with the "a table with no empty slot has
       no natural stop" case reasoned out in its own comment.

src/query_text.c
  :65  quality_check_or_insert QT_QUALITY_SIZE          BOUNDED-COUNTED
       Full -> deliberate clock-slot eviction, documented as such.
  :93  table_check_or_insert   QT_HT_SIZE 4096          BOUNDED-COUNTED (ret 2)
  :118 table_contains          QT_HT_SIZE               BOUNDED-COUNTED
  :147 text_index_source       QT_TEXT_INDEX_SIZE       BOUNDED-COUNTED
  :178 text_index_insert       QT_TEXT_INDEX_SIZE       BOUNDED-COUNTED (ret -1)
  :511 synth queue head        ring buffer, NOT a probe n/a

src/pgss_resolver.c
  :654 resolver enqueue        PGWT_PGSS_QUEUE_SIZE     BOUNDED-COUNTED
       Full -> reclaim a DONE/COOLDOWN slot, else count
       sampled_text_error_total. :687 is this loop's step.

NO open-addressing table at all — verified, not assumed:
  src/interval_index.c (#335, NEW on this base) — growable arrays plus a
    binary search (lower_bound_end); its only loops are the doubling
    reserve and the chunk walk. No hash table, so nothing to bound.
  src/exec_index.c — delegates to pgwt_pid_index_find; adds no table.
  src/bpf/pg_wait_tracer.bpf.c — kernel BPF maps, and the verifier rejects
    unbounded loops, so this defect class cannot exist there.
  anomaly.c, backend.c, backend_meta.c, backend_status_layout.c, cJSON.c,
  cmdline.c, control.c, daemon.c, discovery.c, effective_cores.c,
  escalation.c, escalation_budget.c, event_reader.c, event_stream.c,
  event_writer.c, exact_probe.c, idle_rule.c, map_reader.c, output.c,
  perf_event.c, pg_wait_tracer.c, provider_coop.c, provider_full.c,
  replay.c, sampler.c, snapshot.c, spawn.c, summary_reader.c,
  synthetic_query.c, wait_event.c — no home-slot computation, therefore no
  probe loop. The `*_MASK` hits in sampler.c, exact_probe.c and
  idle_rule.h are BIT masks (capability flags, the idle pacing set), not
  table masks; wait_event.c:1098's `idx++` is a parse cursor;
  server.c:4830's `rnd % seen` is reservoir sampling.

THE ALPHABET ASSUMPTION, stated once because five verdicts rest on it:
`ev->old_event` is PostgreSQL's raw wait_event_info, (class << 24) | index
(pg_wait_tracer.h:324). The number of DISTINCT values a supported PG can
emit is ~409. Two consequences worth writing down:
  - it is an assumption about PostgreSQL, not an invariant of this code. A
    release that adds wait events, or an extension registering many named
    LWLock tranches, erodes it — and AAS_EVT_HT_SIZE 512 has only 1.25x
    headroom. If that is ever crossed, the AAS tab (the main tab) hangs
    rather than returning a short list.
  - a corrupt or hand-forged trace file can carry arbitrary 32-bit values
    in that field. Trace files are trusted input today (the layout guard
    covers the daemon's own writes); if that ever stops being true, these
    five sites are the reason it matters.


8. WHAT THIS BRANCH DOES NOT CLOSE
----------------------------------
src/variant_index.c:600, in vi_merge_rows — the same unbounded 4096-slot
probe, copied from the pre-fix compute.c:4631. NOT fixed here, and not
fixable here: that file does not exist on origin/master
(`git show origin/master:src/variant_index.c` -> fatal: path does not
exist). It arrives with PR #344, the variant-index module, whose branch owns
both the file and the fix. Creating it here would mean two open PRs each
adding the same new file.
So #327's SEVENTH site rides with #344, under the same rule: grow where
cheap, refuse the whole result where growth is impractical, never drop a
key. It is unreachable in production today only because variant_index.c is
absent from origin/master's SERVER_SRCS, and it becomes reachable the moment
the wiring step links it — precisely when nobody will be re-reading #327.

src/compute.c:3919, interference — see section 7. Unbounded and reachable in
production; deliberately outside the six-site scope by #327's own triage;
reported for a follow-up issue rather than folded in silently.
