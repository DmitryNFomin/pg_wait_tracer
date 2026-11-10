# #424 and friends — handover for the owner's final check

Written as work completed, not reconstructed afterwards. Every number here has a named
source file. Anything unverified is marked **UNVERIFIED** rather than omitted.

## What you need to do

**Two signed Mac walks**, one paired with each of counted attempts 3 and 4, against the same
candidate sha and the same clean tree hash. Your criteria are explicit that the Mac-side count
means signed-off manual walks, never automated verdicts (#250), so this cannot be done for you.
Mac-side reads **0 of 2** until you walk them.

Everything else below was taken to completion or is stated as open.

## The defect, in one sentence

A raw-route answer must include every interval that **overlaps** the window, clipped, with
per-occurrence statistics unaffected — for recorded intervals and for intervals still open at the
right edge — and must **say** when it could not.

That sentence survived three independent adviser reviews unchanged. What kept moving was the list
of things required to satisfy it.

## Issues filed tonight (none existed this morning)

| # | What |
|---|---|
| #424 | Right-edge admission loss: an interval open at the window end is never admitted |
| #425 | Same mechanism, idle time: `Idle` 1,113,949 ms / `ClientRead` 978,699 ms dropped at one 900 s point |
| #426 | AAS series ranking sums **unclipped** duration, so a straddler can win a top-N slot it did not earn |
| #427 | Window boundary semantics: raw counts a boundary-closing record in BOTH adjacent windows; raw and summary already disagree at boundaries. **Separate 40–64 h migration, deliberately deferred** |
| #428 | `cpu_open_uncredited_ns_total` documented as control-socket exported but no exporter existed; it also counts gap wall time, not missing measured CPU |
| #429 | `pgwt_reader_find_block` binary-searches a **commit-ordered** index, so the lookup is arbitrary — and it silently bypasses #424's truncation flag |
| #430 | Hourly rotation leaves `w->fp` NULL for lazy reopen, so open-interval crediting pauses until the next pushed record |
| #431 | `check-test-registration.sh` uses bash-only process substitution; under Linux `dash` it cannot run its own comparison (see the correction comment — part of the original report did not reproduce) |

## Proofs that need no reference path

The strongest evidence, because it does not depend on anyone's reconstruction being right:

- **Additivity violation.** Splitting a window and summing the parts must equal the whole, because
  clipping is additive. Raw did not: at 0/1/3/7 internal boundaries the `Idle` deficit was
  0 / −227,136.4 / −381,493.9 / −864,086.5 ms, monotone. The summary path was exactly additive.
- **The half-fix is mechanically caught.** An admission-only prototype produces 3 count failures,
  so the dangerous shortcut cannot land unnoticed.
- **The gate was shown RED before GREEN** in every component, with single-change reverts that each
  fail exactly one check.

## Corrections I had to make to my own statements

Recorded because they bear on how much to trust the rest.

| Claim | Correction |
|---|---|
| "15-minute Overview correct to 0.022%" | Withdrawn — the "ground truth" was itself a finite-lookahead reconstruction that can miss mass |
| "227 backends" | Never substantiated; the digits came from inside a float |
| "Summary path exactly additive on every row" | True only at 1 and 2 parts; the 4- and 8-part rows were raw-answered |
| "Durable replay is a 4–5 day format migration" | Needed **no format bump** — 15 spare header bits and a sidecar |
| "Durability is done, 15/15 crash scenarios" | D1 only. D2 had **no callers**; the scenarios asserted labels, not checkpoint survival |
| "F1 is #427's shared-edge problem" | Wrong — `T` was strictly interior; all three failures were one missing lower bound |
| "`pgwt_compute_transitions` needs caller-side filtering" | Wrong — the filter is inside it, via a flag stamped at the loader |
| "`AAS_SEMANTICS_DECISION.md:439` is stale" | Wrong — it correctly describes v6; the defect was the list stopping at v6 |
| "#429 affects any `--mode full` capture" | Narrower: needs two streams in one file plus a drain lag; never observed on a live capture |

## Gates that were passing while blind

Found tonight, all self-reported by the agents that owned them:

- the summary-raw parity gate placed every window edge in a **data hole** by design
- crash scenarios asserted closure **labels**, not checkpoint survival — 15/15 green with two
  handover windows broken
- a dual-path probe reported "both branches compile" while holding a deliberately broken call
- a mutation suite scored **9/9 green against a stale binary** (missing header prerequisites)
- `.gitignore` drift left **33 of 64** build products untracked-and-unignored, moving the tree hash

The last one is what disqualified counted attempt 2.

## Still open, deliberately

- **#427** — boundary semantics, 40–64 h, separate migration
- **#426**, **#428**, **#430**, **#431** — filed, not fixed
- **D2 checkpoints** — library with an end-to-end gate, **no producer**: no daemon writes one, no
  server reads one. Not a user-visible deliverable. Wiring checklist is in `src/open_ckpt.h`
- **Summary open-CPU on the demo's 900 s view** — v9 accounting landed; producer wiring is behind
  `#if PGWT_SUMMARY_VERSION >= 9` until v9 folds in

## Verification status

**UNVERIFIED until the final box run:** everything in this document that is not marked with a
Linux log. Most green numbers tonight were Mac-local. The pieces with real Linux evidence so far
are the durability suites (85/91 and 134/188 assertions on Ubuntu 24.04/gcc 13, `test_event_writer`
5062/5062, 24/24 real-daemon crash scenarios on PG 17) and the first integration run
(126 of 129, three failures now fixed).

## Decision: the live overlay is NOT wired tonight (unless it can be demonstrated complete)

**What is fixed:** committed closing records repair historical overlap within the scan limits.
Every window that can be answered from records on disk now counts a wait for the time it
overlaps the window, with per-occurrence statistics unaffected, and says when the scan was
truncated.

**What is NOT fixed:** a wait **still open right now** remains absent from raw time views —
including short-window Sessions and Top Events. The demo's 15-minute Overview is unaffected
because it takes the summary path, where v8 credits open waits per sealed second.

**Why not wired:** the overlay module exists, is unit-tested (312 checks, 21/22 mutations
caught) and is reviewed — but wiring it requires more than an attach call. An adviser's
assessment, after retracting its own earlier guarantee:

> *"Separating spans prevents occurrence inflation; it does not prevent omitted attachment or
> consumption."*

Doing it correctly needs an **opaque window object constructed only by the loader**, owning
records, overlay and status, which the time reducers are *required* to consume — plus removal of
the records-only reducer signatures so a forgotten handler cannot compile. Attachment alone
would leave the six handlers that need live time still reporting records-only totals: a
partially repaired interface, where some views are fixed and others silently short. That is
worse than a stated gap.

**Corrections absorbed along the way**, all to instructions I had given:
- not 25 call sites but 17 loader calls, 2 marker loads and 2 wrappers; only **six** need live
  interval support (`handle_aas`, `handle_time_model`, `handle_top_events`, raw
  `handle_top_sessions`, raw `handle_top_queries`, `handle_concurrency`'s occupancy)
- **summary requests do reach the raw loader** — Sessions' `top_wait` backfill and Queries'
  lifecycle backfill — so "route refusal is free and implicit" was wrong
- `wall_of = NULL` **and `offset_ns = 0`**: by the loader tail the merge has already applied each
  segment's offset, and applying a canonical offset again would double-convert
- MIXED/NONE fidelity needs explicit handling; an empty array does not establish the daemon's tier

**If it is wired before the merge**, the bar is: demonstrably complete across all six handlers,
with the omission made impossible by construction rather than by diligence, and gated — including
the property that a historical window yields an empty overlay. Anything short of that ships
without it.

## First green Linux gate (partial scope — read the scope line)

`make box-check EPHEMERAL=1` **exit 0**, `Live UI smoke: overall=PASS`,
**128 passed / 128 executed** (4 skipped opt-in, 0 known-failing, 0 xpass, total 132), run with
`--require-live` so it includes the live-PG capture tier, protocol drift and the live-UI walk.
Log attribution verified: `commit=331f350` and `tree-hash=d5ffedd…` match HEAD at run time and
the `make check` stamp.

**Scope:** this is `agent/424-durability` rebased onto `agent/424-daemon`. It is **not** the full
eight-branch integration tree, whose gate was still running when this was written. Do not read
this as the release gate.

What it does establish:
- **B's link fix works on Linux.** `test_event_reader` (69/69) and `test_concurrent_rw` (9/9),
  previously unbuildable, now build and pass. Moving `pgwt_collect_open_intervals` into its own
  TU was the right call over growing a link line.
- Durability suites on Linux: 91/91 and 196/196; `test_event_writer` 5062/5062,
  `test_durability` 97/97, `test_trace_v2` 98/98, `test_open_interval` 180/180.
- 24/24 real-daemon crash scenarios on PG 17, including the three checkpoint ones.
- Mutants re-run on the rebased tree: 9 + 15 red, **0 undetected, 0 stale-anchor**.
- RED retained and still red: pre-fix recovery ordering (3 failed, `ckpt_window=none`) and the
  pre-#424 writer (7 failed).

Ephemeral VM `170617788` deleted by id, confirmed.

### Discipline worth noting in this round

- It re-ran its suites green on the rebased tree **before** spending a box run.
- It verified `tests/Makefile` kept all four of its targets' recipes after the merge, explicitly
  because that is the silent failure the registration guard exists to catch.
- It **deleted superseded pre-rebase logs** rather than keeping them beside the new ones, on the
  grounds that two logs for one claim at different commits is how a reviewer reads the stale one.
- The crash hook's inertness is now proven by a positive control: seven inert values each must
  complete a real rotation, while the exact name in the same table must cause death — without
  that control, "inert" would be satisfied by a hook that never fires at all.
- It flagged that `box-check` now runs the new `check-link-closure` gate, which has nothing to
  say about a headers-and-tests change, "so nobody reads its silence as coverage".

## Overlay wiring: attempted, reported NOT COMPLETE, branch parked

An agent attempted the full wiring against a pre-stated bar (all six handlers consuming an opaque
window, omission prevented structurally, all six gates RED-then-GREEN). It reported **NOT
COMPLETE** and stopped at **zero** views repaired rather than three.

Its reasoning, which I accept: all six handlers do consume the window and omission *is* prevented
structurally (a signature gate, shown RED on the parent tree), but **no reducer adds overlay span
time**, so the live right edge is still missing. Three of six reducers consuming spans would have
been exactly the partially-repaired interface — some views fixed, others silently short — that
was forbidden. Every numeric answer on that branch is bit-identical to its parent, uniformly.

**Decision: the branch is parked, not merged.** It is a ~150-call-site refactor that changes no
number; `make check` cannot gate it because it builds no C; three of its migrated test TUs cannot
even run on this Mac; and whoever finishes #424 should write the consumption alongside the seam,
not inherit it already merged. Branch `agent/overlay-wiring`, commit `4e196d5`, parent `53218a1`.

Also rejected: adding an `open_intervals` key to raw-route responses that is always
`unavailable`/`refused`. An honest label for a capability that does not exist is still a
permanent protocol addition the UI ignores.

**So the live right edge is NOT fixed.** A wait still in progress remains absent from raw time
views, including short-window Sessions and Top Events. The demo's 15-minute Overview is
unaffected — it takes the summary path, where v8 credits open waits per sealed second.

### Two findings kept from that work

- **#432** — `pgwt_interval_index_query()` builds its own record array and calls
  `pgwt_compute_concurrency()` directly, bypassing the overlay guarantee. Latent (no `src/`
  caller today) but silent if ever wired.
- **`make check` cannot gate C changes at all** — it builds no C. The agent's own measured
  finding: four block-comment `#include` injections in migrated test TUs would have survived any
  number of Mac greens, because nothing on the Mac compiles them. Separately, and on my evidence
  rather than its own, the first integration box run found **three failures in
  `test_window_clip.c`** that the component agents' Mac-local greens had not surfaced (A's gate
  was 47/47 then 53/53 then 59/59 on the Mac; D's was green on the Mac). The agent declined to
  co-sign that second claim because it did not observe that run — correctly. Both observations
  point the same way: this is a structural gap in the project's fast loop, worth closing
  separately.

### Decisions ratified from that agent

- Sessions' summary `top_wait` backfill stays on the summary route — v8 already credits open
  waits per sealed second, and `top_wait` is an argmax over closed per-event totals by definition.
- `dump_summary` is captured-records-only and **prints** `Open intervals: refused — …` rather
  than silently assuming the exclusion.

## Issues filed tonight, final count

**#424–#432.** Seven of the nine were found while fixing the first.

## A green Linux run at 53218a1 — SUPERSEDED, see the correction at the end of this section

The full eight-branch integration tree passed `box-check` on gate-2:

```
box-check: commit=53218a15b0e421be407c6fa94196ac5ae985d4b1 tree-hash=cd32f50718a4f0571ac823310a76f23bf5164313
Live UI smoke: overall=PASS
Executed: 134 (passed 134, failed 0, known-failing 0 (0 xpass)), skipped 4, excluded 0, total 138
```

Log: `tests/results/box-check-ubuntu-20261015-215456.log` (in the integration worktree). Run with
`--require-live`, so it includes the live-PostgreSQL capture tier, protocol drift and the
live-UI walk. The header's commit and tree-hash match the tree being claimed, so the evidence
cannot be from a stale checkout.

Up from 129 executed in the first (red) run to 134 — the extra entries are the newly registered
tests.

### The 36 `FAIL` lines in that log are benign — verified, not assumed

A run reporting `failed=0` with 36 printed `FAIL` lines deserves explanation. They sit inside
Python unit tests named `test_workload_signature_empty_rows_fails`,
`test_workload_signature_missing_one_fails` and `test_workload_signature_zero_time_fails`, fed
synthetic `ticks=2` / `target 0s` fixtures. The harness is correctly reporting a failing tab for
a fixture built to fail; `run_all`'s `failed=0` counts test-level results, not printed lines. I
checked the enclosing test names rather than taking the summary line, because "FAIL lines in a
passing run" is exactly the shape a swallowed failure would have.

### What the first (red) run bought

Three defects that eight Mac-local greens had not surfaced:
- the missing lower bound on INSTANT selection (3 failures in `test_window_clip`)
- `test_interval_index` 2/122 — the index still selected by close while the raw path had moved
  to overlap
- `test_data_current_trace_cache` 3/112 — the straddler lookahead consumed the gap the fixture
  needed, and the fixture **refused rather than passing vacuously**

All three are fixed and the second run is clean.

## Remaining before merge

1. A fresh `make check` stamp at `53218a1` (the push guard requires it; the last recorded stamp
   was at an earlier tip).
2. The `check-link-closure` finding: `map_reader.c gained _pgwt_summary_note_open_ex`. It went
   red on a *derived* branch's `make check` while `box-check` at `53218a1` passed — so it is
   either a true positive exposed by removing the v9 `#if` guards, or an artifact of the guard's
   fixed `304a772` base. With the agent that owns both the gate and the v9 producer commit.
3. A fresh reviewer's verdict (dispatched; first review of any of this work).

### CORRECTION: that run is NOT the release gate

A fresh reviewer reviewed at real HEAD **`e836027`**, one commit beyond the `53218a1` I was
citing, and the difference changes the verdict:

- the `box-check-ubuntu-20261015-215456.log` green above is **stale** against current HEAD, and
  the delta edits `tests/run_all.sh` — which CLAUDE.md names as **never exempt** from a re-run
- there is **no `.pgwt-check.stamp`** on the tree; the reviewer's own `make check` at `e836027`
  exited **2**
- **two blockers**, both outstanding

I recorded a milestone before it was one. The green at `53218a1` was real for that tree; it is
not evidence for the tree we would merge.

### Blocker 1 — the link-closure gate fails on the tree

`map_reader.c gained link-time dependencies: _pgwt_summary_note_open_ex` (`src/map_reader.c:802`).
The branch promoted that guard into `make check` and its own v9 producer violates it;
`tests/test_check_link_closure.sh`'s "the real invocation passes" case also fails. It is a **true
positive**, not an artifact of the guard's fixed base — which answers the open question from
earlier. Same class as the one already fixed once by moving code into `src/open_snapshot.c`.

### Blocker 2 — v9 open-CPU is claimed but NOT wired

This is the failure mode I had explicitly flagged as "the one that still looks green". Measured
on gate-2 with watchpoints attached and `cpu_ns_total` moving 0 → 23.7 s, from the branch's own
commit message:

    summary_open_cpu_credited_ns          = 0
    summary_open_cpu_refused_total        = 0
    summary_cpu_open_uncredited_ns_total  = 0

All three zero means **the writer is never asked** — `src/summary_writer.c:1608` would increment
on a decline — so accounting silently falls back to v8. And `test_v9_open_cpu_wired` is registered
in `run_all.sh` LIVE_TESTS and is **RED** with `KNOWN_FAILING` empty, so box-check at HEAD
**cannot pass**. Resolution is either wire it genuinely or drop the v9 claim and the gate
together; a registered RED live test does not ship.

### Should-fix that defeats the feature on tiered captures

`src/server.c:1743`/`:1774` — `only_straddlers` carries **no block-type test**, so SAMPLES blocks
in the straddler horizon are charged against the 256-block budget even though the record loop
discards them. On a tiered capture the budget is spent on blocks that cannot hold a straddler,
`right_edge_truncated` fires **falsely**, and the TRANSITIONS blocks that do hold straddlers are
skipped. Fails loud, so not a blocker — but it loses the straddlers #424 exists to recover.

### Docs trap with operational teeth

`DEMO_RUNBOOK.md:85`/`:103` and `AAS_SEMANTICS_DECISION.md:500` say **v8**; the code is **v9**,
and those v8 lines were added by this same branch. An operator following the runbook moves aside
only pre-v8 directories, leaves a v8 one, and the v9 server refuses it on the summary fast path —
the Overview silently drops to raw, which is the paint latency the runbook exists to prevent.

### What the review found CLEAN (stated, not omitted)

- **INSTANT/INTERVAL split complete at every consumer** — guards at `compute.c:3488, 3606, 4177,
  4301, 4585, 4887, 5400` plus both-bound selection at `:1728, 2034, 2063`; heatmap safe via its
  own filter, query lifecycle walks duration-0 markers only, `avg_us` divides two INSTANT
  quantities
- **#429 helper collapse is one body**, `-1` treated as "unknown, decode anyway" at both callers,
  both right edges handled
- **Version bumps complete**; no `#if … >= 9` guards remain in `src/`
- **All seven v9 control keys** published unconditionally and asserted by name
- **No `unit_tests.list` drift**; `.gitignore` suffix rule shadows no tracked file
- **No sixth blind gate found** — it checked the four mutation/additivity harnesses specifically
- **No phantom issue filings** — #424–#432 all exist and are OPEN


### FURTHER CORRECTION: the green run never executed the v9 gate

Worse than "stale". The integration agent found that `test_v9_open_cpu_wired` had been registered
in `tests/ci_smoke.sh`, which **`run_all.sh` never invokes** — so the 134/134 run contains
**zero** executions of it (no `ci_smoke`/`v9` string anywhere in that log). The run I called the
release gate did not test the thing most at risk.

### The integration agent's own gate was broken three ways, each found by running it

Worth recording, because it is the clearest example of the night's recurring lesson:

1. registered in a script the master runner never calls → 0 executions
2. its precondition was `tier == "exact"`, but `--mode full` on gate-2 reports `escalated` with
   `mask=3` → would never have run even if invoked
3. unbounded `psql` plus a bare `wait` → killed at 400 s with no verdict
4. the 5 M-row stimulus (~0.75 s) **could not span the tick boundaries v9 credits**, so it would
   have **reported a product bug that was really a weak fixture**. Now 60 M rows with elapsed
   >= 3 s asserted.

Point 4 is the one to remember: a gate can fail for the wrong reason as easily as it can pass for
the wrong reason.

### v9 reachability, stated precisely

Measured on gate-2 with watchpoints `mask=3`, 3 of 3 CPU statements across 10 s of ticks, and
`cpu_ns_total` moving 0 → 23,715,122,379:

    summary_open_cpu_credited_ns   = 0
    summary_open_cpu_refused_total = 0
    summary_cpu_open_uncredited_ns = 0

All three at zero means the writer is **never asked** — a writer asked and declining would show
`refused_total > 0` (`src/summary_writer.c:1608`). So this is a reachability defect, not an
accounting one. Note also: removing the `#if PGWT_SUMMARY_VERSION >= 9` guards is **not** the
cause — the producer credited nothing with them removed, so restoring them would only re-hide it.

**Decision point set:** one focused reachability diagnostic, then either it is wired with a
non-zero counter, or the producer wiring is reverted, `test_v9_open_cpu_wired` is removed from
LIVE_TESTS, v9's writer capability is left dormant and unclaimed, and the gap is stated. A
registered RED live test does not ship.

### Blocker 1, clarified

`map_reader.c` gained `_pgwt_summary_note_open_ex` in its undefined set **because of the `#if`
removal**. On Linux everything still links (`test_event_reader` already pulls `summary_writer.o`),
which is why the 134/134 was genuine — but the symbol-set delta is real and the guard has no
allowlist by design. It may dissolve if the v9 producer is reverted.

### Another retraction, this one from an agent about another agent

Commit `bd241e6` claimed `agent/424-v9` had left `test_open_cpu_accounting` out of `$(TESTS)`.
**False** — it is at `tests/Makefile:39`; the claim came from printing only lines 44-52.
Retraction recorded in `d5bb48a`, and the duplicate entry that had been added was dropped during
conflict resolution, so the tree is correct.

### What the integration agent did land

Eight branches folded (62 commits off `304a772`), #429 helper collapse done with both right edges
preserved (`scan_to` as the hard horizon passed to the helper, `only_straddlers` derived at the
call site from the returned header), interval-index clip dropped, v9 `#if` guards removed with all
seven control keys asserted by name, docs corrected to v9 **plus a new gate so the next version
bump cannot drift** (`tests/test_docs_summary_version.sh`, 11 checks, shown RED against the old
docs), and the `.gitignore` guard extended after the durability merge exposed two more products
(`tests/open_ckpt_stub`, `tests/trace_closure_probe`).

Registration audit: 70 `$(TESTS)` entries, 0 duplicates, 0 without a rule, 81 list entries all
runnable.

## Both blockers resolved — v9's producer reverted, deliberately

**The root cause, found and recorded** (`dc23287`): *"a note can be accepted and sealed by
nobody."* The open-interval note **is** accepted by the writer, but nothing ever seals it — which
is precisely the fourth outcome nobody designed for: neither credited, nor refused, nor counted
as uncredited. That is why all three counters read zero and why "the writer is never asked" was
the right diagnosis from the counters alone.

**Resolution taken** (`fc7d7f5`): the v9 open-CPU **producer is reverted**, with the commit
message stating it plainly — *"the counter says it was never wired, and I cannot prove otherwise
tonight."* I set that decision point explicitly after two failed attempts, and the agent took it
rather than stretching a third attempt into a claim.

**It dissolved blocker 1 for free.** Verified by me, not assumed: `src/map_reader.c` no longer
references `pgwt_summary_note_open_ex`, so the link-closure violation is gone without any TU
surgery, and `test_v9_open_cpu_wired` is deregistered from `run_all.sh` so no RED live test
ships.

### v9's honest status

The **writer** capability exists and is unit-tested (89 checks, 10 RED reverts each failing). **No
producer asks it.** So summary open-CPU accounting is, in practice, unchanged from v8: an
in-flight pure-CPU statement is still absent from the CPU rows of a summary-path view until it
ends. That is a dormant capability, labelled — not a deliverable.

Open question passed to integration: whether `PGWT_SUMMARY_VERSION = 9` remains coherent after
the revert. Bumping a format version for a capability nobody invokes would be its own kind of
dishonesty, so if the bump is now pointless it should be said.

## Also fixed

`7f066fb` — the straddler budget is now spent only on TRANSITIONS blocks (SAMPLES blocks in the
horizon no longer consume it, so `right_edge_truncated` cannot fire falsely and the TRANSITIONS
blocks that hold straddlers are no longer skipped), and the `straddlers_admitted` /
`headers_unreadable` diagnostics are now actually readable rather than accumulated and dropped.

## The sixth blind gate — found after the reviewer looked for one and did not find it

The reviewer specifically hunted a sixth gate-that-looks-at-nothing and reported finding none.
One turned up anyway, **self-reported by the agent that wrote it**:

A's first version of `test_window_overlap` §15 (the tiered-budget section) passed **63/63 against
the unfixed tree**. `run_bypass()` derives the window origin as `earliest_wall_ns - 10 ms`, and
the tiered fixture started at `base + 10 s` — so every window shifted ~10 s and landed **on** the
SAMPLES blocks, where they sit inside the sample edge, are never charged against the budget, and
the defect is **unreachable**. One anchor record at `base + 10 ms` fixed the geometry, and only
then did the section go red.

**The transferable lesson:** the origin convention is implicit in `run_bypass()`, so any fixture
added to that file can silently miss its own target. That is now flagged for a comment at the top
of the file.

Running count for the night: **six** gates found to be measuring nothing or the wrong thing, all
six disclosed by the agent that owned them rather than caught by review.

## Tiered-budget defect fixed (`7f066fb`, branch now `79b9a55`)

63/63 green, 3 failed at `868e1db`. Straddler behind 3 SAMPLES blocks: admitted **0 → 1**. False
truncation: **1 → 0**. Recovered time: **0.0 ms → 2000.0 ms** of Lock. `make check` passed, stamp
`e80945cd…`.

Block type is now resolved **before** the budget is touched, on both the cached (`cb->is_sample`)
and uncached (`bi.block_type`) paths. An unknown type — header unreadable — is deliberately **not**
treated as SAMPLES; it still falls through to the decode like every other unknown on that path.

Three smaller items closed with it: the stale `:375` comment now names which two diagnostics reach
the wire and why the other two do not; `straddlers_admitted` / `headers_unreadable` are readable
at runtime under `PGWT_STRADDLE_STATS=1` (stderr, silent by default, mirroring #283's cur-cache
line) and deliberately kept **off** the wire because `straddlers_admitted` is non-zero on most
ordinary windows; and `count == 0 with total_ms > 0` was verified to render safely — every
division by count in `src/output.c` is already guarded by a `count ?` ternary at `:521, :582,
:683, :992, :1118`.

**Integration hazard, flagged by A itself:** the budget fix must be **re-applied** onto the
collapsed candidacy helper rather than merged as-is, because integration already folded A's
`server_block_candidate()` onto E's `pgwt_reader_block_candidate()` while A's branch still carries
its own shape. The semantic change is identical; the decision points differ.

## The same blind spot, mirrored — caught on re-check

The v9 gate was deregistered from `tests/run_all.sh`, but **left invoked at
`tests/ci_smoke.sh:214`** — and `ci_smoke.sh` is executed by `.github/workflows/ci.yml:804` and
`nightly.yml:243` on the self-hosted gate boxes, which have watchpoints. `run_section` sets
`overall=1` on any non-zero exit and the script ends `exit $overall`, so **both** outcomes of the
seal discriminator turn the CI job red.

**Why the 134/134 could not see it.** Earlier the gate lived in `ci_smoke.sh` and not
`run_all.sh`, so box-check was green while the gate never executed. Then the `run_all.sh`
registration was removed and the `ci_smoke.sh` one was left behind — so box-check is green
**precisely because `run_all.sh` never invokes `ci_smoke.sh`**. The same hole, in the opposite
direction, inside one night.

Reviewer's caveat, kept: it reasoned this from the code path and could not execute `ci_smoke.sh`
on a Mac, so **which** of fail-or-refuse occurs is unmeasured. It does not matter — `run_section`
treats both identically.

## The version bump was unearned, and the branch's own commit says so

`c16de0a` states plainly that today's build **"writes a v8 file wearing a 9"**: v9's only
semantic difference from v8 (the widened `cpu_measured_records`) never materialises without a
producer, and `pgwt_summaries_window_current` compares only the number — so 9 would silently name
two different accounting rules. It names the two sound resolutions (the re-wiring commit bumps to
10, **or** the constant sits at 8 until then) and **takes neither**, shipping 9 behind a
documentation paragraph.

That is the one state the analysis itself rules out, and it violates the project's own #315 rule,
quoted in both CLAUDE.md and `src/interval_index.h`: **never ship a version whose contract is not
yet true.** Merging 9 would also permanently foreclose the cheaper option, leaving only a
bump-to-10 path dependent on a future author reading a doc note.

**Decision: `PGWT_SUMMARY_VERSION` goes back to 8.** The producer was reverted; the constant
follows it. This costs a fresh `box-check` because it touches `src/` — ~40 minutes, which I am
spending rather than starting the last two counted rehearsal attempts against a tree carrying an
unearned version constant.

Also found: `docs/DEMO_RUNBOOK.md:91` advertises "v9's open on-CPU credit" to the operator as a
shipped capability this build does not have. The new docs gate validates the version *number*
against the constant, not the semantic claims around it, so it survived the earlier docs fix.

## Confirmed closed on re-check

Blocker 1 (link closure): the guard was run directly — `RC=0` with the anchor present, so it is
not passing vacuously — both steps wired at `scripts/check.sh:102` and `:105`, and commit
`0c21786` **strengthened** the guard rather than relaxing it into passing. The reviewer checked
for that specifically, since relaxing it would have been the cheap way to close the blocker.

Should-fix 3: properly fixed at a **single** site (`src/server.c:1843`) rather than duplicated
across the cached and uncached paths, which was the structural half of the original objection.

## Pre-merge changes landed, and the box went RED on the candidate

`2f6684b` — `PGWT_SUMMARY_VERSION` back to **8**, and the last invocation of the v9 check
removed. Verified by me: the constant reads 8 at `src/summary_writer.h:512`, and
`test_v9_open_cpu_wired` now appears only as a **comment** in `tests/run_all.sh:422` and
`tests/ci_smoke.sh:208` — neither invokes it.

**But the box run at that tree is RED, one failure**
(`box-check-ubuntu-20261016-002847.log:7826`):

    FAIL: system_event: PgSleep total = 2ms (> 2000ms)
    readiness=Timeout:PgSleep observed (pg_stat_activity shows the sleeper in Timeout|PgSleep)
    sleeper: started=yes, backend_seen=yes, exit_before_cleanup=running, final_exit=-15
    tracer: exit=0, timed_out=False

The sleeper **was** confirmed in `Timeout:PgSleep`, the backend was seen, the tracer exited
cleanly — and the measured total is **2 ms against a >2000 ms expectation**. A whole wait class
very nearly vanishing from the window.

**CORRECTION to the 0/51 figure I had been repeating.** I recorded "did not reproduce in 51
isolated runs on this same box" as evidence. It is not evidence. `tests/results/issue416-scope/
probe416.py:243-247` makes **one** tracer invocation, `--view time_model` — i.e. it replays
test_percentage **Test 1's first capture only**. The assertion that failed here is the **second**
capture, `--view system_event`. **The probe never ran the failing path**, and Test 1 itself
*passed* in the red run (Timeout 49.9%, DB Time 17264 ms). So 0/51 is **zero attempts**, not 51
negatives.

**Two details in the stanza cut the other way** and must be distinguished before this is called a
product defect: `final_exit=-15` on both the sleeper and the CPU process (SIGTERM — harness
teardown), and the tracer's own stderr saying *"controlled backend retired before capture"*. If
the backend retired before the capture window, 2 ms could be a **fixture/teardown race** rather
than lost data. That is a materially less alarming finding and it has not been ruled out.

**Instructions given:** preserve the evidence outside any worktree first; then establish which
path lost it by comparing the TUI, raw `--replay` and the web API over the **same absolute
window** (earlier scope work found the TUI was the only correct path); and explicitly distinguish
product-loss from teardown-race. **Do not re-run to green** — a second run settles nothing — and
**do not add it to `KNOWN_FAILING`**, which is reserved for a test reproducing a filed product bug
actively being fixed in `src/`.

**Merge is blocked on this.** A red box on the release candidate with an honest diagnosis is worth
more than a green one nobody can explain.

### DIAGNOSED: product data loss in the live display path, not a teardown race

**The race hypothesis is refuted three independent ways:**
1. `final_exit=-15` is the harness — `tracer2.communicate()` returns at
   `tests/test_percentage.py:376` **before** `finish_system_event_client()` sends SIGTERM
   (`:130-137`). `exit_before_cleanup=running` on both helpers means both were alive throughout.
2. *"controlled backend retired before capture"* is a PgBackendStatus **layout-validation** string
   (`src/backend_status_layout.c:2428`) printed at **startup** — present verbatim in the
   **passing** `763c4e3` run and in a dozen healthy daemon logs.
3. The window's wall is **identical** in PASS and FAIL: 87 CheckpointWriteDelay waits,
   8606.5 ms vs 8605.7 ms. The window opened and ran its full 8.6 s.

**Mechanism** (inferred from code and numbers, not reproduced): `handle_timer` builds the display
as a copy of the cumulative accumulator **plus** a live `pgwt_read_state_map()` for open
stretches, and **does not drain the event ringbuf** (`src/daemon.c:277-290`). The single-threaded
epoll loop drains afterwards at **64 callbacks per iteration** (`src/daemon.c:32`) against a
producer rate of **~37,700 records/s**. So records still queued when the timer fires are absent
from that tick, while `state_map` — read live — shows the current interval.

That predicts the observed table exactly: the sleeper's `pg_sleep(10)` closed ~1.828 ms before the
tick's state read, its 8.6 s **closing record missed the tick**, and the freshly-opened
`pg_sleep(60)` appears as a 1.828 ms open stretch.

**Why one class vanishes while the rest look fine** — the asymmetry that is #416's signature:
missing the last L ms costs CPU\* and BufFileWrite ~L ms out of 8600 (CPU\* −0.6%, BufFileWrite
+1.5%, both inside run-to-run noise) but costs PgSleep **100%**, because a long wait's entire
contribution rides on a single record. **DB Time came out 50.1% short** (8617 ms vs 17267 ms).

**Not a regression of #424**: exactly one non-comment `src` line differs between the passing
`763c4e3` and the failing `2f6684b` — `PGWT_SUMMARY_VERSION 9→8` — and it is inert on this path.
Pre-existing intermittent, **1 failure in 5 complete runs** on this box.

**The assertion is a floor, not a conservation check.** `PgSleep > 2000 ms` passes on any value
above 2 s, so the same loss truncated to 3 s passes **silently**, and it reads only the last tick.
This defect has very likely been occurring and passing for a long time.

**The decisive check, now running:** one instrumented `system_event` capture with
`-T -v PGWT_DEBUG_DUMP_STATE=1`. Does the on-disk trace contain the 8.6 s `Timeout:PgSleep` record
the TUI did not show?
- **trace has it** ⇒ tick-boundary drain loss, confined to the live TUI. The web UI reads through
  `pgwt-server` from the trace, so **the demo is unaffected**.
- **trace missing it** ⇒ BPF-side loss reaching the trace and therefore the web UI, and **the demo
  is at risk**.

Caveat recorded by the diagnosing agent: `-T` itself perturbs the drain — the implicated variable
— so a non-reproduction with `-T` present is not a negative result.


## #416 DIAGNOSED AND FIXED — the demo was never at risk

### The decisive measurement

Same traces, live TUI versus both offline readers:

| attempt | live TUI | `--replay` | `pgwt-server` (web UI path) |
|---|---|---|---|
| 01 | **1.2 ms**, count 1 | **12,321.0 ms**, max 8,605,746.7 us, count 2 | 12,321.0 ms |
| 06 | **0.7 ms**, count 1 | **12,325.9 ms**, max 8,606,723.7 us, count 2 | 12,325.9 ms |

The ~8.6 s record was **on disk** and both offline readers returned it whole. **The web UI reads
through `pgwt-server` from the trace, so the demo was never affected.** This was a live-display
defect, not data loss.

### #416 reproduced on the SHIPPED path, 1 in 10

Attempt 09 used the unmodified Test-1 invocation (`--view time_model`, **no `-T`, no `-v`**):
DB Time 8600.8 ms, Timeout **1.7 ms = 0.0198%** against a healthy 50.0%. The `system_event`
capture reproduced **2 of 10**. The two assertions fail **independently** — same underlying loss,
different symptoms. So the "0/51" on the issue was the wrong capture, not a low rate.

### The fix (`agent/416-tick-drain`, `d2c1b64`)

Drain the event ring **before** building the display. The budget was measured, not guessed:
backlog at tick time is **86–432 records (median 393)** against 64 per pass, so **64 is too small
by 1.3x–6.8x**. The fix **loops** the existing budget rather than raising it, bounded two ways —
256 passes (16,384 records) **and** a wall-clock budget of half the display interval, clamped
50–500 ms. The time bound is load-bearing: under a forced 20 ms callback a record-only cap could
spend 256 x 64 x 20 ms in one tick; measured overshoot under that fault was 1.29 s against a 2 s
interval, with the display timer still running.

**The assertion was the real problem.** `PgSleep > 2000 ms` is a **floor, not a conservation
check** — the same loss truncated to 3 s passes silently, and it reads only the last tick. That is
why this survived for months. Replaced with "75% of the window the sleeper provably spent in
PgSleep".

### Evidence

48/48 unit checks with **four independent REDs** (drain removed → 18 failures; single 64-callback
pass → 8; both bounds removed → 6; drain after the display copy → 5). `box-check` gate-2:
**135 passed / 0 failed**, exit 0, Live UI smoke PASS, header `commit=d2c1b64`. `ci_smoke
--pg-version 18`: all sections passed. Live reproduction **2/15 pre-fix → 0/15 post-fix**.
`make check` passed.

### Limits the implementer stated itself

- It never observed a live run in the band the new floor **uniquely** catches
  (2000 < PgSleep < 6000); that discrimination is shown deterministically against the shipped
  parser, not live.
- **0/15 post-fix is not proof of absence** — at the 2/15 pre-fix rate, P(0 in 15) ≈ 0.11. The
  deterministic unit test is the real evidence.
- It **deliberately did not** add a "class rows reconcile with DB Time" check: both sides come out
  of the same accumulation, so a dropped record leaves them agreeing — **the can't-fail shape**.

### A third instance of the same structural gap

`test_capture_smoke.py` lives in `ci_smoke.sh`, **not** `run_all.sh` — so `box-check` alone does
not exercise the phase most affected by `src/daemon.c` changes. That is the same
`run_all.sh`-vs-`ci_smoke.sh` split that hid the v9 gate twice tonight, now found in a third
place. It is worth a structural fix separate from this work.

## Review: NO BLOCKERS, clear to merge

The fresh reviewer cleared `agent/416-tick-drain` with two should-fix items and one nit, all
comment-accuracy or dead code, none changing behaviour. Both of its earlier blockers are
confirmed resolved. It also **validated a judgement call** the implementer had made: declining to
add a "class rows reconcile with DB Time" check was right, because both sides come from the same
`event_accum` and a dropped record shortens both equally — the can't-fail shape. The chosen
assertion puts **wall time** on the other side, which is genuinely independent.

### It corrected the safety reasoning, though not the conclusion

The implementer's comment said BPF emits the closing record *"in the same transition that advances
`last_ts`"*, implying atomicity. The source shows otherwise: `emit_event(&evt)` at
`src/bpf/pg_wait_tracer.bpf.c:565` happens **before** the store at `:568-569`, same order at
`:743`/`:781`. The real safety argument is **ordering plus timescale** — the drain strictly
precedes the `state_map` read, and the emit→store gap is nanoseconds against a microsecond
memcpy. **That window is pre-existing and this patch narrows it.** The comment has been corrected
to say so, and the drain-before-`state_map` ordering is now marked load-bearing (it is pinned by
the "drain after the display copy → 5 failures" RED).

### And a clamp that has exactly one member

`interval` is `int` seconds, so `interval*1e9/2` is always >= 500 ms and clamps **down** to MAX:
the budget is **always exactly 500 ms** and the `MIN` (50 ms) leg is unreachable. The `#define`
comment also claimed *"the drain can never be the reason a tick is late"* — it can be late by up
to ~1.78 s against a 2 s interval, since the deadline is checked once per completed 64-callback
pass. Both corrected.

## #433 filed — the meta-defect

`run_all.sh` and `ci_smoke.sh` have **unreconciled membership**, and three times in one night a
test was invoked by one runner, by neither, or existed only as a comment — while `box-check` went
green over it. Twice that produced a green box-check over an unexercised or permanently-red
check. Every instance was caught by someone reading the scripts; never by a gate. The existing
registration guard checks `unit_tests.list` against the Makefile and nothing checks the two
runners against each other.

## Uncounted dry run — protecting the last two counted attempts

With gate-1 otherwise idle and the merge still gating, an **explicitly uncounted** rehearsal dry
run is running against `d8a1cab` (the tree master will become). The criteria permit this
("Pre-attempt dry run: declare it uncounted before it starts"). Rationale: attempt 2 was clean on
every substantive criterion and was disqualified on **provenance alone**, and only two counted
attempts remain. Discovering a harness or capture problem on a counted attempt would be
unrecoverable; discovering it here costs nothing.

## Uncounted dry run: CLEAN — and it found two things worth more than the pass

Archive: `tests/results/demo_rehearsal_DRYRUN_1792120999/` (marked UNCOUNTED in the directory
name and its README). Run against `d8a1cab` on gate-1, 35 min, `RETAIN_TRACE=1`.

**9 of 9 checks green.** `num_events` 7,471,104. Conservation **gap 0.0 ms** on a 2,648,807.0 ms
window. `cross_tab_db_time_agreement` and `cross_tab_aas_agreement` both **0.00%**.
`daemon_integrity` all three counters zero. `daemon_log_clean`: 0 errors, 0 degraded warnings.
11/11 tabs. Off-CPU **7/7 in band** (10.31, 11.17, 11.54, 11.32, 7.78, 11.27, 10.45%), full-window
9.09% — reported, not gated. Sessions first-paint 501 / 1290 / 1030 ms against the 3000 ms bound.
No `KNOWN_FAILING` entry fired.

**Hygiene verified rather than assumed:** `tree-hash.sh` matched an **independent clean checkout**
of `d8a1cab` (`f9eba68d…`), `git status --porcelain` empty, and **still empty after a full
`make -j4 && make -C tests -j4`** — so the build-products class rule written tonight holds under
a real Linux build.

### Finding 1 — the rehearsal cannot validate the #416 fix, in either direction

Two reasons, both confirmed from source rather than inferred:

1. #416 lives in `pgwt_handle_timer` — the **daemon's own live tick display**.
   `demo_rehearsal.sh` runs the tracer with `--daemon` and serves the web UI entirely from
   `pgwt-server` reading trace files on disk, so it **never queries the live-tick path the fix
   touches**.
2. The demo workload's server-side `pg_sleep` — the exact wait class #416 loses 100% of — was
   **replaced with a client-side `time.sleep()` on 2026-10-06** for an unrelated demo-polish
   reason. So no `Timeout:PgSleep` event exists in this workload at all.

**So a clean rehearsal says nothing about #416.** Its evidence is the unit gate (48/48, four
independent REDs) and the live 2/15 → 0/15 reproduction on the `test_percentage` path. The PR
must not imply otherwise.

### Finding 2 — the attempt-2 killer is still live, in a second file

`tests/results/.demo-rehearsal-provenance-<run_id>.json` is **not** in `.gitignore`, unlike its
sibling `.demo-rehearsal-state.sh` which is. The dry run produced one and it appeared as untracked
in `git status --porcelain`; it had to be deleted by hand.

On a **counted** attempt that file moves `scripts/tree-hash.sh`, the provenance stops matching,
and the attempt is disqualified on exactly the grounds attempt 2 was. Being fixed as a **class**
before attempts 3 and 4 — which are the last two under the cap.

This is the second time tonight that an untracked results artifact has threatened the provenance
chain. The first cost a counted attempt; this one was caught by spending an uncounted run
instead, which is precisely what the dry run was for.

## MERGED — master is `97dca0c`

PR **#434** merged at 2026-10-16T05:08:51Z with CI green (no failing checks; `capture-smoke`
across PG 13/16/17/18, `build-and-unit`, `web-ui`, `snapshots`, `protocol-drift`,
`sampled-overhead`). Commit message:

> #424: select raw window records by overlap, with per-occurrence statistics intact — plus #429
> block candidacy and the #416 tick drain (#434)

Master moved `304a772` → **`97dca0c`**. That is the candidate sha for counted attempts 3 and 4,
which are now running against it.

### What is in it

- **#424** — overlap admission at all six sites, the INSTANT/INTERVAL selection split stated once
  and applied at every consumer, straddler lookahead bounded and budgeted with honest truncation
  reporting
- **#429** — per-block candidacy replacing a binary search over a commit-ordered index
- **#416** — the event ring drained before each display tick, bounded by record count and wall
  clock
- `--replay` clipping on both edges, interval-index selection realigned (`VERSION` 1→2), crash
  closure detection (D1), `.gitignore` classes for build products and rehearsal artifacts, a docs
  version gate, and the link-closure guard promoted into `make check`

### What is deliberately NOT in it

- the **live right edge** — overlay module parked unmerged at `agent/overlay-wiring` @ `4e196d5`
- **summary open-CPU** — v9 writer exists, no producer asks it, `PGWT_SUMMARY_VERSION` stays at
  **8** rather than shipping a version whose contract is not yet true
- **D2 checkpoints** — a library with an end-to-end gate and no producer

## Remaining for the owner

1. **Counted attempts 3 and 4** — running now; these are the last two under the cap
2. **Your two signed Mac walks** — Mac-side is **0 of 2** and only your signature advances it
3. **Video + GIFs** at the tagged sha, with my own frame inspection and all three advisers over
   the package
