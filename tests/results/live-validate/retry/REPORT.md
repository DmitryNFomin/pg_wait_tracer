# Live capture validation retry — 2026-10-12

**Completed a full 900-second live validation. The supplied static warm table is a fair predictor for the six raw commands: live medians range from -14.2% to +3.8%. `time_model` is 98.5% faster because the live v5 capture uses summaries; its 2,985 ms static number is a raw-fallback measurement.**

The frozen copy replays every live request's exact window using the identical server binary. Across all seven commands, live/frozen warm differences range from -8.2% to +5.6%; no live warm regression exceeds 20%. Against the freshly measured original dense trace, the largest live slowdown is `transitions` at +19.5% (25.944 vs 21.704 ms), also below 20%. Cold `transitions` takes 1,591.450 ms; the table's warm value describes repeat requests.

**Cache verdict:** growth preserves reuse. Across the live shared-process warm calls, `decoded` goes 2458→2529, exactly matching the 71 newly cached blocks; `served` goes 1206→23401, while `resets` remains 1. Existing blocks are reused. The initial transitions-canary→raw-primer handoff resets once in both live and frozen runs: live `decoded=1227, resets=0` becomes `decoded=2454, resets=1`; frozen 1208 becomes 2416 with the same reset. That reset occurs before the recorded warm sweep and is not caused by live growth. Source inspection explains it: the aggregate path stores raw blocks without calling `cur_cache_sync`, so the first raw loader's identity check drops that unanchored entry. No source change was made.

**Summary-path finding:** the live summary header is v5; all live `time_model` calls return exact data without raw-load cache diagnostics. The source's `handle_time_model`/`should_use_summaries` conditions therefore identify the summary path. The dense v4 trace explicitly logs the v5-version rejection and raw recomputation. Measured warm medians: live 44.448 ms, frozen v5 44.684 ms, dense v4 3,094.034 ms. This is a behavior comparison of the same server build across its eligible paths.

The daemon and all three workload drivers were alive before the live sweep and at final verification. `current.trace` kept the same inode and grew 40,198,357→42,263,283 bytes during the warm sweep (+2,064,926 bytes). At the live checkpoint, 5,304,320 events had been written; final metrics reported 8,118,272. Ring-buffer drops, state-map overflow, query-id overflow, and summary flush failures remained zero. Every measured response was exact and successful; all six warm canaries were 22.379–28.539 ms against a 32.4 ms ceiling.

Capture and workload restart: 10:04:37 UTC. Measurement completed around 10:30 UTC. Owned processes were stopped and the box lock released by 10:31:28 UTC, within the T+90-minute deadline. The startup readiness race and teardown helper's nonzero `wait` result were handled without changing `src/`, `web/`, or shared test helpers. Only the completed restart's 900-second windows are reported; no 300-second measurements were taken.

Raw values for every measured call, primer, and canary: [timings.csv](evidence/timings.csv). Full JSON responses, timestamps, file signatures, and cache diagnostics: [requests.jsonl.gz](evidence/requests.jsonl.gz). Acceptance checks: [verification.json](evidence/verification.json). Build fingerprints: [binary-sha256.txt](evidence/binary-sha256.txt). The retained live capture and frozen snapshot are on gate-1 under `/root/cx-live-validate-retry-20261012/retry-live` and `/root/cx-live-validate-retry-20261012/retry-frozen-900`.

Branch `agent/cx-live-validate`; source commit `75717896130e60f9518105577d5e4da01dc7c2ee`. Gate-1 (`root@2.28.45.47`, Intel Xeon Skylake). Initially queued on gate-2; moved at 09:59 UTC after checking both queues again. No `src/` or `web/` changes. No VM created.

All timings are milliseconds of local server stdin/stdout request-to-complete-JSON latency. Each warm sweep uses one persistent process, primes outside the measurements, then issues four consecutive calls per command in the specified order. Each cold value comes from the first request in a fresh, unprimed server process, immediately followed by three more calls of that same command; every raw value in those series is reported below. Cold includes startup still in progress; separate spawn-to-response values are also shown. No browser or SSH transport latency is included.

