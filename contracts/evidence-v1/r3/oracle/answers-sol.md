# Independent expected answers

Times and durations are integer nanoseconds. Each projection contains only the selected run; candidate bounds below are the projected bounds, with the original attachment anchor stated separately where trimmed. `completeness` is the frozen result state.

## 01_ordinary_run

Selected candidate: attachment a=1, projected [1,15] (A/pid=101/fork=0).
Projection: `{"life":[0,null],"coverage":[[1,15]],"waits":[["PgSleep",3,7],["Lock",9,12]],"open":null}`
Replay: FRONTIER 15 certifies the ordinary run.

```json
{"metric":"wait_time","version":1,"value":4,"unit":"ns","basis":"observed","completeness":"complete","reasons":[],"detail":{"closed_ns":4,"open_ns":0},"diagnostic":{"recorded_ns":4,"covered_ns":14,"required_ns":14}}
{"metric":"completed_waits","version":1,"value":1,"unit":"count","basis":"observed","completeness":"complete","reasons":[],"diagnostic":{"recorded_count":1,"covered_ns":14,"required_ns":14}}
```

## 02_pid_reuse_delayed_life

Selected candidate: attachment a=13, projected [13,20] (A/pid=101/fork=12).
Projection: `{"life":[12,null],"coverage":[[13,20]],"waits":[],"open":["Lock",15,20]}`
Replay: Resolve delayed LIFE.exit=8 for fork 0 before FRONTIER 20; the target is fork 12 (§§1,4).

```json
{"metric":"wait_time","version":1,"value":5,"unit":"ns","basis":"observed","completeness":"complete","reasons":[],"detail":{"closed_ns":0,"open_ns":5},"diagnostic":{"recorded_ns":5,"covered_ns":7,"required_ns":7}}
{"metric":"completed_waits","version":1,"value":0,"unit":"count","basis":"observed","completeness":"complete","reasons":[],"diagnostic":{"recorded_count":0,"covered_ns":7,"required_ns":7}}
```

## 03_exit_inside_window

Selected candidate: attachment a=1, projected [1,10] (A/pid=101/fork=0).
Projection: `{"life":[0,10],"coverage":[[1,10]],"waits":[["PgSleep",3,10]],"open":null}`
Replay: The matching terminal TRANS and LIFE certify exit 10; FRONTIER 12 caps the run at exit (§5.4).

```json
{"metric":"wait_time","version":1,"value":5,"unit":"ns","basis":"observed","completeness":"complete","reasons":[],"detail":{"closed_ns":5,"open_ns":0},"diagnostic":{"recorded_ns":5,"covered_ns":5,"required_ns":5}}
{"metric":"completed_waits","version":1,"value":1,"unit":"count","basis":"observed","completeness":"complete","reasons":[],"diagnostic":{"recorded_count":1,"covered_ns":5,"required_ns":5}}
```

## 04_window_starts_at_exit

Selected candidate: attachment a=1, projected [1,10] (A/pid=101/fork=0).
Projection: `{"life":[0,10],"coverage":[[1,10]],"waits":[["PgSleep",3,10]],"open":null}`
Replay: Exit 10 is certified. The window has no lifetime duration but includes the close at exit (core §4).

```json
{"metric":"wait_time","version":1,"value":0,"unit":"ns","basis":"observed","completeness":"complete","reasons":[],"detail":{"closed_ns":0,"open_ns":0},"diagnostic":{"recorded_ns":0,"covered_ns":0,"required_ns":0}}
{"metric":"completed_waits","version":1,"value":1,"unit":"count","basis":"observed","completeness":"complete","reasons":[],"diagnostic":{"recorded_count":1,"covered_ns":0,"required_ns":0}}
```

## 05_known_exit_lost_terminal

Selected candidate: attachment a=1, projected [1,8] (A/pid=101/fork=0).
Projection: `{"life":[0,12],"coverage":[[1,8]],"waits":[],"open":["PgSleep",3,8]}`
Replay: Retain LIFE.exit=12; VOID 14 freezes at FRONTIER 8 and supplies no terminal close (§4).

```json
{"metric":"wait_time","version":1,"value":null,"unit":"ns","basis":"observed","completeness":"unavailable","reasons":["coverage_incomplete"],"diagnostic":{"recorded_ns":2,"covered_ns":2,"required_ns":6}}
{"metric":"completed_waits","version":1,"value":null,"unit":"count","basis":"observed","completeness":"unavailable","reasons":["coverage_incomplete"],"diagnostic":{"recorded_count":0,"covered_ns":2,"required_ns":6}}
```

## 06_preexisting_before_capture

Selected candidate: attachment a=12, projected [12,20] (A/pid=101/fork=null).
Projection: `{"life":[0,null],"coverage":[[12,20]],"waits":[],"open":["PgSleep",14,20]}`
Replay: Retain the provisional lifetime lower bound 0; discovery 10 does not replace it (§2; lifetime clarification).

```json
{"metric":"wait_time","version":1,"value":null,"unit":"ns","basis":"observed","completeness":"unavailable","reasons":["coverage_incomplete"],"diagnostic":{"recorded_ns":6,"covered_ns":8,"required_ns":18}}
{"metric":"completed_waits","version":1,"value":null,"unit":"count","basis":"observed","completeness":"unavailable","reasons":["coverage_incomplete"],"diagnostic":{"recorded_count":0,"covered_ns":8,"required_ns":18}}
```

## 07_preexisting_covered_window

Selected candidate: attachment a=12, projected [12,20] (A/pid=101/fork=null).
Projection: `{"life":[0,null],"coverage":[[12,20]],"waits":[],"open":["PgSleep",14,20]}`
Replay: The query lies wholly within the certified reread run; lifetime still begins at lower bound 0.

```json
{"metric":"wait_time","version":1,"value":6,"unit":"ns","basis":"observed","completeness":"complete","reasons":[],"detail":{"closed_ns":0,"open_ns":6},"diagnostic":{"recorded_ns":6,"covered_ns":8,"required_ns":8}}
{"metric":"completed_waits","version":1,"value":0,"unit":"count","basis":"observed","completeness":"complete","reasons":[],"diagnostic":{"recorded_count":0,"covered_ns":8,"required_ns":8}}
```

## 08_attach_failure

Selected candidate: none (A/pid=101/fork=0).
Projection: `{"life":[0,null],"coverage":[],"waits":[],"open":null}`
Replay: FRONTIERs without any ATTACH_OK certify no target run (§4).

```json
{"metric":"wait_time","version":1,"value":null,"unit":"ns","basis":"observed","completeness":"unavailable","reasons":["coverage_incomplete"],"diagnostic":{"recorded_ns":0,"covered_ns":0,"required_ns":19}}
{"metric":"completed_waits","version":1,"value":null,"unit":"count","basis":"observed","completeness":"unavailable","reasons":["coverage_incomplete"],"diagnostic":{"recorded_count":0,"covered_ns":0,"required_ns":19}}
```

