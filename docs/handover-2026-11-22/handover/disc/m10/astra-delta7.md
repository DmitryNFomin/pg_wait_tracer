SETTLEMENT

1. **Post-latch ADMIT and the accessor:** accept the challenger’s rule. `admitted` means a valid ADMIT was recorded, including during suspension or under the latch. It removes the missing-ADMIT hold; ordinary release can resume after explicit REARM, without new discovery certification. My proposed second admission fact would preserve the publication stall unless every release consumer distinguished it correctly. Commitment eligibility is already governed by the latch and committed-proof state.

2. **G1:** accept the challenger’s reading. The rejected late TRY supplies no retained attempt or NULL-pointer history. Perform its required fault and revocation, but do not reject subsequent otherwise valid controls solely because that TRY occurred. Existing restrictions on duplicate ADMIT and separately false controls remain.

3. **G3:** accept one refutation per recorded claim. Further observations contradicting that already refuted claim do not fault again solely for that contradiction. A separately delivered conflicting control retains its own fault consequence. My per-observation rule would repeatedly suspend rearmed epochs for one already established false terminal claim.

4. **T2:** accept publication ordering, including callback-visible state. Persist the revoked OPEN and reconcile public views before publishing the associated VOID. Restricting observations to input boundaries would conceal the persisted-file failure identified by the challenger.

5. **Form:** exact outputs remain exact; withdraw the omission allowance. Do not adopt “leaves no state” or “inert afterwards” literally: rejected controls can require revocation, J can establish independent identity facts, and historical admission must continue to bar abandonment. The delta distinguishes accepted claims from required rejection/refutation history.

Agreed: Q1 faults and revokes in either arrival order; an ADMIT contradicted when delivered is not recorded. An ADMIT recorded before the contradiction became known retains its historical admission fact but loses its contradicted proof. P2 records no admission. Quiet candidates survive VOID and REARM. Held batches undergo all validation decidable from available evidence. B1, P1, S1 and S2 remain required. Freeze this delta and independent answers before one implementation round.

CONTRACT EDITS

Apply to `contracts/merge-v1/CONTRACT.md` at `6f18da28784da73bae2dd97621fe311dab984f2c`.

1. SECTION 3. REPLACE:

> A dependency not yet available holds the batch and its frontier; a lost or contradictory dependency causes VOID.

WITH:

> A dependency not yet available holds the batch and its frontier; a lost or contradictory dependency causes VOID. A hold defers release and commitment only. Complete all eligible-batch validation decidable from available evidence, including reconciliation of prospective fresh-record bounds against the batch's records, before returning held; an established contradiction faults at once. Prospective bounds used for validation supply no committed proof or permission to close the startup gap.

2. DECISION 36. REPLACE:

> An ADMIT whose `e` is later than the timestamp of that lifetime's own bound EXIT is contradictory provenance.

WITH:

> An ADMIT whose `e` is later than the timestamp of that lifetime's independently bound EXIT is contradictory provenance in either arrival order. Reject an ADMIT contradicted on delivery without recording admission, route or baseline. If the ADMIT was validly recorded before the EXIT established the contradiction, retain the historical admission fact and refute its proof. The observation establishing the contradiction reports `FAULT(unknown_evidence,d,C)` and revokes any committed lower bound or terminal discharge supported by the contradicted proof under Decision 43. Equality of enable and EXIT timestamps is not this contradiction. These rules preserve ordinary arrival-fault precedence and Decision 43's two-fault rule.

3. DECISION 43. REPLACE:

> Recorded controls do not authorize recovery-time admission or restore discovery certification.

WITH:

> Recording a valid startup ADMIT under the latch removes the missing-ADMIT hold of Decision 36. That lifetime's records may then release under the ordinary validation, completeness, publication and explicit-REARM rules; without a previously committed valid numerical bound their incoming numerical intervals remain uncertified, and no new discovery bound, discharge or startup-gap END commits. This is completion of the recorded startup disposition, not a new recovery attachment. The public per-lifetime `admitted` value means that a valid ADMIT has been recorded at some point in this capture, including during suspension or under the latch; it remains true after later proof refutation. An ADMIT rejected before recording leaves it false. It does not promise a currently valid bound or certification eligibility. If `admission_recorded` is retained as a compatibility field, it exposes the same fact.

