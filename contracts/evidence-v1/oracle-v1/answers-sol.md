## 01_ordinary_run

Projection: `{"life":[0,null],"coverage":[[1,15]],"waits":[["PgSleep",3,7],["Lock",9,12]],"open":null}`
Selected candidate: A/101/0, run anchored at 1, certified through 15.

```jsonl
{"metric":"wait_time","version":1,"value":4,"unit":"ns","basis":"observed","completeness":"complete","reasons":[],"detail":{"closed_ns":4,"open_ns":0},"diagnostic":{"recorded_ns":4,"covered_ns":14,"required_ns":14}}
{"metric":"completed_waits","version":1,"value":1,"unit":"count","basis":"observed","completeness":"complete","reasons":[],"diagnostic":{"recorded_count":1,"covered_ns":14,"required_ns":14}}
```

## 02_pid_reuse_delayed_life

Projection: `{"life":[12,null],"coverage":[[13,20]],"waits":[],"open":["Lock",15,20]}`
Selected candidate: A/101/12, run anchored at 13; delayed LIFE exit at 8 belongs to fork 0.

```jsonl
{"metric":"wait_time","version":1,"value":5,"unit":"ns","basis":"observed","completeness":"complete","reasons":[],"detail":{"closed_ns":0,"open_ns":5},"diagnostic":{"recorded_ns":5,"covered_ns":7,"required_ns":7}}
{"metric":"completed_waits","version":1,"value":0,"unit":"count","basis":"observed","completeness":"complete","reasons":[],"diagnostic":{"recorded_count":0,"covered_ns":7,"required_ns":7}}
```

## 03_exit_inside_window

Projection: `{"life":[0,10],"coverage":[[1,10]],"waits":[["PgSleep",3,10]],"open":null}`
Selected candidate: Run anchored at 1; matching terminal transition certifies exit 10.

```jsonl
{"metric":"wait_time","version":1,"value":5,"unit":"ns","basis":"observed","completeness":"complete","reasons":[],"detail":{"closed_ns":5,"open_ns":0},"diagnostic":{"recorded_ns":5,"covered_ns":5,"required_ns":5}}
{"metric":"completed_waits","version":1,"value":1,"unit":"count","basis":"observed","completeness":"complete","reasons":[],"diagnostic":{"recorded_count":1,"covered_ns":5,"required_ns":5}}
```

## 04_window_starts_at_exit

Projection: `{"life":[0,10],"coverage":[[1,10]],"waits":[["PgSleep",3,10]],"open":null}`
Selected candidate: Run anchored at 1; exit 10 is certified even though the query has empty lifetime intersection.

```jsonl
{"metric":"wait_time","version":1,"value":0,"unit":"ns","basis":"observed","completeness":"complete","reasons":[],"detail":{"closed_ns":0,"open_ns":0},"diagnostic":{"recorded_ns":0,"covered_ns":0,"required_ns":0}}
{"metric":"completed_waits","version":1,"value":1,"unit":"count","basis":"observed","completeness":"complete","reasons":[],"diagnostic":{"recorded_count":1,"covered_ns":0,"required_ns":0}}
```

## 05_known_exit_lost_terminal

Projection: `{"life":[0,12],"coverage":[[1,8]],"waits":[],"open":["PgSleep",3,8]}`
Selected candidate: Run anchored at 1, frozen at FRONTIER 8; LIFE retains exit 12 without a terminal close.

```jsonl
{"metric":"wait_time","version":1,"value":null,"unit":"ns","basis":"observed","completeness":"unavailable","reasons":["coverage_incomplete"],"diagnostic":{"recorded_ns":2,"covered_ns":2,"required_ns":6}}
{"metric":"completed_waits","version":1,"value":null,"unit":"count","basis":"observed","completeness":"unavailable","reasons":["coverage_incomplete"],"diagnostic":{"recorded_count":0,"covered_ns":2,"required_ns":6}}
```

## 06_preexisting_before_capture

Projection: `{"life":[0,null],"coverage":[[12,20]],"waits":[],"open":["PgSleep",14,20]}`
Selected candidate: Provisional generation, run anchored at 12; lifetime lower bound remains 0.

