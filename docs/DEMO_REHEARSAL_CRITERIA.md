# What counts as a clean release gate

**AMENDED 2026-10-25 — the demo is cancelled.** The owner: *"There is no demo
planned anymore, moment for it has gone"* (PGConf Valencia). This document
used to govern a sequence of pre-registered, counted *demo rehearsals* ending
in a tag cut right before a presentation. That event no longer exists, so the
apparatus built around it — counted attempts, a void cap, two separately
counted verdicts (Mac-side vs. capture-side), owner-signed Mac walks, Mac
pre-flight hygiene, and a dedicated `pgwt-stage` box — is **retired**. See
"What changed and why" near the end of this document for the full list and
the reasoning; nothing there is a silent reinterpretation, it is a dated
amendment, per this document's own pre-existing rule below.

**What is NOT retired, and is the actual point of this document now:** the
quality bar is unchanged — *"all features work as designed, no UX issues, all
data is correct"* — and the technical checks below (§1-§8) are the only
30-45 minute full-stack live gate this project has. They become an **ordinary
release gate**: run once before a tag, the same as a `box-check`, not a
rehearsal for an audience. "Release" below means "the tree about to be tagged
or merged", not a presentation.

**Pre-registered, still.** A criterion's bound (a floor, a tolerance, a
threshold) is set BEFORE it is used to judge a run, not adjusted after seeing
one fail. Deciding after a run which of its failures "really counted" is the
failure mode this document exists to prevent (the same one a process retro
found in the lead's own analysis on 2026-09-27: a conclusion chosen after
seeing the data). If a criterion is wrong, it is changed by a commit that says
so, explicitly amending the rule rather than reinterpreting it — never
silently in a report. (The now-retired "attempt counter" that used to reset on
such a change no longer exists; see "What changed and why".)

## The workload the criteria are written against

`tests/demo_rehearsal.sh`: pgbench, 4 clients, `--rate=25`, plus
`tests/live_loop_workload.py`. Full mode. `RECENT_WINDOW_S` = 60 s,
`TIME_MODEL_TOLERANCE_PCT` = 1.0.

**Explicit feature exclusion: tiered escalation.** This demo configuration
cannot exercise sampled-to-exact escalation, bounded grants, expiry back to
sampled, manual deescalation, or escalation-budget denial/accounting.
`tests/demo_rehearsal.sh:3` runs `--mode full`; `src/control.c:66-67` makes an
EXACT provider permanently report tier `"escalated"` (full fidelity).
A clean full-mode rehearsal therefore provides no verification of those
tiered features. This exclusion is explicit for the owner's review against
the bar "all features work as designed"; it is not a claim that they passed.
Existing coverage is `tests/test_escalation.sh`: a real tiered daemon,
4 s grant, expiry, deescalation, budget denial with a reason, at least two
windows and one denial counted, START/END trace markers, and proxy id
round-trip; plus the 32 budget-core checks in `tests/test_escalation_budget.c`.
`test_b5_escalate_flow` in `tests/test_web_ui.py` covers the mock flow.

The presenter's **"Escalate 60s" button is still subject to an honesty check**,
including when an empty-state panel offers it before daemon status is known.
No button defect was found: `web/static/lib/panels.js:158-180` hides it when
`escalation_supported` is false. If clicked while visible, the full-mode
daemon refuses with `"escalation requires --mode tiered"`
(`src/escalation.c:473-476`, `src/control.c:701-708`); the handler displays that
refusal and restores the button (`web/static/lib/panels.js:188-214`), without
claiming an escalation. `node --test tests/web_unit/escalation-button.test.mjs`
pins hiding and the visible-button request/refusal/settlement path through
the real handler, control client and transport, using DOM and daemon-peer
doubles. It is not a live daemon or browser rehearsal.

