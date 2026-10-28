# #424 — why `summary_open_cpu_credited_ns` was 0 on the 2026-10-28 capture

The reading that produced this round, from
`tests/results/box-check-ubuntu-20261028-154345.log` (commit `34a0b822`,
banner at line 10902):

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

## What that capture actually says, and the line in it that settles it

`summary_cpu_open_uncredited_ns_total = 0`.

That counter (`src/summary_writer.c`, `pgwt_summary_push_event`, the "#379
residual" block) adds the WHOLE duration of every closed record whose class
is CPU, whose duration exceeds one second, and which no completed second had
credited. It is incremented outside the `if (o)` slot test, so a pid with no
note counts too. On a capture that burned 34 s of measured CPU it reads
**zero**, which means **not one on-CPU interval longer than a second closed
during the capture**.

So there was nothing for v9 to spread, and 0 was the correct answer. The
stimulus was `SELECT count(*) FROM generate_series(1,60000000)`, and a
set-returning function in FROM is a FunctionScan, which MATERIALISES its
result into a tuplestore — 60 M rows against a 4 MB `work_mem` spills to temp
files, so the backend changes wait event thousands of times a second and
every on-CPU stretch is microseconds long. The fixture proved
`cpu_ns_total` moved; it never proved a gap was held open, and those are
different facts. (The box's PG18 runs `io_method=worker` with 3 io workers,
which is also where `io_worker_refused = 39` comes from.)

`writer-shape-repro.c` / `.log` is that reproduced against the REAL writer,
on this Mac, with no BPF and no PostgreSQL — the live producer's call
sequence (`push_event` → `note_open_ex` → `flush_completed`, the order
`src/daemon.c` uses) under three shapes:

| shape | notes | credited | unplaced | io_worker subset |
|---|---|---|---|---|
| `stable` — one gap held across 12 ticks | 12 | 11 000 ms | 0 | 0 |
| `chatty` — each gap closes inside its own second | 12 | **0** | **12** | 0 |
| `ioworker` — the io_worker rule declines every note | 12 | **0** | 0 | **12** |

`chatty` reproduces the gate capture's signature exactly: notes non-zero,
credited 0, and every refusal counter 0. Before this round the three rows
above were ONE reading.

## What changed

* `src/summary_writer.c` / `.h`: two counters.
  * `open_cpu_io_worker_refused_total` — the on-CPU SUBSET of the io_worker
    refusal. The pre-existing `open_io_worker_refused_total` counts WAIT
    notes too, so 34 notes beside 39 refusals left the overlap unknown and
    "the rule ate every observation" and "it ate none" read identically.
  * `open_cpu_unplaced_total` — an observation ACCEPTED and then retired with
    no second written, counted once per interval at the single site that
    retires one (`open_stop_crediting`, which now takes the writer as a
    parameter so no caller can retire an interval uncounted).
* `src/control.c`: both published.
* `tests/test_v9_open_cpu_wired.sh`: the stimulus is now a plpgsql loop (no
  rows, no tuplestore, no temp file, no wait event — one interval held open);
  the verdict is a decision tree in which every leaf names a file; and the
  PASS leg asserts a NUMBER instead of `credited > 0`.
* `tests/test_open_cpu_accounting.c`: §10, six checks (95 → 101).
* `tests/test_control.sh`: both keys named, so a refactor cannot drop them.

## Evidence in this directory

| file | what it shows |
|---|---|
| `unit-open-cpu-accounting.log` | §10 green, 101 checks, 0 failures |
| `mutation-red-suite.sh` / `.log` | seven mutants, each turning §10 red |
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
| M | FAIL | REFUSE | **the 2026-10-28 capture**: 34 accepted, 34 retired unplaced |

### The mutants

| mutant | the false negative it creates | killed by |
|---|---|---|
| S1 | `cpu_frozen` test dropped → a HEALTHY credited capture reads unplaced > 0 and the live fixture REFUSES it | 10b |
| S2 | the unplaced increment deleted → the fixture FAILs and names `summary_writer.c` again | 10a, 10e |
| S3 | the subset counts WAIT notes too → it stops being a subset and `notes - subset` goes negative | 10c, 10d |
| S4 | the subset increment deleted → the 34-vs-39 overlap is unknown again | 10c |
| S5 | a second, UNCOUNTED retire path used by the close site | 10a |
| S6 | unplaced counted at the NOTE → both sides of the reading come from one event | 10b |
| S7 | the §10 dispatcher line removed from `main()` | nothing: exit stays 0, 101 → 95 checks and the `-- 10:` banner disappears (#686). The banner is printed from inside the section so its absence is the witness. |

## What is NOT established here

Whether the v9 credit path places seconds **on a live capture**. Everything
above is the Mac tier: the real writer driven by a synthetic producer, and
the fixture's decision logic driven by a fake control socket. The live
answer needs `make box-check` with the new stimulus — that run's
`summary_open_cpu_credited_ns` and `summary_open_cpu_measured_ns` are the
first measurement of v9 against a gap that was actually held open.
