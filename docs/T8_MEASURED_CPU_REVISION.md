# T8 Revision — Conserved CPU\* + residual Off-CPU\* (measured-CPU accounting fix)

Status: ⬜ PLANNED — gates v0.13 alongside/replacing the T8 acceptance.
Date: 2026-07-19
Base: branch `t2-fix-el9-cpu-gate` @ `96df0df` (the merged-ready T8), plus
the merged straddle+PIE fixes (#48). This revision REPLACES T8's
per-interval Off-CPU\* math and its self-check; it keeps T8's measured
`sum_exec_runtime` capture, format v3, capability probe, and terminal
flush.
Origin: EL9 close-out validation (2 real findings) + a first-principles
walkthrough of a mixed `CPU → LWLock → CPU → LWLock → IO → CPU` backend.

---

## 1. What the EL9 validation + analysis established

The measured mechanism (BPF reads `task->se.sum_exec_runtime` at each
watchpoint wait-boundary) is correct **in sum** but imprecise **per
interval**, because `sum_exec_runtime` is only brought current at a
scheduler tick (CONFIG_HZ=1000 → 1 ms) or a context switch. A backend
that wakes, runs a **sub-millisecond CPU burst**, then enters a wait is
read at the wait-entry *write* — BEFORE the deschedule updates the
accumulator — so the burst's CPU is missing there and surfaces in the
*next* read, attributed to the **wait** interval instead of the CPU
interval that burned it.

Measured consequences (Rocky 9.7, PG18):
- Pure `pg_sleep` (real sleeps, no CPU between): `wait_gap_cpu ≈ 88 ms`
  → the boundary/tick leak is negligible when there's no CPU to leak.
- pgbench (sub-ms CPU bursts between frequent waits):
  `wait_gap_cpu ≈ 24.3 s ≈ CPU* itself` → roughly half of all CPU is
  mis-timestamped into the following wait intervals.

**Two things are therefore reliable, and two are not:**
- RELIABLE: (a) every interval's **wall duration** (watchpoint
  timestamps — exact); (b) **Σ of all `cpu_ns`** = final − initial
  `sum_exec_runtime` = the backend's **true total CPU** (telescoping
  sum, leak-immune).
- NOT reliable per-interval: which specific interval a sub-ms burst is
  credited to, and hence any per-interval CPU/off-CPU/spin split for
  wait-interleaved workloads.

### Why the obvious per-interval fixes fail (do not attempt them)
For the sequence `CPU1(5,5) → LWLock1(20,0) → CPU2(0.4,0.4) →
LWLock2(15,0) → IO(10,0) → CPU3(3,3)` (dur, trueCPU in ms; true DB=53.4,
CPU=8.4, LWLock=35, IO=10):
- **Model A** — count wait `cpu_ns` into CPU\* AND keep full wait
  durations, Off-CPU\* = Σ(we==0)(dur−cpu_ns): **double-counts** the
  leaked CPU → DB Time overshoots by `Σ wait cpu_ns` (0.6 ms here; ~24 s
  on pgbench).
- **Model B** — wait class = dur−cpu_ns: **conserves DB Time** but
  deflates wait time and inflates Off-CPU\* by the leak (~24 s on
  pgbench). Both are unacceptable.

## 2. The fix — residual Off-CPU\*, computed from exact totals only

Never use per-interval `cpu_ns` for the CPU/off-CPU *split*. Use it only
in the **sum**. Per accounting window (and per AAS bucket):

```
CPU*      = Σ cpu_ns over ALL intervals (we==0 AND wait intervals)   [conserved = true CPU]
wait[c]   = Σ duration of intervals of wait-class c                  [exact from timestamps]
DB Time   = Σ duration over ALL non-idle intervals                   [exact from timestamps]
Off-CPU*  = max(0, DB Time − CPU* − Σ_c wait[c])                     [residual → runqueue/unaccounted]
```

Identity holds by construction: `CPU* + Off-CPU* + Σ wait[c] = DB Time`.
It is **leak-immune**: the leak moves `cpu_ns` between the null-interval
and the following wait-interval, but CPU\* is a *sum* (unchanged) and DB
Time / wait durations come from *timestamps* (unaffected). So Off-CPU\*,
computed as the residual, equals the real runqueue/preemption time of
on-CPU gaps regardless of the leak.

