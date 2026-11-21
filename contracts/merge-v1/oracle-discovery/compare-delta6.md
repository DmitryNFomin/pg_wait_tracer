# Oracle6 comparison: answers-sol (A) vs answers-opus (B) vs EXPECTED, rows U1-U15

Counts: ALL-AGREE 13, ANSWERS-AGREE-EXPECTED-DIFFERS 1 (U15), ANSWERS-DIFFER 1 (U4; A matches EXPECTED), ANSWERS-AGREE-EXPECTED-INCOMPLETE 0, UNDETERMINED 0 (15 rows). Neither answerer wrote UNDETERMINED.

Outcomes compared per row: released records with [U]/[T] marks and order, FRONTIERs, VOID/fault with detection time, counter vector and reason, EXIT published / held / consumed uncovered and the uncovered count, P=L+Q, census known/unknown, startup-gap state and persisted end, latch, discharge (x_U).

Conventions reconciled (not differences):
- EXPECTED `latch=1/0` = A "certification latch=set/not set" = B "latch SET / not set".
- EXPECTED `G(open)` = A "startup gap=open" or "open permanently" = B "startup gap open (for the rest of the capture)". A and B both distinguish the capture-long closure prohibition (rows with an accepted unresolved disposition) from a merely open gap (U4, U5, U8, U9, U14); EXPECTED writes `G(open)` for both, which DECISION 41 ("absence of any final unresolved startup EXIT disposition ... capture-long closure prohibition") makes consistent.
- EXPECTED "uncovered +n" (capture-cumulative increment) = A/B "uncovered=n" for these independent rows.
- EXPECTED `G(5)` (U13) = A "closed with persisted P_close=5" = B "PERSISTED end P_close=5 (GAP_CLOSE published)". EXPECTED `historical G(13)` (U12) = A/B "closes with persisted end P_close=13", later reopened.
- Detection time: all VOIDs are `VOID20` in A, B and EXPECTED (PF's FAULT at 20; U3/U4/U5 likewise d=20 by the row's default observation time).

| ID | class | difference |
|---|---|---|
| U1 | ALL-AGREE | none. `VOID20[C0,unknown_evidence] → A26[T] → F27 → F29`; EXIT24 held through S(28), then consumed uncovered after TERMU_U24 (not published); uncovered 1; P=0+1; census unknown; G(open); latch 1; no discharge. |
| U2 | ALL-AGREE | none. No output before TERMU; `A6[T] → F7`; uncovered 1; P=0+1; census unknown; G(open); latch 0. |
| U3 | ALL-AGREE | none. `A6[T] → F7 → VOID20[C0,unknown_evidence]`; uncovered 1; P=1+0; census known; G(open); latch 1; J identity and numeric resolution retained; no discharge. |
| U4 | ANSWERS-DIFFER (A matches EXPECTED) | A and EXPECTED: `VOID20[C0,unknown_evidence]`, latch 1. B: no output, no FAULT, control simply not accepted, latch 0. All other outcomes agree (uncovered 0, P=0+1, census unknown, G(open)). B flagged this as an assumption. |
| U5 | ALL-AGREE | none. `VOID20[C0,unknown_evidence]`; uncovered 0; P=0+1; census unknown; G(open); latch 1; no PID-60 entry. B reaches the VOID via a flagged assumption (see below); A unflagged. |
| U6 | ALL-AGREE | none. `A6[T] → F7 → F9`; uncovered 1; P=0+0; census known; G(open); latch 0; no END, no discharge. |
| U7 | ALL-AGREE | none. `A6[T] → F7 → F9`; uncovered 1; P=0+0; census known (fence 9); G(open); latch 0; no END, no discharge. |
| U8 | ALL-AGREE | none. No DATA/frontier/fault; EXIT4 and A6 held and undecided; uncovered 0; P=0+1; census unknown; G(open); latch 0. |
| U9 | ALL-AGREE | none. Only `VOID20[C0,allocation_fail]`; after RA EXIT24 undecided, holds A26 and frontiers through S(30); uncovered 0; P=0+1; census unknown; G(open); latch 1. |
| U10 | ALL-AGREE | none. `A6[T] → F7`; disposition before DATA, EXIT4 not held, consumed uncovered at S(8); uncovered 1; P=0+1; census unknown; G(open); latch 0. |
| U11 | ALL-AGREE | none. `A6[T] → F7 → F9`; both duplicates idempotent (no output, fault or second count); uncovered 1; P=0+0; census known; G(open); latch 0. |
| U12 | ALL-AGREE | none. `D12[U] → D15[T] → F15 → F30 → A44[T] → F44 → F45`; historical G(13), one revoked OPEN at LATE_U; no output X_42..S(45); uncovered 2 total (LATE_U 1 + X_42 1); P=1+0; census unknown; G(open); latch 0; no VOID, no extra entry, no discharge, b_D=12 kept. B's counting choice (LATE_U = uncovered #1) is also A's and EXPECTED's. |
| U13 | ALL-AGREE | none. No output or disposition before J; `EXIT_U4 → A6[T] → F7`; uncovered 0; x_U=4; P=0+0; census known; G(5); latch 0. |
| U14 | ALL-AGREE | none. No DATA/frontier/disposition/fault; EXIT held; uncovered 0; P=0+1; census unknown; G(open); latch 0. |
| U15 | ANSWERS-AGREE-EXPECTED-DIFFERS | A and B: uncovered 0 (TERMU accepted, but EXIT24 never consumed because the epoch stays suspended). EXPECTED: "unresolved disposition is accepted and EXIT consumed once, but ordinary release remains suspended; uncovered +1". All other outcomes agree (only `VOID20[C0,unknown_evidence]`, no DATA/frontier, EXIT never published, P=0+1, census unknown, G(open), latch 1, no discharge). |

