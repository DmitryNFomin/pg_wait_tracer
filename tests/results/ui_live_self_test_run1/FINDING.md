# Finding: this branch breaks the live-UI-smoke self-test — 2 of 2 runs

**This file previously claimed the failure was a pre-existing ~6 % flake and
"not caused by this branch". That claim was wrong and is retracted below.** It
was written after one failure, on a base rate that did not condition on the one
variable this branch actually moves. The second box-check run and a per-run row
count disprove it.

Nothing is fixed, hardened, widened or added to `KNOWN_FAILING` here.

## What fails

`tests/ui_live_smoke.sh`'s **self-test** (issue #224 bypass suite / #252 review
round 1 finding 3): it injects a synthetic 400 ms blink and confirms the blink
gate still catches it.

    SELF-TEST FAIL: the injected blink was NOT caught and the tab verdict was
    True (self_test={'tick': 2, 'ratio': 0.0, 'measured': True, 'note': None,
    'inject_blink_ms': 400, 'caught': False})

Identical in both runs, so `box-check` is **red on this branch**:

| run | log | result |
|---|---|---|
| 1 | `../box-check-ubuntu-20261008-090532.log` | `Executed: 98 (passed 97, failed 1)` |
| 2 | `../box-check-ubuntu-20261008-101424.log` | `Executed: 98 (passed 97, failed 1)` |

The 11-tab walk that precedes the self-test passed in full in **both** runs
(`overall: PASS`, `failed_tabs: []`) — Waterfall 101 executions, Scatter 2000
points, Matrix 38 cells. Only the self-test fails.

## Why the first "pre-existing flake" reading was wrong

The signature does pre-exist: `top-queries-qsort` 2026-10-01 carries a
byte-identical one. Across all 63 box-check logs on this Mac that ran the
self-test, 4 failed — 6.3 %. That was the number the retracted claim rested on.

But two consecutive failures at p=0.063 is p≈0.004 under independence, so
"same old flake" was never a comfortable reading, and conditioning on the one
thing this branch changes settles it. The self-test walks **only the Sessions
tab**, and the burst tier's four pgbench invocations open 19 fresh client
backends per ~2.5 min cycle, which the Sessions tab counts:

| Sessions rows in the self-test walk | runs | self-test outcome |
|---|---|---|
| 21-25 | 39 | all pass |
| 35, 36, 46 | 3 | all pass |
| 24 | 1 | FAIL (the 2026-10-02 flake) |
| 35 | 1 | FAIL (the 2026-10-01 flake) |
| **146, 148** | **2 (both this branch)** | **both FAIL** |

Every run before this branch sat at 21-46 rows, median 22. This branch runs at
**146-148 — 3.2x the previous maximum, 6.6x the median** — and fails every
time. Reproduce with `selftest_rate.sh` / `sessrows.sh` (committed beside this
file).

So: the mechanism is pre-existing and was already marginal, which is why it
tripped twice in 61 runs by itself. This branch moves the input that governs
it far outside its historical range and makes it fail deterministically. The
honest attribution is **caused by this branch**, on a latent weakness it did
not create.

## Mechanism, from the two runs' own numbers

`summary.json` here (run 1) and the same record from run 2:

| | run 1 | run 2 |
|---|---|---|
| Sessions rows | 148 | 146 |
| `panel_dims.box_height` | 4001 px | 3973 px |
| achieved offsets, inject tick (ms) | 214, 512, 1011, 1514, 2009 | 229, 510, 1011, 1507, 2011 |
| `capture_ms`, inject tick | 232-451 | 258, 351, 356, 267, 350 |
| `frame_spacing` | ok | ok |
| ratios, every tick | all 0.0 | all 0.0 |

Offset drift is **ruled out** — the frames land within ~20 ms of their
200/500/1000/1500/2000 ms targets.

The overlay is on screen for `mount+700` .. `mount+1100`, a 400 ms window.
`_capture_at_offset` reads `Date.now()` *before* calling `page.screenshot`, so
the frame recorded "at 1011 ms" has its pixels grabbed somewhere in
1011..1367 (capture_ms 356), and the frame "at 510" grabs in 510..861. With a
~250-450 ms capture, neither frame reliably lands wholly inside the overlay's
window — and the gate needs a frame WITH the overlay adjacent to one WITHOUT it
to see a difference at all. A taller panel costs more per screenshot, which is
how the row count reaches the outcome.

## The root cause is shared with the paint result

`../demo_paint_20261008/` reports the same branch blowing the 3000 ms paint
bound, and the common factor is not event volume — events rose 1.55x — but
**session churn**: 537 distinct sessions in a 900 s trace against ~67 before,
245 Sessions rows on the paint walk. The six tabs that blew the paint bound are
the ones whose server-side queries are per-session or per-execution. The burst
tier spawns 19 short-lived backends every 2.5 minutes and keeps none.

That points at a fix which does **not** thin the load the owner asked to keep:
make the burst tier REUSE connections — fewer, longer-lived pgbench clients
driving the same statement volume — so the contention and event rate survive
while the backend churn does not. I have deliberately **not** made that change:
it alters the recovered rig's four invocations, which the brief specified
verbatim, and the branch is already stopped on the paint bound. It is a
recommendation for whoever picks that up, not a conclusion.

## What was deliberately NOT done

- No retry inside the test, no widened injection window, no
  `BLINK_THRESHOLD` change, no `KNOWN_FAILING` entry.
- `tests/ui_live_smoke.py` and the capture path are untouched.
- The load was not reduced.
- box-check was run twice. **Both runs are reported and both are red.** The
  re-run was not a fix and the branch is not green.
