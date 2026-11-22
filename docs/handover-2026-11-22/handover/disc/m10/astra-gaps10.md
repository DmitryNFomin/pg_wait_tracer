VERIFIED: References below use `C` = `origin/master:contracts/merge-v1/CONTRACT.md`, `R/E` = its `oracle-discovery/ROWS.md` and `EXPECTED.md`, and `M` = `scratchpad/wt-m10/src/core/merge.c`. `git rev-parse origin/master` returned `6f18da28784da73bae2dd97621fe311dab984f2c`; the worktree HEAD is `b5ae3fb6579124fcc08082dcff3422e2ba82cff5`.

1. **INFERRED — Q1: fault and revoke.** Add: “An independently lifetime-bound ADMIT claiming successful enablement after that lifetime’s EXIT contradicts the terminal proof; reject the ADMIT and revoke any discharge supported by that EXIT under Decision 43.” Choosing the EXIT over the conflicting successful-enable claim would introduce an unjustified evidence preference.
   INFERRED — Pin `TERM_D18, EXIT_D18, S(19), AD_D19`: fault, remove x, publish one revoked OPEN, pending `1+0`, census unchanged, latch set. Add `e=18` as the non-faulting boundary.
   VERIFIED — C:360 calls this contradictory provenance; C:460 refuses to choose between conflicting terminal claims; C:506 specifies revocation consequences.

2. **INFERRED — P2: reject before recording admission.** Add: “An ADMIT refuted by retained evidence records no accepted admission, route or baseline; preserve independently attributable evidence without treating the rejected control as an accepted proof.” An unrelated counter fault does not invalidate an otherwise valid ADMIT recorded under the latch.
   INFERRED — Pin `TRANS12, ADc6, TRY_NW6`, with pointer-fire and continuity-break suffix variants: the ADMIT faults, `admission_recorded=false`; suffixes do not fault solely because of that rejected admission. No bound or discharge commits.
   VERIFIED — M:837–844 currently records route and admission before checking numerical evidence; E:167 already rejects the analogous numerical admission without accepting it.

3. **INFERRED — G1: retain the adverse evidence.** Change “is not recorded” to “is not accepted as an attachment attempt; retain any independently attributable contradiction of the admitted address proof across VOID and REARM.” Later controls relying on that same refuted proof are invalid; apply the reporting rule below.
   INFERRED — Extend G2b with `B_D6, AD_D6`: retain address refutation, accept neither new claim, restore no proof. Include the qualified non-NULL-fire exception and route-(c) controls.
   VERIFIED — C:426 explicitly makes the late report evidence against the numerical address proof, subject to that exception.

4. **INFERRED — G2: retain the candidate as evidence.** Add: “A recorded quiet candidate remains reconciliation evidence across VOID and REARM, regardless of which arrived first; its retention supplies neither commitment readiness nor permission to certify.”
   INFERRED — Pin both `B6, K, RA, EARLIER_TRANS14` and `K, RA, B6, EARLIER_TRANS14`: D14 contradicts the retained candidate and faults; no bound, END or revoked OPEN results from this uncommitted candidate.
   VERIFIED — C:490 requires reconciliation against proposed bounds; M:259–263 currently clears pending candidates on fault.

5. **INFERRED — G3: one fault per contradicting observation, coalesced within that observation**, subject to the redundant-report exception below. Add: “Refutation remains permanent; each subsequent observation is checked against retained claims and evidence, with at most one discovery unknown-evidence report for its contradictions; identical retransmission retains its stated idempotence.”
   INFERRED — Pin `FINAL_TRANS20, TERM19, EXIT18`: TERM faults and permanently refutes the claim; EXIT supplies another contradiction, never another discharge or revocation. Its redundant report may be omitted under the exception.
   VERIFIED — C:460 preserves terminal evidence and duplicate idempotence; C:508 already coalesces discovery contradictions within one DATA observation.

6. **INFERRED — G4: validate while held.** Add: “An unavailable dependency prevents publication and commitment, but does not defer checks decidable from available evidence; prospective release-order anchors participate in validation only and supply no committed proof.”
   INFERRED — Pin `AD6, FINAL_TRANS12, EARLIER_TRANS14, A20{requires x}, S(40)`, withholding TEXT: fault at batch validation, latch set, no DATA/frontier/bound/END. Retain G8 as the positive held-batch control.
   VERIFIED — C:109–110 requires complete-batch validation; C:480 prohibits using held prospective state for closure. M:997–998,1068–1074 implements the validation reading.

7. **INFERRED — T1: expose both facts explicitly.** Add to the accessor contract/header: “`admission_recorded` means a valid ADMIT was recorded, including under suspension or the latch; `admitted` means it was accepted while certification remained possible. Both describe history, not a presently valid committed proof.” Coordinator disposition decisions and enumeration must use `admission_recorded`.
   INFERRED — Pin `K, RA, ADc6` with `(admission_recorded,admitted)=(true,false)`, and `ADc6,K` with `(true,true)` plus latch set; separately inspect committed-proof fields.
   VERIFIED — M:839,845–846 implements those distinct facts; C:504 requires valid post-latch recording without renewed certification.

8. **INFERRED — T2: define discovery views between primitive inputs.** Add to contract and headers: “Discovery accessors provide a coherent snapshot after a primitive input completes and before the next begins; sinks must not call these accessors or reenter input processing.” Have the coordinator defer inspection until return and the enumeration check coherence there.
   INFERRED — Pin `ADc6,CN6,S19,PTR_NONNULL`: after return, no bound, gap open, pending `1+0`, census unknown, latch set, revoked OPEN persisted. Do not assert an intermediate discovery snapshot inside VOID.
   VERIFIED — R:56,63 specifies observation after primitive inputs; `src/merge_capture.c:247,263–269,642–649` explains the mixed intermediate view.

**INFERRED — Smaller reporting rule:** “Once certification is latched and the epoch is already suspended, an internally detected discovery contradiction may omit a further unknown-evidence report if it requires no additional epoch, coverage or publication consequence; retain its evidence and rejection/refutation state regardless. Otherwise existing fault publication remains mandatory.” Allow zero or one such redundant report per observation, never fabricated reports.

INFERRED — Keep exact first-fault behavior, revocations, ordinary counter/arrival faults, explicit FAULT inputs, publication failures and Decision 43’s two-fault observation. In particular, a contradiction after REARM must still suspend ordinary release: the discovery latch alone does not make later ordinary output harmless. VERIFIED — C:144–150,504 preserves those ordinary release/fault effects.

INFERRED — Enumeration should compare retained evidence, dispositions, proof validity, pending/census state, epoch state, coverage and consumption exactly; permit only the specified redundant reports to vary, and require diagnostics to match reports actually emitted. Lost sensitivity: a detector that stops diagnosing further contradictions while already suspended could pass if it still retains evidence and performs every required state effect; exact counts would catch that diagnostic omission.

INFERRED — G4 still needs its row because it decides whether the first fault occurs. P2 still needs its row because accepted-admission state is normative independently of reporting. The proposal must explicitly amend affected exact post-latch expectations; it cannot merely loosen the test comparator.

INFERRED — Do one compact contract delta first, freeze independent answers, then round 11 against the settled rules. This keeps evidence retention, accessor semantics and fault reporting in one implementable specification. VERIFIED — `docs/REWORK_PLAN_2026-11-11.md:64–67` requires separately reviewed contract changes and frozen independent answers before implementation.