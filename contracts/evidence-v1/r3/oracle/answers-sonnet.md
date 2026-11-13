## 01_ordinary_run

Projection:
    life=[0,null) coverage=[[1,15]] waits=[PgSleep 3-7, Lock 9-12] open=null
    reread null run a=1, F=15 b=15; candidate = the one run.

wait_time:
    {"metric":"wait_time","version":1,"value":4,"unit":"ns","basis":"observed","completeness":"complete","reasons":[],"detail":{"closed_ns":4,"open_ns":0},"diagnostic":{"recorded_ns":4,"covered_ns":14,"required_ns":14}}

completed_waits:
    {"metric":"completed_waits","version":1,"value":1,"unit":"count","basis":"observed","completeness":"complete","reasons":[],"diagnostic":{"recorded_count":1,"covered_ns":14,"required_ns":14}}

## 02_pid_reuse_delayed_life

Projection:
    target gen fork 12: life=[12,null) coverage=[[13,20]] waits=[] open=[Lock,15,20]
    gen fork 0 (exit 8, delayed LIFE) is a separate generation; t=15 TRANS belongs to fork 12.

wait_time:
    {"metric":"wait_time","version":1,"value":5,"unit":"ns","basis":"observed","completeness":"complete","reasons":[],"detail":{"closed_ns":0,"open_ns":5},"diagnostic":{"recorded_ns":5,"covered_ns":7,"required_ns":7}}

completed_waits:
    {"metric":"completed_waits","version":1,"value":0,"unit":"count","basis":"observed","completeness":"complete","reasons":[],"diagnostic":{"recorded_count":0,"covered_ns":7,"required_ns":7}}

## 03_exit_inside_window

Projection:
    life=[0,10) coverage=[[1,10]] waits=[PgSleep 3-10] open=null
    b=min(F=12,exit=10)=10; certified EXIT (LIFE.exit=10, entry 3 known).

wait_time:
    {"metric":"wait_time","version":1,"value":5,"unit":"ns","basis":"observed","completeness":"complete","reasons":[],"detail":{"closed_ns":5,"open_ns":0},"diagnostic":{"recorded_ns":5,"covered_ns":5,"required_ns":5}}

completed_waits:
    {"metric":"completed_waits","version":1,"value":1,"unit":"count","basis":"observed","completeness":"complete","reasons":[],"diagnostic":{"recorded_count":1,"covered_ns":5,"required_ns":5}}

## 04_window_starts_at_exit

Projection:
    life=[0,10) coverage=[[1,10]] waits=[PgSleep 3-10] open=null (same as 03)
    R is empty; span ends at exit so closes at exit are certified.

wait_time:
    {"metric":"wait_time","version":1,"value":0,"unit":"ns","basis":"observed","completeness":"complete","reasons":[],"detail":{"closed_ns":0,"open_ns":0},"diagnostic":{"recorded_ns":0,"covered_ns":0,"required_ns":0}}

completed_waits:
    {"metric":"completed_waits","version":1,"value":1,"unit":"count","basis":"observed","completeness":"complete","reasons":[],"diagnostic":{"recorded_count":1,"covered_ns":0,"required_ns":0}}

## 05_known_exit_lost_terminal

Projection:
    life=[0,12) coverage=[[1,8]] waits=[] open=[PgSleep,3,8]
    exit LIFE kept; VOID freezes run at F=8 (slot 11 is not lifecycle loss); no exit certificate.

wait_time:
    {"metric":"wait_time","version":1,"value":null,"unit":"ns","basis":"observed","completeness":"unavailable","reasons":["coverage_incomplete"],"diagnostic":{"recorded_ns":2,"covered_ns":2,"required_ns":6}}

completed_waits:
    {"metric":"completed_waits","version":1,"value":null,"unit":"count","basis":"observed","completeness":"unavailable","reasons":["coverage_incomplete"],"diagnostic":{"recorded_count":0,"covered_ns":2,"required_ns":6}}

## 06_preexisting_before_capture

Projection:
    life=[0,null) (start_known=false, lower bound 0) coverage=[[12,20]] waits=[] open=[PgSleep,14,20]
    reread null a=12.

wait_time:
    {"metric":"wait_time","version":1,"value":null,"unit":"ns","basis":"observed","completeness":"unavailable","reasons":["coverage_incomplete"],"diagnostic":{"recorded_ns":6,"covered_ns":8,"required_ns":18}}

completed_waits:
    {"metric":"completed_waits","version":1,"value":null,"unit":"count","basis":"observed","completeness":"unavailable","reasons":["coverage_incomplete"],"diagnostic":{"recorded_count":0,"covered_ns":8,"required_ns":18}}

## 07_preexisting_covered_window

Projection:
    same projection as 06: life=[0,null) coverage=[[12,20]] open=[PgSleep,14,20]

wait_time:
    {"metric":"wait_time","version":1,"value":6,"unit":"ns","basis":"observed","completeness":"complete","reasons":[],"detail":{"closed_ns":0,"open_ns":6},"diagnostic":{"recorded_ns":6,"covered_ns":8,"required_ns":8}}

completed_waits:
    {"metric":"completed_waits","version":1,"value":0,"unit":"count","basis":"observed","completeness":"complete","reasons":[],"diagnostic":{"recorded_count":0,"covered_ns":8,"required_ns":8}}

## 08_attach_failure

Projection:
    life=[0,null) coverage=[] waits=[] open=null
    no ATTACH_OK, so no run; frontiers certify no backend coverage.

wait_time:
    {"metric":"wait_time","version":1,"value":null,"unit":"ns","basis":"observed","completeness":"unavailable","reasons":["coverage_incomplete"],"diagnostic":{"recorded_ns":0,"covered_ns":0,"required_ns":19}}

completed_waits:
    {"metric":"completed_waits","version":1,"value":null,"unit":"count","basis":"observed","completeness":"unavailable","reasons":["coverage_incomplete"],"diagnostic":{"recorded_count":0,"covered_ns":0,"required_ns":19}}

## 09_seed_mismatch_then_transition

