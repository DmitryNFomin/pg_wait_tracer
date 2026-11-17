Recorded file-decoding regression evidence (Refs #866)

`el8-4.18-file.script.txt` is an unedited contiguous prefix of
`tmp/p866-candidate/pg13-far-candidate/perf.trace` from
`tests/results/session-20261116/p866-pg13-live2/el8/ev.tgz` on the lead's Mac.
The complete recording is 1,149,450 bytes; this 39,569-byte slice retains
51 tick markers and every intervening readv entry/exit (50 complete ticks,
150 syscall pairs). It ends immediately after marker 51.

Full recording SHA256: `6aa7cba87e87fd1a86b73efa5ad2e2e0591eef949ec908bd41a2b5650cbc1064`.
Slice SHA256: `28b0d35b1ccb4fe6f7748460fc7c7306f68e7b3ffb5c8b8ae75e44e0c89bb78d`.
Kernel/perf: EL8 4.18; tick event `pgwt866_6907_7473:tick`, sampler tid 7473,
postmaster 6925, wait reader 6928. Each complete tick has two postmaster
status calls and one wait-shaped call (492 iovecs, 1968 returned bytes).
There are zero possible discovery calls: the original live run failed its
pipe-readiness check before opening the far-row workload. The fixture proves
decoding and steady-state call shape; it cannot qualify the churn gate.

`ubuntu-6.8-ticks.script.txt` is all 20 unedited lines from
`tests/results/session-20261116/p866-pg13-live2/ubuntu/raw20.txt` on the lead's
Mac, 1,320 bytes. Kernel 6.8, `perf script --ns` with padded columns.
SHA256: `58da02e1ca0f31277b50a264b8a58d172d4a538841e1582e4dc0da42ca3bbf5c`.
These are tick lines only, not Ubuntu syscall evidence.

Portable regression: `python3 -B tests/test_sampler_batch_summary.py`
(`recorded_file_vm_cases`, `file_capture_cases`, `pg13_selection_cases`).

PG13 statement/tick qualification (Refs #866)

The far-row arm records one statement per churn connection: PID, BackendId,
`statement_timestamp()` (E), and `pg_sleep(0.05)`. `churn.starts.json` retains
E in wall-clock nanoseconds. The perf recorder explicitly selects
`--clockid mono`, and the sample reader retains `monotonic_ns` as well as
the file-anchor-derived `timestamp_ns`. Only raw monotonic time selects a
sample's tick. Tick intervals start at `pgwt_sampler_poll` symbol entry and
end at the next marker; the last marker closes the preceding interval.

`clock-alignment.json` retains independent clock probes: a `CLOCK_REALTIME`
read between two `CLOCK_MONOTONIC` reads, before/after capture and each churn
statement. Each probe bounds realtime-minus-monotonic by
`[wall - mono_after, wall - mono_before]`. The gate uses the entire observed
offset envelope, including drift. Both endpoint conversions of E must select
the same first tick T starting at or after E. Missing probes, an ambiguous
T, and evidence outside closed tick intervals
fail qualification. The offset is never inferred from successful attribution.

All 200 PIDs need a nonzero-query-id sample at T or earlier; T+1 is RED.
There is no nominal-period or elapsed-time bound. The churn counter window
starts before the first connection and ends when the last one retires from
enrollment. `churn.metrics.json` retains both counter snapshots. Every failure
must have a daemon outcome keyed by PID, `/proc` start-time incarnation, raw
monotonic tick stamp and phase (`first` or `gate`). The cumulative failure
sequence must reconcile exactly with that counter window; gate outcomes also
reconcile with `cmd_gate_order_read_failed_total`. Missing, duplicated or
malformed evidence is RED.

The existing `PGWT_DEBUG_SAMPLED_ATTR_SHADOW` was checked at every use in
`src/sampler.c`: it controls diagnostic printing, but its invalid-row message
has only PID/errno and it does not print failed gate re-reads. It cannot supply
this evidence. The arm instead enables `PGWT_DEBUG_SAMPLED_ATTR_FAILURES`,
off by default and read once at startup. Its wrapper makes the same batch call
with the same arguments and counter, copies cached incarnation before a reader
can be retired, brackets that call with monotonic timestamps, prints one line
per counted failure, and preserves errno. It adds no target reads or retries.
The portable C case `sampled_attr_failure_diagnostic_opt_in_preserves_outcomes_errno_and_incarnation`
compiles that production wrapper with injected I/O and checks both phases,
retired identities, unchanged results/counters/errno, and zero diagnostic clock
reads when disabled.

The new (d) checks have no elapsed-time allowance or offsetting between classes:

- (d1) Every cohort PID has a persisted sample in every observed tick between
  its first and last sample. A hole is independently RED.
- (d2) An individually identified cohort failure is pre-eligibility only before
  that PID's T. It is terminal only when its read **begins** at/after an observed
  statement return or EXIT. The PQexec return stamp is a conservative upper
  bound on statement end; a read overlapping it stays unexplained. EXIT must
  have an unambiguous recorded fork for the same PID/incarnation. Cached row
  invalidation or errno alone does not prove a cleared/reused row; this
  diagnostic does not expose row contents, so those cases require end/EXIT
  evidence. No inferred exit allowance is granted.
- (d3) Eligible cohort failures and all non-cohort failures are RED. Incarnation
  mismatches and unclassified outcomes are separately counted and RED. Unused
  pre-eligibility or terminal exemptions cannot cancel any of them. The summary
  retains every classified outcome and counts each class and phase separately.

The perf recorder uses system-wide mode for `task:task_newtask` (fork; the
parent is perf's tid column) and `sched:sched_process_free` (EXIT, never
earlier than the real exit): `-p daemon` alone cannot record postmaster forks.
`sched:sched_process_fork`/`exit` are unusable: the tracer under test attaches
its own `tp/sched` programs there, and a BPF tracepoint program returning 0
drops every perf sample of that tracepoint (EL8 4.18 recorded zero of each with
the daemon running and four of each without it).
Readv and tick events retain filters for all existing daemon threads. Every
scheduler loss is subject to the same lossless rule as readv/ticks. Each cohort
PID requires exactly one observed postmaster fork before E; the summary's
`late_backend_timeline` reports fork monotonic time, E wall time and monotonic
bounds, T, first attribution tick, and extra ticks for every late PID.
Computed route, zero scan fallbacks, at most two discovery calls per tick,
zero quiet-window failures, and rejection of every perf loss remain required.
The pre-fix control qualifies only with valid timing evidence and an explicit
missing/late attribution RED; clock or counter failures cannot substitute.

Every run retains its evidence directory, including default `/tmp` directories
and failed runs. Inspect `churn.starts.json`, `churn.samples.json`,
`perf.script.txt`, `clock-alignment.json`, `churn.metrics.json`, and
`churn.failures.json`, `lifecycle.json`, and `summary.json` together. A setup failure before churn cannot qualify a run.
Mac cases `pg13_at_T_passes`, `pg13_at_T_plus_1_RED`,
`pg13_missing_attribution_RED`, `pg13_alignment_unavailable_or_ambiguous_RED`,
and `pg13_unlocated_failures_RED` run on a synthetic jittered
cadence and both verbatim EL8/Ubuntu tick fixtures above. Their E/sample
witnesses are hand-placed synthetic inputs; the historical live runs did not
record statement E and cannot retrospectively qualify this gate.
Additional portable cases cover eligible/background RED, holes, both phases,
incarnation mismatch, terminal evidence, counter reconciliation, non-offsetting
classes, and late-PID timelines. See `../pg13_live5/README.md` for the newer
recorded E/sample regression data.

Lead box commands, from the built candidate tree on each PG13 box, with the
unmodified `0b1d7dd6` tracer built at `/tmp/0b1d7dd6/pg_wait_tracer`:

```sh
# Ubuntu 6.8
sudo python3 -B tests/test_sampler_batch_live.py --pg13-far-rows --master-tracer /tmp/0b1d7dd6/pg_wait_tracer --require-negative-control --evidence-dir /tmp/p866-failure-gate-ubuntu-$(date -u +%Y%m%dT%H%M%SZ)
# EL8 4.18
sudo python3 -B tests/test_sampler_batch_live.py --pg13-far-rows --master-tracer /tmp/0b1d7dd6/pg_wait_tracer --require-negative-control --evidence-dir /tmp/p866-failure-gate-el8-$(date -u +%Y%m%dT%H%M%SZ)
```

Each command runs the candidate and pre-fix control with the same gate and
retains their evidence in separate subdirectories. These commands require
Linux/root and must not run on macOS. Add `--require-huge-pages` only for the
separate huge-pages qualification.
