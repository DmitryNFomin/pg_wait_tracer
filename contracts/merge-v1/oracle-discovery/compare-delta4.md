# Oracle5 comparison: answers-sol (A) vs answers-opus (B) vs EXPECTED, rows R1-R12, N1, N10, N14, P1, P2, P3

Counts: ALL-AGREE 18, ANSWERS-AGREE-EXPECTED-DIFFERS 0, ANSWERS-DIFFER 0, ANSWERS-AGREE-EXPECTED-INCOMPLETE 0, UNDETERMINED 0 (18 rows). Neither answerer wrote UNDETERMINED.

Outcomes compared, per row and per parameter combination (R2: p x 2 orders x {no K, K before pair, K between}; R3: h x q x {D12, PTR non-NULL, continuity break}; R4: h x q; R5: h x p x q; R8: r x {a,b,c}; R9: r x {a,b}; R1/R10: both variants): released records and [U]/[T] marks and order, FRONTIERs, VOID/fault with detection input, counter vector and reason, b and x, startup-gap state and persisted end, revoked startup OPEN (O!), P=L+Q, census, latch, abandoned, uncovered, epoch invalidation, and for N1/N10/P1/P3 listing/counting/contributions.

Conventions reconciled (not differences):
- B writes the startup gap publication inline (`EXIT_D18 → GAP_CLOSE(18) → F18`, `GAP_CLOSE(6) → F7`); A states "gap closes with PERSISTED end n"; EXPECTED writes `G(n)` and says "Gap publications are separate from merge DATA/FRONTIER output". Same outcome.
- B's `FR(24,Cr)` = A's `FRONTIER(24,Cr)` = EXPECTED's `F24[Cr]`. B's `revOPEN` = A's "persisted revoked startup OPEN" = EXPECTED's `O!`.
- Where EXPECTED is silent on the latch (N1, N10, N14, P1, P2, P3) or on O! wording (P2), the contract determines it from EXPECTED's stated outputs, so these are not counted as EXPECTED-INCOMPLETE: DECISION 43 "the first fault requiring VOID permanently latches discovery certification off" (latch set iff a VOID occurs: N14, P2 set; N1, N10, P1, P3 not set, as both answers say), and DECISION 43 "revokes the affected proof and dependent base coverage from boot_start, invalidates affected cached answers, persists a revoked startup OPEN" (P2: EXPECTED "revoke b_D and coverage from boot"; A and B both state the persisted revoked OPEN).

| ID | class | difference |
|---|---|---|
| R1 | ALL-AGREE | none. Both variants: `D14[U] → F14 → VOID20[C0,unknown_evidence]` at B_D6; b, x none; G open; P=1+0; latch set; no O!. |
| R2 | ALL-AGREE | none. All 18 executions: no K `Vu`; K `VOID20[Cr,counter_change] → Vu`; no DATA/FRONTIER; abandoned=0 incl. p=AB_D; no O!. A and B both add detection at the pair's second member (EXPECTED silent on which input; no contradiction). |
| R3 | ALL-AGREE | none. `F7 → Vq → Vu`; G(6) iff h=0 before contradiction; one O! also for h=1; P=(1+h)+0; census known for D12, unknown for pointer/break. |
| R4 | ALL-AGREE | none. `F13 → Vq → Vu`; historical G(12) iff h=0; one O!; census known. |
| R5 | ALL-AGREE | none. `EXIT_D18 → F18 → Vq → Vu`; historical G(18) iff h=0; one O!; abandoned=0 incl. p=AB_D. |
| R6 | ALL-AGREE | none. `EXIT_D18 → F20 → Vu`; abandoned=1; no O!. |
| R7 | ALL-AGREE | none. (a) x=18, G(18) retained, `... → F24[Cr]`; (b) `D12[U] → F12 → VOID20[Cr,counter_change] → F24[Cr]`, b=12, G(13) retained; no O!. |
| R8 | ALL-AGREE | none. Without RA only the K VOID; with RA (a) `F24[Cr] → D26[U] → F26[Cr]`, (b,c) `F24[Cr]`; no b ever. B flags an assumption (see below), changes no output. |
| R9 | ALL-AGREE | none. (a) `EXIT_D18 → F18 → VOID`, +F24[Cr] iff RA; (b) VOID, +`EXIT_D26 → F26[Cr]` iff RA; no x; no TERM fault. |
| R10 | ALL-AGREE | none. Two `VOID20[C0,unknown_evidence]` (PTR9/break, then CN6); census known; no O!. A alone adds attempts=0 (no contradiction). |
| R11 | ALL-AGREE | none. `EXIT_D18 → F18`; latch not set; no fault; uncovered 0 (A explicit, B by its stated default). |
| R12 | ALL-AGREE | none. `EXIT_D18 → F18 → VOID20[C0,seal_violation]`; epoch invalidated; x revoked; historical END 18 then O!; no extra unknown_evidence fault (A explicit; B: "12<18 does not contradict the EXIT timestamp"). |
| N1 | ALL-AGREE | none. `F5 → F7`; no bound at S(6); b=6, G(6), P=0 at S(8); attempts 0; not listed/counted; no role-support publication. |
| N10 | ALL-AGREE | none. Same as N1 with stats-collector role 12. |
| N14 | ALL-AGREE | none. `VOID20[Cw,writer_push_fail]` only; no b, no F7; G open; P=1+0; census known. |
| P1 | ALL-AGREE | none. N1's output; absence of publisher holds nothing, no writer failure. |
| P2 | ALL-AGREE | none. `F5 → F7 → VOID20[C0,unknown_evidence]`; b revoked, O!; G open; P=1+0; census unknown; S(14) no output. |
| P3 | ALL-AGREE | none. `F5 → F7 → EXIT_D12 → F12`; persisted G(6) unchanged (endpoint 6, reserved 0); b=6 kept; not listed/counted; no N/U/numerical contribution. |

## Non-agreeing rows

None.

## Assumptions flagged by an answerer

Set A flagged none (no "assumption" text in answers-sol.md).

Set B flagged one:

1. R8 (a, r=RA): "RA's 'fresh attachment_proofs' cover D's pre-fault admitted attachment, so D26 releases ordinarily as the first final-attachment record ([U]) under §3/D37(a)." Changes no output: A gives the same `F24[Cr] → D26[U] → F26[Cr]` unflagged, and EXPECTED agrees. Bearing text: ROWS "`RA=REARM(21,current-vector,fresh attachment_proofs)`"; CONTRACT §4 (DECISION 27a) "A FRONTIER published after REARM certifies completeness only from the REARM's fresh attachment proofs onward"; DECISION 43 "Ordinary release, fault reporting, abandonment consumption and explicit REARM retain their existing rules" and "A refused commitment cannot certify a numerical interval". The contract is silent on whether a REARM's attachment proofs include a discovery lifetime's pre-fault final attachment; the row text "fresh attachment_proofs" supplies it. One clarifying sentence in the row would settle it: "RA's fresh attachment proofs include D's final attachment α2 from AD_D6."

## Contract or row text that needs a clarifying sentence

- Row R8: the sentence above (no output depends on it in either answer set).
- Rows R2 (and R3-R5 VOID timing): EXPECTED gives the VOID sequence but not the detecting input; both answer sets name the second pair member (R2) or the contradicting input. Optional: state the detecting input in EXPECTED, since DECISION 40 already says "Report FAULT(unknown_evidence,d,C) when the contradiction is established".
