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
**PG14/15/16 are DELIBERATELY NOT CLASSIFIED.** `select_version_tables()` has
exact Timeout enums for PG13 and PG17/18 only; 14/15/16 fall through to the
PG17/18 table as a best-effort *display* fallback, and `src/discovery.c` only
queries `pg_wait_events` on 17+, so nothing rescues them from the live side
either. Deriving the mask from that fallback would turn a cosmetic mislabel
into a silent wrong answer — a Timeout id that is really `PgSleep` resolving to
`CheckpointWriteDelay` and so leaving DB Time, the exact inversion of the
decision above, on a major the live tier runs (PG16).

So an unverified major installs an **empty mask**: every Timeout event stays in
DB Time. That is the over-count direction `src/idle_rule.h` names as fail-safe —
too much load on screen is visible and arguable, too little is neither. Two
advisers disagreed about which of 14/15/16 are actually affected; rather than
ship a table nobody could verify, the code was written not to depend on the
answer.

**A missing or corrupt sidecar does not fall back to PG18.** The trace file
header already records the major, so `pgwt_init_event_names_for_trace()` tries,
in order: the sidecar (authoritative — it carries both the major and the name
provenance), then the header's major (dynamic names are lost, but the
version-selected static tables are right, so the classification survives), then
the unknown-major state (PG18 tables for rendering, **empty** pacing mask).
Keeping the PG18 display default on sidecar failure classified a PG13 or PG16
trace with PG18's Timeout ids — reinstating the live/offline disagreement for
the one case that has no `names_source` key to read. `src/replay.c` **refuses**
a trace whose header records no major at all, which is the same situation, so
the two agree rather than each guessing. The daemon also now warns when its
sidecar write fails, because that is what produces such a trace.

A PG14–16 trace whose sidecar carries **names PostgreSQL actually reported** IS
classified: those ids came from the server, so a mask derived from them is
right by construction. That is the rescue path, and it is why this is a
fail-safe default rather than a permanent downgrade.

**The sidecar records its own provenance** (`names_source`: `observed` vs
`static`), and that is load-bearing rather than decorative. PG16 has no
`pg_wait_events`, so the daemon loads no dynamic names — but it still *writes*
a sidecar, and `pgwt_write_names_json` falls back to the active hardcoded
tables, which for PG16 are PG18's. Without provenance `pgwt-server` loaded that
file as "dynamic names", concluded they came from PostgreSQL, and derived a
PG18 mask for a PG16 trace: the fail-safe defeated by an ordinary capture, with
the daemon's live path (empty mask) and the offline path (PG18 mask)
disagreeing about the same data. A sidecar with no `names_source` key is read
as `static`, the safe assumption.

A **partial** observed list is handled too: on an unverified major only the ids
the server actually reported are classified, because `pgwt_event_name()` falls
back to the static table for any gap — which would have reintroduced the same
defect one id at a time.

Note the uninitialised case: `pg_version` defaults to 18 so that names *render*
rather than showing as Unknown, but the pacing mask starts **empty**. Reusing
the display default for classification made "nobody has said which PostgreSQL
this is" silently equivalent to "this is PG18".

Verified: **PG13, PG17, PG18** — exactly the majors this repo carries an exact
Timeout enum for. PG17's status is derivative rather than independent:
`select_version_tables` gives 17 and 18 the **same** `timeout_events[]` table
(only `io_events` differs), so "17 is verified" means "17's Timeout enum equals
18's". That is true in the PostgreSQL source and is now **pinned by a test** —
`tests/test_wait_event.c` asserts both majors derive the same mask and that
every Timeout id 0–11 resolves to the same name on each, so a future edit that
split them fails loudly instead of quietly making one of the two wrong. Unclassified by design: **PG14, PG15, PG16, and every future
major** (PG19+), unless their trace supplies observed names. The predicate was
briefly `>= 17`, which quietly promised that every future major was verified
and would have classified a PG19 trace against PG18's table whenever its name
load failed.

`tests/test_wait_event.c` pins all of it: the empty mask per unverified major,
the real PG16 capture→sidecar→server round trip, the partial-observed-list
case, and that the guard does not blanket-disable `Client:ClientRead` or
`Activity`.

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

The children always account for the parent. What the named rows cannot
cover — hidden `Activity` parking, plus anything the summary path's bounded
per-event tables dropped — is emitted as a single labelled
`Other idle (parked background loops)` row rather than left as an unexplained gap. An
unexplained gap in a visible total is the thing this change exists to remove;
leaving one at the next level down would have reintroduced it.

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

