# Design question to an adviser (RULE 1 of CLAUDE.md): the info lists start at boot, the default "All" window starts at the first retained record; which view moves

This is a written design question from the lead to an adviser, one question, one answer. It is not an implementation brief; scripts/lead/check-brief.sh does not apply; do not run it. Read-only: edit no file except your answer if told to write one. At most 30 lines. Mark every claim VERIFIED (file:line read, or command and output) or INFERRED. scratchpad = SP

## Material
- The branch: agent/947-coverage-server at b15ec1d2 (repository <repo>; read with `git show b15ec1d2:<path>`). src/coverage.c, src/coverage_trace.c, src/server.c (the info reply's gap and hole lists, `cov_info_lists`), web/static/ (who reads what).
- The round's evidence: /private/tmp/u2b-r18-evidence/scope-questions.txt (item 1), startup-gap-list-replies.json, startup-gap-live-facts.txt, and the two answers the implementer obtained mid-round within its own scope limits: astra-lists-advice.txt, fable-lists-advice.txt (three options (i), (ii), (iii) at its end).
- The owner's standing rules: all numbers correct; two views of one quantity must agree; for anything the tracer cannot do, say it clearly in the interface and documentation, and the application must not fail.

## Measured
The default "All" window is [S, to) where S is the first retained record of the latest boot; its end now reaches the last witnessed loss. The info reply lists gaps and holes for the whole boot. When a capture loss is detected with a watermark floor before S, info lists a gap from boot_start, 1.28 ms and 1.86 ms before S in two saved live replies, 5 s in a synthetic fixture; the default window's own "uncovered" list has the same loss cut at S. Nothing in web/static reads the info lists today (the implementer's search; verify).

## Decide
1. Are these two views of one quantity that disagree (a defect to fix), or two different quantities that are each right (boot-wide inventory vs a window's content) and only need to say what they are?
2. Choose: (i) the info lists stay boot-wide and are documented and labelled as such; the guarantee becomes "every listed gap and hole ends at or before All's end, and its part at or after S is in the default window's uncovered list"; (ii) clip the info lists at the retained start; (iii) let the "not captured before the first record" reason outrank the loss before S. For your choice: the exact guarantee sentence, the test row that can go red (the existing containment rows use fixtures with S equal to boot_start and cannot), what the API documentation says, and anything an external consumer of the info reply would see change.
3. Is there a consumer today or planned (the interface, replay, the tag checklist) for which the boot-wide start matters, or for which a gap starting before "All" would be shown to a user as a contradiction?
