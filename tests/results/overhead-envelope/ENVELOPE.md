# §6 overhead envelope — demo workload, full mode

Satisfies `docs/DEMO_REHEARSAL_CRITERIA.md` §6's pre-demo-attempt-one
precondition: n=3 paired A/B TPS runs with the **demo workload**
(`demos/workload.sh`), in `--mode full`, on the demo box.

Measured on **both** persistent gate boxes, on the coordinator's request:
`pgwt-gate` (gate-1) is the box `scripts/demo-rehearsal.sh` prefers and
where counted rehearsal attempts actually run, so it is the number that
answers "what is the overhead?" on stage. `pgwt-gate-2` (gate-2) was
measured first (gate-1 was busy with PR #383's CI at the time) and is kept
alongside it, not replaced: per issue #366 the two boxes are **not**
comparable (gate-2 has ~2.8x gate-1's memory bandwidth; gate-1 runs
`Mitigation: PTI`), so having both gives a hardware range instead of a
single point estimate, and independently documents that asymmetry on a
real workload.

**The number to quote for the demo box is gate-1's: 39.41% (±1.49pp,
n=3).**

## Common setup (same method on both boxes)

- **pgbench parameters (fixed inside `demos/workload.sh`):** `-c 8 -j 2`
  (8 clients, 2 threads), `-T 60` (60s duration), `-P 10 -r` (10s progress
  reporting, per-statement latencies), default TPC-B-like script, no
  `--select-only`.
