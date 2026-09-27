# What counts as a clean demo rehearsal

**Pre-registered.** This file is committed BEFORE the first rehearsal attempt of
the sequence it governs. Deciding after a run which of its failures "really
counted" is the single failure mode this document exists to prevent, and it is
the same failure mode a process retro found in the lead's own analysis on
2026-09-27: a conclusion chosen after seeing the data.

If a criterion below turns out to be wrong, it is changed by a commit that says
so and the attempt counter resets. It is never reinterpreted in a report.

## The workload the criteria are written against

`tests/demo_rehearsal.sh`: pgbench, 4 clients, `--rate=25`, plus
`tests/live_loop_workload.py` (holder / waiter / sleeper sessions looping, so
`Lock:relation` and `Timeout:PgSleep` recur in every tick rather than once).
Full mode. `RECENT_WINDOW_S` = 60 s, `TIME_MODEL_TOLERANCE_PCT` = 1.0.

A rehearsal run on a different workload, client count, PG version, box class or
script commit is a different experiment and does not count toward the sequence.

## A rehearsal is CLEAN only if ALL of these hold

Any single failure makes the attempt not clean, however good the rest look.

### 1. The verdict is machine-readable and belongs to this run

- The rehearsal script exits 0.
- Its JSON verdict file says overall true, with a per-assertion reason.
- The verdict carries this invocation's `run.id`
  (`tests/results/demo_rehearsal/run.id`) and it is asserted, not assumed. A
  stale summary from a previous run must not be able to satisfy this.

Prose in a log is not a verdict. If a human has to read narrative to find out
whether it passed, it failed this criterion.

### 2. Raw floors, evaluated BEFORE any ratio or equality

A ratio between two numbers that are both near zero is not evidence. These are
checked first and independently:

- total samples > 0;
- DB Time > 0 on every window checked;
- the window's wait-event list contains **both** `Lock:relation` and
  `Timeout:PgSleep` with non-zero time. This is derived, not chosen: the
  workload creates those two events by construction, so their absence means we
  captured something other than the workload;
- AAS ≥ 0.5 on the 60 s recent window. **Provisional**: its only job is to be
  non-vacuous. It is replaced by half of the first clean rehearsal's own
  measured AAS, recorded here by commit, before the sequence is claimed;
- all 11 tabs reached and rendered;
- the daemon process alive for the whole window.

### 3. Conservation, sampled through the run

- DB Time = CPU\* + Off-CPU\* + Σ waits within `TIME_MODEL_TOLERANCE_PCT` (1%).
- Evaluated **every 5 minutes** through the capture, not once at the end. Every
  sample must pass. A 35-minute run judged on its last 60 seconds is n=1.
- **The identity alone is known to be insufficient and must not be quoted as
  proof of correct attribution.** On the raw/exact path Off-CPU\* is the
  residual `max(0, DB − CPU* − Σwaits)` (`src/compute.c`), so a wait class we
  fail to attribute is absorbed into Off-CPU\* and the identity still closes.
  Only over-attribution can move the gap. Therefore also require:
  - `wait_gap_cpu_ms` ≤ 0.1% of DB Time — CPU measured during wait-labelled
    gaps, which should be ≈0;
  - `cpu_clamped_ms` ≤ 0.1% of DB Time — CPU that had to be clamped away.

  Both are already on the wire (`src/server.c`, documented at
  `src/compute.h`). These are the direct over-attribution detectors; the
  identity is a rounding check.
- Off-CPU\* ≤ 10% of DB Time on a box where clients ≤ cores. Derived from the
  run-queue share measured on 2026-09-27 (4.92 pp, one machine) doubled for
  headroom. Replace with a comparison against the "CPU (waiting for a core)"
  band integral once that plumbing exists.

### 4. The walk

- Zero console errors and zero blank panels across all 11 tabs.
- **Zero known-failing tabs.** Owner ruling, 2026-09-27: "all bugs must be
  actually fixed". There is no per-tab exemption and no "accepted visible
  defect" path. A tab listed in `KNOWN_FAILING_TABS` fails the rehearsal.
- The blink gate green on every tab, with measured fraction ≥ 0.5 on every tab
  — an unmeasured tab is not a passing tab.
- Per-tab time to first paint within a stated bound. A six-second spinner
  passes "no console errors" and "not blank" while being exactly what an
  audience notices.
- Viewport pinned to the resolution the demo will actually be shown at, not the
  driver's default.

### 5. Agreement and freshness

- Cross-tab agreement: DB Time and AAS for the same window must agree within
  the same 1% across overview, timeline and top-events. Disagreeing denominators
  between tabs is the product-facing version of the bookkeeping error this
  project has already made in prose.
- Freshness: at the end of the walk, every time-axis tab's newest bucket is
  within 2 ticks of wall clock. A frozen chart is neither blank nor throwing.

### 6. Daemon integrity

- Ring-buffer and lost-event counters are 0. "The daemon was alive" is not
  "the daemon captured everything".
- Overhead within the envelope recorded in `tests/results/overhead_trend.csv`.

### 7. Nothing failed to execute

No test anywhere in the run exited 126 (not executable) or 127 (command not
found). That is a broken invocation, never a known product bug, and
`KNOWN_FAILING` membership does not excuse it.

### 8. Every known-failing line is read by hand

For each `KNOWN_FAILING` entry that fired, its actual failure reason is compared
against its tracking issue. A listed test failing for a DIFFERENT reason than
the one in its issue makes the attempt not clean.

This is a deliberate, time-boxed substitute for the failure-signature work
(matching `KNOWN_FAILING` by name means any failure of a listed test currently
reads green, including an infrastructure failure). Two rehearsals of human eyes
is an acceptable substitute twice; it is not acceptable indefinitely, and the
signature work is scheduled immediately after the demo.

## The sequence, and what stops it from being rolled

- **Clean is reported as "k consecutive clean of N attempted", never as "two
  clean rehearsals".** N is always stated.
- Every attempt is logged and reported, including the failures, each classified
  by the criterion number it failed and as product / harness / infrastructure.
- An attempt that fails criterion 2's floors for an infrastructure reason (the
  box died, the workload never started) is recorded but does not count toward N.
- **At most 4 counted attempts.** If two consecutive clean runs have not
  happened by then, the result is "not clean" and that is what the owner is
  told. Re-rolling until two land in a row is the same after-the-fact selection
  this document exists to prevent.
- The two clean runs must be on the same tree: same commit, same script commit,
  same box class, same PG version, same pgbench parameters, with no commit in
  between.

## Provenance

Criteria drafted by the lead, challenged and corrected by the standing adviser
(`.claude/agents/adviser.md`) on 2026-09-27 — which supplied criterion 3's
residual-absorption finding, the over-attribution detectors, the sampling
requirement, the cross-tab and freshness checks, and the attempt accounting.
The owner's rulings are quoted inline where they bind.