Verify on the §1 sequence: CPU\*=8.4, LWLock=35, IO=10,
Off-CPU\*=53.4−8.4−45=**0** (free core → correct; Models A/B gave 0.6).
Oversubscribed pure-CPU: CPU\*=measured (e.g. 66% of wall), waits=0,
Off-CPU\*=DB−CPU\*=**34%** (the runqueue — Finding #1's headline, now
correct). Oversubscribed wait-heavy: Off-CPU\*=DB−CPU\*−waits = real
runqueue during null gaps.

### What this delivers vs. does not (state honestly in docs/UI copy)
- ACCURATE: per-backend / per-query / per-window **CPU\*** (fixes the
  under-report), all **wait durations**, **DB Time**, and **Off-CPU\***
  as the residual (runqueue + genuinely-unaccounted time — the 10046
  "unaccounted-for time" line).
- NOT AVAILABLE: trustworthy **per-interval** CPU placement, and any
  **per-wait "spin vs blocked"** split. The wait interval's `cpu_ns` is
  boundary-leaked pre-wait CPU, not in-wait spin — do NOT surface a
  spin/blocked decomposition (it would lie). See §5 for the exact-per-
  interval path (S3).

## 3. Concrete changes

### 3.1 compute.c (`compute_time_model`, `compute_aas`)
- **CPU\* total** = Σ `cpu_ns` over ALL intervals whose `cpu_ns !=
  UNKNOWN` (both `old_event==0` and wait intervals). Today (~line
  682–704) wait `cpu_ns` only feeds the self-check counter and is
  dropped from `cpu_measured_ns` — add it in.
- **Remove the per-interval Off-CPU\* split** (the `dur − cpu_ns` per
  on-CPU gap at ~line 682–687 / the `offcpu_aas += ov*(1−cpu_frac)` at
  ~line 445–448). Replace with the **residual**: after summing CPU\* and
  each wait class over the window/bucket, set
  `offcpu = max(0, db_time − cpu_measured − wait_total)`.
