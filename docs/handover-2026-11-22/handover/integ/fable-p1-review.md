PROCEED WITH CHANGES: one P1 fix round (B1, B2 with its second case, S1, S2, the exit half of S3); no change to the frozen header bytes; a text-only clarification first; I2 does not wait.
Refs: P = 566f20d5 src/discovery_producer.c; M = 003ee35a (wt-m12r) src/core/merge.c; MC = 003ee35a src/merge_capture.c; C = contracts/merge-v1/CONTRACT.md; spec = spec-frozen.md.

Q1 continuity
1. VERIFIED: the input is read only when epoch > 1 (M:894-895); the epoch rises only at REARM (M:1575); production never issues REARM: pgwt_capture_rearm returns first unless PGWT_TEST_EXACT_ATTACH_PREMISE=1 (MC:1769, MC:2106-2113, "unit A2 does not exist"). No production path consults the input today.
2. VERIFIED: REARM clears identity_proved for every attachment and restores it only from its own attachment_proofs (M:1583-1591); those are assumed, not measured, for every lifetime (MC:1775-1779). So the missing fact is not a discovery gap: nothing in production proves "still attached at REARM" for a fork-born backend either. It is needed at two seams, not one: REARM's attachment_proofs (D already admitted) and the delayed ADMIT.
3. VERIFIED: with it false, an admitted lifetime faults at its first batch (M:1063). INFERRED: REARM in a discovery capture therefore voids again while any admitted pre-existing backend is alive.
4. Decision: no member in the record, nothing in the P1 fix round, nothing P2 must add as specified. One section B sentence: current_epoch_continuity is false, and no discovery lifetime enters REARM attachment_proofs, until a per-binding witness exists. That is the contract's fault at first batch, and unreachable while REARM stays gated.
5. INFERRED (shape for the unit that makes REARM production): accessor-only, on pgwt_discovery_binding_snapshot: `F attachment_continuous; uint64_t continuity_through_ns`. Stamped on demand by the binding lookup, which the coordinator calls at the REARM request and at delayed-ADMIT delivery; a per-refresh stamp is always stale. True = same kernel perf event id as at enable (PERF_EVENT_IOC_ID then and now), same generation, pinned task not terminal, zero counted disable/close/re-open, all checked at through_ns. False = any of those demonstrated, sticky. Unknown = read failed, deadline, sticky fault or the P1 stub (zeroed output already reads unknown). Route c: the same on pointer_fd plus a completed snapshot fenced after REARM, so the snapshot must be on demand, not change-only (P:1093).
6. VERIFIED: the numerical watch is be->wp_fd in the backend struct (P:1366, P:1414), not in the producer entry, so "no interruption counted" is not a fact the producer owns today. INFERRED: kernel-initiated interruption is not observable by any read; that premise is shared with the fork route.
7. Owner wording: "post-VOID recovery of a discovery capture is not implemented; production REARM is off." Do not write that continuity is solved or deferred to P2.

Q2 census cut (B2)
8. Rule: the first option, re-issue in place. VERIFIED: cursor 0 is the mutable CENSUS (spec:85); a transient fence failure is "pending", never permanent (spec:195, spec:216), which excludes the third option; no appended census kind exists, which excludes the second.
9. The cut is the establishing cut: the completed pair current when the whole predicate first evaluates true. Frozen while proved; on invalidation census_fenced goes false and the pair stays as last-successful; a re-proof takes a strictly larger ordinal.
10. VERIFIED: C_close = max(census_fence, ...) (C:472) and the merge stores the fence as given (MC:890). So the review's second case (rv19/probes.log:19-20, cut 1007000000 carried, number resolved at 1021000000) understates closure. It is a defect under spec:87, not a reading question, and belongs in B2.
11. "Immutable old cuts" then means: an ordinal once read unproved is never later read proved. Gate for I1: refuse that flip, apply the positive census once per ordinal. VERIFIED it goes red on today's bug: probes.log:19 to :20 flips 1/0 to 1/1 at recon_count=1. INFERRED: no legitimate schedule flips at one ordinal (every number resolution follows completed_cut, P:1696, P:1745, P:1862); needs one host case.

Q3 B1
12. Confirmed. VERIFIED: C:316; FORK/NONMEMBER REBIND only resolves the number (MC:956-957) and needs binding and handle (MC:946); inventory() reopens a closed startup gap (MC:856-860); probes.log:8, :13 show the INVENTORY beside inventory=0.
13. The producer emits NUMERIC (as now), then one RESOLUTION R with a nonzero binding_token, the four IDs, fork_cookie in the fork case, cookie 0. No INVENTORY and no discovery cookie; the token stays retained through the resolving fence.
14. INFERRED, unprobed: the scan marks an already-published entry excluded at P:1817-1823, the same class. Gate: over every host schedule, never `excluded && inventory_reported && !kernel_only`; census inventory_count equals the live INVENTORY records handed over.

Q4 S2
15. Unknown, `{false,false}` with error=errno. VERIFIED: spec:60, spec:126; P:1300-1302; the merge break is sticky (M:691). An enable that never succeeded began no observation, so nothing was broken; not attempts-ended either (the retry succeeded, probes2.log).
16. INFERRED: the merge input is a bool, so such a record must produce no POINTER control at all; one I1 case.
17. Do not overclaim the fix. VERIFIED: the failed enable still raises the evidence total (P:1302). INFERRED: the coordinator faults that increment (spec:130), which latches certification off for the capture (C:504). S2 removes a false fact; it does not restore route c.

Q5 S3
18. Exit: only half is observable. INFERRED: the retained pointer_fd still returns the final kernel count after the task exits. VERIFIED: the observer's count sits in a map keyed by a reusable number (P:1055-1059) and BPF may not change (spec:3), so no fire-for-fire comparison exists at exit.
19. Conservative close, P1 fix: at teardown read the final count; equal to the last validated snapshot's fires = closed; greater or unreadable = one user-evidence increment and a scoped FAULT, unknown, not a break. Settling measurement: one EL8 probe that the read after exit returns the final count. If it does not, write it as a stated limit.
20. Sticky fault: one increment at the latch, never per tick (spec:199); totals stay known. Moving the comparison out from behind history() (P:1861) is P2 by spec:80, spec:99. Until then the coordinator reads "not COMPLETE and a live route-c proof" as continuity unknown, never clean.

Q6 S1
21. P1 fix. VERIFIED: the gate's placement is P1's (spec:95); P:1426 precedes the fork return at P:1431-1432; backend.c:387-388 falls through only on -ENOENT; no caller yet. Fix in the producer only. Mutant: stop, then attach a fork-born backend, must return -ENOENT.

Q7 order
22. A text-only clarification of about six lines first (items 4, 8-9, 13, 15); no header byte changes, so the identity check stays green. Then the one fix round after qualification, then P2.
23. INFERRED: I2 does not wait; nothing here touches B vectors, cookies or terminal identifiers (spec:231). Check its brief's file list against discovery_producer.c and the three host tests. I1's next round does wait for items 11 and 16.
24. VERIFIED: the unattributed EL8 run-7 assertion runs with the daemon stopped (tests/test_discovery_producer_live.py:395-407 at 566f20d5). INFERRED: it is therefore not B1; the fix round must not be reported as addressing it.
