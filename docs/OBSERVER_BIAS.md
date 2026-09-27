# Observer bias: why every in-database sampler under-reports load when load matters

Source material: issue #115 (ten comments, four independent investigations
across four throwaway machines, none of which still exist). Harness:
`tests/observer_bias_study.sh`, `tests/kernel_cpu_arbiter.py`, `tests/sched.bt`,
`tests/results/issue115_reconcile/`, `tests/results/issue115_pgws/`,
`tests/results/issue115_xtab/`. This document is the permanent, checked-in
record; the machines that produced the raw captures are gone, but every
number below is quoted from the issue thread and is reproducible with the
command in [How to reproduce](#how-to-reproduce).

## The claim

Every in-database wait-event sampler — a `pg_stat_activity` poller,
`pg_wait_sampling`'s background collector, or our own sampled tier — can only
observe the system at the instants the *operating system* schedules the
sampler's own process onto a CPU. The Linux scheduler hands out that CPU time
preferentially when the run queue is short, i.e. when the machine is least
busy. A sampler therefore systematically **under-reports how busy the system
is precisely when that matters most** — under load, not at idle. This is not
a bug in any one collector; it is a property every in-database sampler shares
by construction, because none of them can see the run queue except by asking
for a turn on the same run queue.

## Method

**Observers compared**, all over the same wall-clock window and the same
pgbench backend pids:
- our **exact tier** (`pg_wait_tracer`'s uprobe/watchpoint path, tiered mode)
- our **sampled tier** (the same daemon's periodic sampler)
- a **`pg_stat_activity` poller**: a separate backend, `pg_stat_clear_snapshot()`
  + `SELECT ... FROM pg_stat_activity` in a tight loop (~50 Hz)
- **`pg_wait_sampling`**: its own background collector at its default
  `profile_period` (10 ms), read via `pg_wait_sampling_profile`

**Kernel ground truth**: `sched:sched_switch` / `sched:sched_wakeup`
tracepoints via `bpftrace` (`tests/sched.bt`, `tests/results/issue115_*/sched*.bt.tmpl`,
`tests/results/issue115_xtab/xtab.bt.tmpl`), cross-checked against
`/proc/<pid>/schedstat` counter deltas (`tests/kernel_cpu_arbiter.py`). The
two agree to about 0.6% on on-CPU time and about 1% on run-queue delay —
independent instruments, same answer.

**Workload**: `pgbench` (TPC-B-like), two regimes on the same machine —
**saturated** (8 clients on a 4-vCPU throwaway box) and **unsaturated** (2
clients). **Window**: one escalation window per capture, 39–60 s depending on
the investigation (noted per table below; `xtab`'s decomposition used 39 s
windows specifically). **Repeats**: 3 interleaved sat/unsat captures per
investigation, plus 8 additional sampling-period control runs for the
`pg_wait_sampling` comparison.

## Numbers, with spread

Every mean below is of 3 repeats; `[min..max]` is the observed range, not a
confidence interval. **Units are stated on every row and are not
interchangeable** — issue #115 mixed a percentage-point bias in "fraction of
backends on-CPU-or-runnable" with a percentage-point bias in "CPU share
reported" once, and the two differed by nearly 2×, so both appear separately
below.

### CPU-or-runnable share, our tiers vs `pg_stat_activity`, reconciled

One 4-vCPU box, one workload, one harness scoring both prior investigations'
methods at once (`tests/results/issue115_reconcile/`). First-pass numbers,
then **re-verified on a second machine after two tooling bugs were fixed**
(below) — the two runs agree to about a point, which is machine-to-machine
variation, not the bugs:

| observer | saturated (8 clients) | unsaturated (2 clients) |
|---|---:|---:|
| exact tier, current (all-or-nothing) rule | 46.93 [46.70..47.20] % | 65.83 % |
| exact tier, proportional-overlap rule | 47.33 % | 62.10 % |
| our sampled tier | 44.33 % | 56.30 % |
| `pg_stat_activity` poller | 38.63 % | 60.29 % |
| kernel: on-CPU + run-queue delay | 67.14 % | 71.29 % |
| *re-verified (2nd machine, fixed tooling):* | | |
| exact tier, current rule | 45.83 % | 65.90 % |
| our sampled tier | 42.80 % | 57.20 % |
| `pg_stat_activity` poller | 37.90 % | 60.32 % |
| kernel | 66.26 % | 71.32 % |

The kernel is highest in every row: **every in-database observer sits below
kernel ground truth**, and the gap is much larger saturated than unsaturated.

### `pg_wait_sampling`, same instrument, second (later) machine

`tests/results/issue115_pgws/`. Whole-backend definition (no wait-event
state column in `pg_wait_sampling`, so every not-waiting backend counts as
CPU, matching the kernel side's definition exactly):

| observer | saturated (8 clients) | unsaturated (2 clients) |
|---|---:|---:|
| kernel: on-CPU + run-queue delay | 67.99 [67.69..68.32] % | 71.98 % |
| our exact tier | 62.27 [61.96..62.60] % | 67.85 % |
| `pg_wait_sampling` | 59.79 [59.74..59.82] % | 66.52 % |

### The bias, measured two ways, both units stated

**(a) fraction of backends on-CPU-or-runnable, by time vs. at the sampler's
own scheduling instants** — this is the direct mechanism test: take every
instant a sampler's own process was switched onto a CPU, and ask what
fraction of tracked backends were on-CPU-or-runnable *at that instant*,
against the same fraction computed by wall-clock time.

| sampler | saturated bias (of wall %) | unsaturated bias (of wall %) |
|---|---:|---:|
| `pg_wait_sampling` collector | −2.67 pp | (not separately isolated) |
| `pg_stat_activity` poller (pgws-comparison run) | −4.27 pp | (not separately isolated) |
| `pg_stat_activity` poller (reconciliation run) | −4.58 pp | +3.73 pp ¹ |

¹ Unsaturated, the poller's run-queue displacement is nearly absent (run
queue held 0.9–2.9% of wall time), so the small residual bias there is not
this mechanism — see [Caveats](#caveats).

The **same** bias, expressed as **(b) percentage points of the CPU share the
sampler reports** (share-of-non-idle, not share-of-wall — a different unit,
converted via the denominator each observer uses):

| sampler | saturated bias (of share) | 
|---|---:|
| `pg_wait_sampling` collector | −2.96 pp |
| `pg_stat_activity` poller (pgws-comparison run) | −6.13 pp |
| `pg_stat_activity` poller (reconciliation run, re-verified) | −6.45 pp |

An earlier pass reported "−6.4 pp" (first investigation) and "−3.7 pp"
(second investigation, using scheduler tracepoints) for what looked like the
same quantity; reconciled, they are the **same measurement in different
units** — −4.75 pp of wall equals −6.71 pp of share on that machine. The
"revised down to 3.7" conclusion was a unit error, not a smaller effect.

**(c) each collector's own measured wakeup-to-running delay** (microseconds,
not percentage points — the direct, causal quantity: how long the sampler's
own process waited for a CPU after the scheduler woke it):

| sampler | unsaturated | saturated (mean) | saturated (p90) |
|---|---:|---:|---:|
| `pg_stat_activity` poller | 8–16 µs | 128–512 µs | (not separately reported) |
| `pg_wait_sampling` collector | 8–16 µs | 16–32 µs | 256–512 µs |

Over one 39 s saturated window the `pg_wait_sampling` collector spent 0.65 s
queued against 0.21 s actually running — queued three times longer than it
ran.

### Be scrupulously fair to `pg_wait_sampling`

Its bias is **about half the poller's** — both in wall-fraction terms
(−2.67 pp vs −4.27 pp) and in share terms (−2.96 pp vs −6.13 pp) — **and its
overhead was the lowest of the three observers measured**: 0.54% of one
processor, half the `pg_stat_activity` poller's and an eighth of our own
escalated daemon's. Its probe costs roughly 42 µs, about half the poller's
per-probe cost, which is exactly why it waits half as long for a processor
and shows half the bias: **rate was ruled out** as an alternative explanation
(at 50, 100 and 1000 Hz its saturated bias measured −2.5, −2.67 and −1.6 pp —
not the halving or doubling a rate-driven effect would produce), and it also
held its nominal sampling rate where our own poller drifted.

**This finding is about a limitation every in-database sampler shares, not
about `pg_wait_sampling`'s implementation quality** — on this evidence
`pg_wait_sampling` is the best-behaved of the three in-database observers
measured, on both bias and overhead, and the paragraph above is stated
prominently for exactly that reason.

## Caveats

These are stated as plainly as the results:

- **An unexplained 1–3 percentage points on an idle machine.** Unsaturated,
  both the `pg_stat_activity` poller and the `pg_wait_sampling` collector show
  a persistent 1–3 pp gap from the kernel that is **not** run-queue
  displacement (the run queue is nearly empty unsaturated) and **not** period
  aliasing (ruled out by an 8-control period sweep). This is open.
- **Eight backends only.** Every measurement here used 8 saturated / 2
  unsaturated pgbench clients. `pg_wait_sampling`'s advantage over the poller
  should **narrow** as the backend count — and therefore its
  process-array-walk cost per probe — grows; this was not measured at higher
  concurrency.
- **`pg_wait_sampling` silently skips samples under load.** At a 1 ms
  `profile_period` it silently skips 17% of its own samples under load, via
  its collector's own deliberate skip path, and **the skipped samples are the
  busiest moments** — the opposite of a random drop. Its 10 ms default never
  skipped in these captures.
- **Our own definitional distance from the kernel is most of our gap, not
  measurement error — so do not read our number as "the true one" either.**
  The exact tier's own 6.03 pp saturated gap from the kernel
  (`tests/results/issue115_xtab/`, closes to a 0.000000 s residual in all 12
  decomposed runs by construction) decomposes into four cells — run-queue
  delay while still marked as waiting (+2.53 pp), on-CPU while still marked
  as waiting (+0.72 pp), run-queue delay while marked idle/`ClientRead`
  (+2.39 pp), and on-CPU while marked idle/`ClientRead` (+0.39 pp) — of which
  the first three, **5.64 pp, are definitional**: the kernel counts
  run-queue-queued time as CPU-or-runnable, our classifier counts it under
  whatever PostgreSQL wait event (or idle state) was open at the time, by
  definition, not by mistake. Only the fourth cell, **0.39 pp, is genuinely
  ours**: on-CPU execution — actually running, not queued — while the wait
  field still reads `ClientRead`, the protocol tail after the socket read
  returns; this is the one product fix the investigation identifies as worth
  making. **0.00 pp is unexplained.** In round numbers this is the "5.6 of
  5.7 points" the investigation opened with (kernel 67.99% − our exact tier
  62.27% = 5.72 pp headline; the closed decomposition's mean gap was 6.03 pp,
  of which 5.64 pp is definitional) — **the large majority of our own gap
  from the kernel is what we choose to count, not something we got wrong.**
  Unsaturated the same split is 2.72 pp definitional / 1.69 pp ours.

## Where investigations disagreed, and were reconciled

This history is a strength, not something to hide — it is why the final
numbers above carry spread and cross-checks instead of a single run:

1. **"Worse at higher sample rates" was never real.** The original #115
   report came from running each rate once; interleaved repeats showed the
   spread *within* a rate exceeds the spread *between* rates, with CPU-share
   deltas of 15.7/17.1/15.1 pp (10 Hz), 15.9/16.1/13.4 pp (100 Hz), 14.1/15.2/13.2 pp
   (200 Hz) — no trend, once measured with repeats.
2. **The sampled tier is right; an all-or-nothing command-boundary
   straddle rule in the exact tier was the first-suspected culprit** —
   partially. Reconciled size: **−0.43 pp saturated, +3.73 pp unsaturated**
   (low concurrency is the *worse* case for this rule, the opposite of the
   first guess), confirmed to two decimals on a second machine with the
   fixed tooling.
3. **Scheduler sampling bias, first measured at "6.4 pp", then "revised down"
   to "3.7 pp"** — both correct, in different units (wall-fraction vs.
   share; see above). The "revision" was a unit error, not new evidence of a
   smaller effect.
4. **The exact tier was originally reported as the *least* accurate of the
   three (worse than the sampled tier), then shown to be the *most*
   accurate** once a non-sampling kernel ground truth replaced a sampled
   `pg_stat_activity` reference — the original conclusion was comparing our
   exact tier against a reference that had the same scheduling bias as our
   sampled tier, which is exactly why the two agreed with each other and
   made the exact tier look wrong by comparison.
5. **`test_cross_validate_tiered.sh` measures scheduler bias under load, not
   tracer accuracy** — it grades the exact tier against the sampled tier,
   and under CPU saturation that comparison is dominated by the sampled
   tier's own scheduling bias. This is why it is informational rather than
   gating, and a wider tolerance would treat the symptom, not the cause.

## How to reproduce

```
sudo tests/observer_bias_study.sh 3          # 3 repeats, all three legs
REPEATS=1 sudo tests/observer_bias_study.sh  # quick smoke, one repeat
LEGS="reconcile xtab" sudo tests/observer_bias_study.sh   # skip the pg_wait_sampling leg
```

**Preconditions** (this is the live tier — Linux, root, `bpftrace`; nothing
here runs on macOS, see `CLAUDE.md`): a running PostgreSQL with `pgbench -i`
already run against it, `pg_wait_sampling` built and in
`shared_preload_libraries` for the `pgws` leg, and
`kernel.sched_schedstats=1` — the script (and `kernel_cpu_arbiter.py sample`
itself) **asserts** this rather than silently reading zeros for the
run-queue-delay column on a fresh machine that never had it set.

**Expected shape**: the kernel arbiter reports the highest on-or-runnable
share in every regime; our exact tier is closest to it; the sampled tier,
`pg_stat_activity` and `pg_wait_sampling` (if included) under-report by
increasing amounts, worst under saturation — matching the tables above
within normal run-to-run spread (a few points; see the reconciled-vs-first-pass
comparison above for what that spread actually looks like).

## Known-broken tooling, fixed, and made to fail loudly

Four bugs were found in the measurement tooling itself while producing the
numbers above — recorded here because a study is only as good as the harness
that produced it, and because the bypass-suite clause requires each to fail
loudly now rather than silently:

1. **A committed `.bt` script with one run's pids already baked in.**
   `tests/results/issue115_arbiter/sched2.bt` (an early, uncommitted scratch
   copy, never landed) had `__TRACK__`/`__SAMPLER__` already substituted with
   one specific run's real pids. Re-run against a different pid set, it
   compiled and ran fine — bpftrace has no way to know the pids are stale —
   and every counter it printed read `0.000 s`, silently. Fix, landed: every
   `.bt.tmpl` here (`sched2.bt.tmpl`, `sched3.bt.tmpl`, `xtab.bt.tmpl`,
   `tests/sched_sampler_bias.bt.tmpl`) keeps its placeholders as literal
   `__WORD__` tokens no bpftrace program can parse un-substituted, and every
   capture script that renders one now `grep`s the rendered output for a
   leftover placeholder *and* asserts the tracked-pid count matches the
   client count, aborting with `FATAL:` before running `bpftrace` at all.
   `recon_capture.sh` (the `cpu-reconcile` branch) was missing both checks
   even though the later `pgws_capture.sh` / `xtab_capture.sh` already had
   them — added here. Demonstrated: `tests/test_observer_bias_guards.sh`
   feeds each template a synthetic partial substitution and asserts the
   guard's `grep` still trips.
2. **bpftrace reprints every populated map at program exit.** Unless a
   histogram map is `clear()`d in the `END` block, bpftrace's own default
   end-of-program dump reprints it a second time — silently — right after
   the script's own explicit `printf()`+`print()` of the same histogram,
   doubling every count the parser then read (this is how the
   `pg_wait_sampling` wakeup-delay numbers above were first measured wrong).
   Fixed two ways: every `.bt`/`.bt.tmpl` histogram map used here now
   `clear()`s itself after printing, *and* `tests/kernel_cpu_arbiter.py`'s
   `delays` parser now refuses (`sys.exit`, not a silent "keep the first
   block") the instant it sees the same map's data appear a second time.
   Demonstrated: `tests/test_kernel_cpu_arbiter.py` feeds `cmd_delays` both a
   clean and a doubled synthetic dump and asserts the doubled one is
   refused, not silently halved back down.
3. **`--exclude-pid` silently capped at 16.** `tests/xp_gate.c`'s exclude
   list was a fixed `uint32_t excl[16]`; a 33-exclude reconciliation
   invocation silently dropped everything past the 16th, with no error and
   no changed exit code. (Exposure was measured, not assumed, once found: on
   the affected data the dropped pids were short-lived helpers with no
   command markers, worth 0.00 pp on every tier in all six affected runs —
   but that is a fact about that data, not a property of the cap.) Fixed:
   the cap is now 4096 (`MAX_EXCL`), and exceeding it is a hard `fprintf`
   + `return 2` refusal, never a silent drop. Demonstrated (compiled and run
   on this machine, not just reasoned about): `./xp_gate <dir> --exclude-pid
   <n> [x4097]` exits 2 with `xp_gate: --exclude-pid count exceeds the
   compiled-in cap of 4096; refusing to silently drop the rest (issue #115)`.
4. **`kernel.sched_schedstats=1` is a precondition, not a default.** A fresh
   machine from the gate snapshot has it unset, so `/proc/<pid>/schedstat`'s
   `run_delay` field — the entire run-queue-delay side of every comparison
   in this document — silently reads 0 until it is enabled.
   `kernel_cpu_arbiter.py sample` already asserted this (`return 2` with a
   message) before this study; `tests/observer_bias_study.sh` now asserts it
   again up front, so a bad machine is rejected in the first second rather
   than after the first ~1-minute capture completes. Demonstrated:
   `tests/test_kernel_cpu_arbiter.py` monkeypatches the one sysctl read and
   asserts `cmd_sample` returns 2 instead of proceeding.