- **Remove the stale-0 → full-gap-as-CPU fallback** for measured-capable
  data (it caused replay CPU\* > physical cores, Finding #1). A stale-0
  `cpu_ns` on a we==0 gap means "the burst leaked to an adjacent
  interval," not "the whole gap was CPU." Because CPU\* is now a sum,
  the leaked burst is already counted in its landing interval — do not
  re-add the full gap. Keep the fallback ONLY for `cpu_ns == UNKNOWN`
  (legacy/v2/sampled, genuinely unmeasured): those windows show CPU\*
  by the old gap-inference and **no Off-CPU\*** (can't compute a residual
  without measured CPU).
- **Off-CPU\* fidelity gating**: emit Off-CPU\* only for windows/buckets
  where measured `cpu_ns` exists (v3 exact). Sampled/v2 windows: CPU\*
  only, no Off-CPU\* row.
- **Bounds**: clamp Off-CPU\* at 0 (never negative); count clamps in a
  metric (`offcpu_clamped_total`) — a nonzero clamp means CPU\* exceeded
  DB−waits, i.e. a measurement inconsistency worth surfacing.

### 3.2 BPF / terminal flush (Finding #1 — replay > physical)
- The terminal flush and the live open-interval path must emit
  **schedstat-measured** `cpu_ns` for the still-open interval (read
  `/proc/<pid>/schedstat` field 1 delta since the interval's seed), NOT
  wall-as-CPU. This bounds a pure-CPU straddler's CPU\* to actual on-CPU
  ns (≤ wall × 1 core per backend), so replay can never report CPU\* >
  physical capacity. (T8 already added the flush; the bug was it
  recorded the gap-inference wall value — switch it to the measured
  value, consistent with §3.1 removing the fallback.)

### 3.3 Self-check (Finding #2 reframe)
- The premise "a wait burns ~0 CPU" is FALSE (sub-ms bursts leak into
  waits). Replace the `test_cross_validate_tiered` wait-gap-≤1% check
  with the **conservation** invariant: over a pgbench window, tracer
  **CPU\*** must equal the sum of traced backends' `/proc/<pid>/stat`
  utime+stime deltas within tolerance (e.g. ±10%). Keep
  `wait_gap_cpu_ns_total` as an **observability** metric (it quantifies
  how much CPU is boundary-attributed — useful diagnostics), not a gate.

### 3.4 Views / mock / metrics
- Off-CPU\* row/category unchanged in shape (already `offcpu`), now fed
  by the residual. Mirror any field changes in `tests/mock_server.py`
  (protocol-drift). `docs/TRACE_FORMAT.md` needs no format change (v3
  `cpu_ns` column stays); update the compute/semantics prose.

## 4. Tests / acceptance (the proof)
1. **Sequence identity** (synthetic, `test_data_*`): a trace encoding a
   mixed `CPU/LWLock/IO` sequence with a known sub-ms burst → assert
   `CPU* + Off-CPU* + Σwaits == DB Time` exactly, wait durations exact,
   CPU\* == Σ true cpu_ns. This is the regression that Models A/B fail.
2. **Conservation (live, EL9)**: pgbench under `--mode full`; tracer
   CPU\* within ±10% of Σ `/proc` utime+stime of the traced backends
   over the window (replaces the wait-gap self-check).
3. **Off-CPU\* under oversubscription (live)**: N_hogs > vCPU, pure-CPU
   → Off-CPU\* > 0 and `CPU* + Off-CPU* ≈ DB Time`; and a NON-
   oversubscribed run → Off-CPU\* ≈ 0 (the §1 sequence gives 0). Both
   must hold (the old code gave false-positive Off-CPU\*).
4. **Replay not > physical (live)**: the pure-CPU straddle trace,
   replayed, reports CPU\* ≤ wall × ncores (Finding #1 closed).
5. v3 round-trip, golden rev3, rev2-still-green, v2-fallback (CPU\* only,
   no Off-CPU\*) — unchanged from T8, must stay green.
6. All 8 CI jobs green; nightly `--core` semantics unchanged.

## 5. Deferred, more-accurate alternative: S3 (sched_switch tracing)
The residual model gives correct **totals** cheaply (no new probes) but
**cannot** give per-interval CPU placement, per-wait spin/blocked, or
per-interval Off-CPU\* for wait-interleaved workloads — the sub-ms leak
is fundamental to reading `sum_exec_runtime` at wait boundaries.

**S3** attaches a BPF program to the `sched_switch` tracepoint and
accounts each tracked backend's on-CPU time at **every context switch**
(exact, no tick quantization), keyed by the state_map. That yields:
- exact per-interval CPU (no leak) → trustworthy per-interval Off-CPU\*,
- a *real* per-wait on-CPU-spin vs truly-blocked decomposition,
- exact CPU/wait attribution even for sub-ms bursts.

Cost: the tracepoint fires for **every** context switch system-wide
(filter-and-drop for untracked pids is ~tens of ns, but on a busy host
that's 10⁵–10⁶ events/s of overhead-bearing work), and it's a larger
BPF surface + a per-CPU reorder concern. It is the right foundation for
a future **off-CPU analysis** track (runqueue latency, involuntary vs
voluntary switches) but is more machinery than v0.13 needs. This
revision's data model (measured `cpu_ns` + residual Off-CPU\*) is
forward-compatible: S3 would replace the *source* of per-interval CPU
without changing the CPU\*/Off-CPU\*/DB-Time contract.

Recommendation: ship the residual model for v0.13; keep S3 as a planned
"exact per-interval CPU / off-CPU analysis" phase (its own doc when
scheduled).

## 6. Workflow
Branch off `t2-fix-el9-cpu-gate`; append commits (keep #46-equivalent
green). No AI attribution; root-cause only; fails-before/passes-after
tests; verify live on the EL9 box (root@116.202.96.172, key
/home/dmitryfomin/gitlab/plan_flip/dmitry-plan-flip-test.key — do not
write elsewhere in that dir) + CI matrix. After green, resume the
EL9→EL8→Ubuntu validation matrix, then v0.13.
