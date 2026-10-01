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
| 1.1 | **#252** blink sweep cannot measure Sessions/Events/Queries at demo length | DONE (merged 44812c9, #255) — follow-on mis-specification in its own `capture_ms` gate fixed by #259 (bb2ad84) | — | — | **Corrected, consistency pass (this commit):** the original stop condition here named a `capture_ms` bound and "≥5/6 measured" that match neither the merged code nor each other. As shipped: `<tab>.capture_budget.ok` is still computed as `true`/`false` per tick against `CAPTURE_MS_BOUND_MS` = 800 ms (`capture_budget_ok`, `tests/ui_live_smoke_lib.py`) — **corrected, review round 2**: an earlier version of this row wrongly said it is always `null`. It is `null` only on a tab that never got far enough to be measured at all (`build_failed_tab_result`'s default); on a normally-completing tab it is a real, often-`false` value — e.g. a 2026-10-01 live run on `agent/ttfp-timing-field` (`tests/results/ui_live/summary.json` in that worktree) has `sessions.capture_budget = {"ok": false, ...}` with six offending ticks up to 1248.9 ms, alongside four other `ok: false` tabs. The field is reporting-only because `capture_budget_ok`'s result is demoted out of `build_tab_result`'s `ok` by #259 (a slow-but-correctly-sized capture is not what the replacement gates protect against), not because it is never computed; the actual per-tick gates are `<tab>.frame_dims.ok` (±`FRAME_DIMS_TOLERANCE_PX` = 1.0 px) and `<tab>.frame_spacing.ok` (≤`FRAME_SPACING_DRIFT_BOUND_MS` = 150 ms); the measured-fraction gate is `<tab>.no_blink.measured.ok`, read from `measured_count/attempted_count >= MIN_MEASURED_FRACTION` where **`MIN_MEASURED_FRACTION = 0.5`** (`tests/ui_live_smoke_lib.py:77`) — i.e. **3/6**, not the 5/6 this row previously demanded. A document must not promise a threshold the code does not enforce; this row now names the fields and constants actually read |
| 1.2 | **#222** Waterfall slowest-first + orphan-row fix | **DONE** (corrected, review round 2 — merged as #262/`8c1eb93`, confirmed via `gh issue view 222` now CLOSED; was wrongly re-confirmed READY earlier this round before the branch's rebase picked it up) | — | — | check stamp matches tree-hash; box-check green; gallery justified |
| 1.3 | **#224** committed live injected-blink proof | DONE — shipped inside #252/#255 as `--inject-blink-ms`, off by default | — | — | self-test section of `tests/ui_live_smoke.py` injects a synthetic blink and asserts the gate catches it (`SELF-TEST PASS`, e.g. `tests/results/box-check-ubuntu-20260930-150337.log`); verified it still runs and still catches the injected blink |

