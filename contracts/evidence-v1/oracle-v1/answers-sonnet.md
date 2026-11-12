## 01_ordinary_run

Projection: life=[0,null) | run reread a=1, F=15, b=15 | coverage=[[1,15]] | waits=[PgSleep 3-7, Lock 9-12], open=null

```json
{"metric": "wait_time", "version": 1, "value": 4, "unit": "ns", "basis": "observed", "completeness": "complete", "reasons": [], "detail": {"closed_ns": 4, "open_ns": 0}, "diagnostic": {"recorded_ns": 4, "covered_ns": 14, "required_ns": 14}}
{"metric": "completed_waits", "version": 1, "value": 1, "unit": "count", "basis": "observed", "completeness": "complete", "reasons": [], "diagnostic": {"recorded_count": 1, "covered_ns": 14, "required_ns": 14}}
```

## 02_pid_reuse_delayed_life

Projection: target fork 12: life=[12,null); fork-0 generation (exit 8, EXIT@8) is separate, T@15 belongs to fork 12 | run reread a=13, F=20 | coverage=[[13,20]] | waits=[], open=[Lock,15,20]

```json
{"metric": "wait_time", "version": 1, "value": 5, "unit": "ns", "basis": "observed", "completeness": "complete", "reasons": [], "detail": {"closed_ns": 0, "open_ns": 5}, "diagnostic": {"recorded_ns": 5, "covered_ns": 7, "required_ns": 7}}
{"metric": "completed_waits", "version": 1, "value": 0, "unit": "count", "basis": "observed", "completeness": "complete", "reasons": [], "diagnostic": {"recorded_count": 0, "covered_ns": 7, "required_ns": 7}}
```

## 03_exit_inside_window

Projection: life=[0,10) | run a=1, F=12, b=min(12,exit 10)=10, certified EXIT with known entry | coverage=[[1,10]] | waits=[PgSleep 3-10], open=null

```json
{"metric": "wait_time", "version": 1, "value": 5, "unit": "ns", "basis": "observed", "completeness": "complete", "reasons": [], "detail": {"closed_ns": 5, "open_ns": 0}, "diagnostic": {"recorded_ns": 5, "covered_ns": 5, "required_ns": 5}}
{"metric": "completed_waits", "version": 1, "value": 1, "unit": "count", "basis": "observed", "completeness": "complete", "reasons": [], "diagnostic": {"recorded_count": 1, "covered_ns": 5, "required_ns": 5}}
```

## 04_window_starts_at_exit

Projection: life=[0,10) | same run as 03 | coverage=[[1,10]] | waits=[PgSleep 3-10], open=null | R=[10,12)∩[0,10)=empty

```json
{"metric": "wait_time", "version": 1, "value": 0, "unit": "ns", "basis": "observed", "completeness": "complete", "reasons": [], "detail": {"closed_ns": 0, "open_ns": 0}, "diagnostic": {"recorded_ns": 0, "covered_ns": 0, "required_ns": 0}}
{"metric": "completed_waits", "version": 1, "value": 1, "unit": "count", "basis": "observed", "completeness": "complete", "reasons": [], "diagnostic": {"recorded_count": 1, "covered_ns": 0, "required_ns": 0}}
```

## 05_known_exit_lost_terminal

Projection: life=[0,12) (LIFE exit survives) | run a=1; VOID@14 (invalid_from null) freezes at F=8, b=8 (not exit, no certified EXIT) | coverage=[[1,8]] | waits=[], open=[PgSleep,3,8]

```json
{"metric": "wait_time", "version": 1, "value": null, "unit": "ns", "basis": "observed", "completeness": "unavailable", "reasons": ["coverage_incomplete"], "diagnostic": {"recorded_ns": 2, "covered_ns": 2, "required_ns": 6}}
{"metric": "completed_waits", "version": 1, "value": null, "unit": "count", "basis": "observed", "completeness": "unavailable", "reasons": ["coverage_incomplete"], "diagnostic": {"recorded_count": 0, "covered_ns": 2, "required_ns": 6}}
```

## 06_preexisting_before_capture

Projection: life=[0,null) (start_known=false, lower bound 0) | run reread null a=12, F=20 | coverage=[[12,20]] | waits=[], open=[PgSleep,14,20]

