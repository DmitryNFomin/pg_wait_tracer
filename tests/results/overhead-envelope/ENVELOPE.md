# §6 overhead envelope — demo workload, full mode

Two separate measurement rounds live in this file, answering two different
questions — **do not conflate them**:

1. **8 unthrottled clients, `demos/workload.sh`** (original round, below):
   not the criteria's experiment — §3 of
   `docs/DEMO_REHEARSAL_CRITERIA.md` explicitly disowns the saturated
   8-client regime on a 4-vCPU box, and §6's accepted band ("~28-32% at 4
   clients") isn't comparable to an 8-client number. Kept for the hardware
   range it documents (see "Side by side" below), not as the criteria's
   answer.
2. **4 clients, unthrottled, pgbench + `tests/live_loop_workload.py`**
   (added 2026-10-13, see "## gate-1, 4 clients unthrottled" below): the
   client count the criteria actually specify
   (`docs/DEMO_REHEARSAL_CRITERIA.md:14-15` names `tests/demo_rehearsal.sh`:
   pgbench 4 clients, `--rate=25`, plus `live_loop_workload.py`). This round
   drops `--rate=25` deliberately — see that section's note — so it is an
   **approximation** of the demo workload (same client count and session
   mix, no rate cap), not the demo workload itself.

## Part 1 — 8 unthrottled clients, `demos/workload.sh`

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

**The number to quote for the demo box (8-client regime) is gate-1's:
39.41% (±1.49pp, n=3).** For the 4-client regime the criteria actually
specify, see Part 2 below: **30.31% (±0.58pp, n=3)**.

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

## Cleanup (Part 1)

Both boxes' build trees (`~/overhead-envelope-20261013/src-tree`,
`~binaries`) and large per-run trace directories (`trace_B{1,2,3}`, ~75MB
each) were deleted after logs were copied back. Only
`~/overhead-envelope-20261013/{logs,workload-242.sh,run_envelope.sh,results.csv}`
-equivalent small files were left; a final pass removed the whole
`~/overhead-envelope-20261013` directory on both boxes. No throwaway VM
was created on either box — both are the persistent gate boxes, and
nothing was left running on either.

## Part 2 — gate-1, 4 clients unthrottled (approximation of the demo workload)

Owner-approved re-run (2026-10-13), requested after two advisers
independently found Part 1's 8-client `demos/workload.sh` run does not
answer the question `docs/DEMO_REHEARSAL_CRITERIA.md` §6 actually asks.
Measured on **`pgwt-gate` (gate-1, `root@2.28.45.47`) only** — the demo
box and the only box issue #237 accepts for decisive evidence. Checked
free first (`uptime` showed load average 0.22/0.59/1.32; `fuser
/tmp/pgwt-box-check.lock` returned nothing; no `ci_smoke`/`box-check`/
`pgbench` process running) before starting.

**Why unthrottled, not `--rate=25`:** under a rate cap both arms hit the
same throughput ceiling, so a TPS-based overhead collapses toward zero and
measures nothing. Unthrottled at 4 clients keeps TPS meaningful and
comparable to the registered ~28-32% band. **This is an approximation of
the demo workload, not the demo workload** — `--rate=25` is dropped
deliberately, and `tests/demo_rehearsal.sh` is not run (no walk, no
time-model check, no 35-minute window).

