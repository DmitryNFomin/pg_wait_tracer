# Review: agent/988-discovery-merge, round 5 delta (b77375cc..d5ad2ac3)

Verdict: **NOT-READY**. One blocker. A non-NULL pointer observation that reaches the merge before a route-(c) ADMIT is ignored, so the no-wait-word bound and the startup closure commit anyway. Two should-fix items and three nits follow. Everything else asked for in the brief is VERIFIED below.

Paths below are relative to the scratchpad base SP.

## What changed
- Each final terminal disposition is now stored per EXIT, keyed by (pid, EXIT time) or by an exact (source, position). Two cookie-less EXITs of one PID each consume their own unresolved disposition. An unresolved disposition no longer marks a resolved lifetime unresolved.
- The no-wait-word route 37(c) is new:
  - Pointer-watch observations arrive as TRY_NO_WAIT_WORD and POINTER.
  - Route c is selected by an ADMIT with route=NO_WAIT_WORD, followed by CONFIRM_NO_WAIT_WORD.
  - Supporting evidence is published before the bound commits.
  - A later non-NULL fire faults, revokes coverage from boot_start and re-opens the census.
  - The lifetime is reported as "present, not measured" with its role.
- Unbound enumerated PIDs are new: they are kept as numeric observations and resolved by J, R or G controls. Pending is reported as lifetimes plus numbers. Changes in the lifecycle-loss counters (components 0/1/2/10) make the census unknown. ma_discovery_exit classifies exits by the three facts.
- 62 new mutants (172 in total), 12 new named guard cases, and all 108 oracle rows.

## Evidence
- Check stamp: not run by me (read-only brief; a check is running in wt-discm).
  - scratchpad/a2go/disc/checkfast-d5ad2ac3.log is in progress; its tail shows no summary.
  - **NOT VERIFIED.**
- box-check: there is no tests/results/box-check-*.log in the main checkout, and I ran no box. The C unit tier and the live tiers on Linux are **not verified**.
- All runs below are mine on `git archive` exports in rv5/{head,base5f,probe}, with ASan/UBSan.
  - `test_discovery_admission.py --mutants` (PYENV 3.11.7): rc=0.
    - "108/108 IDs and verbatim inputs/answers match".
    - 108 oracle rows plus 39 named cases PASS; empty census PASS.
    - **172/172 RED** (rv5/logs/disc-mutants.log).
  - Byte-identical to 5fd6a519 (cmp). The sha256 prefixes match round 4:
    - test_core_merge_contract: 127 lines, ddf62059…
    - test_percpu_transport: 168 lines, 2eae924a…
    - test_transport_wire: 6 lines, 4d9390d3…
    - test_core_merge_adapter: 113 lines, 636f53fc…
  - T-cov `--mutants`: rc=0 on both trees, 18 RED, and the MUTANT/PASS lines are identical.
  - core_merge_adapter_mutants: 27/27 killed, guard sweep 31/31.
  - core_merge_mutants: 36/36 killed; core merge contract 99/99.
  - Also rc=0: capture_bookkeeping, capture_recovery (19/19 mutants killed), transport_startup and pilot_route.
- Diff hygiene:
  - `git diff --check b77375cc d5ad2ac3` is clean.
  - contracts/, tests/unit_tests.list and tests/run_all.sh are unchanged. web/ is untouched.
  - The commit message contains "Refs #988", but a Co-authored-by trailer follows it, so it is not the last line.
- Registration guards: not re-run by me.

## Brief items (reproduce or refute)
1. **Per-EXIT dispositions: VERIFIED for the round-4 scenarios.** The round-4 probe was rebuilt on d5ad2ac3 (rv5/logs/p_disp.log).
   - P2b: `F18[C0] F25[C0]`, uncovered=2, no VOID.
   - P5: unresolved=0 and closed=1 are kept.
   - rv5/probe/tests/p5.c (rv5/logs/p5.log):
     - Q1, three EXITs each unresolved: `F18 F25 F32`, uncovered=3, finish rc=0.
     - Q2, dispositions before the EXITs in reverse order: correct.
     - Q3, both EXITs held with U25 before U18: nothing is released until U18, then F25 and uncovered=2.
     - Q8, a duplicate is idempotent; a later matched disposition for the same EXIT gives VOID.
     - Q9, a named disposition and a legacy one for the same EXIT conflict: VOID.
     - Q10, a named disposition with the wrong position: the EXIT is held and finish gives VOID50.
     - Q11, the right position but the wrong time: VOID.
   - The variants where the unresolved disposition comes first and the lifetime is resolved later (Q4 and Q6) close the gap. See finding 2.
