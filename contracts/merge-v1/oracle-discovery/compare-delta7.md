# Comparison of the independent answer sets with EXPECTED: merge-v1 delta 7

## Where this delta came from (2026-11-21)

The code review of the tenth implementation round and a second exhaustive enumeration (1,726,074 short schedules) left eight open causes: two product defects, two defects of the test's reading of the public views, and four places where the contract did not decide. While settling them the challenger measured two further defects on the built code: a valid ADMIT recorded under the certification latch never released its lifetime's records, so publication stopped for the rest of the capture; and a process killed between a revoking fault's VOID and its revoked OPEN left a file that read complete on a revoked proof. Delta 7 decides all of them. Both design advisers agreed on the direction; where they differed the design author accepted the challenger's reading (one admission fact, no retained history from a rejected control, one refutation per recorded claim, the revoked OPEN persisted before its VOID, exact outputs kept exact).

## First packet: delta 7 as first written (28 executions)

# H rows (H1a..H10b): set 1 (sol) vs set 2 (opus) vs EXPECTED

Sources: D/answers-sol.md (set 1), D/opus/answers-opus.md (set 2), W/oracle-discovery/EXPECTED.md (H block), ROWS.md notes, CONTRACT.md quoted only for notation and citation.

## 0. How I normalised

- END is a persisted gap-history record, not part of the DATA/TEXT/FRONTIER/VOID stream. EXPECTED lists the stream without END and states END separately; set 2 writes `END(x)` and `ROPEN` inline in the stream; set 1 gives them in a separate "persistence order". I removed END/ROPEN/OPEN from every stream and compared them as gap history (OPEN, END, revoked OPEN = O!, and order relative to VOID).
- `F(n,C)`, `V(d,C,r)` (set 1), `Fn[C]`, `VOIDd[C,r]` (set 2), `Vu20`, `Vk`, `Vu22[C]` (EXPECTED) are the same objects.
- Pending `P=a+b` (EXPECTED) = `a=...+...` (set 1) = `L/Q` counts (set 2). `G(n)` = startup gap closed at n; `G(open)` = open or reopened. "Gap reopened from zero" = `closed=false`.
- EXPECTED "reader INCOMPLETE" = set 1 "Unavailable ... withhold numerical answers" = set 2 "unavailable / gap open from boot_start". I assumed these are the same reader outcome; none of the three defines the word.
- Certified mark: EXPECTED and set 1 write `[T]`/`[U]`; set 2 defines only `[U]` and leaves certified records unmarked (`A26`, `A6`). I treated an unmarked A record in set 2 as `[T]` but flag it where it matters (H4a, H10b).
- Where set 2 lists two readings it labels both and does not explicitly choose in H2a, H3c, H5a/H5b, H6c; for A1 it prints R-drop first (H3d, H6a, H6b). I record those as "open" rather than guessing.
- "Default final state" (EXPECTED) = `b=—, x=—, G(open), P=1+0, census=known, latch=1`, abandoned 0, uncovered 0, no END, no O!, no epoch invalidation.
- The 28 executions: H1a-d (4), H2a-e (5), H3a-d (4), H4a-b (2), H5a-b (2), H6a-c (3), H7a-b (2), H8a-b (2), H9a-b (2), H10a-b (2).

## 1. Table

