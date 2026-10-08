# Click-to-painted latency after the 2026-10-08 workload change — OVER THE BOUND

**STOP condition reached.** The brief: *"If paint exceeds or approaches
3000 ms: STOP and report. Do NOT reduce the load. Owner's explicit
instruction: the fix is then the UI or the server, not a thinner workload.
That becomes a separate branch."*

Worst single navigation: **5785 ms against the 3000 ms bound — 0.52x margin,
i.e. 1.93x OVER.** Five of eleven tabs exceed the bound. Two tabs failed to
render *at all* on one navigation each. Nothing in this branch has been
thinned, loosened or retried in response.

## The numbers

`compare-r1.txt` (regenerate with `python3 compare.py walk-r1.json`). Pinned
column = the 2026-10-07 per-tab maximum over 15 navigations, from
`../demo_paint_20261007/SUMMARY.txt`.

| tab | n | min | med | max | pinned max | ratio | state |
|---|---|---|---|---|---|---|---|
| queries | 3 | 2091 | 4615 | **5785** | 1592 | 3.63 | OVER |
| waterfall | 3 | 4401 | 4405 | **4796** | 1464 | 3.28 | OVER |
| scatter | 3 | 3927 | 4065 | **4358** | 1616 | 2.70 | OVER |
| concurrency | 2 | 3246 | 3569 | **3569** | 1165 | 3.06 | OVER + failed to render once |
| transitions | 2 | 2889 | 3253 | **3253** | 971 | 3.35 | OVER + failed to render once |
| matrix | 3 | 2818 | 2875 | 2949 | 1139 | 2.59 | under, barely |
| events | 3 | 215 | 264 | 315 | 266 | 1.19 | ok |
| sessions | 3 | 213 | 241 | 314 | 264 | 1.19 | ok |
| histogram | 3 | 345 | 375 | 408 | 440 | 0.93 | ok |
| overview | 3 | 175 | 239 | 329 | 395 | 0.83 | ok |
| timeline | 3 | 230 | 251 | 300 | 441 | 0.68 | ok |

`paint_analyse.py`'s own verdict is **REFUSED**, and for the right reason:

    r1: tab transitions did not render (no container) -- a tab that never
    painted cannot be excluded from the worst case

That is the gate working as designed (`verdict-r1.txt`). It will not certify a
margin from a walk with a hole in it, which is exactly the false negative its
predecessor had. Both facts point the same way: the raw numbers it declines to
certify are all far over the bound, and two tabs stopped painting entirely.

## The confound, and the internal control that rules it out

The pinned baseline was taken on **gate-2**; this run is on **gate-1**. Same
cx33 class, 4 cores, but the two boxes are known to differ in CPU vendor
(owner declined a swap, 2026-09-29), so a cross-box comparison is not free.

The five light tabs are the control. They read data whose volume barely moved
and they land at **0.68-1.19x** of their pinned maxima — i.e. the two boxes
render the same work in the same time. The six heavy tabs, on the same two
boxes in the same session, are at **2.59-3.63x**. A hardware difference would
not spare overview/events/sessions/histogram/timeline and triple
queries/waterfall/scatter/concurrency/transitions/matrix. The blow-up tracks
what those six tabs read, not which box read it.

## Conditions (`conditions-r1.txt`, stamped into `walk-r1.json`)

| | |
|---|---|
| topology | real Chrome 154.0.8037.100 on this Mac, viewport 1710x981 @ DPR 2 confirmed by two agreeing reads -> `web/pgwt` on the Mac -> `pgwt-server` over ssh on gate-1 -> live `--mode full` capture of PostgreSQL 18 |
| active range | 15m (asserted, not assumed) |
| capture | fresh empty trace dir, age 1112 s at conditions time, `fidelity: exact` |
| load | all three tiers alive and verified: `pgbench -c 4 --rate=25`, `live_loop_workload.py`, `burst_load.sh` — 18 burst cycles, **zero ABORT lines** |
| box load | loadavg 1.71 / 1.31 / 1.73 on 4 cores — not a saturated box |
| volume | 6 660 465 events over 1112.8 s = **5987 events/s**; 537 distinct sessions; whole-trace DB Time 940 950 ms, AAS 0.85 |
| in-window (900 s, derived) | 5.39 M records, against 3.47 M when the 1616 ms figure was pinned — **1.55x** |

The walk's own scraped time model agrees with `../demo_mix_20261008/`:
`Lock:relation` **4.5 %** of DB Time (was 71 %), `Lock:transactionid` 14.6 %,
`IO:WalSync` 9.9 %, `LWLock` 3.9 %.

## My prediction was wrong, and by how much

`PREDICTION.md` derived **2394 ms** (margin 1.25x) from the brief's
`fixed + k x records` model and the measured 1.55-1.57x volume increase. The
measurement is **5785 ms** — the prediction was low by **2.4x**, and it put the
result on the safe side of a bound it is in fact well over.

The model's missing term is visible in the table: cost did not scale with
records at a flat per-record rate. Records went up 1.55x; the heavy tabs went
up 2.6-3.6x. The three unmodelled inputs flagged in `PREDICTION.md` all moved
much further than the record count did — Sessions rows 67 -> **245**, distinct
sessions in the trace 67 -> **537**, timeline spans -> 1086, histogram cells
-> 3544 — and the six affected tabs are precisely the ones whose server-side
queries are per-session or per-execution rather than per-record. That is a
hypothesis from this one walk, not a measurement; the separate UI/server branch
should profile it rather than take my word.

## What was NOT done

- The load was **not** reduced, thinned, rate-limited or shortened.
- `BOUND_MS` (3000) was not moved. `paint_analyse.py` was not touched.
- The two render failures were not retried away.
- n=1. The pinned figure is n=5. One walk is enough to establish "over the
  bound by 1.9x with a 2.6-3.6x per-tab regression" and is **not** enough to
  pin a new number; nothing here proposes one.

## Harness correction made during this run

`harness/stamp.py` computed `workload_alive` as `alive == 3` — a magic number
coupled by convention to `box_run_conditions.sh`'s loop body. Adding the burst
tier made the real count four, so this walk was first stamped
`workload_alive=False` and refused despite its own conditions file listing
four alive processes and zero dead. The safe failure direction was luck:
`alive >= 3` would instead have **approved** a walk with the burst tier dead.

Fixed: `box_run_conditions.sh` now prints `procs_expected=N` from the length of
its own process list and `stamp.py` compares against that, with a floor so
`procs_expected=0` cannot satisfy it and an explicit refusal when the value is
present but unparseable. `harness/test_stamp.sh` is the bypass suite (12
cases); run against the old rule it fails 5, **approving two walks with a dead
tier**. `walk-r1.json` records which rule was applied in
`workload_alive_basis` — here the documented legacy fallback, because this
run's conditions file was written before the count line existed.
