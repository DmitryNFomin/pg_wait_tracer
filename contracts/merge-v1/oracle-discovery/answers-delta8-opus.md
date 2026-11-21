# Independent answer set (opus): merge-v1 delta 8

Written from the amended contract, ROWS.md and TASK-delta8.md alone.

# merge-v1 delta 8 — independent contract-only answers (opus)

Sources read: CONTRACT.md (§§2-6, DECISIONs 30-45 and clarifications), CONTRACT-coverage-a2-v1.md, oracle-discovery/ROWS.md, oracle-discovery/TASK.md. Nothing else was read. Agreement is not implementation or platform qualification.

## 0. Common conventions used in every execution

- Sources {P0,P1,E,L}; mode N; A=(10,1), B=(20,1) attached; D=(30,2^63) installed, independently bound; complete census {D} at fence 5 (known) unless the row family overrides; C0; detection time 20 (22 after RA/REARM where stated); boot_start displayed as 0.
- `S(w)` = `BOUNDARY(P0,w)`, `BOUNDARY(P1,w)`, `BOUNDARY(E,w)`, `BOUNDARY(L,w)`, each with its next source position, each followed by its own release evaluation (§5 "Every expanded input gets its own release evaluation"). With all cutoffs 0 initially, W=min(w[s]) stays 0 until the L constituent, so every release in these rows happens at the L constituent (§3 "an unbounded source has cutoff 0 and blocks release").
- Positions are per source, starting at 1, counting DATA, TEXT and boundary records (§5). Controls (AD, B, TRY, TERM, TERMU, AB, AOP, J, CHECK, REARM, FAULT) consume none (ROWS notation).
- Release attempt order (§3 + Decision 41): publish TEXT/DATA → counter checkpoint ("the counter vector still matches the epoch baseline") → advancing FRONTIER → remaining counter checkpoint(s) → END last when the attempt closes the startup gap. "Successful END publication commits the affected bound, discharge and closure; no fallible step of that attempt follows it." (D41)
- Timeline notation: `OPEN(0)` = initial plain startup GAP_OPEN at boot_start ("Open a hard base-coverage gap at boot_start before advertising startup coverage", D41); `END(P,res=0)` = startup GAP_CLOSE with endpoint P_close and reserved field 0 (D42 "Store P_close in the existing GAP_CLOSE endpoint field and keep its reserved field zero"); `O!` = revoked startup OPEN (D43); `chk` = counter checkpoint (not a record); `▲` = commitment instant.
- State columns: Ep = epoch (e0/e1) active/suspended/exit-required; Lt = certification latch; Adm = historical admitted fact / admission-proof status; TC = retained terminal claim / its refutation; Disp = named EXIT's final-disposition accessor (NONE = no recorded disposition; MATCHED = recorded matched, unrefuted; MATCHED(ref) = recorded MATCHED whose claim is refuted); b,x = committed b_D, x_D; Pend = L+Q (unresolved lifetimes + numeric observations); Cen = census; Gap = startup gap; Cc/Pc = C_close/P_close (hist = historical value after revocation).
- No nonzero exit and no epoch invalidation occur in any execution below (no internal breach; no published frontier is disproved). Storage counters change only where a stated observation vector says so; discovery faults increment only their own diagnostic count (D43 two-fault paragraph; §4 "Ordering faults ... do not fabricate increments in this storage vector").
- Reader evaluation of [50,60) assumes otherwise valid base coverage, attribution and A2 evidence; only the persisted startup-gap records are evaluated here (TASK item 5). "startup-unavailable" means [50,60) lies inside an open startup gap; "startup-OK" means the startup gap does not exclude it.

---

## V4

