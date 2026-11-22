PROCEED WITH CHANGES: one contract delta first, then a single round 11. P1 must not ship alone; item 7(a) is a product change, and the "count is not normative" rule should not be adopted.
Paths: mc = wt-m10 src/core/merge.c, cap = src/merge_capture.c, host/model = tests/discovery_enum_{host.c,model.h}, M/R/E = contract, ROWS, EXPECTED at origin/master 6f18da28 (byte-equal to wt-m10, VERIFIED by cmp).
**Found while checking 7(a), larger than the seven items**
- VERIFIED (triage build of b5ae3fb6, `triage-work/w.sh N <word> <fault> 0 --trace`, events of the last prefix): an ADMIT recorded under the latch never releases its lifetime's records, and the whole frontier stays held for the rest of the schedule. `[AD6,FINAL_TRANS12,S19] fault=0` gives VOID only. The same word with `fault=1` (ADMIT before the fault) gives DATA12, F18, F39, F40. `[S19] fault=0` gives F18, F39, F40.
- VERIFIED: the hold is mc:986 (`!generation->discovery.admitted`), and mc:845 leaves `admitted` false for every latched ADMIT.
- VERIFIED: ABANDON is the only exit today: `[AD6,FINAL_TRANS12,S19,AB,S19] fault=0` gives F18, F39, F40. P1 as worded closes that exit. INFERRED: one fault during startup, one ADMIT arriving after it and one record of that backend then stop all publication until restart.
- VERIFIED: the enumeration cannot see this. After the first fault it compares VOIDs only (host:676-677; comment at host:413-416). No row has an ADMIT after K followed by that lifetime's DATA (R8a puts AD before K, R:184).

**7(a) T1: product change.** Keep one per-lifetime fact, "a valid ADMIT is recorded", and expose that as `admitted`.
- INFERRED (mc:541, mc:845, M:504): for commitment the second fact adds nothing, because `discovery_bound_allowed` tests the latch itself. It differs only under the latch, at mc:751, 808, 881, 986, 1024, 1049, and each of those treats a recorded ADMIT as absent.
- Sentence (D43): "A valid ADMIT recorded under the latch removes the missing-ADMIT hold of Decision 36; that lifetime's records then release under the ordinary rules, uncertified, and commit no bound." Row: `K, RA, S(25), AD_D6, D26@P0{final}, S(27)`, expected as R8(a) with RA (`F24[Cr] → D26[U] → F26[Cr]`, no bound, `L=1`).
- INFERRED: the competing text is M:504 "Recorded controls do not authorize recovery-time admission"; read as a release ban it gives the permanent hold above, so the sentence must say which is meant. With the sentence, P1 stands. Cheapest check: set mc:845 to `true` in `triage-work/fix/ptree` and rerun the 149 rows and the enumeration; the T1 and P1 classes should reach 0. NOT RUN.

**1. Q1: fault and revoke; the ADMIT is not recorded.**
- VERIFIED by reading, not run: mc:761-762 faults with no `discovery_revoke`; in the other order mc:807-808 refuses the TERM, so no discharge commits. The final coverage depends on the arrival order of the same two claims, which is B1's shape (review I4e) and S2's rule applied to a discharge.
- Sentence (D36, last paragraph): "…is contradictory provenance in either arrival order: the ADMIT is not recorded, and a committed discharge supported by that EXIT is revoked under Decision 43." Row: `TERM_D18, EXIT_D18@L, S(19), AD_D{e=19}` gives `EXIT_D18 → F18 → Vu`, x removed, END 18 then one O!, `G(open), P=1+0, L=1`.

**2. P2: not recorded.** Row: `D12@P0, ADc6, TRY_NW_D6` gives exactly one Vu, at ADc6. Repeat with `PTR(non-NULL)` last, and with `AB_D` last (accepted, no fault).
- VERIFIED: the numerical route already faults before recording (mc:759-768 against mc:837-844); G6 says "no accepted admission" (E:167); M:504 records only valid controls.
- INFERRED: pin the AB variant, because M:543 ("a refuted admission claim … cannot be converted into a successful discard") can be read the other way, and refusing AB recreates the permanent hold (`[FINAL_TRANS12,S19] fault=0` gives VOID only, VERIFIED).

