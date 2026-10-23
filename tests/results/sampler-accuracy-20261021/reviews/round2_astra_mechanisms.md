**Your results do not support “samplers become badly inaccurate under ordinary CPU saturation.”** They support a narrower conclusion: 100 Hz sampling estimates aggregate wait-label occupancy quite well, even when most individual waits are missed.

The strongest remaining candidates are:

1. **CPU quota throttling that also stops the collector.**
2. **CPU queueing attributed to the preceding wait, including `ClientRead`.**
3. **Combining `state` and `wait_event` observations taken at different times.**

Only #1 and #3 offer a sampling-specific advantage for tracing. **#2 also fools an exact trace of `wait_event_info`.**

I inspected PostgreSQL 18 and pg_wait_sampling v1.1.11 source and opened the reports linked below. I did not modify files or run experiments on your server. Numerical examples below are predictions, not additional measurements.

**First, define the truth being measured.** These are three different quantities:

- **Label occupancy:** how long `wait_event_info` held each value.
- **Kernel execution state:** running, runnable but unscheduled, blocked, or throttled.
- **Request latency:** from a request reaching the server until its response, including time before PostgreSQL marks it `active`.

An exact wait-field trace measures the first. It needs scheduler and request-boundary information to measure the others.

Your existing numbers already make that distinction important. Running plus run-queue time was **329 backend-seconds**, whereas traced `CPU*` was **258 seconds**. If those cover identical PIDs, windows, and **ungated** lifetimes, at least **71 seconds of running/runnable time occurred under nonzero wait labels**. If the trace excludes time outside a command, that exclusion can also explain the difference. Either way, subtracting `177 s` of execution from `258 s` of `CPU*` does **not** recover the measured `152 s` of run-queue delay.

### Mechanisms, ranked conditionally

There is no defensible universal magnitude ranking without knowing quotas, statement durations, and collection windows. This ordering puts the largest credible failures under their stated triggering conditions first. On your **unthrottled** VM, start with #2 and #3.

