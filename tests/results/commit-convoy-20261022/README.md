# Commit convoy: a slower WAL flush looks like lock contention (gate-3, 2026-10-22)

> Raw logs and trace rings for this directory (files over 1 MB and all `.gz`/`trace/`): [`commit-convoy-20261022-raw.tar.gz`](https://github.com/DmitryNFomin/pg_wait_tracer/releases/tag/evidence-20261023) in the `evidence-20261023` release. Untar it inside `tests/results/` to restore the full directory.

**Result.** Write bursts from a neighbour on the same disk make each commit's WAL flush 4× slower on average (10× at p99).
Row-lock holders keep their locks longer, and transaction latency p99 goes from 4 ms to 1.1 s.

- **pg_wait_sampling** sees Lock:transactionid grow ×25–31 and WALWrite ×9–10 during the bursts, but IO:WalSync only
  ×1.4–1.5. The flush calls became 2.7× rarer while each got 4× slower, so their time share barely moves. The
  profile points at row-lock contention.
- **pg_wait_tracer and the wait-event timing patch** both show the per-call flush time: avg 0.42 → 1.74 ms, p99
  0.85 → 8.2 ms, max 6 → 23–36 ms, while lock waits go 0.75 → 4.96 ms. That points at storage.
- **The control confirms it:** without the fio bursts, p99 is 4.2 ms and Lock:transactionid is 9% of the profile.

Plain SQL (pgbench TPC-B), a real disk, no `pg_sleep`, no injected delays. One run per configuration.

## Setup

- **Box:** gate-3, cpx32, 4 vCPU AMD EPYC-Genoa (KVM), 8 GB, one virtual disk for data, WAL and fio.
- **Servers:**
  - stock PostgreSQL 18.6 (port 5418) with pg_wait_sampling 1.1.11 and pg_wait_tracer (build 2026-10-14)
  - PostgreSQL 20devel `wet-v11` (port 5419) with pg_wait_sampling (built from source) and `pg_wait_event_tracing`
    (`capture = trace` in postgresql.conf, `trace_ring_size = 16MB`)

  Only one server ran at a time. Both use `shared_buffers` 2 GB, `track_io_timing` on, pgbench scale 10.
- **Workload:** `pgbench -b tpcb-like -M prepared -c 16 -j 4 -R 1500`, 150 s, measured window 120 s.
- **Disturbance:** every 10 s, a 2 s `fio --rw=randwrite --bs=64k --direct=1 --iodepth=32 --numjobs=4` on a file on
  the same disk (12 bursts per window), like a backup, ETL or neighbouring VM. Burst wall times are in `bursts.txt`.
- **Sampler fairness:** `pg_wait_sampling_history` was polled every second (`histpoll.sql`), so the sampler had its
  complete per-sample history, not only the profile.

## Results

| 120 s window | cal18: PG18, sampler only | t18: PG18 + tracer `--mode full` | p20: PG20 + patch (trace) | n18: PG18, no fio |
|---|---|---|---|---|
| Transactions | 180,116 | 181,451 | 179,421 | 180,089 |
| p50 / p99 / max | 2.0 / 1,137 / 1,369 ms | 1.9 / 1,137 / 1,387 ms | 1.9 / 1,083 / 1,262 ms | 1.8 / 4.2 / 13 ms |
| Transactions > 50 ms (all inside bursts) | 37,064 | 42,983 (37,265 in bursts) | 36,200 | 0 |
| pg_wait_sampling profile | Lock:tx 35.2, WALWrite 31.1, WalSync 14.6, CPU 13.7 | Lock:tx 33.6, WALWrite 27.3, CPU 22.6, WalSync 12.3 | Lock:tx 36.9, WALWrite 31.6, WalSync 14.4, CPU 12.5 | CPU 34.4, WalSync 31.5, WALWrite 23.1, Lock:tx 9.1 |

Latency is from the scheduled start (`-R`), so it includes queueing behind slow commits.

**pg_wait_sampling, active sessions from the polled history, outside → inside bursts (client backends):**

| Event | t18 | p20 |
|---|---|---|
| Lock:transactionid | 0.23 → 5.85 (×25.5) | 0.18 → 5.51 (×31.3) |
| LWLock:WALWrite | 0.42 → 4.11 (×9.8) | 0.44 → 3.98 (×9.0) |
| **IO:WalSync** | **0.56 → 0.86 (×1.5)** | **0.60 → 0.83 (×1.4)** |
| CPU | 1.13 → 1.33 | 0.64 → 0.46 |
| Lock:tuple | 0.02 → 0.65 | 0.01 → 0.62 |

**Exact per-call durations, outside → inside bursts (`cccause.py`):**

| Event | pg_wait_tracer (t18) | Patch (p20) |
|---|---|---|
| **IO:WalSync** | 112,907 → 15,921 calls; **avg 0.42 → 1.74 ms; p99 0.85 → 8.20 ms**; max 6.5 → 22.9 | 107,791 → 18,252 calls; **avg 0.49 → 1.49 ms; p99 0.81 → 7.60 ms**; max 6.4 → 35.7 |
| LWLock:WALWrite | avg 0.34 → 2.18; p99 1.01 → 8.51 | avg 0.37 → 1.92; p99 0.89 → 8.10 |
| IO:WalWrite | avg 0.02 → 0.03 (the write itself is not slow) | avg 0.02 → 0.02 |
| Lock:transactionid | avg 0.75 → 4.96; p99 2.81 → 24.8; max 13.6 → 145.9 | avg 0.68 → 4.40; p99 2.00 → 23.8; max 6.3 → 169.7 |

(Outside = 87 s, inside = 33 s.)

## Findings

- **The sampler points at the wrong component.** Its share for the flush barely moves (×1.4–1.5) because fewer flushes
  happen while each takes longer. Lock waits, the consequence, grow ×25–31. Time share cannot separate "more calls"
  from "longer calls", and here they move in opposite directions. Having the full history at 10 ms does not help:
  the flush stalls are 1–35 ms, and the history gives no per-call duration.
- **The exact tools show it directly:** the flush itself is 4× slower on average and 10× at p99, while the WAL write is
  unchanged. That means a slow fsync, i.e. storage contention. Both the tracer and the patch show the same numbers.
- **Not a "first wait" story:** the analysis in `ccorder.py` shows that the first ≥ 20 ms wait in each burst is always a
  lock wait. Flushes rarely exceed 20 ms; many moderately slower flushes add up. The signal is the per-call
  distribution, not one giant stall.
- **Observer cost here:** the patch's run is within run-to-run noise of the sampler-only run (36,200 vs 37,064 slow
  transactions). The tracer's run has 16% more slow transactions (42,983).
- **Other tools:** `pg_stat_io` (PG18 WAL fsync time) and OS disk metrics (iostat) would also show slower fsyncs if
  sampled at second granularity. The comparison here is sampler vs exact wait tracing.

## Files (`raw/`)

| Path | Contents |
|---|---|
| `cc.sh`, `histpoll.sql` | harness (`cc.sh TAG SERVER OBS FIO`) and history poller |
| `ccrep.py`, `ccorder.py`, `cccause.py` | latency/profile summary; first long wait per burst; per-call durations in/out of bursts |
| `cc-cal18`, `cc-t18`, `cc-p20`, `cc-n18` | per run: `pgb.txt`, `lat-all.txt.gz`, `bursts.txt`, `pgws-all.csv`, `pgws-hist-poll.csv.gz`, `activity.csv`, `clock0/1`, `t0/t1`; t18: `raw.txt.gz` (pg_wait_tracer transitions via `dump_raw`), tracer output, `trace/`; p20: `rings.csv.gz` (patch trace rings of the 16 client backends), `timing.csv`, `procs.csv` |
