# Short-request tail: the time-share profile ranks the wrong fix (gate-3, 2026-10-22)

> Raw logs and trace rings for this directory (files over 1 MB and all `.gz`/`trace/`): [`checkout-tail-20261022-raw.tar.gz`](https://github.com/DmitryNFomin/pg_wait_tracer/releases/tag/evidence-20261023) in the `evidence-20261023` release. Untar it inside `tests/results/` to restore the full directory.

**Result.** A ~1.5 ms checkout request has a 10–25 ms tail caused by row-lock waits behind a background restock job.

- **pg_wait_sampling** ranks the commit path first (WALWrite + WalSync ~62%), then CPU, catalog reads (12–15%), and
  Lock:transactionid last at 2.3%.
- **The two fixes this ranking suggests do not touch the tail.** `synchronous_commit = off` halves the mean (1.51 →
  0.83 ms) but leaves p99.9 at 11.4 ms. 2 GB of cache removes the reads but leaves p99.9 at 11.3 ms.
- **The wait-event timing patch** reconstructs each request from its trace ring and shows that the slow requests (server
  time > 5 ms) are lock waits: Lock:transactionid dominates 51% of them, 77% of those whose time is spent in the
  database, and is 50% of the wait time inside slow requests. That is a 2.5% item in the overall time share.
- **The fix it points to works:** running the restock job in 5-row chunks takes p99.9 from 11.0 to 5.8 ms, max from
  26.7 to 10.0 ms, and requests > 5 ms from 972 to 388.

Plain SQL, a real disk, no `pg_sleep`, no injected delays. One run per configuration; the baseline was run twice (R0b,
R0c) with matching results.

## Setup

- **Box:** gate-3, cpx32, 4 vCPU AMD EPYC-Genoa (KVM), 8 GB.
- **Server:** PostgreSQL 20devel `wet-v11` (port 5419) as systemd unit `pgwet` with `MemoryMax=1200M`, so the catalog
  does not fit in memory. `shared_buffers` 128 MB, `track_io_timing` on. pg_wait_sampling (built from source) and
  `pg_stat_statements` loaded. `pg_wait_event_tracing` loaded, with `capture` set per database (`trace` only in R1b),
  ring 4 MB.
- **Data (`shop_init.sql`):**
  - `catalog`: 3 M rows, 1.3 GB
  - `stock`: 1,000 rows
  - `orders`: 2 M rows, index on `item_id`
- **Requests (`checkout.sql`):** 16 clients, `-M prepared`, fixed 2,000/s. Each request is `BEGIN`, a PK lookup in
  `catalog` (mostly a real disk read), `UPDATE stock … WHERE item_id = :item` (one of 1,000 rows), `COMMIT`.
- **Restock job (`restock.sql`):** 1 client, Poisson ~5/s. One `UPDATE stock s SET reserved = (SELECT count(*) FROM
  orders WHERE item_id = s.item_id)` over 100 contiguous items (~17 ms; it holds those row locks until it ends).
  - Chunked variant (`restock_chunked.sql`): the same 100 items in 20 statements of 5 (~1 ms each).
- **Measurement:** 150 s per run, 120 s measured window. Request latency is from the pgbench log, counted from the
  scheduled start.

## Results

| Run | Change | Mean | p50 | p99 | **p99.9** | Max | Requests > 5 ms |
|---|---|---|---|---|---|---|---|
| R0c | baseline (MemoryMax 1200M) | 1.51 | 1.44 | 3.08 | **10.98** | 26.7 | 972 |
| R0b | baseline (MemoryMax 900M) | 1.55 | 1.48 | 3.08 | 10.85 | 26.5 | 968 |
| R1b | baseline + patch `capture = trace` | 1.54 | 1.47 | 3.18 | 10.79 | 24.3 | 1,055 |
| F1a | `synchronous_commit = off` (sampler's top item) | **0.83** | 0.77 | 1.74 | **11.36** | 25.7 | 688 |
| F1b | `shared_buffers` 2 GB, no memory limit, prewarmed (sampler's read item) | 1.34 | 1.26 | 2.82 | **11.30** | 23.9 | 954 |
| **F2** | **restock job in 5-row chunks (patch's finding)** | 1.52 | 1.47 | 2.92 | **5.78** | **10.0** | **388** |

All times in ms. R1 (patch trace) accidentally ran with `synchronous_commit = off`, left over from a crashed earlier
batch. It matches F1a and is kept only for reference; R1b is the valid diagnostic run.

**pg_wait_sampling, R0c (request backends, excl. ClientRead):**
- WALWrite 32.7%, WalSync 29.3%, CPU 22.4%, DataFileRead 12.3%, **Lock:transactionid 2.3%**.
- Per query:
  - COMMIT, i.e. no query text: 78% of samples
  - catalog lookup: 16% (DataFileRead 76%)
  - `UPDATE stock`: 6% (Lock:transactionid 41% of it)

**Patch, R1b, per request** (`shtail.py`: requests reconstructed from the trace rings, split at `TxnCommit`, server time
from the end of the idle `ClientRead` to the commit; 107,533 requests in the ~50 s the rings cover):

| | Share of all request wait time | Dominant wait of each slow request (> 5 ms, n = 579) | Share of wait time inside slow requests |
|---|---|---|---|
| LWLock:WALWrite | 34.5% | 68 (12%) | 13.0% |
| IO:WalSync | 30.0% | 15 (3%) | 3.7% |
| Client round trip inside the transaction | 20.5% | 192 (33%) | 31.4% |
| IO:DataFileRead | 11.5% | ~0 | ~0 |
| **Lock:transactionid** | **2.5%** | **297 (51%)** | **49.6%** |

## Findings

- **Time share and tail cause disagree.** The lock waits are 2.5% of all wait time but half of the slow requests and half
  of their wait time. A time-share profile, sampled or exact, ranks the commit path and reads first, and the fixes for
  those (F1a, F1b) leave p99.9 unchanged.
- **Per-request reconstruction finds the cause, and the fix confirms it** (F2: p99.9 −47%, max −63%).
- **A third of the slow requests are slow on the client side:** the client took milliseconds to send the next statement
  of an open transaction. That is visible only per request. A sampler counts it as `ClientRead` (idle).
- **Fairness:** pg_wait_sampling's per-query view does show `UPDATE stock` at 41% Lock:transactionid, so an expert who
  drills into that statement sees lock waits. What the sampler cannot say is that these waits, rather than the 25× larger
  commit-path share, make the tail.
- **The patch's cost:** R1b vs R0c is within noise (p99.9 10.79 vs 10.98 ms; 1,055 vs 972 requests > 5 ms).
- **pg_wait_tracer (run on stock PG18 with the same data, memory limit and workload: T0 baseline, T1 `--mode full`;
  `trtail.py` rebuilds requests from `dump_raw`):**
  - **Boundaries:** a request ends at the switch into `ClientRead` carrying COMMIT's query id, and starts when that idle
    `ClientRead` ends.
  - **Latency:** T0 p99.9 12.66 ms, 1,118 requests > 5 ms. T1 p99.9 7.58 ms, 1,058. No visible tail inflation at this
    rate (single runs).
  - **Sampler on PG18 (T0):** WALWrite 33.1%, WalSync 29.4%, CPU 21.8%, DataFileRead 11.9%, **Lock:transactionid
    3.0%**.
  - **Tracer, T1** (240,946 requests reconstructed, the whole window). Share of all request wait time: WALWrite 35.8%,
    WalSync 31.4%, client round trip 16.1%, DataFileRead 13.9%.
    - Slow requests > 5 ms (838): dominant wait Lock:transactionid 42%, WALWrite 33%. Many sit just above 5 ms.
    - **Slow requests > 8 ms (189): dominant wait Lock:transactionid 75%; Lock is 70.9% of their wait time.**
  - **Patch, same 8 ms threshold (R1b, 286):** Lock:transactionid dominates 65%, client round trip 35%; Lock is 59.5% of
    their wait time.
  - Both tools reach the same conclusion: the tail beyond p99.9 is the restock job's row locks.
- **Operational note for the patch:** an earlier attempt with `trace_ring_size = 16MB` under `MemoryMax=900M` was
  OOM-killed (cgroup OOM). Rings live in shared memory, so ~16 MB × backends counts against a container's limit.

## Files (`raw/`)

| Path | Contents |
|---|---|
| `shop_init.sql`, `checkout.sql`, `restock.sql`, `restock_chunked.sql` | schema and workloads |
| `sh.sh`, `shbatch.sh`, `shbatch2.sh` | harness and batches |
| `shrep.py`, `shtail.py`, `trtail.py` | latency + sampler summary; per-request reconstruction from patch rings / from pg_wait_tracer raw trace |
| `sh-T0`, `sh-T1` | PG18 runs: baseline and pg_wait_tracer `--mode full` (`raw.txt.gz` = `dump_raw` output, `trace/`) |
| `sh-R0`, `sh-R0b`, `sh-R0c`, `sh-R1`, `sh-R1b`, `sh-F1a`, `sh-F1b`, `sh-F2` | per run: `pgb.txt`, `pgb-job.txt`, `lat-all.txt.gz`, `job-all.txt`, `pgws-by-query.csv`, `pgss.csv`, `shared_buffers.txt`, `cgroup.txt`, `t0/t1`, `clock0`; R1/R1b: `rings.csv.gz`, `procs.csv` |

(`sh-R0` is the first calibration, with a lighter 50-row job.)