## Non-agreeing rows

### U4 (ANSWERS-DIFFER; A matches EXPECTED)

Inputs (ROWS): `CUT(5), TERMU_U4{names_exit=false}`. No EXIT DATA is delivered. ROWS also: "Unless overridden, controls explicitly submitted assert their required facts"; "Unless listed, no VOID, FAULT, UNTRACED or additional DATA is produced."

- A: `VOID20[C0,unknown_evidence]`; latch set; uncovered 0; P=0+1; census unknown; gap open (no capture-long prohibition). why: the control fails D40's naming requirement.
- B: no output; control "not accepted (no disposition recorded, nothing applied), no FAULT"; latch not set; uncovered 0; P=0+1; census unknown; gap open.
- EXPECTED: `VOID20[C0,unknown_evidence]`; "invalid unnamed disposition faults"; uncovered +0, `P=0+1, census=unknown, G(open), latch=1`.

Bearing text:
- DECISION 40: "the existing TERMINAL_UNRESOLVED control is accepted with cookie zero if and only if it explicitly names the cookie-less EXIT by capture, source, source position, PID and semantic timestamp, that PID has an unresolved numeric observation, and no inventory entry or registered lifetime has that PID." (states acceptance, not the consequence of non-acceptance)
- DECISION 40: "Finalisation requires established permanent loss ... or a completed reconciliation cut at or after the EXIT timestamp; ... A control falsely claiming these requirements reports FAULT(unknown_evidence,d,C)." ("these requirements" sits after the finalisation sentence)
- DECISION 40: "conflicting disposition or identity claims for the same EXIT report FAULT(unknown_evidence,d,C)." (needs a named EXIT; none here)
- DECISION 43: "A valid subsequent control is recorded without a fault caused by the latch; a false claim retains its fault consequences."
- §4 (A2 rule, unnumbered): "Terminal reconciliation without the matching kernel EXIT, contradictory evidence, or any non-admissible attachment failure reports FAULT(unknown_evidence,d,C)." (A2 context; whether it governs a numeric-only TERMU is not stated)
- Contrast DECISION 45, which has an explicit rule for its control: "An invalid abandonment reports FAULT(unknown_evidence,d,C)." DECISION 40 has no equivalent sentence for an invalid TERMINAL_UNRESOLVED.