```jsonl
{"metric":"wait_time","version":1,"value":null,"unit":"ns","basis":"observed","completeness":"unavailable","reasons":["coverage_incomplete"],"diagnostic":{"recorded_ns":6,"covered_ns":8,"required_ns":18}}
{"metric":"completed_waits","version":1,"value":null,"unit":"count","basis":"observed","completeness":"unavailable","reasons":["coverage_incomplete"],"diagnostic":{"recorded_count":0,"covered_ns":8,"required_ns":18}}
```

## 07_preexisting_covered_window

Projection: `{"life":[0,null],"coverage":[[12,20]],"waits":[],"open":["PgSleep",14,20]}`
Selected candidate: Provisional generation, run anchored at 12; the whole query is covered.

```jsonl
{"metric":"wait_time","version":1,"value":6,"unit":"ns","basis":"observed","completeness":"complete","reasons":[],"detail":{"closed_ns":0,"open_ns":6},"diagnostic":{"recorded_ns":6,"covered_ns":8,"required_ns":8}}
{"metric":"completed_waits","version":1,"value":0,"unit":"count","basis":"observed","completeness":"complete","reasons":[],"diagnostic":{"recorded_count":0,"covered_ns":8,"required_ns":8}}
```

## 08_attach_failure

Projection: `{"life":[0,null],"coverage":[],"waits":[],"open":null}`
Selected candidate: None: no ATTACH_OK, so frontiers provide no backend coverage.

```jsonl
{"metric":"wait_time","version":1,"value":null,"unit":"ns","basis":"observed","completeness":"unavailable","reasons":["coverage_incomplete"],"diagnostic":{"recorded_ns":0,"covered_ns":0,"required_ns":19}}
{"metric":"completed_waits","version":1,"value":null,"unit":"count","basis":"observed","completeness":"unavailable","reasons":["coverage_incomplete"],"diagnostic":{"recorded_count":0,"covered_ns":0,"required_ns":19}}
```

## 09_seed_mismatch_then_transition

Projection: `{"life":[0,null],"coverage":[[9,15]],"waits":[],"open":["PgSleep",8,15]}`
Selected candidate: Transition-anchor run at 8; nonnull untrusted old side excludes close instant 8, giving left edge 9.

```jsonl
{"metric":"wait_time","version":1,"value":null,"unit":"ns","basis":"observed","completeness":"unavailable","reasons":["coverage_incomplete"],"diagnostic":{"recorded_ns":7,"covered_ns":6,"required_ns":7}}
{"metric":"completed_waits","version":1,"value":null,"unit":"count","basis":"observed","completeness":"unavailable","reasons":["coverage_incomplete"],"diagnostic":{"recorded_count":0,"covered_ns":6,"required_ns":7}}
```

## 10_equal_nonzero_seed

Projection: `{"life":[0,null],"coverage":[],"waits":[],"open":null}`
Selected candidate: None: nonzero reread has unknown entry and never leaves its initial wait.

```jsonl
{"metric":"wait_time","version":1,"value":null,"unit":"ns","basis":"observed","completeness":"unavailable","reasons":["coverage_incomplete"],"diagnostic":{"recorded_ns":0,"covered_ns":0,"required_ns":8}}
{"metric":"completed_waits","version":1,"value":null,"unit":"count","basis":"observed","completeness":"unavailable","reasons":["coverage_incomplete"],"diagnostic":{"recorded_count":0,"covered_ns":0,"required_ns":8}}
```

## 11_unknown_entry_close_boundary

Projection: `{"life":[0,null],"coverage":[[17,25]],"waits":[["Lock",16,22]],"open":null}`
Selected candidate: Run anchored at 12, trimmed to 17 after the unknown-entry close at 16; retain the full known Lock wait.

```jsonl
{"metric":"wait_time","version":1,"value":null,"unit":"ns","basis":"observed","completeness":"unavailable","reasons":["coverage_incomplete"],"diagnostic":{"recorded_ns":6,"covered_ns":8,"required_ns":9}}
{"metric":"completed_waits","version":1,"value":null,"unit":"count","basis":"observed","completeness":"unavailable","reasons":["coverage_incomplete"],"diagnostic":{"recorded_count":1,"covered_ns":8,"required_ns":9}}
```

## 12_loss_then_recovery_crossing

