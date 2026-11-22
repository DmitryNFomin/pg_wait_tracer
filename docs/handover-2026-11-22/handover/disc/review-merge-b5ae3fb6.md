NOT-READY

# Review: agent/988-discovery-merge, round 10 (a484d258..b5ae3fb6)

Scratchpad root: SP
- Tree under test: `git worktree add --detach rv15/wt b5ae3fb6` (clean; `scripts/tree-hash.sh` prints 0198ecdbf89798f824261d1fa5af311baa636754, the tree the handoff names). Mutant tree: rv15/mut (same commit). Legacy base: rv13/base5f, checked byte-equal to `git show 5fd6a519:` for merge.c, merge_capture.c, test_core_merge_contract.c, test_percpu_transport.c.
- Probes: rv15/probe/probes.c, appended to the host file with `main` renamed by rv15/build_probe.sh. Output: rv15/logs/probe.log. My mutants: rv15/mymut15.py, rv15/mymut15b.py (logs mymut15*.log). Implementer mutant sample: rv15/logs/impl-mutants.log, impl-mutants-item1.log. Other suites: rv15/logs/other-*.log.
- Time (date -u): 2026-11-21T17:26Z.

## What changed
- A terminal discharge prepared outside a release now sees only committed proofs plus its own candidate. Held prospective bounds and quiet readiness are excluded. Every closure path compares against a published or validated frontier, never the diagnostic cutoff.
- A lifetime takes one resolving proof: a bound and a discharge are never both committed, and a bound wins inside one attempt.
- Late TRY and wait-attempt reports after ADMIT fault. After a numerical ADMIT with no non-NULL fire they also revoke the numerical bound. An early CONFIRM_NO_WAIT_WORD faults. A valid ADMIT after a released EXIT is accepted.
- Conflicting matched terminal claims, and an unresolved claim after a matched one, revoke through the core's single path. A record that fails arrival validation and also contradicts a proof reports two faults.
- A numeric-only cookie-less EXIT can be finalised as unresolved. It is consumed as uncovered only on the ordinary release path.
- New read-only accessor `pgwt_capture_exit_disposition`.

## Evidence
- Check stamp: none. Neither wt-m10 nor my worktree has `.pgwt-check.stamp`, and the brief forbids `make check`. NOT RUN.
- Box-check: no `tests/results/box-check-*.log` exists in the main checkout. The brief excludes eBPF. NOT RUN; this is a host-only review.
- Host suite (VERIFIED, ASan/UBSan, rv15/logs/host.log): "149 oracle rows, 203 named cases passed"; rc=0. Runner: "149/149 IDs and verbatim inputs/answers match", "205 (149 rows; 65 R tuples; 29 second-amendment variants)".
- Registration guards (VERIFIED): "OK: test-registration guard ... append-only invariant holds" and "OK: runner-registration guard ...". `git diff --check a484d258 b5ae3fb6` is clean.

## Findings

### B1 (blocker). src/merge_capture.c:1029-1044: a cookie-zero TERMINAL_UNRESOLVED naming the EXIT that supplied a committed matched discharge faults but does not revoke the discharge. The gap stays closed and the reader says COMPLETE.

**Contract (DECISION 40, new paragraph):** "An unresolved disposition for the same EXIT after a matched disposition has supplied a committed discharge likewise faults and revokes that discharge ... The merge does not judge which of the conflicting claims is false."

**Mechanism (read from source):**
- The conflict is detected (`same_exit` by transport identity; `conflict` because the cookie and the unresolved flag differ).
- It is routed into the core revocation path only when `v->cookie == a->cookie` (:1035-1036). Otherwise :1044 reports a plain `pgwt_capture_fault`, with no `discovery_revoke`.

