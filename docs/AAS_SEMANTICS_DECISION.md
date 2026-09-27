# Decision: AAS semantics — the full decomposed model

Date: 2026-07-12
Status: DECIDED (T2 gate of docs/ROADMAP_AND_STATUS.md)
Evidence: docs/T2_IOWORKER_STUDY.md (empirical study, PG 18.4)
Companion decision: Client:ClientRead = idle-but-visible (unchanged).

## The decision

**Active = non-idle wait OR on-CPU**, where "on-CPU" (`wait_event_info
== 0`) counts:

- for **client backends**: only while a command is open — the same
  window PostgreSQL uses for `pg_stat_activity.state = 'active'`
  (query message received → command complete). This INCLUDES parse,
  plan, bind, execute, and commit/abort processing. It excludes only
  post-command protocol/idle time, which PostgreSQL itself reports as
  idle. Rationale: ungated `we==0` counting measured ~3× true activity
  on chatty OLTP (between-command bookkeeping slices scale with
  transaction rate, not with work); an executor-only gate was rejected
  because it would wrongly exclude commit and parse/plan CPU.
- for **background processes** (autovacuum workers, checkpointer,
  bgwriter, walwriter, …): always. Their parked states are
  instrumented (`Activity` wait class, already classified idle), so
  `we==0` unambiguously means working. No gate needed.

**AAS is decomposed into exhaustive, visible categories — nothing
observed is dropped:**

| Category | Contents | Attribution |
|---|---|---|
| foreground / planning | client-backend waits+CPU inside PLAN window | query_id |
| foreground / execution | waits+CPU inside EXEC window (incl. parallel workers, attributed to the leader's query) | query_id |
| foreground / command overhead | in-command outside plan/exec: parse, bind, commit/abort | query_id where the command has one; else labeled `command overhead` — visible, never dropped |
| maintenance | autovacuum worker activity | labeled (no query_id) |
| background | checkpointer / bgwriter / walwriter / … activity | labeled |
| idle | ClientRead + post-command time | excluded from AAS, visible |

**io_workers (PG18 `io_method=worker`) are EXCLUDED from AAS and
DB Time but surfaced as a dedicated "io_worker busy %" utilization
metric.** Measured basis (study Q1/Q2): counting them double-counts the
same logical I/O 2.0× (the requesting backend already shows
`AioIoCompletion` for that read; AAS would inflate +60% on I/O-bound
work vs PG17 for the identical workload), and io_workers are
structurally query-less (0/342 samples attributable → ~42% of I/O-wait
AAS would become dark matter). Their busy% is a genuine capacity
signal (measured 80% busy at the default 3 workers = near saturation).

## Consequences

- Sampled-tier AAS becomes equal by construction to
  `pg_stat_activity`-based ASH sampling and consistent with the exact
  tier — no step artifact at tier switches (closes finding AAS-1's
  semantic half).
- CPU-bound incidents become visible to the anomaly engine (study Q4:
  waits-only AAS understated by −29% on I/O scans, −71% on a lock
  convoy, −≥98% on a CPU storm).
- Vacuum storms and checkpoint I/O storms move the headline AAS chart
  (as distinct stacked bands) and are anomaly-detectable.
- Total-process CPU utilization remains a separate metric class; AAS
  answers "how many sessions are doing requested work", utilization
  answers "how busy are the postgres processes".
- Cross-version comparability: PG18-with-AIO AAS stays comparable to
  PG17 AAS for the same workload (no io_worker inflation).

## Implementation notes (Phase T2)

- Command-open gate: probe the `pg_stat_activity` state transition
  (e.g. uprobe pair on `pgstat_report_activity`) or an equivalent
  boundary validated against `pg_stat_activity` ground truth; the
  daemon's existing per-pid lifecycle registry carries the flag the
  sampler reads.
- The plan/exec sub-windows come from the existing PLAN_START /
  EXEC_START markers.
- Prerequisite fix (study finding): the uprobe attach path computes
  offsets as `va − 0x400000` (non-PIE assumption) — silently dead
  uprobes on PIE builds (proof: run_cnt=0 while USDT ran 61,869×).
  Must be fixed via proper ELF program-header offset translation
  before any new gate probe is added.
- `pg_stat_io` cross-checks must compare COUNTS, not time (study Q3:
  the historical 0.80 "async overlap" ratio was per-op
  instrumentation-window overhead, 0.55–0.90 depending on latency).

## Addendum (issue #115): CPU\* versus the operating system's own figure

**The convention, stated plainly.** CPU\* is time with no PostgreSQL wait
event set — nothing more. A backend keeps its wait's label (and its full
wall-clock duration) for as long as that wait_event is set, even for the
part of that duration spent queued for a processor after being woken, or
spent already running on one before the field is cleared. So a backend
never "loses" time between the wait bucket and CPU\* — every nanosecond the
kernel can account for is inside DB Time, under one label or the other —
but a fraction of what a kernel-level tool would call "on a CPU or waiting
for one" is filed under the wait that caused it rather than under CPU\*.
This is deliberate, not a bug: it matches Oracle's Active Session History
(`ON CPU` is a state a session reaches only after leaving a wait bucket)
and Amazon's Performance Insights, and it is the more useful convention for
diagnosis — a backend queued for a processor while waiting on
`LWLock:BufferContent` is still telling you the story is lock contention,
not "CPU load" in the generic sense.