### The totals are exact; only the breakdown is bounded

v3 moved idle time out of `class_ns`, which made the per-event tables the only
per-event source of it — and both are bounded: `queries[].top_events[]` holds 8
entries and `events[]` holds 1024. Reading the Idle TOTAL out of either one
reintroduces, one level down, the bug v3 removed: a query with 8 busier
non-idle events would report `Idle = 0` beside a correct DB Time, so DB Time +
Idle would stop accounting for the window under a query filter.

Both totals therefore come from **exact scalars accumulated at the writer**
(`pgwt_summary_accum::idle_ns`, `pgwt_summary_query::idle_ns`). The bounded
lists only drive the NAMED breakdown, and whatever they miss lands in
`Other idle (parked background loops)`. `events[]` additionally reports when it could not take an
entry (`events_overflow`), because a full table still truncates the per-event
breakdown and any class/event-FILTERED total while unfiltered DB Time stays
right — a mismatch that previously had no signal at all.

**The self-checks are surfaced, not merely recorded.** A counter nothing reads
is a counter nobody acts on, which is how three of these were found to be
invisible in review:

| signal | where it appears |
|---|---|
| `idle_children_excess_ms` | the `time_model` response, plus an `ERROR` line naming the path |
| `summary_flush_failures_total` | the control socket's **`metrics`** reply (seconds that are HOLES in every summaries-path window) |
| `summary_events_overflow_total` | the control socket's **`metrics`** reply, plus one `WARN` per process. Counts WRITTEN seconds only — a second lost to a failed rotation or close is discarded without being rolled up, so read it with `summary_flush_failures_total`, not as a complete total |