4. **Your tag**

# ⛔ RELEASE BLOCKED ON #415 — ONE DECISION NEEDED FROM YOU

Everything else is done. This is the only thing standing between you and a tag, and it is a
decision I should not make for you.

## The conflict, in four facts

1. **The new recorder refuses the rehearsed UI.** It raises `Refusal(2)` when the drilled
   Waterfall readout sits below the fold — *"the published clip would need a scroll jump to show
   what the clicked bar was"* — and again if `scroll_y != 0`. On `97dca0c` that readout sits
   **67–101 px below** the fold. The refusal is **correct behaviour**: it is declining to publish
   a clip the product cannot render properly.
2. **The old recorder's clips were judged not postable** — by my own frame inspection, not a
   gate: headline chart ~45% empty, the headline clip 47 of 48 frame pairs at ~0.02% with one at
   1.9% (effectively static), and the drill-down a single flat bar.
3. **Fixing #415 requires a merge**, and any merge now **invalidates counted attempts 3 and 4**
   with **no attempt budget left** (cap 4; 1 and 2 are spent).
4. **Tagging first and recording afterwards violates provenance, not merely ordering.** The
   criteria retain the immutable reference **shared by both attempts and the GIF**; a tag+1 tree
   is immutable but *different*, and cannot inherit the rehearsed tree's validation.