Expanded inputs: (1) `ABANDON_DISCOVERY(30,2^63,20,C0)`; (2) `TERM_D18` (matched, x=18, names EXIT_D18 at L#1); (3) `EXIT_D18@L` L#1 (cookie-bearing); (4-7) S(19): P0#1, P1#1, E#1, L#2 at d=20; writer rejects F18 at d=20 observing Cw; (8) `REARM(21,Cw,fresh A/B proofs)`; (9) `TERM_D18` (identical) d=22; (10-13) S(20): P0#2, P1#2, E#2, L#3 at d=22.

Establishing inputs: abandonment at (1) (D45 acceptance: installed, never admitted, gap open). Hold: none beyond W=0 staging. Fault: (7) writer_push_fail on F18. Commitment: none.

| # | Input | Effect | Ep | Lt | Adm | TC | Disp | b/x | Pend | Cen | Gap |
|---|---|---|---|---|---|---|---|---|---|---|---|
| 1 | AB_D | accepted, abandoned (permanent) | e0 A | 0 | false | none | NONE | -/- | 1+0 | known | open |
| 2 | TERM_D18 | recorded matched final claim (applies when EXIT consumed, D40) | e0 A | 0 | false | x=18 unrefuted | MATCHED | -/- | 1+0 | known | open |
| 3 | EXIT_D18 | staged (W=0); eligible without ADMIT (D40 cookie-bearing paragraph); not discarded (D45 "released normally") | e0 A | 0 | false | x=18 | MATCHED | -/- | 1+0 | known | open |
| 4-6 | P0,P1,E bnd | W=0 | same | | | | | | | | |
| 7 | L bnd W=19 | publish EXIT_D18 ✓ → chk C0 ✓ → F18 rejected at 20, Cw → FAULT writer_push_fail: latch set, suspend, no discharge, no END | e0 S | 1 | false | x=18 unrefuted | MATCHED | -/- | 1+0 | known | open |
| 8 | REARM | accepted (suspended); baseline Cw; cutoffs 0; retained boundaries 19 | e1 A | 1 | false | x=18 | MATCHED | -/- | 1+0 | known | open |
| 9 | TERM_D18 | identical duplicate: idempotent, no fault (D40 "an identical duplicate is idempotent") | e1 A | 1 | … | x=18 | MATCHED | -/- | 1+0 | known | open |
| 10-12 | P0,P1,E bnd | W=0 | | | | | | | | | |
| 13 | L bnd W=20 | empty batch → chk Cw ✓ → FRONTIER(19,Cw) (advances: no frontier ever successfully published). No discharge: "an EXIT output in an attempt that is then VOIDed discharges nothing in that or any later epoch" (Clarification 2026-11-20); latch also forbids it (D43) | e1 A | 1 | false | x=18 | MATCHED | -/- | 1+0 | known | open |

Successful merge stream: `DATA EXIT_D18(t=18) [L#1]` → `VOID(20,Cw,writer_push_fail)` → `FRONTIER(19,Cw)`. No TEXT; no prior_interval_certified markings (no TRANS/MARKER).
Rejected calls: `FRONTIER(18,C0)` at d=20.

Combined timeline: `OPEN(0)` → `EXIT_D18` → chk(C0) → [F18 rejected] → `VOID20[Cw,writer_push_fail]` → (REARM) → chk(Cw) → `FRONTIER(19,Cw)`. No END, no O!.

Prefix inspection (closing attempt at S(19) L): P1=[OPEN]; P2=[OPEN,EXIT_D18] = last prefix before failure = prefix at VOID entry. No prefix contains an END. END never exists.

VOID20 entry: e0 suspended, latch set, abandoned, no ADMIT, TC x=18 unrefuted, Disp MATCHED, no b/x, pending 1+0, census known, gap open under its existing OPEN (D41 failed-closure-attempt sentence). Persisted prefix [OPEN,EXIT_D18]. No revocation (nothing committed), no cache effect. Reader: startup-unavailable.

Diagnostics: writer_push_fail VOID 1; storage vector C0→Cw (writer_push_fail=1); capture paused at VOID (§4 "Allocation or writer failure causes VOID and paused capture"); discovery_abandoned_records 0 (no D TRANS/MARKER/TEXT); uncovered 0; payload disposal: none staged at fault; no invalidation; no nonzero exit.

## N14 (amended: GAP_CLOSE endpoint 6)

Expanded inputs: (1) physical history TRY_NW_D(6) (adapter facts: pointer watch disabled, visible, enable at 6, pinned NULL read after 6, fence, zero fires; PG13 logger role 13); (2) `ADMIT_DISCOVERY(D,α,6,proof{route=c},20,C0)`; (3) `CONFIRM_NO_WAIT_WORD(D,α,6,witness,20,C0)`; (4-7) S(8): P0#1, P1#1, E#1, L#1 at d=20.

Establishing inputs: candidate b=e=6 from (2)+(3) (D37(c): "Commit b_i=e only after matching route-(c) ADMIT, qualified source completeness strictly beyond e, ..."). Fault: END rejection at (7). Commitment: none.

| # | Input | Effect | Ep | Lt | Adm | TC | Disp | b/x | Pend | Cen | Gap |
|---|---|---|---|---|---|---|---|---|---|---|---|
| 1 | TRY_NW physical history | retained history only | e0 A | 0 | false | none | NONE | -/- | 1+0 | known | open |
| 2 | AD{c} | recorded, route c | e0 A | 0 | true/valid | none | NONE | -/- | 1+0 | known | open |
| 3 | CONFIRM_NW | candidate b=6 (matching recorded ADMIT) | e0 A | 0 | true | none | NONE | cand 6 | 1+0 | known | open |
| 4-6 | P0,P1,E | W=0 | | | | | | | | | |
| 7 | L W=8 | empty batch → chk C0 ✓ → F7 ✓ → chk C0 ✓ → C_close=max(5,6)=6, P_close=6 (route (c) supplies no uncertified TRANS/MARKER, D41) → END(6,0) rejected at 20, Cw → writer_push_fail; latch, suspend; b not committed | e0 S | 1 | true/valid | none | NONE | -/- | 1+0 | known | open |

Successful stream: `FRONTIER(7,C0)` → `VOID(20,Cw,writer_push_fail)`. Rejected: `GAP_CLOSE/END(6,res 0)` at 20.
Timeline: `OPEN(0)` → chk → `F7` → chk → [END(6) rejected] → `VOID20[Cw,writer_push_fail]`.
Prefixes: [OPEN]; [OPEN,F7] = last before failure = VOID-entry prefix. No complete END in any prefix (stipulated: rejected GAP_CLOSE publishes no reader-visible END). F7 stays observable ("successful earlier DATA and FRONTIER publications remain observable under the ordinary VOID rules, and no rollback OPEN is required", D41) and belongs to the uncertified suffix (§3).

VOID20 entry: suspended, latch set, admitted true (valid), candidate uncommitted, pending 1+0, census known, startup gap open under existing OPEN; all views and the prefix-only reader agree (D41). Reader: startup-unavailable. D (PG13 logger) is not a backend, not listed or counted (D43).
Diagnostics: writer_push_fail 1; C0→Cw; capture paused; abandoned 0; uncovered 0; no invalidation; no nonzero exit.

## H11d — `AD_D19, TERM_D18, EXIT_D18@L`

Inputs: (1) `ADMIT_DISCOVERY(D,α,19,proof,20,C0)`; (2) `TERM_D18` naming EXIT at L#1, x=18; (3) `EXIT_D18@L` L#1 (cookie-bearing, valid ordering).

| # | Effect | Ep | Lt | Adm | TC | Disp | b/x | Pend | Cen | Gap |
|---|---|---|---|---|---|---|---|---|---|---|
| 1 | AD recorded (no EXIT yet, no contradiction) | e0 A | 0 | true/valid | none | NONE | -/- | 1+0 | known | open |
| 2 | x=18<e=19: "An otherwise qualified matched terminal control with terminal time x<e establishes this same contradiction against a recorded ADMIT on delivery even before its named EXIT DATA arrives: record the terminal claim, refute both recorded claims, report one FAULT(unknown_evidence,d,C)" (D36). Recorded as final MATCHED disposition, refuted (D40 "...whether that control establishes the contradiction or follows its earlier report"). Latch set, suspend, VOID20 | e0 S | 1 | true/refuted | x=18 refuted | MATCHED(ref) | -/- | 1+0 | known | open |
| 3 | arrival-validated during suspension; no second report ("do not report that contradiction again when the matching EXIT arrives"); payload disposed without consumption (§4) | e0 S | 1 | true/refuted | x=18 refuted | MATCHED(ref) | -/- | 1+0 | known | open |

MATCHED becomes observable at input 2, at the recording performed before the VOID callback (so the accessor already returns MATCHED at VOID20 entry).
Stream: `VOID(20,C0,unknown_evidence)` only. Rejected: none. Timeline: `OPEN(0)` → `VOID20`. No END, no O! (nothing committed: "Refuting a candidate that never committed does not itself publish a revoked OPEN", D43).
VOID20 entry: suspended, latch, admitted true/refuted, TC x=18 refuted, Disp MATCHED, pending 1+0, census known, gap open; prefix [OPEN]; reader startup-unavailable; no cache effect.
Diagnostics: unknown_evidence 1; storage C0 unchanged; abandoned 0; uncovered 0 (payload disposal is not uncovered consumption, D40); no invalidation/exit.

## H11e — `AD_D19, EXIT_D18@L, TERM_D17{names that EXIT}, TERM_D18`

Inputs: (1) AD e=19; (2) EXIT_D18 L#1 cookie-bearing; (3) TERM_D17 naming L#1; (4) TERM_D18 naming L#1.

| # | Effect | Ep | Lt | Adm | TC | Disp | Faults |
|---|---|---|---|---|---|---|---|
| 1 | AD recorded | e0 A | 0 | true/valid | none | NONE | 0 |
| 2 | "An ADMIT whose e is later than ... independently bound EXIT is contradictory ... If the ADMIT was validly recorded before the EXIT established the contradiction, retain the historical admission fact and refute its proof. The observation establishing the contradiction reports FAULT" (D36). FAULT#1, latch, suspend; staged EXIT payload discarded (§4); VOID20 #1 | e0 S | 1 | true/refuted | none | NONE | 1 |
| 3 | terminal time 17 ≠ named EXIT's 18: separately false terminal control. "Reject a separately false terminal control before recording a final terminal claim or applying this same-contradiction suppression; preserve its ordinary fault and required refutation history" (D36). FAULT#2 → VOID20 #2 (separately delivered report during suspension, §4). Refutation history: rejected claim x=17 | e0 S | 1 | true/refuted | none recorded (history: x=17 rejected) | NONE | 2 |
| 4 | otherwise qualified match; its only contradiction is the already reported enable/EXIT one → "recorded as refuted, supplies no discharge and adds no report solely for that contradiction" (D36); "a later otherwise qualified match is not rejected merely for disagreeing with that rejected control" (D36); conflict rule of D40 applies only against a *retained final* claim, and none exists | e0 S | 1 | true/refuted | x=18 refuted (final) | MATCHED(ref) | 2 |

Remaining terminal claim after the false TERM: none recorded (only refutation history of x=17). TERM_D18 is accepted as the final disposition because TERM_D17 was rejected before recording and so is not a retained final claim it could conflict with.
b/x none; pending 1+0; census known; gap open throughout.
Stream: `VOID(20,C0,unknown_evidence)` → `VOID(20,C0,unknown_evidence)`. Timeline: `OPEN(0)` → VOID20#1 → VOID20#2. No END/O!.
VOID#1 entry: Adm true/refuted, TC none, Disp NONE, prefix [OPEN]. VOID#2 entry: same plus x=17 rejection history, Disp NONE, prefix [OPEN,VOID#1]. Reader startup-unavailable both.
Diagnostics: unknown_evidence 2; storage unchanged; abandoned 0; uncovered 0; EXIT payload disposed (not consumed).

## H11f — `AD_D19, EXIT_D18@L{no cookie}, TERM_D18`, finish at 40

Inputs: (1) AD e=19; (2) cookie-less EXIT L#1 (PID 30, no binding → undecided EXIT, held until disposition, D40); (3) TERM_D18 binding it to D; (4) capture finish d=40, C0.

| # | Effect | Ep | Lt | Adm | TC | Disp | Faults | uncovered |
|---|---|---|---|---|---|---|---|---|
| 1 | AD recorded | e0 A | 0 | true/valid | none | NONE | 0 | 0 |
| 2 | staged and held (undecided terminal); no D36 contradiction yet (not independently bound to D) | e0 A | 0 | true/valid | none | NONE (undecided) | 0 | 0 |
| 3 | TERM binds EXIT to D; x=18<e=19 → contradiction on delivery: record claim, refute both, one FAULT (D36); recorded as final MATCHED disposition "including for a cookie-less EXIT" (D40); undecided hold removed; latch, suspend; staged EXIT payload discarded; VOID20 | e0 S | 1 | true/refuted | x=18 refuted | MATCHED(ref) | 1 | 0 |
| 4 | finish: disposition exists → "capture finish reports no missing-disposition fault for that EXIT" (D40) | e0 S | 1 | true/refuted | x=18 refuted | MATCHED(ref) | 1 | 0 |

State after TERM: Disp MATCHED(ref), faults 1. At finish: Disp MATCHED(ref), fault total 1 (separately reported: no additional finish fault, no VOID40).
b/x none; pending 1+0; census known; gap open.
Stream: `VOID(20,C0,unknown_evidence)`. Timeline `OPEN(0)` → VOID20. VOID entry: as row 3; prefix [OPEN]; reader startup-unavailable.
Consumption: EXIT never passes validated consumption (discarded at fault): uncovered 0, abandoned 0, published 0.

## G5 — `EXIT_D18@L, S(19), AD_D6, TERM_D18`

Inputs: (1) EXIT_D18 L#1; (2-5) S(19) P0#1,P1#1,E#1,L#2; (6) AD e=6; (7) TERM_D18.

| # | Effect | Ep | Lt | Adm | TC | Disp | b/x | Pend | Gap | Cc/Pc |
|---|---|---|---|---|---|---|---|---|---|---|
| 1 | staged | A | 0 | false | none | NONE | -/- | 1 | open | - |
| 5 | L W=19: EXIT_D18 ✓ → chk ✓ → F18 ✓ → chk ✓; no disposition → no discharge; EXIT release evidence complete | A | 0 | false | none | NONE | -/- | 1 | open | - |
| 6 | AD recorded ("A valid ADMIT delivered after ... EXIT has been released is recorded ... no fault for that order", D36; e=6≤18) | A | 0 | true/valid | none | NONE | -/- | 1 | open | - |
| 7 | matched, unrefuted; backed by previously completed EXIT release → closure possible on this control: "publish END during that control's evaluation without repeating an already published FRONTIER" (D41). C_close=max(5,18)=18; completeness 19>18; P_close=18 (EXIT alone does not trigger, D42). chk(C0) → END(18,0) ▲ commits x=18 and closure | A | 0 | true/valid | x=18 unrefuted | MATCHED | -/18 | 0 | closed | 18/18 |

Stream: `EXIT_D18` → `FRONTIER(18,C0)`. Timeline: `OPEN(0)` → EXIT → chk → F18 → chk → … → chk(TERM) → `END(18,res0)`▲. Prefix before END: [OPEN,EXIT,F18]; including END: +END(18,0); END only after all acceptance checks. No repeated F18.
No VOID. Reader on final prefix: startup gap [0,18) → [50,60) startup-OK. Diagnostics all zero.

## G7 — `TERM_D18, EXIT_D18@L, S(19), AD_D6, B_D6`

Inputs: (1) TERM_D18; (2) EXIT L#1; (3-6) S(19); (7) AD e=6; (8) B_D6 (quiet candidate t=6, e=6).

| # | Effect | Adm | TC | Disp | b/x | Pend | Gap | Cc/Pc |
|---|---|---|---|---|---|---|---|---|
| 1 | matched claim recorded | false | x=18 | MATCHED | -/- | 1 | open | - |
| 2 | staged (disposition present) | false | x=18 | MATCHED | -/- | 1 | open | - |
| 6 | L W=19: EXIT ✓ → chk ✓ → F18 ✓ → chk ✓ → END(18,0) ▲ (C_close=18,P_close=18) | false | x=18 | MATCHED | -/18 | 0 | closed | 18/18 |
| 7 | "once its discharge has committed, a later valid ADMIT or confirmation is recorded and commits no bound" (D41); no fault | true/valid | x=18 | MATCHED | -/18 | 0 | closed | 18/18 |
| 8 | quiet candidate recorded, commits no bound; no records to reconcile | true | x=18 | MATCHED | -/18 | 0 | closed | 18/18 |

Epoch active, latch 0, census known throughout. Stream: `EXIT_D18` → `FRONTIER(18,C0)`. Timeline: `OPEN(0)` → EXIT → chk → F18 → chk → `END(18,res0)`▲. Prefix before END [OPEN,EXIT,F18]; with END adds END(18,0). No VOID; diagnostics zero. Reader: [50,60) startup-OK.

## G7-cut-after-END

Prefix retained: [OPEN(0), EXIT_D18, FRONTIER(18,C0), END(18,res 0)], cut immediately after the complete END becomes reader-visible; in-memory bookkeeping, AD_D6 and B_D6 never execute.
Commitment represented: the committed discharge x_D=18 and committed startup closure with C_close=P_close=18. Basis: "Successful END publication commits the affected bound, discharge and closure; no fallible step of that attempt follows it. A complete reader-visible END constitutes successful publication for this commitment rule, including when the process stops before updating its in-memory state." (D41). The END is not provisional. No ADMIT/confirmation is represented (they were never executed; they would have committed nothing anyway).
Reader on that prefix: startup gap [0,18) unavailable; [50,60) startup-OK subject only to the supplied ordinary coverage evidence. No further persistence or reader guarantee is inferred.

## H1a — `TERM_D18, EXIT_D18@L, S(19), AD_D19`

Inputs: (1) TERM_D18; (2) EXIT L#1; (3-6) S(19); (7) AD e=19.

| # | Effect | Ep | Lt | Adm | TC | Disp | b/x | Pend | Cen | Gap | Cc/Pc |
|---|---|---|---|---|---|---|---|---|---|---|---|
| 1 | claim recorded | A | 0 | false | x=18 | MATCHED | -/- | 1 | known | open | - |
| 2 | staged | A | 0 | false | x=18 | MATCHED | -/- | 1 | known | open | - |
| 6 | EXIT ✓ → chk → F18 ✓ → chk → END(18,0)▲ | A | 0 | false | x=18 | MATCHED | -/18 | 0 | known | closed | 18/18 |
| 7 | e=19 > bound EXIT 18: contradicted on delivery → reject without recording (D36); "Refute each recorded claim ... any recorded matched terminal claim for that EXIT"; "rejection of the ADMIT does not preserve the disputed discharge" → revoke x=18 and dependent coverage from boot_start, invalidate cached answers, persist O!, D pending (D43). Latch set before fault publication; suspend; VOID20 | S | 1 | false | x=18 refuted | MATCHED(ref) | -/- | 1 | known | open (O!) | hist 18/18 |

Census stays known: "These added refutation cases do not themselves invalidate census completeness" (D43).
Stream: `EXIT_D18` → `FRONTIER(18,C0)` → `VOID(20,C0,unknown_evidence)`.
Timeline: `OPEN(0)` → EXIT → chk → F18 → chk → `END(18,0)`▲ → `O!(0)` → `VOID20`.
Prefix before END [OPEN,EXIT,F18]; with END [..,END(18,0)].
VOID20 entry: suspended, latch, admitted false, TC refuted, Disp MATCHED(ref), no b/x, pending 1, census known, gap open; prefix [OPEN,EXIT,F18,END(18),O!]; revocation and cache invalidation complete; reader: O! governs → [50,60) startup-unavailable.
Diagnostics: unknown_evidence 1; storage unchanged; abandoned 0; uncovered 0; no invalidation (F18 not disproved); no exit.

## H8a — `AD_D6, B_D6{e=6}, S(19), TRY_NW_D6`

Inputs: (1) AD e=6 (numerical); (2) B_D6 quiet candidate t=6, e=6; (3-6) S(19) all #1; (7) TRY_NW_D6 report.

| # | Effect | Ep | Lt | Adm | b | Pend | Cen | Gap | Cc/Pc |
|---|---|---|---|---|---|---|---|---|---|
| 1 | AD recorded | A | 0 | true/valid | - | 1 | known | open | - |
| 2 | quiet candidate recorded; proposes 6 (D42) | A | 0 | true | cand 6 | 1 | known | open | - |
| 6 | L W=19 (>6): empty batch → chk → F18 ✓ → chk → C_close=max(5,6)=6, P_close=6 (no uncertified record at 6) → END(6,0)▲ commits b=6 and closure | A | 0 | true/valid | 6 | 0 | known | closed | 6/6 |
| 7 | late attempt report after numerical ADMIT with no recorded non-NULL fire: "reports FAULT(unknown_evidence,d,C) and is not recorded ... is also evidence against the admission's address proof and revokes any committed numerical lower bound" (D38). Revoke b=6, persist O! before VOID (D43), D pending; ADMIT proof refuted (admitted stays true); latch; suspend; VOID20 | S | 1 | true/refuted | - | 1 | known | open (O!) | hist 6/6 |

TC none, Disp NONE, x none throughout. Stream: `FRONTIER(18,C0)` → `VOID(20,C0,unknown_evidence)`.
Timeline: `OPEN(0)` → chk → F18 → chk → `END(6,0)`▲ → `O!(0)` → `VOID20`. Prefix before END [OPEN,F18]; with END [OPEN,F18,END(6,0)].
VOID20 entry: views as row 7; prefix [OPEN,F18,END(6),O!]; reader startup-unavailable (O! governs subsequent reads). Diagnostics: unknown_evidence 1; storage unchanged; others zero.

C5.1 persisted-prefix limit: in normal execution O! is successfully persisted before the VOID callback, so the file is [OPEN,F18,END(6,0),O!] and the reader returns startup-unavailable. If the process stops, or the O! write fails, before any O! becomes persistent, the file holds only [OPEN,F18,END(6,0)]; the existing reader, under its other coverage conditions, establishes the startup gap closed at 6 and would treat [50,60) as startup-OK. An unpersisted in-memory revocation does not change those bytes. "END-last does not make subsequent revocation crash-atomic ... failure of that write or process death before it persists can leave the earlier END readable under the reader's other coverage conditions" (D43). A failed O! write does not satisfy the requirement ("an unsuccessful OPEN publication does not satisfy this requirement") and follows existing publication-failure rules. This does not excuse omitting or postponing O! in the normal case.

## H10a — `TERM_D18, EXIT_D18@L{no cookie}, S(19), TERMU_D18{cookie 0; names that EXIT}`

Inputs: (1) TERM_D18 binds the cookie-less EXIT to D; (2) EXIT L#1; (3-6) S(19); (7) TERMU_D18.

| # | Effect | Ep | Lt | TC | Disp | x | Pend | Gap | Cc/Pc |
|---|---|---|---|---|---|---|---|---|---|
| 1 | matched final disposition recorded | A | 0 | x=18 | MATCHED | - | 1 | open | - |
| 2 | staged, not held (disposition precedes EXIT, D40) | A | 0 | x=18 | MATCHED | - | 1 | open | - |
| 6 | EXIT ✓ → chk → F18 ✓ → chk → END(18,0)▲ | A | 0 | x=18 | MATCHED | 18 | 0 | closed | 18/18 |
| 7 | "An unresolved disposition for the same EXIT after a matched disposition has supplied a committed discharge likewise faults and revokes that discharge. Neither conflicting control replaces the retained final disposition" (D40). One FAULT(unknown_evidence); TERMU not recorded; revoke x=18, persist O!, D pending; latch; suspend; VOID20. (The cookie-zero numeric-only acceptance would fail anyway: PID 30 has an inventory entry.) | S | 1 | x=18 retained (support revoked; see UNRESOLVED U2) | MATCHED | - | 1 | open (O!) | hist 18/18 |

Adm false throughout; census known (D43 refutation cases do not reset census); no capture-long unresolved-disposition prohibition (no unresolved disposition was recorded).
Stream: `EXIT_D18` → `FRONTIER(18,C0)` → `VOID(20,C0,unknown_evidence)`. Timeline: `OPEN(0)` → EXIT → chk → F18 → chk → `END(18,0)`▲ → `O!(0)` → `VOID20`.
VOID20 entry: prefix [OPEN,EXIT,F18,END(18),O!]; reader startup-unavailable. Diagnostics: unknown_evidence 1; uncovered 0 (EXIT consumed under MATCHED, not unresolved); others zero.

## H11b — H1a then `D20@P0{final}`

Inputs: H1a's (1)-(7), then (8) D20@P0 P0#2.
States (1)-(7) as H1a. Input 8: arrival validation passes (P0 boundary 19, no prior P0 DATA, 20≥19). D20 is stamped after the claimed terminal time 18, but that claim is already refuted: "A terminal claim is refuted once; subsequent observations contradicting that already refuted claim report nothing solely for that contradiction" (D40). No committed proof to contradict (x revoked). Epoch suspended → payload disposed without release consumption after evidence retention (§4). No output, no fault.
State after 8: e0 S, latch 1, admitted false, TC x=18 refuted, Disp MATCHED(ref), no b/x, pending 1, census known, gap open (O!), hist Cc/Pc 18/18; P0 last DATA timestamp 20.
Stream and timeline identical to H1a; no additional VOID. Diagnostics: unknown_evidence 1 total; abandoned 0; uncovered 0; D20 disposed (not consumed).

## H11a — `AD_D19, EXIT_D18@L, TERM_D18` (no boundary)

| # | Effect | Ep | Lt | Adm | TC | Disp (accessor) | Faults |
|---|---|---|---|---|---|---|---|
| 1 | AD recorded | A | 0 | true/valid | none | NONE | 0 |
| 2 | EXIT (bound, cookie) at 18 < e=19 → ADMIT proof refuted, historical admission retained, FAULT (D36); latch; suspend; staged EXIT discarded; VOID20 | S | 1 | true/refuted | none | NONE | 1 |
| 3 | matched control "first recorded after this enable/EXIT contradiction was reported, is recorded as refuted, supplies no discharge and adds no report" (D36); "recorded as the final MATCHED disposition ... The exit-disposition accessor returns MATCHED after that recording" (D40) | S | 1 | true/refuted | x=18 refuted | MATCHED(ref) | 1 |

Accessor: before TERM = NONE (no recorded disposition; including at VOID20 entry); after TERM = MATCHED. MATCHED becomes observable at input 3.
b/x none; pending 1; census known; gap open. Stream `VOID(20,C0,unknown_evidence)`; timeline `OPEN(0)` → VOID20. VOID entry prefix [OPEN]; reader startup-unavailable; no revocation. Diagnostics: unknown_evidence 1; uncovered 0; abandoned 0.

## R11 — `EXIT_D18@L, S(19)` (no ADMIT/ABANDON/disposition)

Inputs: (1) EXIT L#1 (D installed → carries discovery cookie, D35/D40); (2-5) S(19).
(1) staged; (5) L W=19: EXIT ✓ → chk → F18 ✓ → chk. "An independently bound cookie-bearing lifecycle EXIT of an inventoried, never-admitted lifetime is eligible for ordinary validated publication without ADMIT, ABANDON or a terminal disposition, but supplies no discharge" (D40); "An advancing FRONTIER may be published while the startup gap remains open" (§3). No END.
Final: active, latch 0, admitted false, TC none, Disp NONE, no b/x, pending 1, census known, gap open.
Stream `EXIT_D18` → `FRONTIER(18,C0)`. Timeline `OPEN(0)` → EXIT → chk → F18 → chk. No VOID. Reader: startup-unavailable. Diagnostics zero.

## H1d — `AD_D18, TERM_D18, EXIT_D18@L, S(19)`

(1) AD e=18 recorded. (2) TERM x=18: "Equality of enable and EXIT timestamps is not this contradiction" (D36) → recorded matched, unrefuted. (3) EXIT L#1 staged. (4-7) S(19): L W=19: EXIT ✓ → chk → F18 ✓ → chk → C_close=max(5,18)=18, P_close=18 → END(18,0)▲ commits x=18 (no b candidate: no TRANS/MARKER released) and closure.
Final: active, latch 0, admitted true/valid, TC x=18 unrefuted, Disp MATCHED, x=18, pending 0, census known, gap closed 18/18.
Stream `EXIT_D18` → `FRONTIER(18,C0)`. Timeline `OPEN(0)` → EXIT → chk → F18 → chk → `END(18,0)`▲. Prefix before END [OPEN,EXIT,F18]. No VOID; diagnostics zero; reader [50,60) startup-OK.

## H6b — `D20@P0, TERM_D19, RA, EXIT_D18@L, S(25)`

Inputs: (1) D20@P0 P0#1; (2) TERM_D19 naming L#1; (3) REARM(21,C0,fresh A/B proofs); (4) EXIT_D18 L#1 at d=22; (5-8) S(25) P0#2,P1#1,E#1,L#2 at d=22.

| # | Effect | Ep | Lt | TC | Disp | Pend | Faults |
|---|---|---|---|---|---|---|---|
| 1 | staged; D never admitted → numerical record waits for ADMIT/AB (D36); implicit P0 boundary 20 | e0 A | 0 | none | NONE | 1 | 0 |
| 2 | arriving control reconciled against retained D20 during the input (D42); D20 is stamped after claimed time 19 → refutes the match (D40) → FAULT(unknown_evidence). Not the enable/termination contradiction, so D40's MATCHED-recording rule does not apply ("whose only contradiction is the Decision 36 enable/termination contradiction"). Reading taken: rejected on delivery, only refutation history retained (see UNRESOLVED U1). Latch; suspend; D20 payload discarded; VOID20 | e0 S | 1 | none (history x=19 refuted) | NONE | 1 | 1 |
| 3 | REARM accepted; baseline C0; cutoffs 0; retained boundary P0=20 | e1 A | 1 | none | NONE | 1 | 1 |
| 4 | EXIT arrives; reconciliation against the already refuted/rejected claim produces no fault ("including when the claim was rejected on delivery and only its refutation history was retained", D40); "An independently bound lifecycle EXIT is not a newly delivered terminal-control claim"; staged | e1 A | 1 | none | NONE | 1 | 1 |
| 8 | L W=25: EXIT ✓ → chk → FRONTIER(24,C0) ✓ → chk. No disposition and latch set → no discharge, no END | e1 A | 1 | none | NONE | 1 | 1 |

Admitted false; no b/x; census known; gap open throughout.
Stream: `VOID(20,C0,unknown_evidence)` → `EXIT_D18` → `FRONTIER(24,C0)`. Timeline: `OPEN(0)` → VOID20 → (REARM) → EXIT → chk → F24 → chk.
VOID20 entry: prefix [OPEN]; reader startup-unavailable; no revocation. Diagnostics: unknown_evidence 1; D20 disposed (not consumed); abandoned 0; uncovered 0.

## H6c — `TERM_D19, D20@P0, RA, TERM_D21`

| # | Effect | Ep | Lt | TC | Disp | Faults |
|---|---|---|---|---|---|---|
| 1 | matched claim x=19 recorded (no contradicting evidence) | e0 A | 0 | x=19 unrefuted | MATCHED | 0 |
| 2 | D20 (P0#1) stamped after 19 refutes the recorded claim (D40) → FAULT; nothing committed, no O!; latch; suspend; D20 discarded; VOID20 | e0 S | 1 | x=19 refuted | MATCHED(ref) | 1 |
| 3 | REARM accepted (d=22 after) | e1 A | 1 | x=19 refuted | MATCHED(ref) | 1 |
| 4 | conflicts in terminal time with the retained final claim → "reports FAULT(unknown_evidence,d,C), including when the retained claim is refuted and no discharge ever committed. Reject the conflicting control without replacing or restoring the retained claim" (D40). VOID22 | e1 S | 1 | x=19 refuted (retained) | MATCHED(ref) | 2 |

Admitted false; no b/x; pending 1; census known; gap open.
Stream: `VOID(20,C0,unknown_evidence)` → `VOID(22,C0,unknown_evidence)`. Timeline `OPEN(0)` → VOID20 → VOID22. Both VOID entries: prefix [OPEN] (+VOID20 at second); reader startup-unavailable; no revocation. Diagnostics: unknown_evidence 2; D20 disposed.

## T2 — `EXIT_pid30@18@L{no cookie}, S(19), A20@P0, S(21), S(40)`, finish at 50

Inputs: (1) cookie-less EXIT L#1; (2-5) S(19) P0#1,P1#1,E#1,L#2; (6) A20@P0 P0#2; (7-10) S(21) P0#3,P1#2,E#2,L#3; (11-14) S(40) P0#4,P1#3,E#3,L#4; (15) finish d=50.
(1) undecided terminal for PID 30 matching inventory entry D: held until disposition (D40). (5) W=19: batch {EXIT18} held with its frontier ("Pending EXIT evidence holds the affected release batch and its frontier", §4/D40). (6) A20 staged. (10) W=21, (14) W=40: batch {EXIT18, A20} still held — no output. (15) "a capture finished without it reports FAULT(unknown_evidence)" (D40) → latch, suspend, staged EXIT and A20 discarded, VOID50.
State throughout until 15: active, latch 0, admitted false, TC none, Disp NONE, pending 1, census known, gap open. After 15: suspended, latch 1.
Stream: `VOID(50,C0,unknown_evidence)`. Timeline `OPEN(0)` → VOID50. VOID entry prefix [OPEN]; reader startup-unavailable.
Diagnostics: unknown_evidence 1; uncovered 0 (EXIT never consumed); payloads disposed; no nonzero exit.

## R7(a) — `TERM_D18, EXIT_D18@L, S(19), K, RA, S(25)`

Inputs: (1) TERM_D18; (2) EXIT L#1; (3-6) S(19); (7) CHECK(20,Cr); (8) REARM(21,Cr,fresh proofs); (9-12) S(25) P0#2,P1#2,E#2,L#3 at d=22, Cr.
(6) EXIT ✓ → chk → F18 ✓ → chk → END(18,0)▲: x=18, C_close=P_close=18, pending 0.
(7) Cr ≠ C0 → counter_change; latch set; suspend; VOID20[Cr,counter_change]. wait_read_fail is not in the lifecycle-loss class → census stays known (D31). Closure preserved: "Preserve previously committed, unrefuted proofs ...; the latch alone neither reopens a closed startup gap nor changes its endpoint" (D43). No O!.
(8) accepted; e1 active, baseline Cr.
(12) W=25 → FRONTIER(24,Cr) (advances over 18).
Final: e1 active, latch 1, x=18, TC unrefuted, Disp MATCHED, gap closed 18/18, census known, pending 0.
Stream: `EXIT_D18` → `FRONTIER(18,C0)` → `VOID(20,Cr,counter_change)` → `FRONTIER(24,Cr)`. Timeline: `OPEN(0)` → EXIT → chk → F18 → chk → `END(18,0)`▲ → VOID20 → chk → F24 → chk.
VOID20 entry: suspended, latch, x=18 committed, gap closed; prefix [OPEN,EXIT,F18,END(18)]; reader [50,60) startup-OK (ordinary loss at 20 lies outside [50,60)).
Diagnostics: counter_change 1; storage C0→Cr; others zero.

## G8 — census {D,H}: `EXIT_D18@L, S(19), AD_H8, A20@P0{requires x}, H21@P0{final}, S(22), TERM_D18`

Inputs: (1) EXIT_D18 L#1; (2-5) S(19); (6) AD_H8; (7) A20 P0#2; (8) H21 P0#3; (9-12) S(22) P0#4,P1#2,E#2,L#3; (13) TERM_D18.
(5) EXIT ✓ → chk → F18 ✓ → chk; no disposition. (6) H admitted e=8. (7),(8) staged. (12) W=22: batch {A20,H21} — A20's dependency unavailable → batch and frontier held; validation completed first, including the prospective H bound 21, no contradiction; "Prospective bounds used for validation supply no committed proof or permission to close the startup gap" (§3). (13) D's matched claim backed by its previously completed EXIT release: D40's discharge conditions hold ("complete release attempt commits without fault"), so x_D=18 is accepted (reading; see UNRESOLVED U3); closure test: H has no committed bound and "never prospective bounds or readiness state left by a held ... release" (D41) → no END.
Final: active, latch 0, D: admitted false, TC x=18 unrefuted, Disp MATCHED, x_D=18; H: admitted true, b_H none; pending 1 (H)+0; census known; gap open; A20/H21 still staged/held.
Stream: `EXIT_D18` → `FRONTIER(18,C0)`. Timeline `OPEN(0)` → EXIT → chk → F18 → chk. No END, no VOID. Reader startup-unavailable. Diagnostics zero.

## J18 — `J_U4, EXIT_U4@L{no cookie}, S(8)`; writer rejects EXIT_U4 at 20, Cw

Start: census = numeric observation PID 50 (unresolved), pending 0+1, census unknown; no D.
(1) J_U4: J resolution creates inventory lifetime U, applies matched disposition at exit 4 for the named EXIT; numeric observation resolved (pending 1+0, "A number is not counted again", D41); census complete through fence 5 with C0 → known.
(2) EXIT_U4 L#1: disposition present → not held; staged.
(3-6) S(8) P0#1,P1#1,E#1,L#2: L W=8: EXIT_U4 publication rejected at 20 → writer_push_fail; latch; suspend; capture paused; no FRONTIER, no discharge, no END. "A rejected EXIT publication likewise leaves the lifetime pending and the startup gap open" (Clarification).
Final: suspended, latch, U: Disp MATCHED (unrefuted, no discharge), x none, pending 1+0, census known (writer_push_fail not lifecycle class), gap open.
Stream: `VOID(20,Cw,writer_push_fail)`. Rejected: `EXIT_U4` DATA at 20. Timeline `OPEN(0)` → [EXIT_U4 rejected] → VOID20. Prefixes: [OPEN] before failure and at VOID entry; no END.
VOID entry: reader startup-unavailable. Diagnostics: writer_push_fail 1; C0→Cw; uncovered 0; rejected EXIT not consumed.

## V4-post-FRONTIER-counter

Inputs: (1) AB_D; (2) TERM_D18; (3) EXIT L#1; (4-7) S(19) P0#1,P1#1,E#1,L#2; at L: EXIT ✓, chk C0, F18 ✓ (C0), chk observes Ce at 20; (8) REARM(21,Ce,fresh A/B proofs); (9) TERM_D18 d=22; (10-13) S(20) P0#2,P1#2,E#2,L#3 at d=22, Ce.
(1)-(3) as V4. (7): post-FRONTIER checkpoint observes Ce ≠ C0 → counter_change before END: latch set, suspend, no discharge, no END ("An attempt that fails publishes no END and commits none of its candidate ... discharges or closure; successful earlier DATA and FRONTIER publications remain observable", D41). VOID20[Ce,counter_change]. Census known (event_ring_drop not lifecycle class).
(8) accepted, e1 active, baseline Ce. (9) identical duplicate: idempotent. (13) W=20 → FRONTIER(19,Ce) (19 > published 18). No discharge (EXIT output in a VOIDed attempt, Clarification; latch).
Final: e1 active, latch 1, abandoned, admitted false, TC x=18 unrefuted, Disp MATCHED, no x, pending 1, census known, gap open.
Stream: `EXIT_D18` → `FRONTIER(18,C0)` → `VOID(20,Ce,counter_change)` → `FRONTIER(19,Ce)`. Rejected: none.
Timeline: `OPEN(0)` → EXIT → chk(C0) → F18 → chk(Ce ✗) → VOID20 → (REARM) → chk → F19 → chk.
Prefixes: [OPEN]; [OPEN,EXIT]; [OPEN,EXIT,F18] = last before failure = VOID-entry prefix. No END anywhere; EXIT and F18 are observable but in the uncertified suffix (§3).
VOID20 entry: suspended, latch, candidates uncommitted, pending 1, census known, gap open under existing OPEN; reader startup-unavailable.
Diagnostics: counter_change 1; storage C0→Ce; abandoned_records 0; uncovered 0.

---

## Mutants (design only; none built or executed)

- H11d: "defer the enable/termination contradiction to EXIT arrival" — distinguishing input TERM_D18 (input 2): contract has VOID20 and accessor MATCHED after input 2; mutant has neither until input 3. Also "report again at EXIT": total unknown_evidence must be 1.
- H11e: "record the separately false TERM_D17 as the retained final claim" — at TERM_D18 mutant raises a third fault (conflict) and accessor stays NONE/claim 17; contract: total faults 2, retained claim x=18 refuted, accessor MATCHED.
- H11f: "missing-disposition check ignores refuted MATCHED" — at finish (d=40) mutant emits FAULT/VOID40; contract: total faults 1, no VOID40, accessor MATCHED.
- END-before-FRONTIER: V4 — mutant publishes END(18) before F18; VOID-entry prefix would contain END; contract prefix [OPEN,EXIT_D18].
- END-before-post-FRONTIER-checkpoint: V4-post-FRONTIER-counter — mutant publishes END before the Ce checkpoint; VOID-entry prefix would be [OPEN,EXIT,F18,END(18)]; contract has no END.
- Suppression of a published FRONTIER after later END rejection: N14 — mutant drops/retracts F7 (or emits a rollback OPEN) after END(6) is rejected; contract: successful stream contains FRONTIER(7,C0) and the VOID-entry prefix is [OPEN,F7] with no rollback OPEN.

## UNRESOLVED

- U1 (H6b, input 2): the contract says a terminal claim contradicted by retained attributable evidence is refuted and faults (D40), and refers to a claim "rejected on delivery and only its refutation history was retained", but does not state explicitly that a matched control contradicted on delivery by an earlier TRANS is rejected rather than recorded-and-refuted. Readings: (i) rejected — no disposition recorded, accessor NONE (taken above, consistent with D36's treatment of contradicted ADMITs and D43 "rejected for its own invalid facts"); (ii) recorded as final MATCHED disposition with claim refuted — accessor MATCHED(ref). Outputs, faults and counts are identical under both readings; only the accessor/claim state differs.
- U2 (H10a, input 7): after a rejected TERMU revokes the discharge of the retained MATCHED claim, the contract says the retained disposition is not replaced and the merge "does not judge which of the conflicting claims is false"; it does not state whether the retained claim is marked refuted. Readings: (i) claim retained and marked contradicted/refuted; (ii) retained unrefuted but supporting no discharge (latch prevents re-commitment anyway). Outputs identical.
- U3 (G8, input 13): the contract does not say explicitly whether an accepted discharge commits at a later TERM control when the closure test fails (no END). Readings: (i) x_D=18 commits at TERM because D40's conditions (successful complete EXIT release, unrefuted matched disposition) hold (taken above); (ii) x_D stays a candidate until a closing END commits it. Outputs identical (no END, gap open, H pending); only the reported committed x_D differs.
