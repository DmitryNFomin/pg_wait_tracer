# Independent answer set (code-blind oracle author)

You are one of two independent authors producing expected outputs for contract test rows. Read ONLY the files in this directory:
- merge-v1-CONTRACT-master.md: the current merge contract.
- merge-v1.txt: the amendment text that replaces that contract's line 152.
- coverage-a2-v1.txt: a new presentation contract.
- ROWS.txt: input-only rows, merge (M) and coverage (C).
Do not read source code, tests or any other file, and do not search the repository. Do not guess what an implementation does: derive each answer from the contract text alone.

For every row and every variant in ROWS.txt, write exactly one line:
`<row id>[<variant>] | <expected output> | <contract clause(s) relied on>`
- Merge rows: give the exact output sequence (DATA/UNTRACED/EXIT/FRONTIER/VOID with reason and time) and the final epoch state.
- Coverage rows: give status (numbers / Unavailable with reason), the note fields (or "no note"), and any diagnostic the contract requires.
If the contract does not determine an answer, write `UNDECIDABLE | <the missing rule, in one sentence>` instead of guessing.
At the end, add a section `AMBIGUITIES` listing every place where you had to choose between two readings.