```json
{"metric": "wait_time", "version": 1, "value": null, "unit": "ns", "basis": "observed", "completeness": "unavailable", "reasons": ["coverage_incomplete"], "diagnostic": {"recorded_ns": 6, "covered_ns": 8, "required_ns": 18}}
{"metric": "completed_waits", "version": 1, "value": null, "unit": "count", "basis": "observed", "completeness": "unavailable", "reasons": ["coverage_incomplete"], "diagnostic": {"recorded_count": 0, "covered_ns": 8, "required_ns": 18}}
```

## 07_preexisting_covered_window

Projection: life=[0,null) | run reread null a=12, F=20 | coverage=[[12,20]] | waits=[], open=[PgSleep,14,20]

```json
{"metric": "wait_time", "version": 1, "value": 6, "unit": "ns", "basis": "observed", "completeness": "complete", "reasons": [], "detail": {"closed_ns": 0, "open_ns": 6}, "diagnostic": {"recorded_ns": 6, "covered_ns": 8, "required_ns": 8}}
{"metric": "completed_waits", "version": 1, "value": 0, "unit": "count", "basis": "observed", "completeness": "complete", "reasons": [], "diagnostic": {"recorded_count": 0, "covered_ns": 8, "required_ns": 8}}
```

## 08_attach_failure

Projection: life=[0,null) | no ATTACH_OK, no run; frontiers certify no backend | coverage=[] | waits=[], open=null (valid LIFE, no candidate)

```json
{"metric": "wait_time", "version": 1, "value": null, "unit": "ns", "basis": "observed", "completeness": "unavailable", "reasons": ["coverage_incomplete"], "diagnostic": {"recorded_ns": 0, "covered_ns": 0, "required_ns": 19}}
{"metric": "completed_waits", "version": 1, "value": null, "unit": "count", "basis": "observed", "completeness": "unavailable", "reasons": ["coverage_incomplete"], "diagnostic": {"recorded_count": 0, "covered_ns": 0, "required_ns": 19}}
```

## 09_seed_mismatch_then_transition

Projection: life=[0,null) | transition anchor a=8 (old=Lock nonnull), state PgSleep entry 8, F=15; trimmed start 9 | coverage=[[9,15]] | waits=[], open=[PgSleep,8,15]

```json
{"metric": "wait_time", "version": 1, "value": null, "unit": "ns", "basis": "observed", "completeness": "unavailable", "reasons": ["coverage_incomplete"], "diagnostic": {"recorded_ns": 7, "covered_ns": 6, "required_ns": 7}}
{"metric": "completed_waits", "version": 1, "value": null, "unit": "count", "basis": "observed", "completeness": "unavailable", "reasons": ["coverage_incomplete"], "diagnostic": {"recorded_count": 0, "covered_ns": 6, "required_ns": 7}}
```

## 10_equal_nonzero_seed

Projection: life=[0,null) | reread nonzero seed PgSleep, never leaves: unknown entry, no core portion | coverage=[] | waits=[], open=null

```json
{"metric": "wait_time", "version": 1, "value": null, "unit": "ns", "basis": "observed", "completeness": "unavailable", "reasons": ["coverage_incomplete"], "diagnostic": {"recorded_ns": 0, "covered_ns": 0, "required_ns": 8}}
{"metric": "completed_waits", "version": 1, "value": null, "unit": "count", "basis": "observed", "completeness": "unavailable", "reasons": ["coverage_incomplete"], "diagnostic": {"recorded_count": 0, "covered_ns": 0, "required_ns": 8}}
```

## 11_unknown_entry_close_boundary

Projection: life=[0,null) | reread seed PgSleep a=12, leaves at u=16, begin u+1=17; F=25 | coverage=[[17,25]] | waits=[Lock 16-22] (known entry, kept despite start<17), open=null

```json
{"metric": "wait_time", "version": 1, "value": null, "unit": "ns", "basis": "observed", "completeness": "unavailable", "reasons": ["coverage_incomplete"], "diagnostic": {"recorded_ns": 6, "covered_ns": 8, "required_ns": 9}}
{"metric": "completed_waits", "version": 1, "value": null, "unit": "count", "basis": "observed", "completeness": "unavailable", "reasons": ["coverage_incomplete"], "diagnostic": {"recorded_count": 1, "covered_ns": 8, "required_ns": 9}}
```