Live requests use the moving latest committed edge while the full-mode daemon and all three workload drivers are alive. Frozen requests replay the exact live `from`/`to` values for every warm and cold call, using a byte-prefix snapshot made after the live sweep. The original dense static trace is measured separately with the identical server binary on the same host, at its original 900-second bounds. `transitions` at 900 seconds is the only canary; full-window `time_model` appears only as a measured command.

## 900-second window

| Command | Live cold | Cold spawn→response | Live calls 1, 2, 3, 4 | Live warm median | Frozen cold | Frozen calls 1, 2, 3, 4 | Frozen median | Live/frozen | Versus supplied dense 900s |
|---|---:|---:|---|---:|---:|---|---:|---:|---:|
| top_queries | 5126.019 | 5137.866 | 4878.310, 4907.168, 4774.829, 4637.114 | 4826.569 | 5412.134 | 4766.388, 4757.655, 4725.502, 5305.663 | 4762.022 | 1.014× (+1.4%) | -3.5% |
| executions | 4832.324 | 4842.318 | 4137.244, 3603.326, 3870.673, 3651.489 | 3761.081 | 4929.059 | 4606.950, 3792.389, 3656.957, 3735.842 | 3764.115 | 0.999× (-0.1%) | -7.5% |
| concurrency | 3869.954 | 3882.811 | 3173.668, 3098.977, 2898.835, 3140.499 | 3119.738 | 3239.743 | 3398.377, 3277.379, 3401.566, 3397.148 | 3397.763 | 0.918× (-8.2%) | -14.2% |
| variants | 3108.787 | 3121.559 | 2956.467, 3078.695, 3143.652, 2798.178 | 3017.581 | 3420.737 | 2929.707, 2855.268, 2846.946, 2860.701 | 2857.984 | 1.056× (+5.6%) | -1.1% |
| exec_scatter | 4583.082 | 4606.068 | 3647.356, 3587.150, 3592.485, 3550.563 | 3589.818 | 4573.551 | 3281.381, 3580.319, 3723.283, 3893.522 | 3651.801 | 0.983× (-1.7%) | -7.6% |
| time_model | 54.503 | 67.546 | 47.715, 41.517, 47.378, 39.883 | 44.448 | 77.479 | 44.757, 41.408, 51.845, 44.612 | 44.684 | 0.995× (-0.5%) | -98.5% |
| transitions | 1591.450 | 1604.530 | 81.406, 20.315, 30.639, 21.250 | 25.944 | 1850.174 | 97.875, 26.644, 24.558, 28.429 | 27.536 | 0.942× (-5.8%) | +3.8% |

Shared-process cache counters after each command (four calls; decoded / served / resets):

| Command | Call 1 | Call 2 | Call 3 | Call 4 |
|---|---|---|---|---|
| top_queries | 2458 / 1206 / 1 | 2464 / 2400 / 1 | 2466 / 3584 / 1 | 2472 / 4758 / 1 |
| executions | 2474 / 5923 / 1 | 2479 / 7085 / 1 | 2481 / 8250 / 1 | 2489 / 9411 / 1 |
| concurrency | 2494 / 10579 / 1 | 2496 / 11750 / 1 | 2499 / 12918 / 1 | 2503 / 14088 / 1 |
| variants | 2505 / 15255 / 1 | 2507 / 16417 / 1 | 2512 / 17574 / 1 | 2513 / 18729 / 1 |
| exec_scatter | 2515 / 19877 / 1 | 2521 / 21025 / 1 | 2522 / 22178 / 1 | 2524 / 23327 / 1 |
| time_model | no raw-load stats | no raw-load stats | no raw-load stats | no raw-load stats |
| transitions | 2529 / 23398 / 1 | 2529 / 23399 / 1 | 2529 / 23400 / 1 | 2529 / 23401 / 1 |

## Original dense trace, same host and binary

