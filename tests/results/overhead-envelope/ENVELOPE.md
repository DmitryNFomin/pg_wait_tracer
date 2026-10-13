# §6 overhead envelope — demo workload, full mode

Satisfies `docs/DEMO_REHEARSAL_CRITERIA.md` §6's pre-demo-attempt-one
precondition: n=3 paired A/B TPS runs with the **demo workload**
(`demos/workload.sh`), in `--mode full`, on a persistent gate box.

## Setup

- **Box:** `pgwt-gate-2` (root@142.132.191.18, Hetzner, AMD EPYC-Rome,
  ~2445 MHz, 4 vCPU, 7 GB RAM). `pgwt-gate` (root@2.28.45.47) was busy
  running CI (`ci_smoke.sh --pg-version 18`, GitHub Actions self-hosted
  runner job `38391620086-1-capture-18`, PR #383) at measurement time —
  confirmed via `fuser /tmp/pgwt-box-check.lock` showing the runner's
  shell/sudo PIDs — so gate-2 was used instead to avoid starving CI.
  Per #366, cross-box timing comparisons are invalid; this entire envelope
  is single-box (gate-2 only).
- **PostgreSQL:** 18.6 (Ubuntu 18.6-1.pgdg24.04+2), port 5418, the box's
  already-provisioned `postgres` database (`pgbench_accounts` scale 10,
  1,000,000 rows — read-only, never re-initialized, per issue #133's
  convention in `tests/test_overhead.sh`). No other active sessions on the
  instance during the run (`pg_stat_activity` checked clear beforehand).
- **pgbench parameters (fixed inside `demos/workload.sh`):** `-c 8 -j 2`
  (8 clients, 2 threads), `-T 60` (60s duration), `-P 10 -r` (10s progress
  reporting, per-statement latencies), default TPC-B-like script, no
  `--select-only`.
- **Workload:** `demos/workload.sh` at commit `c0ab0e0eaf97b14e2071ebb896910edc5d92687b`
  ("Demo workload: replace pg_sleep lock-hold with a real sort spill
  (#242)") — the version on `agent/round2-demo-fixes` at the time of this
  measurement, NOT the one on `origin/master` (which still has the old
  `pg_sleep`-based lock hold). This branch (`agent/overhead-envelope`) is
  based on `origin/master` per the task's instructions, and does not modify
  `demos/`; the #242 script was copied to the box only for execution
  (`tests/results/overhead-envelope/logs/` captures its actual stdout each
  run) and is not part of this branch's tracked `demos/` tree.
  SHA-256 of the executed script: see `workload-242.sha256` in this
  directory.
- **Daemon build:** `pg_wait_tracer` / `pgwt-server` built on gate-2 from
  this branch's tree (`git describe` on the source worktree:
  `v0.13-463-g45d7399`, commit `45d7399` — `origin/master` HEAD at the time
  this branch was created, identical to this branch's own HEAD since
  `demos/`, `src/` and tests were not touched). Built with the box's system
  `clang`/`gcc`/`libbpf` (USDT-capable, no bundled libbpf needed). No
  install step — binaries run in place from the build tree.
- **Daemon invocation (B arm):** `./pg_wait_tracer --daemon --pid <PM_PID>
  -i 1 -T <trace_dir> --mode full -v`, started before the workload, given
  up to 30s to create its control socket (succeeded in all 3 runs in well
  under that), left running for the full workload, then `kill` + `wait` at
  the end of that run (full startup/attach/shutdown logs are in
  `logs/B{1,2,3}.daemon.log`).
- **Pairing:** interleaved A,B,A,B,A,B (not AAA/BBB) — exact run order and
  wall-clock start times are in `results_raw.csv` and each `logs/*.log`
  file's `=== ... start <ISO8601> ===` banner.
- **Full logs preserved** (not tailed) under `tests/results/overhead-envelope/logs/`:
  `A1.log`..`A3.log` (baseline), `B1.log`..`B3.log` (with tracer, includes
  the workload's own stdout/stderr), `B1.daemon.log`..`B3.daemon.log` (the
  tracer daemon's own stderr: attach, probe counts, shutdown summary).
- Driver script used: `run_envelope.sh` in this directory (ran under
  `flock /tmp/pgwt-box-check.lock` on gate-2 so no concurrent box-check
  could collide with it). The driver's own `results.csv` capture has a
  cosmetic bug (`tee` inside a command-substituted function also captured
  the `=== ... start ===` banner line into the CSV field) — preserved
  unedited as `results_raw.csv` for provenance. `results.csv` in this
  directory is the corrected version, with each TPS value re-extracted
  directly from the untouched per-run logs in `logs/` (`grep -oP 'tps =
  \K[\d.]+' logs/{A,B}N.log | tail -1`); the numbers match what is quoted
  below.

## Results

Per-run TPS (`tps = ... (without initial connection time)`, extracted from
each full pgbench log), in run order:

| run | arm | TPS |
|---|---|---|
| 1 | A (baseline) | 2801.819075 |
| 1 | B (full-mode tracer) | 1857.728417 |
| 2 | A (baseline) | 2840.053177 |
| 2 | B (full-mode tracer) | 1857.008170 |
| 3 | A (baseline) | 2844.049802 |
| 3 | B (full-mode tracer) | 1832.371605 |

- **Baseline (A):** mean = 2828.64 TPS, sample stddev = 23.31 (n=3)
- **With tracer (B):** mean = 1849.04 TPS, sample stddev = 14.44 (n=3)
- **Overhead, computed per pair** `(A_i - B_i) / A_i`: 33.70%, 34.61%,
  35.57% — mean 34.63%, sample stddev 0.94 percentage points (n=3)
- **Overhead via the two arm means:** (2828.64 - 1849.04) / 2828.64 =
  **34.63%**

No errors in any log (`grep -i 'error'` matches are only pgbench's own
"0 failed" progress lines and two benign daemon WARN lines present in every
`--mode full` run on this box: `target effective CPU capacity unknown` and
`group 'dba' not found`, neither specific to this measurement).

## Interpretation

- This lands **above** both the owner's accepted ~35% working figure (close
  to it: 34.6% vs ~35%) and the README's documented 6-30% range for the
  full watchpoint tier — **34.6% is above the documented 6-30% band**,
  stated plainly as the task instructions require rather than rounded down
  into it.
- This is higher than `tests/test_overhead.sh --quick`'s 28.80% (4-client)
  and 35.60%/36.02% (peak) figures from pgbench's *default* workload; this
  envelope's workload is heavier (8 clients plus concurrent `LOCK TABLE
  pgbench_branches` holders each running a real external-merge sort, plus
  background heavy-read queries), so a higher overhead than the plain
  pgbench default is expected, not a contradiction.
- Single box (gate-2), single build, n=3 — the spread across the 3 pairs is
  small (0.94pp), so this is not noise-dominated, but it is also not a
  claim about gate-1 or about any other PG version/scale.

## Not done (explicitly out of scope per the task)

- No `make box-check` run (not required for this task).
- No demo rehearsal run.
- No change to `src/`, `demos/`, or any test.