## Why I did not just pick one

Bypassing the fold check would remove an **acceptance condition** — the thresholds would be
untouched, but the recorder would publish something it had judged unpublishable. That is
gate-weakening wearing a different hat, and it is the one thing you told me not to do.

And #415 is genuinely a **product** fix, not an instrument one: it adds `executionListMaxHeight()`
and calls `fitExecutionList()` on render and resize, changing shipped UI geometry. The
unmerged-instrument allowance does not stretch to it, however the provenance is labelled.

## Your options

**A — Tag `97dca0c` now, fix #415 after, record the GIFs from the later tree.** Best GIFs. But
they demonstrate a tree that is not the tagged one, and under your own criteria they cannot
satisfy this release's GIF requirement — they would be an honestly-labelled *later-build
demonstration*.

**B — Post GIFs from the rehearsed tree with the known presentation defect.** Faithful to exactly
what you tagged. But I inspected those clips myself and I would not call them postable, so I
cannot recommend it without saying that out loud.

**C — Park the tag.** Merge #415, regenerate the `waterfall_execution` snapshot baseline via CI,
spend two fresh counted attempts, then record. Costs the attempt sequence and a day; produces a
tag, a GIF and a provenance chain that all agree.

## What is preserved for whichever you choose

Counted attempts 3 and 4 are running against `97dca0c` and their results will be recorded with
their **actual** verdicts — not re-run, not reinterpreted. `agent/gif-composition` is parked
unmerged with the #415 fix and the recorder improvements intact. `agent/overlay-wiring` is parked
at `4e196d5`.

