# evidence-v1 — storage contract for trace v5 EVIDENCE records (Refs #868)

Frozen 2026-11-12 as **logical replay semantics**. Capture mechanisms named as obligations in
CONTRACT.md sections 1 and 3 (FRONTIER barrier, coherent reread, END instant) are not yet demonstrated.

- `CONTRACT.md`: the contract, written by Astra (design adviser). Revision r2 (`REVISION-r2.md`)
  applies the six findings of an independent challenge by Fable against wait_time v1 and merge-v1.
- `wait_time-v1-clarification.md`: the lifetime-start clarification to wait_time v1.
- `scenarios/*.json`: 33 inputs, no expected values.
- `oracle-v1/`: two expected-answer sets written independently from the contract files only, in
  directories containing no product code and no other answers: `answers-sol.md` (gpt-6.1-sol, via
  codex) and `answers-sonnet.md` (Claude Sonnet). `TASK.md` is the instruction both received.
  Both report no ambiguity. Their 66 result objects (33 scenarios x 2 metrics) are identical.
- `expected.json`: those result objects, extracted by script, one row per scenario.

An implementation change may not edit anything in this directory. Changing the contract is its own
reviewed change and invalidates the expected answers.