Projection: `{"life":[0,null],"coverage":[[16,24]],"waits":[],"open":["PgSleep",18,24]}`
Selected candidate: Recovered run anchored at 16: intersection length 8 beats the frozen run [1,8] intersection length 4.

```jsonl
{"metric":"wait_time","version":1,"value":null,"unit":"ns","basis":"observed","completeness":"unavailable","reasons":["coverage_incomplete"],"diagnostic":{"recorded_ns":6,"covered_ns":8,"required_ns":20}}
{"metric":"completed_waits","version":1,"value":null,"unit":"count","basis":"observed","completeness":"unavailable","reasons":["coverage_incomplete"],"diagnostic":{"recorded_count":0,"covered_ns":8,"required_ns":20}}
```

## 13_loss_then_recovery_later_window

Projection: `{"life":[0,null],"coverage":[[16,24]],"waits":[],"open":["PgSleep",18,24]}`
Selected candidate: Recovered run anchored at 16 covers the whole query; the earlier run is excluded.

```jsonl
{"metric":"wait_time","version":1,"value":6,"unit":"ns","basis":"observed","completeness":"complete","reasons":[],"detail":{"closed_ns":0,"open_ns":6},"diagnostic":{"recorded_ns":6,"covered_ns":8,"required_ns":8}}
{"metric":"completed_waits","version":1,"value":0,"unit":"count","basis":"observed","completeness":"complete","reasons":[],"diagnostic":{"recorded_count":0,"covered_ns":8,"required_ns":8}}
```

## 14_global_loss_other_backend

Projection: `{"life":[0,null],"coverage":[[1,8]],"waits":[],"open":["PgSleep",3,8]}`
Selected candidate: Target run anchored at 1, globally frozen at 8; reattachment of PID 202 does not revive PID 101.

```jsonl
{"metric":"wait_time","version":1,"value":null,"unit":"ns","basis":"observed","completeness":"unavailable","reasons":["coverage_incomplete"],"diagnostic":{"recorded_ns":5,"covered_ns":5,"required_ns":17}}
{"metric":"completed_waits","version":1,"value":null,"unit":"count","basis":"observed","completeness":"unavailable","reasons":["coverage_incomplete"],"diagnostic":{"recorded_count":0,"covered_ns":5,"required_ns":17}}
```

## 15_frontier_without_reattach

Projection: `{"life":[0,null],"coverage":[[1,8]],"waits":[],"open":["PgSleep",3,8]}`
Selected candidate: Run anchored at 1 remains frozen at 8; FRONTIER 20 cannot revive it without ATTACH_OK.

```jsonl
{"metric":"wait_time","version":1,"value":null,"unit":"ns","basis":"observed","completeness":"unavailable","reasons":["coverage_incomplete"],"diagnostic":{"recorded_ns":0,"covered_ns":0,"required_ns":12}}
{"metric":"completed_waits","version":1,"value":null,"unit":"count","basis":"observed","completeness":"unavailable","reasons":["coverage_incomplete"],"diagnostic":{"recorded_count":0,"covered_ns":0,"required_ns":12}}
```

## 16_crash_with_uncertified_suffix

Projection: `{"life":[0,null],"coverage":[[1,8]],"waits":[],"open":["PgSleep",3,8]}`
Selected candidate: Run anchored at 1, certified only through 8; the close at 12 is an uncertified suffix.

```jsonl
{"metric":"wait_time","version":1,"value":null,"unit":"ns","basis":"observed","completeness":"unavailable","reasons":["coverage_incomplete"],"diagnostic":{"recorded_ns":5,"covered_ns":5,"required_ns":9}}
{"metric":"completed_waits","version":1,"value":null,"unit":"count","basis":"observed","completeness":"unavailable","reasons":["coverage_incomplete"],"diagnostic":{"recorded_count":0,"covered_ns":5,"required_ns":9}}
```

## 17_wait_spans_frontiers

Projection: `{"life":[0,null],"coverage":[[1,20]],"waits":[],"open":["PgSleep",3,20]}`
Selected candidate: Run anchored at 1, reconstructed through the latest FRONTIER 20 with one still-open wait.

