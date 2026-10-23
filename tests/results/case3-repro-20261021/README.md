# Case 3 reproduced: "slow storage" or "bloat"? Same sampled profile, different root cause

> Raw logs and trace rings for this directory (files over 1 MB and all `.gz`/`trace/`): [`case3-repro-20261021-raw.tar.gz`](https://github.com/DmitryNFomin/pg_wait_tracer/releases/tag/evidence-20261023) in the `evidence-20261023` release. Untar it inside `tests/results/` to restore the full directory.

Gate-2, 2026-10-21. Real reads from a real disk, no `pg_sleep`, no injected delays.

**Result.** The same primary-key lookup was made slow in two different ways. pg_wait_sampling (100 Hz, default)
showed the same picture for both: about 90% `IO:DataFileRead`, same query. pg_wait_tracer's exact tier told them apart
in one view: about 17x more reads per lookup at 117 us each (bloat) versus 1.7 reads per lookup at 1.96 ms each (storage),
with completely different read-latency histograms.

| 60 s, 8 clients, same SQL | Slow storage | Bloat |
|---|---|---|
| Lookups/s (pgbench) | 2,156 | 2,059 |
| Latency p50 / p99 / max | 3.86 / 10.1 / 230 ms | 3.19 / 11.5 / 26 ms |
| **pg_wait_sampling** | **IO:DataFileRead 95.2%**, CPU 4.8% | **IO:DataFileRead 88.6%**, CPU 11.3% |
| pg_wait_sampling AAS | 7.6 | 7.9 |
| **pg_wait_tracer: DataFileRead waits** | **225,780** | **3,612,309** |
| pg_wait_tracer: avg / max read | **1,964 us** / 97.4 ms | **117 us** / 8.6 ms |
| pg_wait_tracer: reads per lookup | ~1.7 | ~29 |
| Disk-read hump of the histogram | 2-8 ms (39% of all reads) | 128-512 us (35%) |
| pg_stat_statements: blocks read / call | 1.7 | 29.2 |
| pg_stat_statements: read time / call | 3.44 ms | 3.64 ms |
| pg_stat_io: avg ms per read | 1.974 | 0.125 |
| Root cause | another process flooding the disk | an idle-in-transaction session holding a snapshot for 11 min, so 30 versions of each hot row survive |
| Fix | storage / I/O isolation | end the session, then vacuum |

Baseline ("normal", no disturbance, whole table): 21,197 lookups/s, p50 0.37 ms, DataFileRead 57.9%,
1.7 blocks read per call at 0.13 ms.

## Honest notes

- `pg_stat_statements` with `track_io_timing` **can** also tell these apart here (1.7 vs 29.2 blocks per call). It
  only sees completed statements, averaged since the last reset; in the production Case 3 the slow lookups hit
  `statement_timeout` and were cancelled, so they never reached it. That part was not reproduced (pgbench aborts a
  client on error), and should be said, not shown.
- The sampler is not wrong here: 90% DataFileRead is true for both. It just cannot say whether that is many fast reads
  or few slow ones. The tracer gives count, duration distribution and per-query breakdown from the same capture.
- pg_wait_tracer's exact tier was running during both measured minutes (and during the baseline), so all numbers in
  the table are for the traced run. Tracer overhead was not separately measured here.
- PostgreSQL 18 default `io_method = worker`; `IO:AioIoCompletion` shows up at a few hundred waits.
- Histogram buckets are pg_wait_tracer's power-of-two microsecond buckets.

## Setup

| Item | Value |
|---|---|
| Box | Hetzner gate-2, 4 vCPU AMD EPYC-Rome, 7 GB RAM, one 76 GB virtual disk (`/dev/sda`), Ubuntu 24.04, kernel 6.8 |
| PostgreSQL | 18, `shared_buffers` 128MB, `track_io_timing` on for the test database |
| Memory | `MemoryMax=1200M` on `postgresql@18-main.service` (cgroup v2, runtime only), so the 3-4 GB table cannot sit in the page cache; reads go to the disk (0.11-0.13 ms each when quiet) |
| Table | `case3.t(id bigint primary key, grp int, filler text)`, 12,000,000 rows, 2.8 GB heap, 257 MB index, fillfactor 100 |
| Query | `SELECT filler FROM t WHERE id = :id`, pgbench `-M prepared -c 8 -j 4`, 80 s run, 60 s measured window |
| Observers | pg_wait_sampling 1.1.11 profile (reset at window start); pg_stat_statements and pg_stat_io (reset); `pg_wait_tracer --mode full --count 1 --interval 60 -T trace` (build of 2026-10-14), then `--replay` for histogram and query views |

Scenarios:

1. **normal**: ids uniform over 1..12,000,000; caches dropped first.
2. **storage**: same ids; `fio --rw=randrw --rwmixread=50 --bs=64k --direct=1 --iodepth=32 --numjobs=4` on a 4 GB file
   on the same disk for the whole run (~16k read + 16k write IOPS, ~1 GB/s each way). Read latency 0.13 -> 1.97 ms.
3. **bloat**: a session runs `BEGIN ISOLATION LEVEL REPEATABLE READ; SELECT count(*) FROM t WHERE id = 1;` and then
   idles (application_name `reporting_job`). Meanwhile `UPDATE t SET grp = grp + 1 WHERE id <= 200000` runs 30 times
   (6,000,000 row versions, table grows to 4.1 GB, all dead versions kept). Lookups then target ids 1..200,000, the
   frequently updated rows. Each lookup visits ~30 index entries and heap tuples on different pages.

Normal and storage use the whole id range; bloat uses the 200,000 hot ids. The SQL text and query id are identical.

## Also run the same day: pgbench TPC-B under a CPU limit (not a tracer win)

`raw/cl.sh`, `raw/cl_report.py`: pgbench TPC-B, 16 clients, with no limit, `CPUQuota=200%` and `100%`, each with
pg_wait_sampling only and with the tracer as well. At the 1-CPU limit TPS fell 4,897 -> 1,547. pg_wait_sampling (9.1
AAS, Lock:transactionid 41%, LWLock:WALWrite 25%, CPU 21%) and the tracer (9.6 client AAS, Lock 50%, LWLock 24%,
CPU* 4.5% + Off-CPU* 4.9%) told the same story: locks. The kernel said 8.6 of 16 sessions were runnable/throttled.
Lock holders are throttled while holding row locks, so lock waits stretch. Neither tool attributes that to the CPU
limit, because the tracer does not yet split run-queue time out of wait intervals. Useful as a product gap, not as a
"tracer beats sampler" case. Per-run numbers: `python3 cl_report.py` in `raw/`.

## Files (`raw/`)

| Path | Contents |
|---|---|
| `c3_init.sql`, `c3_lookup.sql`, `c3_bloat.sh`, `c3_measure.sh`, `c3_report.py` | Case 3 harness (as run on the box; `RANGE` env sets the id range) |
| `c3-{normal,storage,bloat}/` | per scenario: `pgb.txt`, `lat_us.txt` (every lookup's latency, us), `pgws.csv`, `pgss.csv`, `pgio.csv`, `tracer-system_event.txt`, `tracer-hist.txt` (empty, wrong event name, see below), `tracer-query.txt`, `trace/` (pg_wait_tracer trace files, replayable), `t0`/`t1` window |
| `tracer-hist-IO_DataFileRead.txt` | the two DataFileRead histograms (correct event name) |
| `fio-test.txt`, `fio-storage.txt` | fio results during the storage runs |
| `holder.out` | output of the idle-in-transaction session |
| `cl.sh`, `cl_report.py`, `pgb-*`, `pgws-*`, `sched*`, `rounds-*`, `tracer-*-tracer.txt`, `win-*` | CPU-limit TPC-B runs |
| `postgresql.auto.conf.orig` | PG18 config before the tests (restored) |

The histogram in the table above came from
`pg_wait_tracer --replay -T raw/c3-<scenario>/trace --view histogram -e IO:DataFileRead`
(the harness passed `-e DataFileRead`, which the tool does not match). The traces are kept so anyone can re-run any view.

## Reproducing

Needs: PG18 with pg_wait_sampling and pg_stat_statements preloaded, root, `fio`, pg_wait_tracer, cgroup v2.
Hold `/tmp/pgwt-box-check.lock` on shared boxes. Order: `c3_init.sql` (in a fresh database), set `MemoryMax`,
`RANGE=12000000 ./c3_measure.sh normal`, start fio, `RANGE=12000000 ./c3_measure.sh storage`, `./c3_bloat.sh`
(its own output never closes because the idle session keeps the pipe; the updates take ~10 min), then
`RANGE=200000 ./c3_measure.sh bloat`, `python3 c3_report.py normal storage bloat`.
Cleanup: terminate the idle session, drop the database, `systemctl set-property --runtime postgresql@18-main.service
MemoryMax=infinity CPUQuota=`, restore `postgresql.auto.conf`, restart. Do not use `pkill -f` with a pattern that
also appears in your own ssh command line.