| Command | Cold | Cold spawn→response | Calls 1, 2, 3, 4 | Warm median | Supplied baseline | Live 900s / measured dense |
|---|---:|---:|---|---:|---:|---:|
| top_queries | 5489.221 | 5490.693 | 5552.542, 5262.359, 5221.425, 5240.070 | 5251.214 | 5004 | 0.919× (-8.1%) |
| executions | 5401.634 | 5403.771 | 4812.976, 3890.689, 4633.649, 3960.495 | 4297.072 | 4064 | 0.875× (-12.5%) |
| concurrency | 4588.126 | 4591.610 | 3679.094, 3629.363, 4265.458, 3805.431 | 3742.263 | 3636 | 0.834× (-16.6%) |
| variants | 3970.605 | 3972.082 | 3141.320, 3212.967, 3584.632, 3603.389 | 3398.799 | 3050 | 0.888× (-11.2%) |
| exec_scatter | 4956.308 | 4957.513 | 4400.861, 4009.511, 4327.696, 4278.638 | 4303.167 | 3887 | 0.834× (-16.6%) |
| time_model | 3741.469 | 3743.858 | 3220.897, 2967.171, 3425.289, 2923.405 | 3094.034 | 2985 | 0.014× (-98.6%) |
| transitions | 1759.546 | 1761.030 | 24.627, 21.646, 21.761, 21.247 | 21.704 | 25 | 1.195× (+19.5%) |

## Four-call series starting cold, one fresh process per command

These additional series include the cold call already shown above plus all three immediately following calls in that same process. No priming or other command occurs inside a series.

| Trace/window | Command | Calls 1 (cold), 2, 3, 4 | Warm median of 2–4 |
|---|---|---|---:|
| live-900 | top_queries | 5126.019, 4703.002, 4579.068, 4624.971 | 4624.971 |
| live-900 | executions | 4832.324, 3702.150, 3479.596, 3329.166 | 3479.596 |
| live-900 | concurrency | 3869.954, 3503.621, 3277.834, 3428.784 | 3428.784 |
| live-900 | variants | 3108.787, 2784.931, 2596.785, 2767.848 | 2767.848 |
| live-900 | exec_scatter | 4583.082, 3838.619, 4086.599, 3596.632 | 3838.619 |
| live-900 | time_model | 54.503, 46.139, 43.600, 41.761 | 43.600 |
| live-900 | transitions | 1591.450, 25.717, 23.771, 27.500 | 25.717 |
| frozen-900 | top_queries | 5412.134, 4004.826, 4783.426, 4882.359 | 4783.426 |
| frozen-900 | executions | 4929.059, 3678.223, 3661.374, 3605.250 | 3661.374 |
| frozen-900 | concurrency | 3239.743, 3132.343, 3508.595, 3211.963 | 3211.963 |
| frozen-900 | variants | 3420.737, 2757.133, 2904.335, 2956.859 | 2904.335 |
| frozen-900 | exec_scatter | 4573.551, 3472.159, 3856.191, 4039.387 | 3856.191 |
| frozen-900 | time_model | 77.479, 47.394, 46.760, 45.840 | 46.760 |
| frozen-900 | transitions | 1850.174, 34.014, 26.383, 26.002 | 26.383 |
| dense-900 | top_queries | 5489.221, 4980.432, 4807.285, 4834.101 | 4834.101 |
| dense-900 | executions | 5401.634, 4570.645, 4553.373, 4879.941 | 4570.645 |
| dense-900 | concurrency | 4588.126, 4240.159, 3998.219, 4465.181 | 4240.159 |
| dense-900 | variants | 3970.605, 3051.681, 3099.374, 3362.325 | 3099.374 |
| dense-900 | exec_scatter | 4956.308, 4133.699, 3722.407, 4101.875 | 4101.875 |
| dense-900 | time_model | 3741.469, 3266.574, 3307.667, 3175.284 | 3266.574 |
| dense-900 | transitions | 1759.546, 30.470, 26.486, 22.115 | 26.486 |

## Canaries, growth, integrity, and completion

```json
{"cmd": "status", "health": "live-900", "response": "{\"ok\":true,\"mode\":\"full\",\"uptime_s\":921.190588781,\"effective_cpu_capacity_cores\":-1,\"effective_cpu_capacity_source\":\"unknown\",\"backends\":29,\"pg_pid\":1883878,\"version\":\"75717896130e60f9518105577d5e4da01dc7c2ee\",\"pgbackend_layout_source\":\"hard-table\",\"pgbackend_layout_validation\":\"validated\",\"pgbackend_layout_fallback_fields\":0,\"sampled_attr_source\":\"pgbackend_status\",\"tier\":\"escalated\",\"escalation_supported\":false,\"exact_probe_generation\":0,\"exact_probe_attached_mask\":3,\"exact_probe_warmup_reconciled\":false,\"exact_cpu_active\":true,\"exact_query_uprobe_fires_total\":1013351,\"exact_activity_uprobe_fires_total\":706570,\"escalation_seconds_remaining\":0,\"escalation_budget_remaining_s\":0,\"escalation_budget_unlimited\":false,\"escalation_reason\":\"none\",\"sampler_healthy\":true,\"sampler_unhealthy_reason\":\"\",\"cpu_accounting\":\"measured\",\"live_cpu_gate\":\"markers (CMD_START/CMD_END sweep, majority rule)\"}\n"}
```