Determination: ambiguous / silent. The contract states when the cookie-zero control is accepted but not what a non-accepted control that makes no false claim produces. Reading 1 (A, EXPECTED): a submitted unresolved disposition that fails the acceptance conditions is invalid evidence and faults. Reading 2 (B): a control that does not claim to name an EXIT asserts nothing false, the only fault sentences (false claim, conflicting claim) do not apply, and the row default "no VOID, FAULT ... unless listed" leaves it a silent refusal.
Clarifying sentence: "A cookie-zero TERMINAL_UNRESOLVED control that is not accepted under these conditions, including one that names no EXIT, is invalid and reports FAULT(unknown_evidence,d,C)" (or, for the other reading, "... is refused without output or state change").

### U15 (ANSWERS-AGREE-EXPECTED-DIFFERS)

Inputs (ROWS): `PF, X_24, A26@P0, S(28), TERMU_U24, S(30)`. No REARM.

- A: "EXIT24 and A26 are suppressed during suspension; TERMU_U24 is accepted and recorded, but no validated uncovered release consumption occurs. EXIT never published; uncovered=0."
- B: "X_24 is never published and never consumed through a release (the epoch stays suspended), so uncovered=0" (flagged ASSUMPTION, below).
- EXPECTED: "unresolved disposition is accepted and EXIT consumed once, but ordinary release remains suspended; uncovered +1".

Bearing text:
- DECISION 40: "On unresolved the EXIT is consumed as an uncovered record: counted once, never published, and producing no VOID by that disposition alone".
- DECISION 40: "The disposition removes only the undecided-terminal hold; applicable transport validation, producer completion, accounting, source completeness, publication and suspension rules remain in force."
- DECISION 40: "Acceptance is permitted after the Decision 43 latch and does not clear it or rearm the epoch. A disposition ... remains applicable to that named EXIT after subsequent numeric resolution or REARM."
- §4: "While suspended, suppress trusted DATA/TEXT callbacks and FRONTIERs; continue fault reporting."
- DECISION 41: "Consuming an EXIT under an unresolved disposition neither adds a pending lifetime nor removes its existing lifetime or numeric obligation; the uncovered-record count is separate from both pending counts."
- By analogy only: DECISION 45 "consumed under the ordinary release rules, counted once"; DECISION 42 "Merely received, rejected or VOIDed records do not count as validated consumption for the endpoint increment or abandonment diagnostic" (names the endpoint increment and abandonment diagnostic, not the uncovered count); DECISION 45 "trusted release remains suspended until an independently valid REARM".

Determination: ambiguous. "Consumed as an uncovered record" is not defined as either (a) disposition-time consumption, independent of epoch status (EXPECTED), or (b) consumption on the ordinary release path, which suspension suppresses until REARM (A, B). The "suspension rules remain in force" sentence supports (b) if consumption is a release event, and is compatible with (a) if only publication is suppressed. EXPECTED's own U1 (uncovered +1 after REARM) is consistent with both.
Clarifying sentence: "An EXIT under an accepted unresolved disposition that arrives or is staged while the epoch is suspended is counted once as uncovered when [the disposition is accepted or the EXIT arrives, whichever is later] / [it passes validated release consumption after an independently valid REARM]; a record dropped during suspension is [counted / not counted]."

## Assumptions flagged by an answerer

Set A flagged none (no "assumption" text in answers-sol.md).

Set B flagged three assumptions and one counting choice:

1. U4 (what a refused cookie-zero control that names no EXIT produces). B: "the contract states no consequence for a non-accepted cookie-zero control that falsely claims nothing; I take it as a refusal without FAULT". This changes output: B has no VOID and latch 0, against A and EXPECTED (VOID20, latch 1). The contract does not determine it; see U4 above for the text and the clarifying sentence.

