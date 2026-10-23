# Reruns for the blog review: case 4 observer cost and case 1 fix F2 (gate-3, 2026-10-22/23)

> Raw logs and trace rings for this directory (files over 1 MB and all `.gz`/`trace/`): [`reruns-20261023-raw.tar.gz`](https://github.com/DmitryNFomin/pg_wait_tracer/releases/tag/evidence-20261023) in the `evidence-20261023` release. Untar it inside `tests/results/` to restore the full directory.

Two open points from the review of the write-up:

1. **Case 4, bloat, `statement_timeout` 10 ms:** timeouts were 7 with `capture = off` and 93 with `capture = trace`
   (single runs, `../case4-case5-patch-20261022`). The patch, the ring dumps every 5 s and noise were not separated.
2. **Case 1, fix F2 (restock job in 5-row chunks):** a single run (`../checkout-tail-20261022`).

**Short version.**

- **Case 4: the extra timeouts come from reading the rings during the run.** Three rounds, three arms each. Capture
  on without reading the rings vs capture off: no measurable difference (11 / 2 / 0 vs 87 / 22 / 0 timeouts; both are
  noise around a small number). Capture on and the rings read every 5 s: 271 in total vs 109 off, and 2% less
  throughput.
- **Case 1: F2 lowers the count of slow requests in every pair, but p99.9 is not usable here.** The restock job ran
  in 8 ms instead of 17 ms (faster VM), so the lock tail was smaller. Four of the six runs also had short server-wide
  stalls (10–150 ms each, a few per run, cause not found) that dominate p99.9 in both arms. With stalls excluded by
  a definition that flags nothing in any original run (below), requests over 5 ms, baseline vs F2: 125 vs 0, 300 vs
  24, 334 vs 245.

Plain SQL, a real disk, no `pg_sleep`, no injected delays.

## Setup

- **Box:** pgwt-gate-3 re-created from snapshot `pgwt-gate-2-20261022`, cpx32: 4 vCPU AMD EPYC (KVM), 8 GB. A
  new VM, so a different host from the earlier runs: it was faster (case 4 bloat: ~3,900 vs ~2,900 lookups/s;
  average read 61 vs 81 µs).
- **Server:** PostgreSQL 20devel, `wet-v12` tip `f8abf1f`, same build and config as `../case4-case5-patch-20261022`
  (port 5419, unit `pgwet`, `MemoryMax=1200M`, `shared_buffers` 128MB, ring 4MB default, pg_wait_sampling 1.1.11 and
  pg_stat_statements loaded). Case 1 originally ran on `wet-v11`; that makes no difference here, because case 1 runs with
  capture off.
- Data: `c3` (`c2_init.sql`, 12 M rows) and `shop` (`shop_init.sql`), loaded fresh.
- Order (`setup_and_run.sh`, `rerun.sh`): case 1 first, then the bloat condition (`bloat.sh`), then case 4.

## Case 4: bloat, 10 ms timeout (`c4.sh` with a new 5th argument, `c4rep.py`)

`c4.sh … trace none` = capture on, no ring reads during the 60 s window (only `by_statement()` after it).
`c4.sh … trace every5` = as in the original series C. Caches dropped before each run; arm order rotated per round
(o n t / n t o / t o n).

| Arm | Timeouts (round 1 / 2 / 3) | Total | Lookups/s (mean) | p99 ms |
|---|---|---|---|---|
| capture off | 87 / 22 / 0 | 109 | 3,885 | 5.5–5.9 |
| capture trace, rings not read in the window | 11 / 2 / 0 | **13** | 3,912 | 5.4–5.9 |
| capture trace, rings read every 5 s | 57 / 137 / 77 | **271** | 3,798 | 5.9–6.1 |

- Capture on, rings not read: no measurable difference from capture off, three rounds each. Do not read 13 vs 109
  as an improvement: per round the arms are 11 / 2 / 0 and 87 / 22 / 0.
- Reading the rings during the run does cost: about 2.5× the timeouts of the off arm and −2% throughput. Each
  read copies a 4MB ring per backend, 8 backends every 5 s, on the same 4 vCPUs.
- Same answer as before from the rings: timed-out executions had 31.5–31.9 reads at 378–441 µs average.
- pg_wait_sampling: DataFileRead 93.3–94.1% in all nine runs.

## Case 1: F2 repeated (`sh.sh`, `shrep.py`)

Baseline (R0, restock job in one statement) and F2 (5-row chunks), alternated, 3 each; capture off; 150 s runs, 120 s
window; 2,000 requests/s.

| Run | Mean ms | p99 | p99.9 | Max | > 5 ms | **> 5 ms outside stalls** | Stalls (count, total) | Peak late requests |
|---|---|---|---|---|---|---|---|---|
| R0-1 | 0.54 | 0.94 | 3.38 | 14.1 | 125 | **125** | 0 | 4 |
| F2-1 | 0.54 | 0.99 | 1.82 | 4.5 | 0 | **0** | 0 | 0 |
| R0-2 | 0.60 | 1.21 | 23.95 | 70.0 | 779 | **300** | 10, 180 ms | 114 |
| F2-2 | 0.58 | 0.99 | 26.04 | 85.1 | 381 | **24** | 2, 153 ms | 165 |
| R0-3 | 0.60 | 1.13 | 20.33 | 83.2 | 832 | **334** | 9, 170 ms | 157 |
| F2-3 | 0.55 | 1.07 | 10.81 | 35.6 | 539 | **245** | 7, 77 ms | 61 |

**Stall definition (`stalls3.py 20`).** A request is "late" once it is more than 5 ms past its scheduled start
(pgbench `-R` latency counts from the schedule). A stall is a moment when at least 20 requests are late at once. A
slow request is excluded if its slow phase (scheduled start + 5 ms to end) overlaps a stall.

Why 20: a lock convoy only delays the requests on the job's locked rows (100 of 1,000 items). In the six original
runs (`../checkout-tail-20261022`: R0b, R0c, R1b, F1a, F1b, F2), convoys included, the peak number of late requests
was 6–18, so the rule flags nothing there; their counts are unchanged. In the stall runs here it reached 61–165.

Rejected definitions (`stalls.py`, `stalls2.py`, kept for the record):
- Seconds with ≥ 20 requests over 5 ms (first version): count-based, and a convoy can produce that too. The original
  runs have 12–14 bins of 100 ms per run with ≥ 10 slow requests.
- Seconds or 100 ms bins with a high median latency: stalls are too short. Even in a stall second the median stays at
  ~0.5 ms, and F2-3's stalls do not move any bin's median.
- Seconds where ≥ 12 of 16 pgbench clients had a slow request: the original runs trip it 1–11 times per run (8–11 in the baselines).

Sensitivity (`stalls4.py`): most of F2-3's remaining 245 (158) end within 0.5 s after a stall, as the backlog
drains. Also excluding slow requests within 2 s of a stall: 125 vs 0, 192 vs 5, 205 vs 87. With a threshold of
30 instead of 20: 125 vs 0, 344 vs 26, 420 vs 289. Every variant puts F2 below its paired baseline.

- Sampler on the UPDATE: `Lock:transactionid` 22% of its samples in R0, 0–1% in F2. Overall profile: WalSync 37%,
  CPU 29%, DataFileRead 13–18%, Lock:transactionid 1.5%.
- Restock job: p50 8.3 ms (full), 16 ms (all 20 chunks), vs ~17 ms per full job on the original box. So the lock tail
  is about half the size: the original baseline had ~970 requests > 5 ms and p99.9 11 ms.
- Stalls: not at checkpoint times (checkpoints at 21:45:19, 21:50:19, 21:55:19 UTC; stall seconds at 21:47:01–21:47:33,
  21:48:47, 21:49:29, 21:50:39–21:51:55, 21:53:34–21:54:46). Most likely the VM's disk (WalSync is the top sampled
  wait); not verified. They dominate p99.9, so the comparison uses requests over 5 ms outside stalls.

## Files

| Path | Content |
|---|---|
| `raw/setup_and_run.sh`, `raw/rerun.sh` | server setup, data load and run order |
| `raw/c4.sh`, `raw/c4rep.py`, `raw/client.py`, `raw/bloat.sh`, `raw/c2_init.sql` | case 4 harness (`c4.sh` gains `DUMP`) |
| `raw/sh.sh`, `raw/shrep.py`, `raw/shop_init.sql`, `raw/checkout.sql`, `raw/restock*.sql` | case 1 harness |
| `raw/c4-report.txt`, `raw/c1-report.txt` | report output |
| `raw/stalls3.py`, `raw/stalls4.py`, `raw/stalls-report.txt` | stall definition and sensitivity (`stalls.py`, `stalls2.py`: rejected definitions) |
| `raw/c4-bl-{o10,n10,t10}-{1,2,3}`, `raw/sh-{R0,F2}-{1,2,3}` | per run (latency, rings and requests gzipped) |
| `raw/server.log`, `raw/all.log`, `raw/bloat.log` | server log (checkpoints), batch log |