Projection:
    life=[0,null) coverage=[[9,15]] waits=[] open=[PgSleep,8,15]
    transition anchor a=8 with nonnull old: begin a+1=9; entry 8 verified; FRONTIER 5 pre-attach certifies nothing.

wait_time:
    {"metric":"wait_time","version":1,"value":null,"unit":"ns","basis":"observed","completeness":"unavailable","reasons":["coverage_incomplete"],"diagnostic":{"recorded_ns":7,"covered_ns":6,"required_ns":7}}

completed_waits:
    {"metric":"completed_waits","version":1,"value":null,"unit":"count","basis":"observed","completeness":"unavailable","reasons":["coverage_incomplete"],"diagnostic":{"recorded_count":0,"covered_ns":6,"required_ns":7}}

## 10_equal_nonzero_seed

Projection:
    life=[0,null) coverage=[] waits=[] open=null (no candidate)
    reread of nonzero seed that never leaves the wait has no core portion (rule 1).

wait_time:
    {"metric":"wait_time","version":1,"value":null,"unit":"ns","basis":"observed","completeness":"unavailable","reasons":["coverage_incomplete"],"diagnostic":{"recorded_ns":0,"covered_ns":0,"required_ns":8}}

completed_waits:
    {"metric":"completed_waits","version":1,"value":null,"unit":"count","basis":"observed","completeness":"unavailable","reasons":["coverage_incomplete"],"diagnostic":{"recorded_count":0,"covered_ns":0,"required_ns":8}}

## 11_unknown_entry_close_boundary

Projection:
    life=[0,null) coverage=[[17,25]] waits=[Lock 16-22] open=null
    reread nonzero seed leaves at u=16, begin u+1=17; unknown-entry PgSleep close not emitted; Lock entry 16 known.

wait_time:
    {"metric":"wait_time","version":1,"value":null,"unit":"ns","basis":"observed","completeness":"unavailable","reasons":["coverage_incomplete"],"diagnostic":{"recorded_ns":6,"covered_ns":8,"required_ns":9}}

completed_waits:
    {"metric":"completed_waits","version":1,"value":null,"unit":"count","basis":"observed","completeness":"unavailable","reasons":["coverage_incomplete"],"diagnostic":{"recorded_count":1,"covered_ns":8,"required_ns":9}}

## 12_loss_then_recovery_crossing

Projection:
    run1 cov [[1,8]] open [PgSleep,3,8] (frozen at F=8 by VOID; TRANS 11 beyond it ignored); run2 (ATTACH 16) cov [[16,24]] open [PgSleep,18,24]
    life=[0,null); selected run2 (intersection 8 > 4); coverage=[[16,24]] waits=[] open=[PgSleep,18,24]

wait_time:
    {"metric":"wait_time","version":1,"value":null,"unit":"ns","basis":"observed","completeness":"unavailable","reasons":["coverage_incomplete"],"diagnostic":{"recorded_ns":6,"covered_ns":8,"required_ns":20}}

completed_waits:
    {"metric":"completed_waits","version":1,"value":null,"unit":"count","basis":"observed","completeness":"unavailable","reasons":["coverage_incomplete"],"diagnostic":{"recorded_count":0,"covered_ns":8,"required_ns":20}}

## 13_loss_then_recovery_later_window

Projection:
    same two runs as 12; run2 covers R=[16,24) so it is selected: coverage=[[16,24]] waits=[] open=[PgSleep,18,24]

wait_time:
    {"metric":"wait_time","version":1,"value":6,"unit":"ns","basis":"observed","completeness":"complete","reasons":[],"detail":{"closed_ns":0,"open_ns":6},"diagnostic":{"recorded_ns":6,"covered_ns":8,"required_ns":8}}

completed_waits:
    {"metric":"completed_waits","version":1,"value":0,"unit":"count","basis":"observed","completeness":"complete","reasons":[],"diagnostic":{"recorded_count":0,"covered_ns":8,"required_ns":8}}

## 14_global_loss_other_backend

Projection:
    life=[0,null) coverage=[[1,8]] waits=[] open=[PgSleep,3,8]
    global VOID (slot 4, not lifecycle) freezes pid 101 at F=8; only pid 202 reattached at 14.

wait_time:
    {"metric":"wait_time","version":1,"value":null,"unit":"ns","basis":"observed","completeness":"unavailable","reasons":["coverage_incomplete"],"diagnostic":{"recorded_ns":5,"covered_ns":5,"required_ns":17}}

completed_waits:
    {"metric":"completed_waits","version":1,"value":null,"unit":"count","basis":"observed","completeness":"unavailable","reasons":["coverage_incomplete"],"diagnostic":{"recorded_count":0,"covered_ns":5,"required_ns":17}}

## 15_frontier_without_reattach

Projection:
    life=[0,null) coverage=[[1,8]] waits=[] open=[PgSleep,3,8]
    frozen at F=8; FRONTIER 20 cannot revive without fresh ATTACH_OK.

wait_time:
    {"metric":"wait_time","version":1,"value":null,"unit":"ns","basis":"observed","completeness":"unavailable","reasons":["coverage_incomplete"],"diagnostic":{"recorded_ns":0,"covered_ns":0,"required_ns":12}}

completed_waits:
    {"metric":"completed_waits","version":1,"value":null,"unit":"count","basis":"observed","completeness":"unavailable","reasons":["coverage_incomplete"],"diagnostic":{"recorded_count":0,"covered_ns":0,"required_ns":12}}

## 16_crash_with_uncertified_suffix

Projection:
    life=[0,null) coverage=[[1,8]] waits=[] open=[PgSleep,3,8]
    EOF without END: only FRONTIER 8 bounds; TRANS 12 after it does not advance b.

wait_time:
    {"metric":"wait_time","version":1,"value":null,"unit":"ns","basis":"observed","completeness":"unavailable","reasons":["coverage_incomplete"],"diagnostic":{"recorded_ns":5,"covered_ns":5,"required_ns":9}}

completed_waits:
    {"metric":"completed_waits","version":1,"value":null,"unit":"count","basis":"observed","completeness":"unavailable","reasons":["coverage_incomplete"],"diagnostic":{"recorded_count":0,"covered_ns":5,"required_ns":9}}

