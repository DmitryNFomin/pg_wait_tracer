# Comparison of the independent answer sets with EXPECTED: merge-v1 delta 8

## Where this delta came from (2026-11-21)

Delta 7 fixed the order within one release attempt as supporting records, END, then the advancing FRONTIER, and added a plain startup OPEN to be persisted when an attempt failed after its END. The code review of the implementation that followed measured, with the real writer and reader, that a file cut between that END and the rollback OPEN reads complete for windows the capture never committed; the reader pairs an OPEN with an END and has no rule that an END needs a following record, and the rollback OPEN is itself a write that can fail. Both design advisers agreed this is a false completeness claim and that the fix is the order the implementation had before delta 7: END is the last write of the attempt and is the commitment. The same review and a third exhaustive enumeration (2,242,221 short schedules) left one arrival order undecided (a late-dated ADMIT, then a matched terminal control, then the EXIT), one product defect that needed a pinned row (a separately false terminal control after a reported contradiction keeps its own fault), and one cookie-less disposition question; delta 8 decides them (rows H11d, H11e, H11f).

A limit that no write order removes is now stated in the contract: revoking an already committed closure is a retraction write, and a write failure or process death before it is persisted can leave the old END readable.

## Comparison

# Delta 8 comparison: set 1 (answers-sol.md) vs set 2 (opus/answers-opus.md) vs EXPECTED

Normalisations applied (not counted as disagreements):
- Set 1 draws kappa(C) checkpoints in timelines; set 2 writes `chk`. Neither treats them as records. Set 1 writes `O`, set 2 `OPEN(0)`; `END(p,0)` is identical.
- Set 2's state label `MATCHED(ref)` = set 1's "accessor MATCHED, terminal claim refuted". Both distinguish no recorded disposition (NONE/none) from MATCHED with refuted proof.
- Set 1 names storage components (writer_push_fail=1, wait_read_fail=1, event_ring_drop=1). Set 2 gives "C0 -> Cw/Cr/Ce" and names the component only for Cw. CONTRACT line 229 defines Ce as event_ring_drop=1, so the two are the same observation.
- EXPECTED writes `G(c)`, `P=1+0`, `L`; mapped to startup gap closed at c / open, pending 1+0, latch.
- Set 1 prints states as per-input tables with only changed fields; set 2 prints some rows only at the inputs that change state. Unchanged-state inputs were not treated as omissions.

## 1. Table

| Execution | Set 1 vs EXPECTED | Set 2 vs EXPECTED | Set 1 vs set 2 | Note |
|---|---|---|---|---|
| V4 | AGREE (EXPECTED silent: accessor value, prefixes) | AGREE (same silences) | AGREE | No END, no O!; F18 rejected; F19[Cw] after REARM |
| N14 amended | AGREE (EXPECTED silent: pointer-history detail) | AGREE | AGREE | F7 kept; END(6,0) rejected; prefix O,F7 |
| H11d | AGREE | AGREE | AGREE | MATCHED observable at TERM (input 2) |
| H11e | AGREE | AGREE | AGREE | Accessor NONE at both VOIDs, MATCHED(18, refuted) at end; 2 faults |
| H11f | AGREE | AGREE | AGREE | After TERM: MATCHED, 1 fault; finish adds none |
| G5 | AGREE | AGREE | AGREE | No repeated F18; END(18,0) at TERM |
| G7 | AGREE | AGREE | AGREE | Later ADMIT/CONFIRM commit no bound |
| H1a | AGREE | AGREE | AGREE | END18, then O!, then VOID; prefix at VOID entry contains O! |
| H8a | AGREE | AGREE | AGREE | END6, O!, VOID; persistence limit explained identically |
| H10a | AGREE with stated outputs; EXPECTED silent on whether retained MATCHED claim is refuted | DIFFERS(retained terminal claim status): opus offers two readings in U2 and does not choose; EXPECTED silent | DIFFERS(terminal claim refuted vs unrefuted-or-refuted left open) | Stream, timeline, faults, O!, x revoked all agree |
| H11b | AGREE | AGREE | AGREE | D20 adds no fault, no storage change |
| H11a | AGREE | AGREE | AGREE | Accessor NONE at VOID entry, MATCHED after TERM |
| R11 | AGREE | AGREE | AGREE | EXIT + advancing F18 with open startup gap |
| H1d | AGREE | AGREE | AGREE | Equality is not a contradiction; END18 |
| H6b | AGREE (EXPECTED silent: accessor, REARM-time details) | AGREE, with note: opus U1 flags "rejected vs recorded-and-refuted" but states reading (i) "taken above" | AGREE (note) | Stream VOID20, EXIT_D18, F24 in both |
| H6c | AGREE | AGREE | AGREE | VOID20 then VOID22; claim19 stays refuted |
| T2 | AGREE | AGREE | AGREE | Single VOID50 at finish |
| R7(a) | AGREE (EXPECTED silent: storage component, reader result) | AGREE | AGREE | END18 retained, no O!, F24[Cr] after REARM |
| G8 | AGREE | AGREE, with note: opus U3 flags "x_D committed at TERM vs candidate" but states reading (i) "taken above" | AGREE (note) | No END; x_D=18; H unresolved |
| J18 | AGREE | AGREE | AGREE | EXIT_U4 rejected; no F, no END |
| V4-post-FRONTIER-counter | EXPECTED-SILENT (all observables; only the mutant-table row names this execution) | EXPECTED-SILENT | AGREE | EXIT, F18 succeed; checkpoint Ce before END; no END anywhere |
| G7-cut-after-END | EXPECTED-SILENT (all observables) | EXPECTED-SILENT | AGREE | Both: x_D=18 and closure committed, END not provisional |

