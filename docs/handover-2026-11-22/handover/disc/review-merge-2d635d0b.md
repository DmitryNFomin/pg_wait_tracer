# Review: agent/988-discovery-merge, round 3 (8e42b20c..2d635d0b; whole unit 5fd6a519..2d635d0b)

Verdict: **READY-with-nits**. No blockers. One should-fix (test coverage) and one nit. Two design questions need a decision before the producer unit starts.

## What changed
- The startup gap's persisted end is now C+1 only when an uncertified discovery record is stamped exactly C: a fresh-record anchor, an earlier-attempt record, or a record discarded by abandonment. Otherwise it is C. The quiet, census-fence and terminal routes now close at C.
- New ABANDON_DISCOVERY control. For an installed, never-admitted startup process, its records are checked and counted, then dropped: they are never published and no longer hold back the frontier. Its EXIT is still published. The abandonment is permanent and survives REARM. It forgives no loss.
- A startup process that exits before its identity is installed can now be discharged by a matching terminal witness. The discharge commits only if its EXIT publishes successfully.
- The 62-row oracle test now reads gap state and window availability from the emitted GAP_CLOSE record through `pgwt_coverage_verdict`. The four tests that round 2 broke pass again.

## Evidence
- Check stamp: none (`.pgwt-check.stamp` absent in wt-discm). I did not run `make check` (read-only brief). Every Mac test that make check runs and that touches core, capture or open_snapshot was run by hand (below).
- box-check: none for 2d635d0b. No `tests/results/box-check-*.log` in wt-discm, and I ran no box (read-only brief). The C unit tier and live tiers on Linux are **not verified**.
- Host runs on git-archive exports of 2d635d0b and 5fd6a519 (logs in scratchpad/rv3/logs1 and logs2):
  - `test_discovery_admission.py --mutants`: drift check 62/62; 62/62 rows pass; empty census PASS; **75/75 mutants RED**.
  - `scripts/core-status.sh` with ASan/UBSan: rc=0, 21/21 CORE-STATUS PASS. `test_core_status.sh`: rc=0.
  - Passed (rc=0): core_merge_adapter_mutants.sh (27/27 killed, guard sweep 31/31, including percpu `forget_historical_proof` KILLED), test_core_merge_queue, check_server_test_links --self-test, test_consumer_boundary --self-test, transport_diagnostics_mutants, test_coverage_drain --mutants, test_exact_perf_coverage --mutants, test_ledger, core_merge_index_mutants, core_merge_queue_mutants, test_attach_timeline, test_block_type_sites, test_capture_bookkeeping, test_capture_recovery, test_core_evidence_differential, test_core_merge_index, test_core_wait_time_differential, test_coverage_drain, test_discovery_admission, test_evidence_capture_portable, test_evidence_codec_mutants/oracle/redelivery, test_evidence_report, test_exact_perf_coverage, test_init_loss_diagnostics, test_pilot_route, test_transport_startup.
  - test_pilot_route `--require-browser` and `--mutants` first failed on Homebrew python 3.14 with "No module named 'websockets'", an environment problem. Both pass with PYENV_VERSION=3.11.7.
  - No behaviour change with discovery unused. Against 5fd6a519, these outputs are byte-identical: T-cov (`MUTANT`/`PASS` lines; 18 RED; `ignore_preexisting` RED at boot_start), test_core_merge_contract (99/99, rows 1-43 N/Q), test_core_merge_adapter, test_percpu_transport (43/43 N, 49/49 guards), test_transport_diagnostics and test_transport_wire.
- Registration guards: test-registration OK, runner-registration OK. `git diff --check 8e42b20c 2d635d0b` is clean.

## Brief items
1. **First review's finding 1, fixed at each cause.**
   - test_capture_bookkeeping: the copy list now includes the two new headers.
   - The index cost fixture's cookies `i^UINT64_MAX` became `i+1`. Keys are still distinct per pid and below 2^63, and the work-bound and index mutants still pass and kill.
   - The percpu anchor now spans the next line, so it is unique and the mutation is identical.
   - Core mutants M20-M22 are re-anchored on comment-tagged copies of the same three counter checkpoints.
   - Nothing was weakened.
2. **DECISIONs 41-42: the persisted gap end.** The persisted end is `close + increment` (merge_capture.c:522-537). It increments only when some lifetime's greatest consumed uncertified stamp equals C. The reserved field is untouched: production calls `pgwt_writer_coverage_gap(..., gap->at, 0)`.
   - My reader probe, through the emitted records and `pgwt_coverage_verdict`:

     | Row | Persisted end | Window | Reader | Result |
     |---|---|---|---|---|
     | D01 | 13 | [12,16) | INCOMPLETE | |
     | D01 | 13 | [13,16) | COMPLETE | X 2/1 |
     | Q01 | 12 | [12,16) | COMPLETE | 3/1 |
     | Q09 | 11 | [11,16) | COMPLETE | 4/1 |
     | D14b | 15 | [15,16) | COMPLETE | 0/1 |
     | D07a | 20 | | | |

     All match EXPECTED.
   - "never C+1" is RED on all 13 rows EXPECTED lists. "always C+1" is RED on all 9 of its rows. I ran every listed row, not just the one the harness names.
