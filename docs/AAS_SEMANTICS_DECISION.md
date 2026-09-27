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

## Addendum (#187): the CPU group's two bands

Date: 2026-09-27
Status: DECIDED (owner-approved, closes #187; implements the recommendation
recorded on #115)

**The CPU group's meaning does not change.** "CPU" keeps meaning running
plus waiting for a processor — the Oracle ASH / AWS RDS Performance Insights
convention this decision already cites above, and what a database engineer
assumes "CPU" means in a wait-based tool. What changes is only that, where
the exact tier can measure the two halves separately, the UI renders them as
two adjacent bands/rows instead of one:

- **CPU (running)** — on a processor right now (`wait_event_info == 0` AND
  actually scheduled).
- **CPU (waiting for a core)** — runnable but sitting in the kernel run
  queue. No PostgreSQL wait event exists for this state, so by the group's
  own definition it is CPU, not an unaccounted gap; #115's escalation-window
  investigation found this to be a real, measurable ~3.4-3.7pp-of-non-idle
  effect under CPU saturation, not sampling noise.

**Conservation, not addition.** `CPU (running) + CPU (waiting for a core) +
Σ every other wait class == DB Time` held before this change too (T8,
`src/compute.c`'s `offcpu_ns` residual) — #187 was that the web UI's AAS
chart silently DROPPED the already-computed `offcpu` series
(`web/static/lib/builders/aas.js` stacked only the classes in
`WAIT_CLASSES`, which had no `offcpu` entry), under-summing the headline
chart's bands by exactly the run-queue time (~152s against 177s of
execution in one measured window). This addendum fixes the display, not the
computation.

**Two caveats ride the same asterisk, not one.** An un-split `CPU*` row
(the sampled tier, or the rare exact-tier window with no measured `cpu_ns`
at all) means either or both of:

1. PostgreSQL does not instrument every code path — some genuine CPU time
   has no wait event and no further explanation (the ORIGINAL asterisk
   caveat, unchanged).
2. This window cannot separate "running" from "waiting for a core" even
   where the exact tier could elsewhere — sampling cannot observe the run
   queue at all (`pg_stat_activity`-style samplers only get scheduled when
   the run queue is short, so they systematically under-observe exactly the
   state they are trying to measure — #115's own finding).

**Tier-switch behavior**: the sampled tier's `offcpu` value is always 0 (not
absent — see `src/compute.c`'s `pgwt_compute_aas_from_summaries`), so the
CPU group's stacked TOTAL is continuous across a tier switch mid-window; only
the split disappears. No renormalization step, no visible jump.

**Colour**: `offcpu`'s band uses a related hue to `cpu`'s (same green, darker/
less saturated) so the two read as one group split in two rather than
unrelated categories — flagged for the owner's eye as a starting point, not
a final decision (`web/static/lib/format.js`).