2. **Six round-4 guards: VERIFIED.** I re-ran my own removal of each (rv5/logs/mymut5.log, g1–g6). Each is RED on its new case:
   - G_disposition_uninventoried
   - G_discovery_after_finish
   - G_unresolved_reuse and G_unresolved_resolved_lifetime, for :505/:525 as re-expressed
   - G_mapped_hold_reset
   - G_endpoint_call_overflow
   - G_uncovered_position
3. **Rows N1–N16 and J1–J22: VERIFIED against EXPECTED.md.**
   - I compared every structured value in discovery_oracle_expected.h with EXPECTED.md by hand: outputs, C_close, G(), b, P=a+b and census. All match.
   - The host checks the complete outputs, the GAP_CLOSE endpoint through the reader, and the lower-bound and terminal state.
   - Hunt results (rv5/logs/p6.log):
     - (a) A pointer read before enable, a fire inside [enable, fence], or a missing role proof is each refused. A NULL fire is a no-op. A non-NULL fire after the bound faults and revokes (N5). A non-NULL fire *delivered before* ADMIT does not; see finding 1.
     - (b) A number never becomes a lifetime by itself. G under a changed Ci counter does not resolve (probe F). The counter-class case passes and its three mutants are RED.
     - (b) J timestamp contradiction (probes D and D2: the control times agree, the DATA is at 5, in both orders): `VOID20[C0,unknown_evidence]`, L1 N0, census known, and the identity is kept.
     - (b) A second cookie-less EXIT for a J-resolved PID (probe E): it is held, then consumed by an unresolved disposition. A matched disposition for it (E2), or a second J (E3), gives VOID.
     - (b) R with MISSING_FORK leaves the number pending.
     - (c) Both parts of P are maintained, and the gap cannot close while either is nonzero. A stray path exists; see finding 3.
4. **Nothing commits mid-attempt: VERIFIED** (probes C, C2 and C3):
   - A no-word bound with a rejected A7 gives `VOID20[Cw,writer_push_fail]`, no bound, gap open.
   - A rejected FRONTIER gives the same result.
   - Ce at the frontier checkpoint gives `F7 VOID20[Ce,counter_change]` with no bound.
   - J18 and J19 cover the J discharge.
   - A G resolution is not release-scoped; it is refused under any counter delta.
5. **Discovery unused: VERIFIED** (four cmp checks and T-cov; see Evidence).
6. **62 new mutants:**
   - All 62 are RED in my run on their named rows.
   - I sampled 17 and read the anchor and assertion of each: no_word_requires_quiet, NULL_implies_permanent_role, NULL_read_before_enable, no_word_equal_completeness, no_word_pointer_not_a_fault, no_word_initialization_handoff, NULL_fire_counts_read_loss, authentication_timeout_closes_gap, gone_before_failed_open, journal_without_completed_producer, exit_start_equal_F0_is_preexisting, rebind_fork_in_discovery_inventory, journal_exit_current_PID_cookie, census_invalidated_by_nonlifecycle_loss, numeric_forgets_failed_open_update, invent_no_word_terminal_TRANS and missing_fork_not_counted.
   - Each fails for its stated reason, with two exceptions (nit 2).
   - J7–J11 kill only the classifier ma_discovery_exit, through the host's simulated emitter. The consumer's EXIT_OBSERVATION path treats every class except UNKNOWN identically, which is acceptable for this unit.
   - My own 35 mutants on the new code (n1–n35; 5 RED: n8, n9, n23, n30, n33) are recorded in rv5/logs/mymut5.log; see nit 3 for the 30 survivors.
