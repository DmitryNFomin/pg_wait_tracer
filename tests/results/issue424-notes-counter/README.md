# #424 — `summary_open_cpu_notes_total`: was the writer ASKED?

## The question this answers

`tests/test_v9_open_cpu_wired.sh` went red on the gate box on 2026-10-28.
The question put to this change was whether its precondition guard fired, or
whether it asserted a zero with its preconditions unmet.

**It fired, and every precondition held.** From
`tests/results/box-check-ubuntu-20261028-121033.log` (header line 2:
`commit=31a03da61af72816584345a8c76c09a40bf022eb`), test banner at line
10820:

```
=== test_v9_open_cpu_wired (#424, summary v9 producer) ===
  ok: watchpoints attached (mask=15, tier=escalated) -- producer reachable
  ok: all seven v9 control keys present and numeric
  ok: 3 of 3 CPU statements completed, spanning 18s of ticks
  ok: stimulus PROVEN -- cpu_ns_total moved 795480753 -> 58758673391
  summary_open_cpu_credited_ns   = 0
  summary_open_cpu_refused_total = 0
  summary_cpu_open_uncredited_ns_total = 0
  summary_open_credited_ns_total = 75854656030  (the discriminator)
=== test_v9_open_cpu_wired: FAIL ===
```

The `uptime_s: 0.42 / events_total: 0 / tier: "sampled"` metrics snapshot
that suggested an empty daemon belongs to **`test_control`** (its banner is
at line 10750 of the same log, and that daemon is `mode=tiered`); it is not
this fixture's daemon, which ran `--mode full` and reached `mask=15`,
`tier=escalated`.

## But the FAIL's verdict was not established

`credited = 0`, `refused = 0`, `uncredited = 0` is **two states**:

| reading | meaning | where the defect is |
|---|---|---|
| never asked | the producer offered no on-CPU observation | the call site, `pgwt_read_state_map` (`src/map_reader.c`) |
| asked, dropped | an observation arrived and the credit path placed none of it | `src/summary_writer.c` |

Nothing on the control socket separated them, so `FAIL: ... the producer is
not wired` was a guess between two defects in two different files. Five
early exits in `pgwt_summary_note_open_ex` can swallow an observation
(`!w->enabled`, `!w->open`, `pid == 0`, the io_worker refusal, a full open
table) and `src/control.c` published none of the counters behind them.

`summary_open_cpu_notes_total` counts on-CPU notes that arrived **carrying an
observation**, before every refusal in that function, so the two readings
are now different numbers. `summary_open_io_worker_refused_total`,
`summary_open_table_full_total` and `summary_open_notes_expired_total`
already existed and were counted internally only; they are published now so
a FAIL can name which exit took it.

## The artifacts

| file | what it shows |
|---|---|
| `mutation-red-suite.sh` / `.log` | six mutants of the new counter, each a way its detection could be unreachable or vacuous; every one turns a named §9 check red. Runs on macOS or Linux, no BPF, no PostgreSQL. |
| `fixture-discrimination.sh` / `.log` | the live fixture driven against a fake control socket, seven scenarios; the branch's fixture gets all seven right. F and G are the gate-cannot-see cases: a daemon that never comes up, and `psql` exiting 127. Both must REFUSE inside the fixture's own bounds. |
| `fixture-discrimination-PARENT-31a03da6.log` | **the red.** The same seven scenarios against the fixture as it stood at `31a03da6`: **4 of 7 fail.** C and E, because it calls "never asked" and "counter absent" both `FAIL: the producer is not wired`. F and G, because six of its refusal sites printed their reason and then exited silently with no `REFUSE` banner — and since `run_all.sh` counts exit 2 exactly like exit 1, a refusal with no banner is indistinguishable in the gate log from a product defect. |
| `control-syntax-check.sh` / `.log` | `src/control.c`, `src/map_reader.c` and `src/summary_writer.c` compile after the edit, with a positive control (a renamed struct member must be rejected). Syntax only, because `make check` on a Mac never builds C — never a substitute for `make box-check`. |

Scenario C is the 2026-10-28 capture: seconds sealed, the writer never
asked. The parent fixture cannot tell it from scenario D (asked 4210 times,
credited nothing) — same verdict, same exit code, same text.

The banner is now emitted from the fixture's `EXIT` trap whenever the status
is 2, so a refusal added later cannot forget it; the one refusal that fires
before the trap is installed carries it explicitly.

## What is NOT established here

Which of the two states the gate box capture actually was. That needs one
box run with these counters in the build, and **there is no box evidence for
this change.** What was measured, rather than assumed: `$PGWT_BOX`,
`$PGWT_BOX_EL8` and `$PGWT_BOX_EL9` were all unset in this agent's
environment, and `pgwt-gate` (root@2.28.45.47) answered at 14:10Z with
`/tmp/pgwt-box-check.lock` held by four PIDs (load 0.69). UNVERIFIED: the
state of `pgwt-gate-2` and the el8 box — no address for either was available
here, so "all three are busy" is not a claim this file makes.

The wiring fix itself is therefore **not in this change** — this change is
what makes the next run name the file to fix instead of guessing between two.
The counters to read on that run, in order: `summary_open_cpu_notes_total`
first, then the three refusal counters, then
`summary_open_cpu_eff_rejected_total` and `summary_open_cpu_overcredit_ns`.