## 12_loss_then_recovery_crossing

Projection: life=[0,null) | run1 a=1 frozen at F=8 by VOID@14 (T@11 uncertified, dropped): cov [1,8]; run2 reread a=16, F=24: cov [16,24]. Slot 11 is not lifecycle loss so reattach allowed | selected run2 (intersection with R=[4,24): 8 > 4) | coverage=[[16,24]] | waits=[], open=[PgSleep,18,24]

```json
{"metric": "wait_time", "version": 1, "value": null, "unit": "ns", "basis": "observed", "completeness": "unavailable", "reasons": ["coverage_incomplete"], "diagnostic": {"recorded_ns": 6, "covered_ns": 8, "required_ns": 20}}
{"metric": "completed_waits", "version": 1, "value": null, "unit": "count", "basis": "observed", "completeness": "unavailable", "reasons": ["coverage_incomplete"], "diagnostic": {"recorded_count": 0, "covered_ns": 8, "required_ns": 20}}
```

## 13_loss_then_recovery_later_window

Projection: life=[0,null) | same runs as 12 | R=[16,24) fully covered by run2 -> selected | coverage=[[16,24]] | waits=[], open=[PgSleep,18,24]

```json
{"metric": "wait_time", "version": 1, "value": 6, "unit": "ns", "basis": "observed", "completeness": "complete", "reasons": [], "detail": {"closed_ns": 0, "open_ns": 6}, "diagnostic": {"recorded_ns": 6, "covered_ns": 8, "required_ns": 8}}
{"metric": "completed_waits", "version": 1, "value": 0, "unit": "count", "basis": "observed", "completeness": "complete", "reasons": [], "diagnostic": {"recorded_count": 0, "covered_ns": 8, "required_ns": 8}}
```

## 14_global_loss_other_backend

Projection: life=[0,null) | run a=1; VOID@12 (slot 4 query_text_drop, global) freezes all runs at F=8, no reattach for pid 101 | coverage=[[1,8]] | waits=[], open=[PgSleep,3,8]

```json
{"metric": "wait_time", "version": 1, "value": null, "unit": "ns", "basis": "observed", "completeness": "unavailable", "reasons": ["coverage_incomplete"], "diagnostic": {"recorded_ns": 5, "covered_ns": 5, "required_ns": 17}}
{"metric": "completed_waits", "version": 1, "value": null, "unit": "count", "basis": "observed", "completeness": "unavailable", "reasons": ["coverage_incomplete"], "diagnostic": {"recorded_count": 0, "covered_ns": 5, "required_ns": 17}}
```

## 15_frontier_without_reattach

Projection: life=[0,null) | run a=1 frozen at F=8 by VOID@12; F=20 cannot revive it | coverage=[[1,8]] | waits=[], open=[PgSleep,3,8] | R=[8,20) disjoint from coverage

```json
{"metric": "wait_time", "version": 1, "value": null, "unit": "ns", "basis": "observed", "completeness": "unavailable", "reasons": ["coverage_incomplete"], "diagnostic": {"recorded_ns": 0, "covered_ns": 0, "required_ns": 12}}
{"metric": "completed_waits", "version": 1, "value": null, "unit": "count", "basis": "observed", "completeness": "unavailable", "reasons": ["coverage_incomplete"], "diagnostic": {"recorded_count": 0, "covered_ns": 0, "required_ns": 12}}
```

## 16_crash_with_uncertified_suffix

Projection: life=[0,null) | run a=1, last F=8 (T@12 beyond F ignored, no END) | coverage=[[1,8]] | waits=[], open=[PgSleep,3,8]

```json
{"metric": "wait_time", "version": 1, "value": null, "unit": "ns", "basis": "observed", "completeness": "unavailable", "reasons": ["coverage_incomplete"], "diagnostic": {"recorded_ns": 5, "covered_ns": 5, "required_ns": 9}}
{"metric": "completed_waits", "version": 1, "value": null, "unit": "count", "basis": "observed", "completeness": "unavailable", "reasons": ["coverage_incomplete"], "diagnostic": {"recorded_count": 0, "covered_ns": 5, "required_ns": 9}}
```

## 17_wait_spans_frontiers

Projection: life=[0,null) | run a=1, latest F=20 | coverage=[[1,20]] | waits=[], open=[PgSleep,3,20]