2. U5 (whether a control asserting a false acceptance requirement faults). B: "'these requirements' covers the acceptance requirements as well as finalisation". Changes no output: A (unflagged) and EXPECTED both give `VOID20[C0,unknown_evidence]`, latch 1. Bearing text: ROWS "Unless overridden, controls explicitly submitted assert their required facts" (TERMU_U4{pid=60} thus asserts PID 60 has an unresolved numeric observation and no inventory, which is false); DECISION 40 "accepted with cookie zero if and only if ... that PID has an unresolved numeric observation, and no inventory entry or registered lifetime has that PID" and "A control falsely claiming these requirements reports FAULT(unknown_evidence,d,C)"; DECISION 43 "a false claim retains its fault consequences". Determination: ambiguous. "These requirements" may mean only the finalisation requirements, since the sentence directly follows them, or also the acceptance conditions. Under the narrow reading, the fault rests only on DECISION 43's general "a false claim retains its fault consequences" applied to the facts that ROWS says the control asserts. The text does not state that this general clause covers acceptance facts. Clarifying sentence: "'These requirements' includes the acceptance conditions (naming, numeric observation, absence of an inventory entry) as well as the finalisation requirements." This same sentence would not by itself settle U4, where nothing false is asserted.

3. U15 (whether an EXIT in a suspended epoch with no REARM counts as consumed uncovered). B: "the uncovered count increments only on validated release consumption, as for D45 discards ... with no REARM, nothing is released". Changes output relative to EXPECTED (0 vs +1); A reaches the same 0 unflagged. The contract is ambiguous; see U15 above.