**Workload:** `tests/live_loop_workload.py <duration>` (unmodified, driven
standalone — importable from `tests/` with only stdlib + `testutil`, no
extra install) run concurrently with `pgbench -U postgres -d postgres -c 4
-T 60` (unthrottled — `demos/workload.sh`'s `-j`/`-P`/`-r` flags dropped
along with `--rate=25`; this is plain pgbench, not that script). Both
processes are started together and run for the same 60s window per arm;
the pgbench TPS line is the measurement. Confirmed to run standalone
end-to-end in a dry run before the counted runs (both `PASS` lines from
`Workload`'s own internal checks, clean exit).

**PostgreSQL:** 18.6 (Ubuntu 18.6-1.pgdg24.04+2), port 5418, same
already-provisioned `postgres` database as Part 1 (`pgbench_accounts`
scale 10, 1,000,000 rows, not re-initialized). `pg_stat_activity` showed
only the 8 baseline background-worker rows (no query) before and after
the whole sequence.

**Daemon build:** from `origin/master` at commit `ac515f6` (same commit
Part 1's gate-1 run used, and the commit the task named as current
master), built with `make PGWT_BUILD_VERSION=ac515f6` from a clean `git
archive ac515f6` export (no `.git` in the export, so `git describe`
cannot recover the sha from the box; the local worktree's `git describe
ac515f6` = `v0.13-464-gac515f6` confirms the commit independent of the
box-side build).

**Daemon invocation (B arm):** identical to Part 1 — `./pg_wait_tracer
--daemon --pid <PM_PID> -i 1 -T <trace_dir> --mode full -v`, control
socket confirmed present before the workload starts, `kill` + `wait` at
the end of that run.

**Pairing:** interleaved A,B,A,B,A,B — exact start times are in each
run's `logs-gate1-4c/*.log` banner lines and `results_raw_gate1_4c.csv`.

**Full logs preserved** (not tailed) under `logs-gate1-4c/`: `A{1,2,3}.log`
(banner + pgbench output appended), `A{1,2,3}.pgbench.log` /
`A{1,2,3}.workload.log` (each process's own stdout/stderr), `B{1,2,3}.log`,
`B{1,2,3}.pgbench.log`, `B{1,2,3}.workload.log`, `B{1,2,3}.daemon.log`
(tracer stderr: attach, probe counts, shutdown summary — e.g. B1: "trace
file closed: 4509518 events, 46594845 bytes"). `grep -il error
logs-gate1-4c/*.log` matches nothing on any of the 18 files.

Driver script: `run_envelope_4c.sh` in this directory, run under `flock
/tmp/pgwt-box-check.lock`. It reuses Part 1's `run_envelope.sh` pattern
of a `tee`'d banner line inside a command-substituted function, so
`results_raw_gate1_4c.csv` has the identical cosmetic bug (each banner
line captured into the CSV alongside the TPS value) — preserved unedited
for provenance, same as Part 1. `results_gate1_4c.csv` is the corrected
version, independently re-extracted straight from the untouched per-run
`.pgbench.log` files (`grep -oP 'tps = \K[\d.]+' logs-gate1-4c/{A,B}N.pgbench.log
| tail -1`); its six values are what the table above and all arithmetic
below are computed from.

| run | arm | TPS |
|---|---|---|
| 1 | A (baseline) | 1536.848009 |
| 1 | B (full-mode tracer) | 1073.654740 |
| 2 | A (baseline) | 1547.019100 |
| 2 | B (full-mode tracer) | 1068.032553 |
| 3 | A (baseline) | 1527.019222 |
| 3 | B (full-mode tracer) | 1071.360724 |

- Baseline (A): mean = 1536.96 TPS, sample stddev = 10.00 (n=3)
- With tracer (B): mean = 1071.02 TPS, sample stddev = 2.83 (n=3)
- Overhead per pair: 30.14%, 30.96%, 29.84% — **mean 30.31%, sample
  stddev 0.58 percentage points (n=3)**

### Comparison to the registered band and to the 8-client figure

- **30.31% (±0.58pp) falls inside the registered ~28-32% band** at 4
  clients (`docs/DEMO_REHEARSAL_CRITERIA.md` §6) — unlike the 8-client
  39.41%, which is outside it and was the reason this re-run was
  requested.
- It is **9.1 percentage points below** the 8-client gate-1 figure
  (39.41% vs 30.31%), consistent with the 8-client regime being the
  saturated one §3 disowns: more concurrent clients competing for the same
  4 vCPUs gives the watchpoint tier relatively more contention to add
  overhead to, on top of the baseline already being more saturated.
- Both gate-1 figures (this one and Part 1's) are measured on the same
  box and the same `ac515f6` daemon build, so this comparison, unlike
  Part 1's gate-1-vs-gate-2 one, is a controlled single-box, single-build
  before/after on client count and throttling alone.

### Did including `live_loop_workload.py` matter here?

Yes — it was run standalone successfully (plan B was "pgbench alone,
labelled as such" if this failed; it did not fail), so this round, unlike
Part 1, uses the actual workload the criteria name
(`tests/demo_rehearsal.sh`'s own two components, run directly instead of
through that script's 35-minute walk/checks).

## Cleanup (Part 2)

The build tree (`/root/overhead-envelope-20261013-4c`, archive + binary +
three ~45MB trace dirs) and the uploaded source tarball
(`/root/src-ac515f6.tar.gz`) were deleted from gate-1 after logs were
copied back (`du -sh /root/* | grep overhead` → no match; no stray
`pg_wait_tracer`/`pgbench`/`live_loop_workload.py` process from this run
left running). No VM was created. A second, unrelated process
(`/root/pgwt-demo-rehearsal-HEAD/...`) appeared on the box immediately
after this run finished — a different agent's job, in a different
directory, untouched by this cleanup.

## Not done (explicitly out of scope per the task)

- No `make box-check` run (not required for this task).
- No demo rehearsal run (`tests/demo_rehearsal.sh` itself was not
  invoked in either part — Part 2 runs its two components directly).
- No change to `src/`, `demos/`, `docs/`, or any test.