4. DECISION 43. APPEND AFTER the exact sentence:

> Ordinary release, fault reporting, abandonment consumption and explicit REARM retain their existing rules.

TEXT:

> A discovery control rejected for its own invalid facts or ordering records no accepted claim, admission, route, baseline or disposition. Rejection still performs required fault handling and revocation, and preserves independently established identities, numeric resolutions, census facts and the refutation history required by the duplicate rules. An independently observed counter fault does not reject an otherwise valid control. Validly recorded claims, including quiet-baseline candidates, survive VOID, REARM and the certification latch as evidence. Each recorded proof claim is refuted at most once: the first observation establishing its contradiction faults and performs any required revocation; later observations do not repeat that contradiction report or restore the proof. Retain historical admission and final-disposition facts after refutation. A refuted quiet candidate supplies no commitment readiness. These rules do not suppress separately delivered FAULT or LOSS reports, independent arrival or accounting faults, separately false controls, or the late-attempt reports expressly required by Decision 38. A rejected late attempt contributes no retained attempt or pointer history; its required revocation remains effective.

5. DECISION 40. REPLACE:

> An identical retransmission restores no proof and repeats no already established contradiction report; a separately delivered conflicting claim retains its own fault consequences.

WITH:

> A terminal claim is refuted once; subsequent observations contradicting that already refuted claim report nothing solely for that contradiction, including after REARM. Retain enough refutation history to enforce this rule and prevent restoration of the claim. An identical retransmission restores no proof and repeats no already established contradiction report; a separately delivered conflicting claim retains its own fault consequences.

6. DECISION 43. APPEND AFTER the exact sentence:

> Apply ordinary epoch invalidation additionally when the evidence disproves a published frontier.

TEXT:

> Before publishing a VOID associated with an established revocation, successfully persist its revoked startup OPEN and make every public discovery view reflect the revoked commitments, pending counts, startup-gap state, applicable census changes and certification latch. Read-only accessors and the persisted reader must agree inside that VOID's publication callback, not only after the primitive input returns. If one observation requires two faults, complete all revocations established by that observation before publishing its first VOID, while preserving the specified order of the two fault reports. A persisted prefix ending at entry to either VOID callback must already contain the revoked OPEN. Existing publication-failure rules remain applicable; an unsuccessful OPEN publication does not satisfy this requirement.

7. DECISION 45. REPLACE:

> A refuted admission claim or uncertain record attribution cannot be converted into a successful discard.

WITH:

> A previously validly recorded ADMIT bars abandonment throughout the capture, including when recorded under the latch or when its proof is later refuted. An ADMIT rejected before recording does not itself bar abandonment: acceptance still requires the independently established identity, installation, startup-gap and final-disposition facts of this decision. Rejected admission does not establish any of those facts, repair uncertain record attribution, cancel a fault or authorize consumption during suspension.

ROWS (inputs only)

In `oracle-discovery/ROWS.md`, APPEND AFTER the exact final row:

```text
| G8 | Census `{D,H}`. `EXIT_D18@L, S(19), AD_H8, A20@P0{requires x}, H21@P0{final}, S(22), TERM_D18`. TEXT x remains unavailable throughout. |
```

TEXT:

H rows use the G-row defaults unless explicitly overridden: installed, independently bound D; complete census `{D}` at fence 5; mode N; C0; detection time 20. Each row below is an independent execution. Source positions follow the existing rules.

`RA` in H rows always means `REARM(21,current-vector,fresh valid A/B attachment_proofs)`, including D's independently qualified continuing attachment when D has a valid recorded ADMIT. Subsequent observations use detection time 22. `K` retains its existing meaning and makes subsequent observations use Cr.

`BREAK_D` is a demonstrated pointer-observation continuity break, with no separate FAULT input. `WAIT_D6` is delivery of a historical wait-word attempt report for D, not execution of a new physical attachment bracket.

All controls explicitly claiming completed proof assert their required facts unless overridden. In H3d, before AB the attachment attempts have permanently ended without a valid final attachment; D's installed identity and record attribution remain independently established.

