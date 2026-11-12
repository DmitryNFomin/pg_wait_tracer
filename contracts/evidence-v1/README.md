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
- `projections.json`: per scenario, the replay projection (life, coverage, closed waits, open wait) that
  both answer sets give. Extracted mechanically from each set and compared: 33 of 33 identical. A contract test
  compares an implementation's projection with this file, so a projection error that leaves the result object
  unchanged is still caught.
- `oracle-v1/inputs/merge-v1.md`: the exact merge-v1 contract text both answer-writers read (byte-identical
  to `contracts/merge-v1/CONTRACT.md` on draft PR 873 at cdefcf89, not yet on master). The wait_time v1 text
  they read is byte-identical to `contracts/wait_time-v1/contracts/metrics/wait_time.md` on master.
- `notes.md`: the design notes CONTRACT.md cites (N2, N4, N5). The answer-writers did not receive it.

An implementation change may not edit anything in this directory. Changing the contract is its own
reviewed change and invalidates the expected answers.
