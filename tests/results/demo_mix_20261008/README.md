# The demo workload's wait-event mix after the 2026-10-08 change

Measurement only. No `src/` or `web/` change; no bound, tolerance or gate
touched anywhere in this directory.

## What was asked

Owner, 2026-10-08: *"demo should demo more or less real picture of wait
events, pgsleep or lock relation looks very weird, So fix this."* `pg_sleep`
left the demo loop in #312. What remained was `Lock:relation` at **71 % of DB
Time** over a 900 s window, against `LWLock:LockManager` 0.1 % and
`IO:DataFileRead` 0.2 %.

## Topology

| component | where |
|---|---|
| PostgreSQL 18, port 5418, `shared_buffers` 128 MB | `pgwt-gate` (`root@2.28.45.47`), cx33, 4 cores |
| daemon | same box, `--daemon --pid <postmaster> -i 1 -T <fresh empty dir> --mode full -v` |
| load | same box: `pgbench -c 4 --rate=25` + `tests/live_loop_workload.py` + **`tests/burst_load.sh`** (new) |
| fixtures | `pgbench_accounts` 1,000,000 rows; `_smoke_io_load` 3,000,000 rows; `_smoke_lockmgr_fanout` 200 partitions. Already provisioned; nothing here builds them |

Window: 936.9 s (the 900 s budget plus the loop's own 20 s tail and
teardown). One capture, fresh empty trace dir, `fidelity: exact`. Whole
measurement held `/tmp/pgwt-box-check.lock` for its duration.

## Files

| file | what |
|---|---|
| `measure_box.sh` | the box-side driver: daemon, three load tiers, liveness polling that FAILS the run if the burst tier refuses, then the query |
| `measure_query.py` | queries the finished trace through `pgwt-server`'s stdin/stdout protocol |
| `mixreport.py`, `mix.txt` | the indented time model, AAS series statistics, `info`, transitions, row counts |
| `raw.json` | every raw response, so any number above can be rechecked without re-running |
| `burst.log` | the burst tier's own log: 14 cycles, every pgbench summary, zero `ABORT` lines |
| `workload.log`, `pgbench.log` | the other two tiers |

## The result

`db_time_ms` 798 352 over 936.9 s, window AAS 0.852 on 4 cores.

| class | ms | % DB Time |
|---|---|---|
| CPU\* | 417 615 | 52.31 |
| Lock | 236 950 | 29.68 |
| &nbsp;&nbsp;`Lock:transactionid` | 116 358 | 14.57 |
| &nbsp;&nbsp;`Lock:advisory` | 83 621 | 10.47 |
| &nbsp;&nbsp;**`Lock:relation`** | **36 970** | **4.63** |
| IO | 111 367 | 13.95 |
| &nbsp;&nbsp;`IO:WalSync` | 78 509 | 9.83 |
| &nbsp;&nbsp;`IO:DataFileRead` | 12 895 | 1.62 |
| &nbsp;&nbsp;`IO:WalWrite` | 5 474 | 0.69 |
| &nbsp;&nbsp;`IO:DataFileFlush` | 5 471 | 0.69 |
| &nbsp;&nbsp;`IO:DataFileWrite` | 4 682 | 0.59 |
| LWLock | 32 120 | 4.02 |
| &nbsp;&nbsp;`LWLock:WALWrite` | 18 307 | 2.29 |
| &nbsp;&nbsp;`LWLock:LockManager` | 13 703 | 1.72 |
| Timeout | 192 | 0.02 |
| IPC | 107 | 0.01 |

`Idle` is 26 471 328 ms outside DB Time (`Client:ClientRead` 18 992 906,
`Other (background)` 6 709 433, `Timeout:CheckpointWriteDelay` 768 470) --
PR #295's accounting, not part of the mix above.

## Read against the asked-for shape, including where it does not match

- **`Lock:relation` 71 % -> 4.63 %.** Present and non-zero, so
  `demo_rehearsal_lib.REQUIRED_WORKLOAD_EVENTS` is satisfied; no longer the
  story of the window. This was the owner's complaint and it is fixed.
- **Lock is mostly `Lock:transactionid`** (14.57 %, the top single wait) --
  hot-row contention, which is what the target profile named.
- **The commit path is visible**, but as `IO:WalSync` (9.83 %, fsync) rather
  than as WAL-buffer LWLock contention. On four cores at ~600 tps, fsync is
  the bottleneck and the WAL insert locks barely queue; that is what a small
  real system does.
- **LWLock is 4.02 %, NOT the largest or near-largest class.** The target
  profile asked for it to be. An earlier pilot of this burst rig measured
  `LWLock:WALWrite` at 25.5 %; here it is 2.29 %. The burst did not get
  weaker -- 18.3 s of `LWLock:WALWrite` in 900 s, 14 clean cycles -- the
  DENOMINATOR grew, because `live_loop_workload.py`'s IO and lock-manager
  batches contribute the 52 % CPU. Reported rather than tuned: the brief said
  to make the load realistic and report what it gives.
- **`Lock:advisory` 10.47 % is the most artificial-looking thing left.** Real
  applications rarely spend a tenth of their database time on advisory locks.
  It is #312's `pg_sleep` replacement and is outside this change's scope.
- **AAS 0.85 of 4 CPUs.** Per 7.8 s bucket: min 0.008, p50 0.809, p90 1.174,
  max 1.897. A DBA would read this as a lightly loaded box -- the base
  `pgbench --rate=25` is a trickle and the bursts are bursts.
- **Sessions: 265 rows** (was ~67). The burst tier's four pgbench
  invocations open 19 clients per ~2.5 min cycle, so 14 cycles leave ~265
  short-lived backends in the window. Connection churn is real in
  production, but this is a large change to what the Sessions tab shows and
  it undoes part of #243's row-count reduction.
- **Transitions: 46 nodes, 50 links** (`total_link_count` 90, `truncated`
  true); was 5 nodes. Richer, possibly too dense to read on a projector.
- **Volume: 5 668 864 events over 936.9 s = 6 050 events/s**, against the
  3 854 events/s the paint budget was pinned at -- **1.57x**. See
  `../demo_paint_20261008/` for what that does to tab paint.
