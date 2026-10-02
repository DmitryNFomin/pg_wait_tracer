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
rehearsal window costs him **one interaction instead of a dozen**. **Retracted
for the Mac side (owner, 2026-09-29):** the claim that a harness asserts each
item and fails fast was written before there was a Mac-side harness. The
Mac-side pre-flight is checked and recorded by the walker in
`docs/DEMO_MAC_WALK_CHECKLIST.md`.

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

**Retracted for the Mac-side walk:** the earlier text assigned holding the
display awake (`caffeinate` with a trap) and process and VM cleanup to a
Mac-side harness that does not exist. **The owner should never be asked to
kill an orphaned shell.** That happened twice on 2026-09-28 and was a
missing trap, not a thing for him to do.

### What the harness must do with this list

**Retracted for the Mac side (owner, 2026-09-29):** these were proposed harness
requirements, not implemented Mac-side checks. The manual checklist now owns
the Mac-side observations. Do not report this list as automated evidence.

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
- **Mac-side**, walked by hand in real Chrome on the Mac's built-in display,
  against `docs/DEMO_MAC_WALK_CHECKLIST.md` and the same VM in the demo
  topology: §4's time to first paint and viewport, and §5's freshness — *as
  the audience will see them*. The result is a human sign-off on the filled-in
  checklist. These are different quantities from their VM-side namesakes.
  Headless Chromium on a cx33 over localhost is not real Chrome at DPR 2 on
  Apple silicon over an ssh hop, and a bound measured on one does not transfer
  to the other. The VM's number is kept only as a regression detector.

**Retracted (owner, 2026-09-29):** the Mac-side criteria were written as if an
automated harness existed. It does not. `scripts/demo-rehearsal.sh` runs the
walk over ssh on the VM via `scripts/demo-rehearsal-remote-run.sh`;
`tests/demo_rehearsal.py` launches Playwright's bundled headless Chromium
(`p.chromium.launch()`, with no `channel="chrome"` in the tree). It pins the
1710 x 981 viewport at DPR 2 (#223), but a pinned viewport in headless
Chromium on the VM is not a Mac-side walk. `memory_pressure` and bridge-drop
recovery were specified here but implemented in no test or script. Thus
"Mac-side m of M" was a counter that could not advance. Building that harness
was judged days of work competing with the dry run and counted attempts. A
manual walk observes the real presentation environment more honestly than a
headless approximation of it.

Reported as "capture-side clean k of N, Mac-side clean m of M". The Mac-side
count means **signed-off manual walks**, not automated verdicts. **Never as
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

**Void cap: 2** (owner, 2026-09-30). There was previously no limit on how many
attempts could be voided this way, which would let "two consecutive clean
attempts" decay into "two clean attempts eventually" by re-rolling past an
unlimited number of voided ones. After **two** voided attempts — whether from
abnormal Mac-side `memory_pressure` or any other infrastructure reason under
"The sequence, and what stops it from being rolled" below — stop and treat it
as a real problem with the machine or the setup, not bad luck; do not keep
re-rolling. The cap applies to attempts, not to individual within-walk ticks.

The walker records `memory_pressure` and load by hand at the start and end of
the walk and on any tick that looks slow, in the checklist. This is coarser
than the per-tick verdict record originally specified; the trade was made
deliberately for a manual walk. Record the observation, not "the Mac was
busy". For the duration of a Mac-side walk, no implementing agents and no
local VM work run on it — that cost is real and is accepted deliberately.

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

  **Re-confirmed on Chrome 154.0.8037.58** (2026-09-30, **uncounted diagnostic
  reading — not a signed walk, no committed artifact**, same status as the
  2.0 s bridge-drop observation below): two agreeing reads, full screen, 100%
  zoom, `windowState: fullscreen`, built-in `Color LCD` as the only attached
  display. Unchanged: still **1710 x 981 CSS at DPR 2**. This does not stand
  in for a signed Mac-side walk's own viewport check (checklist §1 above,
  "Both readings must agree") — it re-confirms the pin is still plausible on
  the newer Chrome version, nothing more. Record whichever Chrome version is
  actually installed at the time of each counted attempt (153.0.8010.53 or
  154.0.8037.58 as applicable) — the pin held across this one version bump,
  but the next bump needs its own re-check, not an assumption that it is
  still unchanged.

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

