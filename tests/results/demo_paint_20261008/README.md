# Click-to-painted latency, 2026-10-08 — STILL OVER THE BOUND after connection reuse

**STOP condition reached a second time.** Two measured walks in the demo's own
topology on gate-1:

| | r1 (short-lived burst clients) | r2 (connection reuse) | bound |
|---|---|---|---|
| worst single navigation | 5785 ms (queries) | **5504 ms (queries)** | 3000 ms |
| margin | 0.52x | **0.55x** | 1.0x |
| tabs over the bound | 5 of 11 | **6 of 11** | — |
| tabs that failed to render ≥1 nav | 2 (concurrency, transitions) | **2 (the same two)** | — |

Connection reuse did what it was built to do and **did not fix paint.** Per the
instruction not to iterate toward the bound, this stops here.

## The diagnosis this run REFUTES — mine

After r1 I concluded the driver was **session churn**, not event volume, and
that conclusion was accepted. r2 was the experiment that should have confirmed
it. It did the opposite.

r2 holds event volume constant by construction (durations and pauses scale by
the same factor, so average tx/s is identical — asserted arithmetically and
behaviourally in `tests/test_burst_load_guards.sh`) and cuts session churn:

| quantity | r1 | r2 | change |
|---|---|---|---|
| burst `b_wal` invocations in the capture | 18 | 4 | **−78 %** |
| distinct sessions in the trace | 537 | 238 | **−56 %** |
| Sessions tab rows in the window | 245 | 109 | **−56 %** |
| events / s | 5987 | 6236 | +4 % |
| **worst tab paint** | **5785 ms** | **5504 ms** | **−5 %** |

Session churn more than halved; paint moved 5 %, which is inside the
run-to-run spread the pinned n=5 set already showed (1388–1616, ±8 %). Per tab,
r1 → r2: queries −5 %, transitions −7 %, concurrency −2 %, scatter +1 %,
waterfall +4 %, matrix +9 %. That is noise, not a response.

**So paint is not driven by session count.** The brief's original hypothesis —
in-window event/execution volume — was closer than my refinement of it.

Where my reasoning went wrong, precisely: I treated "the five light tabs sit at
0.68–1.19x while the six heavy ones sit at 2.6–3.6x" as a control that
*discriminated* between volume and churn. It does not. The light tabs read
summary files and are insensitive to **both** quantities, so that split is
equally consistent with either hypothesis. It ruled out the cross-box confound,
which is all I should have claimed for it.

## What the evidence now supports

Against the pinned baseline (3854 events/s → 1616 ms worst):

    events/s   3854 -> 6236   = 1.62x
    worst ms   1616 -> 5504   = 3.41x

Superlinear in volume, and the six affected tabs are the ones whose server-side
queries walk executions or aggregate the whole window
(`top_queries`, `executions`, `exec_scatter`, `concurrency`, `transitions`,
`matrix`), while the five that read summaries are flat. The lever that would
move paint is therefore in-window event/execution volume — which the owner has
ruled out reducing. That makes this unambiguously a UI/server problem, now with
a second data point and a refuted alternative rather than one measurement and a
guess.

Two further facts for whoever takes that branch:
- `concurrency` and `transitions` each failed to render within the 15 s
  readiness timeout on one of three navigations, **in both runs**. Reproducible,
  not a fluke, and `paint_analyse.py` correctly refuses both walks for it
  (`verdict-r1.txt`, `verdict-r2.txt`) rather than taking a max over the tabs
  that happened to work.
- `matrix` crossed the bound in r2 (3207 ms) having been 51 ms under it in r1.

## Why the connection-reuse change is kept anyway

It is honest about what it achieves: 6x fewer backend generations for identical
average load, 238 sessions instead of 537, 109 Sessions rows instead of 245. It
is strictly better hygiene on a shared box and it is the only lever available
for the second blocker (the live-UI-smoke blink self-test, which failed 2/2 at
146–148 Sessions rows while all 59 historically passing runs sat at 21–46). At
109 rows that gate may or may not recover — `box-check` on this tree is what
answers it, and its result is reported separately.

## Conditions (both runs, `conditions-r1.txt` / `conditions-r2.txt`)

Real Chrome 154.0.8037.100 on this Mac, viewport 1710x981 @ DPR 2 confirmed by
two agreeing reads, 15m range asserted → `web/pgwt` on the Mac → `pgwt-server`
over ssh on gate-1 → fresh empty trace dir, `--mode full`, PostgreSQL 18,
`shared_buffers` 128 MB. r2: capture age 1110 s, loadavg 1.13/0.99/0.93 on 4
cores, all four load tiers alive (`procs_expected=4`, 0 dead), burst tier zero
`ABORT` lines, `fidelity: exact`, 0 console errors.

r2's scraped time model still shows the profile the owner asked for:
`Lock:relation` 4.6 %, `Lock:transactionid` 14.6 %, `Lock:advisory` 10.9 %,
`IO:WalSync` ~10 %, `LWLock` 3.9 %, AAS 0.90. The shape was not sacrificed.

## What was NOT done

- The load was not thinned. Rates, client counts, WAL volume, lock-manager
  fanout and the IO batch are untouched; only backend lifetime changed.
- `BOUND_MS` (3000) was not moved; `paint_analyse.py` was not touched.
- The two render failures were not retried away.
- No third attempt. n=1 per configuration, against a pinned n=5 — enough to say
  "still over by ~1.8x and unmoved by reuse", not enough to pin a number, and
  none is proposed.