## 17_wait_spans_frontiers

Projection:
    life=[0,null) coverage=[[1,20]] waits=[] open=[PgSleep,3,20]
    b=F=20 (latest frontier).

wait_time:
    {"metric":"wait_time","version":1,"value":12,"unit":"ns","basis":"observed","completeness":"complete","reasons":[],"detail":{"closed_ns":0,"open_ns":12},"diagnostic":{"recorded_ns":12,"covered_ns":12,"required_ns":12}}

completed_waits:
    {"metric":"completed_waits","version":1,"value":0,"unit":"count","basis":"observed","completeness":"complete","reasons":[],"diagnostic":{"recorded_count":0,"covered_ns":12,"required_ns":12}}

## 18_detach_mid_wait

Projection:
    life=[0,null) coverage=[[1,12]] waits=[] open=[PgSleep,3,12]
    b=min(F=15,END=12)=12.

wait_time:
    {"metric":"wait_time","version":1,"value":9,"unit":"ns","basis":"observed","completeness":"complete","reasons":[],"detail":{"closed_ns":0,"open_ns":9},"diagnostic":{"recorded_ns":9,"covered_ns":9,"required_ns":9}}

completed_waits:
    {"metric":"completed_waits","version":1,"value":0,"unit":"count","basis":"observed","completeness":"complete","reasons":[],"diagnostic":{"recorded_count":0,"covered_ns":9,"required_ns":9}}

## 19_end_without_final_frontier

Projection:
    life=[0,null) coverage=[[1,8]] waits=[] open=[PgSleep,3,8]
    END 12 pending (no frontier reaches 12); b=F=8.

wait_time:
    {"metric":"wait_time","version":1,"value":null,"unit":"ns","basis":"observed","completeness":"unavailable","reasons":["coverage_incomplete"],"diagnostic":{"recorded_ns":5,"covered_ns":5,"required_ns":9}}

completed_waits:
    {"metric":"completed_waits","version":1,"value":null,"unit":"count","basis":"observed","completeness":"unavailable","reasons":["coverage_incomplete"],"diagnostic":{"recorded_count":0,"covered_ns":5,"required_ns":9}}

## 20_two_escalation_windows

Projection:
    run1 cov [[1,8]] open [PgSleep,3,8]; run2 cov [[15,23]] open [PgSleep,17,23]; life=[0,null)
    selected run2 (intersection 8 > 7): coverage=[[15,23]] waits=[] open=[PgSleep,17,23]

wait_time:
    {"metric":"wait_time","version":1,"value":null,"unit":"ns","basis":"observed","completeness":"unavailable","reasons":["coverage_incomplete"],"diagnostic":{"recorded_ns":6,"covered_ns":8,"required_ns":22}}

completed_waits:
    {"metric":"completed_waits","version":1,"value":null,"unit":"count","basis":"observed","completeness":"unavailable","reasons":["coverage_incomplete"],"diagnostic":{"recorded_count":0,"covered_ns":8,"required_ns":22}}

## 21_second_escalation_window

Projection:
    same two runs as 20; run2 covers R=[15,23): coverage=[[15,23]] waits=[] open=[PgSleep,17,23]

wait_time:
    {"metric":"wait_time","version":1,"value":6,"unit":"ns","basis":"observed","completeness":"complete","reasons":[],"detail":{"closed_ns":0,"open_ns":6},"diagnostic":{"recorded_ns":6,"covered_ns":8,"required_ns":8}}

completed_waits:
    {"metric":"completed_waits","version":1,"value":0,"unit":"count","basis":"observed","completeness":"complete","reasons":[],"diagnostic":{"recorded_count":0,"covered_ns":8,"required_ns":8}}

## 22_rotation_open_wait

Projection:
    life=[0,null) coverage=[[1,20]] waits=[PgSleep 3-16] open=null
    rotation with complete chain changes nothing; close at 16 (16-13=3 matches entry).

wait_time:
    {"metric":"wait_time","version":1,"value":13,"unit":"ns","basis":"observed","completeness":"complete","reasons":[],"detail":{"closed_ns":13,"open_ns":0},"diagnostic":{"recorded_ns":13,"covered_ns":17,"required_ns":17}}

completed_waits:
    {"metric":"completed_waits","version":1,"value":1,"unit":"count","basis":"observed","completeness":"complete","reasons":[],"diagnostic":{"recorded_count":1,"covered_ns":17,"required_ns":17}}

## 23_rotation_missing_file

Projection:
    life=[0,null) coverage=[[1,8]] waits=[] open=[PgSleep,3,8]
    file 1 missing: chain stops at last valid prefix certificate (FRONTIER 8); file 2 not read.

wait_time:
    {"metric":"wait_time","version":1,"value":null,"unit":"ns","basis":"observed","completeness":"unavailable","reasons":["coverage_incomplete"],"diagnostic":{"recorded_ns":5,"covered_ns":5,"required_ns":17}}

completed_waits:
    {"metric":"completed_waits","version":1,"value":null,"unit":"count","basis":"observed","completeness":"unavailable","reasons":["coverage_incomplete"],"diagnostic":{"recorded_count":0,"covered_ns":5,"required_ns":17}}

## 24_restart_same_pid

Projection:
    capture B: life=[0,null) coverage=[[16,24]] waits=[] open=[Lock,18,24]
    capture A is a different capture; no continuity.

wait_time:
    {"metric":"wait_time","version":1,"value":null,"unit":"ns","basis":"observed","completeness":"unavailable","reasons":["coverage_incomplete"],"diagnostic":{"recorded_ns":6,"covered_ns":8,"required_ns":19}}

completed_waits:
    {"metric":"completed_waits","version":1,"value":null,"unit":"count","basis":"observed","completeness":"unavailable","reasons":["coverage_incomplete"],"diagnostic":{"recorded_count":0,"covered_ns":8,"required_ns":19}}

## 25_distinct_zero_length_waits

Projection:
    life=[0,null) coverage=[[1,10]] waits=[PgSleep 5-5, PgSleep 5-5] open=null
    entry 5 each time; two distinct zero-length waits.