# ✅ CAPTURE-SIDE COMPLETE — 2 consecutive clean of 4 attempted

Both counted attempts ran against the merged master `97dca0c`, on gate-1, PG18.

| | attempt 3 (`1792127766`) | attempt 4 (`1792131171`) |
|---|---|---|
| `ok` / `failed` / `plan_degraded` | True / [] / False | True / [] / False |
| `num_events` | 7,405,568 | 7,380,992 |
| conservation, full window | **gap 0.0 ms (0.00%, exact)** | **gap 0.0 ms (0.00%, exact)** |
| `cross_tab_aas_agreement` | 0.00% | 0.00% |
| `cross_tab_db_time_agreement` | 0.00% | 0.00% |
| `daemon_integrity` | 0 / 0 / 0 | 0 / 0 / 0 |
| Off-CPU, 7 samples (band 5.08–13.52%) | 8.17, 10.16, 12.21, 10.33, 7.96, 11.77, 9.09 — all in band | 11.19, 8.45, 10.09, 10.97, 5.98, 12.49, 11.76 — all in band |
| Off-CPU, full window | 9.01% | 8.67% |
| Sessions first-paint (bound 3000 ms) | 653 / 1283 / 1089 ms | 679 / 1421 / 1095 ms |
| `sweep_offset_coverage` | 33/33 | 33/33 |
| `KNOWN_FAILING` fired | 0 of 33 | 0 of 33 |