```jsonl
{"metric":"wait_time","version":1,"value":12,"unit":"ns","basis":"observed","completeness":"complete","reasons":[],"detail":{"closed_ns":0,"open_ns":12},"diagnostic":{"recorded_ns":12,"covered_ns":12,"required_ns":12}}
{"metric":"completed_waits","version":1,"value":0,"unit":"count","basis":"observed","completeness":"complete","reasons":[],"diagnostic":{"recorded_count":0,"covered_ns":12,"required_ns":12}}
```

## 18_detach_mid_wait

Projection: `{"life":[0,null],"coverage":[[1,12]],"waits":[],"open":["PgSleep",3,12]}`
Selected candidate: Run anchored at 1, capped by END 12 certified by FRONTIER 15; END does not close the wait.

```jsonl
{"metric":"wait_time","version":1,"value":9,"unit":"ns","basis":"observed","completeness":"complete","reasons":[],"detail":{"closed_ns":0,"open_ns":9},"diagnostic":{"recorded_ns":9,"covered_ns":9,"required_ns":9}}
{"metric":"completed_waits","version":1,"value":0,"unit":"count","basis":"observed","completeness":"complete","reasons":[],"diagnostic":{"recorded_count":0,"covered_ns":9,"required_ns":9}}
```

## 19_end_without_final_frontier

Projection: `{"life":[0,null],"coverage":[[1,8]],"waits":[],"open":["PgSleep",3,8]}`
Selected candidate: Run anchored at 1; pending END 12 has no certifying frontier and cannot advance endpoint 8.

```jsonl
{"metric":"wait_time","version":1,"value":null,"unit":"ns","basis":"observed","completeness":"unavailable","reasons":["coverage_incomplete"],"diagnostic":{"recorded_ns":5,"covered_ns":5,"required_ns":9}}
{"metric":"completed_waits","version":1,"value":null,"unit":"count","basis":"observed","completeness":"unavailable","reasons":["coverage_incomplete"],"diagnostic":{"recorded_count":0,"covered_ns":5,"required_ns":9}}
```

## 20_two_escalation_windows

Projection: `{"life":[0,null],"coverage":[[15,23]],"waits":[],"open":["PgSleep",17,23]}`
Selected candidate: Second run, anchored at 15: intersection length 8 beats first run [1,8] length 7; END 23 is certified.

```jsonl
{"metric":"wait_time","version":1,"value":null,"unit":"ns","basis":"observed","completeness":"unavailable","reasons":["coverage_incomplete"],"diagnostic":{"recorded_ns":6,"covered_ns":8,"required_ns":22}}
{"metric":"completed_waits","version":1,"value":null,"unit":"count","basis":"observed","completeness":"unavailable","reasons":["coverage_incomplete"],"diagnostic":{"recorded_count":0,"covered_ns":8,"required_ns":22}}
```

## 21_second_escalation_window

Projection: `{"life":[0,null],"coverage":[[15,23]],"waits":[],"open":["PgSleep",17,23]}`
Selected candidate: Second run, anchored at 15, covers the whole query; END 23 is certified.

```jsonl
{"metric":"wait_time","version":1,"value":6,"unit":"ns","basis":"observed","completeness":"complete","reasons":[],"detail":{"closed_ns":0,"open_ns":6},"diagnostic":{"recorded_ns":6,"covered_ns":8,"required_ns":8}}
{"metric":"completed_waits","version":1,"value":0,"unit":"count","basis":"observed","completeness":"complete","reasons":[],"diagnostic":{"recorded_count":0,"covered_ns":8,"required_ns":8}}
```

## 22_rotation_open_wait

Projection: `{"life":[0,null],"coverage":[[1,20]],"waits":[["PgSleep",3,16]],"open":null}`
Selected candidate: One run anchored at 1 across the complete linked rotation; FRONTIER 20 certifies the close at 16.

```jsonl
{"metric":"wait_time","version":1,"value":13,"unit":"ns","basis":"observed","completeness":"complete","reasons":[],"detail":{"closed_ns":13,"open_ns":0},"diagnostic":{"recorded_ns":13,"covered_ns":17,"required_ns":17}}
{"metric":"completed_waits","version":1,"value":1,"unit":"count","basis":"observed","completeness":"complete","reasons":[],"diagnostic":{"recorded_count":1,"covered_ns":17,"required_ns":17}}
```