| Rank and mechanism | Tools affected; plausible magnitude | Does an exact wait-field trace fix it? | Reproducible comparison |
|---|---|---|---|
| **1. Collector shares an exhausted CPU quota** | pg_wait_sampling, pgsentinel, and SQL sampler backends inside the limited cgroup. **Tens of percent to most nominal samples can disappear.** A client outside the cgroup does not free its SQL backend from the quota. Separately, throttled backends often retain a null wait label, misleadingly displayed as CPU. | **Missing observations: yes**, with lossless transition capture and correct open-interval accounting. **Calling throttled time execution: no.** | Four continuously active CPU workers; `cpu.max = 100000 100000`; compare a 60 s interval, collector rounds, profile counts, scheduler runtime, and `cpu.stat`. Detailed experiment below. |
| **2. A completed resource wait retains its label while the backend queues for CPU** | All wait-label tools. The error is approximately the accumulated runnable time under those labels. Can dominate short I/O or lock waits. For `ClientRead`, request queueing may disappear from active-session DB time entirely. | **No.** The backend cannot clear the field until scheduled. | Wake a sleeping victim using a complete client request or release its advisory lock while its CPU is overloaded. Join wakeup/switch-in events to wait transitions. |
| **3. `state` and wait are read at different times** | `pg_stat_activity` pollers, including pgsentinel. Potentially **large relative errors in CPU/wait composition** for submillisecond statements and millisecond collection delays. Total active-session count need not be biased. Raw pg_wait_sampling profiles avoid this particular join. | **Yes for wait occupancy.** Correct active-state breakdown additionally requires state transitions. | Short-query workload, many backend slots/connections; compare sampled joint states with a simultaneous state-plus-wait timeline. |
| **4. Correlated short incidents** | Especially 1–10 Hz tools. A synchronized 100 ms stall can be **entirely missed at 1 Hz**, regardless of the number of affected sessions. At 100 Hz, analogous exposure is at millisecond scale. | **Yes**, while tracing is active. | Block 100 sessions on a shared advisory lock for 100 ms once. Truth is approximately 10 backend-seconds; repeat with randomized incident phase. |
| **5. Query attribution crosses a statement boundary** | SQL pollers combine an older query/state snapshot with a newer wait. pg_wait_sampling separately reads its query-ID slot and wait field. Error can be large **for a particular short SQL**, even if aggregate event totals are correct. | **Not automatically.** Wait transitions alone do not delimit statements; several statements can execute without changing the wait field. | Alternate two structurally different statements, A without intentional waits and B with `pg_sleep(0.002)`. Trace statement boundaries and waits; count B’s waits attributed to A or query ID zero. |
| **6. Periodic activity aliases with periodic sampling** | All fixed-period samplers. **Near-total undercount or several-fold overcount** is mathematically possible while phases remain aligned. Expected error in ordinary asynchronous OLTP is much smaller; sustained alignment must be demonstrated. | **Yes for label occupancy.** | Issue a 2 ms sleep every 10 ms using an absolute client schedule; sweep phase and compare 7/10/11 ms sampling. Then test actual vacuum delays against PG18 delay timing. |
| **7. Observation changes the workload** | pg_wait_sampling holds shared `ProcArrayLock` while scanning and updating its profile; SQL monitoring adds snapshots, status copying, allocations, and possibly blocking-PID lookups. Usually modest; potentially material near a contention threshold. No justified generic percentage. | **No.** Tracing also perturbs execution and lock-hold durations. | Randomized runs with no monitoring, pg_wait_sampling, one SQL poller, and several pollers. Compare throughput, client p99, CPU, and lock waits using independent measurements. |
| **8. Active-state filtering excludes real work or relevant blockers** | `WHERE state='active'` pollers omit startup/read/protocol intervals and idle transaction holders. Relative omission can be substantial for tiny statements. However, excluding genuinely idle time is correct for an explicitly active-state metric. | **Only with a broader definition and additional lifecycle data.** | Compare the exact `active` integral with server-received request intervals. Separately, leave a transaction idle while holding a row lock and observe its victims. |
| **9. Uninstrumented blocking is called CPU** | All wait-label tools. Potentially dominant in the affected workload. **PG18 `commit_delay` is a concrete example**, not merely an aliasing candidate. | **No.** There is no corresponding transition to capture. | Single writer, `commit_siblings=0`, `commit_delay=10000`, synchronous commits; compare against delay zero using `pg_usleep` probes and scheduler runtime. |
| **10. PI raw sampled load mistaken for scaled load** | Consumers of `db.sampledload.avg`. Above AWS’s internal threshold, raw counts can materially understate load. Correct scaling is not itself evidence of bias. Rare SQL attribution remains noisy. | A local trace avoids session subsampling, but does not establish AWS’s implementation details. | On RDS, hold known numbers of sessions continuously active; compare `db.load.avg`, `db.sampledload.avg`, and known concurrency across a staircase of loads. |
| **11. History overwrite and coarse aggregation** | pg_wait_sampling history and monitoring retention pipelines. Can lose **all evidence of an incident**. Profile totals can survive while timing disappears. Aggregation can flatten peaks without changing total time. | Only if raw trace intervals are retained. | Default 5,000-entry history at 100 sampled PIDs and 100 Hz holds about **0.5 s**. Compare complete trace/profile totals with history fetched every second. |
| **12. Ordinary scheduler-dependent observation bias** | Userspace samplers sharing busy CPUs. Your evidence supports **a few percentage points**, not a catastrophic failure. Direction is workload dependent. | Transition tracing avoids selecting observation instants, but introduces its own overhead. | Repeat your scheduler-state comparison at actual field-read instants, with randomized phases and sampler affinity controls. |

Several implementation details sharpen those distinctions:

- PostgreSQL sets `ClientRead` around `WaitEventSetWait()` and clears the wait only after returning through backend code. Socket readiness and first subsequent execution therefore need separate timestamps. This supports #2 directly. [PG18 socket-read code](https://github.com/postgres/postgres/blob/REL_18_STABLE/src/backend/libpq/be-secure.c), [wait-event implementation](https://github.com/postgres/postgres/blob/REL_18_STABLE/src/backend/storage/ipc/waiteventset.c).
- PG18 copies backend-status information under its change-counter protocol, then obtains the live wait field separately. **This is a temporal mismatch, not arbitrary byte tearing of every column.** State and query text are not simply unrelated reads. [Backend-status snapshot code](https://github.com/postgres/postgres/blob/REL_18_STABLE/src/backend/utils/activity/backend_status.c), [activity-view implementation](https://github.com/postgres/postgres/blob/REL_18_STABLE/src/backend/utils/adt/pgstatfuncs.c).
- pg_wait_sampling v1.1.11 deliberately skips missed sampling opportunities. Its timestamp is taken before acquiring `ProcArrayLock` and scanning processes; it is **not each PID’s actual read timestamp**. Consequently, round-gap measurements do not bound every observation’s timestamp error. [v1.1.11 collector](https://github.com/postgrespro/pg_wait_sampling/blob/v1.1.11/collector.c).
- pg_wait_sampling does not require `state='active'`; pgsentinel’s default collection query does. Do not generalize an active-filter failure to pg_wait_sampling’s raw profile. [pg_wait_sampling inclusion predicate](https://github.com/postgrespro/pg_wait_sampling/blob/v1.1.11/pg_wait_sampling.c), [pgsentinel collection queries](https://github.com/pgsentinel/pgsentinel/blob/master/src/pgsentinel.c).

**A useful mathematical model for #3:** suppose a backend alternates between active/CPU and idle/ClientRead, with active fraction \(p\). If the state snapshot and later wait read become effectively independent:

\[
P(\text{reported active and CPU})=p^2
\]

\[
P(\text{reported active and ClientRead})=p(1-p)
\]

The true active CPU occupancy is \(p\). At \(p=0.2\), the sampled active rows would appear **80% ClientRead**, despite the idealized workload having no ClientRead during active execution. Total reported active occupancy remains \(p\).

This is a limiting example, not a prediction for your box. It illustrates how **the diagnosis can change while AAS stays approximately correct**. Your 23% inactive/null rows alone cannot determine the size of this effect.

**For short incidents, backend count does not create independent samples.** One common 100 ms incident affecting 100 sessions has about a 90% miss probability under an ideal 1 Hz common-phase sampler. If hit, a naive count-times-period estimate gives 100 backend-seconds; if missed, zero; truth is 10. Across randomized repetitions the mean can be correct while any individual incident is badly represented. At 100 Hz, that same incident should be measured well.

**For periodic maintenance, establish actual phase locking.** `vacuum_cost_delay`, bgwriter/walwriter sleeps, and application polling are not automatically synchronized to the collector. Work between sleeps and scheduling jitter can destroy alignment. PG18 provides `track_cost_delay_timing` and `delay_time` for a useful independent vacuum comparison. Background-worker idle waits also should not be mistaken for foreground DB time. [PG18 progress reporting](https://www.postgresql.org/docs/18/progress-reporting.html).

`commit_delay` deserves a separate control: PG18 calls `pg_usleep(CommitDelay)` while holding `WALWriteLock`, without setting a wait event around that sleep. The leader can therefore look like CPU while followers show WALWrite contention. That changes the fix toward the delay setting, but **an exact wait-field trace has the same blind spot**. [PG18 WAL-flush source](https://github.com/postgres/postgres/blob/REL_18_STABLE/src/backend/access/transam/xlog.c).

**Experiment 1: stop the sampler with a real CPU quota.**

Use a dedicated test cluster whose postmaster, workers, collector, and SQL monitoring backends all belong to one cgroup. Keep the controlling client and trace consumer outside that cgroup.

1. Establish four persistent sessions, record their PIDs, disable parallel query, and run this CPU-only statement in each:

   ```sql
   SET statement_timeout = '180s';

   DO $$
   DECLARE
       x integer := 0;
   BEGIN
       LOOP
           x := (x + 1) % 1000000;
       END LOOP;
   END
   $$;
   ```

   Start tracing before execution. Measure a 60 s interior window after all four loops have started. Cancel afterwards. This avoids `generate_series` plans that might spill or perform unintended I/O.

2. Run these conditions, at least five times each:

   | Condition | cgroup `cpu.max` |
   |---|---|
   | Control | `max 100000` |
   | One CPU of aggregate quota | `100000 100000` |
   | Half a CPU of aggregate quota | `50000 100000` |

   Keep all four vCPUs available. Verify membership through `/proc/<pid>/cgroup`; verify parent cgroups impose no additional cap. CPU bandwidth quotas permit parallel consumption followed by throttling until replenishment. [Kernel bandwidth documentation](https://docs.kernel.org/scheduler/sched-bwc.html).

3. Collect:

   - Monotonic window boundaries and exact wait intervals.
   - `/proc/<pid>/schedstat` before/after for each victim.
   - Cgroup `cpu.stat`: `usage_usec`, `nr_periods`, `nr_throttled`, `throttled_usec`.
   - pg_wait_sampling profile deltas and sufficiently retained history.
   - SQL-poller request-start/completion timestamps and returned rows.
   - Trace loss counters and initial/final states.

   Do **not** treat `throttled_usec` as summed backend-seconds or simply add it to task run-queue time. Cgroup throttling accounting and per-task accounting have different scopes. [Cgroup v2 interface](https://docs.kernel.org/admin-guide/cgroup-v2.html).

4. Compare three estimates separately:

   \[
   T_{\mathrm{nominal}}=0.010\times\text{profile samples}
   \]

   \[
   T_{\mathrm{rate\ corrected}}=
   \frac{\text{profile samples}}{\text{actual rounds}}\times60
   \]

   \[
   T_{\mathrm{trace}}=\int\#\{\text{four victims with wait field zero}\}\,dt
   \]

   Sum only the four victim PIDs.

**Predicted result:** all four remain active with predominantly null wait fields, so the trace should approach **240 backend-seconds**. With one CPU of quota, their combined execution should approach **60 CPU-seconds**, slightly less because other cluster processes consume quota.

Ideally, four busy CPUs consume the 100 ms CPU budget in about 25 ms and then stop for roughly 75 ms. Actual per-CPU quota distribution changes the details. Expect long collector gaps and a potentially much lower achieved rate; do not promise a particular Hz value.

If nominal sample-to-time conversion reports much less than 240 seconds, you have demonstrated a real accounting failure. **If normalization by actual rounds repairs it, say so.** The continuous-state workload allows that repair; it does not prove composition bias.

Then repeat with pgbench and a victim alternating CPU work and sleeps. Compare the sampled composition with exact label occupancy. This second stage tests whether observations preferentially fall in particular quota phases.

Finally, move **only the collector** outside the limiting hierarchy, keeping the database workers constrained, and repeat. Recovery of its rate isolates collector throttling from backend behavior.

The practical fix changes from “there is spare CPU / optimize the apparent SQL CPU consumer” to **inspect quota, concurrency, and CPU placement**. Exact wait tracing identifies missing wall time; scheduler/cgroup evidence identifies why it was missing.

**Experiment 2: prove that ClientRead contains server CPU queueing.**

Use one persistent victim session over a local Unix socket. Pin its backend to CPU 0. Pin the client and observation processes to other CPUs.

1. The client repeatedly:
   - Waits 100 ms after receiving the previous response.
   - Records a monotonic send timestamp.
   - Sends one complete simple-protocol `SELECT 1`.
   - Records response completion.
   
   Run 1,000 requests per condition. The pause makes it likely that the backend genuinely entered socket wait before the next request. Confirm that from the trace.

2. Compare:
   - No competing work on CPU 0.
   - Eight CPU burners pinned to CPU 0, all ordinary scheduling priority.
   - The same contention with the victim at nice +19.

   The last condition amplifies the mechanism; present it explicitly as an adverse scheduling configuration, not representative pgbench behavior.

3. Capture:
   - `sched:sched_wakeup` for the victim.
   - `sched:sched_switch`, including runnable switch-outs and subsequent switch-ins.
   - Exact wait-field transitions.
   - Request send/receive timestamps.
   - Optionally an `exec_simple_query` entry probe.
   - Victim schedstat deltas as a consistency check.

   `perf sched` or bpftrace can provide the scheduler events. Record monotonic timestamps and event-loss counters. Track migrations and preemption, not only wakeups.

For each request, identify:

```text
complete request sent
    → backend wakes runnable, still ClientRead
    → backend first runs
    → backend clears ClientRead
    → SQL executes
    → response received
```

Calculate:

\[
T_{\mathrm{runnable,ClientRead}}
=\int 1[\text{victim runnable and label ClientRead}]\,dt
\]

Restrict this calculation to the relevant request intervals. Otherwise the deliberate 100 ms client pauses swamp the result.

**Predicted result:** as CPU contention increases, some server-side request delay appears in `ClientRead`; it remains outside `state='active'` until the backend advances through command handling. A sampler and an exact wait trace can agree perfectly while both mislead someone who interprets that time as the application being slow to send.

The decisive control is to relieve CPU contention without changing the client or socket. Queueing and latency should fall together.

Repeat with a single advisory-lock waiter and an uncontended holder on another CPU. Hold the lock for 100 ms, release it, and measure the waiter’s wakeup-to-run interval. A lock wait can include scheduling after release. Use a single waiter to avoid confusing scheduler delay with another waiter obtaining the lock first.

**This is a strong “wait labels can change the diagnosis” demonstration. It is not evidence that sampling is inferior to tracing.**

**Experiment 3: quantify false CPU-to-ClientRead attribution in `pg_stat_activity`.**

Start with the published reproduction shape, then use your PG18 binary:

```sh
pgbench -n -S -M prepared -c 200 -j 4 -T 120 bench
```

Run separate cluster configurations with `max_connections` around 300, 2,000, and—if resources permit—10,000. Warm the dataset. Keep actual workload concurrency fixed while changing the configured backend-slot count. Also reproduce the original report’s `-j 1` condition, but do not assume its client can saturate your server.

Collect at 1, 10, and optionally 49 Hz, using autocommit:

```sql
SELECT pid, state, wait_event_type, wait_event, query_id, query_start
FROM pg_stat_activity
WHERE backend_type = 'client backend'
  AND pid <> pg_backend_pid();
```

**Retain all rows**, then apply `state='active'` offline. Record collection start/end timestamps externally. Do not assign every row the precision implied by a single `now()`.

The reference measurement needs:

- Exact wait transitions.
- Backend activity-state transitions.
- Statement boundaries for a selected subset of victims.
- Scheduler state if you also want actual execution time.

For activity state, probe `pgstat_report_activity` entry and return, keeping the call interval as boundary uncertainty. Exclude ambiguous boundary intervals or report upper/lower bounds. A hardware watchpoint on the state field is another option. **A wait-only trace cannot certify the joint state/wait truth.**

Compare these integrals with their sampled equivalents:

\[
T_A=\int 1[\text{state active}]dt
\]

\[
T_{A,0}=\int 1[\text{active and wait field zero}]dt
\]

\[
T_{A,CR}=\int 1[\text{active and ClientRead}]dt
\]

Also compare unfiltered pg_wait_sampling event occupancy with the unfiltered wait trace. That is the control distinguishing a SQL-view composition problem from generic sampling error.

**Predicted result if the mechanism is material:**

- Total sampled active occupancy may remain close to \(T_A\).
- Sampled active/null occupancy falls below \(T_{A,0}\).
- Sampled active/ClientRead rises above \(T_{A,CR}\).
- The distortion grows as status-copy-to-wait-read delay becomes large relative to statement turnover.
- Raw pg_wait_sampling event totals remain much closer to the exact event totals.

Repeat with statements lasting roughly 50 ms. Attribution should improve when statement duration greatly exceeds the read skew.

Do not classify every active/ClientRead row as impossible. Extended-protocol exchanges and COPY can legitimately produce that combination. The reference trace must establish the excess; the row combination alone is insufficient.

For all three experiments, use common PID populations and windows, preserve open intervals, report drops, and measure tracing overhead in separate control runs. Your own evidence describes material full-trace overhead in high-transition workloads, so tracing is a reference for **the observed run**, not automatically for the uninstrumented workload.

**For the 10-minute talk, I would choose #3 if the PG18 experiment confirms a material effect.** It has the best combination of an understandable failure, a changed diagnosis, and a published PostgreSQL reproduction:

> “The monitor reported ClientRead. It combined the backend’s earlier active state with its later wait state. We were investigating the client using a state combination that overstated client waiting.”

The qualification belongs on the slide: **this concerns pg_stat_activity-based collection; pg_wait_sampling’s raw profile avoids that particular problem.**

For a story specifically against **pg_wait_sampling 100 Hz**, #1 is the best next experiment. It supplies the missing condition your ordinary-load tests did not exercise: **the collector is forbidden to run for much of each quota period**. I did not find an opened report establishing that exact pg_wait_sampling failure, so treat it as a source-supported experimental hypothesis until measured.

These are the most relevant reports I actually opened:

- **Michael Zhilin, PostgreSQL hackers, 2022:** reports inconsistent wait/state observations under high connection settings, supplies a pgbench reproduction with `max_connections=10000`, and explains the separated reads. This directly supports #3. Its blanket assertion that active/ClientRead cannot occur is too broad; do not repeat that part. [Original report and reproduction](https://www.postgresql.org/message-id/ab1c0a7d-e789-5ef5-1180-42708ac6fe2d@postgrespro.ru).
- **Heikki Linnakangas, PostgreSQL hackers, February 2026:** discusses the same inherent inconsistency and proposed improvements. This is implementation evidence, not a claim that your PG18 installation contains those changes. [Follow-up discussion](https://www.postgresql.org/message-id/ad3852ed-daaa-43d7-a572-0e465666d1bb%40iki.fi).
- **Dan Luu and David Mackey’s Twitter case study:** documents quota-driven latency problems despite low average CPU utilization, and substantial improvements from reducing excessive concurrency. It supports the real-world relevance and changed fix for throttling, **not PostgreSQL sampler bias specifically**. [The container throttling problem](https://danluu.com/cgroup-throttling/).
- **Zalando postgres-operator production-user report:** users object to enforced CPU limits because of throttling. PostgreSQL-specific deployment evidence, but no quantified sampling comparison. [Issue #2959](https://github.com/zalando/postgres-operator/issues/2959).
- **asyncpg user report:** describes active/ClientRead caused by a gap between Parse/Describe/Flush and Bind/Execute/Sync under concurrency. This is a valuable counterexample preventing an overclaim in the torn-row story. [Issue #1276](https://github.com/MagicStack/asyncpg/issues/1276).

AWS documents one-second session sampling and separately documents scaled versus raw sampled load. Neither document justifies claiming that `db.load.avg` is systematically low merely because it uses sampling. [Database-load calculation](https://docs.aws.amazon.com/AmazonRDS/latest/UserGuide/USER_PerfInsights.Overview.ActiveSessions.html), [load scaling semantics](https://docs.aws.amazon.com/AmazonRDS/latest/APIReference/API_PerformanceInsightsMetricQuery.html).

Your existing **598 long waits versus 27,114 short waits** experiment is still excellent talk material: **sampling got total wait time right, while losing the event-count and duration distribution needed to distinguish occasional stalls from pervasive contention.** That is already a defensible reason to trace. It does not require claiming an aggregate sampling error your measurements disproved.