**Section 1 is now entirely DONE**: 1.1 (merged as #255, follow-on fixed by
#259), 1.2 (merged as #262), and 1.3 (shipped inside #255) — corrected, this
commit; an earlier pass through this review round left 1.2 at READY and a
now-stale note claiming it was "the remaining item" after rebasing onto
`origin/master` picked up its merge.

## 2. Criteria that must be pre-registered BEFORE counted attempt one

Committing a bound after seeing a result invalidates it. All of these land in
one reviewed criteria commit, and that commit must precede the tag.

| # | Item | Status | Next action | Depends on |
|---|---|---|---|---|
| 2.1 | Time-to-first-paint bound | **PARTIAL, not DONE** (corrected, review round 2 — a prior version of this row wrongly marked it DONE) | a **PROVISIONAL capture-side-only** 3000 ms global bound is recorded in `docs/DEMO_REHEARSAL_CRITERIA.md` §4, derived from three real-box runs (`agent/ttfp-timing-field`, merged as `fa20067`/#257) — but this row's own requirement is **≥3 navigations per tab on the demo configuration**, and what exists is 1-2 navigations per tab in **headless Chromium on a VM**, not real Chrome on the Mac. Still needed: ≥3 navigations/tab on real Chrome on the Mac. Across the three VM runs the worst tab rose 533→541→1211 ms (+127% run B→C alone), which also cut the bound's margin from the originally-claimed >5.5x to **≈2.5x** — both corrected in the criteria doc | — |
| 2.2 | AAS cross-tab agreement (bucket-weighted) | **DONE** (corrected, review round 2 — was stale READY; #254/`c69c84d` already shipped and wired this in) | `bucket_weighted_aas_ok` runs automatically inside `demo_rehearsal.py`'s `extra_checks["cross_tab_aas_agreement"]` — nothing left to dispatch | — |
| 2.3 | Overhead envelope, n=3 paired full-mode A/B | BLOCKED → 4.1 | the rehearsal runs only the tracer-on arm, so this needs its own paired runs on the demo box. Owner decision recorded separately (criteria §6): current band is accepted as not-a-regression, which does not substitute for this specific demo-workload A/B | 4.1 |
| 2.4 | §2 AAS floor — still PROVISIONAL | **DOC DONE, CODE BLOCKED** (review round 4 — the first clean rehearsal landed, settling the number) | Doc floor settled at **1.15** (half of the clean run's 60s-recent-window AAS, 2.3033) in `docs/DEMO_REHEARSAL_CRITERIA.md` §2. `tests/demo_rehearsal_lib.py`'s `AAS_FLOOR_PROVISIONAL = 0.5` constant is deliberately untouched (docs-only branch) — bumping the enforced gate to 1.15 is a follow-up code change, separate branch. Run id for the clean rehearsal was not supplied to this commit; needs backfilling | — |
| 2.5 | §3 per-class wait CPU | **BLOCKED — method mis-specified** (review round 4, owner ran it on the box — not a box-access problem) | `tests/cross_validate.c` requires a **tiered-mode** trace (sampled blocks + transition blocks, compared over their overlap); full mode produces zero sample blocks. Confirmed against the retained 35-minute full-mode trace: `pgwt-server --dump` → `4432286 transitions, 0 samples`; `cross_validate` → `ERROR: no sample blocks`. What would actually answer this: a per-event `cpu_ns` extractor for full-mode traces (does not exist), or a `--mode tiered` capture (then not the demo configuration). No number invented, no tool built here | — |
| 2.6 | §3 Off-CPU ≤10% placeholder | **DONE** (reworded, review round 3 — retracts round 2's "unverifiable", which was itself wrong) | **Round 2 was wrong**: "4.92 pp" IS a derivable sum (2.53 + 2.39 pp, `docs/OBSERVER_BIAS.md` ~185-188 on `agent/observer-bias-study`) — not invented, not untraceable. The real flaw, now written into the criteria doc: that sum is measured on the **wrong regime** (saturated 8-client/4-vCPU, vs. the gate's own clients≤cores scope, whose analogue is ≤2.72 pp), the **wrong unit** (backend wall-time, not DB Time), and the **wrong quantity** (neither cell flows into Off-CPU\* — one is booked under its open wait event, the other is outside DB Time entirely). Reworded as an honestly-labelled provisional catastrophic-loss tripwire rather than a calibrated bound; demo measured <0.1% (n=7, one run) against it. **Owner decision recorded: #115 is not landing before the tag** (it wouldn't validate the clients≤cores regime this gate needs either) — closes the row via the "reword" branch of this item's original next action, not the "commit the artifact" branch | — |

## 3. Dry runs (uncounted)

| # | Item | Status | Depends on |
|---|---|---|---|
| 3.1 | Uncontended dry run on the demo candidate tree, demo topology | READY (section 1 is now entirely DONE — see above) | 2.4's number is now produced by the first clean rehearsal (review round 4), not this dry run — still useful for one of the real-Chrome-on-Mac navigations 2.1 needs (2.1 is PARTIAL, not DONE — see above) |
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
