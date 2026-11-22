# To the design author (RULE 1 of CLAUDE.md): settle a disagreement on the info reply's gap and hole lists

This is a written design question from the lead. It is not an implementation brief; scripts/lead/check-brief.sh does not apply; do not run it. Read-only: edit no file; print your answer as the final message. At most 30 lines. Mark every claim VERIFIED (file:line read, or command and output) or INFERRED. scratchpad = SP

## Material
- The question both advisers answered: scratchpad/u2b/q-lists-start.md. Your answer (option (i): lists stay boot-wide, the guarantee is narrowed and documented): scratchpad/u2b/astra-lists-start.md.
- The challenger's answer, read it whole, with its probe scripts and logs (it ran three synthetic fixtures on b15ec1d2 and a scratch prototype): scratchpad/u2b/fable-lists-start.md, scratchpad/u2b/fable-lists-probe/{probe,candidate,twoboot,both}.log and the .py files beside them.

## What the challenger measured that your answer did not have
- Fixture A (leading files removed, S = boot_start + 400 s, a loss at 410-420 s): info lists a gap [0,420) against All [400,1000); an explicit window [300,500) answers capture_loss for [300,420) where its loss-free twin answers retention. The overhang is bounded by S - boot_start, not by milliseconds.
- Fixture C (an earlier boot ends with a loss that never closes; the latest boot is clean): the same info reply reports coverage complete over All [130,200), default metrics are returned, and its own list has a gap [55,200), because src/server.c:5442-5447 stretches every earlier boot's query to the latest end. Your guarantee sentence for (i) is single-boot and does not cover this; the reply contradicts itself.
- Option (ii) (clip the list) manufactures a disagreement with explicit windows. Option (iii) as one line (the loss floor clamped to S at src/coverage.c:547) plus stopping an earlier boot's list at the next boot's start (two lines in cov_info_lists): all three fixtures agree, the verdict table (58 windows + 17 rule cases) and the 29 consumer cases pass in the scratch build; default replies unchanged for A and the startup fixture. It changes reason precedence inside [boot_start, S): a specification amendment with new rows.

## Decide
1. Is fixture C a defect independent of the option chosen (a list entry inside a window the same reply calls complete)? Is it on master today (cov_info_lists exists on origin/master) and what would an API consumer see?
2. With A and C in hand: keep (i), or move to (iii) plus the next-boot stop, or something else. If you keep (i), give the guarantee sentence that is true for A and C and the row that goes red. If you move, give the amended precedence sentence for [boot_start, S), the guarantee sentence, and the rows.
3. Where this work belongs: inside the coverage-label unit now under review (round 18 forbade verdict changes), or a separate unit with its own contract/specification delta and two answer sets; and whether the coverage-label unit can merge first with the lists unchanged (nothing in the interface reads them) without shipping a known defect, given the owner's rule that two views of one quantity must agree.