```json
{"metric": "wait_time", "version": 1, "value": 12, "unit": "ns", "basis": "observed", "completeness": "complete", "reasons": [], "detail": {"closed_ns": 0, "open_ns": 12}, "diagnostic": {"recorded_ns": 12, "covered_ns": 12, "required_ns": 12}}
{"metric": "completed_waits", "version": 1, "value": 0, "unit": "count", "basis": "observed", "completeness": "complete", "reasons": [], "diagnostic": {"recorded_count": 0, "covered_ns": 12, "required_ns": 12}}
```

## 18_detach_mid_wait

Projection: life=[0,null) | run a=1, END=12 certified by F=15, b=min(15,12)=12 | coverage=[[1,12]] | waits=[], open=[PgSleep,3,12]

```json
{"metric": "wait_time", "version": 1, "value": 9, "unit": "ns", "basis": "observed", "completeness": "complete", "reasons": [], "detail": {"closed_ns": 0, "open_ns": 9}, "diagnostic": {"recorded_ns": 9, "covered_ns": 9, "required_ns": 9}}
{"metric": "completed_waits", "version": 1, "value": 0, "unit": "count", "basis": "observed", "completeness": "complete", "reasons": [], "diagnostic": {"recorded_count": 0, "covered_ns": 9, "required_ns": 9}}
```

## 19_end_without_final_frontier

Projection: life=[0,null) | run a=1, END@12 pending (no frontier reaches it), F=8 | coverage=[[1,8]] | waits=[], open=[PgSleep,3,8]

```json
{"metric": "wait_time", "version": 1, "value": null, "unit": "ns", "basis": "observed", "completeness": "unavailable", "reasons": ["coverage_incomplete"], "diagnostic": {"recorded_ns": 5, "covered_ns": 5, "required_ns": 9}}
{"metric": "completed_waits", "version": 1, "value": null, "unit": "count", "basis": "observed", "completeness": "unavailable", "reasons": ["coverage_incomplete"], "diagnostic": {"recorded_count": 0, "covered_ns": 5, "required_ns": 9}}
```

## 20_two_escalation_windows

Projection: life=[0,null) | run1 a=1,b=8 cov [1,8] open [PgSleep,3,8]; run2 a=15,b=23 cov [15,23] open [PgSleep,17,23]; R=[1,23): run2 intersection 8 > run1 7 -> run2 | coverage=[[15,23]] | waits=[], open=[PgSleep,17,23]

```json
{"metric": "wait_time", "version": 1, "value": null, "unit": "ns", "basis": "observed", "completeness": "unavailable", "reasons": ["coverage_incomplete"], "diagnostic": {"recorded_ns": 6, "covered_ns": 8, "required_ns": 22}}
{"metric": "completed_waits", "version": 1, "value": null, "unit": "count", "basis": "observed", "completeness": "unavailable", "reasons": ["coverage_incomplete"], "diagnostic": {"recorded_count": 0, "covered_ns": 8, "required_ns": 22}}
```

## 21_second_escalation_window

Projection: life=[0,null) | same runs as 20; R=[15,23) fully covered by run2 | coverage=[[15,23]] | waits=[], open=[PgSleep,17,23]

```json
{"metric": "wait_time", "version": 1, "value": 6, "unit": "ns", "basis": "observed", "completeness": "complete", "reasons": [], "detail": {"closed_ns": 0, "open_ns": 6}, "diagnostic": {"recorded_ns": 6, "covered_ns": 8, "required_ns": 8}}
{"metric": "completed_waits", "version": 1, "value": 0, "unit": "count", "basis": "observed", "completeness": "complete", "reasons": [], "diagnostic": {"recorded_count": 0, "covered_ns": 8, "required_ns": 8}}
```

## 22_rotation_open_wait

Projection: life=[0,null) | complete same-capture chain, no effect; run a=1, F=20 | coverage=[[1,20]] | waits=[PgSleep 3-16], open=null

```json
{"metric": "wait_time", "version": 1, "value": 13, "unit": "ns", "basis": "observed", "completeness": "complete", "reasons": [], "detail": {"closed_ns": 13, "open_ns": 0}, "diagnostic": {"recorded_ns": 13, "covered_ns": 17, "required_ns": 17}}
{"metric": "completed_waits", "version": 1, "value": 1, "unit": "count", "basis": "observed", "completeness": "complete", "reasons": [], "diagnostic": {"recorded_count": 1, "covered_ns": 17, "required_ns": 17}}
```