```json
{"cmd": "metrics", "health": "live-900", "response": "{\"ok\":true,\"uptime_s\":921.1914024,\"effective_cpu_capacity_cores\":-1,\"effective_cpu_capacity_source\":\"unknown\",\"events_total\":5305462,\"events_per_sec\":11640,\"lifecycle_events_total\":458,\"wp_attach_failures_total\":0,\"backends_tracked\":29,\"samples_total\":0,\"samples_per_sec\":0,\"sample_read_faults_total\":0,\"pgbackend_layout_fallbacks_total\":0,\"sampled_attr_tick_read_failures_total\":0,\"sampled_attr_shadow_total\":0,\"sampled_attr_shadow_mismatch_total\":0,\"sampled_attr_shadow_active_total\":0,\"sampled_attr_shadow_active_mismatch_total\":0,\"sampled_attr_shadow_cmd_open_mismatch_total\":0,\"sampled_attr_shadow_query_id_mismatch_total\":0,\"sampled_text_pending\":0,\"sampled_text_resolved_total\":0,\"sampled_text_absent_total\":0,\"sampled_text_evicted_total\":0,\"sampled_text_error_total\":0,\"sampled_text_retry_exhausted_total\":0,\"sampled_text_pgss_scans_total\":0,\"exact_query_uprobe_fires_total\":1013351,\"exact_activity_uprobe_fires_total\":706571,\"exact_probe_attached_mask\":3,\"exact_probe_warmup_reconciled\":false,\"exact_cpu_active\":true,\"exact_probe_generation\":0,\"exact_preseed_entries\":29,\"sample_read_failures_total\":0,\"sample_read_targets\":0,\"sample_read_valid\":0,\"sample_read_invalid\":0,\"sample_read_coverage_pct\":0,\"sampler_ticks_missed_total\":0,\"state_map_full_total\":0,\"seen_query_ids_full_total\":0,\"invalid_wait_reads_total\":0,\"state_reseeds_total\":0,\"sampler_healthy\":true,\"io_worker_busy_pct\":0,\"cpu_accounting\":\"measured\",\"cpu_ns_total\":270366078893,\"offcpu_ns_total\":114351756253,\"cpu_clamped_total\":0,\"wait_gap_cpu_ns_total\":58830426261,\"ring_delta_clamps_total\":0,\"ring_delta_overshoot_ns_total\":0,\"live_cpu_gate\":\"markers (CMD_START/CMD_END sweep, majority rule)\",\"live_cmd_markers_total\":706465,\"live_cpu_unmarked_ns_total\":622604091,\"live_cpu_gate_fallback_total\":0,\"live_query_backfilled_ns_total\":726651218392,\"live_query_unattributed_ns_total\":500805887,\"live_query_pending_overflow_total\":0,\"sampled_idle_in_command_total\":0,\"summary_query_attr_table_full_total\":0,\"summary_query_unattributed_ns_total\":579634621740,\"summary_flush_failures_total\":0,\"summary_events_overflow_total\":0,\"summary_late_events_folded_total\":0,\"io_worker_samples_total\":0,\"io_worker_busy_total\":0,\"noncmd_cpu_samples_total\":0,\"cmd_gate_order_at_risk_total\":0,\"cmd_gate_order_recovered_total\":0,\"cmd_gate_order_read_failed_total\":0,\"cmd_gate_recovered_total\":0,\"ringbuf_drops_total\":0,\"trace_events_written_total\":5304320,\"trace_bytes_written_total\":40093271,\"tier\":\"escalated\",\"escalation_active\":false,\"escalation_seconds_remaining\":0,\"escalation_budget_remaining_s\":0,\"escalation_budget_unlimited\":false,\"escalation_windows_total\":0,\"escalation_denied_total\":0,\"escalation_budget_closed_total\":0,\"anomaly_fires_total\":0,\"anomaly_cpu_saturation_fires_total\":0,\"anomaly_near_total\":0,\"anomaly_dropped_budget_total\":0,\"anomaly_dropped_cooldown_total\":0,\"anomaly_baseline_aas\":0,\"anomaly_cpu_cusum_enabled\":false,\"anomaly_cpu_cusum\":0}\n"}
```

