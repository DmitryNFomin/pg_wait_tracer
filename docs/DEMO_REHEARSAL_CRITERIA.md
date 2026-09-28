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

## The demo configuration (owner, 2026-09-28)

Pre-registered here so the rehearsal is run against the thing being demoed,
rather than the thing that was convenient to test:

- **PostgreSQL 18** (latest).
- **The same hardware class as today's gate box** — Hetzner `cx33`.

Two consequences follow, and both are deliberate:

1. Rehearsals run on a `cx33`, which is what the throwaway VMs already are, so
   the machine under test matches the machine that will be demoed. A rehearsal
   on any other class does not count toward the sequence.
2. **The client is the owner's Mac** (owner, 2026-09-28: "Demo will be using
   my Mac where you are running now"). So the demo topology is: browser and
   Go bridge on the Mac, ssh to the VM, daemon and PostgreSQL on the VM.
   Nothing eBPF runs on macOS, so the capture is necessarily remote — but the
   client half is the Mac, and that is the half a rehearsal on the VM does not
   exercise at all.

## Two verdicts, never merged into one

A rehearsal produces **two** results and they are reported separately:

- **Capture-side**, walked on the VM: everything that does not depend on the
  client — §1 verdict, §2 floors, §3 conservation, §6 daemon integrity, §7
  execution, §8 known-failing, and the data half of §4 and §5 (console errors,
  blank panels, cross-tab agreement).
- **Mac-side**, walked on the Mac against the same VM in the demo topology:
  §4's time to first paint and viewport, and §5's freshness — *as the audience
  will see them*. These are different quantities from their VM-side
  namesakes. Headless Chromium at DPR 1 on a cx33 over localhost is not real
  Chrome at DPR 2 on Apple silicon over an ssh hop, and a bound measured on
  one does not transfer to the other. The VM's number is kept only as a
  regression detector.

Reported as "capture-side clean k of N, Mac-side clean m of M". **Never as
"two clean rehearsals"** — that would claim the Mac half on the strength of
the VM half.

### Attribution for the Mac side, pre-registered