4. U12 counting choice (LATE_U(40) as uncovered record #1, X_42 as #2, total 2). Changes no output: A gives "1 at LATE, then 2" and EXPECTED "uncovered +2 total". Bearing text: ROWS U12 "Its LATE_U(40) represents exactly one consumed uncovered record of PID 50"; DECISION 40 "counted once". The row text determines it.

## Contract or row text that needs a clarifying sentence

- DECISION 40, non-accepted cookie-zero TERMINAL_UNRESOLVED (U4): the sentence in the U4 section.
- DECISION 40, uncovered consumption during suspension (U15): the sentence in the U15 section.
- DECISION 40, scope of "these requirements" (U5, no output depends on it in either answer set): the sentence in assumption 2.


## Settlement and re-check (2026-11-21)

The design author settled the three points above with exact text (three sentences of DECISION 40; row U15's expected output):
- U4: a submitted cookie-zero TERMINAL_UNRESOLVED that fails an acceptance or finalisation requirement reports FAULT(unknown_evidence) and records no disposition, even if it asserts nothing false; a control naming no EXIT is invalid.
- U15: acceptance alone neither consumes the EXIT nor increments the uncovered-record count; the EXIT is counted once only when it passes validated consumption on the ordinary release path in an active epoch. EXPECTED for U15 changed from uncovered +1 to uncovered +0.
- U5: "falsely claiming" covers the acceptance conditions as well as the finalisation requirements.

Separately, an independent test implementer working from the contract alone could not determine one outcome (one DATA observation that both fails arrival validation and contradicts a discovery proof). Both design advisers chose the same rule (two faults, the arrival fault first, then one unknown_evidence); it is a NEW rule, added to DECISION 43 with row O1.

Two fresh independent answer sets, written from the settled text alone, were taken for rows U4, U5, U9 (unchanged, as a control), U15 and O1. Both agree with each other and with EXPECTED on all five (the "Re-check" sections of answers-delta6-sol.md and answers-delta6-opus.md).

Final count for delta 6: 16 rows (U1-U15, O1); after the settlement, the answer sets and EXPECTED agree on all 16.


## Second packet (2026-11-21): the contract gaps found by the exhaustive enumeration

An independent reference model and the merge implementation were compared over 1,307,178 short schedules. The 18,021 schedules that differed came from six causes; four were places where the contract did not decide the outcome (a CONFIRM_NO_WAIT_WORD before its route-(c) ADMIT; a TRY_NO_WAIT_WORD after an ADMIT; a bound and a discharge of one lifetime committable in one release attempt; a conflicting terminal claim after a committed discharge). A code review added a fifth (closure computed from uncommitted plan state of a held release). Both design advisers answered; where they differed the lead took the conservative reading (a later claim or observation that disputes a committed proof revokes it). Rows G1-G8 pin the rules; N6 and P3 state `x=—` explicitly.

# Oracle7 comparison: answers-sol (A) vs answers-opus (B) vs EXPECTED, rows G1-G8, N6, P3

Counts: ALL-AGREE 12, ANSWERS-AGREE-EXPECTED-DIFFERS 0, ANSWERS-DIFFER 0, ANSWERS-AGREE-EXPECTED-INCOMPLETE 0, UNDETERMINED 0 (12 rows, all named variants included). Neither answerer wrote UNDETERMINED.

Outcomes compared per row and variant: released records and order, FRONTIERs, each VOID/fault with detection time, counter vector and reason, which control faults and whether it is recorded, b and x per lifetime, revocations and revoked OPENs (O!), startup-gap state and persisted end (END), P=L+Q, census, certification latch, uncovered and abandoned counts, and for G8 the held set and reader verdict.

Conventions reconciled (not differences):
- EXPECTED `L=1/0` = A "latch=set / not set" = B "latch SET / unset".
- EXPECTED `G(c)` and "one END c" = A "persists GAP_CLOSE(c)" = B "GAP_CLOSE(P_close=c) ... PERSISTED end c". EXPECTED `O!` = A "persist revoked GAP_OPEN(0)" = B "revoked startup OPEN persisted". EXPECTED "historical END c followed by O!" = A GAP_CLOSE(c) then revoked GAP_OPEN(0) = B "historical END c remains observable".
- EXPECTED `b=—, x=—` = A `b_D=none; x_D=none` = B "no b, no x".
- Relative order of GAP_CLOSE and FRONTIER: B writes GAP_CLOSE before the FRONTIER of the same attempt (G2b, G3, G4, G4b, G7, N6), and after F18 in G5 (closure on a later control). A and EXPECTED list only DATA/FRONTIER in the output stream. Treated as notation per EXPECTED's top paragraph ("Gap publications are separate from merge DATA/FRONTIER output"); see flagged assumption 3 for whether the contract fixes it.
- census, abandoned and uncovered: EXPECTED's G-row default (`census=known`, `abandoned=0`, uncovered increment 0) = A's explicit values = B's stated defaults ("stay 0 in every row unless stated"; census known in every row).
- All VOIDs are `VOID20[C0,...]` in A, B and EXPECTED (`Vu=VOID20[C0,unknown_evidence]`).

| ID | class | outcome (all three agree) |
|---|---|---|
| G1 | ALL-AGREE | Only `Vu`, at CN6; CN6 faults, not recorded, not retained; TRY and later ADc6 recorded without another fault; `b=—, x=—`, G(open), no END/O!, P=1+0, census known, L=1. |
| G2 | ALL-AGREE | Base: only `Vu`, at TRY; TRY not recorded, revokes nothing; ADc6 and CN6 recorded, CN6 commits nothing; `b=—, x=—`, G(open), no END/O!, P=1+0, L=1. Prefixed: `VOID20[C0,allocation_fail] → Vu`, same final state (latched by the first VOID). |
| G2b | ALL-AGREE | `F18 → Vu`; before TRY `b_D=6, G(6), P=0+0`; TRY not recorded, revokes b; historical END 6 then one O!; no epoch invalidation; final `b=—, x=—`, G(open), P=1+0, census known, L=1. |
| G3 | ALL-AGREE | Base: `EXIT_D18 → F18`; bound wins the same attempt: `b_D=6, x=—`, END 6, P=0+0, L=0. Revocation variant: adds `Vu`, TRY not recorded, b revoked, one O!, terminal evidence not promoted; final `b=—, x=—`, G(open), P=1+0, L=1. |
| G4 | ALL-AGREE | Both variants: `EXIT_D18 → F18 → Vu`; `x_D=18` and END 18 commit at S(19), then revoked by the conflicting TERM (not replacing the retained disposition); one O!; final `b=—, x=—`, G(open), P=1+0, L=1. |
| G4b | ALL-AGREE | `EXIT_D18 → F18 → Vu`; x=18 and END 18 commit, TERMU faults, does not replace the matched disposition, consumes nothing; one O!; uncovered 0; final `b=—, x=—`, G(open), P=1+0, L=1. |
| G5 | ALL-AGREE | `EXIT_D18 → F18`; at S(19) no discharge, P=1; AD_D6 recorded without fault, no bound; TERM commits `x_D=18` and closure END 18; final `b=—, x=18`, G(18), P=0+0, L=0, no O!. |
| G6 | ALL-AGREE | Only `Vu`, at AD_D6; TRY recorded; ADMIT not accepted; no DATA/FRONTIER/END/O!; final `b=—, x=—`, G(open), P=1+0, L=1. |
| G7 | ALL-AGREE | `EXIT_D18 → F18`; x=18 and END 18 at S(19); AD_D6 and B_D6 recorded without fault, no bound; final `b=—, x=18`, G(18), P=0+0, L=0; one END, no O!. |
| G8 | ALL-AGREE | `EXIT_D18 → F18`; AD_H8 recorded; S(22) holds A20, H21 and F21 (TEXT x unavailable), no `b_H`; TERM commits only `x_D=18`; final `b_D=—, b_H=—, x_H=—`, G(open), no END/O!/fault, P=1+0, L=0; reader INCOMPLETE after the last boundary. |
| N6 | ALL-AGREE | `F5 → F7 → EXIT_D12 → F12`; `b_D=6` at S(8), END 6 (`C_close=6`, no increment); NULL fire internal only; EXIT ordinary, no x, no terminal TRANS; P=0+0, census known, L=0, C0 unchanged; no listing or numerical contribution. |
| P3 | ALL-AGREE | Same output; persisted G(6) (reserved field zero), unchanged by EXIT12; `x=—`; not a listed backend, no N_k/U_k/numerical contribution, no omission note, no role-support record. |

Row-level notes (no classification change):
- G8: EXPECTED names the reader window `[50,60)`, which ROWS G8 does not request. A ("a reader window after the last boundary is not complete"; final gap OPEN(0)) and B ("A reader window after the last boundary (22) is NOT complete ... startup gap is still open from boot_start") both cover it.
- G6: A states its reason as contradicting the retained NULL-pointer history; B as a false address claim (flagged). Same outputs; see assumption 1.
- G2b / G3 variant: A also states "invalidate affected cached answers"; B does not mention caches. EXPECTED does not list caches for these rows; DECISION 43 requires it ("invalidates affected cached answers"). Not an outcome either answer contradicts.

## Non-agreeing rows

None.

## Assumptions flagged by an answerer

Set A flagged none. Set B flagged the following; each is addressed against the contract text.

### 1. G6: numerical ADMIT after a NULL-pointer TRY faults as a false address claim and is not recorded

Effect: changes no output relative to A or EXPECTED (all give `Vu` at AD_D6, no accepted admission, L=1).

Bearing text:
- DECISION 36: "Contradictory provenance or a claimed admission proof containing a false required fact reports `FAULT(unknown_evidence,d,C)`." and "Its proof includes Decisions 31–35, route-specific address qualification, ... Unknown facts are false."
- DECISION 38: "A qualified non-NULL pointer fire makes that still-pending lifetime eligible for ordinary Decision 37(a) or 37(b) attachment; wait-word attempts start then."
- DECISION 37(c): "This route applies only to a bound discovery lifetime whose qualified permanent role cannot acquire or use a wait word." DECISION 39: "Decision 37(c) ... does not qualify a NULL target as a wait-word address."
- DECISION 38 (last paragraph) covers only the reverse order: a TRY "delivered after an ADMIT ... after a numerical ADMIT with no qualified non-NULL pointer fire recorded for that lifetime ... is also evidence against the admission's address proof".
- ROWS: "`AD_De` ... admission facts satisfy Decision 36 unless overridden."

Determination: the fault is determined by inference, not by a sentence for this order. The TRY default supplies a qualified permanent role (logger 13) and no non-NULL fire, so under DECISION 38 no wait-word attempt can have started and the numerical address proof cannot be true; DECISION 36 then faults it. Two points are not stated: (i) the ROWS default "admission facts satisfy Decision 36" could be read as asserting a valid address proof, which the recorded TRY history makes impossible; (ii) DECISION 36 does not say whether a faulting ADMIT is recorded (contrast DECISION 38, "is not recorded", for TRY). (ii) changes no output in G6 because the latch is set by the same fault and no later control follows.
Clarifying sentence: "A numerical-route ADMIT for a lifetime whose recorded TRY_NO_WAIT_WORD history has no qualified non-NULL pointer fire is contradictory provenance: it reports FAULT(unknown_evidence,d,C) and is not recorded."

### 2. G5 and G8: a TERM arriving after its EXIT's release completed commits the discharge on that control; in G5 closure also commits on that control

Effect: changes no output; A gives the same in both rows (G5 "backed by the previously completed EXIT release, it commits x_D=18 and persists GAP_CLOSE(18)"; G8 "commits x_D=18 using D's previously completed EXIT release"), and EXPECTED G5 `TERM commits x_D=18 ... G(18)`, G8 `TERM commits only x_D=18`.

Bearing text:
- DECISION 40: "supplies no discharge until both its successful complete release attempt and an unrefuted matching terminal disposition exist, subject to the Decision 43 latch."
- Clarification (lead, 2026-11-20): "A terminal discharge commits only when the release attempt that output its EXIT completes without fault ... whatever the order of TERM and EXIT."
- DECISION 41: "Every closure path applies the same ... test to committed, unrefuted proofs plus only those candidates whose commitments successfully complete in that same attempt; outside a release attempt this permits the current terminal-discharge candidate backed by its previously completed EXIT release".
- §1 DECISION (line 2): "Evaluate release after every individual input"; ROWS: "release evaluation after every input".

Determination: determined. The discharge needs both facts and the second arrives with TERM; DECISION 41 explicitly allows a closure path outside a release attempt for that candidate. In G5 completeness strictly beyond C_close=max(5,18)=18 holds from S(19), so closure commits; in G8 H is unresolved, so no closure (DECISION 41 "discharge of every inventory entry"). No clarifying sentence needed.

### 3. Several rows: GAP_CLOSE listed before the FRONTIER of the same release attempt

Effect: notation only per the task rule and EXPECTED ("Gap publications are separate from merge DATA/FRONTIER output"). Every publication succeeds in these rows, so no committed state depends on the order.

Bearing text:
- DECISION 41: "Require qualified completeness strictly beyond `C_close`, ... and successful publication of all supporting records and coverage evidence before committing closure." "Neither `C_close` nor `P_close` supplies a source boundary or advances FRONTIER."
- DECISION 40 / clarification: a discharge commits only when the attempt completes "including every counter checkpoint and the FRONTIER publication when one is due".
- DECISION 41: closure considers "candidates whose commitments successfully complete in that same attempt".

Determination: silent on the persisted relative order. For a discharge-based closure (G4, G4b, G7) one reading is that closure must follow F18's publication, because the discharge it relies on commits only after that FRONTIER is published, which would put GAP_CLOSE after F18, against B's listing. The other reading is that DECISION 41's same-attempt clause lets the attempt commit discharge and closure together, with no persisted order between the gap record and the FRONTIER. For bound-based closures (G2b, G3, N6) no sentence orders them.
Clarifying sentence: "Within one release attempt, the startup-gap END is published [after / before / unordered relative to] that attempt's FRONTIER; a reader must not depend on their relative order."

### 4. Several rows: D's EXIT carries its discovery cookie unless marked `{no cookie}`

Effect: relevant to output, but A and EXPECTED make the same assumption (A G5: "the independently bound cookie-bearing EXIT"; EXPECTED G4, G5, G7, G8 release EXIT_D18 at S(19) before any disposition). Under the other reading, a cookie-less EXIT would be held until TERM. In G8 it would then join the batch held for TEXT x, so EXIT_D18, F18 and `x_D=18` would not occur.

Bearing text:
- ROWS G rows: "Use existing R-row defaults: installed, independently bound D"; R rows: "D is installed and independently bound"; ROWS: "`TERM_Dt` and `EXIT_Dt@L` substitute D and timestamp `t` in the existing terminal notation"; G4b marks its EXIT `{no cookie}` explicitly.
- §6 (before DECISION 36): "Copy the resolved cookie into the envelope before publication ... Resolve terminal TRANS and EXIT from one identity snapshot."
- DECISION 40: "If a discovery cookie exists, snapshot it before tombstoning." "An independently bound cookie-bearing lifecycle EXIT of an inventoried, never-admitted lifetime is eligible for ordinary validated publication without ADMIT, ABANDON or a terminal disposition".

Determination: determined by inference. An installed identity has a cookie, which the envelope rule copies into the EXIT; G4b's explicit marker is the exception. No ROWS sentence states the default directly.
Clarifying sentence (ROWS): "In G rows, `EXIT_Dt@L` carries D's installed discovery cookie unless marked `{no cookie}`."

### 5. G1 and G2: the later valid controls are recorded under the latch

Effect: changes no output; A says the same (G1 "subsequent ADc6 is valid and recorded under the latch"; G2 "ADc6 is recorded ... CN6 is valid and recorded but commits nothing"); EXPECTED G1 "ADc6 is subsequently recorded without another fault", G2 "CN6 is recorded without another fault".

Bearing text:
- DECISION 43: "A valid subsequent control is recorded without a fault caused by the latch; a false claim retains its fault consequences." "After that point no new discovery lower bound, terminal discharge or startup-gap END commits".
- DECISION 38: "including a valid ADMIT recorded under the Decision 43 latch or during suspension".
- DECISION 37(c): CN without a prior matching ADMIT "reports `FAULT(unknown_evidence,d,C)` and is not retained as a candidate for a later ADMIT" (so G1's ADc6 has no CN to pair with).

Determination: determined. No clarifying sentence needed.

### 6. G8: H is installed and bound like D

Effect: changes no output; A treats AD_H8 as valid and recorded, and EXPECTED lists no fault. If H were not installed, AD_H8 would claim a false required fact (DECISION 36) and fault, and EXPECTED's "no ... fault" and L=0 would not hold.

Bearing text:
- ROWS: "`H=(pid=40,g=2^63+1)`"; G8 "Census `{D,H}`"; "`AD_De` ... admission facts satisfy Decision 36 unless overridden"; R rows: "`h∈{0,1}` means H absent or inventoried and unresolved".
- DECISION 36: the proof "includes Decisions 31–35 ... installation visibility before enable"; "a claimed admission proof containing a false required fact reports `FAULT(unknown_evidence,d,C)`".

Determination: determined by the ROWS AD default (admission facts satisfy DECISION 36, which include installation and binding) together with H's stated cookie. No ROWS sentence says directly that H is installed.
Clarifying sentence (ROWS): "In G8, H is installed and independently bound with cookie `2^63+1`, as D is."

## Contract or row text that would benefit from a clarifying sentence

No answer depends on these. Each would make explicit a default that all three sets inferred the same way.
- DECISION 36/38: numerical ADMIT after a NULL-only TRY history (assumption 1).
- DECISION 41: persisted order of gap END and FRONTIER within one attempt (assumption 3).
- ROWS G-row defaults: cookie on D's EXIT (assumption 4) and installation of H (assumption 6).


After this comparison one notation sentence was added to ROWS.md (a lifecycle EXIT of D carries its discovery cookie unless marked `{no cookie}`; H in G8 is installed and bound like D), stating what both answerers and EXPECTED had assumed. Left as notes for a later delta, because no outcome depends on them in these rows: whether a faulting ADMIT is itself recorded (G6), and the relative order of a gap publication and the FRONTIER of the same release attempt.

Final count for delta 6: 26 new rows (U1-U15, O1, G1, G2, G2b, G3, G4, G4b, G5, G6, G7, G8) and two existing rows made explicit (N6, P3); 149 rows in all. After the settlements, the answer sets and EXPECTED agree on every one.