## 09_seed_mismatch_then_transition

Selected candidate: attachment a=8, projected [9,15] (A/pid=101/fork=0).
Projection: `{"life":[0,null],"coverage":[[9,15]],"waits":[],"open":["PgSleep",8,15]}`
Replay: Discard the transition anchor's nonnull old side; trim coverage to 8+1 and retain the known open entry 8 (§5.2–4).

```json
{"metric":"wait_time","version":1,"value":null,"unit":"ns","basis":"observed","completeness":"unavailable","reasons":["coverage_incomplete"],"diagnostic":{"recorded_ns":7,"covered_ns":6,"required_ns":7}}
{"metric":"completed_waits","version":1,"value":null,"unit":"count","basis":"observed","completeness":"unavailable","reasons":["coverage_incomplete"],"diagnostic":{"recorded_count":0,"covered_ns":6,"required_ns":7}}
```

## 10_equal_nonzero_seed

Selected candidate: none (A/pid=101/fork=null).
Projection: `{"life":[0,null],"coverage":[],"waits":[],"open":null}`
Replay: The reread wait has unknown entry and never closes, so the run has no core-representable portion (§5.1).

```json
{"metric":"wait_time","version":1,"value":null,"unit":"ns","basis":"observed","completeness":"unavailable","reasons":["coverage_incomplete"],"diagnostic":{"recorded_ns":0,"covered_ns":0,"required_ns":8}}
{"metric":"completed_waits","version":1,"value":null,"unit":"count","basis":"observed","completeness":"unavailable","reasons":["coverage_incomplete"],"diagnostic":{"recorded_count":0,"covered_ns":0,"required_ns":8}}
```

## 11_unknown_entry_close_boundary

Selected candidate: attachment a=12, projected [17,25] (A/pid=101/fork=null).
Projection: `{"life":[0,null],"coverage":[[17,25]],"waits":[["Lock",16,22]],"open":null}`
Replay: Omit the unknown-entry PgSleep close at 16; begin coverage at 17, preserving the full known-entry Lock wait (§5.1,3).

```json
{"metric":"wait_time","version":1,"value":null,"unit":"ns","basis":"observed","completeness":"unavailable","reasons":["coverage_incomplete"],"diagnostic":{"recorded_ns":6,"covered_ns":8,"required_ns":9}}
{"metric":"completed_waits","version":1,"value":null,"unit":"count","basis":"observed","completeness":"unavailable","reasons":["coverage_incomplete"],"diagnostic":{"recorded_count":1,"covered_ns":8,"required_ns":9}}
```

## 12_loss_then_recovery_crossing

Selected candidate: attachment a=16, projected [16,24] (A/pid=101/fork=0).
Projection: `{"life":[0,null],"coverage":[[16,24]],"waits":[],"open":["PgSleep",18,24]}`
Replay: VOID 14 freezes the first run at 8. Select the second run: intersection length 8 versus 4 (§5 selection).

```json
{"metric":"wait_time","version":1,"value":null,"unit":"ns","basis":"observed","completeness":"unavailable","reasons":["coverage_incomplete"],"diagnostic":{"recorded_ns":6,"covered_ns":8,"required_ns":20}}
{"metric":"completed_waits","version":1,"value":null,"unit":"count","basis":"observed","completeness":"unavailable","reasons":["coverage_incomplete"],"diagnostic":{"recorded_count":0,"covered_ns":8,"required_ns":20}}
```

## 13_loss_then_recovery_later_window

Selected candidate: attachment a=16, projected [16,24] (A/pid=101/fork=0).
Projection: `{"life":[0,null],"coverage":[[16,24]],"waits":[],"open":["PgSleep",18,24]}`
Replay: VOID 14 freezes the first run at 8. The fresh run covers the whole query (§§4,5).

```json
{"metric":"wait_time","version":1,"value":6,"unit":"ns","basis":"observed","completeness":"complete","reasons":[],"detail":{"closed_ns":0,"open_ns":6},"diagnostic":{"recorded_ns":6,"covered_ns":8,"required_ns":8}}
{"metric":"completed_waits","version":1,"value":0,"unit":"count","basis":"observed","completeness":"complete","reasons":[],"diagnostic":{"recorded_count":0,"covered_ns":8,"required_ns":8}}
```

## 14_global_loss_other_backend

Selected candidate: attachment a=1, projected [1,8] (A/pid=101/fork=0).
Projection: `{"life":[0,null],"coverage":[[1,8]],"waits":[],"open":["PgSleep",3,8]}`
Replay: Query-text loss is global; reattaching PID 202 cannot extend the target's frozen run (§§3,4).

```json
{"metric":"wait_time","version":1,"value":null,"unit":"ns","basis":"observed","completeness":"unavailable","reasons":["coverage_incomplete"],"diagnostic":{"recorded_ns":5,"covered_ns":5,"required_ns":17}}
{"metric":"completed_waits","version":1,"value":null,"unit":"count","basis":"observed","completeness":"unavailable","reasons":["coverage_incomplete"],"diagnostic":{"recorded_count":0,"covered_ns":5,"required_ns":17}}
```

## 15_frontier_without_reattach

Selected candidate: attachment a=1, projected [1,8] (A/pid=101/fork=0).
Projection: `{"life":[0,null],"coverage":[[1,8]],"waits":[],"open":["PgSleep",3,8]}`
Replay: VOID freezes the target at 8. FRONTIER 20 cannot revive it without fresh ATTACH_OK (§4).

```json
{"metric":"wait_time","version":1,"value":null,"unit":"ns","basis":"observed","completeness":"unavailable","reasons":["coverage_incomplete"],"diagnostic":{"recorded_ns":0,"covered_ns":0,"required_ns":12}}
{"metric":"completed_waits","version":1,"value":null,"unit":"count","basis":"observed","completeness":"unavailable","reasons":["coverage_incomplete"],"diagnostic":{"recorded_count":0,"covered_ns":0,"required_ns":12}}
```

## 16_crash_with_uncertified_suffix

Selected candidate: attachment a=1, projected [1,8] (A/pid=101/fork=0).
Projection: `{"life":[0,null],"coverage":[[1,8]],"waits":[],"open":["PgSleep",3,8]}`
Replay: Discard the uncertified TRANS 12 at EOF; reconstruct the open wait at FRONTIER 8 (§6).

```json
{"metric":"wait_time","version":1,"value":null,"unit":"ns","basis":"observed","completeness":"unavailable","reasons":["coverage_incomplete"],"diagnostic":{"recorded_ns":5,"covered_ns":5,"required_ns":9}}
{"metric":"completed_waits","version":1,"value":null,"unit":"count","basis":"observed","completeness":"unavailable","reasons":["coverage_incomplete"],"diagnostic":{"recorded_count":0,"covered_ns":5,"required_ns":9}}
```

## 17_wait_spans_frontiers