`tests/live_loop_workload.py` (issue #214; `adv_holder`/`io_reader` added
2026-10-06; `io_writer`/eight `lockmgr_N` sessions added 2026-10-07, owner
scope addition "add more load") loops seventeen persistent sessions for
the whole run, not just the two the §2 floors need. **Every pinned number
below this point in the doc predates the 2026-10-07 load addition and
needs re-measuring** -- the owner has implicitly accepted that by asking
for more load:
- `holder` / `waiter` / `sleeper`: `Lock:relation` recurs in every tick —
  the waiter genuinely blocking on the holder's lock, unchanged since issue
  #157 and what §2's floors and the conservation check are written
  against. `sleeper` stopped issuing `pg_sleep` on 2026-10-06 (owner: a
  literal `pg_sleep(1.3)` visible in the Top Queries panel at PGCONF.EU
  reads as a faked slow query to any DBA in the room, while the waiter's
  real multi-second block is honest slow work) — the session still exists
  but `fire()` is called with `sleep_s=0`, which skips the sleep statement
  entirely; the blocking SQL that produces `Lock:relation` is unconditional
  and unaffected. `pg_sleep` itself is untouched as the accuracy tests'
  known quantity (test_aas_accuracy.py, test_accuracy.py,
  test_deterministic.py, test_query_accuracy.py,
  test_capture_smoke.py's own default-arg call sites) — only this live
  demo loop changed.
- `reporter` / `adv_holder`: `reporter` rotates six structurally distinct
  SELECTs every tick (a catalog lookup, a CPU-bound aggregate, and FOUR
  differently-shaped advisory-lock holds) — the query-id and duration
  variety Queries, Histogram, Waterfall and Scatter need to show more than
  a single flat line. The advisory-lock shapes (added 2026-10-06, replacing
  two `pg_sleep` calls; expanded from two to four shapes 2026-10-07, owner
  scope addition — dropping `pg_sleep` cost the demo ~36s of visible DB
  Time per 90s window and the original two-shape replacement only gave
  back ~3.4s) have `adv_holder` hold `pg_advisory_lock(id)` — four distinct
  lock ids, 42/43/44/45 — for a client-side sleep (0.1s/0.3s/1.0s/3.0s)
  while `reporter`'s own `pg_advisory_lock(id)` call blocks on it for that
  long — a real `Lock:advisory` wait of exactly the hold duration, reading
  as an application mutex rather than a manufactured sleep.
- `row_holder` / `row_waiter`: a hot-row `UPDATE` contended by two backends,
  producing `Lock:transactionid` — a second, distinct wait class from the
  table-level `Lock:relation` above, realistic because contention on a
  shared counter/status row is one of the most common real-world lock
  waits.
- `io_reader`: added 2026-10-06 so the IO panels show real
  `IO:DataFileRead`/`IO:DataFileWrite` activity (owner: "we also need IO
  datafile read and write"). An initial version (`SELECT count(*) FROM
  pgbench_accounts`, a full sequential scan) was tried after a live run
  showed the Transitions tab thin (3-5 nodes), on the theory that more IO
  activity might add Transitions node variety — it did NOT ("nodes" there
  are distinct wait EVENT NAMES, `web/static/lib/builders/
  transitions.js`; measured 5 nodes with or without it on two independent
  box-check runs, 2026-10-07) — and a ~150MB scan every tick was heavy for
  no node-count benefit. **A separate, unrelated finding the same day**
  (`tests/results/sampler-294/round2/FINDING-ui-live-frame-spacing.txt` on
  another branch, read-only evidence) established that the Transitions
  tab's intermittent live-UI-smoke failure is a `frame_spacing`
  calibration edge on the tab's own tick-1 warm-up transient, reproduced
  on an unmodified tree with no workload change at all — not caused by
  this workload. `io_reader` was simplified 2026-10-07 regardless, on its
  own merits: an indexed single-row read on a random `aid`
  (pgbench_accounts has 1,000,000 rows) still produces a real
  `IO:DataFileRead` whenever that row's page is not in `shared_buffers`,
  at a small fraction of the scan's cost. No sleep, no CPU-dependent
  timing. One point-read per ~9s tick measured at ~0.1% of DB Time —
  not visible on the IO panels — so `io_reader` also runs a BATCH of 200
  random single-row reads every tick against `IO_LOAD_TABLE` (below).
- `io_writer`: added 2026-10-07, same scope addition. A batch of 100
  scattered single-row `UPDATE`s every tick against `IO_LOAD_TABLE`, a
  dedicated side table (`_smoke_io_load`, provisioned once by
  `tests/provision-runner.sh`, 3,000,000 rows, sized comfortably above
  this box's 128MB `shared_buffers` — see that script's own comment for
  the measured build time and on-disk size) — dirties pages that the
  background writer/checkpointer later evict, producing real
  `IO:DataFileWrite`. No sleep anywhere in either batch.
- `lockmgr_0`..`lockmgr_7`: added 2026-10-07, same scope addition
  (`LWLock:LockManager` explicitly requested weeks ago, still ~0.0%).
  Eight CONCURRENT sessions fire 20 repetitions each, every tick, of a
  query against `_smoke_lockmgr_fanout` (also provisioned once by
  `tests/provision-runner.sh`: a 200-partition RANGE-partitioned table,
  left empty on purpose) filtered on a non-partition-key column so no
  partition can be pruned. A single backend's fast path holds at most 16
  weak relation locks (a fixed PostgreSQL constant, not a GUC); touching
  200 relations in one query falls through to the shared, partitioned
  lock manager `LWLock:LockManager` guards. No sleep.

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

A rehearsal run on a different workload, client count, PG version, box class,
**sha, or tag** (see "The sequence, and what stops it from being rolled"
below — sha pre-tag, tag once one is cut, per the 2026-10-13 amendment) is a
different experiment and does not count toward the sequence.

## The release configuration

Originally pinned to the specific demo's hardware and client (owner,
2026-09-28); restated 2026-10-25 for the ordinary release gate since there is
no longer a specific event to match:

- **PostgreSQL 18** (latest).
- **The same hardware class as the gate box** — Hetzner `cx33` (`pgwt-gate` /
  `pgwt-gate-2`, or an ephemeral VM of the same class — CLAUDE.md). This is
  the box `make box-check` already uses; nothing new to provision.
- **The client for the one recorded walk (see "The recorded walk" below) is
  real Chrome on a laptop** — "a laptop", not "the demo Chrome": the
  1710x981 CSS @ DPR 2 viewport pin below exists because the #415/#462/#473
  fold checks are written against it, not because of anything about a
  presentation. Nothing eBPF runs on macOS, so the capture itself is always
  remote (daemon + PostgreSQL on the Linux box, browser + Go bridge on
  the laptop, ssh in between) — that topology is unchanged by the demo's
  cancellation, because it was never about the demo, it is how this product's
  client/server split works.

## Dropped: presenter-machine hygiene and the two-verdict split

**DROPPED 2026-10-25.** Everything that used to live here — an owner
pre-flight checklist for the presenting Mac (auto-lock, zoom, a second
display, Time Machine, quitting apps), the proposed "harness must assert
this" list for that checklist, and the "two verdicts, never merged"
capture-side/Mac-side split with its own void cap and attribution rules for
Mac `memory_pressure` — was stage insurance for a specific presenter's
specific machine at a specific event. None of it is genuine product
verification: it is care for the machine a human is about to demo from, and
there is no such machine or moment anymore.

**One piece of it is genuine and is kept, moved to "The recorded walk"
below:** a video recording cannot establish numerical correctness — it shows
layout, emptiness and motion, not whether the displayed numbers are right.
That point does not depend on any of the dropped apparatus, so it survives
the cut on its own merits.

## The recorded walk

**KEPT, restated as an ordinary release step.** One recorded real-Chrome walk
of all eleven tabs is made per release candidate (`scripts/demo-walk-record.py`
or an equivalent manual recording), and that recording is analysed by both
standing advisers (codex and Fable) plus the lead before the tag — not by the
owner; see "What changed and why" for why the owner's own walks are dropped.
This replaces the old pair of owner-signed Mac walks with a single recording
and an asynchronous three-way review, which is enough to catch a visible
defect (blank panel, console error, frozen chart, a UI regression the
automated gates below do not look for) without needing a human to sit through
it live.

**A recording cannot establish numerical correctness — say this explicitly so
a clean recording is never treated as proof the numbers are right.** Video
shows layout, emptiness and motion: whether a panel renders, whether it is
visibly blank, whether a chart advances. It cannot show whether the AAS value
on screen is the AAS the capture actually produced. That is what §1-§8 below
(the automated capture-side checks) and a direct comparison of displayed
values against the underlying API responses are for — the recording and the
numeric checks are two different kinds of evidence, and neither substitutes
for the other.

### The client, pinned — restated 2026-10-25: "a laptop", not "the demo Chrome"

This pin originally described a specific presenter's machine for a specific
event (owner, 2026-09-28: "Screen, chrome"). The event is cancelled, but the
number is not stage trivia: the #415/#462/#473 fold checks are written
against this exact viewport, so changing it would change what those checks
actually exercise. It is kept, unchanged, as **the pinned viewport for the one
recorded walk** (see "The recorded walk" below) — a laptop running real
Chrome, not a special "demo" configuration.

- **Display: the laptop's built-in screen**, not an external monitor —
  `Color LCD`, Built-in Liquid Retina, **2880 x 1864 Retina**, `Main Display:
  Yes`.
- **Full screen.** Historically pinned to
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
  built-in panel, 100% zoom, in a clean profile. Environment to record
  alongside the recorded walk (see below): **macOS 15.6.1 (24G90), Chrome
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

  Pin the Chrome and macOS versions in the recorded walk's record: a version
  update between the recording and the tag could change rendering, paint
  timing and WebSocket behaviour, which is exactly what the recording is
  supposed to show honestly.

  **Re-confirmed on Chrome 154.0.8037.58** (2026-09-30, diagnostic reading,
  no committed artifact): two agreeing reads, full screen, 100% zoom,
  `windowState: fullscreen`, built-in `Color LCD` as the only attached
  display. Unchanged: still **1710 x 981 CSS at DPR 2**. Record whichever
  Chrome version is actually installed at the time of the recorded walk — the
  pin held across this one version bump, but the next bump needs its own
  re-check, not an assumption that it is still unchanged.

  **Still unmeasured, stated as such rather than assumed equal**: the
  screen-shared (Zoom/Meet/Teams) variant. Screen sharing changes
  resolution, scaling and layout in ways not yet quantified here, and
  needs the owner driving the real conferencing tool to measure — open
  item, not a criterion yet.

**Display confirmed**: the built-in panel, not an external monitor — the
identity is recorded alongside the Chrome and macOS versions, and the pinned
viewport is the measured **1710 x 981 CSS at DPR 2** above (the 1440 x 932 and
the superseded Safari 1710 x 1069 figures are not second final values) — a
walk at a different display, or a different browser, is a different
experiment, exactly as a different PG version would be.

### Known behavior: recovery after the bridge drops

Observed once, kept as a documented behavior rather than a required gate step
(the owner-signed step-3 bridge-drop drill this used to describe is part of
the retired Mac-walk apparatus above). The code path is sound on inspection —
`connect()` re-fetches the session token on every attempt, so the bridge's
per-process token rotating on restart does not strand an open tab — and a
2026-09-30 diagnostic observation on the real topology (Go bridge killed,
tearing down ssh and `pgwt-server` with it) matched that: the degraded overlay
appeared in 0.4 s with reason `disconnected`, previously loaded rows were
retained rather than blanked, and the UI recovered in **2.0 s** without a
reload, with a second `/session` request returning 200. This is informational
background, not a pass/fail criterion; nothing below requires reproducing it
per release.

### `pgwt-stage` is retired — it was never provisioned

This document used to require a dedicated, pre-provisioned `pgwt-stage` box
for the demo itself (owner, 2026-09-28: "we will provision dedicated node
couple days in advance"), distinct from the gate boxes and the rehearsal's own
throwaway VMs. **It was never built.** The owner's later decision (2026-10-13,
delivery-queue item 4.1) already had the gate box (`pgwt-gate`) stand in for
it for the counted rehearsals, with a real dedicated box deferred to "before
the real demo". That event is cancelled, so the deferred box is simply never
needed — there is no live event left for it to serve.

Whatever still needs an uncontended Linux box (e.g. a paired overhead A/B,
which must not share CPU with other work) uses **gate-2**, which CLAUDE.md
already names as the reproduction box: reserve it exclusively for the
duration of the paired measurement, and pin CPU model, kernel, PG version,
configuration and dataset so the two sides of the pair are comparable. A
pinned ephemeral VM also works for a paired measurement, since neighbour
noise pairs out within the one VM — but it loses cross-day comparability
(#451), so prefer gate-2 when it is free (CLAUDE.md's "persistent boxes
first" rule).

## A release is CLEAN only if ALL of these hold

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
  `Lock:advisory` with non-zero time. This is derived, not chosen: the
  workload creates those two events by construction, so their absence means we
  captured something other than the workload. (Was `Lock:relation` +
  `Timeout:PgSleep` until 2026-10-06, when the DEMO workload dropped
  `pg_sleep` entirely — owner: a literal `pg_sleep(1.3)` visible in the Top
  Queries panel at PGCONF.EU reads as a faked slow query to any DBA in the
  room. `pg_sleep` is untouched as the accuracy tests' known quantity; only
  this live loop's required pair changed, to what it now produces instead —
  `adv_holder` holding `pg_advisory_lock(42)` while `reporter`'s own call
  blocks on it.);
- **AAS ≥ 1.15 on the 60 s recent window — WITHDRAWN as a gate**
  (owner-approved criteria amendment, 2026-10-24). AAS is now **reported,
  not gated**, following §3's Off-CPU* precedent: the retained `aas_floor`
  diagnostic has `ok: null`, `gating: false`, the measured `aas`, and its
  detail string. Neither a low nor a high reading gives a pass/fail verdict.

  **Hazard analysis is the basis.** The floor's own code comment stated its
  only purpose: the provisional 0.5 rejected an AAS reading of ~0 the same
  way `MIN_DB_TIME_MS` rejects a ~0 `db_time_ms`. That anti-vacuity hazard is
  already closed by three unchanged checks: `MIN_DB_TIME_MS = 1.0`, checked
  before any ratio; `capture_has_events_ok`; and
  `workload_signature_present_ok`, requiring **both** `Lock:relation` and
  `Lock:advisory` with ms > 0. Raising 0.5 to 1.15 attached no new hazard.
  The historical derivation was half of the first clean run's
  `cross_tab_aas_agreement` reading, `derived=2.3033 time_model=2.3033`
  (`run.id` 1790869219, n=1): 1.1517, recorded as 1.15. That run used a
  throwaway VM (`root@46.225.4.87`) which no longer exists. Neither adviser
  found a downstream check that becomes vacuous between AAS 1.01 and 1.12.
  AAS = active time / window also partly measures box speed: the same code
  on two cx33s read 1.23–1.43 on gate-1 and 1.01–1.12 on gate-2.

  **The trigger for this review was a red run:** gate-2 measured AAS
  **1.01–1.12 on master `0479723` while rendering all 33 tab-walks**. This
  project's rule is that a bound is never the thing to change to make a red
  go green. The defence for withdrawing this one is the hazard analysis
  above: it holds whether gate-2 read **1.01 or 1.30**. The anti-vacuity
  checks already close the stated hazard independently of that red result;
  this amendment withdraws an unsupported gate rather than recalibrating
  it to the failing box. Demo richness remains independently gated by
  `tests/demo_workload_coverage.py`: `SCATTER_MIN_POINTS = 5`,
  `SCATTER_MIN_SPREAD_RATIO = 3.0`, `WATERFALL_INTERESTING_MS = 500`, per-tab
  `*_populated()` checks and `concurrency_populated(resp, num_cpus)`.

  **AAS availability is still gated — only its magnitude was withdrawn.** A server
  that stops reporting AAS altogether still fails the rehearsal loudly:
  `cross_tab_aas_agreement` is in `REQUIRED_EXTRA_CHECKS`, runs on the identical
  recent window, and `bucket_weighted_aas_ok` fails closed on a missing or
  non-numeric `time_model` `aas`. Unlike §3's Off-CPU diagnostic, the `aas_floor`
  entry carries no `available` leg of its own, so do not re-derive a fail-safe
  gap from its absence — the availability check lives in the extra-checks set.
  Demo richness is also still gated on **peak AAS ≥ `num_cpus`**
  (`tests/demo_workload_coverage.py:192`), which is a stricter bar than the
  withdrawn 1.15 window average.

  **Temporary qualification gap:** the replacement, applied-load
  measurement gating what was offered rather than the box's response, is
  **planned and not yet built**. Existing anti-vacuity and demo-richness
  gates do not fill that applied-load qualification gap; the criteria are
  incomplete in that respect until the replacement lands.

  **Counter reset:** this criteria change resets the clean-attempt counter
  under this document's own rule. At landing the counter was **0 of 4
  counted (1 of 2 voids)**, so nothing was voided by this amendment.
- all 11 tabs reached and rendered;
- the daemon process alive for the whole window.

### 3. Conservation, sampled through the run

- DB Time = CPU\* + Off-CPU\* + Σ waits within `TIME_MODEL_TOLERANCE_PCT` (1%).
- Evaluated **every 5 minutes** through the capture, not once at the end. Every
  sample must pass and carry `offcpu.available == true`. Missing measured CPU
  (`has_measured_cpu=False`) is absence, with null magnitude/percentage, and
  refuses the attempt. No Off-CPU magnitude cap is registered; present physical
  values remain reported-only. A 35-minute run judged on its last 60 seconds is n=1.
- **The identity alone is known to be insufficient and must not be quoted as
  proof of correct attribution.** On the raw/exact path Off-CPU\* is the
  residual `max(0, DB − CPU* − Σwaits)` (`src/compute.c`), so a wait class we
  fail to attribute is absorbed into Off-CPU\* and the identity still closes.
  Only over-attribution can move the gap. Therefore also require:
  - **Per-class wait CPU, not a DB-Time fraction.** `Timeout:PgSleep` and
    `Lock:relation` are pure sleeps: a correct implementation records
    essentially no on-CPU time inside them (cpu/dur well under 0.01%,
    bounded by at most a few deadlock-timeout wakeups). **Restated
    2026-10-13 — criteria change, measured before written (see the RESOLVED
    paragraph below for the measurement that drove it):** the signature is
    `cpu_ns` approaching `duration_ns`, not `cpu_ns` alone: a stale wait
    label or an unclosed `on_cpu_ts` books most of the wait's wall time as
    CPU. Flag a pure-sleep event only when BOTH hold — **`cpu_ns` >=
    824,430 ns AND `cpu_ns`/`duration_ns` >= 50%**. Report per-event maxima
    of both quantities separately; a class mean hides the outlier. **Why
    the conjunction, not either half:** absolute-only fires on a benign
    3.004 s sleep carrying 923,834 ns at **0.031%** of its own duration —
    the event is just long, not over-attributed; ratio-only would
    false-positive on a 0.159 ms uncontended advisory-lock acquisition
    carrying 52,661 ns at **33.148%** — a real but tiny absolute cost, not
    a stuck CPU clock. Worst observed ratio is 33.148%, giving ~1.8x
    margin to the 50% line; the absolute threshold stays at 824,430 ns,
    unmoved by this restatement. This is not a weakened bar: the previous
    wording ("a PgSleep or Lock event carrying cpu_ns on the order of a
    millisecond is the over-attribution signature") named no threshold at
    all, and a literal reading of it would have flagged the 923,834 ns
    event above while a genuine 33.148%-ratio event sat unremarked in the
    same pair of captures — the restated test catches the real case and
    correctly clears the benign one.
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

  Per-class wait CPU is still not on the wire in the `time_model` response
  — the measurement below is a direct offline raw-trace walk, run once, not
  a check wired into `demo_rehearsal.py`. **RESOLVED 2026-10-13 — answered
  by changing the instrument, not the bar.** This paragraph used to say to
  settle it via `tests/cross_validate.c` over "any full-mode gate-box
  trace"; that finding stands as the reason the original method could never
  work, not as an open blocker: `tests/cross_validate.c`'s own header says
  it "reads a **tiered-mode** trace directory that contains BOTH sampled
  blocks (always-on) and transition blocks (escalation window)" and
  compares the two over their overlap window — it needs SAMPLES blocks to
  compare against, and **full mode produces none.** Confirmed against the
  retained 35-minute full-mode trace from the first clean rehearsal:
  `pgwt-server --dump` on it reports `4432286 transitions, 0 samples,
  Fidelity: exact`; `cross_validate` on the same trace exits with `ERROR: no
  sample blocks — sampler produced nothing` (`tests/cross_validate.c`'s own
  message, line ~149) — a structural mismatch between the tool's input
  contract and full mode's output, not fixable by re-running it against a
  different full-mode trace.

  **What actually answered it:** a per-event `cpu_ns` extractor that walks
  a full-mode raw trace directly (not `cross_validate.c`), built and run on
  `agent/cx-25-raw-perclass-cpu` @ `853cfc1`
  (worktree `.claude/worktrees/agent-a98a1ee2ed1061c28`), evidence under
  `tests/results/issue25-perclass-raw-scan/`. Two full-coverage walks, both
  cross-verified against block headers *and* `pgwt-server --dump`, 0
  unknown `cpu_ns` in either:
  - **Oct-2 capture:** 4,307,056 / 4,307,056 records, 1052/1052 blocks.
    `Timeout:PgSleep` 326 events, max 923,834 ns at **0.0308%** ratio.
    `Lock:relation` 436 events, max ratio **0.0148%**.
  - **Oct-13 current-workload capture:** 7,385,206 / 7,385,206 records,
    1804/1804 blocks. `Lock:advisory` 179 events, max absolute 210,008 ns
    on a 1.000 s hold (0.021%), max ratio **33.148%** on a 0.159 ms
    acquisition (52,661 ns absolute). `Lock:relation` 392 events, max ratio
    0.025%. `Lock:tuple` 1 event, 0.465%.

  **Combined: 1,850 pure-sleep events, zero showing over-attribution** by
  the restated test above. Contrast: 57 of 64 recorded threshold crossings
  sit at ratio **≥50%** at millisecond scale, all in IO/Client classes
  where CPU legitimately dominates — that is what the real signature looks
  like, and it is visibly absent from every pure-sleep event measured.

  **Measured envelope, recorded but not used to move the threshold:**
  sleep-class p99 rose from 79-91 µs (Oct-2 capture) to 171-203 µs (Oct-13
  capture) as the workload changed between the two captures. The unchanged
  824,430 ns absolute threshold is therefore now **4.05x** the worst
  sleep-class p99, down from 10x against the Oct-2 figures alone — the
  envelope widened and the threshold held anyway; it was neither
  retightened when the margin was 10x nor loosened now that it is 4x.

  **Unmeasured, recorded as unmeasured rather than folded into "clean":**
  `Lock:extend` had 0 events in the Oct-13 capture (2 in an earlier,
  unretained capture — statistically empty, not evidence either way).
  `Timeout:PgSleep` had 0 events in the Oct-13 capture because the workload
  dropped `pg_sleep` on 2026-10-06 (§2 above); it is covered only by the
  Oct-2 capture's 326 events above, not by current-workload data.

  **This is a criteria change, made the legitimate way** (this document's
  own rule: a criteria change is fine "by a commit that says so"). It does
  not weaken the bar: a literal reading of the old wording ("cpu_ns on the
  order of a millisecond") would have flagged the Oct-2 923,834 ns PgSleep
  event — genuinely millisecond-scale in absolute terms — as the thing it
  is defined to catch, while the real 33.148%-ratio advisory-lock event sat
  in the same pair of captures unremarked by an absolute-only reading. The
  restated conjunction clears the first and would catch the second. The
  measurement preceded the restatement, not the reverse: both captures
  above were walked first, the false-fire and false-negative were found,
  and only then was the wording rewritten to name the mechanism it needs to
  detect.

  **Related historical data point, same dump:** over the whole 1875.8 s
  capture, `CPU (waiting for a core)` was **746.5 ms against 4,170,804.2 ms
  of DB Time — 0.018%** (746.5 / 4170804.2 × 100). This predates the
  2026-10-07 load addition; it does not calibrate the current workload.

- **Instrument added 2026-10-13 (issue #386), bound deliberately NOT
  changed by it.** Two rehearsal windows failed the then-current 10% cap at
  **10.04%** and **11.48%**, and the stated cause — "contention from the 2026-10-07 load
  increase" — was an *inference*, never a measurement: run-queue delay and
  uninstrumented in-execution blocking (major faults, writeback) produce
  the identical residual, and the failing run's trace had already been
  deleted. The rehearsal now measures the run-queue component
  **independently of our own instrumentation**: per-PG-backend
  `/proc/<pid>/schedstat` **field 2** (`run_delay` — cumulative ns a task
  spent runnable but not on a CPU), sampled every 5 s across the capture
  and differenced over the *same* window each `offcpu_cap` sample uses, so
  each window records three figures — `offcpu_ms`, `run_delay_ms`, and the
  **unexplained remainder** `offcpu_ms − run_delay_ms` — plus the daemon's
  own `%CPU` and the box's load/run-queue length (an adviser's point: a
  "regime step" part-way into a long capture could be the *observer*
  getting heavier as the trace grows rather than the workload getting
  contended; those two series separate the cases).
  **How the remainder may be read (the asymmetry is the point).**
  Σ`run_delay` over the backends is an **upper bound** on the run-queue
  share that could be *inside* Off-CPU\*, not an exact match: the kernel
  counter also accrues while a backend is idle, in `ClientRead`, or already
  inside another wait event, and none of that time is in Off-CPU\* (same
  distinction this section's point 3 already draws). So a **positive**
  unexplained remainder is the strong reading — even the upper bound of
  run-queue delay fails to cover the residual, so an instrumentation gap
  must account for the difference — while a **non-positive** one only says
  run-queue delay *could* explain it. A bound belongs on the positive
  direction. The remainder is therefore reported signed, never clamped at
  0: clamping would erase the very overlap that tells a reader which of the
  two readings they are in.
  `tests/demo_rehearsal_lib.py` gained no check and no changed tolerance
  from this instrument (it was written while `OFFCPU_CAP_PCT` was still
  10.0; the cap itself was removed by the "reported, not gated" bullet
  below, which this instrument neither anticipated nor depends on — it
  measures `run_delay` whether or not anything gates on the residual).
  **The bound on the unexplained
  remainder is not registered here**, because registering a bound in the
  same change that first measures it is exactly what this document exists
  to prevent — it gets its own reviewed commit once the distribution over a
  full run is known. Why it is the bound worth having: the unexplained
  remainder is workload-independent, so unlike a percentage-of-DB-Time cap
  it would survive a load change, whereas the 10% cap that used to sit here
  was in part a proxy for how contended the box happened to be — one of the
  reasons it was removed rather than retuned.
  **Blind spot, stated:** `run_delay` only accumulates when
  `kernel.sched_schedstats` is 1, and it is **0 by default on both gate
  boxes** (measured 2026-10-13). `tests/demo_rehearsal.sh` now enables it
  for the duration of a run and restores it at teardown; when it cannot,
  every window records an explicit **refusal** instead of a 0 —
  a 0 there would credit the whole residual to "unexplained" with no
  evidence at all (`tests/schedstat_lib.py`, bypass suite in
  `tests/test_schedstat_lib.py`). That sysctl is **global**, so two runs on
  one box can reset it under each other; the result is refusals, not wrong
  numbers, and the lock is the remedy.
  **Second blind spot, now closed by measurement:** those refusal paths
  detect *blindness*, never a counter that is readable but **stuck** — a
  wrong field offset or a wrong backend enumeration would produce a
  plausible Σ`run_delay` with no refusal at all. So the reading's
  responsiveness was measured directly, once, on gate-2
  (`tests/schedstat_contention_check.sh`, artifacts and verdict in
  `tests/results/schedstat_contention/`, 2026-10-13, nproc=4, 10 s per leg):
  schedstats=0 **refused** (rc=2, `run_delay_ms` null, not 0 ms); four
  sleeping children read **0.0 ms**; sixteen spinning children on four vCPUs
  read **111,775.654 ms** — a **111,776×** spread no stuck counter can
  produce. This establishes responsiveness and sign, **not accuracy against
  an independent oracle**, which is a further reason the decomposition stays
  reporting-only and no bound on the unexplained remainder is registered.

- **Off-CPU\* is reported, not gated (replacement for #386, 2026-10-13).**
  `evaluate_time_model_window` now retains an `offcpu` diagnostic containing
  physical `offcpu_ms`, `db_time_ms`, and percentage, with `ok: null` and
  `gating: false`; it has no `offcpu_cap` pass/fail sub-check. The existing
  conservation, raw-path, nonzero-DB-Time, CPU-clamp, workload-signature and
  AAS gates remain. This criteria change resets the clean-attempt counter;
  the reported figures below are calibration evidence, not counted attempts.

  **Settled decomposition and provenance:** [#386's “SOLVED by offline
  decomposition” comment](https://github.com/DmitryNFomin/pg_wait_tracer/issues/386#issuecomment-6151275012),
  with tool and transcript under `tests/results/offcpu_decomposition/` on
  `agent/offcpu-run-delay-decomposition`. On the retained **gate-1, 1834 s**
  capture, **96.1%** of Off-CPU\* is the eight-session
  `_smoke_lockmgr_fanout` `SELECT count(*)` tick (96.3% on round-2):
  **186 bursts**, about **1.1 s every 10 s**, eight runnable backends on
  **four shared vCPUs**. Between bursts Off-CPU is effectively zero.
  This is physical contention introduced by the 2026-10-07 workload addition,
  outside the former clients ≤ cores premise.

  The gate's own **60 s window at all 1775 placements** across that clean
  run gives **min 5.08%, median 8.96%, p95 11.25%, max 13.52%**.
  **405/1775 (22.8%)** exceed 10%; the old bound lies near p77 and the
  estimated probability of seven consecutive passing windows is **0.16**.
  Window phase determines the verdict; this is not a product-health signal.

  **Whole-capture alternative, with measured derivation:** burst-magnitude
  CV **0.42** and **6.09 bursts/window** predict window CV
  `0.42 / sqrt(6.09) ≈ 0.170` (unrounded decomposition prediction **0.169**,
  observed **0.177**). Across **186 bursts**, expected CV is
  `0.42 / sqrt(186) = 0.0308` (**3.08%**); scaling the observed window CV
  gives `0.177 × sqrt(6.09/186) = 0.0320` (**3.20%**).
  Starting from that capture's measured **8.405%** whole-capture share,
  a candidate mean + three-CV upper envelope is
  `8.405% × (1 + 3 × 0.0308) = 9.18%` (9.21% using observed CV).
  **This is a model-based contention envelope, not a calibrated probability
  or a correctness bound; it is not enforced.** Aggregation reduces phase
  noise but does not separate a defect from contention: round-2's
  whole-capture share was **2.779%**, versus gate-1's **8.405%**.

  **Concurrency alternative:** instantaneous runnable count tracks this
  mechanism (passing window samples max **2**, other windows mean
  **3.33–4.50**, max **7–15**), but still measures the box, not attribution
  correctness. Neither alternative can usefully gate product correctness
  here. **No replacement correctness cap is registered.** This deliberate
  reporting-only decision replaces the unsupported 10% placeholder; the
  previous 4.92 pp citation was a real sum (2.53 + 2.39 pp), but measured
  another regime, denominator and quantity. **The recorded owner decision
  that #115 (`agent/observer-bias-study`) is not landing before the tag
  remains unchanged.**

  **What the replacement does NOT detect:** a dropped wait label moves wall
  time from a wait into a CPU gap and **raises Off-CPU\*** while the
  accounting still closes. The separate **per-class wait-CPU scan (queue
  item 2.5, resolved above)** supplies the attribution evidence; Off-CPU
  magnitude is not a dropped-label detector.

  Each raw window's physical magnitude stays in the saved conservation
  samples. The whole-capture **summary path has no Off-CPU signal**, even
  when labelled `fidelity: exact`; legacy/sampled data is also unavailable.
  Both report `available: false` and null magnitude/percentage, not measured
  zero or PASS. Below the DB-Time floor the percentage is null and the
  independent conservation gate still fails. Sparse trailing windows are
  not averaged and presented as a whole-capture measurement.

**Known, unreproduced item: #202** (owner, 2026-09-30 — recorded, not closed).
`test_multi_window.py`'s own conservation check once measured class rows
summing to 20.2% more than DB Time (DB Time 15255 ms vs. Σ top-level class
rows 18330 ms), an over-attribution direction this identity *can* detect, on
a single `make box-check` run. n=1, never reproduced (the issue itself notes
"a second box run is in progress" and carries no comment recording its
result). This criteria commit does NOT re-run that reproduction. Separately,
the 35-minute rehearsal referenced elsewhere in this document recorded its
own eight conservation samples through the capture as all 0.00% gap, with
cross-tab DB Time agreement also 0.00%. The latter is the shared-total
identity retired from the gate under #258, so it supplies no independent
attribution evidence and cannot refute #202. **#202
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
  dependency is resolved, not open. A positive `attempted_count` is also
  required: zero attempted ticks is no measured coverage. Each tab's sweep
  must have at least one tick with a valid first sample before the existing
  500 ms second target; per-tab absence refuses the attempt. Every per-tick
  miss remains reported as `EARLY WINDOW LIKELY MISSED`; per-tick strictness
  is deliberately deferred pending the scheduling-tail investigation (#397).
  Other sweep drift remains reported-only, without a new spacing threshold.
- **Per-tab time to first paint — SETTLED: 3000 ms, global (owner, review
  round 6).** `tests/ui_live_smoke.py`'s
  `_navigate_to_tab` carries `ttfp_ms` (issue #245, merged to `master` as
  `fa20067` / PR #257): elapsed ms from the navigation's own landing click
  (the Sessions-row drill-down click for Timeline, the bare tab click
  everywhere else) to the ViewManager's first fresh mount of that tab.
  `build_tab_result` now enforces <= 3000 ms on each tab/navigation and refuses
  absent or invalid readings; failure detail names the tab and measured value.
  This VM-side regression detector does not replace the manual Mac sign-off.

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

  > **These numbers do not describe the workload we intend to demo (2026-10-05).**
  > They were recorded against the base workload only — pgbench `-c 4 --rate=25`
  > plus the Python lock/sleep loop, roughly 1,000 trace records per second. They
  > are internally consistent for that workload and were honestly measured; they
  > are simply not the workload the owner asked for ("spikes, lwlocks,
  > concurrency", every panel populated).
  >
  > Two independent reasons they cannot be carried forward:
  >
  > 1. **The enriched workload is denser.** With the four-burst cycle actually
  >    running, the capture reaches ~4,154 records/s after detuning (6,692 before).
  >    Server-side cost scales with in-window volume: at a 900 s window the five
  >    raw-event endpoints measured 2,059-3,254 ms, against 256-423 ms at 120 s.
  >    See #283 — `current.trace` events are never cached, so a moving window
  >    re-decodes ~98% of the same data every refresh, which is the dominant term.
  >
  > 2. **Every summaries-path number here was inflated by #277.** The summary
  >    writer double-wrote the in-progress second on every tick flush, so any
  >    window >= 120 s with no pid filter over-reported: measured 2.16x at `-i 1`
  >    and 1.14x at `-i 5`. That covers Overview AAS, Events, Time Model, Top
  >    Queries and Sessions. The fix does not repair already-written summary files,
  >    so the capture behind this table cannot be re-analysed — it has to be
  >    retaken.
  >
  > Additionally, during part of the period these were pinned, the burst workload
  > was silently a no-op: an `rsync --delete` into the workload directory removed
  > the four pgbench script files at ~12:06 UTC on 2026-10-02, and the driver
  > filtered pgbench's stderr through `grep`, so the failure was invisible. The
  > last `pgwt_burst` row is 12:01:41 UTC. Any burst-related claim measured after
  > that time, until the workload moved to `/root/workload` with a sentinel file
  > and per-invocation assertions, is unsupported.
  >
  > Re-pin after #277 merges, on a fresh capture, recording: capture age, in-window
  > record count, distinct sessions, achieved burst rates, box load, and which file
  > each number came from.

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
  the old 10% Off-CPU placeholder (now superseded by §3’s reporting-only
  decision) and kept this bound at 3000 ms when
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
  1. **Not "flat", and not unbounded — grows by at most a few milliseconds
     over the demo.** `handle_top_queries` opens `current.trace` three
     times per request: once via `coverage_refresh` inside
     `server_load_events_fi_mode` (`src/server.c:1845`), once more via
     `load_file_range_mono`'s own `pgwt_reader_open` for the actual data
     load (`src/server.c:1908`, function at `src/server.c:1064`), and a
     third time via `coverage_refresh` inside `should_use_summaries`
     (`src/server.c:2259`). Each open rebuilds `current.trace`'s block
     index from all committed block headers via its meta-file strategy
     (`pgwt_reader_open`, `src/event_reader.c`) — so this part of the cost
     does grow with the capture, not with the requested window. But all
     four rows in the table above were measured against the same capture
     (so `current.trace` held the same committed-block count for all of
     them, varying only the requested window), and the 60 s row already
     pays this fixed per-request cost in full: its 14 ms total bounds it.
     That caps the fixed component, not the total — **391 ms is a
     measurement for this 43-minute session, not a constant that would
     hold at a longer one**, since the fixed component itself would be
     larger against a `current.trace` holding more committed blocks (up to
     an hour's worth before rotation resets it; see the runbook's
     hourly-rotation note).
  2. **The 60 s window's 0 rows is not explained here as "no completed
     executions."** Below 120 s this endpoint uses raw events; at 5
     minutes and above it can use summaries for the class breakdown
     (`should_use_summaries`). Those are different code paths measured at
     different window sizes, and the latency numbers above do not by
     themselves establish why row counts differ between them — record the
     latency observation without attaching that cause. Separately measured
     (all windows anchored well inside the capture, ending 5 minutes before
     the last event, so no edge effects): a 110 s window (raw path) and a
     130 s window (summaries path, since it crosses the 120 s boundary)
     both returned **0 rows**; 300 s and 900 s windows (both summaries)
     returned 7 rows each. The 130 s result disproves treating 120 s as the
     boundary between "empty" and "has rows" — rows appear somewhere
     between 130 s and 5 minutes, and this data does not say where or why.
     **Practical consequence: a window shorter than ~5 minutes can render
     an empty Queries panel with no error** — keep the demo's range at 15
     minutes; never narrow to 1 minute on Queries.
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
  The automated attribution constraint is mandatory
  `cross_tab_class_attribution` (`class_rows_vs_top_events_ok`): regroup
  Top Events rows labeled by `pgwt_class_name` against Overview class rows
  attributed by `pgwt_wait_class_index`, over the identical recent window.
  Query errors fail the check; a missing entry fails required-check membership.
  It runs unconditionally in the same path that formerly ran the total check.
- Freshness: at the end of the walk, every time-axis tab's newest bucket is
  within 2 ticks of wall clock. A frozen chart is neither blank nor throwing.

**#258: retired total-only gate and measured subsumption (2026-10-26).**
On master `d02b7bd6`, the 60s recent window is below the server's 120s
summary threshold. Both handlers use `server_load_events_fi` with identical
bounds and PID, then sum clipped non-idle, non-io_worker intervals. The hidden
Activity events skipped by Top Events are already idle. The accumulators are
double vs integer nanoseconds, but there is no independent attribution input
in that total comparison. `cross_tab_db_time_agreement` no longer runs or
appears in `REQUIRED_EXTRA_CHECKS`; its helper remains a non-gating historical
diagnostic used by the mutation regression test. Per-class agreement supplies
the attribution constraint that the shared totals cannot supply.

Measured with the production raw accumulators over a 60s window: one 1000ms
CPU event with `cpu_ns == duration_ns`, and ONE 2000ms `Lock:relation` event.
Both checks passed before injection. In a temporary copy of `compute.c`,
skip only that Lock event's attribution immediately after adding its duration
to `db_time_ns`. Top Events receives the unchanged two-event array. Both DB
Time totals remain 3000ms; time_model's Off-CPU remainder grows from 0 to
2000ms. The observed outputs on that same mutated input were:

```text
cross_tab_class_attribution: FAIL -- 2 classes, worst gap 100.00% (tolerance 1.0%): CPU tm=3000.0/te=1000.0 (66.67%); Lock tm=0.0/te=2000.0 (100.00%) -- DISAGREES: CPU: time_model=3000.0ms vs top_events=1000.0ms (66.67%); Lock: time_model=0.0ms vs top_events=2000.0ms (100.00%)
cross_tab_db_time_agreement: PASS -- a=3000.0ms b=3000.0ms gap=0.00% (tolerance 1.0%)
```

`test_class_attribution_subsumes_total_check_for_one_dropped_event` preserves
these response rows and asserts both outcomes, residual conservation, and
failure of the rehearsal verdict when the surviving gate fails or is absent.
The old total comparison's zero gap is not evidence of correct attribution.

Separate existing limitation observed on this same tree: with the healthy
CPU event measured at 500ms instead of 1000ms, Top Events reports 500ms
separately as `offcpu_ms`. Its rows then sum to 2500ms against 3000ms DB Time,
and the current class comparator refuses with a 16.67% row-sum gap. Its
precondition still expects the earlier unsplit CPU row. This is a false
failure on healthy measured Off-CPU data, not a skip; #258 leaves that
assertion unchanged.

### 6. Daemon integrity

- `degraded_warnings == []` is required. An announced WARN fallback or LEGACY
  tier is still a degraded full-mode capture and refuses the attempt.
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
- **One-time pre-demo overhead envelope: DONE, measured 2026-10-13 on
  gate-1, PG18.** n=3 paired A/B, interleaved, `--mode full`: pgbench +
  `tests/live_loop_workload.py` **unthrottled at 4 clients** (the criteria's
  own client count) gave **30.31% ± 0.58pp**, inside the registered
  ~28-32% band below. `--rate=25` was **deliberately dropped** for this
  measurement: under a rate cap both arms are held to the same TPS ceiling
  by the cap itself, not by the tracer, so TPS-based overhead degenerates
  toward zero regardless of the real cost — this result is the demo
  workload's client count but not its throttle, and is labelled that way.
  A separate n=3 A/B at 8 clients with `demos/workload.sh` (the saturated
  regime, §3 above) gave 39.41% on gate-1 / 34.63% on gate-2 — kept as
  secondary context only, not compared directly to the 30.31% figure.
  Full untailed logs: `tests/results/overhead-envelope/ENVELOPE.md`.
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
| §2 `Lock:relation` + `Lock:advisory` present (was `Timeout:PgSleep`; changed 2026-10-06, see §2 above) | yes (`workload_signature_present_ok`) |
| §2 AAS on the recent window (1.15 floor withdrawn, 2026-10-24) | **reported, not gated** (`aas_floor_ok`, `aas_floor.ok: null`, `gating: false`, measured `aas` and detail retained). Anti-vacuity stays with `MIN_DB_TIME_MS`, `capture_has_events_ok` and `workload_signature_present_ok`; applied-load qualification is **planned, not built — a temporary gap** (see §2 amendment) |
| §2 all 11 tabs reached, daemon alive throughout | yes (`build_demo_summary`'s `expected_tabs_per_pass` floor; `_assert_daemon_alive`, mirrors the existing workload-alive fail-safe) |
| §3 conservation identity | yes, sampled every ~5 minutes through the capture (`conservation_sample_interval_s`), every sample must pass — not once at the end |
| §3 `wait_gap_cpu_ms` ≤ 0.1% | **removed, not implemented** — corrected 2026-09-27/28: this was never a valid over-attribution detector (the BPF measures on-CPU across the whole wait-start/wait-end span, so an IO wait is legitimately CPU-bearing; two live runs measured 0.376–0.392%, expected on a correct implementation). The real per-class signature (`cpu_ns` approaching `duration_ns` on a `Timeout:PgSleep`/`Lock:*` event) still has no field on this wire — **measured offline instead, 2026-10-13**, via a direct raw-trace walk, not through `time_model`: see §3's RESOLVED per-class paragraph above for the figures and the restated threshold (`cpu_ns` >= 824,430 ns AND ratio >= 50%, both required). Not a wired automatic gate inside `demo_rehearsal.py` |
| §3 `cpu_clamped_ms` ≤ 0.1% | yes (`cpu_clamped_ok`), scope corrected in its own code comment: vouches for CPU-class gaps only, never wait events (`src/compute.c`'s wait branch never clamps `cpu_ns > dur`) |
| §3 Off-CPU\* physical magnitude | **reported, not gated** (`time_model_offcpu_diagnostic`, `offcpu.ok: null`, `gating: false`) — #386’s settled decomposition and all 1775 clean-run window placements invalidate the 10% cap. Expected whole-capture CV 3.08–3.20%; candidate 9.18% envelope is contention context only, not a correctness bound. Raw samples retain ms and percentage; unavailable summary/tier data is null, not zero. Every gating sample requires `offcpu.available == true`; physical magnitude stays reported-only. The independent accounting gates remain |
| §4 zero known-failing tabs | yes (`build_demo_summary` uses raw `ok`, ignoring the exemption) |
| §4 blink measured fraction ≥ 0.5 | yes — positive attempted count is also required, and each tab sweep refuses early-window absence (no tick with a valid first sample before 500 ms); every per-tick miss remains visible, with strictness deferred under #397; landed via `agent/blink-anchor-mount-seq` (#209, merged to master), inherited automatically once `agent/rehearsal-bypass-suite` rebased: `demo_rehearsal.py` has no independent blink-measurement code, it fully delegates to `ui_live_smoke.py:run_tab()`, which now calls `blink_sweep_gate_verdict` itself. Pinned with a regression test using real measured numbers (`tests/test_demo_rehearsal_lib.py`, run.id 1790574871: scatter 0.1305, transitions 0.0072–0.0172) |
| §4 time to first paint, pinned viewport | **SETTLED** (review round 6): bound 3000 ms, global, **≈3.4x margin** (worst observed: transitions at 893 ms) — re-measured after PR #268 (#265) replaced `handle_top_queries`'s O(n^2) bubble sorts with `qsort`; supersedes the pre-#268 figure of queries at 2432 ms / ≈1.2x margin, which is now wrong. Measured ≥3 navigations/tab, real Chrome 154 on the Mac, live full-mode capture on the demo topology, satisfying this criterion's own requirement in full. The bound itself (3000 ms) is unchanged and was neither widened when the margin was thin nor tightened now that it is comfortable — same reasoning both times. `top_queries` latency is separately window-dependent (391 ms at the demo's planned 15-minute window, rising toward 1237 ms at full-capture range on a 2583 s capture) — see the window-dependence finding below this bullet; that is a server-side cost, not a UI paint regression, and is unrelated to the ≈3.4x margin above. Timeline not yet measured by this method (instrumentation gap: bare click, not Sessions-row drill-down — recorded as unmeasured, not a pass). `ttfp_ms` (merged `master`, `fa20067`, issue #245/PR #257) now gates every automated tab/navigation at the registered <= 3000 ms bound; absence refuses, and failure detail names the tab/value. This is the VM regression detector; the Mac-side measurement remains separate. Mac-side checklist: still manual sign-off, **deliberately stays qualitative** (paints without a visible spinner) — it has no stopwatch, so it does not independently check the 3000 ms number either; this round's measurement was a one-off script, not the checklist itself. Retracted: the 2026-09-29 contended walk's 30 s Timeline finding measured the "select a session" prompt from a bare tab click, not paint after a Sessions-row drill-down — the same trap this round's Timeline gap repeats at the instrumentation level, not the product level |
| §5 cross-tab agreement, freshness | partial — VM-side freshness: yes (`freshness_ok`, `info`'s `now_ns` vs `to_ns`); Mac-side visible freshness: manual checklist and human sign-off. Cross-tab agreement: per-class attribution yes (mandatory `cross_tab_class_attribution`, `class_rows_vs_top_events_ok`, same recent window); the redundant total-only `cross_tab_db_time_agreement` gate was retired under #258 (helper retained only as a diagnostic for regression proof); AAS leg yes (`bucket_weighted_aas_ok`, `agent/aas-agreement-and-retention`) — a bucket-weighted re-derivation from the `aas` endpoint's own `bucket_ns` over the identical window, compared against `time_model.aas`, wired into `extra_checks["cross_tab_aas_agreement"]` alongside per-class attribution. Retracted: the earlier walk's 0.0000% came from an unweighted mean of buckets, which was not that comparison and established no agreement; see `tests/demo_rehearsal_lib.bucket_weighted_aas_ok`'s own comment for why the two formulas diverge whenever the window does not divide the endpoint's `bucket_ns` evenly (the common case) |
| §6 lost-event counters, overhead envelope | lost-event counters: yes (`daemon_integrity_ok`: `ringbuf_drops_total`/`state_map_full_total`/`seen_query_ids_full_total`, already on the wire via pgwt-server's control proxy, no `src/` change needed; blind spot stated in the code comment: a lost LIFECYCLE event is silent, no counter increments). Overhead envelope: **measured 2026-10-13** — 30.31% ± 0.58pp (n=3, 4 clients, unthrottled, gate-1, inside the registered band), see §6's own paragraph above and `tests/results/overhead-envelope/ENVELOPE.md`. Every box-check also still runs `tests/test_overhead.sh --quick` (~10 minutes) via `tests/run_all.sh` as an ongoing regression check (default pgbench load, `--mode full`, appends to `tests/results/overhead_trend.csv` on the box, not synced back or committed) — that is a different, continuous measurement and does not substitute for this one-time demo-workload figure, which is now recorded separately |
| §7 no test exited 126/127 | **partial, and this is a human-readable aid, not an automated gate**: `tests/demo_rehearsal.sh`'s `report_early_exit` labels a dead subprocess's 126/127 exit code in the log for a human reading it afterward. It does NOT change the script's own exit code (every call site already `exit 1` regardless of the labeled reason) and has no test coverage of its own — reviewer finding, 2026-09-28. Do not read this row as "126/127 fails the gate automatically"; it already did, via the pre-existing `kill -0` + `exit 1` checks, which is why this addition changes nothing observable except the log's wording |
| §8 known-failing lines read by hand | manual by construction; not applicable to `demo_rehearsal.py` itself, which grants zero known-failing exemptions (§4 row above) -- nothing here for a human to hand-verify against an issue |

The `time_model_conservation()` vacuous pass on `db_time_ms <= 0` (a rehearsal
that captured nothing used to score clean) is now FIXED on
`agent/rehearsal-bypass-suite`, along with every other row marked "yes" above
-- each ships with a case proving it can go red
(`tests/test_demo_rehearsal_lib.py`).

## The ordinary release rule

**REPLACED 2026-10-25.** Everything that used to live in this section —
pre-attempt dry runs, two-of-N counted attempts, a 4-attempt cap, a separate
2-void cap and its infrastructure/product/harness attribution bookkeeping,
"capture-side N of M / Mac-side N of M" reporting, the merge-window freeze
between attempt 1 and the tag, the multi-step reordering of owner actions
around video/GIF analysis, and the pre-registered attempt-record format — is
retired. It was built to answer one question a live demo audience makes
urgent: "is this specific, immutable tree, walked twice, clean enough to show
a room full of people right now?" With no audience and no fixed date, that
question does not need that machinery; an ordinary release gate asks only
"is this tree good enough to tag", once.

**The arithmetic that justified dropping the counted-attempt apparatus,
rather than just not needing it:** it was never a good instrument for finding
intermittent defects. Two independent whole-run attempts catch a bug that
fails only 1 time in 4 with probability `1 - 0.75^2 = 0.4375` — about 44%, a
coin flip, for 30-45 minutes of box time each. A targeted, repeated run of
just the one suspect check (what issue #537's diagnosis is doing right now)
finds the same 1-in-4 intermittent far more cheaply by running many more
times at a fraction of the cost per run. Counted whole-run attempts were
solving the wrong problem even while the demo still existed.

**The rule, replacing all of it:** the tree that gets tagged is the tree
that has, on the same sha:

1. passed `make box-check` (the existing Linux build, C units,
   synthetic-data and real-PG tiers CLAUDE.md already requires for every PR);
2. one clean rehearsal — a single run of `tests/demo_rehearsal.py`'s
   capture-side checks (§1-§8 above), with no re-rolling past a failure: a
   criterion fails, it gets fixed, and the rule applies to the fixed tree
   from a fresh run, not a cherry-picked one from history;
3. one analysed recording — the walk from "The recorded walk" above, reviewed
   by the lead and both standing advisers.

**A merge to master after that re-runs all three once**, on the new sha,
before the tag moves. This is the same "a new sha is a new candidate"
principle the old sequence already had (§3's conservation section and the
old attempt-record rules both said so) — only the bookkeeping around it is
gone. There is no cap on how many times this can happen; each merge is
expected to be rare in the narrow window between "ready to tag" and the tag
itself, same as it would be for any other release process.

**What this does NOT weaken:** no gate inside §1-§8 is removed, no bound is
widened, no KNOWN_FAILING entry is added. The only things dropped are the
accounting *around* a gate result (how many times you were allowed to try,
whether a failure was "voided", which of two humans' machines it ran on) —
none of which is itself a check on the product.

## What changed and why (amendment, 2026-10-25)

Explicit amendment, not a reinterpretation, per this document's own
pre-existing rule: the owner cancelled the demo (PGConf Valencia — "the
moment for it has gone"), and both standing advisers independently concluded
the rehearsal apparatus built around that specific event should mostly drop
away, leaving the tag as an ordinary release gate.

| Dropped | Why |
|---|---|
| Two-of-N counted attempts, the 4-attempt cap, re-roll prohibition | Built to answer "is this one immutable tree clean enough to show an audience right now"; see "The ordinary release rule" for the 44% arithmetic showing it was never a good intermittency instrument anyway |
| Void cap and infrastructure/product/harness attribution bookkeeping | Administration around counted attempts; no counted attempts, no administration |
| Dry-run declarations, attempt records | Pre-registration scaffolding for a sequence that no longer exists |
| The merge-window freeze between attempt 1 and the tag | Existed so two counted attempts stayed attributable to one sha; "The ordinary release rule" gets the same guarantee (same sha for box-check + rehearsal + recording) without freezing merges |
| "Two verdicts, never merged" (capture-side vs. Mac-side), the owner's two signed Mac walks, the owner-actions-after-video-analysis reordering | Stage insurance for a human presenting live; replaced by one recorded walk analysed by the lead and both advisers (see "The recorded walk") |
| Mac pre-flight hygiene (auto-lock, zoom, second display, Time Machine, quitting apps) | Presenter-machine care for a presentation that is not happening |
| `pgwt-stage` | Never provisioned; its only purpose was the live demo event itself — see "`pgwt-stage` is retired" above |

**Kept, unchanged in substance:** `tests/demo_rehearsal.py`'s capture-side
checks (§1-§8); the 1710x981 viewport pin (restated, not retracted — the
#415/#462/#473 fold checks need it); the overhead-envelope and per-class
wait-CPU measurements already recorded in §3/§6 (these were already DONE on
master before this amendment and are not reopened by it — see the note on
`docs/DEMO_DELIVERY_QUEUE.md` for a correction to a stale claim about their
status); and the fact that a recording proves layout/motion, never numeric
correctness.

## Provenance

Criteria drafted by the lead, challenged and corrected by the standing adviser
(`.claude/agents/adviser.md`) on 2026-09-27 — which supplied criterion 3's
residual-absorption finding, the over-attribution detectors, the sampling
requirement, the cross-tab and freshness checks, and the attempt accounting
(the last of these retired 2026-10-25, see "What changed and why" above).
The owner's rulings are quoted inline where they bind. The 2026-10-25
amendment was drafted against the demo's cancellation; both advisers agreed
on dropping the rehearsal apparatus, and the lead corrected their shared
premise that the demo-workload overhead A/B was still outstanding — it was
already measured 2026-10-13 (§6) — before writing that table.