## 2. DIFFERS

### H10a: retained terminal claim after the rejected unresolved control

Observable: whether the retained final MATCHED claim for D is marked refuted after TERMINAL_UNRESOLVED(cookie 0) is rejected and the discharge x=18 is revoked.

- Set 1: "Conflicts with final MATCHED and its committed discharge. Reject unresolved control; retain MATCHED, refute its proof and revoke x/closure." At VOID entry: "final disposition remains MATCHED, terminal proof refuted". Cites no contract sentence in the answer.
- Set 2: table row 7, terminal-claim column: "x=18 retained (support revoked; see UNRESOLVED U2)". U2: "the contract says the retained disposition is not replaced and the merge 'does not judge which of the conflicting claims is false'; it does not state whether the retained claim is marked refuted. Readings: (i) claim retained and marked contradicted/refuted; (ii) retained unrefuted but supporting no discharge." Neither reading is declared as taken (unlike U1 and U3). Cited sentence: D40, "An unresolved disposition for the same EXIT after a matched disposition has supplied a committed discharge likewise faults and revokes that discharge. Neither conflicting control replaces the retained final disposition or authorizes another consumption of the EXIT." (CONTRACT.md D40).
- EXPECTED H10a: "Reject the cookie-zero unresolved control, retain the historical MATCHED disposition, remove x and persist O! before Vu20." Silent on whether the claim is refuted.

Everything else in H10a agrees across all three: stream `EXIT_D18 -> F18 -> VOID20[C0,unknown_evidence]`; timeline `O -> EXIT -> F18 -> END(18,0) -> O! -> VOID`; END endpoint 18 reserved 0; x revoked, historical closure (18,18); census known; accessor MATCHED; unknown_evidence +1; uncovered 0; no storage change; prefix at VOID entry ends in O!.

## 3. U1-U3 of set 2

| Point | Executions touched | Set 1 | EXPECTED | Any stream, fault count or published record differs? |
|---|---|---|---|---|
| U1: H6b input 2, matched control contradicted on delivery by retained D20: rejected (accessor NONE) or recorded-and-refuted (accessor MATCHED) | H6b only (H6c's control is recorded before D20 arrives, so is not affected) | Concluded "Reject control before recording a final terminal claim; retain refutation history"; accessor none. Cited sentence: none quoted in the answer; closing remark "The enable/termination MATCHED exception does not apply: the rejected control had a different established contradiction, D20 after terminal19." | H6b: "The first fault occurs at TERM_D19, which retained D20 already contradicts; no accepted terminal disposition or discharge results." Mutant table: "Record a terminal claim already contradicted on delivery as an accepted proof". Consistent with reading (i); accessor value not stated | No. Stream, fault total 1, EXIT18/F24 publication, no O!: identical under both readings and in both sets. Set 2 itself says only the accessor/claim state differs, and chose reading (i), which is set 1's |
| U2: H10a input 7, retained claim refuted or not after rejected TERMU | H10a only (G4b, H10b are not in this task) | Refuted ("terminal proof refuted") | Silent on refuted status; says "retain the historical MATCHED disposition" | No. Stream, faults, O!, x revocation, consumption are identical. Only the claim's refuted/unrefuted flag differs (see section 2) |
| U3: G8 input 13, whether x_D=18 commits at TERM when closure test fails | G8 only (G5, G7 close with END, so x commits there) | x_D=18 recorded at TERM ("Completed earlier EXIT release supports x_D=18"), no END | "TERM commits only x_D=18" | No. Stream `EXIT_D18 -> F18`, no END, gap open, H pending: identical. Set 2 chose reading (i), which matches set 1 and EXPECTED |

## 4. Counts (22 executions: the 20 listed plus the two publication-boundary executions)

- All three agree on everything stated (including executions where EXPECTED is silent on some observables, and the two noted choices in H6b and G8): 21. Of these, V4-post-FRONTIER-counter and G7-cut-after-END are wholly EXPECTED-SILENT, so for them only the two answer sets are compared with each other.
- Stream and timeline agree, another observable differs: 1 (H10a, retained terminal claim refuted status).
- Stream or timeline differs: 0.

Specific timeline checks that all three (or both sets where EXPECTED is silent) agree on:
- END position: after F18 and after the post-FRONTIER checkpoint in G5, G7, H1a, H1d, H10a, H11b, H8a (END6 after F18), R7(a), and the G7-cut prefix; absent in V4, V4-post-FRONTIER-counter, J18, G8, R11, T2, N14 (END rejected, not published).
- No END or extra OPEN in a failed attempt: V4, N14, V4-post-FRONTIER-counter, J18. Revoked O! appears only after a committed-and-revoked closure (H1a, H8a, H10a, H11b), before VOID.
- Additional plain OPEN: none in any execution.
- Fault counts per reason: writer_push_fail 1 (V4, N14, J18); counter_change 1 (R7(a), V4-post); unknown_evidence 1 (H11a/d/f, H1a, H8a, H10a, H11b, H6b, T2), 2 (H11e, H6c).

## Left as a note

One reader marks the retained MATCHED claim of H10a as refuted after the conflicting unresolved control; the other leaves the label open; EXPECTED says only that the historical MATCHED disposition is retained. No stream, timeline, fault count, revocation or published record depends on it.

Final count for delta 8: 3 new rows (H11d, H11e, H11f), 183 rows in all; V4, N14, the H-row order clause, H11b's timeline and the ordering mutants rewritten for END-last.
