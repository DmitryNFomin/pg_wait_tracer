# Comparison of the independent answer sets with EXPECTED: merge-v1 delta 9

## Where this delta came from (2026-11-21)

The fourth exhaustive enumeration (2,242,221 short schedules) run against the twelfth merge implementation round passed every pinned row and every model-free invariant and left one family of 1,992 schedules, all with the order "EXIT, a terminal control contradicted by that EXIT, then a qualified terminal control", with no ADMIT. The implementation recorded the contradicted control as the retained refuted claim and faulted the later qualified one as a conflict; the reference model rejected the first and accepted the second. One answerer had flagged this point as unresolved in delta 8 and its comparison had recorded it as making no difference to any output, which holds only until a later qualified control arrives. Both design advisers chose rejection on delivery; the implementation was also inconsistent with itself (with a cookie-less EXIT it recorded a match dated 19 for an EXIT stamped 18). The code review of the same round added three points decided here: a terminal control delivered while another batch is held commits its own discharge (row H13 pins what the implementation already does); an EXIT disposed during suspension is not an unfinished record (row H11g); and the END publisher reports failure only when no complete END is reader-visible.

## Comparison (11 executions)

# Comparison: answers-sol (set 1), answers-opus (set 2), EXPECTED, merge-v1 delta 9

Normalisation: V20/VOID20/Vu20 are the same VOID(20,C0,unknown_evidence); O/OPEN the startup OPEN; END21 = GAP_CLOSE endpoint 21 reserved 0;
sol's "A/0, S/1" = opus's "active/clear, suspended/set"; sol's per-input tables and opus's grouped S(w) rows (5a-5d etc.) are the same expanded
inputs; sol's Q and opus's Rej-hist are the same rejected-control history; "M(t,D)" = MATCHED(t,D). Neither set offered an unchosen alternative.
Opus's UNRESOLVED notes (H12c finish fault published as VOID40; H13 x_H commits at TERM) are chosen readings, so they count as agreement;
both match EXPECTED (VOID40; x_H=18 at TERM_H18) and sol. Both sets also agree with each other on the remaining items sol/opus
state differently only in presentation (e.g. H12d REARM proof wording; EXPECTED silent).
EXPECTED is silent (all rows) on: per-input pending payload counts, per-input fault totals for rows other than the stated "exactly N", the
census/latch after every input for most rows, and Decision-41 prose; where both sets give these they agree with each other.
H6b/H6c: EXPECTED states only the stream and the faults (H6b also accessor NONE throughout, VOID-entry state); H6c accessor/state per input is EXPECTED-SILENT; sets agree with each other.

## 1. Table

| Execution | Set 1 vs EXPECTED | Set 2 vs EXPECTED | Set 1 vs Set 2 | Note |
|---|---|---|---|---|
| H11g | AGREE (EXPECTED-SILENT: payload per input beyond "zero after TERM", fault total per input) | AGREE (same silences) | AGREE | V20 -> F25; accessor NONE,NONE,M18,M18,...; unfinished 0 |
| H12a | AGREE (EXPECTED-SILENT: per-input pending) | AGREE | AGREE | V20; NONE,NONE,M18 unrefuted |
| H12b | AGREE (EXPECTED-SILENT: fault total per input) | AGREE | AGREE | V20; finish adds nothing |
| H12c | AGREE | AGREE | AGREE | V20 -> V40; fault total 2 |
| H12d | AGREE | AGREE | AGREE | V20; NONE,NONE,NONE,M21 |
| H12e | AGREE | AGREE | AGREE | V20; both retransmissions silent |
| H13 | DIFFERS (prior_interval_certified marking of D20 in the merge stream) | AGREE (EXPECTED-SILENT: per-input payload counts) | DIFFERS (same observable) | sol: D20 true ([T]); opus and EXPECTED: false ([U]) |
| H6b | AGREE (EXPECTED-SILENT: per-input payload, per-input state) | AGREE | AGREE | V20 -> EXIT_D18 -> F24 |
| H6c | AGREE (EXPECTED-SILENT: accessor, pending, VOID-entry state) | AGREE (same silences) | AGREE | V20 -> V22 |
| H11e | AGREE | AGREE | AGREE | V20 -> V20; 2 faults; M18 refuted at end |
| H11f | AGREE | AGREE | AGREE | V20; finish adds nothing |

