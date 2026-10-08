#294 residual probe — arms A/B/C were NOT RUN. Evidence of why, plus the
red/green evidence for the one output change that was made.

Date: 2026-10-07, ~19:40–20:10 UTC. Branch agent/sampler-residual-probe.
Daemon built from agent/sampler-cmd-gate-order @ d7800619e4017a923ed0543837350c02e8a5881c
(the read-order fix tree; see .FIXTREE_SHA on the box at /root/probe294/fixtree).
Comparator and generator built from THIS branch, on gate-2, so one binary
served every arm.

── OUTCOME ─────────────────────────────────────────────────────────────────
The arms DID run, on a throwaway Hetzner VM (see ARMS_SUMMARY.txt /
ARMS_DELTAS.txt / arms/). The section below records the FIRST attempt, on
gate-2, which was refused because the box was occupied; it is kept because it
is why an ephemeral VM was authorised.

── WHY THE FIRST ATTEMPT (gate-2) DID NOT RUN ──────────────────────────────
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
The gate-2 attempt produced no ratio. The VM run did; see ARMS_SUMMARY.txt.

arm294.sh / run_arms.sh are the harness that was then run on the VM. They are a fork of
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

════════════════════════════════════════════════════════════════════════════
THE ARMS, AS RUN — throwaway VM, 2026-10-07 ~20:57–21:13 UTC
════════════════════════════════════════════════════════════════════════════
Host: Hetzner cx33 id=169272199 ip=2.28.138.111, created from the
pgwt=gate-snapshot image, deleted by id at the end of the run.
nproc=4 (AMD EPYC-Rome), 7 GiB RAM, kernel 6.8.0-139-generic, PG 18 port 5418,
pgbench scale 10 (1,000,000 accounts). Lock free, 1 foreign client backend
(my own counting psql, which arm294.sh excludes via pid <> pg_backend_pid()).
4 vCPU matters: arm C's CLIENTS=3 really is fewer runnable backends than cores.

Daemon: agent/sampler-cmd-gate-order @ d7800619e4017a923ed0543837350c02e8a5881c
        (the read-order fix tree — the bias it closes must already be gone).
Comparator + generator: this branch, so ONE cross_validate binary served all
nine runs. 10 Hz, 60 s escalation window, interleaved A,B,C x3.
All nine exited rc=0; run_arms.sh waited for loadavg1 < 0.35 first and records
PRE_LOADAVG/POST_LOADAVG beside every arm (arms/run_arms.log).

  arm                                  n  ratio median   delta_pp median   spread
  A baseline    8 clients SCHED_OTHER  3        0.7990            -6.52   1.30 pp
  B real-time   8 clients SCHED_FIFO10 3        0.8678            -3.90   2.96 pp
  C unsaturated 3 clients SCHED_OTHER  3        0.7605            -4.69   4.30 pp

  per-run delta_pp   A: -7.61 -6.31 -6.52
                     B: -4.39 -1.43 -3.90
                     C: -4.69 -2.57 -6.88

ratio    = RAW_CPU_SAMPLED_NS / RAW_CPU_EXACT_NS
delta_pp = (sampled CPU share) - (exact CPU share), signed; negative = the
           sampled tier reads LESS CPU, which is the #294 sign.

READING (see the report for the full argument):
 * Arm A reproduced the established bias IN THIS SESSION, on this harness, on
   this box: -6.52 pp median, ratio 0.799, against the issue's -6.0 pp / 0.77.
   No cross-session comparison is relied on.
 * Arm B PARTIALLY collapses it, and the separation is complete: every B run
   (-4.39 .. -1.43) read a smaller gap than every A run (-7.61 .. -6.31).
   Exact one-sided permutation on complete separation of 3 vs 3: p = 1/20 =
   0.05. So the tick's landing IS scheduling-correlated. But the ratio moves
   only 0.799 -> 0.868 and 3.90 pp SURVIVES at SCHED_FIFO 10, so scheduling
   accounts for ~2.6 of 6.5 pp (~40%) and is NOT the whole mechanism.
 * Arm C REFUTES "no gap when unsaturated": all three runs are negative, and
   its ratio median 0.7605 is no better than arm A's 0.7990 with 3 runnable
   clients on 4 cores. Removing saturation does not remove the gap. That is
   the more serious of the four readings — it is not an environmental limit.
 * AMBIGUOUS on magnitude: arm C's spread (4.30 pp) EXCEEDS the A-C median
   difference (1.83 pp), and C's range overlaps A's. So whether saturation
   MODULATES the size of the gap is unresolved. C is noisier because it has
   480-582 CPU samples per run against A's 1103-1147 (arms/ARMS_DELTAS.txt).

NEXT CHEAPEST MEASUREMENT: arms A and C only (B is already separated), same
10 Hz, escalation window 300 s instead of 60 s. That multiplies samples x5 at a
FIXED operating point, shrinking per-run noise ~2.2x; 6 runs ~= 32 min. Raising
the sample rate instead would be cheaper but invalid: the gap is itself
rate-dependent (0.77 at 10 Hz vs 0.88 at 200 Hz), so a 100 Hz run answers a
different question.

WHERE THE MISSING CPU GOES (ARMS_DELTAS.txt; this is what --show-idle added).
Sampled minus exact, same window, Gns = 1e9 ns. Arm A rep 1:
    CPU    -36.0 Gns      waits  +9.6 Gns      idle (ClientRead etc.) +51.7 Gns
The sign is the same in all nine runs for CPU (always negative) and idle
(always positive, +25.9 .. +51.7 Gns). The sampled tier reads less CPU and MORE
idle, every time. Do NOT read these three as a closed budget: the two tiers do
not observe the same total time (DB+idle differs by ~25 Gns in arm A rep 1),
because the exact tier records transitions for traced pids while the sampler
samples every pid each tick. They are three independent deltas with a
consistent sign, not an accounting identity.

FINDING, contradicting a premise in the issue text ("zero read failures"):
sampled_attr_tick_read_failures_total is 4881-4995 in EVERY one of the nine
runs, against sampled_attr_shadow_total of 4656 (arm A rep 1) and 1737 (arm C
rep 1) — i.e. 52% to 74% of the SHADOW comparison could not be taken at all.
It is essentially CONSTANT across arms despite arm C having 3 clients instead
of 8, the signature of a fixed set of structurally unreadable pids (background
processes) rather than load-dependent loss. This is NOT evidence that samples
are dropped: invalid_wait_reads_total and cmd_gate_order_read_failed_total are
0 in all nine, and the counter increments inside the diagnostic shadow path
(src/sampler.c:1173 on the fix branch), not the sampling path. But the
"nothing is being dropped" claim rests partly on a comparison that is blind for
most of its attempts, and that should be said out loud rather than assumed.