Selected candidate: attachment a=1, projected [1,20] (A/pid=101/fork=0).
Projection: `{"life":[0,null],"coverage":[[1,20]],"waits":[],"open":["PgSleep",3,20]}`
Replay: The latest FRONTIER extends the same run's observation to 20, without inventing any close (§4).

```json
{"metric":"wait_time","version":1,"value":12,"unit":"ns","basis":"observed","completeness":"complete","reasons":[],"detail":{"closed_ns":0,"open_ns":12},"diagnostic":{"recorded_ns":12,"covered_ns":12,"required_ns":12}}
{"metric":"completed_waits","version":1,"value":0,"unit":"count","basis":"observed","completeness":"complete","reasons":[],"diagnostic":{"recorded_count":0,"covered_ns":12,"required_ns":12}}
```

## 18_detach_mid_wait

Selected candidate: attachment a=1, projected [1,12] (A/pid=101/fork=0).
Projection: `{"life":[0,null],"coverage":[[1,12]],"waits":[],"open":["PgSleep",3,12]}`
Replay: FRONTIER 15 certifies END 12; END caps the open observation and does not close the wait (§§3,4).

```json
{"metric":"wait_time","version":1,"value":9,"unit":"ns","basis":"observed","completeness":"complete","reasons":[],"detail":{"closed_ns":0,"open_ns":9},"diagnostic":{"recorded_ns":9,"covered_ns":9,"required_ns":9}}
{"metric":"completed_waits","version":1,"value":0,"unit":"count","basis":"observed","completeness":"complete","reasons":[],"diagnostic":{"recorded_count":0,"covered_ns":9,"required_ns":9}}
```

## 19_end_without_final_frontier

Selected candidate: attachment a=1, projected [1,8] (A/pid=101/fork=0).
Projection: `{"life":[0,null],"coverage":[[1,8]],"waits":[],"open":["PgSleep",3,8]}`
Replay: END 12 remains uncertified at EOF; retain FRONTIER 8 only (§§3,6).

```json
{"metric":"wait_time","version":1,"value":null,"unit":"ns","basis":"observed","completeness":"unavailable","reasons":["coverage_incomplete"],"diagnostic":{"recorded_ns":5,"covered_ns":5,"required_ns":9}}
{"metric":"completed_waits","version":1,"value":null,"unit":"count","basis":"observed","completeness":"unavailable","reasons":["coverage_incomplete"],"diagnostic":{"recorded_count":0,"covered_ns":5,"required_ns":9}}
```

## 20_two_escalation_windows

Selected candidate: attachment a=15, projected [15,23] (A/pid=101/fork=0).
Projection: `{"life":[0,null],"coverage":[[15,23]],"waits":[],"open":["PgSleep",17,23]}`
Replay: Keep runs separate. Select the second run: intersection length 8 versus 7; do not combine them (§5).

```json
{"metric":"wait_time","version":1,"value":null,"unit":"ns","basis":"observed","completeness":"unavailable","reasons":["coverage_incomplete"],"diagnostic":{"recorded_ns":6,"covered_ns":8,"required_ns":22}}
{"metric":"completed_waits","version":1,"value":null,"unit":"count","basis":"observed","completeness":"unavailable","reasons":["coverage_incomplete"],"diagnostic":{"recorded_count":0,"covered_ns":8,"required_ns":22}}
```

## 21_second_escalation_window

Selected candidate: attachment a=15, projected [15,23] (A/pid=101/fork=0).
Projection: `{"life":[0,null],"coverage":[[15,23]],"waits":[],"open":["PgSleep",17,23]}`
Replay: The second run alone covers the query; END 23 is certified by FRONTIER 25 (§5).

```json
{"metric":"wait_time","version":1,"value":6,"unit":"ns","basis":"observed","completeness":"complete","reasons":[],"detail":{"closed_ns":0,"open_ns":6},"diagnostic":{"recorded_ns":6,"covered_ns":8,"required_ns":8}}
{"metric":"completed_waits","version":1,"value":0,"unit":"count","basis":"observed","completeness":"complete","reasons":[],"diagnostic":{"recorded_count":0,"covered_ns":8,"required_ns":8}}
```

## 22_rotation_open_wait

Selected candidate: attachment a=1, projected [1,20] (A/pid=101/fork=0).
Projection: `{"life":[0,null],"coverage":[[1,20]],"waits":[["PgSleep",3,16]],"open":null}`
Replay: The complete linked rotation preserves the run; TRANS 16 closes the wait and FRONTIER 20 certifies it (§6).

```json
{"metric":"wait_time","version":1,"value":13,"unit":"ns","basis":"observed","completeness":"complete","reasons":[],"detail":{"closed_ns":13,"open_ns":0},"diagnostic":{"recorded_ns":13,"covered_ns":17,"required_ns":17}}
{"metric":"completed_waits","version":1,"value":1,"unit":"count","basis":"observed","completeness":"complete","reasons":[],"diagnostic":{"recorded_count":1,"covered_ns":17,"required_ns":17}}
```

## 23_rotation_missing_file

Selected candidate: attachment a=1, projected [1,8] (A/pid=101/fork=0).
Projection: `{"life":[0,null],"coverage":[[1,8]],"waits":[],"open":["PgSleep",3,8]}`
Replay: Stop capture A at missing file 1; ignore file 2 and retain FRONTIER 8 (§6).

```json
{"metric":"wait_time","version":1,"value":null,"unit":"ns","basis":"observed","completeness":"unavailable","reasons":["coverage_incomplete"],"diagnostic":{"recorded_ns":5,"covered_ns":5,"required_ns":17}}
{"metric":"completed_waits","version":1,"value":null,"unit":"count","basis":"observed","completeness":"unavailable","reasons":["coverage_incomplete"],"diagnostic":{"recorded_count":0,"covered_ns":5,"required_ns":17}}
```

## 24_restart_same_pid

Selected candidate: attachment a=16, projected [16,24] (B/pid=101/fork=null).
Projection: `{"life":[0,null],"coverage":[[16,24]],"waits":[],"open":["Lock",18,24]}`
Replay: Use only target capture B. Its provisional lifetime lower bound is 0; do not stitch capture A (§§4,6).

```json
{"metric":"wait_time","version":1,"value":null,"unit":"ns","basis":"observed","completeness":"unavailable","reasons":["coverage_incomplete"],"diagnostic":{"recorded_ns":6,"covered_ns":8,"required_ns":19}}
{"metric":"completed_waits","version":1,"value":null,"unit":"count","basis":"observed","completeness":"unavailable","reasons":["coverage_incomplete"],"diagnostic":{"recorded_count":0,"covered_ns":8,"required_ns":19}}
```

## 25_distinct_zero_length_waits

Selected candidate: attachment a=1, projected [1,10] (A/pid=101/fork=0).
Projection: `{"life":[0,null],"coverage":[[1,10]],"waits":[["PgSleep",5,5],["PgSleep",5,5]],"open":null}`
Replay: Preserve physical tie order and both distinct zero-length waits; identical payloads do not deduplicate (§§1,2).