So step 3 of the manual Mac-side walk drops the bridge mid-walk, brings it
back, and records whether the UI reconnects, how long it takes, and whether
any state is lost in the signed checklist.

**Diagnostic observation, 2026-09-30 (uncounted — not a signed walk):** on the
real topology (Go bridge killed, which tears down ssh and `pgwt-server` with
it), the degraded overlay appeared in 0.4 s with reason `disconnected`,
previously loaded rows were retained rather than blanked, and the UI
recovered in **2.0 s** without a reload, with a second `/session` request
returning 200 — i.e. the per-process token was correctly re-fetched, the
exact risk this section names. This is a real recovery number where there was
previously only inspection, but it has no committed artifact, it is a
diagnostic run, **not** step 3 of a signed Mac-side walk, and **it does NOT
close or satisfy step 3** — every counted attempt still needs its own signed
step-3 observation in the checklist, and this paragraph must not be read as
having already provided one. It replaces only the earlier placeholder
characterization of the 16 s backoff cap as "not
evidence" — it does not set a pass/fail bound, and step 3 above still needs
its own signed observation per counted attempt.

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
- **AAS ≥ 1.15 on the 60 s recent window** (owner, box access, review round
  4 — settled; was ≥0.5, provisional). The first clean rehearsal landed:
  `ok: true`, `failed: []`. Its measured AAS on the 60 s recent window —
  the basis this bullet itself names — was `cross_tab_aas_agreement`'s
  `derived=2.3033 time_model=2.3033` (the two already had to agree within
  1% to pass that check, so either number is the same measurement twice;
  used `derived`/`time_model` from the 60 s window, not the whole-capture
  `AAS: 2.22` also reported from the same run, because this bullet's own
  text scopes the floor to "the 60 s recent window", and the whole-capture
  figure is a different window over a different quantity). Half of 2.3033
  is **1.1517**, recorded here as **1.15**. **`run.id`: 1790869219.**
  Provenance, stated plainly rather than implied: the clean rehearsal ran on
  a throwaway VM (`root@46.225.4.87`); its capture finished early
  (`rehearsal.done` written while `scripts/demo-rehearsal.sh`'s own
  `DURATION_MIN`-derived sleep was still running), so this run id and both
  AAS figures were read directly off
  `/root/pgwt-demo-rehearsal-master/tests/results/demo_rehearsal/` on that
  box, not from a synced local artifact — none existed yet when this was
  recorded (the rsync only happens after the launcher's sleep ends). This is
  why no worktree in this repo's local clone had a matching `summary.json`:
  the nearest candidate found (`aas-agreement-and-retention` worktree,
  `derived=2.3617 time_model=2.3624`, `ok: False`) is a different, older,
  failing run, not this one. **Doc vs. code,
  deliberately not closed here**: this branch is docs-only, so
  `tests/demo_rehearsal_lib.py`'s `AAS_FLOOR_PROVISIONAL = 0.5` constant is
  untouched — the gate itself still enforces 0.5 today. Per this project's
  own rule ("if a document and the code disagree, the document changes
  here; a code change is a separate branch"), moving the enforced floor to
  1.15 is a follow-up code change on its own branch, not part of this
  commit.
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

  Per-class wait CPU is not currently on the wire. **BLOCKED — method
  mis-specified (owner, box access, review round 4), not a box-access
  problem.** This paragraph used to say to settle it via
  `tests/cross_validate.c` over "any full-mode gate-box trace". That method
  cannot work: `tests/cross_validate.c`'s own header says it "reads a
  **tiered-mode** trace directory that contains BOTH sampled blocks
  (always-on) and transition blocks (escalation window)" and compares the
  two over their overlap window — it needs SAMPLES blocks to compare
  against. **Full mode produces none.** Run against the retained 35-minute
  full-mode trace from the first clean rehearsal (below):
  `pgwt-server --dump` on it reports `4432286 transitions, 0 samples,
  Fidelity: exact`; `cross_validate` on the same trace exits with `ERROR: no
  sample blocks — sampler produced nothing` (`tests/cross_validate.c`'s own
  message, line ~149) — not a flaky run, a structural mismatch between the
  tool's input contract and full mode's output. **What would actually
  answer this:** either a per-event `cpu_ns` extraction tool for full-mode
  traces, which does not exist today, or a deliberate `--mode tiered`
  capture — which is then not the demo configuration (`--mode full`), so it
  would answer a different question. **Not invented, not built here**: no
  number is recorded for the max-`cpu_ns`-on-`Timeout:PgSleep` figure this
  row originally asked for, and the tool is not written in this docs-only
  commit.

  **Related data point, same dump, corroborates the Off-CPU\* section
  above:** over the whole 1875.8 s capture, `CPU (waiting for a core)` was
  **746.5 ms against 4,170,804.2 ms of DB Time — 0.018%**
  (746.5 / 4170804.2 × 100, checked). That is the real-world size of the
  quantity the 10% cap gates, on the demo workload, over a full run, not
  just the per-5-minute-window figures above — consistent with, not a
  substitute for, the <0.1% (n=7, one run) already recorded there.

- Off-CPU\* ≤ 10% of DB Time on a box where clients ≤ cores. **RETRACTION (this
  commit, review round 3):** an earlier version of this paragraph said the
  gate comment's "4.92 pp" was unverifiable because it does not appear as a
  literal string anywhere on `agent/observer-bias-study` or its four
  predecessor branches. That check was real but its conclusion was wrong: the
  figure **is** derivable, just not as a literal substring. **4.92 pp IS
  the sum of two cells `docs/OBSERVER_BIAS.md` writes down explicitly** (that
  branch, ~lines 185-188; data in `tests/results/issue115_xtab/`): run-queue
  delay while still marked as **waiting** (+2.53 pp) plus run-queue delay
  while marked **idle**/`ClientRead` (+2.39 pp) = **4.92 pp**, exactly what
  `tests/demo_rehearsal_lib.py`'s gate comment calls "the measured run-queue
  (waiting-for-a-core) share". Retracting "confirmed unverifiable" — it was
  not invented, it is a sum, and the sum is correct.

  **The real flaw is sharper than provenance, and it is three separate
  problems, not one:**
  1. **Regime.** Those two cells come from a **saturated** run — 8 pgbench
     clients on a 4-vCPU box (`docs/OBSERVER_BIAS.md`'s own "Eight backends
     only" caveat) — while the gate's own comment scopes the bound to
     "clients ≤ cores". The same study's unsaturated analogue ("2.72 pp
     definitional", which bounds the run-queue-specific cells from above) is
     **≤2.72 pp, not 4.92**. The number is measured on the opposite regime
     from the one it gates.
  2. **Unit.** The 4.92 pp is percentage points of **backend wall-time** — a
     kernel-vs-our-exact-tier CPU-or-runnable share (the study's 67.99% vs.
     62.27% headline) — **not percentage points of DB Time**, which is what
     `time_model_offcpu_cap_ok` actually divides by. The gate comment's own
     phrase "4.92 percentage points of DB Time" names the wrong denominator
     for the number it cites.
  3. **Quantity — the serious one.** Neither cell flows into Off-CPU\* at
     all. `src/compute.c`'s Off-CPU\* is the residual `DB Time − CPU\* − Σ
     waits` — time inside DB Time, actively executing, attributable to no
     known wait class. Run-queue delay while a backend is already marked
     **waiting** is booked under that open wait event (inside Σ waits, not
     the residual); run-queue delay while marked **idle**/`ClientRead` is
     outside DB Time entirely (DB Time only counts active execution). The
     four-cell decomposition this number comes from never measures run-queue
     delay in the one state Off-CPU\* actually represents: active,
     non-waiting, but off a core. **The citation bounds a different quantity
     than the one the gate enforces**, not merely the wrong regime.

  **Honest statement of what this gate is:** the Off-CPU\* cap catches a
  dropped wait class worth ≥10% of DB Time. The demo configuration measured
  **<0.1%** (n=7 samples, one run). The 10% has **no measured basis in the
  gated quantity** — only a loose upper bound borrowed from a saturated
  run-queue figure that measures something else. **What it does not catch,
  concretely:** a dropped class smaller than 10% of DB Time passes green,
  and this demo's own workload contains such classes —
  `LWLock:WALWrite` ≈3.7%, `Lock:tuple` ≈1.5% — either could silently vanish
  from capture and this check would still read `ok`.

  **Non-gating, worth recording, not fixing here:** the whole-window
  diagnostic reports `has_measured_cpu=False` (no Off-CPU\* signal at that
  granularity) while the same response is labelled `fidelity: exact` — its
  Off-CPU\* check is vacuous at exactly the point it claims exact fidelity.
  Noted for whoever next touches this response shape; out of scope here.

  **Do not tighten the cap.** A 1-2% bound would have 25-50x headroom over
  today's measured 0.04%, but this is **one run** — no second measurement
  shows cx33 run-queue noise reliably stays under a tighter number, and
  changing a gating constant on n=1 hours before the tag is exactly the move
  this project has refused all week. **Keep `OFFCPU_CAP_PCT = 10.0`**,
  labelled honestly as a provisional catastrophic-loss tripwire rather than
  a calibrated bound, per the code comment's own existing "FIRST bound, not
  a permanent one" framing.

  **Owner decision (recorded, not an open question): #115
  (`agent/observer-bias-study`) is NOT landing before the tag.** It would not
  even validate the clients ≤ cores premise this gate needs — problem 1
  above is a different box regime that #115 does not re-measure — and it is
  a large, late change this close to the tag.

**Known, unreproduced item: #202** (owner, 2026-09-30 — recorded, not closed).
`test_multi_window.py`'s own conservation check once measured class rows
summing to 20.2% more than DB Time (DB Time 15255 ms vs. Σ top-level class
rows 18330 ms), an over-attribution direction this identity *can* detect, on
a single `make box-check` run. n=1, never reproduced (the issue itself notes
"a second box run is in progress" and carries no comment recording its
result). This criteria commit does NOT re-run that reproduction. Separately,
the 35-minute rehearsal referenced elsewhere in this document recorded its
own eight conservation samples through the capture as all 0.00% gap, with
cross-tab DB Time agreement also 0.00% — evidence against the same defect
showing up in *this* workload, not evidence the #202 report was wrong. **#202
is NOT a demo blocker and must NOT be closed** on the strength of that
absence; it is a known, unreproduced product report, tracked and left open.

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
  — an unmeasured tab is not a passing tab. **Corrected, consistency pass
  (this commit)**: the dependency note here previously said "measured
  fraction" did not exist on `master` and named the unmerged
  `agent/blink-anchor-mount-seq` branch as the thing that would add it. That
  branch (#209) has since merged to `master`, which this branch is cut from —
  `MIN_MEASURED_FRACTION = 0.5` is in `tests/ui_live_smoke_lib.py:77` today,
  read into `<tab>.no_blink.measured.ok` via `measured_count/attempted_count
  >= min_measured_fraction`. This criterion is therefore evaluable now; the
  dependency is resolved, not open.
- **Per-tab time to first paint — SETTLED: 3000 ms, global (owner, review
  round 6).** `tests/ui_live_smoke.py`'s
  `_navigate_to_tab` carries `ttfp_ms` (issue #245, merged to `master` as
  `fa20067` / PR #257): elapsed ms from the navigation's own landing click
  (the Sessions-row drill-down click for Timeline, the bare tab click
  everywhere else) to the ViewManager's first fresh mount of that tab.

  **This criterion's own requirement — ≥3 navigations per tab, on the demo
  configuration (real Chrome on the Mac, not headless Chromium on a VM) —
  is now met.** The owner ran it directly, re-measured after PR #268 (#265)
  replaced `handle_top_queries`'s two O(n^2) bubble sorts with `qsort` —
  the prior table below was recorded before that fix and understated the
  product: real Chrome 154 at the pinned demo viewport (1710×981 @ DPR 2,
  full screen), against a **live full-mode capture** on the actual demo
  topology (Go bridge on the Mac, `pgwt-server` over ssh to the demo box
  (cx33)), **3 navigations per tab**, each preceded by a return to Overview
  so nothing is measured warm:

  | tab | min | median | max |
  |---|---:|---:|---:|
  | **transitions** | **697** | **707** | **893** |
  | scatter | 328 | 445 | 454 |
  | queries | 379 | 411 | 445 |
  | waterfall | 380 | 412 | 428 |
  | concurrency | 298 | 301 | 349 |
  | matrix | 297 | 329 | 331 |
  | histogram | 189 | 229 | 265 |
  | overview | 126 | 133 | 193 |
  | events | 137 | 144 | 146 |
  | sessions | 139 | 142 | 145 |

  Raw per-navigation values checked against this table (3 values per tab,
  min/median/max all reproduce exactly;
  `/private/tmp/claude-501/-Users-dmitryfomin-work-git-pg-wait-tracer/258d73f0-4867-4c95-9aca-9a7966648e2b/scratchpad/ttfp-post/post.json`
  — the orchestrator's scratchpad, not checked into the repo).

  **Timeline is NOT in this table — recorded as not yet measured by this
  method, not as a pass.** The measurement script clicked the bare Timeline
  tab, which shows a "select a session" prompt rather than a chart (the
  same #242/#245 trap this document has already retracted once) — an
  instrumentation gap in this particular script, not a product result.
  Timeline must be reached through a Sessions-row drill-down, as
  `tests/ui_live_smoke.py`'s `_navigate_to_tab` already does; this ad hoc
  Mac-side script did not. Until it is re-run correctly, Timeline has no
  real-Chrome-on-Mac measurement.

  **Methodological warning, worth recording even though superseded:** an
  earlier attempt at this same measurement produced a worthless >15x
  margin by measuring tabs against a *retained* (historical) trace, where
  Events/Sessions/Queries returned "No data for selected range" and the
  other tabs were timing an empty panel's paint, not real content — the
  UI's default window is live, so a paint measurement against a historical
  trace measures nothing. The table above is the corrected, live-capture
  re-run; the >15x figure was never written into this document and should
  not be treated as having existed as a measurement.

  **Worst single navigation: 893 ms (transitions). Margin: 3000/893 ≈
  3.37x, rounded 3.4x.** This supersedes the pre-#268 figure of 2432 ms
  (queries) / ≈1.2x recorded in the table above's predecessor — that
  number is now wrong and must not be used; it measured `handle_top_queries`
  before the qsort fix. It also supersedes the still-earlier "≈2.5x"
  VM/headless-Chromium figure described further below, already superseded
  once before this round. **The larger margin is a real product
  improvement (the O(n^2) sorts were genuinely slow), not measurement
  noise — but the bound itself was NOT raised, and is NOT being tightened,
  on the strength of it.** Moving a gating constant in either direction on
  n=3, hours before a tag, is exactly the kind of after-the-fact adjustment
  this whole document exists to prevent — the same reasoning that kept
  `OFFCPU_CAP_PCT` at 10% in §3 above and kept this bound at 3000 ms when
  the margin was thin. A future reader should see 3000 ms as a deliberate,
  unchanged choice, not something nobody revisited. Transitions' own spread
  (697–893 ms, a 196 ms range across 3 runs) is now comfortably inside the
  remaining headroom, unlike the pre-#268 queries spread that consumed most
  of it.

  **Demo-visible, not just a gate concern:** pre-#268, Queries took ~2.4 s
  to paint and was called out in `docs/DEMO_RUNBOOK.md` §7 as a tab a
  presenter should expect and cover a visibly empty panel for. That is no
  longer true — Queries now paints in well under half a second. The
  slowest tab is now Transitions (~0.7–0.9 s, its DFG graph layout), which
  was already noted in §7 as comparatively slow; `docs/DEMO_RUNBOOK.md` §7
  is updated (this commit) to reflect both changes.

  **Prior VM/headless-Chromium data, superseded, kept for context only —
  do not use for the margin above:** three runs on `agent/ttfp-timing-field`
  (run A `tests/results/box-check-ubuntu-20260930-115044.log`, cited in PR
  #257's body; run B `box-check-ubuntu-20260930-150337.log`; run C
  `box-check-ubuntu-20261001-083418.log`) measured sessions 67→148→264,
  overview 74→98→306, events 94→94→273, histogram 185→198→594, timeline
  (drill-down) 220→301→397, queries 227→248→509, waterfall 298→342→832,
  concurrency 304→291→734, scatter 318→389→809, matrix 341→332→889,
  transitions 533→541→1211 — rising across all three runs, on a different
  box class (VM, not the Mac client) and headless Chromium (not real
  Chrome). These numbers established that n=1-2-per-tab VM data was not a
  stable basis for the bound, which is exactly why this criterion asked for
  ≥3 navigations on the real demo configuration in the first place; they
  are superseded by the table above for every purpose other than that
  historical point, and the real Chrome/Mac numbers are generally LOWER
  than the VM/headless ones were trending toward (e.g. transitions 1211 on
  the VM vs. 1150 max on real Chrome/Mac) — consistent with "capture-side
  (VM) conditions do not establish the Mac-side result" elsewhere in this
  document, in both directions.

  **Global, not per-tab**, per the original pre-registered guidance
  (`docs/DEMO_DELIVERY_QUEUE.md` item 2.1: "set ONE generous global bound
  (never below 3 s)"): a single number is what a human watching a timer
  during the demo can actually hold in their head.

  **What would settle this:** ≥3 navigations per tab, on real Chrome on the
  Mac (the actual demo client), ideally run back-to-back with a VM-side
  capture to separate "box got busier" from "this code got slower." Until
  then, delivery-queue item 2.1 stays open (see that document) and this
  bound is the best available number, not a closed criterion.
- **Window dependence: `top_queries` latency tracks the requested range,
  not just whether it's one of the tabs above.** `handle_top_queries`
  (`src/server.c:2720`) always loads raw events for its lifecycle stats
  (exec/plan counts) — summaries carry no exec/plan markers — regardless of
  whether `should_use_summaries` (`src/server.c:2255`) picks the summary
  path for the class breakdown. So its cost tracks the **requested window**,
  not a fixed per-tab constant. Measured on a 2583 s capture (4,227,072
  events), `top_queries` alone:

  | window | latency |
  |---|---:|
  | 60 s | 14 ms |
  | 5 min | 70 ms |
  | **15 min** | **391 ms** |
  | full capture (43 min) | 1237 ms |

  **Two things this does NOT establish, stated so neither gets assumed
  later:**
  1. **Not independent of capture length.** Each request refreshes
     coverage twice (`coverage_refresh`, called once directly inside
     `server_load_events_fi_mode` and once inside `should_use_summaries`,
     `src/server.c:1845` and `src/server.c:2259`), and opening
     `current.trace` rebuilds its block index from all committed block
     headers every time (`pgwt_reader_open`'s meta-file strategy,
     `src/event_reader.c`) — so some overhead does grow with the capture.
     **391 ms is a measurement for this 43-minute session, not a constant**
     that would hold at, say, 2 hours.
  2. **The 60 s window's 0 rows is not explained here as "no completed
     executions."** Below 120 s this endpoint uses raw events; at 5
     minutes and above it can use summaries for the class breakdown
     (`should_use_summaries`). Those are different code paths measured at
     different window sizes, and the latency numbers above do not by
     themselves establish why row counts differ between them — record the
     latency observation without attaching that cause.
- The 2026-09-29 walk's ten-tab distribution was measured with a second
  Chromium and server-side probes on the same 4-vCPU box, so using it as the
  demo bound would repeat the wrong-configuration viewport pin — superseded
  by the real-box measurement above.
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
- **Overhead band, current levels accepted (owner, 2026-09-30): "ok current
  level is accepted, we will work on that afterwards".** This records the
  measured band rather than setting a target the demo must hit — it does not
  replace the n=3 demo-workload A/B above, it sets the expectation for what
  that A/B will show. On cx33, `--mode full`, write-heavy pgbench: **~28-32%
  at 4 clients, 32-38% at 16, 33-37% at 64**. Record that this is **not a
  regression**: the band holds on every measured day since 2026-09-16 across
  both gate boxes (11 distinct days, 15 box-days, 1802 rows, from
  `overhead_trend.csv` on the boxes — this per-commit's own `make check` runs
  on macOS and cannot reach either box's filesystem to re-read that CSV, so
  this figure is recorded as given and not independently re-derived here; see
  the report for this commit). The README's `~6%` full-trace write-heavy
  figure is a **different machine class and configuration** — confirmed
  against `README.md`'s own benchmark-environment paragraph: cx43 (8 vCPU),
  `shared_buffers = 2 GB`, 8 clients, vs. this band's cx33 at 4/16/64 clients
  — so the two numbers are not like-for-like and neither supersedes the
  other.

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
| §2 AAS ≥ 0.5 on the recent window | yes (`aas_floor_ok`), but **the gate still enforces the OLD provisional 0.5** — the doc's own floor is now settled at 1.15 (see §2's own paragraph above), `tests/demo_rehearsal_lib.py`'s `AAS_FLOOR_PROVISIONAL` constant is deliberately untouched by this docs-only commit, and moving it to 1.15 is a follow-up code change on a separate branch |
| §2 all 11 tabs reached, daemon alive throughout | yes (`build_demo_summary`'s `expected_tabs_per_pass` floor; `_assert_daemon_alive`, mirrors the existing workload-alive fail-safe) |
| §3 conservation identity | yes, sampled every ~5 minutes through the capture (`conservation_sample_interval_s`), every sample must pass — not once at the end |
| §3 `wait_gap_cpu_ms` ≤ 0.1% | **removed, not implemented** — corrected 2026-09-27/28: this was never a valid over-attribution detector (the BPF measures on-CPU across the whole wait-start/wait-end span, so an IO wait is legitimately CPU-bearing; two live runs measured 0.376–0.392%, expected on a correct implementation). The real per-class signature (a `Timeout:PgSleep`/`Lock:relation` event carrying millisecond-scale `cpu_ns`) needs data not on the wire in the `time_model` response today — reported as a gap, not proxied from the aggregate |
| §3 `cpu_clamped_ms` ≤ 0.1% | yes (`cpu_clamped_ok`), scope corrected in its own code comment: vouches for CPU-class gaps only, never wait events (`src/compute.c`'s wait branch never clamps `cpu_ns > dur`) |
| §3 Off-CPU\* ≤ 10% | yes (`time_model_offcpu_cap_ok`), but the 10% is a **provisional catastrophic-loss tripwire**, not a calibrated bound — see §3's own corrected paragraph above: its cited "4.92 pp" derivation is a real sum but measures a different regime, unit, and quantity than the gate enforces; demo measured <0.1% (n=7, one run); kept at 10%, not tightened, on n=1 |
| §4 zero known-failing tabs | yes (`build_demo_summary` uses raw `ok`, ignoring the exemption) |
| §4 blink measured fraction ≥ 0.5 | yes — landed via `agent/blink-anchor-mount-seq` (#209, merged to master), inherited automatically once `agent/rehearsal-bypass-suite` rebased: `demo_rehearsal.py` has no independent blink-measurement code, it fully delegates to `ui_live_smoke.py:run_tab()`, which now calls `blink_sweep_gate_verdict` itself. Pinned with a regression test using real measured numbers (`tests/test_demo_rehearsal_lib.py`, run.id 1790574871: scatter 0.1305, transitions 0.0072–0.0172) |
| §4 time to first paint, pinned viewport | **SETTLED** (review round 6): bound 3000 ms, global, **≈3.4x margin** (worst observed: transitions at 893 ms) — re-measured after PR #268 (#265) replaced `handle_top_queries`'s O(n^2) bubble sorts with `qsort`; supersedes the pre-#268 figure of queries at 2432 ms / ≈1.2x margin, which is now wrong. Measured ≥3 navigations/tab, real Chrome 154 on the Mac, live full-mode capture on the demo topology, satisfying this criterion's own requirement in full. The bound itself (3000 ms) is unchanged and was neither widened when the margin was thin nor tightened now that it is comfortable — same reasoning both times. `top_queries` latency is separately window-dependent (391 ms at the demo's planned 15-minute window, rising toward 1237 ms at full-capture range on a 2583 s capture) — see the window-dependence finding below this bullet; that is a server-side cost, not a UI paint regression, and is unrelated to the ≈3.4x margin above. Timeline not yet measured by this method (instrumentation gap: bare click, not Sessions-row drill-down — recorded as unmeasured, not a pass). `ttfp_ms` (merged `master`, `fa20067`, issue #245/PR #257) remains measurement-only and gates nothing automatically — `build_tab_result` never reads it into `ok`; the bound above is enforced only by a human reading this document during a counted attempt. Mac-side checklist: still manual sign-off, **deliberately stays qualitative** (paints without a visible spinner) — it has no stopwatch, so it does not independently check the 3000 ms number either; this round's measurement was a one-off script, not the checklist itself. Retracted: the 2026-09-29 contended walk's 30 s Timeline finding measured the "select a session" prompt from a bare tab click, not paint after a Sessions-row drill-down — the same trap this round's Timeline gap repeats at the instrumentation level, not the product level |
| §5 cross-tab agreement, freshness | partial — VM-side freshness: yes (`freshness_ok`, `info`'s `now_ns` vs `to_ns`); Mac-side visible freshness: manual checklist and human sign-off. Cross-tab agreement: DB-Time leg yes (`cross_tab_db_time_agreement_ok`, `time_model` vs `top_events` for the identical window); AAS leg yes (`bucket_weighted_aas_ok`, `agent/aas-agreement-and-retention`) — a bucket-weighted re-derivation from the `aas` endpoint's own `bucket_ns` over the identical window, compared against `time_model.aas`, wired into `extra_checks["cross_tab_aas_agreement"]` alongside the DB-Time leg. Retracted: the earlier walk's 0.0000% came from an unweighted mean of buckets, which was not that comparison and established no agreement; see `tests/demo_rehearsal_lib.bucket_weighted_aas_ok`'s own comment for why the two formulas diverge whenever the window does not divide the endpoint's `bucket_ns` evenly (the common case) |
| §6 lost-event counters, overhead envelope | partial — lost-event counters: yes (`daemon_integrity_ok`: `ringbuf_drops_total`/`state_map_full_total`/`seen_query_ids_full_total`, already on the wire via pgwt-server's control proxy, no `src/` change needed; blind spot stated in the code comment: a lost LIFECYCLE event is silent, no counter increments). Overhead envelope: **not yet measured for the demo workload on the demo box**. Correction: the earlier claim that nothing measures full mode was wrong. Every box-check runs `tests/test_overhead.sh --quick` (~10 minutes) via `tests/run_all.sh`; its paired baseline/tracer A/B uses the default pgbench load, pins `--mode full`, and appends to `tests/results/overhead_trend.csv` on the box. The tracked CSV has only a header because `tests/results/` is excluded from box-check's up-rsync; box-generated rows are not synced back or committed. The ~70-minute sweep is the same script without `--quick`; `sampled_overhead_gate.py --mode sampled` runs only for `src/` changes. Retracted: the quick sweep is not a per-attempt requirement or a substitute for measuring the demo workload. Do n=3 paired full-mode A/B TPS runs with the demo workload on the demo box and record the envelope before counted attempt one; the 40-minute rehearsal retained neither an A/B baseline nor even its with-tracer `tps =` line (cleanup tailed 20 lines and deleted the full log) |
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
- **Void cap: 2** (owner, 2026-09-30, see "Attribution for the Mac side" above).
  At most two attempts total may be voided for an infrastructure reason — Mac
  `memory_pressure` or otherwise — before the sequence stops: a third void is
  not re-rolled past, it is reported as a real problem with the machine or the
  setup. Without this cap, an unlimited number of voided attempts would let
  "two consecutive clean attempts" decay into "two clean attempts eventually".
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
