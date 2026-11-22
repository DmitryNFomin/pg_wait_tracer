# Design question to an adviser (RULE 1 of CLAUDE.md): what the merge contract should say in seven places the round-10 review and the exhaustive enumeration found undecided, in the smallest form

This is a written design question from the lead to an adviser, one question, one answer. It is not an implementation brief; scripts/lead/check-brief.sh does not apply; do not run it. Read-only: edit no file except your answer if told to write one. At most 45 lines. Mark every claim VERIFIED (file:line read, or command and output) or INFERRED. scratchpad = SP

## Material
- The contract: contracts/merge-v1/CONTRACT.md section 6 and contracts/merge-v1/oracle-discovery/{ROWS.md,EXPECTED.md} on origin/master (6f18da28; 149 rows, DECISIONs 30-44). `git show origin/master:<path>`.
- The implementation under review: commit b5ae3fb6, worktree scratchpad/wt-m10 (src/core/merge.c, src/merge_capture.c).
- The code review: scratchpad/a2go/disc/review-merge-b5ae3fb6.md (blocker B1, should-fix S1, S2, design question Q1, the section on the 270 schedules).
- The triage of the enumeration's differences (1,726,074 schedules; eight causes): scratchpad/a2go/disc/m10/triage.md (causes T1, T2, P1, P2, G1-G4, each with its shortest schedule, the contract lines for each reading and the code lines).

## What is not in question
B1 (an unresolved disposition must revoke a committed matched discharge whatever its cookie), P1 (ABANDON after an ADMIT recorded under the latch must fault), S1 (missing test), S2 (the own-EXIT provenance rule must hold in both arrival orders) go to the next implementation round as they stand. Say so only if you think one of them is wrong.

## Decide each, with the sentence you would add or change and the row you would pin (or "no contract change")
1. **Q1.** A valid-format ADMIT whose enable time is later than the EXIT that backs an already committed discharge of that lifetime: fault only, or fault and revoke the discharge?
2. **P2.** A route-(c) ADMIT that is itself refuted by an earlier numerical record of the same lifetime: is it recorded before it faults (so that a later TRY or pointer fire faults again), or is a faulting ADMIT not recorded? (delta 6 left "whether a faulting ADMIT is itself recorded" as a note.)
3. **G1.** A TRY_NO_WAIT_WORD after an ADMIT "faults and is not recorded" and is "evidence against the address proof". Do later controls of that lifetime (a quiet candidate, another ADMIT) then fault because of it?
4. **G2.** A fault drops a pending quiet candidate (merge.c:259-263), so a later record that would have contradicted it reports nothing, while the same candidate delivered after the fault is kept and does fault. Which is the rule: a candidate pending at a fault is kept as evidence, or no candidate is retained under the latch in either order?
5. **G3.** Several observations contradicting one terminal claim: one fault per refuted claim, or one per contradicting observation?
6. **G4.** A batch held on a missing query text: is it validated while held (a contradiction in it faults now), or only when released?
7. **T1 and T2, the public views.** (a) The capture has two per-lifetime facts, "an ADMIT was recorded" and "an ADMIT was accepted while certification was still possible"; the public accessor state exposes the second under the name `admitted` and the coordinator and the test read it. What should the accessor promise? (b) Inside the VOID callback of a revoking fault the status view still reads closed with the bound already revoked; it is consistent after the call returns. Should the product make the views consistent before it emits VOID, or should the contract and header say views are defined only between primitive inputs (and the coordinator and the test must not read them from a sink)?

## And one question about form
Items 3, 4 and 5, and most of the schedules behind them, differ only in how many FURTHER fault reports appear after certification is already latched off for the capture (no bound, discharge or END can follow). Every earlier amendment pinned such counts row by row, and each round of pinning has cost about half a day (delta, two independent answer sets, comparison, contract PR, implementer round, enumeration round). Is there a smaller rule that is still decidable by two independent readers: for example "after the first VOID of a capture, each control or observation that contradicts recorded evidence reports at most one unknown_evidence fault and the count of such further reports is not normative; what is normative is the latch, the absence of any later bound, discharge, END or closure, and the retained evidence", with the enumeration comparing only those? State what would be lost (a real defect that only an exact post-latch count would catch), and whether G4 (a fault-free schedule) and item 2 are outside such a rule and still need a pinned row.

## Finally
The order of work you recommend: one more contract delta first and then implementation round 11, or round 11 now on the undisputed items (B1, P1, S1, S2) with the contract items following.
