Standalone per-class wait CPU measurement, criteria item 2.5

Result: **REFUSED (exit 2), not a clean verdict.** One measured pure-sleep event exceeds the diagnostic threshold. The execution-detail route cannot answer the entire criterion on this capture: 55 lanes truncate, and all 436 `Lock:relation` events are absent from marker-identified executions. A separate event-filtered query confirms `top_events` count 436 versus `executions` count 0 (`relation-coverage.json`). Removing those records from the denominator would conceal the criterion's main sleep class.

Input is the contract's retained `tests/results/demo_rehearsal/trace`, located in the primary checkout because it is ignored and absent from this worktree. This is the **October 2, 2026 PG18 full-mode capture**, not the post-October-6 workload. Its actual window is 1790895946646458457–1790897797661724062 (1851.015265605 s). `dump.txt` (trailing whitespace normalized) confirms 4,307,056 transitions and zero samples. `info` reports the on-disk estimate 4,308,992. These are different server counters; neither is an invented sample count. Input SHA256 is recorded in `measurement.json`. The replay server was compiled from base ac515f6 with an offline ENOSYS process stub, using `build_server.sh`; it loads the capture's event-name sidecar. The old summary is refused by the existing server, which recomputes the census from raw records (`measurement.server.log`). The reported 10 CPUs are the local replay default, not evidence about capture hardware.

Actual measurable classes and statistics follow. Pure-sleep candidates are `Timeout:PgSleep` and every `Lock:*` class, including relation (unmeasured). **PgSleep is present: 326 events. Lock:advisory is absent: 0 events.** This historical capture cannot establish the current advisory-lock workload's behavior. The arithmetic mean of per-event cpu/duration ratios is distinct from total CPU / total duration, also retained in JSON. The max CPU event and max ratio event need not be the same. CPU* gaps are excluded from class figures but checked for unknown CPU. Original record durations and CPU are preserved even when an interval crosses an execution/window boundary; no clipping can hide the CPU outlier. Figures for truncated classes are partial observations, never certified maxima over the capture.

| Measured class | Events | Max CPU µs | Mean CPU µs | Max cpu/dur % | Mean cpu/dur % | >824430 ns |
|---|---:|---:|---:|---:|---:|---:|
| IO:BufFileWrite | 55000 | 1114.733 | 11.258 | 100.000000 | 99.967000 | 1 |
| IO:DataFileExtend | 297 | 152.736 | 42.977 | 100.000000 | 100.000000 | 0 |
| IO:DataFileRead | 16623 | 182.972 | 25.204 | 100.000000 | 96.955653 | 0 |
| LWLock:BufferContent | 17 | 10.860 | 7.380 | 100.000000 | 100.000000 | 0 |
| LWLock:LockManager | 4 | 15.828 | 10.198 | 100.000000 | 73.678885 | 0 |
| LWLock:XactSLRU | 1 | 6.122 | 6.122 | 100.000000 | 100.000000 | 0 |
| LWLock:XidGen | 34 | 22.832 | 7.327 | 100.000000 | 97.254206 | 0 |
| Lock:extend | 2 | 22.122 | 21.120 | 31.975460 | 30.600899 | 0 |
| Lock:transactionid | 590 | 112.622 | 37.597 | 100.000000 | 4.149202 | 0 |
| Timeout:PgSleep | 326 | 923.834 | 37.931 | 0.030753 | 0.002776 | 1 |

`Lock:relation`: 436 capture events, 0 measured; CPU maxima/means/ratios **unknown**, not zero. The 30-class capture census below also includes idle/background and utility/planning work that execution detail cannot serialize. Those classes are not called clean. The pure-sleep count comparison is an additional completeness refusal, not an Off-CPU calculation; issue #386's residual cap is separate and unchanged.

| Capture wait class | Capture count | Measured count |
|---|---:|---:|
| Client:ClientRead | 319033 | 0 |
| IO:BufFileRead | 282040 | 0 |
| IO:BufFileWrite | 281985 | 55000 |
| IO:ControlFileSyncUpdate | 5 | 0 |
| IO:ControlFileWriteUpdate | 5 | 0 |
| IO:DataFileExtend | 300 | 297 |
| IO:DataFileFlush | 26031 | 0 |
| IO:DataFileRead | 16654 | 16623 |
| IO:DataFileSync | 87 | 0 |
| IO:DataFileWrite | 35152 | 0 |
| IO:RelationMapRead | 158 | 0 |
| IO:SlruFlushSync | 5 | 0 |
| IO:SlruWrite | 28 | 0 |
| IO:WalSync | 46919 | 0 |
| IO:WalWrite | 46978 | 0 |
| LWLock:BufferContent | 17 | 17 |
| LWLock:DynamicSharedMemoryControl | 1 | 0 |
| LWLock:LockManager | 5 | 4 |
| LWLock:PgStatsData | 1 | 0 |
| LWLock:SInvalWrite | 1 | 0 |
| LWLock:WALInsert | 4 | 0 |
| LWLock:WALWrite | 1186 | 0 |
| LWLock:XactSLRU | 1 | 1 |
| LWLock:XidGen | 34 | 34 |
| Lock:extend | 2 | 2 |
| Lock:relation | 436 | 0 |
| Lock:transactionid | 590 | 590 |
| Timeout:CheckpointWriteDelay | 14951 | 0 |
| Timeout:PgSleep | 326 | 326 |
| Timeout:VacuumDelay | 101 | 0 |

