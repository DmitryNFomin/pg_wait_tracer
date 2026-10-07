#294 residual probe — arms A/B/C were NOT RUN. Evidence of why, plus the
red/green evidence for the one output change that was made.

Date: 2026-10-07, ~19:40–20:10 UTC. Branch agent/sampler-residual-probe.
Daemon built from agent/sampler-cmd-gate-order @ d7800619e4017a923ed0543837350c02e8a5881c
(the read-order fix tree; see .FIXTREE_SHA on the box at /root/probe294/fixtree).
Comparator and generator built from THIS branch, on gate-2, so one binary
served every arm.

── WHY THE ARMS DID NOT RUN ────────────────────────────────────────────────
gate2-occupied.txt: pgwt-gate-2 (root@142.132.191.18) was not idle. At
2026-10-07T19:56:47Z another agent's run held /tmp/pgwt-box-check.lock
(`flock -n ... sleep 9000`, started 19:52:45, i.e. until ~22:22) and was
running `pgbench -c 4 -T 7200 --rate=25` plus a pgwt_wl_* live workload loop:
22 foreign client backends (sleeper, row_holder/row_waiter, lockmgr_0..7,
io_reader/io_writer, adv_holder, reporter).

That is fatal to this experiment specifically, not merely noisy:
  - arm C's independent variable IS the number of runnable backends vs cores.
    With 22 foreign backends on a 4-vCPU box, "CLIENTS=3, unsaturated" is not
    the condition being tested.
  - the daemon traces the whole postmaster, so the foreign backends' waits
    (Lock, ClientRead, IO) enter both tiers and move every share.
  - arm294.sh asserts the observed client-backend count equals CLIENTS, which
    cannot hold while they exist. Relaxing that assertion to get a number
    would be hardening a test against runner noise, so it was not relaxed.
gate-1 was excluded by the task contract (two branches need it for decisive
box-checks) and VM creation was likewise excluded, so no arm was measured.
Nothing in this directory contains a sampled-vs-exact ratio: there is none.

arm294.sh / run_arms.sh are the ready harness, unrun. They are a fork of
tests/results/sampler-294/diag294.sh with four additions, all measurement:
  - RT_PRIO launches the daemon under `chrt -f N` and then VERIFIES the policy
    from /proc/<pid>/stat (field 41 = policy, 40 = rt_priority). A chrt that is
    absent or exits 126/127 would otherwise run arm B at normal priority and
    read as "hypothesis disproved" — the most dangerous false negative here.
    The baseline arms assert policy==0 for the same reason in reverse.
  - the observed client-backend count is asserted against CLIENTS (above).
  - cross_validate runs with --show-idle; its exit 4 (refusing because the
    pacing mask is empty) aborts the run rather than being read as zero.
  - an EXIT trap, because the first smoke run orphaned a daemon and a pgbench
    on a shared box.
run_arms.sh interleaves A,B,C ×3 rather than blocking AAA/BBB/CCC: over 12
minutes the box's own state drifts (page cache, checkpoint phase, autovacuum),
and a blocked order aliases arm with time.

── THE OUTPUT CHANGE, RED AND GREEN ────────────────────────────────────────
cross_validate gained --show-idle (and --raw-ns, cherry-picked from c15b85e on
agent/sampler-cpu-bias-diag). tests/test_cross_validate_idle.sh, now in
tests/unit_tests.list, gates it. All four logs below were produced on gate-2.

idle-test-green.txt                     39 passed, 0 failed  (this branch)
idle-test-red-vs-parent.txt             13 passed, 27 failed (origin/master's
    tests/cross_validate.c, built from the same objects: no --show-idle, no
    --raw-ns, so every idle assertion and every bypass case goes red)
idle-test-red-stale-generator.txt       exit 1 — $GEN built from
    origin/master's gen_test_traces.c has no --pg-version, so bypass case B1
    (the empty pacing mask) is unreachable; the test REFUSES rather than
    quietly running 38 of 39 cases. A source-file grep was the first version of
    that pre-flight and it approved this exact binary, because the source next
    to it was fresh; it is now a behavioural probe of the binary.
idle-test-red-missing-comparator.txt    exit 1 — a missing/non-executable
    comparator (the 126/127 case) fails the test, never skips it.

Two false negatives found in the test itself while writing it, both now fixed
and both visible in the green log's wording:
  - the per-row greps were unscoped, so they matched the SHARE table above;
    "SpinDelay is not in the idle table" went red against a correct tool.
    Scoped to the idle section via sect().
  - $(( "" + "" )) aborted the whole script at the conservation check when a
    field was absent, so the entire bypass suite below it never executed
    against the parent comparator. A missing field now reports a failure.