Two informational "EARLY WINDOW LIKELY MISSED" lines per attempt (timeline tabs), `ok` unaffected
— the deferred #397 strictness class, not failures.

**Provenance held this time.** `scripts/tree-hash.sh` = `6d0f720aeb26463c2b962b0379e0156678bd8860`
verified **three independent ways** — the worktree, a `git worktree add --detach` clean checkout,
and a **fresh `git clone` on gate-1** — all byte-identical. `git status --porcelain` empty before
each launch and after each archive; `remote_tree_hash` matched with
`tree_hash_verification: "match"` both times; a full `make all && make -C tests` on a fresh clone
left porcelain empty. The attempt-2 failure mode did not recur.

Traces retained (214 MB and 206 MB).

## The v9 check did not apply, and the agent proved that rather than guessing

I had required `summary_open_cpu_credited_ns` to be non-zero as "the failure that still looks
green". On this tree it is **structurally 0 by design**: `PGWT_SUMMARY_VERSION` is **8**, and
`src/map_reader.c:647` calls the v8 `pgwt_summary_note_open`, never the v9
`pgwt_summary_note_open_ex`. `src/control.c` says so in its own comments — the capability is
published but dormant. The live window had closed before the check could run, so the agent used
**source proof instead of a live sample** and flagged the deviation rather than skipping or
guessing. That is stronger evidence, not weaker: deterministic rather than one noisy run.

