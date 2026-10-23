# Cases 4 and 5 on the wait-event timing patch (gate-3, 2026-10-22)

> Raw logs and trace rings for this directory (files over 1 MB and all `.gz`/`trace/`): [`case4-case5-patch-20261022-raw.tar.gz`](https://github.com/DmitryNFomin/pg_wait_tracer/releases/tag/evidence-20261023) in the `evidence-20261023` release. Untar it inside `tests/results/` to restore the full directory.

Cases 4 and 5 of the talk were measured with pg_wait_sampling and pg_wait_tracer only. This directory repeats both on
PostgreSQL 20devel with the wait-event timing patch, so that all five cases have a patch result.

**Short version.**

- **Case 4 (slow disk or bloat): the patch tells them apart, like the tracer.** pg_wait_sampling shows twins
  (DataFileRead 96.6% vs 94.7%). The patch shows 1.7 reads per lookup at ~1.35 ms each (slow disk) vs 29 reads per
  lookup at ~81 µs each (bloat), per execution, including the ones cancelled by `statement_timeout`. Throughput with
  `capture = trace` was the same as with `capture = off` (timeouts: see Cost below).
- **Case 5 (CPU limit): the patch does not find it, as the talk already says.** At a 1.3-CPU limit (~72% average use,
  7–11% of periods throttled), p99 goes from 5 ms to 131–172 ms. The kernel says ~0.9 sessions are waiting for a CPU.
  The patch shows Lock:transactionid twice as slow (avg 0.87 → 1.7 ms) and blames row locks for the slowest 1% of
  transactions. Nothing in its output says "CPU". It times the same labels as the sampler, from inside the backend, so
  run-queue time is inside the wait durations.

Plain SQL, a real disk, no `pg_sleep`, no injected delays. Single runs per configuration, except the 1.3-CPU runs
(2 with the patch capturing, 2 without).

## Setup

- **Box:** Hetzner pgwt-gate-3 (re-created from snapshot `pgwt-gate-2-20261022`), cpx32: 4 vCPU AMD EPYC-Genoa (KVM),
  8 GB, one virtual disk.
- **Server:** PostgreSQL 20devel, branch `wet-v12` of github.com/DmitryNFomin/postgres, tip `f8abf1f` ("trace
  level"; the branch was re-pushed since `ae41909b85`, same v12 SQL API including `phase` in `by_statement()`).
  Built with meson, release, no cassert. Port 5419, run as the transient unit `pgwet.service` so cgroup limits apply.
  - `shared_buffers` 128MB, `io_method = worker` (default), `track_io_timing = on`, `compute_query_id = on`
  - `shared_preload_libraries = pg_stat_statements, pg_wait_sampling, pg_wait_event_tracing`
  - pg_wait_sampling 1.1.11 (master) at defaults
  - `pg_wait_event_tracing.trace_ring_size` 4MB (default); `capture` set per database (`off` or `trace`)
- **Observer sessions** (ring dumps, stats queries) run with `PGOPTIONS=-c pg_wait_event_tracing.capture=off`.
  Without that, each dump session got its own ring, left it orphaned on exit (by design: flight recorder), and the
  first smoke test hit the 1200M memory limit (OOM kill). `pg_stat_clear_orphaned_wait_event_rings()` runs before
  every run.
- PG18 on the box was stopped during the runs. pg_wait_tracer was not run (its case 4 and 5 results are in
  `../case3-repro-20261021`, `../truncation-and-cancelled-20261022` and `../sampler-accuracy-20261021`).

## Case 4: slow disk or bloat (`c4.sh`, `c4batch.sh`, `bloat.sh`, `client.py`, `c4rep.py`)

Same workload as test 2 in `../truncation-and-cancelled-20261022`:
- Table `c3.t`, 12 M rows, 2.8 GB heap, 257 MB index. `MemoryMax=1200M` on the unit, caches dropped before each run.
- `client.py`: 8 psycopg workers, closed loop, `SELECT filler FROM t WHERE id = $1`. `statement_timeout` 0 or 10 ms.
- **Slow disk:** fio randrw 64k, iodepth 32, 4 jobs on the same disk; ids over 12 M.
- **Bloat:** an idle REPEATABLE READ session plus 30 rounds of `UPDATE … WHERE id <= 200000` (table 4.1 GB, 6 M dead
  tuples); ids over 1..200,000.
- With `capture = trace` every client ring was dumped every 5 s and deduplicated on (pid, seq); no records were lost
  to ring wrap. An execution is ExecStart → TxnCommit (completed) or TxnAbort (cancelled).

| 60 s window | Slow disk, off | Slow disk, trace | Bloat, off | Bloat, trace |
|---|---|---|---|---|
| Lookups/s, no timeout | 2,992 | 3,054 | 2,884 | 2,955 |
| Timeouts at 10 ms (off / trace) | 124 (0.07%) | 111 (0.06%) | 7 (0.00%) | 93 (0.05%) |
| **pg_wait_sampling** | **DataFileRead 96.7%**, CPU 3.3% | 96.5% | **DataFileRead 94.6%**, CPU 5.2% | 94.7% |
| pgss blocks read / call | 1.7 | 1.7 | 29.1 | 29.2 |
| pg_stat_io avg read | 1.39 ms | 1.35 ms | 0.085 ms | 0.084 ms |
| **Patch: reads per execution** | — | **1.7** | — | **29.2** |
| **Patch: avg read** | — | **1,350 µs** | — | **81 µs** |
| Patch: DataFileRead histogram | — | 48% < 16 µs (OS cache), 48% at 1–4 ms | — | 59% < 4 µs, 36% at 128–512 µs |
| Patch: timed-out executions (10 ms run) | — | 111: 1.9 reads each, avg 7.5 ms | — | 93: 31.7 reads each, avg 427 µs |
| Patch: slowest 500 executions | — | 1.9 reads, avg 4.6–5.0 ms | — | 31.8 reads, avg 301–305 µs |

Numbers in the "trace" columns are from the no-timeout run unless marked; the 10 ms runs agree to within 1–2%.

**Findings:**
- The patch separates the two causes from SQL, per execution, the same way pg_wait_tracer did on PG18 (1.7 reads at
  1.96 ms vs 29 reads at 117 µs there; this box's disk is faster under fio).
- It also covers the cancelled executions, which pg_stat_statements never sees. As in the tracer runs, here they look
  like the completed ones, just with slower reads (slow disk) or more reads (bloat).
- **Cost:** throughput unchanged (trace vs off: +2% in both scenarios). Timeouts: slow disk 111 vs 124, bloat 93 vs
  7. The bloat difference is 0.05% vs 0.004% of requests in single runs with the 5-second dump loop running; it may be
  the patch, the dumps or noise, and was not separated. pg_wait_tracer's exact tier raised timeouts 4× (1.1% → 5.0%)
  and 8× (0.14% → 1.56%) in the same test on PG18.
- **As before, this is the weakest "only the trace can tell" case:** pgss blocks/call and pg_stat_io avg read tell
  the same story for completed calls.

## Case 5: CPU limit with a periodic parallel report (`c5.sh`, `c5batch.sh`, `c5batch2.sh`, `report.sql`, `c5rep.py`)

Same workload as case B in `../cpu-limit-and-locktail-20261021`: pgbench TPC-B scale 10, 16 clients outside the
cgroup, `-M prepared`, fixed 1,400 tx/s; `report.sql` (GROUP BY with 2 parallel workers) every ~5 s. CPU limit =
`systemctl set-property --runtime pgwet.service CPUQuota=…` (cgroup v2 `cpu.max`, 100 ms period). Ground truth:
`/proc/<pid>/schedstat` and cgroup `cpu.stat`. With `capture = trace` the rings were dumped once, right after the
window (the dump runs inside the cgroup); a 4 MB ring held the last ~48 s of the 60 s window, so the ring rows cover
~80% of the window's transactions. The stats rows cover the whole window.

On this box the workload needs 0.92 cores without a limit (0.76 on gate-2), so the 1-CPU limit of the original B1 is
an overload here (87–95% of periods throttled, p99 0.9–3.5 s, `c5-P*`, `c5-report-overload.txt`). The main set uses
1.3 CPU, which gives the same ~72% average use as B1.

| 60 s window | No limit | **1.3-CPU limit** (2 runs off, 2 trace) | 2.5-CPU limit |
|---|---|---|---|
| CPU used / periods throttled | 0.92–0.94 cores / 0% | 0.92–0.97 cores (71–75% of limit) / 7–11% | 0.96 / 1% |
| Request p50 / p99 / p99.9 | 1.72 / 5.2–7.5 / 25–46 ms | **1.72–1.84 / 131–172 / 189–311 ms** | 1.71 / 5.3 / 15 ms |
| Kernel: on a CPU / **waiting for a CPU** | 0.85–0.88 / 0.20–0.21 | 0.84–0.89 / **0.66–0.91** | 0.88 / 0.22 |
| pg_wait_sampling top | CPU 34, WalSync 31, WALWrite 23, Lock:tx 10 | CPU 28–30, WalSync 24–26, WALWrite 24–25, **Lock:tx 17–20** | CPU 35, WalSync 31, WALWrite 23, Lock:tx 9 |
| **Patch: waiting sessions (excl. ClientRead)** | 1.24 | **1.90–1.93** | 1.13 |
| Patch: Lock:transactionid avg / max | 0.87 ms / 45 ms | **1.67–1.77 ms / 77–85 ms** | 0.72 ms / 23 ms |
| Patch: WALWrite avg; WalSync avg | 418 µs; 488 µs | 574–577 µs; 527 µs | 377 µs; 469 µs |
| Patch rings: slowest 1% of transactions, dominant wait | Lock:tx 45%, WALWrite 39% | **Lock:tx 45–49%**, WALWrite 33–36% | Lock:tx 45%, WALWrite 37% |

**Findings:**
- **The patch points at row locks.** Under the limit its Lock:transactionid waits double in average and its slowest
  transactions are lock-dominated, which reads as row-lock contention. The real cause is CPU throttling: lock holders
  and woken waiters sit in the run queue (kernel: up to 0.9 sessions waiting for a CPU), so every wait that spans a
  throttled period gets longer. Raising the limit to 2.5 CPU brings p99 back to 5 ms.
- **This is inherent to timing waits inside the backend.** A wait ends when the backend runs again, so run-queue time
  after the wake-up is counted inside the wait. The patch measures that duration exactly but cannot split it.
- **What the patch does better than the sampler here:** its totals are not throttled. pg_wait_sampling's collector
  ran at 96.6–97.9 samples/s per backend under the limit; the patch counts every call. Neither shows CPU starvation.
- **Cost:** trace vs off at 1.3 CPU: CPU 0.95–0.97 vs 0.92 cores, throttled periods 10–11% vs 7–10%, p99 131–137 vs
  144–172 ms. Within run-to-run noise; at most a few percent of CPU.
- So the talk's line stands: "The patch can't do this: it times the same labels from inside the backend." Case 5 needs
  an observer outside the cgroup with run-delay (pg_wait_tracer feature F1, `../cpu-limit-and-locktail-20261021/HANDOVER-tracer-work.md`).

## Updated table: which tool answers which case

| Case | Sampler | pg_wait_tracer | Patch |
|---|---|---|---|
| 1. Short-request tail | ranks the wrong fix | lock tail found | lock tail found |
| 2. Relation-lock convoy | 0.00% for the query | found | found (v12) |
| 3. Commit convoy | blames locks | per-call flush time | per-call flush time |
| 4. Slow disk or bloat | twins | reads × duration per execution | **same, from SQL (this run)** |
| 5. CPU limit | throttled, blames Lock/WAL | correct session count; split needs F1 | **blames locks (this run)** |

## Files (`raw/`)

| Path | Contents |
|---|---|
| `c4.sh`, `c4batch.sh`, `bloat.sh`, `client.py`, `c2_init.sql`, `c4rep.py`, `c4-report.txt` | case 4 harness, report and its output |
| `c4-{st,bl}-{o,t}{0,10}/` | per run: `req-all.txt.gz` (every request: start, end, pid, id, outcome), `pgws.csv`, `pgss.csv`, `pgio.csv`, `timing0/1.csv` (patch stats at window start/end), `rings.csv.gz` (deduplicated trace rings), `by_statement.csv.gz`, `clock0` (wall vs CLOCK_MONOTONIC), `t0`/`t1` |
| `fio-storage.txt`, `bloat.log`, `holder.out` | fio result, bloat build log, idle session output |
| `c5.sh`, `c5batch.sh`, `c5batch2.sh`, `report.sql`, `c5rep.py`, `c5-report.txt`, `c5-report-overload.txt` | case 5 harness, report and outputs |
| `c5-Q*/` (1.3-CPU set), `c5-P*/` (1-CPU overload set) | per run: `lat-all.txt.gz`, `rep-all.txt`, `pgws-all.csv`, `pgss.csv`, `sched0/1`, `cg0/1`, `cpu.max`, `timing0/1.csv`, `rings.csv.gz`, `clock0`, `t0`/`t1` |

`c5-P*` used a 5-second ring dump loop inside the cgroup (`c5batch.sh`, first version of `c5.sh`); the current
`c5.sh` dumps once after the window. Reproduce: `python3 c4rep.py c4-*` and `python3 c5rep.py c5-Q*` in `raw/`.