All three must be 0. The two writer counters are in `metrics`, **not** `status`
(`build_status()` does not embed `build_metrics()`), and
`tests/test_state_map_loud.py` asserts both keys are present in the `metrics`
reply so that "surfaced" is a gated claim rather than a sentence in this file. `idle_children_excess_ms` is compared **even when the Idle
parent is zero** — a zero parent with non-zero named children is the canonical
shape of the writer and the reader disagreeing about the rule (a record written
with an empty mask, read with PG18's), and returning early on a zero parent made
the one scenario the counter exists for the one it could not see. ~270 distinct
PostgreSQL wait events against 1024 slots is headroom, not a proof, and it is
not a proof at all for `Extension` events.

### Summary files moved to v5

The per-second summary records precompute DB Time at WRITE time, so v1/v2 files
carry the OLD accounting. A directory holding both would blend two rules inside
one window and report a DB Time that is neither. `PGWT_SUMMARY_VERSION` is now
**5**; the reader refuses every earlier version for computation; and
`should_use_summaries()` PREFLIGHTS the window
(`pgwt_summaries_window_current`) so a refusal becomes a raw recompute rather
than a plausible-looking empty answer — the visitor skips files it cannot
open, which on its own would have produced a silently partial window.

What each bump changed, since the version number is a statement about the
accounting rule and not about the byte layout:

- **v3/v4** exclude idle events at the writer (so no read path subtracts
  anything back out) and carry the exact idle scalars.
- **v5** (#317) also files event 0 — CPU — into `queries[].top_events[]`,
  which `accum_query_add`'s `old_ev != 0` guard had been dropping. A v4 second
  and a v5 second answer "what is in this query's event list" differently, so
  a window holding both would report a per-query event breakdown that is
  neither: the Events tab drilled into one query would show a CPU* row
  carrying only part of the window. Layout is byte-identical to v4; the bump
  exists for the rule, not for the bytes.

- **v6** (#315, #376) excludes io_worker records at the writer, from all five
  accumulators — `class_ns`, `idle_ns`, `events[]` (histograms included),
  `sessions[]` and `queries[]` — so the exclusion described above finally
  holds on the summary path too. Layout is byte-identical to v5; the bump
  exists for the rule.

  **The writer is TOLD, it does not guess.** `pgwt_summary_push_event` takes a
  `cat_flag` resolved by the producer: `src/event_stream.c` hoists
  `pgwt_live_pid_cat_flag` above the push, and `src/escalation.c` resolves in
  `flush_open_intervals` (its capture-end flush is a second producer that
  never passes through the event-stream callback — and it is where the
  divergence peaked, because an io_worker spends the whole capture in one
  `Activity:IoWorkerMain` wait whose only record is the one that flush
  synthesizes). A check on `evt->flags` would have been dead in the daemon:
  nothing on the live path sets `PGWT_EVENT_FLAG_IO_WORKER` (hole N7 in
  `tests/test_agg_raw_crosscheck.c`), which is why the writer reads the
  argument and never the flag.

  `pgwt_compute_heatmap` gained the matching raw-side exclusion in the same
  commit: `events[]` carries the histograms the summary heatmap is built from
  and `handle_heatmap` routes both ways, so excluding at the writer alone
  would have moved the divergence to the latency grid instead of removing it.

  Measured on `tests/results/demo_rehearsal/trace` (PG18, `io_method=worker`,
  1851 s, 3 io_workers), summary minus raw on Idle: whole capture
  **+5,549,649.64 ms (+23.36%) → -3,484.06 ms (-0.0147%)**; a 150 s window
  containing the capture end **+5,768,521.72 ms (+308.16%) → +215,388.03 ms
  (+11.51%)**. The capture-end remainder is NOT io_worker time: it is the
  general straddler mechanism (the writer buckets a long interval whole into
  the second in which it closes, while the raw path clips it to the window —
  a long `Client:ClientRead` behaves identically), unchanged by v6 and
  unchanged at +2.43% one second earlier. The summary path still reports no
  io_worker BUSY total (`cat_ms[]` / `io_worker_busy_pct` are raw-path-only,
  hole N1), as it did before v6.

  **Residual, measured not assumed:** `pgwt_live_pid_cat_flag` returns 0
  (foreground) while a pid's metadata is unparsed, so an io_worker's earliest
  records can still be written as foreground, while the raw path tags them
  post hoc from `backends.jsonl`. On this capture that residual is **0 ms of
  5,553,133.69 ms** (no io_worker record closes in the capture's first
  second). The writer counts what it excluded
  (`summary_io_worker_events_excluded_total` /
  `summary_io_worker_ns_excluded_total` on the control socket) so the residual
  stays observable in production rather than assumed.

- **v7** (#408) stores the MEASURED-CPU SPLIT in each per-second record, and
  resolves the command gate before the push rather than after it. Unlike v5
  and v6 this one DID change the layout — the record grew a trailer, so a
  reader expecting the pre-v7 shape mis-parses a v7 file rather than merely
  disagreeing with it. See the `v7` notes in `src/summary_writer.h`.

- **v8** (#379) spreads a long interval across the seconds it covers instead
  of charging the whole duration to the second in which it closes. A v7
  second holds the whole duration and a v8 second holds one second of it, so
  a window mixing the two double-counts the overlap. Layout is byte-identical
  to v7; the bump exists for the rule. The writer never infers an open
  interval from the record stream — it credits only what a producer
  explicitly noted, and only while that note is fresh
  (`PGWT_SUMMARY_OPEN_STALE_NS`), so a producer that notes nothing degrades
  exactly to v7. Three residuals (a long on-CPU gap, up-to-one-tick
  attribution at an interval boundary, and a second that completed before the
  pid had a note) are listed and COUNTED in `src/summary_writer.h`.

- **v9** (#424) — **DESIGNED AND WRITTEN, NOT SHIPPED.** `PGWT_SUMMARY_VERSION` is **8**; v9 is what the next bump will mean. It makes an OPEN on-CPU interval creditable per second. v8
  deliberately refused that case: a per-second credit has no `cpu_ns` of its
  own, so spreading a CPU gap's wall while its measurement stayed whole at
  close would have put everything into `cpu_clamped_ns` and nothing into
  `cpu_measured_ns`, destroying v7's split. v9 credits it only when the
  PRODUCER supplies the measurement, and widens `cpu_measured_records` from
  "records" to "CPU observations" so that a window whose only CPU time is
  one still-open gap still reports `has_measured_cpu` — without that widening
  the reader would ignore the split it had just been given and emit one
  combined CPU* row, which is the v8 symptom one layer up. Layout is
  byte-identical to v8; the bump exists for the rule.

  **WHY THE CONSTANT IS 8 AND NOT 9.** With no producer, v9 credits
  nothing, so its only semantic difference from v8 (the widened
  `cpu_measured_records`) never materialises: a build shipping 9 today
  would write a v8 file wearing a 9. The version exists to stop a window
  blending two accounting rules, and `pgwt_summaries_window_current`
  compares only the number — so shipping 9 now and wiring the producer
  later without a further bump makes **9 name two different rules**, with
  no reader able to tell those files apart. Two resolutions were sound:
  ship 9 and require the re-wiring commit to bump to 10, or hold at 8 and
  let that commit set 9. **We hold at 8**, because merging 9 permanently
  forecloses the other option and leaves correctness depending on a future
  author reading this paragraph. The producer was reverted; the constant
  follows it back. The commit that wires the producer sets this to 9 —
  `tests/test_v9_open_cpu_wired.sh` is its acceptance test and
  `tests/test_docs_summary_version.sh` will drag these docs along.

  **Claimed is not wired.** `summary_open_cpu_credited_ns` on the control
  socket is the check: on a capture with an in-flight CPU statement it must
  be non-zero. Measured on gate-2 at integration HEAD, watchpoints attached,
  three CPU statements spanning 10 s of ticks and `cpu_ns_total` moving
  0 → 23,715,122,379, all three of `summary_open_cpu_credited_ns`,
  `summary_open_cpu_refused_total` and `summary_cpu_open_uncredited_ns_total`
  read **0** — a writer that is never asked, so the accounting silently falls
  back to v8. `tests/test_v9_open_cpu_wired.sh` is that check, in the live
  tier, and it is RED until the producer is wired.

This list is kept in step with `PGWT_SUMMARY_VERSION` in
`src/summary_writer.h`, which is **8** as of 2026-10-16 — the v9 entry above
describes the next bump, not this build. It stopped at v6 for
two bumps; a version list that trails the constant is worse than no list,
because it reads as "nothing has changed since" — and it misled once more
after that, when the v7/v8 entries were added against a tree that had since
become v9.

Startup recovery still archives an intact older file instead of calling it
corrupt.

**Why two bumps.** v3 introduced the writer-side exclusion; v4 added the exact
per-query and per-record idle scalars. Those added bytes changed the on-disk
layout, and v3 files had already been written — so keeping the number would
have left two layouts under one version, mis-parsing in the exact
silently-partial shape the preflight exists to prevent. The version is the
cheap half of that guarantee: "an old file is refused" is strictly stronger
than "no such file should exist".

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

---

## Addendum, 2026-10-25 (#439): background DB Time in Sessions vs Overview — no decision needed, the model already answered

Status: **no semantic change.** This records what the existing model says, the
measurement that confirms it, and the one thing that was actually wrong.

#439 reported that Sessions shows `bgwriter`/`walwriter`/`checkpointer`/
`autovacuum` rows carrying DB Time while Overview puts "Other (background)"
outside DB Time, and asked which tab is right. **Neither is wrong.** The two
rows are two different quantities that share three English words:

| | what it is | inside DB Time? |
|---|---|---|
| Sessions' `DB Time` for a bgwriter pid | that process's non-idle time | **yes** |
| Overview's `background` category (`cat_ms[PGWT_CAT_BG]`) | the same time, system-wide | **yes** |
| Overview's Idle child `Other (background)` | hidden `Activity`-class **parking** (`src/compute.c:1283`) | **no**, in either tab |

The decomposed model at the top of this file already settled the first two:
"background — checkpointer / bgwriter / walwriter / … activity" and
"maintenance — autovacuum worker activity" are **categories of AAS**, i.e. of
DB Time. `pgwt_event_category` (`src/compute.c:737`) implements exactly that,
and the only thing it moves *outside* DB Time is `PGWT_CAT_IO_WORKER`.

**Measured** (local `pgwt-server`, `tests/results/demo_rehearsal/trace`, PG18,
1851 s, 4.3M events, raw recompute):

```
time_model           db_time_ms = 2495784.951108
sum of top_sessions  db_time_ms = 2495784.951108     diff 0.000000000
categories: planning 3.238513 + execution 998509.276421
          + command 1493681.800708 + maintenance 825.638179
          + background 2764.997287                 = 2495784.951108
Idle child "Other (background)"                    = 7757772.931164  (not DB Time)
```

The two totals are bit-identical because `pgwt_compute_top_sessions`
(`src/compute.c:1911`) and `pgwt_compute_time_model` (`src/compute.c:1328`)
apply the **same three predicates** to the same events — `pgwt_filter_matches`,
the `pgwt_is_idle_event` skip, the `PGWT_EVENT_FLAG_IO_WORKER` skip — and clip
with the same `event_window_ns`. Sessions is a *decomposition* of Overview's DB
Time, not a second total. Conservation is therefore untouched by this issue:
nothing moved.

**Scope of the exactness claim: the RAW path.** The measurement above is a raw
recompute (the retained capture's summary files predate v8, so the server
refused them). On the SUMMARY path the per-second `sessions[]` table is bounded
at `SUMMARY_MAX_SESSIONS` (1024) and `find_or_insert_session`
(`src/summary_writer.c:143`) returns NULL when it is full; both the writer
(`:503`) and the reader (`:789`) then skip that pid's time while `class_ns` —
and therefore DB Time — still carries it. So a window with more than 1024
distinct pids in one second can report Σ sessions < DB Time there, with **no
overflow counter** (unlike `events_overflow`). Not changed here, and not what
#439 was about; recorded so nobody reads "bit-identical" as a cross-path
guarantee. The disclosure text deliberately does not claim the rows sum to
Overview's total for the same reason — the Sessions tab can also be truncated
client-side (`model.truncation`).

**What was wrong was the disclosure, not the accounting.** The label half was
first patched in #481, as a display-only remap of the raw row name to
"Background idle (parked)". That remap is **gone**: #439's follow-up fixed the
name at the SOURCE instead (`src/compute.c` now emits "Other idle (parked
background loops)" directly) and deleted the client-side rename, so Overview
renders the server's string as-is like every other row. The row name quoted in
the table above, and in the measured dump below, is the pre-rename one this
addendum was written against.
The Sessions half, added in #494, asserted a scope difference that does not
exist — *"Overview uses a different, system-wide DB Time total that excludes
the parked-idle time of background processes"* — naming as the differentiator
something Sessions excludes identically, and telling a reader that two numbers
agreeing to the nanosecond cannot be reconciled. The note and the column
tooltip now say what is true, and the sentence is **derived** from
`DB_TIME_MEMBERSHIP` in `web/static/lib/builders/table-configs.js` so a future
change to what is inside DB Time cannot leave the user-visible text behind.

**Still open, and not addressed here:** the CLI/sampler half recorded in
#439's second comment (`Extension:Extension` from the `pg_wait_sampling`
collector counted as client DB Time in the sampler-vs-tracer comparison
views). That is a `backend_type` classification gap outside `pgwt-server`'s
own paths; `pgwt-server` classifies it correctly via
`bm_type_to_cat_flag` (`src/server.c:1035`).

---

## Addendum (#382): the Latency heatmap keeps the io_worker exclusion

**Decision: the io_worker AAS/DB-Time exclusion (v6, above) is NOT extended
into a separate "should the Latency heatmap also exclude io_worker latency"
question — it already does, and that is staying.** `pgwt_compute_heatmap`
drops `PGWT_EVENT_FLAG_IO_WORKER` records (`src/compute.c:2316-2322`) for a
reason independent of the AAS decision: summary v6 drops io_worker records
at the writer entirely (no bytes to read back), so the raw path has to match
or `handle_heatmap`'s two routes (raw vs. summary) would disagree cell for
cell past the window length where the reader switches (hole N7,
`tests/test_agg_raw_crosscheck.c`). Re-including io_worker latency in the
heatmap would need a summary format bump (v6 would have to start keeping
what it now discards) plus new `src/` plumbing, for **zero viewer-visible
change**: the requesting backend's own `AioIoCompletion` wait already carries
the latency that query actually experienced (`src/compute.c:1768`,
`src/anomaly.c:53`), so every real query-facing latency is already in the
grid.

**AAS exclusion does not logically require latency exclusion** — they are
different questions. AAS/DB Time answers "how many sessions are doing
chargeable work"; a latency heatmap answers "how long did waits of this kind
take". An io_worker's own wait durations are a legitimate answer to the
second question even though double-counting its busy time into DB Time is
wrong for the first. The two decisions happen to agree here (both exclude)
but for unrelated reasons, and only because of the v6 writer-format
constraint above — not because the heatmap's exclusion was independently
re-derived and found correct.

**Capability gap, recorded rather than hidden:** an I/O-heavy capture using
`io_method=worker` loses the physical io_worker's OWN read/write latency
distribution from the heatmap — e.g. a worker queueing behind a slow device
while every requesting backend's `AioIoCompletion` wait looks uniform. The
backend-side wait still shows total latency including queueing, but cannot
distinguish "this backend queued" from "the device is slow", which the
io_worker's own per-operation latency could. Tracked as a separate feature
request (io_worker latency as its own heatmap series or overlay) rather than
folded into #382, since it needs the summary format bump above and is useful
independently of whether #382's exclusion ever changes.