```json
{"capture_info": {"from_ns": 1791799477238017449, "id": 47, "now_ns": 1791800399013979892, "num_cpus": 4, "num_events": 5304320, "protocol": 1, "server_version": "75717896130e60f9518105577d5e4da01dc7c2ee", "to_ns": 1791800398485062373}, "seconds": 900, "summary_header": "5047575305000100"}
```

```json
{"canary": "live-900-pre", "inflated": false, "raw_ms": [1684.313981, 29.063783, 25.232952], "reference_ceiling_ms": 32.4, "warm_median_ms": 27.1483675}
```

```json
{"canary": "live-900-post", "inflated": false, "raw_ms": [20.061786, 24.006405, 21.444109], "reference_ceiling_ms": 32.4, "warm_median_ms": 22.725257}
```

```json
{"after": {"inode": 408012, "mtime_ns": 1791800480650741654, "size": 42263283}, "before": {"inode": 408012, "mtime_ns": 1791800406066745071, "size": 40198357}, "grew": true, "growth": "live-900"}
```

```json
{"canary": "frozen-900-pre", "inflated": false, "raw_ms": [1518.299252, 25.94657, 22.451489], "reference_ceiling_ms": 32.4, "warm_median_ms": 24.1990295}
```

```json
{"canary": "frozen-900-post", "inflated": false, "raw_ms": [29.418498, 30.376572, 26.701003], "reference_ceiling_ms": 32.4, "warm_median_ms": 28.538787499999998}
```

```json
{"after": {"inode": 407073, "mtime_ns": 1791800559290009983, "size": 47617736}, "before": {"inode": 407073, "mtime_ns": 1791800559290009983, "size": 47617736}, "grew": false, "growth": "frozen-900"}
```

```json
{"canary": "dense-900-pre", "inflated": false, "raw_ms": [2155.386483, 26.818479, 25.392075], "reference_ceiling_ms": 32.4, "warm_median_ms": 26.105277}
```

```json
{"canary": "dense-900-post", "inflated": false, "raw_ms": [22.669487, 22.023652, 22.735013], "reference_ceiling_ms": 32.4, "warm_median_ms": 22.379332499999997}
```

```json
{"after": {"inode": 525946, "mtime_ns": 1791453104802295934, "size": 52087149}, "before": {"inode": 525946, "mtime_ns": 1791453104802295934, "size": 52087149}, "grew": false, "growth": "dense-900"}
```

```json
{"cmd": "status", "health": "final", "response": "{\"ok\":true,\"mode\":\"full\",\"uptime_s\":1452.898138551,\"effective_cpu_capacity_cores\":-1,\"effective_cpu_capacity_source\":\"unknown\",\"backends\":29,\"pg_pid\":1883878,\"version\":\"75717896130e60f9518105577d5e4da01dc7c2ee\",\"pgbackend_layout_source\":\"hard-table\",\"pgbackend_layout_validation\":\"validated\",\"pgbackend_layout_fallback_fields\":0,\"sampled_attr_source\":\"pgbackend_status\",\"tier\":\"escalated\",\"escalation_supported\":false,\"exact_probe_generation\":0,\"exact_probe_attached_mask\":3,\"exact_probe_warmup_reconciled\":false,\"exact_cpu_active\":true,\"exact_query_uprobe_fires_total\":1539995,\"exact_activity_uprobe_fires_total\":1075901,\"escalation_seconds_remaining\":0,\"escalation_budget_remaining_s\":0,\"escalation_budget_unlimited\":false,\"escalation_reason\":\"none\",\"sampler_healthy\":true,\"sampler_unhealthy_reason\":\"\",\"cpu_accounting\":\"measured\",\"live_cpu_gate\":\"markers (CMD_START/CMD_END sweep, majority rule)\"}\n"}
```