Boundary CPU is expected by design (`src/compute.c`'s AAS comment around line 676 and the conservation comments around line 1221). These observations cannot distinguish boundary leakage, entry/exit CPU, and wakeup CPU. The measurable sleep classes' typical envelope is **tens of microseconds**, not a claimed few µs: PgSleep median 33.463 µs, p95 52.910 µs, p99 78.799 µs; transactionid median 30.527 µs, p95 65.834 µs, p99 82.443 µs; extend p99 22.122 µs. A small-duration lock has a high ratio even when absolute CPU is tiny; transactionid's ratio mean 4.149% is therefore not a substitute for its 112.622 µs absolute maximum.

The diagnostic threshold is **824430 ns = 10 × max(78799, 82443, 22122) ns**, an order of magnitude above the observed p99 sleep-class envelope and still below the doc's millisecond signature. This is a calibration on one retained capture, not a pre-registered bound and not a new gate. `calibration.json` is the initial threshold-free exploratory run before adding the independent coverage census; its status already refused truncation. `measurement.json` repeats the full walk with the threshold and the additional completeness checks. Neither run claims a clean result.

One pure-sleep event crosses: **PgSleep 923834 ns (923.834 µs)** on pid 1171002 at 1790897311539822175, duration 3004037354 ns, ratio 0.030753%. Its raw execution/detail response is in `outlier.json`. This is approximately millisecond-scale and 11.7× its class p99, so it is a candidate over-attribution signature for follow-up, not proof of a stale label. The same numeric cutoff also catches one BufFileWrite event (1114.733 µs), whose IO label legitimately carries CPU; this is reported but does not count as a sleep signature. Unrepresented/truncated records prevent any assurance that no other outlier exists.

All completeness/refusal counts (units are named in the keys):

| Refusal | Count |
|---|---:|
| window_empty | 0 |
| no_executions | 0 |
| execution_pages_incomplete | 0 |
| truncated_lanes | 55 |
| omitted_lane_events | 1018050 |
| lane_count_mismatch | 0 |
| detail_count_mismatch | 0 |
| null_cpu_events | 0 |
| invalid_events | 0 |
| execution_event_shortfall | 0 |
| empty_eventful_executions | 0 |
| detail_errors | 0 |
| nonexact_responses | 0 |
| execution_count_mismatch | 0 |
| no_pure_sleep_events | 0 |
| server_errors | 0 |
| census_errors | 0 |
| pure_sleep_events_unrepresented | 436 |
| pure_sleep_count_inconsistent | 0 |

The 55 truncated lanes omitted 1,018,050 records in total (waits and CPU gaps); these counts are not silently discarded. The coverage refusal adds 436 unrepresented pure-sleep events. All 16 other named refusal counts are zero. Class exceedance counts above describe only measured records, even when the result is REFUSED.

Walk/accounting counts:

| Quantity | Count |
|---|---:|
| executions_found | 508210 |
| cpu_gap_events | 72894 |
| duplicate_execution_rows | 1047 |
| eventful_executions | 17664 |
| eventless_executions | 490546 |
| execution_detail_requests | 17664 |
| executions_requests | 3067 |
| inferred_execution_ends | 0 |
| open_execution_rows | 0 |
| split_execution_pages | 1518 |
| top_events_requests | 1 |
| unique_lane_events | 145788 |

Execution enumeration uses inclusive, overlapping recursive windows to recover 500-row pages, deduplicating (pid, start_ns). All 1518 page splits recovered; execution_pages_incomplete is 0. The server's execution-row `n_events` counts all attributed transitions, including CPU gaps. The 490546 zero-record executions are explicitly counted without a detail call; the 17664 eventful executions each receive one detail request. Nested and worker event identities are deduplicated independently. There are no open or inferred-end eventful rows in this capture. Null CPU refuses on either waits or CPU gaps. A no-execution/empty window refuses; an eventful zero-span row also refuses rather than skipping silently.

Validation: 15 focused tests passed (`tests.txt`) covering outliers hidden by means, absent PgSleep, null CPU in waits/gaps, truncated worker lanes and omitted counts, missing execution starts, sampled fidelity, count inconsistencies, nested/worker deduplication, eventless and invalid-span executions, page recovery and irrecoverable truncation, and unanchored relation waits. Real empty-window and nonexistent-trace invocations each exited 2, recorded in `empty-window.json` and `missing-trace.json`. No src files changed, no gate was added, and no box-check or rehearsal was run.

Reproduce the measurement (expected exit 2 on this input):

```sh
bash tests/results/perclass-wait-cpu/run_evidence.sh /PATH/TO/tests/results/demo_rehearsal/trace
```

Or build with `build_server.sh`, then invoke `python3 tests/measure_wait_cpu.py --trace-dir DIR --threshold-ns 824430 --output RESULT.json`. Omit the threshold for calibration only; CALIBRATION exits 3 and is never a clean verdict. Optional `--protocol-log FILE` retains every request/response. Do not transfer the diagnostic threshold to a new workload without measuring its boundary CPU envelope. **Item 2.5 remains genuinely blocked for a complete verdict by the execution-start requirement and 2000-event lane cap; the first measured PgSleep outlier also needs investigation.**