- **Workload:** `demos/workload.sh` at commit
  `c0ab0e0eaf97b14e2071ebb896910edc5d92687b` ("Demo workload: replace
  pg_sleep lock-hold with a real sort spill (#242)") — identical
  (SHA-256-verified, `workload-242.sha256`) to the version now on
  `origin/master` (`ac515f6`, merged via #383) and to the one on
  `agent/round2-demo-fixes` at measurement time. This branch
  (`agent/overhead-envelope`) does not modify `demos/`; the script was
  copied to each box only for execution, and each run's full stdout is in
  `logs-gate2/` / `logs-gate1/`.
- **PostgreSQL:** 18.6 (Ubuntu 18.6-1.pgdg24.04+2), port 5418, each box's
  already-provisioned `postgres` database (`pgbench_accounts` scale 10,
  1,000,000 rows — read-only, never re-initialized, per issue #133's
  convention in `tests/test_overhead.sh`). No other active sessions on
  either instance during its run (`pg_stat_activity` checked clear
  beforehand).
- **Daemon invocation (B arm):** `./pg_wait_tracer --daemon --pid <PM_PID>
  -i 1 -T <trace_dir> --mode full -v`, started before the workload, given
  up to 30s to create its control socket (succeeded well under that on
  every run on both boxes), left running for the full workload, then
  `kill` + `wait` at the end of that run.
- **Pairing:** interleaved A,B,A,B,A,B (not AAA/BBB) on both boxes — exact
  run order and wall-clock start times are in each box's
  `results_raw_gate{1,2}.csv` and each `logs-gate{1,2}/*.log` file's
  `=== ... start <ISO8601> ===` banner.
- **Full logs preserved** (not tailed) under `logs-gate2/` and
  `logs-gate1/`: `A1.log`..`A3.log` (baseline), `B1.log`..`B3.log` (with
  tracer, includes the workload's own stdout/stderr), `B1.daemon.log`..
  `B3.daemon.log` (the tracer daemon's own stderr: attach, probe counts,
  shutdown summary).
- Driver script: `run_envelope.sh` in this directory, used unmodified on
  both boxes, run under `flock /tmp/pgwt-box-check.lock` so no concurrent
  box-check could collide with it. Its own `results.csv` capture has a
  cosmetic bug (`tee` inside a command-substituted function also captured
  the `=== ... start ===` banner line into the CSV field) — preserved
  unedited as `results_raw_gate{1,2}.csv` for provenance.
  `results_gate{1,2}.csv` are the corrected versions, each TPS re-extracted
  directly from the untouched per-run logs (`grep -oP 'tps = \K[\d.]+'
  logs-gateN/{A,B}M.log | tail -1`); the numbers match what is quoted
  below.
- No installs on either box (system `clang`/`gcc`/`libbpf`, already
  USDT-capable, built both binaries). No `make box-check` or rehearsal run.
  Both boxes' ~36MB build trees under `~/overhead-envelope-20261013` were
  deleted after collecting logs (see "Cleanup" below).

## gate-2 (`root@142.132.191.18`, AMD EPYC-Rome, ~2445 MHz, 4 vCPU, 7 GB)

Measured first, while gate-1 was occupied by PR #383's CI
(`ci_smoke.sh --pg-version 18`, confirmed via `fuser
/tmp/pgwt-box-check.lock` showing the GitHub Actions runner's PIDs).

**Daemon build:** from this branch's tree at commit `45d7399` (`origin/master`
HEAD when this branch was created; `git describe` = `v0.13-463-g45d7399`).

| run | arm | TPS |
|---|---|---|
| 1 | A (baseline) | 2801.819075 |
| 1 | B (full-mode tracer) | 1857.728417 |
| 2 | A (baseline) | 2840.053177 |
| 2 | B (full-mode tracer) | 1857.008170 |
| 3 | A (baseline) | 2844.049802 |
| 3 | B (full-mode tracer) | 1832.371605 |

- Baseline (A): mean = 2828.64 TPS, sample stddev = 23.31 (n=3)
- With tracer (B): mean = 1849.04 TPS, sample stddev = 14.44 (n=3)
- Overhead per pair: 33.70%, 34.61%, 35.57% — **mean 34.63%, sample stddev
  0.94 percentage points (n=3)**

## gate-1 (`root@2.28.45.47`, Intel Xeon Skylake, ~2095 MHz, 4 vCPU, 7 GB,
`Mitigation: PTI`) — the demo box

Measured after PR #383 merged and gate-1's CI job finished (confirmed free
via `uptime` + `fuser /tmp/pgwt-box-check.lock` returning nothing, plus no
`ci_smoke`/`box-check` process running).

**Daemon build:** from `origin/master` at commit `ac515f6` (#383, merged
after the gate-2 run — carries #376/#315/#266/#242), built with
`make PGWT_BUILD_VERSION=ac515f6` from a clean `git archive ac515f6`
export (the exported tree has no `.git`, so `git describe` cannot recover
the real sha; it is pinned explicitly via the Makefile's documented
override instead).

| run | arm | TPS |
|---|---|---|
| 1 | A (baseline) | 1745.361915 |
| 1 | B (full-mode tracer) | 1044.169656 |
| 2 | A (baseline) | 1717.156381 |
| 2 | B (full-mode tracer) | 1024.187341 |
| 3 | A (baseline) | 1678.180426 |
| 3 | B (full-mode tracer) | 1045.625230 |

- Baseline (A): mean = 1713.57 TPS, sample stddev = 33.73 (n=3)
- With tracer (B): mean = 1037.99 TPS, sample stddev = 11.98 (n=3)
- Overhead per pair: 40.17%, 40.36%, 37.69% — **mean 39.41%, sample stddev
  1.49 percentage points (n=3)**

## Side by side

| | gate-2 (AMD EPYC-Rome) | gate-1 (Intel Skylake, PTI) — demo box |
|---|---|---|
| Daemon build | `45d7399` | `ac515f6` (#383) |
| Baseline mean TPS | 2828.64 | 1713.57 |
| With-tracer mean TPS | 1849.04 | 1037.99 |
| Overhead mean | 34.63% | **39.41%** |
| Overhead spread (n=3) | ±0.94pp | ±1.49pp |

Gate-1's baseline throughput is itself ~40% lower than gate-2's on the
identical workload (1713.57 vs 2828.64 TPS) — consistent with #366's
memory-bandwidth asymmetry and gate-1's PTI mitigation, independent of the
tracer. The tracer's overhead is also higher on gate-1 (39.41% vs 34.63%),
which is the expected direction if PTI's per-syscall cost and the lower
memory bandwidth both bite harder under the watchpoint tier's additional
per-event work — consistent with, not proof of, the mechanism named in
#366.

## Did building from `ac515f6` (#383's v6 summary-writer change) move the
number?

The two runs are not a controlled before/after on the same box (gate-2 ran
`45d7399`, gate-1 ran `ac515f6`, and the boxes differ), so this section is
a judgement call, not a measurement, and is stated as such.

`git diff 45d7399 ac515f6 -- src/` touches `summary_writer.c`,
`summary_writer.h`, `event_stream.c`, `escalation.c`, `compute.c`,
`control.c`. The capture-path change (`src/event_stream.c`,
`src/summary_writer.c`) is: resolve the pid's io_worker category flag
once per event (a hash/array lookup already being done, just hoisted a
few lines earlier and reused instead of computed twice — `pa` /
`cat_flag` used to be resolved again ~65 lines later; now it is computed
once and passed down), then branch on one `bool` in `accum_event` to skip
five accumulator writes for io_worker records (and, on an io_worker's
EXIT, free a qattr slot instead of walking the deferred-attribution path).
There is no new syscall, lock, allocation, or I/O on this path — it is at
most a few extra branches and one pointer dereference per captured event,
and for io_worker events it does *less* work than before (it skips
`summary_qattr_step`). My read: this is not a plausible source of a
4-5 percentage-point shift; it is far smaller than the hardware/mitigation
gap between the two boxes, which is the much larger and better-attested
effect (#366). I would not block on re-running gate-2 from `ac515f6` to
confirm this, but flag it as a reasonable thing to re-check if a future
`src/` change on the capture path is suspected of moving overhead instead.

No errors in any log on either box (`grep -i 'error'` matches are only
pgbench's own "0 failed" progress lines; the only daemon WARN lines seen
are `target effective CPU capacity unknown` and `group 'dba' not found`,
present identically on both boxes and not specific to this measurement).

## Interpretation

- **gate-1 (the demo box): 39.41%** — above both the owner's accepted ~35%
  working figure and the README's documented 6-30% range. This is the
  number that should be quoted if asked on stage.
- gate-2: 34.63% — closer to, but still above, the ~35% figure, and also
  above the documented 6-30% band.
- Both are higher than `tests/test_overhead.sh --quick`'s 28.80% (4-client)
  and 35.60%/36.02% (peak) figures from pgbench's *default* workload; this
  envelope's workload is heavier (8 clients plus concurrent `LOCK TABLE
  pgbench_branches` holders each running a real external-merge sort, plus
  background heavy-read queries), so higher overhead than the plain
  pgbench default is expected on both boxes, not a contradiction.
- n=3 per box; spreads are small relative to the box-to-box gap (0.94pp
  and 1.49pp vs a 4.8pp difference between boxes), so the two figures are
  not noise-indistinguishable from each other.

## Cleanup

Both boxes' build trees (`~/overhead-envelope-20261013/src-tree`,
`~binaries`) and large per-run trace directories (`trace_B{1,2,3}`, ~75MB
each) were deleted after logs were copied back. Only
`~/overhead-envelope-20261013/{logs,workload-242.sh,run_envelope.sh,results.csv}`
-equivalent small files were left; a final pass removed the whole
`~/overhead-envelope-20261013` directory on both boxes. No throwaway VM
was created on either box — both are the persistent gate boxes, and
nothing was left running on either.

## Not done (explicitly out of scope per the task)

- No `make box-check` run (not required for this task).
- No demo rehearsal run.
- No change to `src/`, `demos/`, or any test.