| Exec | Set 1 vs EXPECTED | Set 2 vs EXPECTED | Set 1 vs set 2 | Note |
|---|---|---|---|---|
| H1a | PARTIAL(stream agrees; x revoked, O!, pending 1=1+0, final default differ) | PARTIAL(same) | AGREE | Both keep x_D=18, pending 0=0+0, no O!; A7 |
| H1b | AGREE | AGREE | AGREE | Vu20 at EXIT arrival, admitted=true, nothing committed |
| H1c | AGREE | AGREE | AGREE | |
| H1d | AGREE | AGREE | AGREE | |
| H2a | AGREE | PARTIAL(stream: R2-i agrees; R2-ii is `VOID22[Cr,unknown_evidence]` with no D26/F26; not chosen) | PARTIAL | A2 |
| H2b | AGREE | AGREE | AGREE | |
| H2c | AGREE | AGREE | AGREE | |
| H2d | AGREE | AGREE | AGREE | |
| H2e | AGREE | AGREE | AGREE | |
| H3a | AGREE | AGREE | AGREE | whether TRY is retained: set 1 silent; no observable difference |
| H3b | AGREE | AGREE | AGREE | |
| H3c | AGREE | PARTIAL(stream: R6-i agrees; R6-ii adds a second `VOID20[C0,unknown_evidence]`; not chosen) | PARTIAL | A6 |
| H3d | AGREE | PARTIAL(abandoned count: R-drop 1 agrees; R-keep 2; not chosen) | PARTIAL | A1; stream `Vu20 -> F26` in both readings |
| H4a | AGREE | AGREE (A26 mark not stated) | AGREE | A9 |
| H4b | AGREE | AGREE | AGREE | unknown_evidence total 2 |
| H5a | AGREE | PARTIAL(stream: R8-i agrees; R8-ii only `Vk`; not chosen) | PARTIAL | A8 |
| H5b | AGREE | PARTIAL(same) | PARTIAL | A8 |
| H6a | AGREE | PARTIAL(stream agrees under R-drop; R-keep output undecided) | PARTIAL | A1, A12 |
| H6b | AGREE | PARTIAL(same) | PARTIAL | A1, A12 |
| H6c | PARTIAL(stream: 1 or 2 VOIDs; EXPECTED has `Vu22`) | PARTIAL(same two readings) | AGREE | Both leave open what EXPECTED decides; A5 |
| H7a | AGREE | AGREE | AGREE | |
| H7b | AGREE | AGREE | AGREE | |
| H8a | AGREE | AGREE (cached-answer invalidation not mentioned) | AGREE | views at callback entry and reader agree |
| H8b | AGREE | AGREE | AGREE | census unknown at entry |
| H9a | AGREE | AGREE | AGREE | |
| H9b | AGREE | AGREE | AGREE | |
| H10a | AGREE | AGREE | AGREE | |
| H10b | AGREE | AGREE (A6 mark not stated) | AGREE | |

## 2. Details for every DIFFERS / PARTIAL

### H1a (both sets PARTIAL vs EXPECTED; sets agree)

Observable: committed discharge x_D=18, pending split, gap history (revoked OPEN), final state.

