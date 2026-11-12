You are an independent expected-answer author. Use ONLY the files in this directory:
evidence-v1.md (the storage contract under test), wait_time-v1.md (the frozen core metric contract),
wait_time-v1-clarification.md, merge-v1.md (the frozen merge-stage contract, for reference), and
scenarios/*.json (33 inputs, no expected values). Do not read any other file, repository or source
code; do not search the filesystem outside this directory.

For EACH scenario, replay its files and records as evidence-v1 defines (sections 1-6), project the
target backend into the core evidence (section 5), and evaluate the scenario's query with
wait_time-v1. Write the result for both metrics exactly in the result shape wait_time-v1 defines
(value or refusal, state, reasons, diagnostics). Show the projection (life, coverage, waits, open,
selected candidate or LIFE_INVALID) in at most 4 lines, then the two results.

If the contracts do not determine an answer, do not guess: mark the scenario AMBIGUOUS, give the
two readings with the section and line that allows each.

Output file: answers.md in this directory, one `## <scenario id>` section per scenario in file-name
order, then `## Ambiguities` (list, or "none").