wait_time:
    {"metric":"wait_time","version":1,"value":0,"unit":"ns","basis":"observed","completeness":"complete","reasons":[],"detail":{"closed_ns":0,"open_ns":0},"diagnostic":{"recorded_ns":0,"covered_ns":3,"required_ns":3}}

completed_waits:
    {"metric":"completed_waits","version":1,"value":2,"unit":"count","basis":"observed","completeness":"complete","reasons":[],"diagnostic":{"recorded_count":2,"covered_ns":3,"required_ns":3}}

## 26_unreadable_counters

Projection:
    life=[0,null) coverage=[[1,8]] waits=[] open=[PgSleep,3,8]
    VOID null (t=11) freezes at F=8 and taints lifecycle; VOID 14 restores readable baseline but preceding baseline unknown => still lifecycle uncertainty; ATTACH 16 for fork 0 ignored; no new LIFE.

wait_time:
    {"metric":"wait_time","version":1,"value":null,"unit":"ns","basis":"observed","completeness":"unavailable","reasons":["coverage_incomplete"],"diagnostic":{"recorded_ns":4,"covered_ns":4,"required_ns":20}}

completed_waits:
    {"metric":"completed_waits","version":1,"value":null,"unit":"count","basis":"observed","completeness":"unavailable","reasons":["coverage_incomplete"],"diagnostic":{"recorded_count":0,"covered_ns":4,"required_ns":20}}

## 27_preempt_rt_unqualified

Projection:
    qualified=false: certificates ignored, LIFE retained: life=[0,null) coverage=[] waits=[] open=null

wait_time:
    {"metric":"wait_time","version":1,"value":null,"unit":"ns","basis":"observed","completeness":"unavailable","reasons":["coverage_incomplete"],"diagnostic":{"recorded_ns":0,"covered_ns":0,"required_ns":9}}

completed_waits:
    {"metric":"completed_waits","version":1,"value":null,"unit":"count","basis":"observed","completeness":"unavailable","reasons":["coverage_incomplete"],"diagnostic":{"recorded_count":0,"covered_ns":0,"required_ns":9}}

## 28_legacy_v2

Projection:
    legacy v2: no v1 evidence => LIFE_INVALID (life=null), coverage=[] waits=[] open=null

wait_time:
    {"metric":"wait_time","version":1,"value":null,"unit":"ns","basis":"observed","completeness":"unavailable","reasons":["evidence_invalid"],"diagnostic":{"recorded_ns":null,"covered_ns":null,"required_ns":null}}

completed_waits:
    {"metric":"completed_waits","version":1,"value":null,"unit":"count","basis":"observed","completeness":"unavailable","reasons":["evidence_invalid"],"diagnostic":{"recorded_count":null,"covered_ns":null,"required_ns":null}}

## 29_legacy_v3

Projection:
    legacy v3: no v1 evidence => LIFE_INVALID (life=null), coverage=[] waits=[] open=null

wait_time:
    {"metric":"wait_time","version":1,"value":null,"unit":"ns","basis":"observed","completeness":"unavailable","reasons":["evidence_invalid"],"diagnostic":{"recorded_ns":null,"covered_ns":null,"required_ns":null}}

completed_waits:
    {"metric":"completed_waits","version":1,"value":null,"unit":"count","basis":"observed","completeness":"unavailable","reasons":["evidence_invalid"],"diagnostic":{"recorded_count":null,"covered_ns":null,"required_ns":null}}

## 30_legacy_v4

Projection:
    legacy v4: no v1 evidence => LIFE_INVALID (life=null), coverage=[] waits=[] open=null

wait_time:
    {"metric":"wait_time","version":1,"value":null,"unit":"ns","basis":"observed","completeness":"unavailable","reasons":["evidence_invalid"],"diagnostic":{"recorded_ns":null,"covered_ns":null,"required_ns":null}}

completed_waits:
    {"metric":"completed_waits","version":1,"value":null,"unit":"count","basis":"observed","completeness":"unavailable","reasons":["evidence_invalid"],"diagnostic":{"recorded_count":null,"covered_ns":null,"required_ns":null}}

## 31_late_data_revokes_prefix

Projection:
    life=[0,null) coverage=[] waits=[] open=null
    VOID invalid_from=1 revokes FRONTIER 10 (F absent, H=10); run has no surviving certificate and is discarded.

wait_time:
    {"metric":"wait_time","version":1,"value":null,"unit":"ns","basis":"observed","completeness":"unavailable","reasons":["coverage_incomplete"],"diagnostic":{"recorded_ns":0,"covered_ns":0,"required_ns":9}}

completed_waits:
    {"metric":"completed_waits","version":1,"value":null,"unit":"count","basis":"observed","completeness":"unavailable","reasons":["coverage_incomplete"],"diagnostic":{"recorded_count":0,"covered_ns":0,"required_ns":9}}

## 32_end_before_delayed_transition

Projection:
    life=[0,null) coverage=[[1,12]] waits=[] open=[PgSleep,11,12]
    TRANS 11 (physically after END 12, before FRONTIER 15) belongs to the run; b=min(15,12)=12; old-null duration unused.

wait_time:
    {"metric":"wait_time","version":1,"value":1,"unit":"ns","basis":"observed","completeness":"complete","reasons":[],"detail":{"closed_ns":0,"open_ns":1},"diagnostic":{"recorded_ns":1,"covered_ns":11,"required_ns":11}}

completed_waits:
    {"metric":"completed_waits","version":1,"value":0,"unit":"count","basis":"observed","completeness":"complete","reasons":[],"diagnostic":{"recorded_count":0,"covered_ns":11,"required_ns":11}}

## 33_anchor_transition_before_attach

Projection:
    life=[0,null) coverage=[[9,15]] waits=[] open=[PgSleep,8,15]
    same as 09 with the anchor TRANS physically before ATTACH_OK.

wait_time:
    {"metric":"wait_time","version":1,"value":null,"unit":"ns","basis":"observed","completeness":"unavailable","reasons":["coverage_incomplete"],"diagnostic":{"recorded_ns":7,"covered_ns":6,"required_ns":7}}

