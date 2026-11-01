# #424 — why `summary_open_cpu_credited_ns` was 0 on the 2026-10-28 capture

The reading that produced this round, from
`tests/results/box-check-ubuntu-20261028-154345.log` (commit `34a0b822`,
banner reported at line 10902; this source log is not committed, so the
following historical numbers cannot be independently checked here):

```
ok: watchpoints attached (mask=15, tier=escalated) -- producer reachable
ok: all seven v9 control keys present and numeric
ok: stimulus PROVEN -- cpu_ns_total moved 554515748 -> 34958729319
ok: the writer WAS asked -- summary_open_cpu_notes_total = 34
summary_open_cpu_credited_ns   = 0
summary_open_credited_ns_total = 45822328957   (seconds really were sealed)
summary_open_cpu_refused_total       = 0
summary_open_table_full_total        = 0
summary_open_notes_expired_total     = 0
summary_cpu_open_uncredited_ns_total = 0
summary_open_io_worker_refused_total = 39
FAIL: ... the observation reached the writer and the CREDIT PATH placed
      none of it ... All of those zero narrows it to the seal placing no
      second.
```

## What the historical reading suggests

`summary_cpu_open_uncredited_ns_total = 0`.

That counter (`src/summary_writer.c`, `pgwt_summary_push_event`, the "#379
residual" block) adds the WHOLE duration of a closed CPU record longer than
one second when `already == 0`. A zero does not prove that no such interval
closed: a record with prior credit need not increment it. See [#709](https://github.com/DmitryNFomin/pg_wait_tracer/issues/709)
for the counter's double-counting defect; it is not an independent oracle
for whether a stimulus held an interval open.

The historical reading reports about 34 s of measured CPU. The stimulus was
`SELECT count(*) FROM generate_series(1,60000000)` (60 M rows, with a reported
4 MB `work_mem`). FunctionScan tuplestore spills and short on-CPU intervals
are a hypothesis for the zero credit, not established by these aggregate
counters. The fixture proved `cpu_ns_total` moved; it did not prove a gap
was held open. The reported PG18 configuration was `io_method=worker` with
3 io workers and `io_worker_refused = 39`; the missing source log also
prevents checking those numbers here.

`writer-shape-repro.c` / `.log` is that reproduced against the REAL writer,
on this Mac, with no BPF and no PostgreSQL — the live producer's call
sequence (`push_event` → `note_open_ex` → `flush_completed`, the order
`src/daemon.c` uses) under three shapes:

| shape | notes | credited | unplaced | io_worker subset |
|---|---|---|---|---|
| `stable` — one gap held across 12 ticks | 12 | 11 000 ms | 0 | 0 |
| `chatty` — each gap closes inside its own second | 12 | **0** | **12** | 0 |
| `ioworker` — the io_worker rule declines every note | 12 | **0** | 0 | **12** |

`chatty` reproduces the reported aggregate pattern: notes non-zero,
credited 0, and every refusal counter 0. It demonstrates one way to produce
that pattern; it does not identify the cause of the historical capture.

## What changed

* `src/summary_writer.c` / `.h`: two counters.
  * `open_cpu_io_worker_refused_total` — the on-CPU SUBSET of the io_worker
    refusal. The pre-existing `open_io_worker_refused_total` counts WAIT
    notes too, so 34 notes beside 39 refusals left the overlap unknown and
    "the rule ate every observation" and "it ate none" read identically.
  * `open_cpu_unplaced_total` — an interval ACCEPTED and then retired with
    no second written, counted once per interval at the single site that
    retires one (`open_stop_crediting`, which now takes the writer as a
    parameter so no caller can retire an interval uncounted).
* `src/control.c`: both published.
* `tests/test_v9_open_cpu_wired.sh`: the stimulus is now a plpgsql loop (no
  rows, no tuplestore, no temp file, no wait event — one interval held open);
  the unplaced REFUSE verdict reports aggregates without assigning a cause;
  the PASS leg asserts a NUMBER instead of `credited > 0`.
* `tests/test_open_cpu_accounting.c`: §10, six checks (95 → 101).
* `tests/test_control.sh`: both keys named, so a refactor cannot drop them.

## Evidence in this directory

| file | what it shows |
|---|---|
| `unit-open-cpu-accounting.log` | §10 green, 101 checks, 0 failures |
| `mutation-red-suite.sh` / `.log` | seven mutants: six turn §10 red; S7 silently drops it |
| `fixture-discrimination.log` | the new fixture, 13 synthetic scenarios, 13/13 |
| `fixture-discrimination-PARENT-34a0b822.log` | the PARENT fixture against the same 13: **6 wrong** |
| `writer-shape-repro.c` / `.log` | the three shapes above, against the real writer |

The 13-scenario harness itself is
`tests/results/issue424-notes-counter/fixture-discrimination.sh` (extended in
place from 7 scenarios to 13; its own log in that directory was regenerated,
and the `-PARENT-31a03da6.log` beside it is the previous round's 7-scenario
run, kept as history).

### The six the parent fixture gets wrong

| scenario | parent | required | why |
|---|---|---|---|
| H | PASS | FAIL | wall placed with measured 0 — v7's split destroyed |
| I | PASS | FAIL | measured > wall — the reader's residual goes negative |
| J | PASS | FAIL | 2 s measured of a 30 s stimulus: the token credit `credited > 0` approves |
| K | FAIL | REFUSE | the two attribution keys absent — a zero is three states |
| L | FAIL | REFUSE | every observation was an io_worker's; the credit path was offered nothing |
| M | FAIL | REFUSE | synthetic model of the historical capture: 34 accepted, 34 retired unplaced; the historical capture did not publish unplaced |

### The mutants

| mutant | the false negative it creates | killed by |
|---|---|---|
| S1 | `cpu_frozen` test dropped → the credited unit interval incorrectly reads unplaced > 0 (10b); a live REFUSE also requires credited = 0 | 10b |
| S2 | the unplaced increment deleted → the fixture FAILs and names `summary_writer.c` again | 10a, 10e |
| S3 | the subset counts WAIT notes too → it stops being a subset and `notes - subset` goes negative | 10c, 10d |
| S4 | the subset increment deleted → the 34-vs-39 overlap is unknown again | 10c |
| S5 | a second, UNCOUNTED retire path used by the close site | 10a |
| S6 | unplaced counted at the NOTE → both sides of the reading come from one event | 10b |
| S7 | the §10 dispatcher line removed from `main()` | nothing: exit stays 0, 101 → 95 checks and the `-- 10:` banner disappears (#686). The banner is printed from inside the section so its absence is the witness. |

## THE LIVE ANSWER (`live-fixture-gate3-20261028.log`)

The fixture, run against the real PG18 cluster on `pgwt-gate-3` with the
clock-sized stimulus. This is the first measurement of v9 against an on-CPU
gap that was actually held open:

```
ok: 3 of 3 CPU statements completed, spanning 9s of ticks
ok: stimulus PROVEN -- cpu_ns_total moved 10591 -> 26431536953
ok: the writer WAS asked -- summary_open_cpu_notes_total = 27
summary_open_cpu_credited_ns   = 23465838422
summary_open_cpu_measured_ns   = 22853487355
summary_cpu_open_uncredited_ns_total = 18194860320
summary_open_cpu_io_worker_refused_total = 0  (the on-CPU subset)
summary_open_cpu_unplaced_total      = 0
ok: the v9 open-CPU producer IS wired and the credit is coherent
    (measured/stimulus = 86%, floor 25%)
=== test_v9_open_cpu_wired: PASS ===
```

The hand-run establishes live credit: 23.47 s of wall and 22.85 s of
measured CPU, 86% of the daemon's schedstat delta. Its `uncredited` reading
is 18.19 s, versus the historical report's 0; that difference does not
independently establish the cause of the earlier zero credit.

The two new counters are both 0 on this hand-run. However,
`summary_open_cpu_unplaced_total` is **11** in the passing
`box-check-MERGE-eecbdfe3.log` and **13** in the passing
`box-check-FINAL-c2e05b45.log`; the io_worker CPU subset is **0** in both.
`unplaced` counts accepted intervals retired without open-CPU credit. It
separates that outcome from io_worker refusal in the synthetic shapes
(`chatty`: unplaced 12, subset 0; `ioworker`: unplaced 0, subset 12), but its
being non-zero does **not** distinguish a healthy capture from an unhealthy
one or prove that the stimulus failed. A general discrimination claim is
unsupported by these captures.

## The observed spread in `uncredited` (four reported runs; mismatch n = 1)

The reported readings span 9.02 s to 18.19 s. The first two credited
readings differ by about 0.534 s (0.534161578 s using the reported 24 s and
the committed hand-run value). The table preserves all four reported runs:

| run | tree | credited | uncredited | mismatch | measured/stimulus |
|---|---|---|---|---|---|
| hand-run, gate-3 | af5e9130 | 23.466 s | 18.195 s | not published | 86% |
| box-check, gate-3 | af5e9130 | 24.000 s | 9.019 s | not published | 61% |
| box-check, VM | eecbdfe3 | 25.805 s | 9.068 s | not published | 60% |
| box-check, VM | c2e05b45 | 24.648 s | 18.042 s | **2** | 64% |

The hand-run, MERGE and FINAL values above agree with their committed
logs (seconds rounded to three decimals; percentages truncated by the
fixture). The gate-3 box-check row (24.000 s, 9.019 s, 61%) has no committed
source log in this tree, so those three readings remain unverified. The
hand-run log also does not publish its tree id; `af5e9130` is reported
provenance for the two gate-3 rows, not independently verified by these logs.

Only FINAL publishes `summary_open_note_mismatch_total`: **2**, beside
18.042 s uncredited and 24.648 s credited. This is **n = 1** for the joint
observation. The mismatch count is capture-wide; it does not identify two
of the three stimulus intervals. Nor do the earlier ~9 s readings establish
one mismatch each. Those explanations remain hypotheses.

[#709](https://github.com/DmitryNFomin/pg_wait_tracer/issues/709) tracks both
the `cpu_open_uncredited` double-tell and the unexplained close-time
identity-check refusals. The defect pre-exists on master; this branch wires
the producer and makes it observable. It is not fixed here.

The previous proposed mechanism (a mismatch falls back to a watermark
"normally 0") is not established: the add requires `already == 0`, which
on a mismatch requires `credited_until_ns <= start`. The watermark is
monotone and reset only by `pgwt_summary_forget_open`. The aggregate
readings do not establish that sequence. Publishing mismatch alongside
uncredited supplies context, not a correction or proof of comparability
across captures. Nothing in the gate's assertions depends on uncredited.

The **25% floor remains an owner design question**: the reported observed
band is **60–86%, n = 4**, a **26 pp** spread; **25%** is **2.4x** below the
observed minimum. It is empirical, not derived from the 3 s batch floor,
and remains unchanged. One of the four readings lacks a committed source
log, as noted above.

Also worth recording because the guard earned it: the FIRST version of the
plpgsql stimulus used a fixed `FOR i IN 1..40000000`, which that box runs in
2 s, and the fixture REFUSED on its own `>= 3 s` floor instead of approving a
zero (reported in `box-check-ubuntu-20261028-171149.log`, also not committed,
so the 2 s reading is unverified). The loop is now sized by a wall-clock
deadline.
