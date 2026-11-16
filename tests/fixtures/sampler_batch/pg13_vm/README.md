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
enrollment. `churn.metrics.json` retains both counter snapshots. Its
`sampled_attr_tick_read_failures_total` delta must not exceed the number of
distinct observed `(pid, tick)` samples whose tick starts before that PID's E,
including query-id-zero samples. This uses the perf marker, even if E falls
between that marker and the sampler's timestamp read. The summary reports both quantities.
Computed route, zero scan fallbacks, at most two discovery calls per tick,
zero quiet-window failures, and rejection of every perf loss remain required.
The pre-fix control qualifies only with valid timing evidence and an explicit
missing/late attribution RED; clock or counter failures cannot substitute.

Every run retains its evidence directory, including default `/tmp` directories
and failed runs. Inspect `churn.starts.json`, `churn.samples.json`,
`perf.script.txt`, `clock-alignment.json`, `churn.metrics.json`, and
`summary.json` together. A setup failure before churn cannot qualify a run.
Mac cases `pg13_at_T_passes`, `pg13_at_T_plus_1_RED`,
`pg13_missing_attribution_RED`, `pg13_alignment_unavailable_or_ambiguous_RED`,
and `pg13_churn_failures_above_pre_E_ticks_RED` run on a synthetic jittered
cadence and both verbatim EL8/Ubuntu tick fixtures above. Their E/sample
witnesses are hand-placed synthetic inputs; the historical live runs did not
record statement E and cannot retrospectively qualify this gate.

Lead box commands, from the built candidate tree on each PG13 box, with the
unmodified `0b1d7dd6` tracer built at `/tmp/0b1d7dd6/pg_wait_tracer`:

```sh
# Ubuntu 6.8
sudo python3 -B tests/test_sampler_batch_live.py --pg13-far-rows --master-tracer /tmp/0b1d7dd6/pg_wait_tracer --require-negative-control --evidence-dir /tmp/p866-tick-gate-ubuntu-$(date -u +%Y%m%dT%H%M%SZ)
# EL8 4.18
sudo python3 -B tests/test_sampler_batch_live.py --pg13-far-rows --master-tracer /tmp/0b1d7dd6/pg_wait_tracer --require-negative-control --evidence-dir /tmp/p866-tick-gate-el8-$(date -u +%Y%m%dT%H%M%SZ)
```

Each command runs the candidate and pre-fix control with the same gate and
retains their evidence in separate subdirectories. These commands require
Linux/root and must not run on macOS. Add `--require-huge-pages` only for the
separate huge-pages qualification.