```json
{"metric":"wait_time","version":1,"value":0,"unit":"ns","basis":"observed","completeness":"complete","reasons":[],"detail":{"closed_ns":0,"open_ns":0},"diagnostic":{"recorded_ns":0,"covered_ns":3,"required_ns":3}}
{"metric":"completed_waits","version":1,"value":2,"unit":"count","basis":"observed","completeness":"complete","reasons":[],"diagnostic":{"recorded_count":2,"covered_ns":3,"required_ns":3}}
```

## 26_unreadable_counters

Selected candidate: attachment a=1, projected [1,8] (A/pid=101/fork=0).
Projection: `{"life":[0,null],"coverage":[[1,8]],"waits":[],"open":["PgSleep",3,8]}`
Replay: Null counters taint lifecycle identity. A restored baseline and old-generation ATTACH_OK 16 cannot repair it (§4); retain the frozen prefix.

```json
{"metric":"wait_time","version":1,"value":null,"unit":"ns","basis":"observed","completeness":"unavailable","reasons":["coverage_incomplete"],"diagnostic":{"recorded_ns":4,"covered_ns":4,"required_ns":20}}
{"metric":"completed_waits","version":1,"value":null,"unit":"count","basis":"observed","completeness":"unavailable","reasons":["coverage_incomplete"],"diagnostic":{"recorded_count":0,"covered_ns":4,"required_ns":20}}
```

## 27_preempt_rt_unqualified

Selected candidate: none (A/pid=101/fork=0).
Projection: `{"life":[0,null],"coverage":[],"waits":[],"open":null}`
Replay: Unqualified capture: retain verified LIFE, ignore attachment/frontier certificates (§5, lines 291–296).

```json
{"metric":"wait_time","version":1,"value":null,"unit":"ns","basis":"observed","completeness":"unavailable","reasons":["coverage_incomplete"],"diagnostic":{"recorded_ns":0,"covered_ns":0,"required_ns":9}}
{"metric":"completed_waits","version":1,"value":null,"unit":"count","basis":"observed","completeness":"unavailable","reasons":["coverage_incomplete"],"diagnostic":{"recorded_count":0,"covered_ns":0,"required_ns":9}}
```

## 28_legacy_v2

Selected candidate: LIFE_INVALID (A/pid=101/fork=null).
Projection: `{"life":null,"coverage":[],"waits":[],"open":null}`
Replay: Legacy v2/v3/v4 has no v1 LIFE or exact evidence; project LIFE_INVALID (§6, lines 318–321).

```json
{"metric":"wait_time","version":1,"value":null,"unit":"ns","basis":"observed","completeness":"unavailable","reasons":["evidence_invalid"],"diagnostic":{"recorded_ns":null,"covered_ns":null,"required_ns":null}}
{"metric":"completed_waits","version":1,"value":null,"unit":"count","basis":"observed","completeness":"unavailable","reasons":["evidence_invalid"],"diagnostic":{"recorded_count":null,"covered_ns":null,"required_ns":null}}
```

## 29_legacy_v3

Selected candidate: LIFE_INVALID (A/pid=101/fork=null).
Projection: `{"life":null,"coverage":[],"waits":[],"open":null}`
Replay: Legacy v2/v3/v4 has no v1 LIFE or exact evidence; project LIFE_INVALID (§6, lines 318–321).

```json
{"metric":"wait_time","version":1,"value":null,"unit":"ns","basis":"observed","completeness":"unavailable","reasons":["evidence_invalid"],"diagnostic":{"recorded_ns":null,"covered_ns":null,"required_ns":null}}
{"metric":"completed_waits","version":1,"value":null,"unit":"count","basis":"observed","completeness":"unavailable","reasons":["evidence_invalid"],"diagnostic":{"recorded_count":null,"covered_ns":null,"required_ns":null}}
```

## 30_legacy_v4

Selected candidate: LIFE_INVALID (A/pid=101/fork=null).
Projection: `{"life":null,"coverage":[],"waits":[],"open":null}`
Replay: Legacy v2/v3/v4 has no v1 LIFE or exact evidence; project LIFE_INVALID (§6, lines 318–321).

```json
{"metric":"wait_time","version":1,"value":null,"unit":"ns","basis":"observed","completeness":"unavailable","reasons":["evidence_invalid"],"diagnostic":{"recorded_ns":null,"covered_ns":null,"required_ns":null}}
{"metric":"completed_waits","version":1,"value":null,"unit":"count","basis":"observed","completeness":"unavailable","reasons":["evidence_invalid"],"diagnostic":{"recorded_count":null,"covered_ns":null,"required_ns":null}}
```

## 31_late_data_revokes_prefix

Selected candidate: none (A/pid=101/fork=0).
Projection: `{"life":[0,null],"coverage":[],"waits":[],"open":null}`
Replay: VOID.invalid_from=1 revokes FRONTIER 10; no surviving certificate remains, while LIFE survives (§4).

```json
{"metric":"wait_time","version":1,"value":null,"unit":"ns","basis":"observed","completeness":"unavailable","reasons":["coverage_incomplete"],"diagnostic":{"recorded_ns":0,"covered_ns":0,"required_ns":9}}
{"metric":"completed_waits","version":1,"value":null,"unit":"count","basis":"observed","completeness":"unavailable","reasons":["coverage_incomplete"],"diagnostic":{"recorded_count":0,"covered_ns":0,"required_ns":9}}
```

## 32_end_before_delayed_transition

Selected candidate: attachment a=1, projected [1,12] (A/pid=101/fork=0).
Projection: `{"life":[0,null],"coverage":[[1,12]],"waits":[],"open":["PgSleep",11,12]}`
Replay: Resolve delayed TRANS 11 through END 12 before applying END; FRONTIER 15 certifies the full run (§3).

```json
{"metric":"wait_time","version":1,"value":1,"unit":"ns","basis":"observed","completeness":"complete","reasons":[],"detail":{"closed_ns":0,"open_ns":1},"diagnostic":{"recorded_ns":1,"covered_ns":11,"required_ns":11}}
{"metric":"completed_waits","version":1,"value":0,"unit":"count","basis":"observed","completeness":"complete","reasons":[],"diagnostic":{"recorded_count":0,"covered_ns":11,"required_ns":11}}
```

## 33_anchor_transition_before_attach

Selected candidate: attachment a=8, projected [9,15] (A/pid=101/fork=0).
Projection: `{"life":[0,null],"coverage":[[9,15]],"waits":[],"open":["PgSleep",8,15]}`
Replay: Resolve the physically earlier TRANS as the transition anchor; discard its old side and trim to 9 (§§3,5.2).

```json
{"metric":"wait_time","version":1,"value":null,"unit":"ns","basis":"observed","completeness":"unavailable","reasons":["coverage_incomplete"],"diagnostic":{"recorded_ns":7,"covered_ns":6,"required_ns":7}}
{"metric":"completed_waits","version":1,"value":null,"unit":"count","basis":"observed","completeness":"unavailable","reasons":["coverage_incomplete"],"diagnostic":{"recorded_count":0,"covered_ns":6,"required_ns":7}}
```

