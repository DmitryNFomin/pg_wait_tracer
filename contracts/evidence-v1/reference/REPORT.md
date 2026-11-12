# Evidence-v1 reference replay report

All scenarios were replayed from their inputs, without comparison to expected answers.
Counts below include metric result objects only; the projection is a separate JSONL object.
The evaluator uses integer arithmetic and the lifetime-start clarification. It treats
qualified fixture descriptors as assertions, without claiming to verify kernel protocols.
It consumes merged storage records; it does not reimplement transport capture or merge.

| Scenario | Result objects | Ambiguity |
|---|---:|---|
| 01_ordinary_run | 2 | none |
| 02_pid_reuse_delayed_life | 2 | none |
| 03_exit_inside_window | 2 | none |
| 04_window_starts_at_exit | 2 | none |
| 05_known_exit_lost_terminal | 2 | none |
| 06_preexisting_before_capture | 2 | none |
| 07_preexisting_covered_window | 2 | none |
| 08_attach_failure | 2 | none |
| 09_seed_mismatch_then_transition | 2 | none |
| 10_equal_nonzero_seed | 2 | none |
| 11_unknown_entry_close_boundary | 2 | none |
| 12_loss_then_recovery_crossing | 2 | none |
| 13_loss_then_recovery_later_window | 2 | none |
| 14_global_loss_other_backend | 2 | none |
| 15_frontier_without_reattach | 2 | none |
| 16_crash_with_uncertified_suffix | 2 | none |
| 17_wait_spans_frontiers | 2 | none |
| 18_detach_mid_wait | 2 | none |
| 19_end_without_final_frontier | 2 | none |
| 20_two_escalation_windows | 2 | none |
| 21_second_escalation_window | 2 | none |
| 22_rotation_open_wait | 2 | none |
| 23_rotation_missing_file | 2 | none |
| 24_restart_same_pid | 2 | none |
| 25_distinct_zero_length_waits | 2 | none |
| 26_unreadable_counters | 2 | none |
| 27_preempt_rt_unqualified | 2 | none |
| 28_legacy_v2 | 2 | none |
| 29_legacy_v3 | 2 | none |
| 30_legacy_v4 | 2 | none |
| 31_late_data_revokes_prefix | 2 | none |
| 32_end_before_delayed_transition | 2 | none |
| 33_anchor_transition_before_attach | 2 | none |

## Ambiguous contract sentences found

None of these questions is exercised by the supplied 33 scenarios. They remain
explicit ambiguity branches rather than alternative numerical answers.

1. Evidence-v1 §5: “An unexpected counter vector, malformed record, or unsupported
   evidence kind acts as global VOID at the preceding F; if framing/identity cannot
   be trusted, stop the file chain there instead.” For a malformed record without
   readable counters, which baseline and lifecycle-uncertainty detection instant
   does this synthetic VOID use? What precise field failures make identity untrusted
   enough to stop the chain instead? A malformed logical fixture triggers ambiguity.
2. Evidence-v1 §4: “Overlapping attachments are invalid.” Together with §5's
   invalid-record rule, this does not specify the counter baseline or the effective
   lifecycle-uncertainty time for the resulting synthetic VOID. The same question
   applies to an attachment violating §2's “Require attachment a <= END e and
   LIFE.start <= a; no attach at/after known exit.”
3. Evidence-v1 §3: “Assign transitions to runs by generation and timestamp, not
   position relative to ATTACH_OK or END.” An END with no uniquely addressable run
   has no specified invalid-record recovery baseline. Touching runs with tied
   transitions are explicitly resolved by withholding certification and are not
   treated as an ambiguity.
4. Evidence-v1 §4: “FRONTIER validates its dependencies and, if H exists, must
   exceed H; set H=F=t on acceptance.” A nonadvancing FRONTIER is not acceptable,
   but whether to ignore it, freeze active runs, or stop the chain is unspecified.
5. Evidence-v1 §7: “target is {capture,pid,fork}.” Together with §6's “A boot change
   creates a different clock domain; windows cannot cross it,” a target cannot
   disambiguate two boots that reuse the same capture identifier. The evaluator
   reports ambiguity if that occurs, rather than choosing a clock domain.
6. Evidence-v1 §5: “Run consistency failure discards the suffix back to its last
   valid certificate and acts as global VOID; contradictory LIFE invalidates that
   generation.” The retained certificate is specified, but the synthetic VOID's
   counter baseline and lifecycle-uncertainty instant are not. A run mismatch
   triggers ambiguity instead of choosing recovery behavior.

## Verification

`reference/run_all.sh` executed every scenario as a separate Python process,
parsed each emitted line as a JSON object, checked result counts, result shapes,
complete-metric arithmetic, single-candidate selection, lifetime containment,
and open-observation bounds, and wrote stdout directly to `reference/out/`.
No expected-answer files or outside repositories were used. Inputs were unchanged.