Everything else in H13 (END21 endpoint 21 reserved 0, C_close=20, P_close=21, x_H=18 at TERM_H18, b_D=20 at END, accessor, pending 2/2/1/0, prefixes before/after TERM,
AD and END, reader restriction [0,21), F18/F24/F29, no faults, latch 0, [50,60) eligible after END) is agreed by all three.

## 2. Disagreement detail

### H13: prior_interval_certified of D20 (merge stream)

- EXPECTED (line 228 and notation line 9): merge stream `EXIT_H18 -> F18 -> D20[U] -> F24 -> F29`; `[U]` = `prior_interval_certified=false`; timeline `... D20[U] -> F24 -> END21 -> F29`.
- Set 1 (sol): stream `EXIT_H18 -> F18 -> D20[prior_interval_certified=true] -> F24 -> F29`. Text: "The output's existing `prior_interval_certified` boolean is **true** under the contract's rule for generations not admitted through 25b; no discovery storage flag is added." The same answer also says "Its discovery incoming interval is uncertified and excluded through the persisted startup gap."
- Set 2 (opus): stream `EXIT_H18 -> F18 -> TRANS_D20[prior_interval_certified=false] -> F24 -> F29`; timeline `D20[U]`; "validation sees D20 as the first released TRANS of the final attachment -> route (a), b_D=20, [U]"; later "D20 is an uncertified fresh-record anchor at exactly C_close ... P_close=21 (D42)".
- Observable: the output's `prior_interval_certified` for D20 (and the persisted-prefix annotation). Set 1 also, internally, calls D20 an uncertified fresh-record anchor for P_close=21, so its marking and its P_close reasoning pull in different directions; this is reported, not resolved.
- Contract sentences cited:
  - Set 1: CONTRACT.md (line 125, delta by lead 2026-11-13): "Every TRANS/MARKER of a generation not admitted through 25b (an attached generation, or one created by FORK+INIT under 25) carries `prior_interval_certified=true`...; only a 25b generation's first released TRANS/MARKER carries false." Set 1 applied the first clause.
  - Set 2 / EXPECTED: CONTRACT.md line 119: "For a 25b generation, the first successfully released TRANS/MARKER carries `prior_interval_certified=false`..." applied to D20 as the first released TRANS of D's final startup attachment admitted at AD_D6 (ROWS H13: "D20 belongs to D's final startup attachment, whose otherwise qualified ADMIT is delivered only at AD_D6").
- Not affected: the stream order, all counts, END21 endpoint/reserved, and every other observable.

No other observable differs between the two answer sets or against EXPECTED in any of the other ten executions.

## 3. Counts (11 executions)

- All three agree on everything stated: 10 (H11g, H12a, H12b, H12c, H12d, H12e, H6b, H6c, H11e, H11f).
- Stream/timeline agrees but another observable differs: 0.
- Stream or timeline differs: 1 (H13: set 1's D20 marking [T] vs [U]; set 2 agrees with EXPECTED).

## Settlement 9b

The one difference, the certification mark of D20 in H13, exposed a real conflict in the text: an older sentence gave every record of a generation not admitted through Decision 25b a certified prior interval, while discovery is a distinct admission route whose first released final-attachment record is uncertified (Decision 37(a)). The design author replaced that sentence; no expected output changed (the marks of D01, D02c, D03, D09, D10, D14a-c, Q02-Q06, Q09, A04, N13, J13, R1, R7(b), R8(a), U12, H2a, H2b and the certified marks of D02a, Q01, Q07, A07 stand). Two fresh answerers then derived the marks of H13, D03, D02a and Q01 from the clarified text: both give D20 false in H13, D12 false and D15 true in D03, D15 true in D02a and in Q01, with nothing undecided, which is what EXPECTED says.

Final count for delta 9: 7 new rows (H11g, H12a-H12e, H13), 190 rows in all.