## 34_malformed_record_prefix

Selected candidate: attachment a=1, projected [1,8] (A/pid=101/fork=0).
Projection: `{"life":[0,null],"coverage":[[1,8]],"waits":[],"open":["PgSleep",3,8]}`
Replay: Malformed TRANS 12 lacks duration; stop before it (§5.1). Retain the target's certified prefix at 8.

```json
{"metric":"wait_time","version":1,"value":5,"unit":"ns","basis":"observed","completeness":"complete","reasons":[],"detail":{"closed_ns":0,"open_ns":5},"diagnostic":{"recorded_ns":5,"covered_ns":7,"required_ns":7}}
{"metric":"completed_waits","version":1,"value":0,"unit":"count","basis":"observed","completeness":"complete","reasons":[],"diagnostic":{"recorded_count":0,"covered_ns":7,"required_ns":7}}
```

## 35_malformed_record_suffix

Selected candidate: attachment a=1, projected [1,8] (A/pid=101/fork=0).
Projection: `{"life":[0,null],"coverage":[[1,8]],"waits":[],"open":["PgSleep",3,8]}`
Replay: Malformed TRANS 12 lacks duration; stop before it (§5.1). Retain the target's certified prefix at 8.

```json
{"metric":"wait_time","version":1,"value":null,"unit":"ns","basis":"observed","completeness":"unavailable","reasons":["coverage_incomplete"],"diagnostic":{"recorded_ns":0,"covered_ns":0,"required_ns":8}}
{"metric":"completed_waits","version":1,"value":null,"unit":"count","basis":"observed","completeness":"unavailable","reasons":["coverage_incomplete"],"diagnostic":{"recorded_count":0,"covered_ns":0,"required_ns":8}}
```

## 36_unknown_kind_prefix

Selected candidate: attachment a=1, projected [1,8] (A/pid=101/fork=0).
Projection: `{"life":[0,null],"coverage":[[1,8]],"waits":[],"open":["PgSleep",3,8]}`
Replay: Unsupported FUTURE_EVIDENCE at 12 stops the chain; its readable counters do not make it VOID (§5.1). Retain the target's certified prefix at 8.

```json
{"metric":"wait_time","version":1,"value":5,"unit":"ns","basis":"observed","completeness":"complete","reasons":[],"detail":{"closed_ns":0,"open_ns":5},"diagnostic":{"recorded_ns":5,"covered_ns":7,"required_ns":7}}
{"metric":"completed_waits","version":1,"value":0,"unit":"count","basis":"observed","completeness":"complete","reasons":[],"diagnostic":{"recorded_count":0,"covered_ns":7,"required_ns":7}}
```

## 37_unknown_kind_suffix

Selected candidate: attachment a=1, projected [1,8] (A/pid=101/fork=0).
Projection: `{"life":[0,null],"coverage":[[1,8]],"waits":[],"open":["PgSleep",3,8]}`
Replay: Unsupported FUTURE_EVIDENCE at 12 stops the chain; its readable counters do not make it VOID (§5.1). Retain the target's certified prefix at 8.

```json
{"metric":"wait_time","version":1,"value":null,"unit":"ns","basis":"observed","completeness":"unavailable","reasons":["coverage_incomplete"],"diagnostic":{"recorded_ns":0,"covered_ns":0,"required_ns":8}}
{"metric":"completed_waits","version":1,"value":null,"unit":"count","basis":"observed","completeness":"unavailable","reasons":["coverage_incomplete"],"diagnostic":{"recorded_count":0,"covered_ns":0,"required_ns":8}}
```

## 38_frontier_counter_change_prefix

Selected candidate: attachment a=1, projected [1,8] (A/pid=101/fork=0).
Projection: `{"life":[0,null],"coverage":[[1,8]],"waits":[],"open":["PgSleep",3,8]}`
Replay: Changed counters turn FRONTIER 12 into VOID; the fresh second run is valid but the first covers this query (§5 counter and selection rules).

```json
{"metric":"wait_time","version":1,"value":5,"unit":"ns","basis":"observed","completeness":"complete","reasons":[],"detail":{"closed_ns":0,"open_ns":5},"diagnostic":{"recorded_ns":5,"covered_ns":7,"required_ns":7}}
{"metric":"completed_waits","version":1,"value":0,"unit":"count","basis":"observed","completeness":"complete","reasons":[],"diagnostic":{"recorded_count":0,"covered_ns":7,"required_ns":7}}
```

## 39_frontier_counter_change_suffix

Selected candidate: attachment a=16, projected [16,24] (A/pid=101/fork=0).
Projection: `{"life":[0,null],"coverage":[[16,24]],"waits":[],"open":["PgSleep",18,24]}`
Replay: Changed event_ring_drop counters turn FRONTIER 12 into VOID; lifecycle remains eligible, and fresh ATTACH_OK 16 recovers (§§4,5).

```json
{"metric":"wait_time","version":1,"value":6,"unit":"ns","basis":"observed","completeness":"complete","reasons":[],"detail":{"closed_ns":0,"open_ns":6},"diagnostic":{"recorded_ns":6,"covered_ns":8,"required_ns":8}}
{"metric":"completed_waits","version":1,"value":0,"unit":"count","basis":"observed","completeness":"complete","reasons":[],"diagnostic":{"recorded_count":0,"covered_ns":8,"required_ns":8}}
```

## 40_attach_counter_change_prefix

Selected candidate: attachment a=1, projected [1,8] (A/pid=101/fork=0).
Projection: `{"life":[0,null],"coverage":[[1,8]],"waits":[],"open":["PgSleep",3,8]}`
Replay: ATTACH_OK 12 is consumed only as a lifecycle-loss VOID, before overlap checks; old-generation recovery is blocked (§§4,5 counter rule). Retain the target's certified prefix at 8.

```json
{"metric":"wait_time","version":1,"value":5,"unit":"ns","basis":"observed","completeness":"complete","reasons":[],"detail":{"closed_ns":0,"open_ns":5},"diagnostic":{"recorded_ns":5,"covered_ns":7,"required_ns":7}}
{"metric":"completed_waits","version":1,"value":0,"unit":"count","basis":"observed","completeness":"complete","reasons":[],"diagnostic":{"recorded_count":0,"covered_ns":7,"required_ns":7}}
```

## 41_attach_counter_change_suffix

Selected candidate: attachment a=1, projected [1,8] (A/pid=101/fork=0).
Projection: `{"life":[0,null],"coverage":[[1,8]],"waits":[],"open":["PgSleep",3,8]}`
Replay: ATTACH_OK 12 is consumed only as a lifecycle-loss VOID; the target fork 0 remains frozen despite the later fresh fork 26 (§§4,5). Retain the target's certified prefix at 8.