3. **DECISION 40 and A15.**
   - A cookie-less EXIT of an inventoried pid is now held. A matching TERM binds it, using the retained terminal time and never the current map entry.
   - My probe: EXIT@9, then TERM(9), then S(16) gives `EXIT_D9 F15`, pending 0, census still complete, closed at 9, and [9,10) COMPLETE.
   - A rejected EXIT leaves pending 1 with no close (A15 plus an extra schedule). The mutant `terminal_commit_after_rejected_EXIT` is RED.
4. **DECISION 45.** All 20 A rows pass, with their mutants RED: A04/A06/A20 acceptance, A05/A10 contradiction, A09 duplicate, A12 REARM, A08 loss, A03 C+1.
   - Leak hunt, all as specified:
     - CONFIRM before AB: no lower bound, since quiet_ready needs admitted.
     - A TRANS after the published EXIT is dropped, and the later TERM gives VOID unknown_evidence.
     - A dropped record followed by a failed publication: wire identity ok=1 with no double count.
     - Shared TEXT is published and not counted.
     - A dependency hold still holds an abandoned record.
     - identity_known=false is refused before any drop.
   - No published abandoned record found.
5. **No behaviour change with discovery off:** see Evidence.
6. **The 75 mutants:** all RED at their named row with the intended assertion. I sampled 18 by hand, each against oracle-derived text: always/never C+1, ignore_earlier_attempt_endpoint, ignore_discard_endpoint, increment_for_earlier_uncertified_stamp, EXIT_is_numerical_endpoint, abandon_supplies_lower_bound, publish_abandoned_TEXT, count_staged_records_at_abandon, discard_lifecycle_EXIT, ADMIT_after_abandon, first_abandon_after_closure, reject_duplicate_after_closure, forget_abandonment_on_REARM, terminal_commit_after_rejected_EXIT, abandonment_by_PID, skip_abandoned_arrival_boundary, abandon_supplies_completeness. Removing `zero_wait_infers_on_cpu` is justified: seeding on-CPU state is the producer's job, and Q07 keeps meaningful mutants.

## Findings
1. **should-fix: about 11 new fail-safe guards have no test that fails when they are removed.** Reproduced with my own mutants (scratchpad/rv3/mymut.py); with each removed, 62/62 still pass:
   - merge.c:509 — a dropped record after the terminal witness
   - merge.c:568-570 — consumed drops later than x_i
   - merge.c:627 — CONFIRM checks staged earlier attempts
   - merge.c:818 — the check after each batch
   - merge.c:578 — EXIT time against TERM time
   - merge.c:722 — FORK/INIT of an abandoned lifetime
   - merge_capture.c:714 — conflicting TERM times
   - merge_capture.c:1022-1023 — the cookie-less EXIT's time match and an ambiguous owner
   - merge_adapter.c:311 — the exclusive TEXT owner's binding
   - the C+1 overflow guard

   Concrete case for merge.c:568-570 (probe p_guardb.c):
   - Input: AB_D, D12@P0, EXIT_D10@L, S(13), TERM(D,10), S(14).
   - Expected (DECISION 45, "every discarded timed record must remain inside the persisted unavailable interval"): VOID unknown_evidence and no close. The product does this today.
   - With the guard removed: G(10) commits, [10,14) reads COMPLETE although D's dropped record at 12 lies outside the gap, and no test goes red.

   The code is correct today; only the gate cannot see these guards fail.
2. **nit: TEXT marked exclusive is trusted without checking for other users** (merge.c:856, 1044). Probe p_exit.c P6:
   - Input: TEXT for query 77 marked exclusive to D, admitted H, AB_D, then H15 depending on query 77.
   - Actual: H15 is published referencing a query text that was dropped and never published.
   - This only happens if the producer marks shared metadata as exclusive. Record it in the producer brief; not a merge defect today.

No blockers.

## Design questions for the owner
1. **A startup process exits before its identity is installed, and no matching TERM can be delivered.** DECISION 40 allows this: "an unresolved or contradictory match supplies no terminal discharge." Its cookie-less EXIT is then held for ever (merge_capture.c:816, :1027-1032). That holds the whole lifecycle source, so nothing is released for any process.
   - Reproduced (p_stall.c): 25 rounds of an ordinary attached lifetime produce no output at all, then finish gives `VOID200[unknown_evidence]`.
   - It fails loudly, not with garbage, and production never enables discovery.
   - The contract has no "terminal unresolved" outcome. ABANDON covers installed identities only, and DECISION 38 sends uninstalled candidates to "the existing rules", where a cookie-less record is uncovered.
   - Decision needed before the producer unit: should the producer always send either a TERM or a new "unresolved" outcome, or should the held EXIT revert to uncovered at some point?
2. **An EXIT published in a release attempt that is then VOIDed still counts for discharge** (merge.c:892-898, extra A03 schedule):
   - A counter change at the checkpoint after the EXIT VOIDs the release. After REARM, the TERM discharges D and closes the gap at 18.
   - §3 says such an output "belongs to the uncertified suffix discarded by VOID". The A15 clarification says only "published successfully".
   - Does the discarded suffix count as published for terminal discharge?