## 23_rotation_missing_file

Projection: life=[0,null) | chain stops at file 0 (file 1 missing), F=8; file 2 not salvaged | coverage=[[1,8]] | waits=[], open=[PgSleep,3,8]

```json
{"metric": "wait_time", "version": 1, "value": null, "unit": "ns", "basis": "observed", "completeness": "unavailable", "reasons": ["coverage_incomplete"], "diagnostic": {"recorded_ns": 5, "covered_ns": 5, "required_ns": 17}}
{"metric": "completed_waits", "version": 1, "value": null, "unit": "count", "basis": "observed", "completeness": "unavailable", "reasons": ["coverage_incomplete"], "diagnostic": {"recorded_count": 0, "covered_ns": 5, "required_ns": 17}}
```

## 24_restart_same_pid

Projection: target capture B pid 101 fork null: life=[0,null); capture A not joined | run reread a=16, F=24 | coverage=[[16,24]] | waits=[], open=[Lock,18,24]

```json
{"metric": "wait_time", "version": 1, "value": null, "unit": "ns", "basis": "observed", "completeness": "unavailable", "reasons": ["coverage_incomplete"], "diagnostic": {"recorded_ns": 6, "covered_ns": 8, "required_ns": 19}}
{"metric": "completed_waits", "version": 1, "value": null, "unit": "count", "basis": "observed", "completeness": "unavailable", "reasons": ["coverage_incomplete"], "diagnostic": {"recorded_count": 0, "covered_ns": 8, "required_ns": 19}}
```

## 25_distinct_zero_length_waits

Projection: life=[0,null) | run a=1, F=10, four tied transitions in append order | coverage=[[1,10]] | waits=[PgSleep 5-5, PgSleep 5-5] (distinct), open=null

```json
{"metric": "wait_time", "version": 1, "value": 0, "unit": "ns", "basis": "observed", "completeness": "complete", "reasons": [], "detail": {"closed_ns": 0, "open_ns": 0}, "diagnostic": {"recorded_ns": 0, "covered_ns": 3, "required_ns": 3}}
{"metric": "completed_waits", "version": 1, "value": 2, "unit": "count", "basis": "observed", "completeness": "complete", "reasons": [], "diagnostic": {"recorded_count": 2, "covered_ns": 3, "required_ns": 3}}
```

## 26_unreadable_counters

Projection: life=[0,null) | run1 a=1 frozen at F=8 by VOID@11 (counters null = lifecycle uncertainty); VOID@14 readable but preceding baseline unknown; no new LIFE with fresh fork, so ATTACH_OK@16 not eligible, no run2 | coverage=[[1,8]] | waits=[], open=[PgSleep,3,8]

```json
{"metric": "wait_time", "version": 1, "value": null, "unit": "ns", "basis": "observed", "completeness": "unavailable", "reasons": ["coverage_incomplete"], "diagnostic": {"recorded_ns": 4, "covered_ns": 4, "required_ns": 20}}
{"metric": "completed_waits", "version": 1, "value": null, "unit": "count", "basis": "observed", "completeness": "unavailable", "reasons": ["coverage_incomplete"], "diagnostic": {"recorded_count": 0, "covered_ns": 4, "required_ns": 20}}
```

## 27_preempt_rt_unqualified

Projection: life=[0,null) retained (LIFE verified, qualified=false) | certificates ignored | coverage=[] | waits=[], open=null

```json
{"metric": "wait_time", "version": 1, "value": null, "unit": "ns", "basis": "observed", "completeness": "unavailable", "reasons": ["coverage_incomplete"], "diagnostic": {"recorded_ns": 0, "covered_ns": 0, "required_ns": 9}}
{"metric": "completed_waits", "version": 1, "value": null, "unit": "count", "basis": "observed", "completeness": "unavailable", "reasons": ["coverage_incomplete"], "diagnostic": {"recorded_count": 0, "covered_ns": 0, "required_ns": 9}}
```

## 28_legacy_v2

Projection: life=null (LIFE_INVALID; legacy trace has no v5 LIFE) | coverage=[] | waits=[], open=null