Observe commitments, `admitted`, pending split, census, latch, epoch suspension, persisted gap history, fault diagnostics and consumption after every primitive input. Additionally, in every H row containing a revoking fault, inspect the public views and persisted reader at entry to each VOID callback. H8 additionally retains the persisted prefix at that entry and evaluates it independently with no later publication.

| ID | Input sequence / starting-state override |
|---|---|
| H1a | `TERM_D18, EXIT_D18@L, S(19), AD_D19`. |
| H1b | `AD_D19, EXIT_D18@L, S(20)`. No TERM or confirmation is supplied. |
| H1c | `TERM_D18, EXIT_D18@L, S(19), AD_D18`. |
| H1d | `AD_D18, TERM_D18, EXIT_D18@L, S(19)`. |
| H2a | `K, RA, S(25), AD_D6, D26@P0{final}, S(27)`. |
| H2b | `K, AD_D6, RA, S(25), D26@P0{final}, S(27)`. |
| H2c | `K, AD_D6, D26@P0{final}, S(27)`. No REARM. |
| H2d | `K, RA, AD_D6, AB_D`. |
| H2e | `K, RA, ADc6, AB_D`. |
| H3a | `D12@P0{final}, ADc6, TRY_NW_D6`. |
| H3b | `D12@P0{final}, ADc6, PTR_D14(non-NULL)`. |
| H3c | `D12@P0{final}, ADc6, BREAK_D`. |
| H3d | `D12@P0{final}, ADc6, AB_D, RA, D26@P0{earlier}, S(27)`. |
| H4a | `AD_D6, TRY_NW_D6, RA, B_D6{e=6}, A26@P0, S(27)`. |
| H4b | `AD_D6, TRY_NW_D6, RA, B_D6{e=6}, TRY_NW_D6, S(27)`. |
| H5a | `B_D6{e=6,α2}, K, RA, D14@P0{earlier}`. |
| H5b | `K, RA, B_D6{e=6,α2}, D14@P0{earlier}`. |
| H6a | `TERM_D19, D20@P0, RA, EXIT_D18@L, S(25)`. TERM names the source position subsequently occupied by EXIT_D18, but claims terminal timestamp 19. |
| H6b | `D20@P0, TERM_D19, RA, EXIT_D18@L, S(25)`. TERM names the source position subsequently occupied by EXIT_D18, but claims terminal timestamp 19. |
| H6c | `TERM_D19, D20@P0, RA, TERM_D21`. Both terminal controls name the same prospective EXIT source position. No EXIT DATA is delivered. |
| H7a | `AD_D6, D12@P0{final}, D14@P0{earlier}, A20@P0{requires x}, S(40)`. TEXT x remains unavailable. |
| H7b | `AD_D6, A10@P1{requires x}, D12@P0{final}, D14@P0{earlier}, S(40)`. TEXT x remains unavailable. |
| H8a | `AD_D6, B_D6{e=6}, S(19), TRY_NW_D6`. No qualified non-NULL pointer fire is supplied. |
| H8b | `NW_D6, S(19), PTR_D12(non-NULL)`. |
| H9a | `PTR_D5(non-NULL), AD_D6, B_D6{e=6}, S(8), TRY_NW_D6`. |
| H9b | `PTR_D5(non-NULL), AD_D6, B_D6{e=6}, S(8), WAIT_D6`. |
| H10a | `TERM_D18, EXIT_D18@L{no cookie}, S(19), TERMU_D18{cookie=0; names exactly that EXIT; explicitly claims final unresolved disposition}`. TERM_D18 supplies the independently valid binding to installed D. |
| H10b | Override with the J-row numeric-observation defaults. `X_4, A6@P0, S(8), J_U4, TERMU_U4`. Both controls name exactly X_4; the final control explicitly claims final unresolved disposition despite the preceding J resolution. |

EXPECTED

In `oracle-discovery/EXPECTED.md`, REPLACE the exact anchor:

```text
**Required mutants**
```

WITH the following block, including its final heading:

H rows: `Vk=VOID20[Cr,counter_change]`; `Vu20=VOID20[C0,unknown_evidence]`; `Vu22[C]=VOID22[C,unknown_evidence]`. Frontiers use C0 unless marked Cr. Every execution begins with the ordinary startup OPEN.