- EXPECTED: "At S(19), `x_D=18, G(18), P=0+0, latch=0`; AD_D19 is rejected without recording admission. Remove x and persist O! before Vu20. Final default state; historical END 18 remains." Final default = `x=—, G(open), P=1+0`.
- Set 1: "The independently supported terminal discharge remains valid: the rejected ADMIT supplies no proof supporting that discharge and does not refute its independent terminal binding. There is no revoked OPEN." Final: `x_D=18`, pending `0=0+0`, gap `[0,18)` closed.
- Set 2: "The revocation clause covers only proofs 'supported by the contradicted proof'. The discharge rests on TERM+EXIT, so nothing is revoked (see A7)." Final: discharge x=18 stands, pending 0; gap history OPEN0, END(18), VOID, no ROPEN.
- Stream in all three: `EXIT_D18 -> F18 -> VOID20[C0,unknown_evidence]`. admitted=false, latch on, e0 suspended in both sets.
- Sentences. Both sets: Decision 36 "Reject an ADMIT contradicted on delivery without recording admission, route or baseline" and "revokes any committed lower bound or terminal discharge supported by the contradicted proof under Decision 43". EXPECTED cites nothing in the file; the opposite reading (set 2's A7 names it) is Decision 43: "independently attributable evidence contradicting a committed discovery lower bound or terminal discharge revokes the affected proof ... persists a revoked startup OPEN and returns the lifetime to pending".

### H2a (set 2 PARTIAL)

Observable: stream after F24.

- EXPECTED: `Vk -> F24[Cr] -> D26[U] -> F26[Cr]`; admitted=true after AD; final epoch active.
- Set 1: same stream; "AD_D6 is validly recorded under the latch. admitted=true; no bound commits"; D26[U] and F26 publish at L(27,2); e1 active.
- Set 2: R2-i `VOID20[Cr,counter_change] -> F24[Cr] -> D26[U] -> F26[Cr]`; R2-ii `VOID20[Cr,counter_change] -> F24[Cr] -> VOID22[Cr,unknown_evidence]` (no D26, no F26); R2-iii weak, disfavoured. Its final state lists admitted=true, no bound, pending L1, latch set, gap OPEN0, and does not name the epoch state.
- Sentences. Set 1 / R2-i: Decision 43 "Recording a valid startup ADMIT under the latch removes the missing-ADMIT hold ... That lifetime's records may then release under the ordinary validation, completeness, publication and explicit-REARM rules". Set 2 R2-ii: section 4 fresh attachment proofs on recovery plus the ROWS note "RA ... including D's independently qualified continuing attachment when D has a valid recorded ADMIT" (RA precedes AD in H2a). R2-iii: Decision 43 "Discovery does not authorize recovery-time admission".

### H3c (set 2 PARTIAL)

Observable: a second VOID20 for BREAK_D.

- EXPECTED: only Vu20; "The continuity observation produces no additional fault from the rejected admission; census remains known."
- Set 1: `V(20,C0,unknown_evidence)` only; "`BREAK_D` cannot revoke an accepted route-(c) proof: the rejected ADMIT recorded none ... It produces no additional discovery-refutation report."
- Set 2: R6-i `VOID20` only; R6-ii `VOID20 -> VOID20` (second unknown_evidence while suspended, no revocation, no census change). Not chosen.
- Sentences. R6-i: Decision 43 "A demonstrated break in its required continuous observation or ownership proof has the same consequence", read as applying "after an accepted Decision 37(c) bound". R6-ii: Decision 39 "A demonstrated observation-continuity violation follows the existing fault rules" and section 4 "attachment interruption ... remains a counted coverage fault, delivered as FAULT(unknown_evidence)" (both as quoted by set 2; I did not re-read Decision 39). Set 1 cites no sentence.

### H3d (set 2 PARTIAL)

Observable: `discovery_abandoned_records`.

- EXPECTED: "D12 was dropped by ordinary fault handling and is not counted. After RA, D26 is consumed without publication; `abandoned=1`." Stream `Vu20 -> F26`.
- Set 1: `V(20) -> F(26)`; "`discovery_abandoned_records` becomes 1 ... D12 contributes no discard count".
- Set 2: R-drop `VOID20 -> F26[C0]`, abandoned=1; R-keep same stream but D12 also consumed, abandoned=2. Not chosen.
- Sentences. R-drop: Decision 43 "before dropping its payload during suspension"; Decision 40 "While suspended, receipt, staging or payload disposal does not count as uncovered consumption". R-keep: Decision 40 "any subsequent consumption requires an independently valid REARM and satisfaction of the ordinary release requirements".

### H5a and H5b (set 2 PARTIAL)

Observable: whether D14 faults on arrival.

- EXPECTED: `Vk -> Vu22[Cr]`, "the second fault at D14 arrival"; no O!.
- Set 1: `V(20,Cr,counter_change) -> V(22,Cr,unknown_evidence)`; "D14@P0 ... refutes the retained quiet claim: unknown_evidence +1; suspend e1".
- Set 2: R8-i `VOID20[Cr,counter_change] -> VOID22[Cr,unknown_evidence]`; R8-ii (contradiction only at reconciliation or release; D14 never eligible, no boundaries) `VOID20[Cr,counter_change]` only. Not chosen. Same for H5b.
- Sentences. R8-i: Decision 42 "contradicts it and reports FAULT(unknown_evidence,d,C)" and Decision 43 "the first observation establishing its contradiction faults". R8-ii: set 2 cites only A8's hold/release timing; no sentence quoted.

### H6a and H6b (set 2 PARTIAL)

Observable: output of S(25) when a staged D20 survives REARM.

- EXPECTED: `Vu20 -> EXIT_D18 -> F24`; first fault at D20 (H6a) or TERM_D19 (H6b); EXIT18 adds no fault.
- Set 1: same, with no discussion of D20 after VOID.
- Set 2 R-drop: `VOID20 -> EXIT_D18 -> F24[C0]`. R-keep: D20 still held for missing ADMIT; either `EXIT_D18 -> F19`, whole batch held, or `VOID22` for a missing e1 attachment proof; undecided.
- Sentences. R-drop: Decision 43 "before dropping its payload during suspension". R-keep: Decision 36 "those records and their release frontier wait for matching ADMIT or accepted ABANDON_DISCOVERY" and section 3 "holds the batch and its frontier" (as quoted by set 2).

### H6c (both sets PARTIAL; sets agree)

Observable: whether TERM_D21 produces `VOID22`.

- EXPECTED: `Vu20 -> Vu22[C0]`; "The separately delivered conflicting TERM_D21 faults independently; it neither replaces nor restores the first claim."
- Set 1: conflicting-final-claim reading gives `V(22,C0,unknown_evidence)`, total 2; committed-discharge-condition reading records TERM21 with no fault, total 1. "The last control is not fully decided by the contract." Neither chosen.
- Set 2: R5-i `VOID20 -> VOID22[C0,unknown_evidence]`; R5-ii only `VOID20`. Neither chosen.
- Sentences. Faulting reading (both): Decision 40 "a separately delivered conflicting claim retains its own fault consequences"; set 2 adds "conflicting disposition or identity claims for the same EXIT report FAULT(unknown_evidence,d,C)". Non-faulting reading (both): Decision 40 "A matched terminal claim that differs, in terminal time or in the EXIT it names, from the claim supporting a committed discharge ... faults and the discharge is revoked" requires a committed discharge, and none exists.

## 3. Set 2's ambiguities A1-A12

| A | Rows | Set 1 assumed | EXPECTED assumes | Same outcome? |
|---|---|---|---|---|
| A1 staged/suspended records across VOID and REARM | H3d, H6a, H6b | silently R-drop (D12 "disposes of the tentative payload"; D20 unmentioned after VOID) | R-drop (H3d "D12 was dropped by ordinary fault handling"; H6 streams) | Set 1 = EXPECTED; set 2 gives R-keep alternatives (count 2; H6 output undecided) |
| A2 ADMIT after RA | H2a | silently R2-i | R2-i | Set 1 = EXPECTED; set 2 unchosen R2-ii differs |
| A3 END vs FRONTIER order | H1a,H1c,H1d,H8a,H8b,H9a,H9b,H10a,H10b | not stated in streams; only OPEN->END->ROPEN->VOID persistence order | END is a separate gap publication; stream omits it | Yes after normalisation; relative order not compared |
| A4 epoch suspension at VOID-callback entry | H8a,H8b,H10a,H10b | explicit only in H8b ("does not separately specify"); final e0 suspended | silent | Not compared; no one asserts a value |
| A5 conflicting claim, no committed discharge | H6c | explicit, both readings, none chosen | faults (`Vu22`) | Sets agree; neither commits to EXPECTED |
| A6 BREAK_D with no committed route-(c) proof | H3c | silently no fault | no fault | Set 1 = EXPECTED; set 2 open |
| A7 rejected ADMIT revoking a committed discharge | H1a | explicit: no revocation | revokes x, O! | No; both sets differ from EXPECTED |
| A8 which observation establishes a contradiction | H1b,H5a,H5b,H7a,H7b | arrival for H1b and H5; batch validation for H7 (silently) | same | H1b, H7a, H7b identical in all three; H5a/H5b open in set 2 |
| A9 B_D6 after refuted admission proof | H4a,H4b | explicit: recorded, no readiness, no repeated report | same | Yes |
| A10 TRY after rejected ADc6 | H3a | no fault; retention not stated | no fault | Yes |
| A11 PTR at 12 below F18, epoch invalidation | H8b | explicit: no epoch invalidation | default: none | Yes |
| A12 EXIT after refuted terminal claim | H6a,H6b | explicit: no repeat report | no fault | Yes |

## 4. Counts (28 executions)

- All three agree on everything, with no unchosen alternative: 19 (H1b, H1c, H1d, H2b, H2c, H2d, H2e, H3a, H3b, H4a, H4b, H7a, H7b, H8a, H8b, H9a, H9b, H10a, H10b).
- Stream agrees in every statement made, another observable differs or is open: 2 (H1a: both sets differ from EXPECTED on x, pending, O!; H3d: set 2 offers an unchosen abandoned count of 2).
- Stream differs or is left open: 7 (H2a, H3c, H5a, H5b, H6a, H6b: set 2 offers an unchosen alternative stream, set 1 = EXPECTED; H6c: both sets leave open what EXPECTED decides).
- 19 + 2 + 7 = 28. No row has two definite, contradictory streams between set 1 and set 2. The only definite disagreement with EXPECTED is H1a (both sets, same answer).

## 5. Not addressed, and existing rows

- EXPECTED H8a/H8b "dependent cached answers invalidated": set 1 says it in H8a ("Revocation invalidates affected cached answers"), not in H8b; set 2 does not mention it in either.
- EXPECTED H1b "EXIT is not published": set 1 states EXIT publication counts zero; set 2 says "no release and no FRONTIER" without naming the EXIT. Equivalent.
- EXPECTED H4a "B6 ... does not hold ordinary A release": both sets publish A26, so equivalent; neither names the hold clause.
- Observables the sets state that EXPECTED does not: epoch suspension at VOID-callback entry (A4); persisted-prefix contents (set 2 lists `OPEN0, END, F18, ROPEN`).
- Existing rows (EXPECTED strengthens N5, P2, R3, R4, R5, R12, G2b, G3 revocation variant, G4 both variants, G4b):
  - Set 1 names N5, P2, R3 pointer/continuity variants, R4, R5 "committed-discharge variants", R12, G2b, G3 revocation variant, G4, G4b. It narrows R5 to committed-discharge variants where EXPECTED says "every h/p/q variant". It also lists R1, R6, O1 (no O!), D10/J20 (no merge VOID), R7 counter fault (no reopening); these are consistent with EXPECTED lines for R1, R6, O1, R7.
  - Set 2 names G2b, G3 revocation variant, G4 variants, G4b, P2, R3 committed variants, N5, and "R10's second variant". It does not name R4, R5, R12 (it says it did not see the earlier rows). EXPECTED R10 reads "no O!" and does not list R10 among changed rows, so set 2's R10 mention would change that expected output (add an O! before the VOID).
  - Both sets say DATA/FRONTIER/VOID counts and final outcomes are unchanged; only the position of the revoked OPEN relative to the VOID changes.

## Settlements 7b and 7c

The design author kept every expected output of the first packet and sharpened the sentences the readers could not derive (H1a: a late-dated ADMIT rejected on delivery also revokes the EXIT-backed discharge it disputes; H6c: a second conflicting terminal control faults before any commitment; when a contradiction is established; END before the advancing FRONTIER within one release attempt; what survives a fault and a REARM). The challenger then checked the sharpened text against the shipped base merge and the existing rows: the rewritten section 4 sentence restates existing behaviour (corrected for an internal breach, where the status is exit-required); END-before-FRONTIER left one case uncovered, a release attempt that fails after its END was published (row V4), now decided: a plain startup OPEN is persisted before that failure's VOID; and a three-input schedule was undecided, now row H11a. Three probe rows outside the first packet were added (H11a, H11b, H11c). The re-derivation packet was cut to the executions whose deciding sentence changed plus one over-reach guard per changed sentence: 34 executions.

## Second packet: fresh answerers on the settled text (34 executions)

# Comparison: set 1 (answers-sol.md) vs set 2 (opus/answers-opus.md) vs EXPECTED.md, 34 executions

## Normalisation applied
- Set 1 `O0`/`R0`/`E<n>`/`F<n>[C]`/`V<d>[C,r]` = set 2 `OPEN0`/`rOPEN0`/`END(n)`/`F<n>[C]`/`VOID<d>[C,r]`; set 1 plain rollback `O0` = set 2 `rbOPEN0`; EXPECTED `OPEN`/`O!` = plain OPEN/revoked OPEN, `Vu20`/`Vk`/`Vu22[C]` = VOID forms. Set 1 `a/v` = set 2 `adm/proof`. Set 1 `T18+`/`Q6+` = set 2 "claim recorded", `-` = "refuted". Set 1 pending `L+Q` = set 2 `P=L+Q`.
- Both sets also list `S(w)` as four separate boundary primitives; compared as such.
- EXPECTED-SILENT convention: for every row other than H11a-c and V4, EXPECTED gives merge-only stream, END/O! presence, final state, counts; it gives no combined timeline order of plain OPEN, no per-primitive state, no claim statuses, no VOID-entry state. Where I compare the combined timelines of such rows, set 1 and set 2 agree with each other and are consistent with every EXPECTED statement (every execution begins with plain OPEN; END and O! placed as EXPECTED says). I do not repeat "silent on combined timeline" per row. Only observable-specific silences are listed in the table.
- Rule applied: an alternative offered without choosing = DISAGREEMENT (U2, U3 of set 2). Where set 2 offered two readings but said which it applied (U1, U4), counted as agreement with a note (U4 is the "inferred, not stated" case; U1 is "applied, other reading described"). If the caller prefers to count U1/U4 as disagreement, H3a, H3d and G6 would move (and H3b/H3c nominally, set 2 says same either way).

## 1. Table

| Execution | set 1 vs EXPECTED | set 2 vs EXPECTED | set 1 vs set 2 | note |
|---|---|---|---|---|
| H1a | AGREE | AGREE | AGREE | O0,EXIT,END18,F18,rOPEN,V20; x=18 then revoked; unknown_evidence +1 |
| H2a | AGREE | AGREE | AGREE | Vk,F24[Cr],D26[U],F26[Cr]; epoch 1 active; admitted=true |
| H3c | AGREE | AGREE | AGREE | Only V20; break gives no second fault; set 2 U1 note |
| H3d | AGREE | AGREE | AGREE | V20,F26; abandoned=1 (D26), D12 not counted; set 2 U1 note (applied same reading) |
| H5a | AGREE | AGREE | AGREE | Vk,Vu22[Cr]; Q6 refuted once at D14 |
| H5b | AGREE | AGREE | AGREE | same as H5a; second fault at D14 |
| H6a | AGREE | AGREE | AGREE | V20,EXIT_D18,F24; no repeated fault |
| H6b | AGREE | AGREE | AGREE | same stream as H6a; fault at TERM_D19 |
| H6c | AGREE | AGREE | AGREE | V20,V22; TERM_D21 faults independently |
| H1b | AGREE | AGREE | AGREE | Only V20 at EXIT arrival; admitted=true, proof refuted |
| H1c | AGREE | AGREE | AGREE | EXIT,END18,F18; no fault; equality allowed |
| G4 (v1: TERM_D19 same EXIT) | AGREE; EXPECTED-SILENT(T18 claim status) | AGREE; EXPECTED-SILENT(T18 claim status) | DIFFERS(recorded/refuted claim status of TERM_D18; set 2 undecided U3) | streams, O!, x revoked identical |
| R10 pointer fire | AGREE | AGREE | AGREE | V20,V20; no O!, census known |
| R10 continuity break | AGREE | AGREE | AGREE | same as pointer variant; first fault at break |
| H3b | AGREE | AGREE | AGREE | Only V20; PTR adds no fault; set 2 U1 note (same either way) |
| N5 | AGREE; EXPECTED-SILENT(CN6 status after PTR) | AGREE; EXPECTED-SILENT(CN6 status; set 2 also silent) | AGREE (set 2 silent on CN6-/v after fault) | F5,END6,F7,rOPEN,V20; census unknown |
| H7a | AGREE; EXPECTED-SILENT(admission-proof validity) | AGREE; EXPECTED-SILENT(admission-proof validity) | DIFFERS(admission-proof status after fault; set 2 undecided U2) | stream identical: only V20 |
| R2 (p=empty, no K, TERM,D20,EXIT,S25) | AGREE | AGREE | AGREE | Only V20; T18 refuted; no repeat report at EXIT |
| O1 | AGREE | AGREE | AGREE (set 2 silent on Q6 status at first VOID entry) | V20[clock_regression],V20[unknown_evidence]; counts 1/1 |
| N14 | AGREE | AGREE | AGREE | O0,V20[Cw,writer_push_fail]; END6 rejected, no rollback OPEN |
| G5 | AGREE | AGREE | AGREE | EXIT,F18,END18 (END during TERM) |
| G7 | AGREE | AGREE | AGREE | EXIT,END18,F18; later ADMIT/CONFIRM commit nothing |
| V4 | AGREE | AGREE | AGREE | O0,EXIT,END18,plain O0,V20[Cw],F19[Cw]; saved prefix reads open |
| A18 | AGREE | AGREE | AGREE | V20[allocation_fail]; abandoned 0 |
| U15 | AGREE | AGREE | AGREE | Only V20; pending 0+1, uncovered 0 |
| H2b | AGREE | AGREE | AGREE | same stream as H2a; ADMIT recorded under suspension |
| H4a | AGREE | AGREE (set 2 omits the [T] mark on A26; EXPECTED has A26[T]) | AGREE (same omission) | V20,A26,F26; TRY rejected; Q6 recorded, no hold |
| H3a | AGREE | AGREE | AGREE | Only V20; TRY retained, no fault; set 2 U1 note |
| G6 | AGREE | AGREE | AGREE | Only V20 at ADMIT; set 2 U4 note (inferred) |
| R5 (h=1,p=empty,q=empty) | AGREE | AGREE | AGREE | EXIT,F18,rOPEN,V20; no END; pending 2+0 after |
| R5 (h=1,p=AB_D,q=K) | AGREE | AGREE | AGREE | EXIT,F18,Vk,rOPEN,Vu; abandoned 0 |
| H11a | AGREE | AGREE | AGREE | OPEN,V20; TERM recorded as refuted, no fault |
| H11b | AGREE | AGREE | AGREE | OPEN,EXIT,END18,F18,rOPEN,V20; D20 no fault |
| H11c | AGREE | AGREE | AGREE | OPEN,Vk,F24[Cr],Vu22[Cr]; D26 not published |

## 2. DIFFERS in detail

### G4 (variant 1): recorded/refuted claim status of TERM_D18 after the conflicting TERM_D19
- Set 1: "Conflicting terminal time: reject T19, refute the discharge-supporting claim, revoke x and dependent closure ... The original final disposition is not replaced." (explicit: T18 refuted.)
- Set 2: "TERM_D19 rejected; TERM_D18 retained as final disposition (refuted-label: UNDECIDED U3)". UNDECIDED U3: "whether the retained TERM_D18 claim is marked refuted after the time conflict revokes its discharge, or stays unrefuted with its discharge revoked." Not chosen.
- Observable: recorded/refuted control claims after the last primitive (and at VOID entry).
- Sentences cited. Set 1 cites none by number. Set 2 cites D40 "Reject the conflicting control without replacing or restoring the retained claim" and D43 for revocation. CONTRACT.md line 460: "...reports FAULT(unknown_evidence,d,C), including when the retained claim is refuted and no discharge ever committed. Reject the conflicting control without replacing or restoring the retained claim; any committed discharge contradicted by the conflict is additionally revoked under Decision 43."
- EXPECTED G4: "remove x_D=18 ... final b=—,x=—, G(open), P=1+0, L=1" and the row note "Preserve discharge after a conflicting terminal timestamp or EXIT identity" mutant; silent on the claim label. Streams, O!, counts identical in both sets.

### H7a: admission-proof status after the eligible-batch contradiction
- Set 1: step 1 "Record a=true,v=valid"; at the fault "The ADMIT remains historically recorded; the prospective lower bound is rejected by reconciliation"; VOID entry "a=true". It never marks the proof refuted, so by its carry-forward rule proof validity stays valid.
- Set 2: "Admission-proof status after this: see UNDECIDED U2." U2: "the contract says the earlier-attempt record contradicts the prospective bound (D42). It does not say whether the recorded ADMIT's proof becomes refuted or stays valid. Neither reading changes any publication." Not chosen.
- Observable: admission-proof validity (state at VOID entry and final).
- Sentences cited: set 1 none; set 2 D42 (prospective fresh-record bound contradicted by earlier-attempt interval) and §3 "Complete all eligible-batch validation decidable from available evidence ... an established contradiction faults at once".
- EXPECTED H7a: "`admitted=true`; no DATA, TEXT, frontier or committed bound." silent on proof validity.

## 3. U1-U4

- U1 (set 2: H3a, H3b, H3c, H3d; reading: route-(c) ADMIT rejected before recording when contradicted by retained D12). Set 1 reached the same outcome (adm=false, v=absent, ADMIT rejected before recording, D12 fault-disposed) in all four, from the idea that D12 contradicts the permanent no-wait-word provenance; it gives no decision number. Sentence at issue (CONTRACT.md line 360, D36): "Reject an ADMIT contradicted on delivery without recording admission, route or baseline." and line 506 (D43) "An attributable TRANS/MARKER contradicts a permanent no-wait-word proof." Consequences set 2 described for the other reading: H3a TRY would fault again; H3d AB_D barred. EXPECTED: H3a "No admission, route-(c) baseline or permanence claim is recorded. The subsequent TRY ... no additional fault"; H3d "ADc6 is not recorded; AB is accepted during suspension". Outcome differs: no, set 1, set 2 (as applied) and EXPECTED all agree.
- U2 (set 2: H7a). Set 1 concluded: ADMIT stays recorded, nothing else; proof validity not marked refuted (valid by carry-forward), no sentence cited. EXPECTED silent. Outcome: set 1 vs set 2 differ on proof validity only because set 2 is undecided; publications agree.
- U3 (set 2: G4 v1). Set 1 concluded T18 refuted (explicit), from the conflict-with-retained-claim rule (line 460 above, no sentence cited by number). EXPECTED silent on the label. Outcome: differs between set 1 (refuted) and set 2 (undecided). Publications, O!, counts agree.
- U4 (set 2: G6; set 2 chose FAULT/ADMIT rejected, "inferred from D39, D42 and D43"; other reading: ADMIT recorded, no VOID, F18[C0] at S(19)). Set 1 concluded the same as set 2's chosen reading ("Required numerical address provenance is contradicted by retained history. Reject ADMIT before recording"), declaring its undecided list empty and citing no sentence. EXPECTED G6: "Vu at AD_D6; no accepted admission; final b=—,x=—, G(open), P=1+0, L=1; no DATA, FRONTIER, END or O!." Outcome: set 1, set 2 (chosen) and EXPECTED agree; only the "inferred" flag differs.

## 4. Counts (34 executions)
- All three agree on everything stated: 32 (every execution except G4 and H7a), with the table notes (set 2 omitting the A26 [T] mark in H4a; set 2 silent on CN6 status in N5 and on Q6 status at the first VOID entry in O1; U1/U4 flags).
- Stream agrees but another observable differs: 2 (G4: claim status; H7a: admission-proof status).
- Stream differs: 0.
- Counting convention note: of the 32 agreeing, 6 carry a recorded note (H4a, N5, O1 omissions; U1: H3a, H3d; U4: G6) and would turn into differences only under a stricter reading.

## Left as notes for a later delta

No publication, fault count, commitment or gap history depends on these in the 34 executions; both answer sets and EXPECTED reach the same outcomes.
- The internal label of a terminal claim whose discharge was revoked by a conflicting claim (G4), and of an admission proof after an earlier-attempt contradiction in a held batch (H7a): one reader labels, the other leaves it open, EXPECTED is silent.
- Two outcomes that one reader marked as inferred rather than stated, and reached identically: a route-(c) ADMIT contradicted on arrival is rejected and not recorded (H3a-H3d), and a numerical ADMIT after a retained TRY_NO_WAIT_WORD observation faults (G6).

Final count for delta 7: 31 new rows (H1a-H10b, H11a-H11c), 180 rows in all; V4's gap history made explicit.
