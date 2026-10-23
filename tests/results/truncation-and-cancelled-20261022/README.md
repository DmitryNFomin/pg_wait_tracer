# Sampler vs tracer: autovacuum truncation convoy, and cancelled lookups (gate-3, 2026-10-22)

> Raw logs and trace rings for this directory (files over 1 MB and all `.gz`/`trace/`): [`truncation-and-cancelled-20261022-raw.tar.gz`](https://github.com/DmitryNFomin/pg_wait_tracer/releases/tag/evidence-20261023) in the `evidence-20261023` release. Untar it inside `tests/results/` to restore the full directory.

**Box:** Hetzner pgwt-gate-3, cpx32: 4 vCPU AMD EPYC-Genoa (KVM), 8 GB RAM. Created from a snapshot of gate-2:
- PostgreSQL 18.6, port 5418
- pg_wait_sampling 1.1.11 at defaults (10 ms, `profile_queries = top`, history 5,000 rows)
- `pg_stat_statements`, `track_io_timing = on`, `log_lock_waits = on` (`deadlock_timeout` 1 s)
- `log_autovacuum_min_duration = 0`
- pg_wait_tracer build of 2026-10-14

Plain SQL, no `pg_sleep`, no injected delays. Single runs per configuration.

## Test 1: autovacuum truncation convoy (`tr.sh`, `rep1.py`)

**Setup:**
- Table `t(id bigint pk, grp int, payload text)`: 20 M rows, 2.1 GB heap. Cloned per run from a template database, then
  prewarmed.
- `shared_buffers` 2 GB, `autovacuum_naptime` 10 s.
- Table options: `autovacuum_vacuum_scale_factor = 0.01`, `autovacuum_vacuum_cost_delay = 0`.
- Workload: 16 pgbench clients doing PK lookups on ids 1..10 M at a fixed 8,000/s, for 300 s.
- At 60 s, `DELETE FROM t WHERE id > 10000000`. Autovacuum then vacuums the table and truncates the empty second half
  (133,333 pages).
- `pg_stat_progress_vacuum` is polled every 0.2 s to timestamp the phases.

**How truncation stalls lookups.** Autovacuum takes an ACCESS EXCLUSIVE lock to truncate. It checks every 20 ms
whether anyone is waiting; if so, it truncates what it has verified, releases the lock, and retries 50 ms later. Every
lookup that arrives meanwhile waits on `Lock:relation`.

| Run (300 s) | Requests > 15 ms | of which in truncation window | p99.9 / p99.99 / max | pgss lookup max | `log_lock_waits` |
|---|---|---|---|---|---|
| p1: truncation on, sampler only (pilot) | 3,041 | 3,029 | 20.9 / 74.7 / 92.5 ms | 38.8 ms | 0 |
| **s1: truncation on, sampler only** | **3,524** | 3,130 | 24.2 / 84.3 / 155.7 | 155.4 | 0 |
| f1: truncation on, + pg_wait_tracer `--mode full` | 12,098 | 12,083 | 24.5 / 111.8 / 130.7 | 35.7 | 0 |
| q1: truncation on, + `pg_blocking_pids()` poller (5 ms) | 3,749 | 3,744 | 23.1 / 103.4 / 129.2 | 49.4 | 0 |
| m1: truncation on, simple protocol (`-M simple`) | 3,761 | 3,409 | 24.2 / 81.8 / 105.6 | 73.4 | 0 |
| **o1: `vacuum_truncate = off` (control)** | **10** | — | 1.3 / 5.1 / 41.5 | 41.4 | 0 |

The truncation phase lasted 1.2–1.8 s, except under the tracer (f1), where it lasted 9.6 s.

**Repeats (same harness, later the same day):**

| Run | Requests > 15 ms (all inside truncation window) | p99.99 / max | Sampler: lookup Lock:relation | Sampler: lock samples with no query | Tracer: Lock:relation on the lookup |
|---|---|---|---|---|---|
| s2 / s3 (sampler only) | 3,550 / 3,273 | 101 / 126, 70 / 96 ms | 0.00% / 0.00% | 897 / 877 | — |
| f2 / f3 (tracer full) | 11,636 / 13,047 | 107 / 125, 134 / 152 ms | 0.00% / 0.00% | 3,288 / 3,603 | 1,408 / 1,488 waits, avg 23.9 / 24.5 ms |
| o2 (`vacuum_truncate = off`) | 1 | 3.4 / 15.4 ms | — | 0 | — |

Every finding below held in all 3 sampler-only, 3 traced and 2 control runs.

**What each tool says about the lookup:**

| | pg_wait_sampling | pg_wait_tracer (f1) |
|---|---|---|
| Lookup's own wait profile (per query) | CPU 90%, DataFileRead 10%, **Lock:relation 0.00%** (all runs) | CPU*, **Lock:relation 1,374 waits, 33.3 s total, avg 24.2 ms, max 126 ms**, DataFileRead |
| Where the Lock:relation time went | **839 / 3,290 / 970 / 795 samples with no query attached** (s1/f1/q1/m1): query ID 0 or not in pgss | All 1,374 waits attributed to the lookup's query ID (`-4621018123696140640`) |
| Global profile | Lock:relation 1.5–2.3% (5.5% in f1) | Lock:relation 4.8% of DB time |
| Wait-duration distribution | not available | 94.6% of the lock waits ≥ 16 ms |
| Holder side | `Timeout:VacuumTruncate` 1.3% (f1), not tied to anything | `Timeout:VacuumTruncate` 152 × 50 ms: the autovacuum worker backing off |
| Retrospective | history ring (5,000 rows) covers ~1.6 s at the end; **0 lock rows** | full trace, replayable |

**Findings:**
- **The sampler cannot connect the stall to the query it hurt.** The relation lock is taken before the query ID is
  published: at bind/plan-cache revalidation for prepared statements, at parse/analysis for simple protocol. So
  pg_wait_sampling records those samples with no query, and the lookup's per-query profile says "90% CPU, no locks".
  This affects every relation-lock convoy, e.g. DDL or migrations queueing behind or ahead of traffic.
- **The tracer attributes the waits to the lookup,** with counts and durations.
- **Ordinary tools:**
  - `log_lock_waits` is silent (waits < `deadlock_timeout`).
  - pgss shows a max with no cause.
  - The autovacuum log line reports "pages: 133333 removed" with no stall information.
  - A 5 ms `pg_blocking_pids()` poller does name `autovacuum` in 1,928 of 1,928 waiter rows, but only if it was
    running at the time.
- **Observer effect:** with the exact tier on, the truncation phase stretched from 1.2 s to 9.6 s and slow requests
  went from 3.5k to 12.1k. The tracer's per-lookup overhead (p99 0.51 → 2.88 ms) means more lock holders and waiters,
  so autovacuum backs off more often.

### Test 1 with the wait-event timing patch (`wet-v11`), `trw.sh`

**Setup:**
- PostgreSQL 20devel built from `github.com/DmitryNFomin/postgres` branch `wet-v11` (tip `382f87e`), port 5419.
- Same config and workload as above.
- `pg_wait_event_tracing` preloaded, with `capture` set per database. pg_wait_sampling is built against the same server
  and running in the same runs.
- With `capture = trace`, every lookup backend's ring (131,072 records each, ~50 s of history at this rate) and
  `pg_wait_event_trace_by_statement()` were dumped 3 s after the truncation phase ended.

| | w0: capture = off | w1: capture = trace |
|---|---|---|
| Requests > 15 ms (all in truncation window) | 3,052 | 3,369 (3,226 in window) |
| p99.99 / max | 95.3 / 121.5 ms | 81.3 / 102.0 ms |
| pg_wait_sampling: lookup Lock:relation / unattributed lock samples | 0.00% / 787 | 0.00% / 821 |
| Patch stats (`pg_stat_get_wait_event_timing`, 16 lookup backends) | — | **Lock:relation 208 calls, 8,088 ms, avg 38.9 ms, max 101.5 ms** |
| Patch ring: query_id on the Lock:relation records | — | **0 for all 208** |
| Patch ring: neighbouring markers | — | previous marker `Idle` (208/208); next marker the lookup's `ExecStart` (195) or `QueryStart` (13), query_id `-4621018123696140640` |
| `pg_wait_event_trace_by_statement()` | — | **all 208 in `<idle>`** (8,088 ms) |

**Findings:**
- **The patch records the convoy exactly** (count, durations, max, per backend), **with no visible observer
  effect**: slow requests 3,369 vs 3,052, against the tracer's 3.3×.
- **It misattributes it.** The lock is acquired after the client message arrives but before the statement-start marker:
  at bind/plan-cache revalidation for prepared statements, at parse analysis for simple protocol. So the wait record
  carries query_id 0, and `by_statement()` buckets it as `<idle>`.
- **The information is in the ring:** every such wait sits between `Idle` and the lookup's start marker.
- **Suggested patch fix:** attribute waits that occur after the `ClientRead` wait ends and before the next
  `QueryStart`/`ExecStart` to that next statement, as its "pre-statement" phase. Do it in `by_statement()`, or emit a
  marker when ClientRead ends.
- **pg_wait_sampling cannot do this:** it never sees which statement follows.
- **Corrections from the patch author** (`REPLY-from-patch-author.md`):
  - In v11 every wait record has query_id 0, so the query_id point above shows nothing. The position in the ring is
    the evidence.
  - The suggested fix misses buffered requests that start right after `TxnCommit` with no `Idle`.
  - The cost line needs repetitions.
  
  All three are addressed in the v12 section below.

### Test 1 on `wet-v12` (fixed `by_statement()`), `batchv12.sh`, `bs12.py`

**Setup:**
- Branch `wet-v12` (tip `ae41909b85`). Relative to v11 it changes only the contrib module's SQL, comments, tests and docs.
- Rebuilt into `/opt/pgwet`, same config as above (`shared_buffers` 2 GB, `trace_ring_size` 4 MB). `trsrc` was recreated
  with `setup1.sh`'s SQL. PG18 was stopped during the runs.
- Three `capture = trace` runs alternated with three `capture = off` runs (`trw.sh`, unchanged).

**Attribution (`bs12.py`):**

| Run | Convoy waits (stats = ring = by_statement) | avg / max | `by_statement` bucket of the lock waits | Lookup `DataFileRead` |
|---|---|---|---|---|
| v12t1 | 144 | 45.2 / 107.1 ms | **statement, `phase = 'pre'`: 144** | 34,274 in statement buckets, 0 in `<idle>` |
| v12t2 | 208 | 38.8 / 110.1 ms | **statement, `phase = 'pre'`: 208** | 27,725 in statement buckets, 0 in `<idle>` |
| v12t3 | 32 | 94.1 / 119.2 ms | **statement, `phase = 'pre'`: 32** | 27,441 in statement buckets, 1 in `<unattributed>` |

- **Both gaps are fixed in all three runs.** The ring, `by_statement()` and `pg_stat_get_wait_event_timing()` agree on
  every convoy wait. Only `ClientRead` is left in `<idle>`.
- **Buffered requests (v12t1):** 231 executions start directly after `TxnCommit` with no `Idle` between. All 22 of
  those with waits of their own got their own bucket.
- **Ring dump vs `by_statement()`:** `DataFileRead` counts differ by −6 to +9 per backend. This is a measurement
  artifact: the two dump calls run while pgbench still runs at 8,000/s, so they see slightly different rings.
- **Not exercised here:** `<no-statement>`, `DO`/`CALL`, the simple protocol.

**Cost, convoy runs (`rep1.py`; v11 w0/w1 added):**

| | Requests > 15 ms | p99.99 / max (ms) | Truncation phase |
|---|---|---|---|
| off: v12o1 / o2 / o3 (w0) | 1,436 / 3,164 / 3,530 (3,052) | 74 / 88, 84 / 106, 92 / 118 (95 / 122) | 0.0 / 1.0 / 1.8 (1.0) s |
| trace: v12t1 / t2 / t3 (w1) | 2,947 / 3,086 / 1,656 (3,369) | 87 / 107, 85 / 111, 97 / 120 (81 / 102) | 0.6 / 1.6 / 0.2 (1.2) s |

- **No cost visible.** The run-to-run spread (1.4k–3.5k slow requests in each configuration) follows how long autovacuum
  holds the lock in that run, and it is much larger than any difference between `off` and `trace`. Mean of the
  four runs per side: off 2,796, trace 2,765.
- **pg_wait_sampling, all eight runs:** lookup `Lock:relation` 0.00%. The history ring at the end holds no
  `Lock:relation` rows. `log_lock_waits` logged nothing (waits < 1 s).

## Test 2: cancelled lookups, slow disk vs bloat (`c2.sh`, `client.py`, `rep2.py`)

**Setup:**
- Case 3 setup: `t` 12 M rows (2.8 GB heap), `shared_buffers` 128 MB, cgroup `MemoryMax=1200M`, caches dropped.
- `client.py`: 8 psycopg workers, closed loop, `statement_timeout = 10ms`. It continues after cancellations and logs
  every request (start, end, backend pid, id, outcome).
- **Storage:** fio randrw 64k, iodepth 32, 4 jobs on the same disk (16k read + 16k write IOPS); ids over 12 M.
- **Bloat:** an idle REPEATABLE READ session plus 30 rounds of `UPDATE … WHERE id <= 200000` (table 4.1 GB,
  6 M dead tuples); ids over 1..200,000.

| 60 s window | Storage, sampler only | Storage, + tracer | Bloat, sampler only | Bloat, + tracer |
|---|---|---|---|---|
| Timeouts | 1,535 (1.14%) | 5,148 (5.02%) | 240 (0.14%) | 1,999 (1.56%) |
| pg_wait_sampling, client backends | **DataFileRead 96.6%**, CPU 3.4% | DataFileRead 94.2% | **DataFileRead 94.7%**, CPU 5.3% | DataFileRead 87.8%, CPU 12.0% |
| pgss (completed calls only): blocks read / call | 1.7 | 1.7 | 29.2 | 29.2 |
| pg_stat_io, avg read | 1.825 ms | 2.478 ms | 0.084 ms | 0.115 ms |
| pg_wait_tracer, 500 slowest executions (≥ 10 ms, i.e. the timed-out ones) | — | **2 reads each, avg read 8.3 ms** | — | **32 reads each, avg read 378 µs** |

**Findings:**
- **Tracer vs sampler:** the sampler shows twins (DataFileRead 95–97% vs 88–95%). The tracer separates the cancelled
  executions directly: few slow reads (storage) vs many fast reads (bloat). That points at different fixes: storage
  isolation vs ending the snapshot and vacuuming.
- **vs other tools:** pgss excludes the cancelled calls, but its completed calls carry the same signature (1.7 vs 29.2
  blocks/call), and pg_stat_io / `pg_stat_get_backend_io()` show the read latency (1.8–2.5 vs 0.08–0.12 ms). Here the
  cancelled executions are not different from the successful ones, so cancellation does not hide the cause.
- **Observer effect:** the exact tier roughly quadrupled (storage) and octupled (bloat) the timeout rate.
- **Tracer limit:** pgwt-server `executions` returns at most 500 rows per request.

## Files (`raw/`)

| Path | Contents |
|---|---|
| `setup1.sh`, `tr.sh`, `rep1.py`, `lookup.sql`, `progress.sql`, `poll.sql` | Test 1 harness and report |
| `tr-{p1,s1,f1,q1,m1,o1}/` | per run: `pgb.txt`, `lat-all.txt.gz`, `progress.txt`, `delete.txt`, `pgws-all.csv`, `pgws-by-query.csv`, `pgws-history.csv`, `pgss.csv`, `pglog.txt`, `blockers.txt` (q1), tracer outputs and `trace/` (f1) |
| `c2_init.sql`, `bloat.sh`, `client.py`, `c2.sh`, `rep2.py` | Test 2 harness and report |
| `c2-{storage,bloat}-{full,pgws}/` | per run: `req-all.txt.gz`, `pgws*.csv`, `pgss.csv`, `pgio.csv`, `bio0/1.csv`, `client-pids`, tracer outputs and `trace/` (full) |
| `fio-storage.txt`, `bloat.log`, `holder.out`, `postgresql.auto.conf.orig` | context |

Reproduce: `python3 rep1.py tr-s1 tr-f1 …` locally; `rep2.py` needs `/root/pgwt-server` for the tracer rows.