completed_waits:
    {"metric":"completed_waits","version":1,"value":null,"unit":"count","basis":"observed","completeness":"unavailable","reasons":["coverage_incomplete"],"diagnostic":{"recorded_count":0,"covered_ns":6,"required_ns":7}}

## 34_malformed_record_prefix

Projection:
    life=[0,null) coverage=[[1,8]] waits=[] open=[PgSleep,3,8]
    chain stops at: TRANS 12 missing duration (malformed); retained F=8; later records and file 1 of capture A ignored.

wait_time:
    {"metric":"wait_time","version":1,"value":5,"unit":"ns","basis":"observed","completeness":"complete","reasons":[],"detail":{"closed_ns":0,"open_ns":5},"diagnostic":{"recorded_ns":5,"covered_ns":7,"required_ns":7}}

completed_waits:
    {"metric":"completed_waits","version":1,"value":0,"unit":"count","basis":"observed","completeness":"complete","reasons":[],"diagnostic":{"recorded_count":0,"covered_ns":7,"required_ns":7}}

## 35_malformed_record_suffix

Projection:
    life=[0,null) coverage=[[1,8]] waits=[] open=[PgSleep,3,8]
    chain stops at: TRANS 12 missing duration (malformed); retained F=8; later records and file 1 of capture A ignored.

wait_time:
    {"metric":"wait_time","version":1,"value":null,"unit":"ns","basis":"observed","completeness":"unavailable","reasons":["coverage_incomplete"],"diagnostic":{"recorded_ns":0,"covered_ns":0,"required_ns":8}}

completed_waits:
    {"metric":"completed_waits","version":1,"value":null,"unit":"count","basis":"observed","completeness":"unavailable","reasons":["coverage_incomplete"],"diagnostic":{"recorded_count":0,"covered_ns":0,"required_ns":8}}

## 36_unknown_kind_prefix

Projection:
    life=[0,null) coverage=[[1,8]] waits=[] open=[PgSleep,3,8]
    chain stops at: unsupported kind FUTURE_EVIDENCE at 12; retained F=8; later records and file 1 of capture A ignored.

wait_time:
    {"metric":"wait_time","version":1,"value":5,"unit":"ns","basis":"observed","completeness":"complete","reasons":[],"detail":{"closed_ns":0,"open_ns":5},"diagnostic":{"recorded_ns":5,"covered_ns":7,"required_ns":7}}

completed_waits:
    {"metric":"completed_waits","version":1,"value":0,"unit":"count","basis":"observed","completeness":"complete","reasons":[],"diagnostic":{"recorded_count":0,"covered_ns":7,"required_ns":7}}

## 37_unknown_kind_suffix

Projection:
    life=[0,null) coverage=[[1,8]] waits=[] open=[PgSleep,3,8]
    chain stops at: unsupported kind FUTURE_EVIDENCE at 12; retained F=8; later records and file 1 of capture A ignored.

wait_time:
    {"metric":"wait_time","version":1,"value":null,"unit":"ns","basis":"observed","completeness":"unavailable","reasons":["coverage_incomplete"],"diagnostic":{"recorded_ns":0,"covered_ns":0,"required_ns":8}}

completed_waits:
    {"metric":"completed_waits","version":1,"value":null,"unit":"count","basis":"observed","completeness":"unavailable","reasons":["coverage_incomplete"],"diagnostic":{"recorded_count":0,"covered_ns":0,"required_ns":8}}

## 38_frontier_counter_change_prefix

Projection:
    run1 cov [[1,8]] open [PgSleep,3,8]; run2 (ATTACH 16) cov [[16,24]] open [PgSleep,18,24]; life=[0,null)
    FRONTIER 12 (slot 11 changed) consumed as VOID, no frontier published; selected run1 (covers R=[1,8)).

wait_time:
    {"metric":"wait_time","version":1,"value":5,"unit":"ns","basis":"observed","completeness":"complete","reasons":[],"detail":{"closed_ns":0,"open_ns":5},"diagnostic":{"recorded_ns":5,"covered_ns":7,"required_ns":7}}

completed_waits:
    {"metric":"completed_waits","version":1,"value":0,"unit":"count","basis":"observed","completeness":"complete","reasons":[],"diagnostic":{"recorded_count":0,"covered_ns":7,"required_ns":7}}

## 39_frontier_counter_change_suffix

Projection:
    same two runs as 38; run2 covers R=[16,24): coverage=[[16,24]] waits=[] open=[PgSleep,18,24]

wait_time:
    {"metric":"wait_time","version":1,"value":6,"unit":"ns","basis":"observed","completeness":"complete","reasons":[],"detail":{"closed_ns":0,"open_ns":6},"diagnostic":{"recorded_ns":6,"covered_ns":8,"required_ns":8}}

completed_waits:
    {"metric":"completed_waits","version":1,"value":0,"unit":"count","basis":"observed","completeness":"complete","reasons":[],"diagnostic":{"recorded_count":0,"covered_ns":8,"required_ns":8}}

## 40_attach_counter_change_prefix

Projection:
    fork 0: life=[0,null) coverage=[[1,8]] waits=[] open=[PgSleep,3,8]
    ATTACH 12 (slot 0 changed) => VOID + lifecycle taint; ATTACH 16/TRANS 18 for fork 0 ignored; fork 26 is a separate generation.

wait_time:
    {"metric":"wait_time","version":1,"value":5,"unit":"ns","basis":"observed","completeness":"complete","reasons":[],"detail":{"closed_ns":0,"open_ns":5},"diagnostic":{"recorded_ns":5,"covered_ns":7,"required_ns":7}}

completed_waits:
    {"metric":"completed_waits","version":1,"value":0,"unit":"count","basis":"observed","completeness":"complete","reasons":[],"diagnostic":{"recorded_count":0,"covered_ns":7,"required_ns":7}}

## 41_attach_counter_change_suffix

Projection:
    same projection as 40; R=[16,24) has no coverage.

wait_time:
    {"metric":"wait_time","version":1,"value":null,"unit":"ns","basis":"observed","completeness":"unavailable","reasons":["coverage_incomplete"],"diagnostic":{"recorded_ns":0,"covered_ns":0,"required_ns":8}}