# CORRECTION to option C

I wrote that option C costs "the attempt sequence and a day". **The cap is now fully used — 4 of
4.** So merging #415 would reset the counter with **no attempts remaining**, which means C also
requires you to **amend the four-attempt cap** in `docs/DEMO_REHEARSAL_CRITERIA.md`. That is a
criteria change, not just a time cost, and only you can make it.

# WHERE THIS ENDS

**Done:** #424, #429, #416 fixed, reviewed, box-green 135/135, merged as `97dca0c`. Capture-side
**2 consecutive clean of 4**. Ten issues filed (#424–#433). Six blind gates found and closed.

**Needs you, and only you:**
1. **Two signed Mac walks** against `97dca0c` — Mac-side is **0 of 2** and no automated result
   advances it.
2. **The #415 decision** — A (tag now, GIFs from a later tree), B (GIFs from the rehearsed tree
   with the known defect), or C (amend the cap, merge #415, re-rehearse).

# VIDEO ANALYSED — by me and all three advisers. Verdict: NOT POSTABLE

The owner asked for the video to be inspected by me **and** the advisers before handover. I had
skipped it, treating the #415 blocker as if it settled the question. It did not. Here is the
result.

## The product is correct — three independent re-derivations

All three advisers re-derived the Overview numbers from the clean rehearsal at `97dca0c` and
confirmed them. #408's two CPU rows render. AAS 1.39 is consistent. No computation is wrong.

**One correction to my own claim:** I said conservation holds "exactly on screen". It holds
**after rounding** — the displayed sums are 878.10099 s and 1253.2173 s. Exactness lives in the
underlying measurement, not in the rendered table.

## Nine presentation defects, filed

I had inspected **two** tabs. The advisers checked all eleven.

| issue | defect |
|---|---|
| **#435** | Overview renders Idle as `0.0%` beside 22,122 s with a populated AAS column; Events already uses `—` for the same case |
| **#436** | Transitions clips arcs **top and bottom**, nodes sit off-canvas, and **`Lock:relation` — the largest wait at 713 s — is absent at 20% simplify** |
| **#437** | Events P50/P95/P99 print `>=16.4ms` for waits averaging **3.9 s**, because the histogram's top bucket is 16 ms — the row contradicts its own `avg` and `max` |
| **#438** | Transition Matrix is a degenerate L: every transition routes through `CPU*`, so the grid is empty |
| **#439** | Sessions shows background processes with DB Time while Overview puts them outside it — the tabs disagree |
| **#440** | Overview rows with near-equal times swap order between ticks — visible flicker in any clip |
| **#441** | Queries top row reads 720.4 s / 91 execs / **36 us avg** — correct (the lock precedes `EXEC_START`) but reads as impossible |
| **#442** | Concurrency burst markers fire on nearly every bucket, distinguishing nothing |

Timeline also shows a single lane.

## No gated GIF can be cut from this material at all

- The footage is **Chromium-on-box Playwright video at 1710x980** — not pinned Mac Chrome, not
  the gated recorder. A cut from it has the rehearsal's provenance but **no `walk.json`** and no
  gate behind it.
- **It is visually static.** Measured between refreshes on `overview.webm`: 0.82%, 0.14%,
  **0.000%**, 0.32%, 0.11% changed pixels — every step under the 1% visible-change bar, one step
  with no refresh at all. The earlier clips that scored 6% got it from a **scripted crosshair
  sweep this walk does not perform**.
- **The capture scrolls the AAS chart away after ~6 s**, and other clips show capture jumps and
  grey regions (attempt 3, late Concurrency, ~15 s). Duration cannot rescue the headline.

**Correction to an earlier claim of mine:** I had recorded "~10 s dwell, one visible update". That
is wrong for this material — the webms run 34-42 s with **18 ticks per tab per attempt**. The
blocker is framing and steady-state stillness, not dwell length.

## The drill-down has two independent problems — confirmed

Both Waterfalls are the **same object**: query `-6981478149691145874` (`pg_advisory_lock(45)`),
exactly 3.000 s, one lane, "Leader events 4, Workers 0".

1. **#415** — the readout sits below the fold, so the gated recorder *refuses* (correctly).
2. **Content** — the executions list is "longest running first", and every long row is that
   advisory-lock query. Fixing #415 yields the same flat bar.

**All three advisers qualify this**, and I had overstated it: the evidence proves **uninformative
recorded selections**, not that the workload cannot produce multi-wait executions. The pgbench
UPDATEs do have CPU+IO+Lock profiles — they just run 100-500 us, too short to see. So this is a
**selection** problem as much as a workload one, and that is more fixable than I implied.

Precision note: #415 is about the **readout below the lane**, not the waterfall itself — the lane
and its axis are within the viewport.

# UNANIMOUS RECOMMENDATION: option C

All three advisers independently recommend the same path:

1. **Explicitly amend the four-attempt cap** in `docs/DEMO_REHEARSAL_CRITERIA.md` — it is fully
   used at 2 consecutive clean of 4, so this is a criteria change only the owner can make
2. Merge **#415**, and address **execution selection** and **capture framing**
3. Re-rehearse on one frozen candidate, with paired signed Mac walks
4. Approve and cut the GIFs **before** tagging, preserving the provenance chain

Options A (tag now, record from a later tree) and B (post this material) were both examined and
rejected: A breaks provenance rather than merely ordering, and B means publishing visibly weak
material and calling an approved pair something it is not.