```json
{"metric":"wait_time","version":1,"value":null,"unit":"ns","basis":"observed","completeness":"unavailable","reasons":["coverage_incomplete"],"diagnostic":{"recorded_ns":0,"covered_ns":0,"required_ns":8}}
{"metric":"completed_waits","version":1,"value":null,"unit":"count","basis":"observed","completeness":"unavailable","reasons":["coverage_incomplete"],"diagnostic":{"recorded_count":0,"covered_ns":0,"required_ns":8}}
```

## 42_overlapping_attachment_prefix

Selected candidate: attachment a=1, projected [1,8] (A/pid=101/fork=0).
Projection: `{"life":[0,null],"coverage":[[1,8]],"waits":[],"open":["PgSleep",3,8]}`
Replay: Overlapping ATTACH_OK 12 stops the chain before that attachment (§5.2). Retain the target's certified prefix at 8.

```json
{"metric":"wait_time","version":1,"value":5,"unit":"ns","basis":"observed","completeness":"complete","reasons":[],"detail":{"closed_ns":0,"open_ns":5},"diagnostic":{"recorded_ns":5,"covered_ns":7,"required_ns":7}}
{"metric":"completed_waits","version":1,"value":0,"unit":"count","basis":"observed","completeness":"complete","reasons":[],"diagnostic":{"recorded_count":0,"covered_ns":7,"required_ns":7}}
```

## 43_overlapping_attachment_suffix

Selected candidate: attachment a=1, projected [1,8] (A/pid=101/fork=0).
Projection: `{"life":[0,null],"coverage":[[1,8]],"waits":[],"open":["PgSleep",3,8]}`
Replay: Overlapping ATTACH_OK 12 stops the chain before that attachment (§5.2). Retain the target's certified prefix at 8.

```json
{"metric":"wait_time","version":1,"value":null,"unit":"ns","basis":"observed","completeness":"unavailable","reasons":["coverage_incomplete"],"diagnostic":{"recorded_ns":0,"covered_ns":0,"required_ns":8}}
{"metric":"completed_waits","version":1,"value":null,"unit":"count","basis":"observed","completeness":"unavailable","reasons":["coverage_incomplete"],"diagnostic":{"recorded_count":0,"covered_ns":0,"required_ns":8}}
```

## 44_end_without_run_prefix

Selected candidate: attachment a=1, projected [1,8] (A/pid=101/fork=0).
Projection: `{"life":[0,null],"coverage":[[1,8]],"waits":[],"open":["PgSleep",3,8]}`
Replay: END 12 for PID 202 has no addressable run; stop before that END (§5.3). Retain the target's certified prefix at 8.

```json
{"metric":"wait_time","version":1,"value":5,"unit":"ns","basis":"observed","completeness":"complete","reasons":[],"detail":{"closed_ns":0,"open_ns":5},"diagnostic":{"recorded_ns":5,"covered_ns":7,"required_ns":7}}
{"metric":"completed_waits","version":1,"value":0,"unit":"count","basis":"observed","completeness":"complete","reasons":[],"diagnostic":{"recorded_count":0,"covered_ns":7,"required_ns":7}}
```

## 45_end_without_run_suffix

Selected candidate: attachment a=1, projected [1,8] (A/pid=101/fork=0).
Projection: `{"life":[0,null],"coverage":[[1,8]],"waits":[],"open":["PgSleep",3,8]}`
Replay: END 12 for PID 202 has no addressable run; stop before that END (§5.3). Retain the target's certified prefix at 8.

```json
{"metric":"wait_time","version":1,"value":null,"unit":"ns","basis":"observed","completeness":"unavailable","reasons":["coverage_incomplete"],"diagnostic":{"recorded_ns":0,"covered_ns":0,"required_ns":8}}
{"metric":"completed_waits","version":1,"value":null,"unit":"count","basis":"observed","completeness":"unavailable","reasons":["coverage_incomplete"],"diagnostic":{"recorded_count":0,"covered_ns":0,"required_ns":8}}
```

## 46_nonadvancing_frontier_prefix

Selected candidate: attachment a=1, projected [1,8] (A/pid=101/fork=0).
Projection: `{"life":[0,null],"coverage":[[1,8]],"waits":[],"open":["PgSleep",3,8]}`
Replay: The second physical FRONTIER 8 is nonadvancing; stop before it without deduplicating it (§5.4). Retain the target's certified prefix at 8.

```json
{"metric":"wait_time","version":1,"value":5,"unit":"ns","basis":"observed","completeness":"complete","reasons":[],"detail":{"closed_ns":0,"open_ns":5},"diagnostic":{"recorded_ns":5,"covered_ns":7,"required_ns":7}}
{"metric":"completed_waits","version":1,"value":0,"unit":"count","basis":"observed","completeness":"complete","reasons":[],"diagnostic":{"recorded_count":0,"covered_ns":7,"required_ns":7}}
```

## 47_nonadvancing_frontier_suffix

Selected candidate: attachment a=1, projected [1,8] (A/pid=101/fork=0).
Projection: `{"life":[0,null],"coverage":[[1,8]],"waits":[],"open":["PgSleep",3,8]}`
Replay: The second physical FRONTIER 8 is nonadvancing; stop before it (§5.4). Retain the target's certified prefix at 8.

```json
{"metric":"wait_time","version":1,"value":null,"unit":"ns","basis":"observed","completeness":"unavailable","reasons":["coverage_incomplete"],"diagnostic":{"recorded_ns":0,"covered_ns":0,"required_ns":15}}
{"metric":"completed_waits","version":1,"value":null,"unit":"count","basis":"observed","completeness":"unavailable","reasons":["coverage_incomplete"],"diagnostic":{"recorded_count":0,"covered_ns":0,"required_ns":15}}
```

## 48_run_state_mismatch_prefix

Selected candidate: attachment a=1, projected [1,8] (A/pid=101/fork=0).
Projection: `{"life":[0,null],"coverage":[[1,8]],"waits":[],"open":["PgSleep",3,8]}`
Replay: TRANS 12 has old=Lock while current state is PgSleep; cut at that TRANS after run resolution (§5.6). Retain the target's certified prefix at 8.

```json
{"metric":"wait_time","version":1,"value":5,"unit":"ns","basis":"observed","completeness":"complete","reasons":[],"detail":{"closed_ns":0,"open_ns":5},"diagnostic":{"recorded_ns":5,"covered_ns":7,"required_ns":7}}
{"metric":"completed_waits","version":1,"value":0,"unit":"count","basis":"observed","completeness":"complete","reasons":[],"diagnostic":{"recorded_count":0,"covered_ns":7,"required_ns":7}}
```

## 49_run_state_mismatch_suffix

Selected candidate: attachment a=1, projected [1,8] (A/pid=101/fork=0).
Projection: `{"life":[0,null],"coverage":[[1,8]],"waits":[],"open":["PgSleep",3,8]}`
Replay: TRANS 12 has old=Lock while current state is PgSleep; cut at that TRANS after run resolution (§5.6). Retain the target's certified prefix at 8.

