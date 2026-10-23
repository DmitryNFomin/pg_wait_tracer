# CPU limit vs. wait profile, and the lock tail re-tested (gate-2, 2026-10-21)

> Raw logs and trace rings for this directory (files over 1 MB and all `.gz`/`trace/`): [`cpu-limit-and-locktail-20261021-raw.tar.gz`](https://github.com/DmitryNFomin/pg_wait_tracer/releases/tag/evidence-20261023) in the `evidence-20261023` release. Untar it inside `tests/results/` to restore the full directory.

Two experiments meant to find a real case where tracing finds the root cause and sampling does not.
Plain SQL workloads, no `pg_sleep`, no injected delays. Single runs per configuration (not replicated).

**Short version.**

- **Case B (CPU limit + periodic report):** this is a real case where the wait profile misleads. A 1-CPU cgroup limit
  at 72% average use plus a parallel report every ~5 s raises p99 from 4.6 to 317 ms. pg_wait_sampling blames
  Lock / WAL and shows no CPU-starvation signal. The fix suggested by the profile (`synchronous_commit = off`) leaves
  p99 at 68 ms, and afterwards the sampler sees only a third of the active sessions. The kernel shows the cause
  directly: run-delay. **pg_wait_tracer does not find it today.** The exact tier's own cost, charged to the same cgroup,
  collapses the workload, and the sampled tier shows the same labels as pg_wait_sampling.
- **Case A (lock tail behind a batch job, controlled re-run):** not a tracer case. Ordinary tools find it (the per-query
  sampler profile, `log_lock_waits` with a 10 ms `deadlock_timeout`, `pg_blocking_pids()`). At a fixed 600 tx/s, the
  exact tier's own latency tail (no batch job: 219 requests > 20 ms, p99.9 72 ms) is larger than the problem
  (90 requests > 20 ms, p99.9 33 ms). Run-to-run I/O noise on this VM is of the same size as the lock tail.

## Setup

- **Box:** Hetzner gate-2: 4 vCPU AMD EPYC-Rome, 7 GB RAM, Ubuntu 24.04, kernel 6.8.
- **PostgreSQL 18.6, port 5418:**
  - `shared_buffers` 128MB, `io_method = worker` (default)
  - `track_io_timing = on`, `log_lock_waits = on`
  - `pg_stat_statements`
  - pg_wait_sampling 1.1.11 at defaults (10 ms periods, `profile_queries = top`, `sample_cpu = on`)
- **Data:** pgbench scale 10.
- **CPU limit:** `systemctl set-property --runtime postgresql@18-main.service CPUQuota=…`, i.e. cgroup v2 `cpu.max`
  with a 100 ms period, as a Kubernetes CPU limit does. pgbench runs outside that cgroup.
- **pg_wait_tracer:** build of 2026-10-14 (`/root/pg_wait_tracer`). The daemon runs outside the cgroup, but the exact
  tier's watchpoint and BPF work runs in the backends, so it is charged to the PostgreSQL cgroup.
- **Ground truth:**
  - `/proc/<pid>/schedstat` per client backend: on-CPU ns and run-delay ns; run-delay includes throttled time.
  - Cgroup `cpu.stat`: `usage_usec`, `nr_periods`, `nr_throttled`.
  - pgbench per-transaction log. With `-R`, latency is counted from the scheduled start, i.e. what a client sees.

## Case B: CPU limit with a periodic parallel report

**Workload:**
- Requests: pgbench TPC-B (built-in), 16 clients, `-M prepared`, fixed Poisson rate of 1,400 tx/s.
- Report: `report.sql`, a `GROUP BY bid` over `pgbench_accounts` with 2 parallel workers, 1 client, `-R 0.2`. It takes
  ~120 ms on 3 processes (~0.4 CPU-s per run).
- Each run lasts 80 s; the measured window is 60 s.
- Steady OLTP alone at 72–85% of a 1-CPU limit is almost never throttled (`b-cal-*`: 0–2% of periods). The bursts
  come from the report.

| 60 s window | B0 no limit | **B1 1-CPU limit** | B5 limit + `synchronous_commit=off` | B6 limit + report without parallel workers | B7 2-CPU limit |
|---|---|---|---|---|---|
| CPU used (cgroup) | 0.76 cores | **0.72 cores (72% of limit)** | 0.65 | 0.72 | 0.73 |
| Periods throttled | 0% | 14% | 15% | 11% | 3% |
| Request p50 / p99 / p99.9 | 1.23 / 4.6 / 8.8 ms | **1.32 / 317 / 490 ms** | 0.77 / 68 / 88 ms | 1.30 / 134 / 244 ms | 1.24 / 22 / 54 ms |
| Kernel: sessions on a CPU / waiting for a CPU | 0.66 / 0.13 | **0.66 / 0.93** | 0.57 / **1.57** | 0.67 / 0.80 | 0.66 / 0.28 |
| pg_wait_sampling active sessions (samples × 10 ms) | 1.37 | 2.00 | **0.72** | 1.75 | 1.42 |
| pg_wait_sampling top events | CPU 38, WalSync 34, WALWrite 19, Lock:tx 7 | **CPU 28, WalSync 24, WALWrite 23, Lock:tx 20** | **CPU 82**, Lock:tx 12 | CPU 32, WalSync 28, WALWrite 22, Lock:tx 16 | CPU 36, WalSync 33, WALWrite 20, Lock:tx 9 |
| pg_wait_sampling samples/s per backend | 100.2 | 95.7 | 93.2 | 96.8 | 99.5 |
| Report median | ~120 ms | 569 ms | 593 ms | 590 ms | 202 ms |

**What a DBA sees with the sampler (B1):**
- Lock:transactionid triples (7% → 20%), and WAL waits stay at ~45%.
- Nothing says "CPU".
- The CPU graph says 72% of the limit, so CPU looks fine.

**The fix the profile suggests (B5).**
- The WAL waits point to `synchronous_commit = off`, which gives up durability. It helps (p99 317 → 68 ms) but does not
  remove the stall.
- After it, the profile says the database is nearly idle and CPU-bound: 0.72 active sessions, 82% CPU.
- The kernel says 0.57 sessions were on a CPU and **1.57 were waiting for one**.
- Most of that waiting is probably backends that a new query has woken but that have not yet run, still labelled
  `Client:ClientRead`. Any wait-event observer counts that as idle, so the stall leaves DB Time entirely.
- That attribution is inferred from the totals (sampler AAS vs. kernel on-CPU + run-delay), not measured per sample.

**What actually helps:**
- Raising the limit (B7) brings p99 from 317 to 22 ms.
- Removing the report's parallel burst (B6) only halves p99.

**Real-world references for this pattern:** CFS-quota throttling at average utilisation well under the limit. See Dan
Luu, "The container throttling problem" (danluu.com/cgroup-throttling/), and Kubernetes issue #67577 on CFS quota
throttling.

### pg_wait_tracer in case B

| Run | What happened |
|---|---|
| **B2** (`--mode full`, 1-CPU limit) | The workload collapsed. Throttled in 99% of periods, CPU 0.99 cores, throughput 880/s against an offered 1,400, latency p50 10 s. Unusable as a diagnosis. |
| B3 (`--mode full`, no limit) | Cgroup CPU 0.76 → **1.41 cores** at the same request rate (+85%). The help text claims 6–30% overhead. |
| **B4** (`--mode sampled --sample-rate 100`, 1-CPU limit) | No measurable cost (CPU 0.74, same latency as B1). But the same labels as pg_wait_sampling: Lock 23%, WALWrite 17%, WalSync 13%, CPU* 17%, plus Extension:Extension 26% (pg_wait_sampling's collector, counted as DB Time). No CPU-starvation signal. |

The exact tier measures on-CPU time per interval, and Off-CPU* is the non-running remainder of CPU-labelled gaps. Time
spent runnable *inside* a wait label (wake-up to running, including throttled time) is not split out. Nor is a woken
backend that is still labelled ClientRead. Also, `--replay … --view time_model` of B2 shows CPU* 22.4% with no Off-CPU*
row, while the live `system_event` shows CPU* 4.3% (measured): replay falls back to gap inference.

**What would make this a tracer case.** Read `/proc/<pid>/schedstat` (run-delay) per backend on every sampled-tier
tick. That is outside the cgroup and costs PostgreSQL nothing. Attribute the delta to the label held, and report it as
"waiting for CPU", including the ClientRead-labelled part as DB Time. In the exact tier, read
`current->sched_info.run_delay` at each transition, as is already done for `cpu_ns`. With that, B1 would read: of 2.0
active sessions, ~0.9 were waiting for a CPU. That is the root cause, and pg_wait_sampling cannot report it: it reads
only the 4-byte label, and its collector is throttled with the backends.

## Case A: lock tail behind a batch job, controlled re-run

- `req.sql` and `batch.sql` are as in `../lock-tail-20261021`. Requests: 8 clients at a fixed Poisson 600 tx/s.
- Batch: 1 client, Poisson mean 1/s, a 20,001-row UPDATE in one transaction.
- `poll.sql` polls `pg_blocking_pids()` every 5 ms (`\watch 0.005`).

| 60 s window | A0 no batch | **A1 batch** | A2 batch + tracer + poller | A3 no batch + tracer | A4 batch + poller + `deadlock_timeout` 10ms | A6 batch in 1,000-row chunks |
|---|---|---|---|---|---|---|
| Request p50 / p99 / p99.9 | 2.1 / 6.7 / 13.1 ms | **1.9 / 8.2 / 32.6 ms** | 3.5 / 16.9 / 61.9 | 3.4 / 12.7 / 72.2 | 2.6 / 9.1 / 26.2 | 2.9 / 11.1 / 62.3 |
| Requests > 20 ms | 6 (0.017%) | **90 (0.25%)** | 259 | **219** | 61 | 155 |
| Lock:tx, global profile | 0% | 0.67% | 0.59% | 0% | 0.71% | 0% |
| Lock:tx, request UPDATE's own samples (profile_queries) | 0% | **19.6% (2nd)** | 24.3% | 0% | 26.9% | 0% |
| pgss request UPDATE max | 1.0 ms | 44.8 ms | 80.9 | 2.0 | 57.0 | 2.8 |
| `log_lock_waits` lines | 0 | 0 | 0 | 0 | **30 (with blocker pid)** | 0 |
| `pg_blocking_pids()` blockers | — | — | batch 137/137 | — | batch 123/125 | — |
| IO:AioIoCompletion share | 18.7% | 5.1% | 32.4% | 31.4% | 25.6% | 22.5% |

**Findings:**
- **The batch job does create a tail (A0 → A1)**, and the sampler shows it once you look at the statement
  (Lock 19.6% of the UPDATE's samples).
- `deadlock_timeout = 10ms` makes `log_lock_waits` name the blocker. A `pg_blocking_pids()` poller attributes 98–100% of
  the waits to the batch session. These are ordinary, cheap tools.
- **pg_wait_tracer's exact tier adds a bigger tail than the problem (A3 vs. A1):** without the batch job, tracing alone
  gives 219 requests > 20 ms and p99.9 72 ms. AioIoCompletion rises to ~31%.
- **Chunking the batch (A6) removes the lock waits** (UPDATE max 2.8 ms) but not the request tail. The tail in A6 comes
  from something else. AioIoCompletion and checkpoint activity vary from run to run (5–31%), and single runs cannot
  separate a 0.25% tail from that noise.
- **Conclusion:** keep this as "sampling found the contention; per-execution tracing explains individual outliers", not
  as "the sampler could not find it". It is not presentable while the exact tier adds more tail than it explains.

## Product issues found (pg_wait_tracer build 2026-10-14)

1. **Exact-tier cost.** B3: +0.65 cores (+85%) at 1,400 TPC-B tx/s. A3: p99.9 13 → 72 ms at 600 tx/s. This is charged to
   the monitored cgroup, so under a CPU limit it can cause the very throttling being diagnosed (B2). The help text says
   6–30%.
2. **Runnable time is not split out of wait labels,** and neither is a woken backend still labelled ClientRead. This is
   the gap that makes case B invisible to the tracer too.
3. **The sampled tier has no task-state or run-delay signal,** although it already runs outside the cgroup.
4. **`Extension:Extension`** (pg_wait_sampling collector latch) is counted as DB Time (B4: 26%).
5. **Replay vs. live CPU*:** B2 replay time_model CPU* 22.4% with no Off-CPU*, live system_event CPU* 4.3%.

## Files (`raw/`)

| Path | Contents |
|---|---|
| `runb.sh`, `repb.py`, `report.sql` | case B harness (`REPORT_RATE`, `REPORT_PGOPTIONS` env), report |
| `b-B0` … `b-B7`, `b-cal*` | per run: `pgb.txt`, `lat-all.txt.gz`, `rep-all.txt` (report runs), `pgws-all.csv`, `pgws-by-query.csv`, `pgss.csv`, `sched0/1`, `cg0/1`, `cpu.max`, `client-pids`, `t0/t1`, tracer output and `trace/` for B2/B3/B4 |
| `runa.sh`, `repa.py`, `req.sql`, `batch.sql`, `batch_chunked.sql`, `poll.sql` | case A harness (`POLL=1`, `DLT=10ms` env), report |
| `a-A0` … `a-A6` | per run: `pgb.txt`, `pgb-batch.txt`, `lat-all.txt.gz`, `bat-all.txt`, `pgws.csv`, `pgws-by-query.csv`, `pgss.csv`, `blockers.txt`, `pglog.txt`, tracer `trace/` for A2/A3 |
| `postgresql.auto.conf.orig` | PG18 config before the test (restored) |

Reproduce: `python3 repb.py b-B0 b-B1 …` and `python3 repa.py a-A0 a-A1 …` in `raw/`.

Gate-2 was restored afterwards: pg_wait_sampling removed, config restored, CPU limit cleared, lock released.