```json
{"metric": "wait_time", "version": 1, "value": null, "unit": "ns", "basis": "observed", "completeness": "unavailable", "reasons": ["evidence_invalid"], "diagnostic": {"recorded_ns": null, "covered_ns": null, "required_ns": null}}
{"metric": "completed_waits", "version": 1, "value": null, "unit": "count", "basis": "observed", "completeness": "unavailable", "reasons": ["evidence_invalid"], "diagnostic": {"recorded_count": null, "covered_ns": null, "required_ns": null}}
```

## 29_legacy_v3

Projection: life=null (LIFE_INVALID; legacy trace has no v5 LIFE) | coverage=[] | waits=[], open=null

```json
{"metric": "wait_time", "version": 1, "value": null, "unit": "ns", "basis": "observed", "completeness": "unavailable", "reasons": ["evidence_invalid"], "diagnostic": {"recorded_ns": null, "covered_ns": null, "required_ns": null}}
{"metric": "completed_waits", "version": 1, "value": null, "unit": "count", "basis": "observed", "completeness": "unavailable", "reasons": ["evidence_invalid"], "diagnostic": {"recorded_count": null, "covered_ns": null, "required_ns": null}}
```

## 30_legacy_v4

Projection: life=null (LIFE_INVALID; legacy trace has no v5 LIFE) | coverage=[] | waits=[], open=null

```json
{"metric": "wait_time", "version": 1, "value": null, "unit": "ns", "basis": "observed", "completeness": "unavailable", "reasons": ["evidence_invalid"], "diagnostic": {"recorded_ns": null, "covered_ns": null, "required_ns": null}}
{"metric": "completed_waits", "version": 1, "value": null, "unit": "count", "basis": "observed", "completeness": "unavailable", "reasons": ["evidence_invalid"], "diagnostic": {"recorded_count": null, "covered_ns": null, "required_ns": null}}
```

## 31_late_data_revokes_prefix

Projection: life=[0,null) | VOID@12 invalid_from=1 revokes FRONTIER@10 (>=1); F rolled back to absent; run a=1 has no surviving certificate -> discarded | coverage=[] | waits=[], open=null

```json
{"metric": "wait_time", "version": 1, "value": null, "unit": "ns", "basis": "observed", "completeness": "unavailable", "reasons": ["coverage_incomplete"], "diagnostic": {"recorded_ns": 0, "covered_ns": 0, "required_ns": 9}}
{"metric": "completed_waits", "version": 1, "value": null, "unit": "count", "basis": "observed", "completeness": "unavailable", "reasons": ["coverage_incomplete"], "diagnostic": {"recorded_count": 0, "covered_ns": 0, "required_ns": 9}}
```

## 32_end_before_delayed_transition

Projection: life=[0,null) | run a=1, T@11 (ts<=END 12) belongs to run despite appended after END; END certified by F=15, b=12 | coverage=[[1,12]] | waits=[], open=[PgSleep,11,12]

```json
{"metric": "wait_time", "version": 1, "value": 1, "unit": "ns", "basis": "observed", "completeness": "complete", "reasons": [], "detail": {"closed_ns": 0, "open_ns": 1}, "diagnostic": {"recorded_ns": 1, "covered_ns": 11, "required_ns": 11}}
{"metric": "completed_waits", "version": 1, "value": 0, "unit": "count", "basis": "observed", "completeness": "complete", "reasons": [], "diagnostic": {"recorded_count": 0, "covered_ns": 11, "required_ns": 11}}
```

## 33_anchor_transition_before_attach

Projection: life=[0,null) | transition anchor a=8 (old=Lock nonnull, discarded), state PgSleep entry 8, F=15; trimmed start 9 | coverage=[[9,15]] | waits=[], open=[PgSleep,8,15]

```json
{"metric": "wait_time", "version": 1, "value": null, "unit": "ns", "basis": "observed", "completeness": "unavailable", "reasons": ["coverage_incomplete"], "diagnostic": {"recorded_ns": 7, "covered_ns": 6, "required_ns": 7}}
{"metric": "completed_waits", "version": 1, "value": null, "unit": "count", "basis": "observed", "completeness": "unavailable", "reasons": ["coverage_incomplete"], "diagnostic": {"recorded_count": 0, "covered_ns": 6, "required_ns": 7}}
```

## Ambiguities

none