completed_waits:
    {"metric":"completed_waits","version":1,"value":null,"unit":"count","basis":"observed","completeness":"unavailable","reasons":["coverage_incomplete"],"diagnostic":{"recorded_count":0,"covered_ns":0,"required_ns":8}}

## 42_overlapping_attachment_prefix

Projection:
    life=[0,null) coverage=[[1,8]] waits=[] open=[PgSleep,3,8]
    chain stops at: ATTACH_OK 12 overlaps the active run; retained F=8; later records and file 1 of capture A ignored.

wait_time:
    {"metric":"wait_time","version":1,"value":5,"unit":"ns","basis":"observed","completeness":"complete","reasons":[],"detail":{"closed_ns":0,"open_ns":5},"diagnostic":{"recorded_ns":5,"covered_ns":7,"required_ns":7}}

completed_waits:
    {"metric":"completed_waits","version":1,"value":0,"unit":"count","basis":"observed","completeness":"complete","reasons":[],"diagnostic":{"recorded_count":0,"covered_ns":7,"required_ns":7}}

## 43_overlapping_attachment_suffix

Projection:
    life=[0,null) coverage=[[1,8]] waits=[] open=[PgSleep,3,8]
    chain stops at: ATTACH_OK 12 overlaps the active run; retained F=8; later records and file 1 of capture A ignored.

wait_time:
    {"metric":"wait_time","version":1,"value":null,"unit":"ns","basis":"observed","completeness":"unavailable","reasons":["coverage_incomplete"],"diagnostic":{"recorded_ns":0,"covered_ns":0,"required_ns":8}}

completed_waits:
    {"metric":"completed_waits","version":1,"value":null,"unit":"count","basis":"observed","completeness":"unavailable","reasons":["coverage_incomplete"],"diagnostic":{"recorded_count":0,"covered_ns":0,"required_ns":8}}

## 44_end_without_run_prefix

Projection:
    life=[0,null) coverage=[[1,8]] waits=[] open=[PgSleep,3,8]
    chain stops at: END 12 for pid 202 addresses no run; retained F=8; later records and file 1 of capture A ignored.

wait_time:
    {"metric":"wait_time","version":1,"value":5,"unit":"ns","basis":"observed","completeness":"complete","reasons":[],"detail":{"closed_ns":0,"open_ns":5},"diagnostic":{"recorded_ns":5,"covered_ns":7,"required_ns":7}}

completed_waits:
    {"metric":"completed_waits","version":1,"value":0,"unit":"count","basis":"observed","completeness":"complete","reasons":[],"diagnostic":{"recorded_count":0,"covered_ns":7,"required_ns":7}}

## 45_end_without_run_suffix

Projection:
    life=[0,null) coverage=[[1,8]] waits=[] open=[PgSleep,3,8]
    chain stops at: END 12 for pid 202 addresses no run; retained F=8; later records and file 1 of capture A ignored.

wait_time:
    {"metric":"wait_time","version":1,"value":null,"unit":"ns","basis":"observed","completeness":"unavailable","reasons":["coverage_incomplete"],"diagnostic":{"recorded_ns":0,"covered_ns":0,"required_ns":8}}

completed_waits:
    {"metric":"completed_waits","version":1,"value":null,"unit":"count","basis":"observed","completeness":"unavailable","reasons":["coverage_incomplete"],"diagnostic":{"recorded_count":0,"covered_ns":0,"required_ns":8}}

## 46_nonadvancing_frontier_prefix

Projection:
    life=[0,null) coverage=[[1,8]] waits=[] open=[PgSleep,3,8]
    chain stops at: FRONTIER 8 not above H=8; retained F=8; later records and file 1 of capture A ignored.

wait_time:
    {"metric":"wait_time","version":1,"value":5,"unit":"ns","basis":"observed","completeness":"complete","reasons":[],"detail":{"closed_ns":0,"open_ns":5},"diagnostic":{"recorded_ns":5,"covered_ns":7,"required_ns":7}}

completed_waits:
    {"metric":"completed_waits","version":1,"value":0,"unit":"count","basis":"observed","completeness":"complete","reasons":[],"diagnostic":{"recorded_count":0,"covered_ns":7,"required_ns":7}}

## 47_nonadvancing_frontier_suffix

Projection:
    life=[0,null) coverage=[[1,8]] waits=[] open=[PgSleep,3,8]
    chain stops at: FRONTIER 8 not above H=8; retained F=8; later records and file 1 of capture A ignored.

wait_time:
    {"metric":"wait_time","version":1,"value":null,"unit":"ns","basis":"observed","completeness":"unavailable","reasons":["coverage_incomplete"],"diagnostic":{"recorded_ns":0,"covered_ns":0,"required_ns":15}}

completed_waits:
    {"metric":"completed_waits","version":1,"value":null,"unit":"count","basis":"observed","completeness":"unavailable","reasons":["coverage_incomplete"],"diagnostic":{"recorded_count":0,"covered_ns":0,"required_ns":15}}

## 48_run_state_mismatch_prefix

Projection:
    life=[0,null) coverage=[[1,8]] waits=[] open=[PgSleep,3,8]
    chain stops at: TRANS 12 old=Lock but state is PgSleep; retained F=8; later records and file 1 of capture A ignored.

wait_time:
    {"metric":"wait_time","version":1,"value":5,"unit":"ns","basis":"observed","completeness":"complete","reasons":[],"detail":{"closed_ns":0,"open_ns":5},"diagnostic":{"recorded_ns":5,"covered_ns":7,"required_ns":7}}

completed_waits:
    {"metric":"completed_waits","version":1,"value":0,"unit":"count","basis":"observed","completeness":"complete","reasons":[],"diagnostic":{"recorded_count":0,"covered_ns":7,"required_ns":7}}

## 49_run_state_mismatch_suffix

Projection:
    life=[0,null) coverage=[[1,8]] waits=[] open=[PgSleep,3,8]
    chain stops at: TRANS 12 old=Lock but state is PgSleep; retained F=8; later records and file 1 of capture A ignored.