7. **Handoff header (pgwt_discovery.h)**, the nine kinds and what the producer must supply:
   - CONFIRM_NO_WAIT_WORD: the same `no_word` witness as the route-c ADMIT (role, enable, read_at, fence, handles), and ADMIT's `e` equal to `no_word.enable`.
   - TRY_NO_WAIT_WORD: `no_word` (`continuous` sets pointer_watched) and `authentication_deadline`.
   - POINTER: `pointer_value`, `pointer_read_ok`, `pointer_continuous`. `pointer_time` is never read.
   - WAIT_ATTEMPT: one per physical attempt; it is refused for a watched NULL candidate.
   - WAIT_LIMIT: compared against the detection time `d`.
   - JOURNAL: `resolution` fully qualified, `names_exit`, `exit_source=L`, `exit_position`, `terminal_time` and `resolution.exit_time`, plus a discovery-range cookie that the producer must reserve (Decision 32).
   - REBIND: `resolution` with binding/handle, `journal_eligible=false`, the cookie, and proof bits if the identity is installed.
   - GONE: `gone_at` after the failed open registered through `pgwt_capture_discovery_numeric(..., failed_open)`, `fence >= gone_at`, and `fence >= census fence`.
   - EXIT_OBSERVATION: `resolution` exit facts; the producer must already have counted any missing fork.
   - Ambiguities for the integration unit:
     - (i) Which of `terminal_time` and `resolution.exit_time` is the journal's time and which is the DATA timestamp. The handoff text says terminal_time is the journal's, but the field names suggest the reverse. This is harmless: mapped() also checks the real DATA timestamp (probe D).
     - (ii) POINTER must not be issued before a route-c ADMIT while finding 1 stands.
     - (iii) A PID bound normally after `coverage_begin(pids)` is never resolved (finding 3); only REBIND resolves it.
     - (iv) The capture baseline must be taken at or before F0, or the lifecycle-class latch does not cover [F0, capture start].
     - (v) `ma_discovery_exit` mirrors the BPF filter; it is not called on the EXIT path itself.

## Findings
1. **blocker — src/core/merge.c:561/594/640: a route-c ADMIT ignores a non-NULL pointer observation already retained for that lifetime.** Reproduced (rv5/probe/tests/p6.c, case A).
   - Input: D inventoried with a complete census. `TRY_NW_D(6; NULL read at 7, fence 7, role 13 qualified)`. Then `POINTER(D, t=9, non-NULL, read_ok, continuous)`, which sets `pointer_nonnull=1` (printed). Then the route-c ADMIT, CONFIRM_NO_WAIT_WORD and `S(10)`.
   - Expected:
     - Decision 37(c): "A fire observing non-NULL contradicts the permanent-role proof and follows Decision 43".
     - Decision 36: a claimed admission proof containing a false required fact gives `FAULT(unknown_evidence)`.
     - The same physical history delivered in N5's order (POINTER after ADMIT) is VOIDed and revoked.
   - Actual: `F9[C0]`, b_D=6, closed at 6, census complete, P=0+0. The lifetime is shown as "present, not measured" while the merge holds proof that it initialized. Its waits are then never traced, under a closed startup gap.
   - The outcome depends on control delivery order. This is a fail-safe violation: the merge accepts a proof that its own retained state contradicts.
   - A weaker sibling (case B): a NULL candidate without a role proof, pointer never non-NULL, then a numerical ADMIT and quiet CONFIRM, is accepted (b=11, closed). WAIT_ATTEMPT refuses exactly this state at :564, but ADMIT and CONFIRM do not.
   - Fix: fault the route-c ADMIT/CONFIRM when `pointer_nonnull`, and fault a numerical ADMIT when `pointer_watched && !pointer_nonnull`. Add one case each with a removal mutant.
   - Not reachable today: discovery is enabled nowhere in src/ (INFERRED, as in round 4).