**The consequence: an operating-system figure reads higher, on the same
window, by construction.** `top`/`ps`-style per-process accounting (and any
run-queue-aware sampler) counts a process as consuming a processor for as
long as it is running OR runnable, with no notion of a PostgreSQL wait
label at all. Every nanosecond pg_wait_tracer files under a wait's label
that the kernel would count as "on CPU or run-queue" is, from the
operating system's side, indistinguishable from CPU\*. The gap is not
noise or measurement error on either side; it is two different, equally
valid partitions of the same wall time.

**The gap's shape (measured, kernel-timeline-joined ground truth, one
saturated TPC-B window, from issue #115's final
reconciliation — the split moves with workload structure, not the total):**

| component | share of the gap |
|---|---|
| run-queue delay while a (non-idle) wait event is set | ~42% |
| run-queue delay while `Client:ClientRead` (idle) is set | ~40% |
| on-CPU execution while a wait event is set (the S3 "spin" measurement) | ~12% |
| on-CPU execution while idle is set (the protocol tail after a socket read returns, before the next wait/idle transition) | ~6% |
| unexplained | ~0% |

Two-thirds of the gap is run-queue delay (queued for a processor, not yet
running), split roughly evenly between "queued while a real wait is set"
and "queued while idle is set" — both invisible to a per-process CPU
sampler for exactly the reason above (it under-samples the state it is
trying to measure: the sampler's own wakeup competes for the same queue).
The remaining third is genuine on-CPU execution that pg_wait_tracer already
attributes to a wait or to idle rather than to CPU\*, by the stated
convention.

**What pg_wait_tracer's own numbers are comparable with.** CPU\* alone is
comparable to *no* single figure `top` reports — it is a strictly narrower
quantity by design. The daemon surfaces one further figure that narrows the
gap without changing the convention: `os_cpu_ms` (`src/compute.c`
`pgwt_compute_time_model`, "OS CPU (ref)" in the web UI's overview panel) is
CPU\* plus every wait's and every idle interval's own measured on-CPU
nanoseconds (the two "on-CPU execution while X is set" rows above) — the
most complete on-CPU figure obtainable from what the daemon already
measures per interval, without adding a new instrument. It is comparable to
a kernel on-CPU-only figure (e.g. `utime+stime`), and it still reads *below*
a run-queue-aware figure (`top`'s notion of "busy", Oracle/AWS ASH's `ON
CPU`, or the kernel-timeline join above), because pg_wait_tracer has no
scheduler-tracepoint instrumentation and therefore cannot see run-queue
delay at all — the two-thirds of the gap above stays permanently
unaccounted for on our side until that instrument exists. The UI withholds
`os_cpu_ms` outright (rather than showing an under-counted number) for any
window where even one wait/idle interval's on-CPU contribution is not
actually measured — a sampled-tier window, a `.trace` v2 file, or a tier
switch partway through the window.