wait_time:
    {"metric":"wait_time","version":1,"value":null,"unit":"ns","basis":"observed","completeness":"unavailable","reasons":["coverage_incomplete"],"diagnostic":{"recorded_ns":0,"covered_ns":0,"required_ns":8}}

completed_waits:
    {"metric":"completed_waits","version":1,"value":null,"unit":"count","basis":"observed","completeness":"unavailable","reasons":["coverage_incomplete"],"diagnostic":{"recorded_count":0,"covered_ns":0,"required_ns":8}}

## 50_capture_boot_conflict_prefix

Projection:
    life=[0,null) coverage=[[1,8]] waits=[] open=[PgSleep,3,8]
    chain stops at: descriptor boot-2 reuses capture A; retained F=8; later records and file 1 of capture A ignored.

wait_time:
    {"metric":"wait_time","version":1,"value":5,"unit":"ns","basis":"observed","completeness":"complete","reasons":[],"detail":{"closed_ns":0,"open_ns":5},"diagnostic":{"recorded_ns":5,"covered_ns":7,"required_ns":7}}

completed_waits:
    {"metric":"completed_waits","version":1,"value":0,"unit":"count","basis":"observed","completeness":"complete","reasons":[],"diagnostic":{"recorded_count":0,"covered_ns":7,"required_ns":7}}

## 51_capture_boot_conflict_suffix

Projection:
    life=[0,null) coverage=[[1,8]] waits=[] open=[PgSleep,3,8]
    chain stops at: descriptor boot-2 reuses capture A; retained F=8; later records and file 1 of capture A ignored.

wait_time:
    {"metric":"wait_time","version":1,"value":null,"unit":"ns","basis":"observed","completeness":"unavailable","reasons":["coverage_incomplete"],"diagnostic":{"recorded_ns":0,"covered_ns":0,"required_ns":8}}

completed_waits:
    {"metric":"completed_waits","version":1,"value":null,"unit":"count","basis":"observed","completeness":"unavailable","reasons":["coverage_incomplete"],"diagnostic":{"recorded_count":0,"covered_ns":0,"required_ns":8}}

## 52_malformed_before_frontier_prefix

Projection:
    life=[0,null) coverage=[] waits=[] open=null
    TRANS without t stops chain before any FRONTIER (F absent); LIFE retained.

wait_time:
    {"metric":"wait_time","version":1,"value":null,"unit":"ns","basis":"observed","completeness":"unavailable","reasons":["coverage_incomplete"],"diagnostic":{"recorded_ns":0,"covered_ns":0,"required_ns":1}}

completed_waits:
    {"metric":"completed_waits","version":1,"value":null,"unit":"count","basis":"observed","completeness":"unavailable","reasons":["coverage_incomplete"],"diagnostic":{"recorded_count":0,"covered_ns":0,"required_ns":1}}

## 53_malformed_before_frontier_suffix

Projection:
    same as 52: life=[0,null) coverage=[] waits=[] open=null

wait_time:
    {"metric":"wait_time","version":1,"value":null,"unit":"ns","basis":"observed","completeness":"unavailable","reasons":["coverage_incomplete"],"diagnostic":{"recorded_ns":0,"covered_ns":0,"required_ns":8}}

completed_waits:
    {"metric":"completed_waits","version":1,"value":null,"unit":"count","basis":"observed","completeness":"unavailable","reasons":["coverage_incomplete"],"diagnostic":{"recorded_count":0,"covered_ns":0,"required_ns":8}}

## 54_independent_capture_after_fault

Projection:
    capture B: life=[0,null) coverage=[[1,24]] waits=[] open=[PgSleep,3,24]
    capture A's fault does not affect B.

wait_time:
    {"metric":"wait_time","version":1,"value":8,"unit":"ns","basis":"observed","completeness":"complete","reasons":[],"detail":{"closed_ns":0,"open_ns":8},"diagnostic":{"recorded_ns":8,"covered_ns":8,"required_ns":8}}

completed_waits:
    {"metric":"completed_waits","version":1,"value":0,"unit":"count","basis":"observed","completeness":"complete","reasons":[],"diagnostic":{"recorded_count":0,"covered_ns":8,"required_ns":8}}

## 55_run_duration_mismatch

Projection:
    life=[0,null) coverage=[[1,8]] waits=[] open=[PgSleep,3,8]
    TRANS 12 duration 8 != 12-3 => chain stops at it; F=8.

wait_time:
    {"metric":"wait_time","version":1,"value":null,"unit":"ns","basis":"observed","completeness":"unavailable","reasons":["coverage_incomplete"],"diagnostic":{"recorded_ns":5,"covered_ns":7,"required_ns":23}}

completed_waits:
    {"metric":"completed_waits","version":1,"value":null,"unit":"count","basis":"observed","completeness":"unavailable","reasons":["coverage_incomplete"],"diagnostic":{"recorded_count":0,"covered_ns":7,"required_ns":23}}

## 56_attachment_before_life

Projection:
    life=[0,null) coverage=[[1,8]] waits=[] open=[PgSleep,3,8]
    ATTACH_OK 4 for pid 202 starts before LIFE.start 5 => chain stops there; F=8.

wait_time:
    {"metric":"wait_time","version":1,"value":null,"unit":"ns","basis":"observed","completeness":"unavailable","reasons":["coverage_incomplete"],"diagnostic":{"recorded_ns":5,"covered_ns":7,"required_ns":23}}

completed_waits:
    {"metric":"completed_waits","version":1,"value":null,"unit":"count","basis":"observed","completeness":"unavailable","reasons":["coverage_incomplete"],"diagnostic":{"recorded_count":0,"covered_ns":7,"required_ns":23}}

## 57_nonadvancing_after_rollback

Projection:
    target fork 15 has no LIFE in the retained prefix => LIFE_INVALID (life=null)
    VOID invalid_from=9 rolls F to 8 (H=10); FRONTIER 9 <= H stops the chain, so file 1 (LIFE fork 15) is never read.