**Reproduced (VERIFIED, rv15/logs/probe.log):**
- **I4a.** Input: census {D}. `TERM_D18, EXIT_D18@L{no cookie}, S(19)` closes the gap at 18, with x=18 and END18. Then a TERMU with cookie 0, pid 30, names_exit, source 3, the EXIT's position and time 18.
  - Expected (G4b's outcome): Vu, x removed, O!, P=1+0, gap open, reader INCOMPLETE.
  - Actual: `out='EXIT_D18 F18[C0] VOID20[C0,unknown_evidence]' D:x=18 P=0+0 closed=1 C=18 latch=1 gaps: OPEN0 END18 reader(50,60)=COMPLETE`. The accessor still reports MATCHED.
- **I4e (numeric route, the U3 claims in the opposite order).** Input: U13's `X_4, A6, S(8), J_U4`, which gives x_U=4 and closes the gap at 5. Then `TERMU_U4` with cookie 0, naming X_4 (r10_unresolved(4,1)).
  - Actual: `VOID20[C0,unknown_evidence]`, x_U=4 kept, `closed=1 C=5 ... END5 reader(50,60)=COMPLETE`.
  - In U3's order (TERMU first, then J) the same two claims leave the gap open with P=1+0. The final coverage verdict therefore depends on the arrival order of two contradictory claims about one EXIT.
- **Control:** the same schedule with cookie DG (I4d, the implementer's G4b) revokes correctly: `O!0 reader INCOMPLETE`.

**Reachability:** a coordinator that evaluates numeric-only finalisation and submits its cookie-zero TERMU after J has already bound the EXIT. This is exactly the race the U rows model. The TERMU's claims are then false, and the contract says the merge must not judge which claim is false.

**What the fix must cover (not a design):**
- Any unresolved disposition that names, by transport identity or by PID and time, the EXIT supporting a committed matched discharge must revoke that discharge, whatever cookie the control carries.
- Add I4a and I4e as named cases, with a mutant.
- A naive widening of :1035 (my myC: drop the cookie equality) turns G_ambiguous_terminal_owner red. The fix must keep that guard's outcome.

### S1 (should-fix). merge.c:713 and :729: the "no qualified non-NULL pointer fire" exception has no failing test.
- DECISION 38 (new): a late TRY or wait-attempt report after a numerical ADMIT revokes the bound only "with no qualified non-NULL pointer fire recorded for that lifetime".
- My mutant my1 (revoke regardless of `pointer_nonnull`) passes the whole host suite: `my1_TRY_revokes_despite_nonnull_fire GREEN(survived)`.
- Probe I3c (VERIFIED) is `PTR_D5(non-NULL), AD_D6, B_D6, S(8), TRY_NW_D6`.
  - On the product: Vu, b=6 kept, no O!. This is correct.
  - Under my1: b removed, O!, P=1. This is wrong, and no test sees it.
- Add I3c (and its WAIT variant) as named cases.

### S2 (should-fix). merge.c:762 together with merge.c:633: the own-EXIT provenance rule is enforced only when the EXIT arrives before the ADMIT.
- DECISION 36 (new): "An ADMIT whose `e` is later than the timestamp of that lifetime's own bound EXIT is contradictory provenance."
- Line 762 checks this only for an EXIT already recorded. An EXIT that arrives later is accepted, because `discovery_record_consistent` returns true for non-TRANS ranks.
- Reproduced (VERIFIED):
  - I3e: `AD_D19 (numerical, e=19), EXIT_D18@L (D cookie), B_D19, S(20)` gives `EXIT_D18 F19[C0] D:b=19 closed=1 C=19 latch=0 END19`, with no fault.
  - I3e' (reverse order): `EXIT_D18, S(19), AD_D19` gives `VOID20[C0,unknown_evidence]`, latch=1.
- Expected for I3e: FAULT(unknown_evidence), with no bound committed from the contradictory admission.
- Impact: the persisted END (19) is later than D's exit, so I found no wrong availability number. The defect is a missing loud refusal: certification stays unlatched on a proof the contract calls contradictory.

### Nits
1. The commit message again ends with `Co-authored-by: Codex <noreply@openai.com>` after `Refs #988`. VERIFIED with `git log -1 --format=%B b5ae3fb6 | tail`.
2. The `e == exit_stamp` boundary at merge.c:762 is untested: my6 (`>=`) survives. The product is correct by reading.
3. The order check for an unresolved EXIT (merge.c:975-976) has no failing case: my9 survives. It is defence-in-depth, identical to the ordinary-record check at :1065.

## Brief items 1-7: premise checks (all VERIFIED on the built code, rv15/logs/probe.log, unless marked otherwise)

### 1. Closure from committed evidence
- **STa** (the brief's ST1): before TEXT, x_D=18, P=1+0, open, reader INCOMPLETE; a repeated TERM is idempotent. After TEXT x arrives, H21[U] gives b_H=21, C=21, END22 (P_close=C+1 for the fresh anchor), COMPLETE.
- **STc** (TERM, then K, REARM, S40, TEXT): latched, H never bound, open.
- **STk** (K before TERM): no discharge under the latch, open.
- **STd** (two lifetimes, both EXITs released, an unrelated held batch): TERM_H19 then TERM_D18 closes at 19 from committed x_H plus the candidate x_D. This is the positive half of G8.
- **STe** (quiet H behind the held batch): open until TEXT, then b_H=20 and END20.
- **STf** (resumed release rejected by the writer): open and latched.
- **STg** (EXIT inside the held batch): no discharge until the release completes; then x=18, b_H=21, END22.
- **Reading:**
  - `pgwt_merge_discovery_candidate_state` (merge.c:1758, :1763) ignores the plan and `quiet_ready` while `terminal_candidate` is set.
  - `discovery_progress` (merge_capture.c:694) uses one frontier test for every caller. The TERM path passes the published frontier (merge.c:820); the release paths pass W-1 and F (:1215, :1251).
- **Mutants:**
  - Implementer's r10_stale_held_prospective, r10_stale_quiet_ready and r10_diagnostic_cutoff_completes: RED. My my8 (TERM prepare fed the cutoff): RED.
  - My myA (stale prospective read alone) survives and is equivalent. Prospective anchors of a held attempt have t >= previous W = F+1, so the published-frontier test can never pass for them (merge.c:929 comment, :1068; INFERRED from the code). myB (stale quiet_ready alone): RED on ST2b.

### 2. One resolving proof
- **Same attempt as EXIT:** I2a (quiet), I2b (no-word) and I2c (fresh) each commit b and no x (closure at 6, 6 and 12 with END13).
- **I2d:** a bound, then EXIT and TERM, then a late TRY. The bound is revoked, no x is promoted, and P=1 even after TERM is re-sent and S25.
- **I2g:** EXIT released, then AD and B behind a held batch (quiet_ready=1), then TERM. The discharge commits first (closure at 18). After TEXT no bound commits.
- **Reading:** predicates at merge.c:539-550 and the release guard at :1205-1206.

### 3. Control ordering
- **I3a:** a late TRY while suspended revokes b, and the gap stays open after REARM.
- **I3b:** WAIT twice gives one O! and two Vu.
- **I3c:** see S1.
- **I3g and G5:** a valid ADMIT after a released matched EXIT does not fault.
- **I3f:** ADMIT e19 after the matched EXIT_D18 faults. The x=18 discharge is kept: see design question Q1.
- **I3h:** a duplicate CN is idempotent.
- **Previous review's mutants:** k30 (lifecycle_seen restored) is RED on G5 and G7.

### 4. Conflicting terminal claims
- **I4b:** a conflicting TERM_D19 in a fatal epoch revokes, giving O! and P=1.
- **I4c:** the same while suspended revokes.
- **I4d:** TERMU with cookie DG after a control-path discharge revokes.
- **Gap:** a cookie-zero TERMU does not revoke (B1).

### 5. Two faults for one observation
- **I5b:** a seal violation that also refutes x gives `VOID20[C0,seal_violation] VOID20[C0,unknown_evidence]`, one O!, and counts seal=1, unknown=1.
- **Order in merge.c:1356-1360:** evidence (:1311-1325) first, then `late_fault`, then `fault`.
- **Duplicate identity:** the breach check moved ahead of evidence retention (:1308-1310), so it remains the sole report (G_duplicate_no_evidence).

### 6. Numeric-only unresolved EXIT
- **I6a:** TERMU, then K, then the EXIT received while suspended, then REARM and S30. uncovered stays 0 and the number stays pending: suspended receipt is never counted. This matches DECISION 40 and the implementer's G_unresolved_received_suspended.
- **Mutants:** my3 (no drop reclassification), my4 (unchecked named identity) and my5 (cut strictly after EXIT): RED.

### 7. Interface
- **Declarations:** merge_capture.h:165-171 match the brief token for token. `pgwt_capture_discovery_state` and both latch fields are unchanged in the diff.
- **After a writer error:** the disposition is recorded before core delivery (merge_capture.c:1067 precedes :1084). The accessor answers MATCHED; the implementer's G_accessor_writer_error passes, both named and time-only.
- **EXIT never seen:**
  - A named disposition answers by transport identity.
  - A time-only (`names_exit=false`) disposition answers NONE until the EXIT is mapped. Probe A2: `disp(30,3,1)=0`, then 1 after arrival. The binding happens at merge_capture.c:1428-1431.
  - Within the writer-error scenario the EXIT has always been seen, because discharge requires `exit_release_committed` (INFERRED).

## The enumeration's 270 "closed while pending/census not clean" schedules: explained
- **VERIFIED by probe E1/E2.** Inside the VOID sink callback of a revoking fault, the views read `closed=1 P=1+0`, and the persisted reader still says COMPLETE. After the call returns they are coherent: `closed=0`, O!, INCOMPLETE.
- **Mechanism (read from source):**
  - The core clears the proof (`discovery_revoke`, merge.c:703, :714, :723, :730) and then calls `fault()`, which emits VOID synchronously (:276).
  - The capture resets `startup_closed`, the census and the revoked OPEN only after `pgwt_merge_feed` returns (merge_capture.c:262-266).
  - The enumeration checks views inside callbacks (discovery_enum_host.c de_sink → de_callback_invariant, prefix 7 = in-step). Its shortest word, ADc6 CN6 S19 PTR_NONNULL, is the POINTER path, unchanged since 89b6cba4. G2b adds the new TRY site.
- **Reachability:** SEARCHED src/ for callers of `pgwt_capture_discovery_status`, `pgwt_capture_discovery_state` and `pgwt_capture_exit_disposition` outside merge_capture.c; the only hits are header declarations. The daemon's VOID sink (open_snapshot cov tracker) reads none of them. NOT SEARCHED: other branches and the future coordinator.
- **Classification:** not a defect of this commit on any current path. It becomes one if the integration unit reads these views from inside a sink.

## Mutants
- **Implementer, random sample (seed 20261121, 12 of 288):** r10_finalise_before_retained_witness, discharge_VOIDed_EXIT_suffix, exit_unknown_start_precedes_F0, n19_F0_after_boot_ok, publish_abandoned_MARKER, n12_gone_ignores_journal, abandon_supplies_lower_bound, stale_marker_baseline, forget_prearrival_disposition, o_capture_terminal_conflict, n29_late_inventory_keeps_census, n20_fence_before_census_ok.
- **Implementer, extra r10 sample (seed 7, 6):** r10_unresolved_keeps_discharge, r10_CN_generation_under_latch, r10_seal_before_clock, r10_reject_unresolved_after_latch, r10_terminal_blocked_by_unrelated_hold, r10_discharge_with_quiet_bound.
- **Result for both samples:** "18/18 RED". Plus the item-1, k28 and k30 set: "5/5 RED". VERIFIED.
- **Mine, against the full host suite (rv15/logs/mymut15*.log):**
  - RED: k28, k30, my2, my3, my4, my5, my8, myB.
  - Survivors: my1 (S1), my6 (nit 2), my9 (nit 3), my7 (equivalent: a discharged lifetime lingers in the quiet list but cannot commit, because :1261 is gated by `discovery_bound_allowed`), myA (equivalent, see item 1).
- **No test passes for the wrong reason** among those I read (ST0-ST3, G8, FX2, G_one_proof_*, G_late_attempt_*). Each asserts the full output, b/x, the pending split, the latch, the gap history and the [50,60) reader. `r10_primitive` checks coherence after every primitive.

## Changed existing cases (VERIFIED by reading every removed line of the host-test diff)
- N6 and P3: x is no longer asserted (DECISION 41, "supplies no terminal discharge").
- G_unrefuted_lower_survives_terminal_refutation: b=12 with END12 and no O! (the same-attempt rule).
- G_unresolved_*: uncovered counted only on active release.
- G_mapped_hold_reset: new setup, same expected output.
- P2: adds S(14). R12: the END-before-O! order is now asserted.
- Each traces to the sentence the handoff cites. None weakens an outcome without one.

## Discovery disabled and legacy
- **Legacy bytes (VERIFIED, rv15/logs/leg-*):** test_core_merge_contract (127 lines), test_core_merge_adapter (113), test_transport_wire (6) and test_percpu_transport (168) are byte-identical to 5fd6a519 (cmp); all rc=0.
- **Reading:**
  - The breach-check move (merge.c:1308) keeps the disabled order, because the block it now precedes is discovery-only.
  - The uncovered paths are gated by `discovery_enabled` (merge.c:972, :1133, :1362).
  - `discovery_prepare` is NULL when disabled.
  - The new capture branches are gated (merge_capture.c:1217, :1421, :1441, :1503).
- **Other host suites:** All rc=0 (VERIFIED, rv15/logs/other-summary.log): core_merge_mutants.sh ("36/36 killed", including the re-anchored M8 and M29), core_merge_adapter_mutants.sh ("27/27 killed; guard sweep 31/31 PASS"), test_core_merge_queue.py, test_capture_bookkeeping.py, test_capture_recovery.py ("19/19 killed"), test_transport_startup.py, transport_diagnostics_mutants.py ("5/5 killed"), test_exact_perf_coverage.py --mutants (18 mutants, rc=0).

## Design questions for the owner
- Q1 (an adviser question first, under RULE 1): should a valid-format ADMIT whose e is later than the EXIT of an already committed discharge (I3f) revoke that discharge, or only fault? DECISION 36 calls it contradictory provenance but names no revocation; DECISION 43's general clause could be read either way. The product faults and keeps x=18 closed under the latch.

## Not checked
- make check, box-check and anything eBPF-related (excluded by the brief).
- All 288 implementer mutants: only 23 were run.
- The full 1,726,074-schedule enumeration (not re-run).
- open_snapshot's handling of a revoked PREEXISTING OPEN: read, and unchanged by this commit.