The Mac is also this project's build machine, and local memory pressure has
already killed two rehearsal attempts (#176). So, decided in advance rather
than after seeing a result:

- a Mac tick recorded with memory pressure other than normal is **void**
  (infrastructure), not a failure;
- a tick over bound with **normal** pressure, while the VM-side walk is clean
  for the same tick, is a **client-side product finding**;
- both sides red is a product failure.

`memory_pressure` and load are recorded per tick in the verdict, so this is
decided by data rather than by "the Mac was busy". For the duration of a
Mac-side walk, no implementing agents and no local VM work run on it — that
cost is real and is accepted deliberately.

### The client, pinned (owner, 2026-09-28: "Screen, chrome")

- **Display: the Mac's built-in screen**, not the projector — `Color LCD`,
  Built-in Liquid Retina, **2880 x 1864 Retina**, `Main Display: Yes`.
- **Viewport: 1440 x 932 CSS at devicePixelRatio 2.** That is what the walk
  pins, since a Retina panel reports twice the CSS resolution.
- **Browser: Safari** (owner, 2026-09-28: "it safari not chrome"). Pin the
  Safari and macOS versions in the verdict; an update between a clean
  rehearsal and the demo invalidates the client half, because rendering, paint
  timing and WebSocket behaviour are exactly what that half measures.

  **Safari must be driven as Safari.** Playwright's `webkit` is a different
  build — different JIT, networking stack, and timer and WebSocket behaviour —
  so measuring WebKit and reporting it as Safari would be the same class of
  error as every instrument defect found on 2026-09-27: measuring a near
  neighbour of the thing and labelling it the thing. `/usr/bin/safaridriver`
  is present, so real Safari is drivable over WebDriver. If it ever cannot be,
  the honest fallbacks are a scripted manual walk with the harness recording,
  or WebKit **explicitly labelled a proxy** with its differences stated — never
  WebKit under Safari's name.

  A Chrome measurement is not evidence for this criterion. The first
  Mac-side walk was built against Chrome before this correction; its findings
  about the bridge, the ssh hop and freshness stand, its paint numbers do not.

**One ambiguity, still open.** An `LG ULTRAFINE` at
6016 x 3384 (UI 3008 x 1692) is also attached to this Mac. "Screen" was
answered against a laptop-screen-or-projector question, so it is read as the
built-in panel — but if the demo is actually driven on the external display,
the pinned viewport is 1504 x 846 CSS at DPR 2 instead, and every Mac-side
first-paint number has to be re-measured on it. Cheap to correct now,
expensive to discover on the day.

### The demo machine (owner, 2026-09-28: "we will provision dedicated node couple days in advance")

A dedicated PG 18 `cx33`, created days ahead, **not** an ephemeral VM. Two
requirements follow from how the janitor works, and both are mechanical:

1. It must **not** carry the `pgwt=ephemeral` label. `tests/hetzner-sweep.sh`
   deletes labelled machines older than its cutoff, and the demo node will be
   days old by definition — precisely the shape the sweep exists to remove.
2. Its name must be in `PROTECTED_NAMES` in `tests/hetzner-sweep.sh`, as a
   second independent guard. The script's own comment says not to rely on the
   missing label alone, because an unlabelled persistent box looks identical
   to an operator's one-off VM.

**Name: `pgwt-stage`, deliberately not `pgwt-demo`.** The rehearsal's own
throwaway VMs are already created as `pgwt-demo-<epoch>-<rand>`, so a
dedicated node called `pgwt-demo` would sit one careless prefix match away
from the machines the sweep is built to delete. `PROTECTED_NAMES` matches
exact names today, but the demo node is the one machine where a future
loosening of that match must not be able to reach.

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
  - **Per-class wait CPU, not a DB-Time fraction.** `Timeout:PgSleep` and
    `Lock:relation` are pure sleeps: a correct implementation records
    essentially no on-CPU time inside them (cpu/dur well under 0.01%,
    bounded by at most a few deadlock-timeout wakeups). **A PgSleep or
    Lock event carrying cpu_ns on the order of a millisecond is the
    over-attribution signature** — a stale wait label, or an `on_cpu_ts`
    that was never closed. That is the check; the per-class figure is the
    discriminating quantity.
  - `cpu_clamped_ms` ≈ 0. Note its limit: the wait branch never clamps
    `cpu_ns > dur` (`src/compute.c:810`), so `cpu_clamped_ms = 0` vouches
    for CPU gaps only, never for wait events.

**Corrected 2026-09-27 — `wait_gap_cpu_ms` as a fraction of DB Time is NOT an
over-attribution detector, and an earlier version of this file wrongly made it
one at ≤ 0.1%.** Two live runs measured 0.376–0.392%, and that is expected on a
correct implementation: the BPF measures exact on-CPU between the wait-start and
wait-end writes, which spans the syscall's own on-CPU work, so an IO wait is
legitimately CPU-bearing — a `pwrite` to page cache is nearly all on-CPU under a
wait label. `src/compute.h`'s "≈0, a sleeping task burns no CPU" is a false
premise for IO classes. In that run: ~4k IO waits in ~40 s carrying 257 ms, i.e.
about 60 µs per event, inside the syscall-overhead envelope. The a-priori bound
the mechanism supports is `wait_gap_cpu_ms ≤ Σ IO-class wait ms + N_wait × c`
with c ≈ 10 µs of entry/exit and watchpoint-handler cost — honest but far too
loose to gate on (~1.2 s for that run). Baselining the DB-Time fraction instead
would be illegitimate: it is a property of the workload's IO mix, not of the
implementation, so it cannot separate a defect from a change of mix.

  Per-class wait CPU is not currently on the wire. Settle it offline, once, over
  any full-mode gate-box trace — per-event `cpu_ns` is in `pgwt_trace_event` and
  `tests/cross_validate.c` already reads it — and record the maximum `cpu_ns`
  seen on a `Timeout:PgSleep` event as the evidence.

- Off-CPU\* ≤ 10% of DB Time on a box where clients ≤ cores. **Provisional and
  weakly sourced**: it doubles a run-queue share of 4.92 pp measured once, on
  one machine, on 2026-09-27 — and that measurement's artifact is not in this
  repository (it lives on the unmerged `agent/observer-bias-study` branch), so
  nobody can currently check it. Treat the number as a placeholder that must be
  replaced either by the committed artifact or by a comparison against the
  "CPU (waiting for a core)" band integral once that plumbing exists. A bound
  no reader can verify is exactly what #200 is about.

**Which counters:** `wait_gap_cpu_ms` and `cpu_clamped_ms` above are the
**time_model response** fields (`src/compute.c` → `src/server.c`, in the same
response the rehearsal already queries). They are NOT the daemon-level
`wait_gap_cpu_ns_total` / `cpu_clamped_total` counters in the metrics blob,
which are structurally always 0 whenever `cpu_ns == PGWT_CPU_NS_UNKNOWN` (the
sampled and legacy paths) and are therefore not a health signal there. Do not
substitute one family for the other.

### 4. The walk

- Zero console errors and zero blank panels across all 11 tabs.
- **Zero known-failing tabs.** Owner ruling, 2026-09-27: "all bugs must be
  actually fixed". There is no per-tab exemption and no "accepted visible
  defect" path. A tab listed in `KNOWN_FAILING_TABS` fails the rehearsal.
- The blink gate green on every tab, with measured fraction ≥ 0.5 on every tab
  — an unmeasured tab is not a passing tab. **Dependency**: "measured fraction"
  does not exist on `master`; it is `MIN_MEASURED_FRACTION` on the unmerged
  `agent/blink-anchor-mount-seq` branch, which also replaces the blink gate
  with the mount-anchored offset sweep. Until that branch lands, this criterion
  cannot be evaluated at all, and a rehearsal run before it is not a counted
  attempt.
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

- `ringbuf_drops_total` = 0 — this is the lost-trace-event counter, and trace
  events are what DB Time is built from. Also `state_map_full_total` = 0 and
  `seen_query_ids_full_total` = 0 (`src/control.c`). All three are already in
  the metrics blob, reachable through pgwt-server's control proxy, which the UI
  itself already calls — no new instrumentation is needed, contrary to an
  earlier assessment that grepped the wrong files.
- **Known blind spot, stated rather than implied:** a lost *lifecycle* event is
  silent. `lifecycle_rb` reserve failures increment nothing (`src/bpf/…` guards
  the write with `if (ev)` and no counter), and the symptom is a backend simply
  absent from the capture — under-attribution, which §3 already establishes the
  conservation identity cannot see either. So a clean run means "no trace events
  were dropped", never "nothing was missed". Closing this needs `src/` work and
  is scheduled after the demo.
- "The daemon was alive" is not "the daemon captured everything".
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

## What the harness actually implements today

The doc is the contract; the harness is what enforces it, and right now the two
are far apart. **Until this table is all "yes", `demo_rehearsal.sh` exiting 0
does NOT mean this document was satisfied** — it means a small fraction of it
was checked. Reading an exit code as the whole contract is precisely the
failure this file exists to prevent, so it is written down here rather than
left to be discovered.

| Criterion | Implemented? |
|---|---|
| §1 exit code + JSON verdict | yes |
| §1 run.id asserted against staleness | no — `run.id` is written (`demo_rehearsal.py:257`) but never checked |
| §2 samples > 0, DB Time > 0 | no |
| §2 `Lock:relation` + `Timeout:PgSleep` present | no |
| §2 AAS ≥ 0.5 on the recent window | no |
| §2 all 11 tabs reached, daemon alive throughout | partial (daemon-log ERROR/FATAL scan only) |
| §3 conservation identity | yes, but ONCE at the end — not the 5-minute cadence |
| §3 `wait_gap_cpu_ms` / `cpu_clamped_ms` ≤ 0.1% | no |
| §3 Off-CPU\* ≤ 10% | no |
| §4 zero known-failing tabs | yes (`build_demo_summary` uses raw `ok`, ignoring the exemption) |
| §4 blink measured fraction ≥ 0.5 | no — blocked on `agent/blink-anchor-mount-seq` |
| §4 time to first paint, pinned viewport | no |
| §5 cross-tab agreement, freshness | no |
| §6 lost-event counters, overhead envelope | no |
| §7 no test exited 126/127 | no |
| §8 known-failing lines read by hand | manual by construction |

Also confirmed present and wrong: `time_model_conservation()` returns pass
outright when `db_time_ms <= 0`, so a rehearsal that captured nothing scores
clean today. That work is in flight on `agent/rehearsal-bypass-suite`, where
each criterion ships with a case proving it can go red.

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
