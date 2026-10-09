#327 / #347 — PARKED BRANCH STATE, 2026-10-09
=============================================

Branch: agent/probe-bounds   HEAD: 79e4b75 (local only, NEVER pushed)
Parked by coordinator decision: the demo-critical paint work needs
src/server.c and src/compute.c, and #327 is not on the demo path. Nothing
here is half-done; see "WHAT IS DONE" below for the site-by-site state.

The work is COMPLETE and GREEN on this branch. It was parked for priority,
not for defects. Resuming it is a rebase, a `make check`, and a box-check.


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

  redgreen-327.txt  (redprobe327.c built twice: against the parent
                     compute.c at f78ab4c and against this branch's)
    parent: sessions / sessions_summary / queries / queries_summary /
            fingerprints / variants  — ALL SIX exit 124, i.e. killed after
            30 s with no result. The probe loop spins.
    fixed : all six exit 0, returning 1025 / 1025 / 2049 / 2049 / 2049 /
            4097 rows respectively.
  Reproduce: sh run_redgreen.sh <parent-bin> <fixed-bin> 30

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

GATES:
  make check  PASSED (full), exit 0, stamp 2cac15036fcb7fc61a91d47d392e05fe1669cd00
  The stamp does NOT equal `scripts/tree-hash.sh` now (b6d4927…) because
  tree-hash covers untracked files and THIS directory was written after the
  check finished. No TRACKED file changed after the check: `git status
  --short` shows only `?? tests/results/probe-bounds/`. Re-run `make check`
  on resume rather than trusting the stamp.
  make box-check  NOT RUN — the coordinator's park instruction forbade it.
  One attempt was started before the instruction arrived and got as far as
  "waiting for /tmp/pgwt-box-check.lock" on pgwt-gate-2, then was
  terminated. NOTHING ran on either box; no live pass is claimed. The
  attempt's hetzner-sweep deleted 0 VMs and created none.
  Box state at the time, for the record (both busy, measured 16:47 UTC):
    pgwt-gate   load 20.78, 6 pids on the lock: 1 run_all.sh at 16 min,
                a CI job waiting 1h45m, another agent waiting 49 min
    pgwt-gate-2 load  4.11, 6 pids on the lock: 1 run_all.sh at 33 min,
                an agent waiting 33 min, a CI job waiting 14 min
  So neither box was free; per the brief this is reported, not claimed.

NOT TESTED, honestly:
  - No Linux build and no C unit run on x86 — everything above is macOS
    clang. src/server.c cannot even be compiled on macOS (st_mtim,
    SOCK_CLOEXEC); it was syntax-checked with
    `-Dst_mtim=st_mtimespec -DSOCK_CLOEXEC=0 -fsyntax-only`, clean, which is
    NOT the same as a real build.
  - test_variants_p95 was NOT run: it links with `-Wl,--wrap=realloc`,
    which is GNU ld only. It is the one affected test with no local result.
    Run it first on resume.
  - No end-to-end check that the new `compute_failed` responses render
    sensibly in the UI. web/static/lib/transport.js:79 turns any
    `data.error` into a TransportError generically, so no web/ change was
    made and none is believed necessary — but that is reasoning, not a
    screenshot.


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

Evidence in THIS directory (all untracked, by the tests/results convention):
  probe327.c, probe327-rehearsal.txt          variants/interference numbers
  probe_pids.c, probe-pids-rehearsal.txt      distinct pids per window
  repro347.c, repro347-parent.txt,
    repro347-fixed.txt                        the #347 before/after
  redprobe327.c, run_redgreen.sh,
    redgreen-327.txt                          the six-site hang / no-hang
  spawn_stub.c                                macOS link stub (pipe2)