Unless a row states otherwise, its final state is `b=—, x=—, G(open), P=1+0, census=known, latch=1`; `abandoned=0`, uncovered increment 0, no END, no revoked OPEN, no epoch invalidation and no nonzero exit. Listed output streams are complete. Each listed VOID increments only its named fault diagnostic; discovery faults add no storage-counter increment. `admitted` is false unless expressly stated.

For every listed revocation, remove the affected commitment, update public views and persist exactly one O! before the associated VOID callback begins. Historical END and DATA remain observable. The persisted reader rejects coverage dependent on the revoked proof, including `[50,60)` with otherwise valid supplied coverage.

| ID | Expected output and state |
|---|---|
| H1a | `EXIT_D18 → F18 → Vu20`. At S(19), `x_D=18, G(18), P=0+0, latch=0`; AD_D19 is rejected without recording admission. Remove x and persist O! before Vu20. Final default state; historical END 18 remains. |
| H1b | `Vu20` at EXIT arrival; EXIT is not published. `admitted=true` records the earlier accepted ADMIT, whose proof is now refuted. No bound or discharge ever commits; final default state otherwise. |
| H1c | `EXIT_D18 → F18`. `admitted=true, b=—, x_D=18, G(18), P=0+0, census=known, latch=0`; exactly one END 18; no fault or O!. |
| H1d | `EXIT_D18 → F18`. `admitted=true, b=—, x_D=18, G(18), P=0+0, census=known, latch=0`; exactly one END 18; no fault or O!. |
| H2a | `Vk → F24[Cr] → D26[U] → F26[Cr]`. `admitted=true` after AD; final default commitment/pending/gap state. The final epoch remains active. |
| H2b | `Vk → F24[Cr] → D26[U] → F26[Cr]`. `admitted=true` immediately after AD during suspension and after RA; final default commitment/pending/gap state. The final epoch remains active. |
| H2c | Only `Vk`. `admitted=true`; epoch remains suspended. D26 supplies no publication or consumption; no implicit REARM. |
| H2d | `Vk → Vu22[Cr]`, the second fault at AB. `admitted=true`; abandonment is rejected and records no disposition or consumption. |
| H2e | `Vk → Vu22[Cr]`, the second fault at AB. `admitted=true`; abandonment is rejected and records no disposition or consumption. No route-(c) bound commits. |
| H3a | Only `Vu20`, at ADc6. No admission, route-(c) baseline or permanence claim is recorded. The subsequent TRY is not a late-ADMIT violation and produces no additional fault. |
| H3b | Only `Vu20`, at ADc6. The pointer observation produces no additional fault from the rejected admission; census remains known. |
| H3c | Only `Vu20`, at ADc6. The continuity observation produces no additional fault from the rejected admission; census remains known. |
| H3d | `Vu20 → F26`. ADc6 is not recorded; AB is accepted during suspension without another fault. D12 was dropped by ordinary fault handling and is not counted. After RA, D26 is consumed without publication; `abandoned=1`. Final epoch active; no bound, discharge or END. |
| H4a | `Vu20 → A26[T] → F26`. `admitted=true`; TRY is not recorded. B6 is recorded without another fault, supplies no commitment, and does not hold ordinary A release. Final epoch active. |
| H4b | `Vu20 → Vu22[C0]`. `admitted=true`; B6 itself adds no fault. The second late TRY independently violates Decision 38 and faults; neither TRY is recorded. No DATA or frontier. |
| H5a | `Vk → Vu22[Cr]`, the second fault at D14 arrival. The pre-K quiet candidate survives K and RA as evidence, is refuted once and supplies no readiness or commitment. No O!, because it never committed. |
| H5b | `Vk → Vu22[Cr]`, the second fault at D14 arrival. The post-K quiet candidate is retained and refuted with exactly the same consequences as H5a. |
| H6a | `Vu20 → EXIT_D18 → F24`. The first fault occurs at D20 and refutes the recorded terminal claim. EXIT18 supplies no repeated contradiction report and no discharge; final epoch active. |
| H6b | `Vu20 → EXIT_D18 → F24`. The first fault occurs at TERM_D19, which retained D20 already contradicts; no accepted terminal disposition or discharge results. EXIT18 adds no discovery fault; final epoch active. |
| H6c | `Vu20 → Vu22[C0]`. D20 refutes the first terminal claim. The separately delivered conflicting TERM_D21 faults independently; it neither replaces nor restores the first claim. |
| H7a | Only `Vu20`, during S(40)'s eligible-batch validation. The proposed fresh bound 12 conflicts with earlier-attempt D14 despite the unavailable TEXT dependency. `admitted=true`; no DATA, TEXT, frontier or committed bound. |
| H7b | Only `Vu20`, during S(40)'s eligible-batch validation. Encountering the unavailable A10 dependency earlier in output order does not stop validation of D12 and D14. `admitted=true`; no DATA, TEXT, frontier or committed bound. |
| H8a | `F18 → Vu20`. Before TRY: `admitted=true, b_D=6, G(6), P=0+0, census=known, latch=0`, one END 6. Before entry to the VOID callback: b removed, `closed=false, P=1+0, census=known, latch=1`, O! persisted and dependent cached answers invalidated. Both the callback's reader and the independently reopened prefix report INCOMPLETE. Final default state with `admitted=true`. |
| H8b | `F18 → Vu20`. Before PTR: `admitted=true, b_D=6, G(6), P=0+0, census=known, latch=0`, one END 6. Before entry to the VOID callback: b removed, `closed=false, P=1+0, census=unknown, latch=1`, O! persisted and dependent cached answers invalidated. Both the callback's reader and the independently reopened prefix report INCOMPLETE. Final `b=—, x=—, G(open), P=1+0, census=unknown, latch=1, admitted=true`. |
| H9a | `F7 → Vu20`. Retain `admitted=true, b_D=6, x=—, G(6), P=0+0, census=known, latch=1`; exactly one END 6 and no O!. The historical late TRY faults but the retained qualified non-NULL fire prevents address-proof revocation. |
| H9b | `F7 → Vu20`. Retain `admitted=true, b_D=6, x=—, G(6), P=0+0, census=known, latch=1`; exactly one END 6 and no O!. The historical late wait-attempt report faults but the retained qualified non-NULL fire prevents address-proof revocation. |
| H10a | `EXIT_D18 → F18 → Vu20`. At S(19), `x_D=18, G(18), P=0+0`. Reject the cookie-zero unresolved control, retain the historical MATCHED disposition, remove x and persist O! before Vu20. Final default state; historical END 18 remains; uncovered increment 0. |
| H10b | No output before J; then `EXIT_U4 → A6[T] → F7 → Vu20`. J establishes U, resolves the number and commits `x_U=4, G(5), P=0+0, census=known`. Reject TERMU, retain the historical MATCHED disposition, revoke x and persist O! before Vu20. Final `b_U=—, x_U=—, G(open), P=1+0, census=known, latch=1`; historical END 5 remains; uncovered increment 0. |