```json
{"metric":"wait_time","version":1,"value":null,"unit":"ns","basis":"observed","completeness":"unavailable","reasons":["coverage_incomplete"],"diagnostic":{"recorded_ns":0,"covered_ns":0,"required_ns":8}}
{"metric":"completed_waits","version":1,"value":null,"unit":"count","basis":"observed","completeness":"unavailable","reasons":["coverage_incomplete"],"diagnostic":{"recorded_count":0,"covered_ns":0,"required_ns":8}}
```

## 50_capture_boot_conflict_prefix

Selected candidate: attachment a=1, projected [1,8] (A/pid=101/fork=0).
Projection: `{"life":[0,null],"coverage":[[1,8]],"waits":[],"open":["PgSleep",3,8]}`
Replay: The boot-2 descriptor conflicts with capture A's bound boot-1; ignore it and every later A file (§5.5; §6). Retain the target's certified prefix at 8.

```json
{"metric":"wait_time","version":1,"value":5,"unit":"ns","basis":"observed","completeness":"complete","reasons":[],"detail":{"closed_ns":0,"open_ns":5},"diagnostic":{"recorded_ns":5,"covered_ns":7,"required_ns":7}}
{"metric":"completed_waits","version":1,"value":0,"unit":"count","basis":"observed","completeness":"complete","reasons":[],"diagnostic":{"recorded_count":0,"covered_ns":7,"required_ns":7}}
```

## 51_capture_boot_conflict_suffix

Selected candidate: attachment a=1, projected [1,8] (A/pid=101/fork=0).
Projection: `{"life":[0,null],"coverage":[[1,8]],"waits":[],"open":["PgSleep",3,8]}`
Replay: The boot-2 descriptor conflicts with capture A's bound boot-1; ignore it and every later A file (§5.5; §6). Retain the target's certified prefix at 8.

```json
{"metric":"wait_time","version":1,"value":null,"unit":"ns","basis":"observed","completeness":"unavailable","reasons":["coverage_incomplete"],"diagnostic":{"recorded_ns":0,"covered_ns":0,"required_ns":8}}
{"metric":"completed_waits","version":1,"value":null,"unit":"count","basis":"observed","completeness":"unavailable","reasons":["coverage_incomplete"],"diagnostic":{"recorded_count":0,"covered_ns":0,"required_ns":8}}
```

## 52_malformed_before_frontier_prefix

Selected candidate: none (A/pid=101/fork=0).
Projection: `{"life":[0,null],"coverage":[],"waits":[],"open":null}`
Replay: The TRANS missing t stops the chain before any FRONTIER. Keep independently verified LIFE and discard the tentative run (§5 prefix rule).

```json
{"metric":"wait_time","version":1,"value":null,"unit":"ns","basis":"observed","completeness":"unavailable","reasons":["coverage_incomplete"],"diagnostic":{"recorded_ns":0,"covered_ns":0,"required_ns":1}}
{"metric":"completed_waits","version":1,"value":null,"unit":"count","basis":"observed","completeness":"unavailable","reasons":["coverage_incomplete"],"diagnostic":{"recorded_count":0,"covered_ns":0,"required_ns":1}}
```

## 53_malformed_before_frontier_suffix

Selected candidate: none (A/pid=101/fork=0).
Projection: `{"life":[0,null],"coverage":[],"waits":[],"open":null}`
Replay: The TRANS missing t stops the chain before any FRONTIER. Ignore the successor; retain LIFE but no candidate (§5 prefix rule).

```json
{"metric":"wait_time","version":1,"value":null,"unit":"ns","basis":"observed","completeness":"unavailable","reasons":["coverage_incomplete"],"diagnostic":{"recorded_ns":0,"covered_ns":0,"required_ns":8}}
{"metric":"completed_waits","version":1,"value":null,"unit":"count","basis":"observed","completeness":"unavailable","reasons":["coverage_incomplete"],"diagnostic":{"recorded_count":0,"covered_ns":0,"required_ns":8}}
```

## 54_independent_capture_after_fault

Selected candidate: attachment a=1, projected [1,24] (B/pid=101/fork=0).
Projection: `{"life":[0,null],"coverage":[[1,24]],"waits":[],"open":["PgSleep",3,24]}`
Replay: Capture A stops at its malformed TRANS; independent target capture B remains readable through FRONTIER 24 (§§5,6).

```json
{"metric":"wait_time","version":1,"value":8,"unit":"ns","basis":"observed","completeness":"complete","reasons":[],"detail":{"closed_ns":0,"open_ns":8},"diagnostic":{"recorded_ns":8,"covered_ns":8,"required_ns":8}}
{"metric":"completed_waits","version":1,"value":0,"unit":"count","basis":"observed","completeness":"complete","reasons":[],"diagnostic":{"recorded_count":0,"covered_ns":8,"required_ns":8}}
```

## 55_run_duration_mismatch

Selected candidate: attachment a=1, projected [1,8] (A/pid=101/fork=0).
Projection: `{"life":[0,null],"coverage":[[1,8]],"waits":[],"open":["PgSleep",3,8]}`
Replay: TRANS 12 has 12−8=4, inconsistent with known entry 3; stop before that TRANS (§5.6). Retain the target's certified prefix at 8.

```json
{"metric":"wait_time","version":1,"value":null,"unit":"ns","basis":"observed","completeness":"unavailable","reasons":["coverage_incomplete"],"diagnostic":{"recorded_ns":5,"covered_ns":7,"required_ns":23}}
{"metric":"completed_waits","version":1,"value":null,"unit":"count","basis":"observed","completeness":"unavailable","reasons":["coverage_incomplete"],"diagnostic":{"recorded_count":0,"covered_ns":7,"required_ns":23}}
```

## 56_attachment_before_life

Selected candidate: attachment a=1, projected [1,8] (A/pid=101/fork=0).
Projection: `{"life":[0,null],"coverage":[[1,8]],"waits":[],"open":["PgSleep",3,8]}`
Replay: ATTACH_OK 4 for PID 202 precedes LIFE.start=5; cut at its physical position, after FRONTIER 8 (§5.2 and prefix rule). Retain the target's certified prefix at 8.

```json
{"metric":"wait_time","version":1,"value":null,"unit":"ns","basis":"observed","completeness":"unavailable","reasons":["coverage_incomplete"],"diagnostic":{"recorded_ns":5,"covered_ns":7,"required_ns":23}}
{"metric":"completed_waits","version":1,"value":null,"unit":"count","basis":"observed","completeness":"unavailable","reasons":["coverage_incomplete"],"diagnostic":{"recorded_count":0,"covered_ns":7,"required_ns":23}}
```

## 57_nonadvancing_after_rollback

Selected candidate: LIFE_INVALID (A/pid=101/fork=15).
Projection: `{"life":null,"coverage":[],"waits":[],"open":null}`
Replay: VOID revokes FRONTIER 10 and rolls F back to 8, but H remains 10. FRONTIER 9 stops the chain; target LIFE(fork=15) in the successor is never read (§4; §5.4).

