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
`tests/live_loop_workload.py`. Full mode. `RECENT_WINDOW_S` = 60 s,
`TIME_MODEL_TOLERANCE_PCT` = 1.0.

`tests/live_loop_workload.py` (issue #214) loops five persistent sessions for
the whole run, not just the two the §2 floors need:
- `holder` / `waiter` / `sleeper`: `Lock:relation` and `Timeout:PgSleep`
  recur in every tick — unchanged since issue #157, and what §2's floors and
  the conservation check are written against.
- `reporter`: rotates four structurally distinct SELECTs every tick (a
  catalog lookup, a CPU-bound aggregate, and two differently-shaped
  `pg_sleep` calls) — the query-id and duration variety Queries, Histogram,
  Waterfall and Scatter need to show more than a single flat line.
- `row_holder` / `row_waiter`: a hot-row `UPDATE` contended by two backends,
  producing `Lock:transactionid` — a second, distinct wait class from the
  table-level `Lock:relation` above, realistic because contention on a
  shared counter/status row is one of the most common real-world lock
  waits.

`tests/demo_workload_coverage.py` is the machine-checkable half: given a
trace dir this workload produced, it queries every tab's own endpoint and
applies the per-tab condition (`python3 tests/demo_workload_coverage.py
--trace-dir DIR`); its pure per-tab checkers are unit-tested by
`tests/test_demo_workload_coverage.py`, wired into `scripts/check.sh`. It also
runs inside `tests/demo_rehearsal.py`'s own end-of-capture checks
(`extra_checks["tab_coverage"]`, before the trace dir is torn down, over the
trailing `WATERFALL_LIVE_WINDOW_S` = 900s rather than the whole capture — a
window_too_large refusal from pgwt-server is recorded as its own "could not
evaluate" outcome, never conflated with a genuinely empty tab) — an empty tab
on a real rehearsal fails that rehearsal's `ok`, not just the standalone tool
a human has to remember to run. The Concurrency tab's check is **CPU-count-
relative** (peak AAS >= the capture box's own `num_cpus`, not a fixed
number) — measured once, on today's 4-vCPU `cx33`, with a comfortable 9.00-
vs-4 margin; a future box-class change moves this gate's difficulty, so
treat that as a deliberate decision, not a surprise discovered mid-rehearsal.

A rehearsal run on a different workload, client count, PG version, box class or
**tag** (see "The sequence, and what stops it from being rolled" below) is a
different experiment and does not count toward the sequence.

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

## Owner pre-flight checklist (run ONCE per rehearsal window)

Every one of these was discovered the hard way on 2026-09-28, one at a time,
each costing the owner a separate interruption. They are collected here so a
rehearsal window costs him **one interaction instead of a dozen**. The harness
asserts each of them and **fails fast naming the unmet one** — it never retries
into an opaque error and never quietly proceeds.

Before saying "go":

1. **Auto-lock off.** System Settings → Lock Screen → "Require password after
   screen saver begins or display is turned off" → **Never**, and display sleep
   set long. macOS refuses WebDriver fullscreen on a locked session, and the
   screen re-locks *during* the several minutes of VM provisioning — so
   unlocking once is not enough. Restore afterwards.
2. **Page zoom at 100% on the pgwt tab** (`Cmd+0`). Safari persists zoom
   per-hostname: a 115% setting on `localhost` silently shrank the viewport by
   15% and produced a layout nobody intended. The harness navigates via
   `127.0.0.1`, which carries no such setting, but the owner's own tab is
   still whatever he last left it at.
3. **Display awake and the session unlocked** at the moment of starting.
4. **No multipass or other local VMs running** (`multipass list`, then
   `multipass stop <name>`). Two Ubuntu VMs were consuming CPU during the first
   attempts.
5. **Time Machine not mid-backup**, and no Photos media analysis running. Both
   appeared in the top CPU consumers during a measurement.
6. **Second display detached** if the demo is on the built-in panel.
   WindowServer drew 40.7% CPU compositing a 6K external alongside the Retina
   panel, versus 19.7% with it detached.
7. **Quit heavy apps** that will not be part of the demo.

Two things that are the harness's job, not the owner's, and are listed here
only so nobody asks him for them again: holding the display awake (the harness
owns its own `caffeinate` with a trap) and cleaning up its own processes and
VMs. **The owner should never be asked to kill an orphaned shell.** That
happened twice on 2026-09-28 and was a missing trap, not a thing for him to do.

### What the harness must do with this list

- Assert every item it can observe — lock state via `CGSSessionScreenIsLocked`,
  display sleep, achieved viewport, effective zoom, load and memory pressure.
- On an unmet precondition: **stop, name it, do not retry.** An opaque failure
  costs an owner interaction; a named one costs a setting change.
- Record every observed value in the verdict, so a reader can tell later which
  conditions produced a number.

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
- **Presented FULL SCREEN** (owner, 2026-09-28). Historically pinned to
  Safari's measured **1710 x 1069 CSS at devicePixelRatio 2**; `screen`
  reports 1710 x 1107 logical, full screen leaves 38 px of chrome, and the
  content layer is 3420 x 2138 physical.

  **An earlier pin of 1440 x 932 was wrong and is retracted.** It came from
  dividing the panel's 2880 x 1864 by the DPR of 2, which assumes macOS maps
  physical to logical at exactly the device pixel ratio. This Mac runs the
  built-in display in a *scaled* mode: 1710 x 1107 logical at DPR 2, rendering
  3420 x 2214 and downsampling to the 2880 x 1864 panel. So the derived number
  was 19% too narrow, and any paint or layout measurement taken at it does not
  describe the demo.

  Two related traps, both measured rather than assumed:
  - **A window rect is not a content viewport.** Asking WebDriver for a
    1440 x 932 *window* yields an 880 px tall *page* — the browser's title and
    tab bar take real px in a window, less (but not zero) in full screen. A
    harness must set the outer size, read `innerWidth`/`innerHeight` back,
    and assert the achieved viewport rather than trusting the request.
  - Numbers taken at one viewport do not transfer to another, in either
    direction.
  - **A viewport read immediately after entering full screen can be
    transient, not settled.** During the Chrome measurement below, a reading
    of exactly **1710 x 1069** — the old Safari pin, both plausible and
    wrong for Chrome — appeared mid-animation and was superseded 637 ms
    later by the settled 981. A single post-transition reading can catch
    exactly this trap and "confirm" a stale number. **Required: a viewport
    measurement is two identical readings taken after the full-screen
    animation settles, never a single reading taken right after the
    transition.**

- **Browser changed: Chrome, not Safari** (owner, 2026-09-28). Supersedes
  the Safari pin above, not a retraction of it as wrong — Safari's number
  was correct for Safari; `screen` size and DPR are identical between the
  two, Chrome's toolbar simply costs ~88px more height (981 vs 1069) than
  Safari's did.

  **Pin: 1710 x 981 CSS at devicePixelRatio 2** — Chrome full screen, the
  built-in panel, 100% zoom, in a dedicated demo profile. Environment to
  record alongside every counted attempt: **macOS 15.6.1 (24G90), Chrome
  153.0.8010.53**, built-in `Color LCD` 2880 x 1864 Retina as the only
  attached display, `screen` 1710 x 1107.

  Evidence: 7 readings across 3 separate full-screen entries, all
  identical, plus 2 more at the other hostname and an independent in-page
  reporting channel that settled on the same number every time. Full
  record — environment, the measurement table, the effective-zoom check,
  the transient-1069 finding with its raw readings, and an explicit "what
  is NOT measured" section — lives in `docs/chrome-demo-viewport-2026-09-28.md`
  (same precedent as `docs/gate-box-noise-2026-09-17.json`: a dated raw-
  measurement file in `docs/`, cited here rather than copied here, so
  there is exactly one record of the number, never two to drift apart the
  way 1440x932 and 1710x1069 once did).

  Pin the Chrome and macOS versions in the verdict, same reasoning as
  before the browser changed: an update between a clean rehearsal and the
  demo invalidates the client half, because rendering, paint timing and
  WebSocket behaviour are exactly what that half measures.

  **Still unmeasured, stated as such rather than assumed equal**: the
  screen-shared (Zoom/Meet/Teams) variant. Screen sharing changes
  resolution, scaling and layout in ways not yet quantified here, and
  needs the owner driving the real conferencing tool to measure — open
  item, not a criterion yet.

**Display confirmed** (owner, 2026-09-28): the built-in panel, not the
`LG ULTRAFINE` 6016 x 3384 also attached to this Mac — the identity is
recorded in the verdict alongside the Chrome and macOS versions, and the
pinned viewport is the measured **1710 x 981 CSS at DPR 2** above (the
1440 x 932 and the superseded Safari 1710 x 1069 figures are not second
final values) — a rehearsal walked on a different display, or a different
browser, is a different experiment, exactly as a different PG version
would be.

### Recovery after the bridge drops

Added because the owner observed it before any harness did: a Safari tab open
against a bridge that went away showed an error chip and eleven blank panels.

The code path is sound on inspection — `connect()` re-fetches the session
token on every attempt, so the bridge's per-process token rotating on restart
does **not** strand an open tab — but nobody has ever observed a recovery.
Inspection is not observation, and the difference between "recovers within the
16 s backoff cap" and "stays blank until someone reloads" is the difference
between a hiccup and a dead demo.

So the Mac-side walk measures it: drop the bridge mid-walk, bring it back, and
record whether the UI reconnects, how long it takes, and whether any state is
lost. The measured number belongs in the verdict; the source's 16 s cap is not
evidence.

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
  audience notices. Set the bound from the first uncontended run on the demo
  configuration and pre-register it before counted attempt one; no bound is
  set yet. The 2026-09-29 walk's ten-tab distribution was measured with a
  second Chromium and server-side probes on the same 4-vCPU box, so using it
  as the demo bound would repeat the wrong-configuration viewport pin.
  **Retracted:** that walk's 30 s Timeline "paint" finding timed a bare tab
  click's "select a session" prompt, not a chart. Timeline has no standalone
  entry point with data; time the drill-down from a Sessions row as
  `tests/ui_live_smoke.py`'s `_navigate_to_tab` does.
- Viewport pinned to the resolution the demo will actually be shown at, not the
  driver's default.

### 5. Agreement and freshness

- Cross-tab agreement: DB Time and AAS for the same window must agree within
  the same 1% across overview, timeline and top-events. Disagreeing denominators
  between tabs is the product-facing version of the bookkeeping error this
  project has already made in prose. The AAS leg requires a bucket-weighted
  re-derivation over the identical window; an unweighted mean of buckets
  cannot establish it.
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
- One-time pre-demo overhead envelope: n=3 paired A/B pgbench TPS runs with
  and without the tracer, in full mode with the demo workload on the demo box;
  record the result and envelope before counted attempt one. The audience will
  ask for this number; a rehearsal capture runs only the with-tracer arm and
  cannot produce it.

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
| §1 run.id asserted against staleness | yes — not by this branch directly: `agent/rehearsal-bypass-suite` built its own `verdict_is_fresh` gate, then found at rebase time that master's `tests/demo_rehearsal_orchestrator_lib.validate_results_dir` (#176) already covers the identical concern, more thoroughly (also checks run.id's numeric format and summary.json's required keys) — removed the redundant duplicate rather than keep two |
| §2 samples > 0, DB Time > 0 | yes (`capture_has_events_ok`, `time_model_conservation`'s `MIN_DB_TIME_MS` floor, checked before any ratio) |
| §2 `Lock:relation` + `Timeout:PgSleep` present | yes (`workload_signature_present_ok`) |
| §2 AAS ≥ 0.5 on the recent window | yes (`aas_floor_ok`) — still the PROVISIONAL floor this section describes, not yet replaced by a measured baseline |
| §2 all 11 tabs reached, daemon alive throughout | yes (`build_demo_summary`'s `expected_tabs_per_pass` floor; `_assert_daemon_alive`, mirrors the existing workload-alive fail-safe) |
| §3 conservation identity | yes, sampled every ~5 minutes through the capture (`conservation_sample_interval_s`), every sample must pass — not once at the end |
| §3 `wait_gap_cpu_ms` ≤ 0.1% | **removed, not implemented** — corrected 2026-09-27/28: this was never a valid over-attribution detector (the BPF measures on-CPU across the whole wait-start/wait-end span, so an IO wait is legitimately CPU-bearing; two live runs measured 0.376–0.392%, expected on a correct implementation). The real per-class signature (a `Timeout:PgSleep`/`Lock:relation` event carrying millisecond-scale `cpu_ns`) needs data not on the wire in the `time_model` response today — reported as a gap, not proxied from the aggregate |
| §3 `cpu_clamped_ms` ≤ 0.1% | yes (`cpu_clamped_ok`), scope corrected in its own code comment: vouches for CPU-class gaps only, never wait events (`src/compute.c`'s wait branch never clamps `cpu_ns > dur`) |
| §3 Off-CPU\* ≤ 10% | yes (`time_model_offcpu_cap_ok`) |
| §4 zero known-failing tabs | yes (`build_demo_summary` uses raw `ok`, ignoring the exemption) |
| §4 blink measured fraction ≥ 0.5 | yes — landed via `agent/blink-anchor-mount-seq` (#209, merged to master), inherited automatically once `agent/rehearsal-bypass-suite` rebased: `demo_rehearsal.py` has no independent blink-measurement code, it fully delegates to `ui_live_smoke.py:run_tab()`, which now calls `blink_sweep_gate_verdict` itself. Pinned with a regression test using real measured numbers (`tests/test_demo_rehearsal_lib.py`, run.id 1790574871: scatter 0.1305, transitions 0.0072–0.0172) |
| §4 time to first paint, pinned viewport | no — the earlier "viewport IS pinned" described `ui_live_smoke.py:run_tab`'s 1280×900, not the demo's measured Chrome 1710×981; matching the demo viewport is not established. Time-to-first-paint still has no timing field in a tab result or stated bound. Set that bound from the first uncontended run on the demo configuration and pre-register it before counted attempt one; the contended 2026-09-29 walk cannot set it. Retracted: its 30 s Timeline finding measured the "select a session" prompt from a bare tab click, not paint after a Sessions-row drill-down (`_navigate_to_tab`) |
| §5 cross-tab agreement, freshness | partial — freshness: yes (`freshness_ok`, `info`'s `now_ns` vs `to_ns`). Cross-tab agreement: DB-Time leg only, yes (`cross_tab_db_time_agreement_ok`, `time_model` vs `top_events` for the identical window). AAS leg: **not yet satisfied** — requires a bucket-weighted re-derivation from the `aas` endpoint over the identical window against `time_model.aas`. Retracted: the walk's 0.0000% came from an unweighted mean of buckets, which is not that comparison and establishes no agreement |
| §6 lost-event counters, overhead envelope | partial — lost-event counters: yes (`daemon_integrity_ok`: `ringbuf_drops_total`/`state_map_full_total`/`seen_query_ids_full_total`, already on the wire via pgwt-server's control proxy, no `src/` change needed; blind spot stated in the code comment: a lost LIFECYCLE event is silent, no counter increments). Overhead envelope: **not yet measured**; `tests/results/overhead_trend.csv` has only a header, `sampled_overhead_gate.py --mode sampled` runs only for `src/` changes, and nothing measures full mode. Retracted: the separate ~10-minute sweep is not a per-attempt requirement or a substitute. Do n=3 paired full-mode A/B TPS runs with the demo workload on the demo box and record the envelope before counted attempt one; the 40-minute rehearsal retained neither an A/B baseline nor even its with-tracer `tps =` line (cleanup tailed 20 lines and deleted the full log) |
| §7 no test exited 126/127 | **partial, and this is a human-readable aid, not an automated gate**: `tests/demo_rehearsal.sh`'s `report_early_exit` labels a dead subprocess's 126/127 exit code in the log for a human reading it afterward. It does NOT change the script's own exit code (every call site already `exit 1` regardless of the labeled reason) and has no test coverage of its own — reviewer finding, 2026-09-28. Do not read this row as "126/127 fails the gate automatically"; it already did, via the pre-existing `kill -0` + `exit 1` checks, which is why this addition changes nothing observable except the log's wording |
| §8 known-failing lines read by hand | manual by construction; not applicable to `demo_rehearsal.py` itself, which grants zero known-failing exemptions (§4 row above) -- nothing here for a human to hand-verify against an issue |

The `time_model_conservation()` vacuous pass on `db_time_ms <= 0` (a rehearsal
that captured nothing used to score clean) is now FIXED on
`agent/rehearsal-bypass-suite`, along with every other row marked "yes" above
-- each ships with a case proving it can go red
(`tests/test_demo_rehearsal_lib.py`).

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
- **The two clean runs must be on the same tag, not merely the same tree.**
  Owner, 2026-09-28: "instead of code freeze we will make a tag on master when
  it will be demo ready and will continue development" -- a tag pins the tree
  the way "same commit, no commit in between" used to, without requiring
  master itself to stop moving:
  - When master is judged demo-ready it is **tagged**; the demo machine is
    built from that tag, and every counted rehearsal runs **against that
    tag**, from a worktree checked out at it — never from master HEAD, which
    keeps moving.
  - A counted attempt **names the tag it ran against**, alongside the box
    class, PG version, pgbench parameters, and the display identity and
    Chrome/macOS versions already recorded in the verdict.
  - Two clean attempts count as consecutive **only if they ran on the same
    tag**.
  - **Landing a fix and re-tagging resets the clean-attempt counter to
    zero.** An early or hopeful tag costs the whole sequence, so the tag is
    cut when the tree is genuinely ready, not to start the clock.
  - Development continues on master in parallel and does not disturb an
    in-flight sequence — that is the point of the tag, and it removes the
    objection that with several branches in flight no two runs could ever
    share a tree.

## Provenance

Criteria drafted by the lead, challenged and corrected by the standing adviser
(`.claude/agents/adviser.md`) on 2026-09-27 — which supplied criterion 3's
residual-absorption finding, the over-attribution detectors, the sampling
requirement, the cross-tab and freshness checks, and the attempt accounting.
The owner's rulings are quoted inline where they bind.