2. **should-fix — src/merge_capture.c:595-613, with tests/discovery_admission_host.c:1489-1493: an unresolved disposition no longer keeps the startup gap open.** Reproduced (p5.c Q6, Q4, and the implementer's own G_unresolved_does_not_block_closure).
   - Q6 input: D pending. `EXIT_pid30@18{no cookie}, S(19), TERMU_D18`, then `AD_D20, D22, S(24)`.
   - Expected (Decision 40): "the entry stays unresolved … and the startup gap stays open for the rest of the capture". Round 4 enforced this.
   - Actual: `F18 D22[U] F23`, closed=1.
   - Q4 (unresolved at 18, matched at 25) also closes. G_unresolved_does_not_block_closure *asserts* closure in the case where D is bound, an unresolved EXIT of pid 30 arrives while H is pending, and H is then bound.
   - Under the amended three-fact filter, a second cookie-less pre-F0 EXIT of one PID, or one for a bound lifetime whose cookie is installed, is a producer contradiction (INFERRED). The brief only required that an already gap-closed lifetime not be marked unresolved.
   - Fix: a capture-level latch. An unresolved disposition consumed while the gap is open blocks closure for the rest of the capture. It must not alter resolved entries or P. P5 and G_unresolved_resolved_lifetime keep passing; G_unresolved_does_not_block_closure inverts.
3. **should-fix — src/merge_capture.c:658 and :783: the documented `coverage_begin(pids)` path never closes.** Reproduced (rv5/probe/tests/p7.c).
   - Input: `pgwt_capture_coverage_begin(c, boot, {30}, 1)` with discovery enabled. Then `pgwt_capture_discovery_inventory(30, DG)`, installed, `census(5, true)`, `AD_D8, D12, S(13), S(40)`.
   - Expected: the header (merge_capture.h:130) still says "PID-only entries await lifetime binding". DECISION 31 keeps numeric observations only for PIDs that *cannot* be bound, and DECISION 41 says "A number is not counted again after it is resolved into an inventory lifetime".
   - Actual: `D12[U] F12 F39`, P=0+1, census unknown, gap open forever. Round 4 filled the PID placeholder here.
   - The failure is safe (no wrong number), but the documented API now has no closure. Either let a successful inventory binding resolve that PID's numeric observation, or stop registering `pids` as numeric and update the header.

Nits:
1. src/merge_capture.c:732 and :805: after numeric-capacity overflow (`preexisting_tracking_failed`), the status can still report the census complete with P=0 while closure is refused (probe G: `census=1 unres=0 closed=0`). Two views disagree, though only in diagnostics. The round-4 inventory overflow had the same shape.
2. Two of the 62 mutants mutate the host harness, not product code: invent_no_word_terminal_TRANS and missing_fork_not_counted. The product guard that refuses a TRANS for a no-word lifetime (merge.c:511) has no red test; my n24 survives.
3. New guards with no red test. Equivalent or redundant: n18 (redundant with census_complete), n3, n6, n28. The other survivors among my 35 own mutants, each VERIFIED SURVIVED:
   - no_word_valid: fence ≥ read_at, handle equality, binding/ownership.
   - The CONFIRM role match, and the ADMIT `e` vs `no_word.enable` cross-check.
   - POINTER discontinuity after the bound (DECISION 43, "demonstrated break").
   - The before/after counter delta in J/R/G, F0 ≤ boot_start, and fence ≥ census fence.
   - G `fence ≥ gone_at` and G `!journal_eligible`.
   - R J-precedence and R binding/handle.
   - J `names_exit`/position and J tgid.
   - A cookie-less EXIT for a pending number arriving *before* its J. This is a real ordering, and the mutant n17 survives.
   - The discovery-range cookie counted as fork, and unqualified clocks.
   - TRY/WAIT_ATTEMPT after ADMIT, a quiet CONFIRM after a no-word ADMIT, and a TRANS for a no-word lifetime.
   - A late inventory entry keeping the census, the named-exit source check, the mapped time check, and WAIT_LIMIT before the deadline.

## Design questions for the owner
None. Findings 1–3 can be settled from the contract text.