```json
{"metric":"wait_time","version":1,"value":null,"unit":"ns","basis":"observed","completeness":"unavailable","reasons":["evidence_invalid"],"diagnostic":{"recorded_ns":null,"covered_ns":null,"required_ns":null}}
{"metric":"completed_waits","version":1,"value":null,"unit":"count","basis":"observed","completeness":"unavailable","reasons":["evidence_invalid"],"diagnostic":{"recorded_count":null,"covered_ns":null,"required_ns":null}}
```

## 58_counter_loss_fresh_fork

Selected candidate: attachment a=27, projected [27,32] (A/pid=101/fork=26).
Projection: `{"life":[26,null],"coverage":[[27,32]],"waits":[],"open":["PgSleep",29,32]}`
Replay: Ignore blocked old-generation ATTACH_OK 16. Fresh observed fork 26 after lifecycle-loss time 12, then fresh attachment, restores eligibility (§4).

```json
{"metric":"wait_time","version":1,"value":3,"unit":"ns","basis":"observed","completeness":"complete","reasons":[],"detail":{"closed_ns":0,"open_ns":3},"diagnostic":{"recorded_ns":3,"covered_ns":5,"required_ns":5}}
{"metric":"completed_waits","version":1,"value":0,"unit":"count","basis":"observed","completeness":"complete","reasons":[],"diagnostic":{"recorded_count":0,"covered_ns":5,"required_ns":5}}
```

## 59_frontier_counter_recovery

Selected candidate: attachment a=14, projected [14,25] (A/pid=101/fork=0).
Projection: `{"life":[0,null],"coverage":[[14,25]],"waits":[["PgSleep",17,22]],"open":null}`
Replay: FRONTIER 12 with changed query_text_drop counters acts only as VOID; fresh ATTACH_OK 14 recovers and its run covers the query (§5 counter rule).

```json
{"metric":"wait_time","version":1,"value":5,"unit":"ns","basis":"observed","completeness":"complete","reasons":[],"detail":{"closed_ns":5,"open_ns":0},"diagnostic":{"recorded_ns":5,"covered_ns":11,"required_ns":11}}
{"metric":"completed_waits","version":1,"value":1,"unit":"count","basis":"observed","completeness":"complete","reasons":[],"diagnostic":{"recorded_count":1,"covered_ns":11,"required_ns":11}}
```

## 60_attach_counter_recovery

Selected candidate: attachment a=15, projected [15,24] (A/pid=101/fork=0).
Projection: `{"life":[0,null],"coverage":[[15,24]],"waits":[["PgSleep",17,21]],"open":null}`
Replay: Consume changed-counter ATTACH_OK 12 only as VOID before overlap checking; fresh ATTACH_OK 15 creates the recovered run (§5 counter rule).

```json
{"metric":"wait_time","version":1,"value":4,"unit":"ns","basis":"observed","completeness":"complete","reasons":[],"detail":{"closed_ns":4,"open_ns":0},"diagnostic":{"recorded_ns":4,"covered_ns":9,"required_ns":9}}
{"metric":"completed_waits","version":1,"value":1,"unit":"count","basis":"observed","completeness":"complete","reasons":[],"diagnostic":{"recorded_count":1,"covered_ns":9,"required_ns":9}}
```

## 61_blocked_attach_fresh_generation

Selected candidate: attachment a=21, projected [21,31] (A/pid=101/fork=20).
Projection: `{"life":[20,null],"coverage":[[21,31]],"waits":[["PgSleep",23,28]],"open":null}`
Replay: Ignore lifecycle-blocked ATTACH_OK 16. Fresh observed fork 20 and attachment 21 permit the new generation's certified run (§4).

```json
{"metric":"wait_time","version":1,"value":5,"unit":"ns","basis":"observed","completeness":"complete","reasons":[],"detail":{"closed_ns":5,"open_ns":0},"diagnostic":{"recorded_ns":5,"covered_ns":10,"required_ns":10}}
{"metric":"completed_waits","version":1,"value":1,"unit":"count","basis":"observed","completeness":"complete","reasons":[],"diagnostic":{"recorded_count":1,"covered_ns":10,"required_ns":10}}
```

## 62_transition_outside_runs

Selected candidate: attachment a=16, projected [16,26] (A/pid=101/fork=0).
Projection: `{"life":[0,null],"coverage":[[16,26]],"waits":[["PgSleep",18,23]],"open":null}`
Replay: TRANS 12 lies after END 9 and before attachment 16, so ignore it without fault; select the recovered attachment (§5.6).

```json
{"metric":"wait_time","version":1,"value":5,"unit":"ns","basis":"observed","completeness":"complete","reasons":[],"detail":{"closed_ns":5,"open_ns":0},"diagnostic":{"recorded_ns":5,"covered_ns":10,"required_ns":10}}
{"metric":"completed_waits","version":1,"value":1,"unit":"count","basis":"observed","completeness":"complete","reasons":[],"diagnostic":{"recorded_count":1,"covered_ns":10,"required_ns":10}}
```

## 63_recovery_before_void_time

Selected candidate: attachment a=1, projected [1,8] (A/pid=101/fork=0).
Projection: `{"life":[0,null],"coverage":[[1,8]],"waits":[],"open":["PgSleep",3,8]}`
Replay: Recovery ATTACH_OK 11 is earlier than latest VOID.t=12; stop before it, ignoring all later recovery records (§4, lines 199–201). Retain the target's certified prefix at 8.

```json
{"metric":"wait_time","version":1,"value":null,"unit":"ns","basis":"observed","completeness":"unavailable","reasons":["coverage_incomplete"],"diagnostic":{"recorded_ns":0,"covered_ns":0,"required_ns":10}}
{"metric":"completed_waits","version":1,"value":null,"unit":"count","basis":"observed","completeness":"unavailable","reasons":["coverage_incomplete"],"diagnostic":{"recorded_count":0,"covered_ns":0,"required_ns":10}}
```

## 64_end_after_blocked_attach

Selected candidate: attachment a=21, projected [21,30] (A/pid=101/fork=20).
Projection: `{"life":[20,null],"coverage":[[21,30]],"waits":[["PgSleep",23,27]],"open":null}`
Replay: Ignore lifecycle-blocked ATTACH_OK 16 and schema-valid END 18 for the tainted generation; fresh fork 20 and attachment 21 remain valid (§4; §5.3).

```json
{"metric":"wait_time","version":1,"value":4,"unit":"ns","basis":"observed","completeness":"complete","reasons":[],"detail":{"closed_ns":4,"open_ns":0},"diagnostic":{"recorded_ns":4,"covered_ns":9,"required_ns":9}}
{"metric":"completed_waits","version":1,"value":1,"unit":"count","basis":"observed","completeness":"complete","reasons":[],"diagnostic":{"recorded_count":1,"covered_ns":9,"required_ns":9}}
```

## Ambiguities

none