## 23_rotation_missing_file

Projection: `{"life":[0,null],"coverage":[[1,8]],"waits":[],"open":["PgSleep",3,8]}`
Selected candidate: Run anchored at 1 from file 0 only; missing file 1 stops replay and file 2 is not salvaged.

```jsonl
{"metric":"wait_time","version":1,"value":null,"unit":"ns","basis":"observed","completeness":"unavailable","reasons":["coverage_incomplete"],"diagnostic":{"recorded_ns":5,"covered_ns":5,"required_ns":17}}
{"metric":"completed_waits","version":1,"value":null,"unit":"count","basis":"observed","completeness":"unavailable","reasons":["coverage_incomplete"],"diagnostic":{"recorded_count":0,"covered_ns":5,"required_ns":17}}
```

## 24_restart_same_pid

Projection: `{"life":[0,null],"coverage":[[16,24]],"waits":[],"open":["Lock",18,24]}`
Selected candidate: Capture B provisional generation, run anchored at 16; capture A cannot supply continuity or coverage.

```jsonl
{"metric":"wait_time","version":1,"value":null,"unit":"ns","basis":"observed","completeness":"unavailable","reasons":["coverage_incomplete"],"diagnostic":{"recorded_ns":6,"covered_ns":8,"required_ns":19}}
{"metric":"completed_waits","version":1,"value":null,"unit":"count","basis":"observed","completeness":"unavailable","reasons":["coverage_incomplete"],"diagnostic":{"recorded_count":0,"covered_ns":8,"required_ns":19}}
```

## 25_distinct_zero_length_waits

Projection: `{"life":[0,null],"coverage":[[1,10]],"waits":[["PgSleep",5,5],["PgSleep",5,5]],"open":null}`
Selected candidate: Run anchored at 1; physical tie order preserves both distinct zero-length waits.

```jsonl
{"metric":"wait_time","version":1,"value":0,"unit":"ns","basis":"observed","completeness":"complete","reasons":[],"detail":{"closed_ns":0,"open_ns":0},"diagnostic":{"recorded_ns":0,"covered_ns":3,"required_ns":3}}
{"metric":"completed_waits","version":1,"value":2,"unit":"count","basis":"observed","completeness":"complete","reasons":[],"diagnostic":{"recorded_count":2,"covered_ns":3,"required_ns":3}}
```

## 26_unreadable_counters

Projection: `{"life":[0,null],"coverage":[[1,8]],"waits":[],"open":["PgSleep",3,8]}`
Selected candidate: Original run frozen at 8; null counters taint identity, so restored counters and ATTACH_OK 16 cannot recover fork 0.

```jsonl
{"metric":"wait_time","version":1,"value":null,"unit":"ns","basis":"observed","completeness":"unavailable","reasons":["coverage_incomplete"],"diagnostic":{"recorded_ns":4,"covered_ns":4,"required_ns":20}}
{"metric":"completed_waits","version":1,"value":null,"unit":"count","basis":"observed","completeness":"unavailable","reasons":["coverage_incomplete"],"diagnostic":{"recorded_count":0,"covered_ns":4,"required_ns":20}}
```

## 27_preempt_rt_unqualified

Projection: `{"life":[0,null],"coverage":[],"waits":[],"open":null}`
Selected candidate: None: unqualified capture retains verified LIFE but ignores ATTACH_OK/FRONTIER/END certificates.

```jsonl
{"metric":"wait_time","version":1,"value":null,"unit":"ns","basis":"observed","completeness":"unavailable","reasons":["coverage_incomplete"],"diagnostic":{"recorded_ns":0,"covered_ns":0,"required_ns":9}}
{"metric":"completed_waits","version":1,"value":null,"unit":"count","basis":"observed","completeness":"unavailable","reasons":["coverage_incomplete"],"diagnostic":{"recorded_count":0,"covered_ns":0,"required_ns":9}}
```

## 28_legacy_v2

Projection: `{"life":null,"coverage":[],"waits":[],"open":null}`
Selected candidate: LIFE_INVALID: legacy v2 supplies no v1 lifetime or certificates.