wait_time:
    {"metric":"wait_time","version":1,"value":null,"unit":"ns","basis":"observed","completeness":"unavailable","reasons":["evidence_invalid"],"diagnostic":{"recorded_ns":null,"covered_ns":null,"required_ns":null}}

completed_waits:
    {"metric":"completed_waits","version":1,"value":null,"unit":"count","basis":"observed","completeness":"unavailable","reasons":["evidence_invalid"],"diagnostic":{"recorded_count":null,"covered_ns":null,"required_ns":null}}

## 58_counter_loss_fresh_fork

Projection:
    fork 26: life=[26,null) coverage=[[27,32]] waits=[] open=[PgSleep,29,32]
    ATTACH 12 (slot 0) => VOID + taint of fork 0; fresh LIFE fork 26>=12 after VOID, fresh ATTACH 27.

wait_time:
    {"metric":"wait_time","version":1,"value":3,"unit":"ns","basis":"observed","completeness":"complete","reasons":[],"detail":{"closed_ns":0,"open_ns":3},"diagnostic":{"recorded_ns":3,"covered_ns":5,"required_ns":5}}

completed_waits:
    {"metric":"completed_waits","version":1,"value":0,"unit":"count","basis":"observed","completeness":"complete","reasons":[],"diagnostic":{"recorded_count":0,"covered_ns":5,"required_ns":5}}

## 59_frontier_counter_recovery

Projection:
    life=[0,null) run2 (ATTACH 14): coverage=[[14,25]] waits=[PgSleep 17-22] open=null
    FRONTIER 12 (slot 4) consumed as VOID, run1 frozen at 8; ATTACH 14 matches new baseline.

wait_time:
    {"metric":"wait_time","version":1,"value":5,"unit":"ns","basis":"observed","completeness":"complete","reasons":[],"detail":{"closed_ns":5,"open_ns":0},"diagnostic":{"recorded_ns":5,"covered_ns":11,"required_ns":11}}

completed_waits:
    {"metric":"completed_waits","version":1,"value":1,"unit":"count","basis":"observed","completeness":"complete","reasons":[],"diagnostic":{"recorded_count":1,"covered_ns":11,"required_ns":11}}

## 60_attach_counter_recovery

Projection:
    life=[0,null) run2 (ATTACH 15): coverage=[[15,24]] waits=[PgSleep 17-21] open=null
    ATTACH 12 (slot 11) consumed as VOID, run1 frozen at 8 so ATTACH 15 does not overlap.

wait_time:
    {"metric":"wait_time","version":1,"value":4,"unit":"ns","basis":"observed","completeness":"complete","reasons":[],"detail":{"closed_ns":4,"open_ns":0},"diagnostic":{"recorded_ns":4,"covered_ns":9,"required_ns":9}}

completed_waits:
    {"metric":"completed_waits","version":1,"value":1,"unit":"count","basis":"observed","completeness":"complete","reasons":[],"diagnostic":{"recorded_count":1,"covered_ns":9,"required_ns":9}}

## 61_blocked_attach_fresh_generation

Projection:
    fork 20: life=[20,null) coverage=[[21,31]] waits=[PgSleep 23-28] open=null
    VOID slot 0 taints fork 0; ATTACH 16 ignored; fresh fork 20 and ATTACH 21.

wait_time:
    {"metric":"wait_time","version":1,"value":5,"unit":"ns","basis":"observed","completeness":"complete","reasons":[],"detail":{"closed_ns":5,"open_ns":0},"diagnostic":{"recorded_ns":5,"covered_ns":10,"required_ns":10}}

completed_waits:
    {"metric":"completed_waits","version":1,"value":1,"unit":"count","basis":"observed","completeness":"complete","reasons":[],"diagnostic":{"recorded_count":1,"covered_ns":10,"required_ns":10}}

## 62_transition_outside_runs

Projection:
    run2 (ATTACH 16): life=[0,null) coverage=[[16,26]] waits=[PgSleep 18-23] open=null
    run1 ended at 9 (cov [[1,9]]); TRANS 12 lies outside every run and is ignored; run2 selected (covers R).

wait_time:
    {"metric":"wait_time","version":1,"value":5,"unit":"ns","basis":"observed","completeness":"complete","reasons":[],"detail":{"closed_ns":5,"open_ns":0},"diagnostic":{"recorded_ns":5,"covered_ns":10,"required_ns":10}}

completed_waits:
    {"metric":"completed_waits","version":1,"value":1,"unit":"count","basis":"observed","completeness":"complete","reasons":[],"diagnostic":{"recorded_count":1,"covered_ns":10,"required_ns":10}}

## 63_recovery_before_void_time

Projection:
    life=[0,null) coverage=[[1,8]] waits=[] open=[PgSleep,3,8]
    ATTACH_OK t=11 < latest VOID.t=12 stops the chain there; later records ignored.

wait_time:
    {"metric":"wait_time","version":1,"value":null,"unit":"ns","basis":"observed","completeness":"unavailable","reasons":["coverage_incomplete"],"diagnostic":{"recorded_ns":0,"covered_ns":0,"required_ns":10}}

completed_waits:
    {"metric":"completed_waits","version":1,"value":null,"unit":"count","basis":"observed","completeness":"unavailable","reasons":["coverage_incomplete"],"diagnostic":{"recorded_count":0,"covered_ns":0,"required_ns":10}}

## 64_end_after_blocked_attach

Projection:
    fork 20: life=[20,null) coverage=[[21,30]] waits=[PgSleep 23-27] open=null
    ATTACH 16 ignored (taint); END 18 to tainted fork 0 ignored; fresh fork 20 + ATTACH 21.

wait_time:
    {"metric":"wait_time","version":1,"value":4,"unit":"ns","basis":"observed","completeness":"complete","reasons":[],"detail":{"closed_ns":4,"open_ns":0},"diagnostic":{"recorded_ns":4,"covered_ns":9,"required_ns":9}}

completed_waits:
    {"metric":"completed_waits","version":1,"value":1,"unit":"count","basis":"observed","completeness":"complete","reasons":[],"diagnostic":{"recorded_count":1,"covered_ns":9,"required_ns":9}}

## Ambiguities

none