Existing rows whose ordering expectations are strengthened:

- **N5 and P2:** the pointer/continuity revocation's O! and census change precede VOID publication.
- **R3:** every DATA, pointer and continuity variant, for every h and q, exposes its completed revocation before the contradiction's VOID. Preserve each variant's existing census result.
- **R4:** every h/q variant persists its bound-revocation O! before the contradiction's VOID.
- **R5:** every h/p/q variant persists its discharge-revocation O! before the contradiction's VOID.
- **R12:** supporting-epoch invalidation and discharge revocation are reflected, with O! persisted, before its seal-violation VOID.
- **G2b:** numerical-bound revocation precedes the late-TRY VOID.
- **G3, revocation variant only:** bound revocation precedes the appended VOID; retained terminal evidence still supplies no replacement discharge.
- **G4, both variants:** discharge revocation precedes the conflicting-control VOID.
- **G4b:** discharge revocation precedes the unresolved-control VOID.

Their merge-only output streams, exact fault counts, historical END values, final states and consumption counts are unchanged. No existing row receives an optional fault count or a changed final answer. Existing two-fault observations preserve their arrival-fault-first diagnostic order.

**Required mutants**

MUTANTS

In `oracle-discovery/EXPECTED.md`, APPEND AFTER the exact existing mutant-table row:

```text
| Prevent a valid late terminal discharge merely because another batch is held | G8 |
```

TEXT:

| Mutation | Row(s) that must turn red |
|---|---|
| Fault late ADMIT but retain its contradicted committed discharge | H1a |
| Record an ADMIT already contradicted by retained EXIT evidence | H1a |
| Check enable-versus-EXIT provenance only when ADMIT arrives last | H1b |
| Erase historical admission when later EXIT refutes its proof | H1b |
| Treat enable/EXIT timestamp equality as contradictory | H1c, H1d |
| Expose certification eligibility as `admitted`, or leave the missing-ADMIT hold after valid post-latch admission | H2a, H2b |
| Record valid admission only in an active epoch | H2b |
| Let recorded admission implicitly rearm or release during suspension | H2c |
| Accept abandonment after numerical or route-(c) admission recorded under the latch | H2d, H2e |
| Commit a new bound or mark D26 certified after post-latch admission | H2a, H2b |
| Record route-(c) admission before checking retained numerical evidence | H3a, H3b, H3c |
| Treat rejected admission as barring otherwise valid abandonment | H3d |
| Count fault-dropped D12 as an abandonment discard | H3d |
| Retain the rejected late TRY as NULL-pointer history and fault the subsequent otherwise valid B | H4a |
| Suppress a separately delivered repeat late-attempt report after the first fault | H4b |
| Drop quiet evidence on VOID or REARM | H5a |
| Refuse or forget valid quiet evidence recorded under the latch | H5b |
| Re-fault a rearmed epoch solely because EXIT contradicts an already refuted terminal claim | H6a |
| Record a terminal claim already contradicted on delivery as an accepted proof | H6b |
| Suppress a separately delivered conflicting terminal control because an earlier claim was refuted | H6c |
| Return held before validating the batch's prospective-bound contradiction | H7a, H7b |
| Stop validation upon encountering the first missing dependency in output order | H7b |
| Publish a prospective bound, DATA or frontier from the contradictory held batch | H7a, H7b |
| Persist revoked OPEN only after the VOID callback returns | H8a, H8b |
| Update status only after VOID, or leave pointer-driven census invalidation until input return | H8a, H8b |
| Repair only live views while the persisted prefix retains the unretracted END | H8a, H8b |
| Revoke a numerical bound on late TRY despite a retained qualified non-NULL fire | H9a |
| Revoke a numerical bound on late wait-attempt report despite a retained qualified non-NULL fire | H9b |
| Require the conflicting unresolved control's cookie to equal the committed discharge's cookie before revoking | H10a, H10b |
| Replace MATCHED with the rejected unresolved disposition or count another uncovered consumption | H10a, H10b |

TASK

Give each independent answerer only the amended contract, amended `ROWS.md` and this TASK. Do not supply this settlement, `EXPECTED.md`, mutants, implementation, review, triage, previous answer sets or product output.

Derive every H-row answer independently. Each suffixed ID is a separate execution. Expand S into its primitive boundary inputs and evaluate after every primitive input.

For each execution report:

- The complete ordered DATA/TEXT/FRONTIER/VOID stream, including detection times, vectors and certification markings.
- The input establishing each fault and its exact diagnostic increment.
- Historical and current admission, committed bounds/discharges, pending split, census, latch and epoch state.
- Startup OPEN/END/revoked-OPEN history, including its order relative to VOID callbacks.
- Abandonment and uncovered-consumption counts.
- Any epoch invalidation or nonzero exit.
- For H8, public-view state and the persisted-reader result at VOID-callback entry, plus the reader result from that saved prefix without subsequent publications.

For the existing revocation rows named in the delta, independently derive the strengthened publication ordering and check whether their previously specified merge streams or final outcomes must change. Identify any remaining ambiguity rather than consulting code or choosing the product's result. Freeze and compare the two answer sets before implementation.

NOT DECIDED HERE

- Producer/kernel qualification, physical attachment recovery, or removal of the permanent certification latch.
- A new persistence format, crash-atomic storage protocol, or replacement for existing writer-failure handling.
- The internal representation of admission and refutation history, beyond the stated accessor promises.
- Implementation correctness or test status: this is the contract settlement and delta, not execution evidence.