**3. G1: no, product reading, no extra sentence.** VERIFIED: M:426 says the TRY "is not recorded, whatever history it names"; the model's `address_refuted` (model:242) is state kept from an unrecorded control. INFERRED safe: the TRY's own VOID latches, so nothing can commit afterwards.

**4. G2: the candidate is kept as evidence, in both orders.** Sentence (D43): "A recorded quiet-baseline candidate is retained across VOID and REARM like a recorded ADMIT; it can no longer commit, and the first observation contradicting it faults once and disposes of it." Rows: `B_D6, K, RA, D14@P0{earlier}` and `K, RA, B_D6, D14@P0{earlier}`, both ending in Vu at D14.
- INFERRED: this is the rule already used for ADMIT, ABANDON and terminal claims (M:426, M:460, M:533). UNDER-SUPPORTED: the triage switched the model, not the product. Nobody has run the product with `quiet_pending` kept across `fault()` (mc:259-263) against the 149 rows. Run that before writing the sentence.

**5. G3: one fault per refuted claim.** Sentence (D40): "A terminal claim is refuted once; later observations contradicting an already refuted claim report nothing." Row: `TERM_D19, D20@P0, EXIT_D18@L` gives exactly one Vu, at D20.
- VERIFIED (model:316, host:926-931): the other reading faults on every later TRANS of that lifetime, and the driver re-arms after each op. A live backend with one false TERM would void every re-armed epoch.

**6. G4: validated while held (product reading).** Sentence (section 3, after M:110): "A hold defers release and commitment only; validation of the eligible batch, including each prospective fresh-record bound against the batch's records, completes first, and an established contradiction faults at once." Row: `AD_D6, D12@P0{final}, D14@P0{earlier}, A20@P0{requires x}, S(40)` gives Vu at S(40), no DATA or FRONTIER, `L=1`.
- VERIFIED: M:109-110 puts validation before the hold. INFERRED: the verdict then does not depend on whether an unrelated TEXT has arrived, and it matches the core's other batch checks (the loop continues after `held = true`, mc:1063-1066). M:480 bars prospective bounds from closure, not from refusal.

**7(b) T2: product change.** Sentence (D43): "A revocation's revoked OPEN is persisted, and every public view reflects it, before the VOID of the same fault is published." Row: G2b with status and reader read inside the VOID callback: `closed=0, P=1+0`, INCOMPLETE.
- VERIFIED: VOID leaves at mc:276 inside `pgwt_merge_feed`; the OPEN is published after it returns (cap:262-266). review:128 (the reviewer's probe, not mine): inside the callback the persisted reader says COMPLETE.
- INFERRED: a kill between those two writes leaves a file that reads COMPLETE on a revoked proof. "Views undefined inside a sink" would hide that. Cheapest check: abort inside the VOID sink of probe E1 and run the reader on the bytes.

**Form: do not adopt the non-normative count.**
- VERIFIED: a further fault is not only a count. `fault()` suspends the epoch and discards the staged queue of every source (mc:247-268), and the driver has re-armed before each later op (host:926-931).
- VERIFIED: the proposed wording contradicts pinned text: M:504 "recorded without a fault caused by the latch", E:160-161 "without another fault", M:460 "repeats no already established contradiction report".
- INFERRED, lost with it: the G3 livelock, and any product that voids a healthy re-armed epoch on a valid control.
- Smaller rule that stays exact and decides items 2-5 in one paragraph: "A control that reports a fault is not recorded and leaves no state. Validly recorded claims survive VOID, REARM and the latch. Each recorded claim is refuted at most once, by the first observation contradicting it; that observation faults and the claim is inert afterwards. The latch removes commitments only."
- G4 and item 2 need pinned rows either way: G4 decides whether the first VOID happens at all, item 2 decides what evidence is retained.

**Order:** one delta first (the sentences and rows above), then one round 11 with B1, P1, S1, S2 and the delta. INFERRED: Q1, P2, 7(a), G2 and T2 change the same functions as B1, P1 and S2, so a split costs a second round on the same lines.
**Not checked:** the 149 rows and the full enumeration were not rerun; `answers-*`, `compare-*` and the model's release path were not read; no proposed product change was built.
