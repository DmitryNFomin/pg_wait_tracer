# Demo delivery queue — the ordered path to 2026-10-12

The standing, ordered list of everything between here and a demo-ready tag.
**Its purpose is that work never stops for want of a decision**: any lead —
this one or a replacement — reads this file and knows what to dispatch next
without reconstructing state from a conversation.

Written 2026-09-30 after a measured **7.9-hour overnight gap** in which nothing
was produced (#252 filed 00:04, next artifact 08:00) because the lead reported
and then waited instead of advancing the queue. The rule that follows from it:

> **On every agent report or gate result, the lead dispatches the next READY
> item in the same turn.** Never "I'll report in the morning". A reviewer may
> hand nits straight back to its implementer; it does not choose the next item.

Quality bar does not move: no dropped gates, no widened tolerances, no
hardening against noise, no counted attempt on unverified evidence.

## Status key

`READY` dispatchable now · `BLOCKED` waiting on a named item · `OWNER` only the
owner can do it · `DONE`

---

## 1. Demo blockers — must merge before the tag

| # | Item | Status | Next action | Depends on | Stop condition |
|---|---|---|---|---|---|
| 1.1 | **#252** blink sweep cannot measure Sessions/Events/Queries at demo length | DONE (merged 44812c9, #255) — follow-on mis-specification in its own `capture_ms` gate fixed by #259 (bb2ad84) | — | — | **Corrected, consistency pass (this commit):** the original stop condition here named a `capture_ms` bound and "≥5/6 measured" that match neither the merged code nor each other. As shipped: `<tab>.capture_budget.ok` is always `null`/reporting-only (`CAPTURE_MS_BOUND_MS`/`capture_budget_ok`, `tests/ui_live_smoke_lib.py`, demoted by #259 — a slow-but-correctly-sized capture is not what the gate protects against); the actual per-tick gates are `<tab>.frame_dims.ok` (±`FRAME_DIMS_TOLERANCE_PX` = 1.0 px) and `<tab>.frame_spacing.ok` (≤`FRAME_SPACING_DRIFT_BOUND_MS` = 150 ms); the measured-fraction gate is `<tab>.no_blink.measured.ok`, read from `measured_count/attempted_count >= MIN_MEASURED_FRACTION` where **`MIN_MEASURED_FRACTION = 0.5`** (`tests/ui_live_smoke_lib.py:77`) — i.e. **3/6**, not the 5/6 this row previously demanded. A document must not promise a threshold the code does not enforce; this row now names the fields and constants actually read |
| 1.2 | **#222** Waterfall slowest-first + orphan-row fix | READY (confirmed open, 2026-10-01) | gate cycle on current tree (fix + red-case tests already complete) | — | check stamp matches tree-hash; box-check green; gallery justified |
| 1.3 | **#224** committed live injected-blink proof | DONE — shipped inside #252/#255 as `--inject-blink-ms`, off by default | — | — | self-test section of `tests/ui_live_smoke.py` injects a synthetic blink and asserts the gate catches it (`SELF-TEST PASS`, e.g. `tests/results/box-check-ubuntu-20260930-150337.log`); verified it still runs and still catches the injected blink |

**1.1 is DONE** (corrected, this commit — was previously marked the hard
blocker here; merged as #255, follow-on fixed by #259). **1.2 (#222) is now
the remaining item in this section.**

## 2. Criteria that must be pre-registered BEFORE counted attempt one

Committing a bound after seeing a result invalidates it. All of these land in
one reviewed criteria commit, and that commit must precede the tag.

| # | Item | Status | Next action | Depends on |
|---|---|---|---|---|
| 2.1 | Time-to-first-paint bound | DONE (this commit) — **3000 ms, global**, recorded in `docs/DEMO_REHEARSAL_CRITERIA.md` §4 with derivation; sourced from `agent/ttfp-timing-field`/PR #257 (one navigation per tab, not yet the ≥3-navigations-each this row originally asked for — a reproducibility check against a second real run found a >2x per-tab swing, noted in the criteria doc, and the bound's margin was set wide enough to absorb it) | — | — |
| 2.2 | AAS cross-tab agreement (bucket-weighted) | READY | compute offline from a run's `aas` + `time_model` responses over the identical window | — |
| 2.3 | Overhead envelope, n=3 paired full-mode A/B | BLOCKED → 4.1 | the rehearsal runs only the tracer-on arm, so this needs its own paired runs on the demo box | 4.1 |
| 2.4 | §2 AAS floor — still PROVISIONAL | BLOCKED → 3.1 | set from the first clean uncontended run | 3.1 |
| 2.5 | §3 per-class wait CPU | READY | settle offline: max `cpu_ns` on a `Timeout:PgSleep` event via `tests/cross_validate.c` | — |
| 2.6 | §3 Off-CPU ≤10% placeholder | READY | commit the artifact from `agent/observer-bias-study` or reword the row | — |

## 3. Dry runs (uncounted)

| # | Item | Status | Depends on |
|---|---|---|---|
| 3.1 | Uncontended dry run on the demo candidate tree, demo topology | BLOCKED → 1.2 (1.1 is now DONE) | produces 2.4's number (2.1 is now DONE — see above) |
| 3.2 | Throwaway Mac walk against the checklist, to prove it is walkable | READY | needs a quiet Mac, not the tag |

## 4. Owner-only

| # | Item | Status | Why it cannot be delegated |
|---|---|---|---|
| 4.1 | Provision **`pgwt-stage`** | OWNER | owner rule 2026-09-28: he says when, a couple of days ahead. **Needed EARLIER than the tag** — 2.3's overhead A/B requires it |
| 4.2 | Two uninterrupted Mac sign-off windows | OWNER | the walk needs the real display, real Chrome, normal memory pressure, and no agents on that Mac |
| 4.3 | Any unresolved demo-bound decision | OWNER | must be settled before the criteria commit, not after |

## 5. The tag, then the counted sequence

1. Criteria commit (section 2) lands and is reviewed.
2. **Tag** the reviewed master commit. Build `pgwt-stage` from that tag.
3. Capture-side attempt 1 + signed Mac walk 1 — **against the tag**, from a
   worktree checked out at it.
4. Capture-side attempt 2 + signed Mac walk 2 — **same tag**.

Master keeps moving throughout; this is a tag, not a code freeze (owner rule).
Landing a fix into the demo build requires a **new tag and resets both counters
to zero**. Report as "capture-side k of N, Mac-side m of M" — never merged into
one number, and Mac-side means signed manual walks.

## Parallelism rules

- `box-check` starts FIRST; `make check` runs during it — never in series.
- Reviewer starts reading code while gates run; verdict waits for final-tree
  evidence.
- Different branches' box runs go on different boxes. `pgwt-gate` is the only
  box accepted for decisive evidence (#237); `pgwt-gate-2` takes review-round
  re-runs.
- At most two branches in gate phase (the Mac's `make check` lock is the cap).
- Arm auto-merge as soon as a PR is ready; do not watch CI.

## Known schedule risk

Four of four full-length rehearsals have each surfaced a NEW blocking harness
defect (#160, #176, #239, #252), roughly two days each plus a re-tag. The
counter-measure is item 1.1's last step — running the live UI smoke at the demo
viewport (1710x981 @ DPR 2) inside every `box-check`, so a DPR-2 defect fails at
PR time instead of after a 40-minute rehearsal. #252 escaped four PR gates
precisely because every gate walks at 1280x900 @ DPR 1.
