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
  bgwriter, walwriter, …): always. No gate needed.

  > **CORRECTED 2026-10-06.** This bullet used to justify "always" with
  > *"their parked states are instrumented (`Activity` wait class, already
  > classified idle), so `we==0` unambiguously means working"*. **That
  > premise was false**, and the checkpointer is the counter-example:
  > `Timeout:CheckpointWriteDelay` is a SECOND parked state, and it lives in
  > the `Timeout` class, not `Activity`. So a checkpointer asleep on a timer
  > was counted as DB Time — 807,549 ms (0.897 AAS) in one measured 900 s
  > demo window, 37.5% of that window's entire DB Time. The conclusion
  > ("always", i.e. no command-open gate for background processes) still
  > holds and did not depend on the premise: the gate's purpose is to
  > exclude between-command protocol time, which background processes have
  > none of. What the false premise hid is that `Activity` was never the
  > complete list of parked states. See "Timer sleeps are not DB Time"
  > below for the rule that replaced it.

**AAS is decomposed into exhaustive, visible categories — nothing
observed is dropped:**

| Category | Contents | Attribution |
|---|---|---|
| foreground / planning | client-backend waits+CPU inside PLAN window | query_id |
| foreground / execution | waits+CPU inside EXEC window (incl. parallel workers, attributed to the leader's query) | query_id |
| foreground / command overhead | in-command outside plan/exec: parse, bind, commit/abort | query_id where the command has one; else labeled `command overhead` — visible, never dropped |
| maintenance | autovacuum worker activity | labeled (no query_id) |
| background | checkpointer / bgwriter / walwriter / … activity | labeled |
| idle | ClientRead + post-command time + Timeout **pacing** sleeps (2026-10-06) | excluded from AAS, visible |

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

---

## Addendum, 2026-10-06: timer sleeps are not DB Time

Status: **DECIDED** by the owner, 2026-10-06. Supersedes nothing in the
decomposed model above; it corrects the *membership* of the `idle` row.

### The rule, verbatim

> "there is DB time when database doing smth - not just waiting for timer"

> "if database is consuming any resources (CPU, IO and so on) - it is not idle
> and must go to DB time"

This is **not** a foreground/background rule. A process asleep on a timer is not
doing work, whoever it is. It is the test every future wait event is judged
against.

### The split

Six `Timeout` events are pure pacing sleeps: **excluded from DB Time and AAS,
still VISIBLE** everywhere (the `Client:ClientRead` precedent —
`pgwt_is_idle_event` yes, `pgwt_is_hidden_event` no).

| Event | Why it is a pacing sleep |
|---|---|
| `Timeout:CheckpointWriteDelay` | the checkpointer spreading its writes over `checkpoint_completion_target` |
| `Timeout:VacuumDelay` | the `vacuum_cost_delay` throttle |
| `Timeout:BaseBackupThrottle` | base-backup rate limit |
| `Timeout:RecoveryApplyDelay` | `recovery_min_apply_delay` |
| `Timeout:RecoveryRetrieveRetryInterval` | wait before retrying a WAL fetch |
| `Timeout:WalSummarizerError` | retry delay after an error — nothing is being done |

Four stay **in DB Time**:

| Event | Why it stays |
|---|---|
| `Timeout:PgSleep` | **owner decision, the narrower reading**: a client ASKED for the sleep and the server is inside that command. Not the database pacing itself. |
| `Timeout:SpinDelay` | backoff inside an OUTSTANDING attempt to acquire a contended spinlock |
| `Timeout:RegisterSyncRequest` | backoff inside an outstanding attempt to push through a FULL fsync queue |
| `Timeout:VacuumTruncate` | backoff inside an outstanding lock acquisition |

**The operative test is "timer sleep with NO OUTSTANDING RESOURCE REQUEST", not
"calls `pg_usleep`".** All four of the retained events call `pg_usleep`; what
keeps them in DB Time is that the process is in the middle of trying to get
something.

### The SpinDelay tension, written down rather than papered over

A strict reading of the second quoted sentence — "is it consuming resources
during this span" — would make `SpinDelay` **idle**, and it is worth being
honest about why it is not.

In PostgreSQL's `perform_spin_delay()` the `SpinDelay` wait event brackets only
the `pg_usleep` **backoff**; the spinning itself (the `SPIN_DELAY()`
pause-instruction loop that actually burns CPU) happens OUTSIDE the wait event.
So during a `Timeout:SpinDelay` span the process is genuinely asleep and
consuming nothing.

It stays in DB Time anyway, because the process has an **outstanding request for
a contended resource**, and "a backend is stuck trying to get a spinlock" is
precisely the diagnostic a DBA needs the number for. Hiding it would turn the
busiest possible signal — spinlock contention — into invisible idle time. The
same reasoning covers `RegisterSyncRequest` (a full fsync queue) and
`VacuumTruncate` (a lock retry): all three are *waiting for something*, which
the pacing sleeps are not.

This is a judgment call that prefers the diagnostic over the literal reading,
and it is recorded here so a future reader does not mistake it for an oversight.

*(Caveat on the `perform_spin_delay` description: it matches the author's
reading of `src/backend/storage/lmgr/s_lock.c`, but no PostgreSQL source tree
was available in the worktree where this was written, so it is not a verified
citation. If it is wrong, the conclusion still stands on the
outstanding-request argument alone, which does not depend on where the wait
event brackets sit.)*

### STATIC classification, deliberately — not a dynamic `cpu_ns` test

The rule is implemented as a **static, per-event list** (`src/idle_rule.c`,
`pacing_timeout_names[]`), **not** as a runtime test on each span's measured
`cpu_ns`.

That is not a weakening of the owner's rule; a dynamic test would be an unfaithful
implementation of it. `se.sum_exec_runtime` is only current at a scheduler tick
or a context switch, so a sub-millisecond CPU burst just before a wait is
attributed to the FOLLOWING interval (the known leak at `src/compute.c`'s idle
skip, see "Known leak" below). Measured CPU inside a pacing span is therefore
largely *measurement spillover from the preceding work*, not work done during
the sleep. A dynamic rule would reclassify the same event run to run depending
on where a tick happened to land, and DB Time would stop being reproducible —
the same window would answer differently on two reads. The static list gives one
answer per event, on every tier, on every PG version.

### Version-correctness is the implementation's hard part

The `Timeout` event ids are **version-dependent, and ids 1 and 2 invert**:

| id | PG18 | PG13 |
|---|---|---|
| 1 | `CheckpointWriteDelay` (pacing) | `PgSleep` (DB Time) |
| 2 | `PgSleep` (DB Time) | `RecoveryApplyDelay` (pacing) |
| 7 | `VacuumDelay` (pacing) | *(does not exist)* |

A hardcoded id list is therefore not merely imprecise on PG13 — it gets
`PgSleep` exactly backwards. So the set is written down as NAMES and the
id-indexed mask is derived from the ACTIVE name table, rebuilt at the end of
both `pgwt_init_event_names()` and `pgwt_load_names_json()` (pgwt-server calls
them in that order, so a trace's own sidecar must get the last word).
PG14–16 use best-effort tables, so their `Timeout` classification is
**not verified** — only PG13 and PG17/18 are.

### Where the time goes instead: a named Idle row

Making 807 s of `CheckpointWriteDelay` idle without saying so would have moved
an unexplained number onto the Overview. The time model now emits the Idle total
with a NAMED breakdown:

```
DB Time                             1,346,000 ms   indent 0
  ...classes...                                    indent 1
Idle                                1,207,000 ms   indent 0
  Timeout:CheckpointWriteDelay        807,549 ms   indent 2
  Client:ClientRead                   399,000 ms   indent 2
```

Idle sits at indent **0**, not 1: `tests/demo_rehearsal_lib.py`'s
`time_model_conservation()` sums every indent-1 row and requires the total to
equal `db_time_ms`, and idle time is by definition not in `db_time_ms`.
Indent 0 is also what `web/static/views/overview.js` already looks for and what
`tests/mock_server.py` has shipped in its fixture all along — the real server
simply never emitted the row. Hidden (`Activity`-class) idle time stays inside
the parent total and gets no child row: `pgwt_is_hidden_event` is unchanged, and
a background process parked in its main loop is not a diagnostic.

### Known leak, documented and NOT fixed here

`src/compute.c` skips an idle interval WHOLE, including its measured `cpu_ns`.
Combined with the spillover above, checkpointer write CPU that leaks into the
following `CheckpointWriteDelay` span is now dropped from `CPU*` — exactly as
backend CPU before a `Client:ClientRead` already was. This is the ClientRead
precedent generalising, not a new behaviour, and it is pinned by
`tests/test_idle_accounting.c` section 5, which asserts the two events are
accounted identically. Fixing the spillover means changing how `cpu_ns` is
attributed across interval boundaries, which is a separate change.

### Summary files moved to v3

The per-second summary records precompute DB Time at WRITE time, so v1/v2 files
carry the OLD accounting. A directory holding both would blend two rules inside
one window and report a DB Time that is neither. `PGWT_SUMMARY_VERSION` is now
3; v3 excludes idle events at the writer (so no read path subtracts anything
back out); the reader refuses v1/v2 for computation; and `should_use_summaries()`
PREFLIGHTS the window (`pgwt_summaries_window_current`) so a refusal becomes a
raw recompute rather than a plausible-looking empty answer — the visitor skips
files it cannot open, which on its own would have produced a silently partial
window. Startup recovery still archives an intact older file instead of calling
it corrupt.

### Measured impact

| Quantity | Before | After |
|---|---|---|
| DB Time, 900 s enriched demo window | 2,153,447 ms | ~1,346,000 ms (−37.5%) |
| AAS, same window | ~2.39 | ~1.50 |
| `Timeout:CheckpointWriteDelay` alone | 807,549 ms = 0.897 AAS | moved to Idle |

The before/after pair is the pre-change measurement recorded in the task brief,
not a reading taken from this branch; the only number this branch measured is
that `Timeout:CheckpointWriteDelay` is 0.897 AAS of that window. The
`AAS_FLOOR_PROVISIONAL = 0.5` gate in `tests/demo_rehearsal_lib.py` is
deliberately NOT re-pinned here (the documented 1.15 is a separate follow-up,
`docs/DEMO_REHEARSAL_CRITERIA.md`).
