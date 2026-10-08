# Paint prediction from the measured volume change — DERIVED, NOT MEASURED

This file exists so the prediction can be checked against the real walk when
it lands, and so nobody mistakes it for the walk. **Every number here is
arithmetic on someone else's measurement.** The only measured input that is
mine is the event rate in `../demo_mix_20261008/`.

## Inputs

| input | value | source | measured by me? |
|---|---|---|---|
| worst-case tab paint, pinned | 1616 ms (n=4 per-run worst; median 1538.5) | `../demo_paint_20261007/SUMMARY.txt` | no |
| bound | 3000 ms | `docs/DEMO_REHEARSAL_CRITERIA.md` | no |
| pinned event rate | 3854 events/s | `../demo_paint_20261007/SUMMARY.txt` run r1 | no |
| pinned in-window records (900 s, derived) | 3.47 M | same | no |
| fixed (non-record-dependent) part of a paint | ~200 ms render | the brief's own model | no |
| scaling model | paint = fixed + k x records, flat per record over a 14x range | the brief | no |
| **new event rate** | **6050 events/s** (5 668 864 events / 936.885 s) | `../demo_mix_20261008/mix.txt`, `raw.json` `info` | **yes** |
| **new in-window records (900 s, derived)** | **5.44 M** (6050 x 900) | same, same derivation as the pinned figure | **yes** |

Volume ratio: 6050 / 3854 = **1.57x**.

## The arithmetic

The brief states its own model's output: *"If the record-dependent part of
1616 ms scales 2.6x, paint lands near 3800 ms."* Solving that for the split:

    record_part x 2.6 + fixed = 3800
    record_part x 1.0 + fixed = 1616
    => record_part x 1.6 = 2184  =>  record_part = 1365 ms, fixed = 251 ms

(`fixed = 251 ms` agrees with the brief's independent "~200 ms render", which
is the only cross-check available on this split.)

At the measured 1.57x:

    1365 x 1.57 + 251 = 2394 ms      margin 3000 / 2394 = 1.25x

Carrying the pinned run-to-run spread through the same scaling (1388..1616,
i.e. -9.6 %/+5.1 % about the median 1485):

    lower 2224 ms (1.35x)   upper 2591 ms (1.16x)

## Why this is not good enough to act on

Three things push the real number ABOVE this estimate, and the model has no
term for any of them:

1. **The worst tab's own row count doubled.** Top Queries went from 22 to 42
   rows (`../demo_mix_20261008/mix.txt`), and `queries` was the worst tab in
   three of the five pinned walks. Record count is not the only input to that
   tab's cost.
2. **Transitions went from 5 nodes to 46** (50 links, `total_link_count` 90,
   `truncated` true). That is a render-side cost the "records x k" term does
   not model at all.
3. **Sessions went from ~67 rows to 265.** Cheap per row on the pinned
   numbers (170-264 ms for the whole tab), but it is another input moving in
   the wrong direction.

One thing pushes it below: `Lock:relation` no longer occupies one backend
continuously, so the per-session timeline is less lopsided. No reason to think
it offsets the three above.

## Verdict on the prediction alone

2394 ms predicted, margin 1.25x, against 1.86x before. Under the 3000 ms bound
but with roughly a third of the previous headroom, and with three unmodelled
terms all pointing the wrong way. By the brief's own rule — *"If paint exceeds
or approaches 3000 ms: STOP and report"* — 1.25x with unmodelled upside risk
is close enough to "approaches" that the real walk is the only acceptable
evidence. `harness/run_chain.sh` is that walk; see `verdict-r1.txt` for its
result.