```jsonl
{"metric":"wait_time","version":1,"value":null,"unit":"ns","basis":"observed","completeness":"unavailable","reasons":["evidence_invalid"],"diagnostic":{"recorded_ns":null,"covered_ns":null,"required_ns":null}}
{"metric":"completed_waits","version":1,"value":null,"unit":"count","basis":"observed","completeness":"unavailable","reasons":["evidence_invalid"],"diagnostic":{"recorded_count":null,"covered_ns":null,"required_ns":null}}
```

## 29_legacy_v3

Projection: `{"life":null,"coverage":[],"waits":[],"open":null}`
Selected candidate: LIFE_INVALID: legacy v3 supplies no v1 lifetime or certificates.

```jsonl
{"metric":"wait_time","version":1,"value":null,"unit":"ns","basis":"observed","completeness":"unavailable","reasons":["evidence_invalid"],"diagnostic":{"recorded_ns":null,"covered_ns":null,"required_ns":null}}
{"metric":"completed_waits","version":1,"value":null,"unit":"count","basis":"observed","completeness":"unavailable","reasons":["evidence_invalid"],"diagnostic":{"recorded_count":null,"covered_ns":null,"required_ns":null}}
```

## 30_legacy_v4

Projection: `{"life":null,"coverage":[],"waits":[],"open":null}`
Selected candidate: LIFE_INVALID: legacy v4 supplies no v1 lifetime or certificates.

```jsonl
{"metric":"wait_time","version":1,"value":null,"unit":"ns","basis":"observed","completeness":"unavailable","reasons":["evidence_invalid"],"diagnostic":{"recorded_ns":null,"covered_ns":null,"required_ns":null}}
{"metric":"completed_waits","version":1,"value":null,"unit":"count","basis":"observed","completeness":"unavailable","reasons":["evidence_invalid"],"diagnostic":{"recorded_count":null,"covered_ns":null,"required_ns":null}}
```

## 31_late_data_revokes_prefix

Projection: `{"life":[0,null],"coverage":[],"waits":[],"open":null}`
Selected candidate: None: VOID invalid_from=1 revokes FRONTIER 10; no earlier frontier survives, while verified LIFE is retained.

```jsonl
{"metric":"wait_time","version":1,"value":null,"unit":"ns","basis":"observed","completeness":"unavailable","reasons":["coverage_incomplete"],"diagnostic":{"recorded_ns":0,"covered_ns":0,"required_ns":9}}
{"metric":"completed_waits","version":1,"value":null,"unit":"count","basis":"observed","completeness":"unavailable","reasons":["coverage_incomplete"],"diagnostic":{"recorded_count":0,"covered_ns":0,"required_ns":9}}
```

## 32_end_before_delayed_transition

Projection: `{"life":[0,null],"coverage":[[1,12]],"waits":[],"open":["PgSleep",11,12]}`
Selected candidate: Run anchored at 1, capped at END 12; the delayed transition at 11 is included before FRONTIER 15 certifies END.

```jsonl
{"metric":"wait_time","version":1,"value":1,"unit":"ns","basis":"observed","completeness":"complete","reasons":[],"detail":{"closed_ns":0,"open_ns":1},"diagnostic":{"recorded_ns":1,"covered_ns":11,"required_ns":11}}
{"metric":"completed_waits","version":1,"value":0,"unit":"count","basis":"observed","completeness":"complete","reasons":[],"diagnostic":{"recorded_count":0,"covered_ns":11,"required_ns":11}}
```

## 33_anchor_transition_before_attach

Projection: `{"life":[0,null],"coverage":[[9,15]],"waits":[],"open":["PgSleep",8,15]}`
Selected candidate: Transition-anchor run at 8, trimmed to 9; anchor TRANS before ATTACH_OK is valid since both precede FRONTIER 15.

```jsonl
{"metric":"wait_time","version":1,"value":null,"unit":"ns","basis":"observed","completeness":"unavailable","reasons":["coverage_incomplete"],"diagnostic":{"recorded_ns":7,"covered_ns":6,"required_ns":7}}
{"metric":"completed_waits","version":1,"value":null,"unit":"count","basis":"observed","completeness":"unavailable","reasons":["coverage_incomplete"],"diagnostic":{"recorded_count":0,"covered_ns":6,"required_ns":7}}
```

## Ambiguities

none