```json
{"cmd": "metrics", "health": "final", "response": "{\"ok\":true,\"uptime_s\":1452.898791978,\"effective_cpu_capacity_cores\":-1,\"effective_cpu_capacity_source\":\"unknown\",\"events_total\":8119895,\"events_per_sec\":7441,\"lifecycle_events_total\":614,\"wp_attach_failures_total\":0,\"backends_tracked\":29,\"samples_total\":0,\"samples_per_sec\":0,\"sample_read_faults_total\":0,\"pgbackend_layout_fallbacks_total\":0,\"sampled_attr_tick_read_failures_total\":0,\"sampled_attr_shadow_total\":0,\"sampled_attr_shadow_mismatch_total\":0,\"sampled_attr_shadow_active_total\":0,\"sampled_attr_shadow_active_mismatch_total\":0,\"sampled_attr_shadow_cmd_open_mismatch_total\":0,\"sampled_attr_shadow_query_id_mismatch_total\":0,\"sampled_text_pending\":0,\"sampled_text_resolved_total\":0,\"sampled_text_absent_total\":0,\"sampled_text_evicted_total\":0,\"sampled_text_error_total\":0,\"sampled_text_retry_exhausted_total\":0,\"sampled_text_pgss_scans_total\":0,\"exact_query_uprobe_fires_total\":1539995,\"exact_activity_uprobe_fires_total\":1075901,\"exact_probe_attached_mask\":3,\"exact_probe_warmup_reconciled\":false,\"exact_cpu_active\":true,\"exact_probe_generation\":0,\"exact_preseed_entries\":29,\"sample_read_failures_total\":0,\"sample_read_targets\":0,\"sample_read_valid\":0,\"sample_read_invalid\":0,\"sample_read_coverage_pct\":0,\"sampler_ticks_missed_total\":0,\"state_map_full_total\":0,\"seen_query_ids_full_total\":0,\"invalid_wait_reads_total\":0,\"state_reseeds_total\":0,\"sampler_healthy\":true,\"io_worker_busy_pct\":0,\"cpu_accounting\":\"measured\",\"cpu_ns_total\":421619192190,\"offcpu_ns_total\":219478483933,\"cpu_clamped_total\":0,\"wait_gap_cpu_ns_total\":87406637098,\"ring_delta_clamps_total\":0,\"ring_delta_overshoot_ns_total\":0,\"live_cpu_gate\":\"markers (CMD_START/CMD_END sweep, majority rule)\",\"live_cmd_markers_total\":1075761,\"live_cpu_unmarked_ns_total\":799618450,\"live_cpu_gate_fallback_total\":0,\"live_query_backfilled_ns_total\":1149477554459,\"live_query_unattributed_ns_total\":619190553,\"live_query_pending_overflow_total\":0,\"sampled_idle_in_command_total\":0,\"summary_query_attr_table_full_total\":0,\"summary_query_unattributed_ns_total\":1130230832280,\"summary_flush_failures_total\":0,\"summary_events_overflow_total\":0,\"summary_late_events_folded_total\":0,\"io_worker_samples_total\":0,\"io_worker_busy_total\":0,\"noncmd_cpu_samples_total\":0,\"cmd_gate_order_at_risk_total\":0,\"cmd_gate_order_recovered_total\":0,\"cmd_gate_order_read_failed_total\":0,\"cmd_gate_recovered_total\":0,\"ringbuf_drops_total\":0,\"trace_events_written_total\":8118272,\"trace_bytes_written_total\":60173877,\"tier\":\"escalated\",\"escalation_active\":false,\"escalation_seconds_remaining\":0,\"escalation_budget_remaining_s\":0,\"escalation_budget_unlimited\":false,\"escalation_windows_total\":0,\"escalation_denied_total\":0,\"escalation_budget_closed_total\":0,\"anomaly_fires_total\":0,\"anomaly_cpu_saturation_fires_total\":0,\"anomaly_near_total\":0,\"anomaly_dropped_budget_total\":0,\"anomaly_dropped_cooldown_total\":0,\"anomaly_baseline_aas\":0,\"anomaly_cpu_cusum_enabled\":false,\"anomaly_cpu_cusum\":0}\n"}
```

```json
{"complete": true}
```
