# When is wait-event sampling wrong? Measurements on gate-2, 2026-10-21

> Raw logs and trace rings for this directory (files over 1 MB and all `.gz`/`trace/`): [`sampler-accuracy-20261021-raw.tar.gz`](https://github.com/DmitryNFomin/pg_wait_tracer/releases/tag/evidence-20261023) in the `evidence-20261023` release. Untar it inside `tests/results/` to restore the full directory.

**Bottom line.** pg_wait_sampling at its 100 Hz default estimates *wait-label share* well on an ordinary loaded box,
even at 8x CPU oversubscription. It is materially wrong in two situations we could reproduce:

1. **Under a cgroup CPU quota** (the mechanism behind Kubernetes CPU limits) its collector, a background worker in the
   same cgroup, is throttled with the backends. With 4 busy sessions under a 0.5-CPU quota it reported 20-24% of the real
   session time (AAS 0.8-0.9 instead of 4.0) and mis-split wait vs CPU by up to 16 points. pg_wait_tracer, observing
   from outside the cgroup, reported ~4.0, and its exact tier split it into ~0.4 running + ~3.6 waiting for a CPU.
2. **For individual events.** Same total wait time with 598 vs 27,114 waits gives the same sample count; a wait of
   d < 10 ms leaves a sample with probability ~d/10 ms (measured).

A third effect is real but **not** sampling-specific: under CPU saturation the wait label stays set while the backend
waits in the run queue, so every wait-event view (sampled or an exact trace of `wait_event_info`) attributes CPU
queueing to the preceding wait. At 32 clients on 4 vCPU a victim was 65% runnable-but-queued while all views showed it
"75% sleeping".

We also **disproved** a claim we had planned to make: on this box neither a bare 100 Hz timer nor pg_wait_sampling's
collector runs late under (unthrottled) load.

Context: written for the talk "Beyond Sampling: True Wait Event Tracing with pg_wait_tracer" and for pg_wait_tracer
development. Experiments were designed with, and reviewed by, a second model (reviews/), then run by Claude Code.

---

## 1. Environment

| Item | Value |
|---|---|
| Host | Hetzner VM gate-2 (`pgwt-gate-2`), 4 vCPU AMD EPYC-Rome, 1 thread per core, 7 GB RAM |
| OS | Ubuntu 24.04.4, kernel 6.8.0-138-generic, cgroup v2 |
| PostgreSQL | 18 (PGDG), cluster `18/main`, port 5418, `shared_buffers` 128MB, `max_connections` 100 |
| Data | pgbench scale 10 (1,000,000 accounts), fits the OS page cache, not shared_buffers |
| Sampler under test | pg_wait_sampling 1.1.11 (`postgresql-18-pg-wait-sampling` 1.1.11-1.pgdg24.04+1), defaults: history_period = profile_period = 10 ms, sample_cpu on; `history_size` raised to 2,000,000 |
| Second sampler | a SQL `pg_stat_activity` poller (DO loop, `pg_stat_clear_snapshot()`, insert, `pg_sleep(0.01)`): the pattern of SQL-based ASH scripts |
| Tracer | `/root/pg_wait_tracer` on gate-2, build of 2026-10-14 (not a git checkout) |
| Other load | PG 13/16/17 clusters online but idle; `/tmp/pgwt-box-check.lock` held for the whole session |

The PG18 config was changed for the tests (pg_wait_sampling preloaded) and restored afterwards; the package was purged.

## 2. What "truth" means here

Three different quantities; the experiments say which one they use.

- **Wait-label occupancy**: how long `wait_event_info` held each value. Measured by victim sessions logging their own
  phases with `clock_timestamp()`, or by pg_wait_tracer's exact tier.
- **Kernel execution state**: on a CPU, runnable but queued (incl. cgroup throttling), or blocked. Measured from
  `/proc/<pid>/schedstat` (field 1 = on-CPU ns, field 2 = run-delay ns) before/after a window.
- **Session activity**: a session that is busy for the whole window counts 1 active session (AAS 1).

## 3. Experiments and results

All per-run outputs are in `console/`; raw files in `raw/`; scripts in `tools/`.

### E1. Does a normal-priority 100 Hz timer wake late under load?

`tools/ticklat.c`: SCHED_OTHER, nice 0, `clock_nanosleep(TIMER_ABSTIME)` every 10 ms for 60 s, lateness per tick.
Loads: idle, pgbench -S c4/c8/c16, TPC-B c8.

| Load | p50 | p99 | p99.9 | max | > 1 ms | missed |
|---|---|---|---|---|---|---|
| idle | 0.12 ms | 0.19 | 1.58 | 2.6 | 0.2% | 0 |
| -S c4 | 0.08 | 0.22 | 2.06 | 2.7 | 0.3% | 0 |
| -S c8 | 0.08 | 0.24 | 2.08 | 3.4 | 0.2% | 0 |
| -S c16 | 0.08 | 0.41 | 2.58 | 4.6 | 0.5% | 0 |
| TPC-B c8 | 0.08 | 0.23 | 1.50 | 2.8 | 0.2% | 0 |

**No.** Waking sleepers get a CPU quickly. 6,000 ticks per run: p99.9 rests on ~6 values.

### E2. Does pg_wait_sampling's collector keep 100 Hz under load?

Distinct `pg_wait_sampling_history.ts` values in a 60 s window = collector rounds.

| Load | loadavg | Rate | gap p50 / p99 / max |
|---|---|---|---|
| idle | 0.1 | 100.0 Hz | 10.13 / 10.21 / 12.9 ms |
| -S c16 | 13.9 | 100.0 Hz | 10.15 / 10.60 / 14.6 ms |
| TPC-B c16 | 8.6 | 100.0 Hz | 10.11 / 10.63 / 13.2 ms |

**Yes**, without a CPU quota. The ts is taken before the PGPROC scan, so this bounds round cadence, not per-PID read time.

### E5. Same total wait time, different event counts

Two sessions sleep for 30 s each: `pg_sleep(0.05)` loop vs `pg_sleep(0.0005)` loop (actual ~1.1 ms per call).
pg_wait_sampling_profile reset before.

| Run | Session | True waits | True wait time | Profile samples |
|---|---|---|---|---|
| idle | long | 598 | 30.00 s | 2,999 |
| idle | short | 27,114 | 29.95 s | 2,962 |
| TPC-B c8 | long | 598 | 30.02 s | 2,999 |
| TPC-B c8 | short | 25,932 | 29.94 s | 2,965 |

The share is right; the 45x difference in count (and 45x in duration) is invisible.

### E6. Probability that a single short wait leaves a sample

A holder takes advisory lock 42 for 0.2/1/2/5/10/20 ms (random), a victim acquires it in a loop and logs t0/t1.
"Seen" = a history row for the victim with event `advisory` and ts in [t0 - 1 ms, t1].
Waits within 1 ms of the previous wait are excluded so one sample cannot match two waits.

| Wait duration | idle: n / seen | TPC-B c8: n / seen | min(1, d/10 ms) |
|---|---|---|---|
| 0.05-0.5 ms | 300 / 2.3% | 295 / 3.7% | ~2% |
| 0.5-1 ms | 397 / 8.6% | 239 / 7.1% | ~8% |
| 1-2 ms | 507 / 18.5% | 524 / 14.3% | ~15% |
| 2-5 ms | 774 / 32.0% | 858 / 33.8% | ~34% |
| 5-10 ms | 739 / 77.0% | 748 / 76.1% | ~75% |
| 10-20 ms | 1044 / 99.9% | 949 / 99.8% | 100% |

The sampler behaves exactly as the periodic-sampling model predicts, loaded or not.

### E7. Known-truth victims vs two samplers, idle to 8x oversubscribed

4 victims loop: 1 ms busy loop, then `pg_sleep(0.001)`, 60 s, logging sleep time. Background pgbench -S c0..c32.
Samplers: pg_wait_sampling profile and the SQL poller. Kernel: victims' schedstat over 56 s.

| Background | Label truth: sleep % | pg_wait_sampling | SQL poller (active rows) | Kernel on-CPU | Kernel run-queue |
|---|---|---|---|---|---|
| none | 67.3 | 66.9 | 67.7 | 34% | 0.3% |
| c4 | 74.8 | 78.1 | 79.2 | 24% | 26.7% |
| c8 | 71.9 | 72.5 | 75.3 | 16% | 36.4% |
| c16 | 72.0 | 72.6 | 73.8 | 10% | 47.4% |
| c32 | 75.5 | 75.9 | 76.3 | 6.7% | 65.5% |

(Kernel % = share of 4 x 56 s victim wall.)

- Samplers track the label truth within 0.4-4.4 points. Saturation alone does not make them wrong.
- The label truth itself drifts from what the session does: at c32 the views say 75.5% sleeping / 24.5% CPU; the
  kernel says 6.7% on a CPU, 65.5% queued, ~28% blocked. After `pg_sleep` (or a lock, or an I/O) completes, the backend
  cannot clear `wait_event_info` until it runs, so queueing is labelled with the old wait; queueing with no wait set is
  labelled CPU. **This also fools an exact trace of `wait_event_info`**; it needs scheduler data.

### Q1. Pure-CPU sessions under a cgroup CPU quota

`systemctl set-property --runtime postgresql@18-main.service CPUQuota=…` (cgroup v2 `cpu.max`), 4 victims in a pure
busy loop (wait_event 0 throughout), 60 s. Truth: 240 session-s, AAS 4.0.

| Quota | Collector rate | gap p99 | Victim samples | x 10 ms | of truth | AAS reported | Kernel on-CPU / run-delay |
|---|---|---|---|---|---|---|---|
| none | 100.0 Hz | 11.8 ms | 24,000 | 240.0 s | 100% | 4.00 | 236.6 / 3.5 s |
| 1 CPU | 39.2 Hz | 87.8 ms | 9,400 | 94.0 s | 39% | 1.57 | 59.9 / 180.1 s |
| 0.5 CPU | 23.3 Hz | 98.0 ms | 5,592 | 55.9 s | 23% | 0.93 | 29.9 / 210.1 s |
| 0.25 CPU | 19.9 Hz | 98.9 ms | 4,768 | 47.7 s | 20% | 0.79 | 14.9 / 225.1 s |

Throttled time shows up in schedstat run-delay. Every sample still says "CPU" (correct label); the volume is wrong.
Normalising by actual rounds would recover the volume here (continuous state), but tools convert samples x period.

### Q2/Q3. Mixed sessions under a quota: volume and split

E7 victims (1 ms CPU + 1 ms sleep) x4, 60 s, plus the SQL poller; 3 repetitions per quota (Q3), one earlier run (Q2).

| Quota | pg_wait_sampling time vs truth | Sleep %: label truth / pg_wait_sampling / SQL poller | Poller rate |
|---|---|---|---|
| none (x3) | 100% | 67.2 / 67.2 / 67.6 | 97.6 Hz |
| 1 CPU (x3) | 82% | 71.3 / 66.4 / 68.4 | 74 Hz |
| 0.5 CPU (x3) | 42-52% | 77.4 / 67.3 / 72.3 | 43 Hz |
| Q2 single runs | 81% (1 CPU), 42% (0.5 CPU) | 72.9/67.9, 77.1/64.5 | n/a |

Under a quota both samplers are throttled and look mostly when the cgroup is running, so they over-weight CPU:
pg_wait_sampling's sleep share is 3-8 points low at 1 CPU and 6-16 points low at 0.5 CPU (per run).

### Q4. Same quota, observed by pg_wait_tracer from outside the cgroup (3 repetitions)

0.5-CPU quota, 4 pure-CPU victims, 60 s; pg_wait_tracer run from a root ssh session (cgroup `user.slice`), once
`--mode sampled --sample-rate 100`, then `--mode full`; pg_wait_sampling read for the same pids each time.

| Rep | pg_wait_sampling (victims) | pg_wait_tracer sampled: CPU* | pg_wait_tracer exact: CPU* + Off-CPU* |
|---|---|---|---|
| 1 | 4,900 / 4,952 samples = 49-50 s | 244.8 s | 23.6 + 219.1 = 242.7 s |
| 2 | 4,912 / 4,972 = 49-50 s | 244.4 s | 27.4 + 215.2 = 242.6 s |
| 3 | 5,654 / 5,519 = 55-57 s | 244.7 s | 18.4 + 224.1 = 242.5 s |

Truth 240 s. pg_wait_sampling: AAS 0.8-0.9, all CPU. pg_wait_tracer: AAS ~4.0; the exact tier's Off-CPU*
(time with no wait event while not on a CPU, from its sched_switch accounting) matches the kernel's throttled/queued time.

**Product finding:** pg_wait_tracer reported `Extension:Extension` ~60 s of DB time in every Q4 run. That is the
pg_wait_sampling collector's latch wait (a background worker idling), which should probably be classed as idle.

## 4. What this supports, and what it does not

Supported:
- Sampling cannot recover event counts, durations or the evidence for one short wait (E5, E6), by its nature.
- Under a CPU quota, an in-cgroup sampler (pg_wait_sampling, SQL pollers) under-reports load by up to ~5x and skews
  the wait/CPU split (Q1-Q4). An observer outside the cgroup does not (Q4).
- Under CPU saturation, wait labels absorb run-queue time for every wait-event view (E7).

Not supported (do not claim):
- "Samplers run late under load" without a quota (E1, E2). The earlier kernel-traced issue115 run found a small
  selection bias (-3.4 pp with 8 backends on 4 vCPU, -0.4 pp with 2): real, small.
- "Exact tracing of wait_event_info fixes the run-queue labelling" (E7): it needs scheduler data.
- Any claim about AWS Performance Insights' internals; nothing here measured it.

## 5. Threats to validity

- One 4-vCPU VM, no SMT; cloud host noise uncontrolled. Repetitions only for Q3 (x3) and Q4 (x3).
- E7/Q2/Q3 truth = the victim's own `clock_timestamp()` phases (wait-label occupancy), not kernel state.
- pg_wait_sampling history ts precedes the per-PID reads; E6 uses a 1 ms slack window.
- The quota experiments use an aggregate cgroup quota on the whole cluster, like a container limit; per-pod details differ.
- The SQL poller and pg_wait_sampling both add load; tracer overhead was not measured here.
- Per-session rows for E5/E7/Q2/Q3 were in tables that were dropped during cleanup; their printed summaries in
  `console/` are the only record.

## 6. How to reproduce

On a disposable or locked box (gate-2 convention: hold `/tmp/pgwt-box-check.lock` with `flock` for the whole session).

1. Copy `tools/box/*` to a working dir; `tools/e1.sh` + `tools/ticklat.c` for E1 (expects `/root/sampler-e1`).
2. Install pg_wait_sampling and preload it. **Pitfall:** `ALTER SYSTEM SET shared_preload_libraries = '…,…'` through
   nested shell quoting produced one quoted list element and PG18 refused to start; edit `postgresql.auto.conf` or
   use a SQL file. Also raise `pg_wait_sampling.history_size` (default 5,000 rows covers ~3 s at 16 backends x 100 Hz).
3. Run `e2.sh`, `e5.sh`, `e6.sh` + `e6_stats.py`, `e7.sh` + `e7_report.sql` + `e7_sched.py`, `q1.sh` + `q1_stats.py`,
   `q2.sh`, `q3.sh`, `q4.sh`. Scripts assume `/root/sampler-e2`, PG18 on port 5418, `pgbench_accounts` populated.
4. psql does not substitute `:'var'` inside `$$` bodies; the SQL files pass parameters via `set_config()`.
5. A DO-block poller must call `pg_stat_clear_snapshot()` each iteration, or it re-reads one cached snapshot.
6. Quota cleanup: `systemctl set-property --runtime postgresql@18-main.service CPUQuota=` and verify
   `systemctl show -p CPUQuotaPerSecUSec` = infinity (reading `cpu.max` right after may still show the old value).
7. Restore: original `postgresql.auto.conf` (saved as `raw/sampler-e2/postgresql.auto.conf.orig`), restart, purge the
   package, drop the test tables, release the lock (kill the holder's `sleep` child too).

## 7. Files

| Path | Contents |
|---|---|
| `tools/ticklat.c`, `tools/e1.sh` | E1 timer-lateness program and driver |
| `tools/box/` | E2, E5, E6, E7, Q1-Q4 scripts, SQL and analysis |
| `raw/sampler-e1/lat-*.txt` | E1 per-tick lateness (ns), 6,000 lines per load |
| `raw/sampler-e2/rounds-*.txt` | E2 collector round timestamps |
| `raw/sampler-e2/e6-*.csv` | E6 per-wait duration (ms) and seen flag |
| `raw/sampler-e2/sched-*-c*.txt` | E7 victims' schedstat start/end |
| `raw/sampler-e2/q-hist-*.csv`, `q-sched*`, `q-pids-*`, `q-win-*` | Q1 history rows, schedstat, pids, windows |
| `raw/sampler-e2/q4-*.txt` | Q4 repetition 3 tracer and pg_wait_sampling outputs |
| `raw/sampler-e2/pgb-*.txt` | pgbench outputs |
| `console/` | printed summaries of every run (only record for E5, E7, Q2, Q3, Q4 reps 1-2) |
| `reviews/round1_*` | first review of the talk's sampling argument, draft test plan, sources (second model) |
| `reviews/round2_*` | load-dependent failure mechanisms and experiment designs (second model) |

Related earlier results: `../issue115_arbiter/EVIDENCE_BIAS.txt` (kernel-traced scheduling bias of a
pg_stat_activity poller), `../sampler-294/` (pg_wait_tracer's own sampler vs its exact tier).

## 8. External references (opened and checked during the review)

- pg_wait_sampling 1.1.11 collector source: https://github.com/postgrespro/pg_wait_sampling/blob/v1.1.11/collector.c
- Kernel CFS bandwidth control: https://docs.kernel.org/scheduler/sched-bwc.html
- Container throttling case study (Dan Luu): https://danluu.com/cgroup-throttling/
- Zalando postgres-operator, CPU limits and throttling: https://github.com/zalando/postgres-operator/issues/2959
- pg_stat_activity state/wait inconsistency report (pgsql-hackers, 2022): https://www.postgresql.org/message-id/ab1c0a7d-e789-5ef5-1180-42708ac6fe2d@postgrespro.ru
- Frequency vs duration discussion (pgsql-hackers, 2025): https://www.postgresql.org/message-id/aIIeX7p2cKUO6KTa@ip-10-97-1-34.eu-west-3.compute.internal
- ASH vs 10046 trace in practice: https://piotrsajda.com/oracle-blog/temp-tablespace-on-flash/
- Microsoft: brief spikes can leave no sampled record: https://learn.microsoft.com/en-us/azure/postgresql/troubleshoot/concepts-troubleshooting-guides#limitations
