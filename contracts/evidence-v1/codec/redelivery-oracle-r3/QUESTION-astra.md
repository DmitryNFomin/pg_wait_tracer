## Task

Read the issue bodies: `gh issue view 913 --json body --jq .body`, and the same for 914. If you have no network, read
them from issue-crossboot.md and issue-baseline.md in
/private/tmp/claude-501/-Users-dmitryfomin-work-git-pg-wait-tracer/27cc85b4-040d-47d2-ba40-e0c59ac54ec1/scratchpad/storage/.
Also read the adviser answers in that folder: astra-ref-ambig.md (yours) and fable-ref-ambig.md (Fable's). The
reference evaluator and its REPORT.md are in redeliv-ref/ there. Contracts: `git show
origin/master:contracts/evidence-v1/{CONTRACT.md,REVISION-r3.md,codec/CODEC.md,codec/REDELIVERY.md}` (origin/master
9b20b7f1).

Draft, as text in your answer (do not edit files):
1. **#913:** the normative text for a capture id seen under more than one boot in an unordered multiset. Cover:
   - its outcome name and where it sits relative to the closed codec reasons;
   - what records survive, for that capture id and for others;
   - witnesses;
   - the "failed decode, not an empty success" signalling;
   - what happens when only a non-root file carries the other boot.
2. **#914:** the corrected CODEC §2/§8 text and the REDELIVERY clause 13-15/19 changes: baseline leaves the static
   link fields and becomes a replay check against the running baseline at the successor descriptor. Say exactly which
   lines change.
3. **REDELIVERY.md header (review nit N1 from the merged REDELIVERY contract PR):** replace "Independent answer sets are pending" (lines 4-5)
   and "proposed requirements" (line 78) with the final state.
4. **New cases** continuing the numbering (32 and up), in the existing table format and notation, with expected
   values:
   - at least 4 for #913: two roots; a non-root under a second boot; an unrelated capture alongside; a duplicate of
     the reused file;
   - at least 4 for #914: a baseline-changing VOID then a successor with the running baseline; the same with the
     original baseline; a successor with no prior VOID; a redelivered predecessor.
   Define any new fixture in the notation style.
5. Confirm that cases 1-31 keep their expected values, or name any that change and why.
Mark claims about existing text [Verified] file:line. At most 160 lines.

## Task

Your draft: astra-redelivery-r3.md in
/private/tmp/claude-501/-Users-dmitryfomin-work-git-pg-wait-tracer/27cc85b4-040d-47d2-ba40-e0c59ac54ec1/scratchpad/storage/.
Fable's challenge (ACCEPT-WITH-CHANGES, defects D1-D5, case notes and missing items): fable-redelivery-r3-challenge.md
in the same folder. The first task text is brief-redelivery-r3.md there.

Write revision r3b as text in your answer (do not edit files):
1. For D1-D5, each case note in section 3, and each missing item in section 4: ACCEPT or REJECT, with a reason and a
   contract line where you reject.
2. The complete r3b normative text for everything this revision changes, not a diff. Include:
   - the three failure levels (delivery, capture, aggregate), defined once;
   - the precedence statement for CONTRACT §1 versus clause 2a (D1), and say which file each line goes in;
   - the deliberate PU-to-SC change in clause 14 (D5);
   - the writer obligation.
3. The complete new-case table (32 and up) with expected values, including the added cases: unknown baseline, a
   FRONTIER or ATTACH_OK that moves the running baseline, and `A,A2t0`. Add the aggregate-status cell for cases 17
   and 31 and every new case. Define every new fixture in the notation style, including what V's replay needs (K:148-
   149, 153-154).
4. A statement on cases 1-31, naming every cell that changes.
Mark claims about existing text [Verified] file:line. At most 200 lines.
