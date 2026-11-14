# evidence-v1 — storage contract for trace v5 EVIDENCE records (Refs #868)

Frozen 2026-11-12 as **logical replay semantics**. Capture mechanisms named as obligations in
CONTRACT.md sections 1 and 3 (FRONTIER barrier, coherent reread, END instant) are not yet demonstrated.

- `CONTRACT.md`: the contract, written by Astra (design adviser). Revision r2 (`REVISION-r2.md`)
  applies the six findings of an independent challenge by Fable against wait_time v1 and merge-v1.
  Revision r3 (`REVISION-r3.md`) defines fault handling: a malformed record, an unknown kind or a structural fault
  stops that capture's chain at the last valid certificate. Its 31 new scenarios and two answer sets are in `r3/`.
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
- `reference/`: an executable reference evaluator (`replay.py`, Python standard library), written by
  gpt-6.1-sol (codex) from the same inputs the answer-writers received and nothing else (`TASK.md`), with
  no expected answers in its directory. Run over all 33 scenarios, its result objects equal `expected.json`
  33/33 and its target projections equal `projections.json` 33/33; it reported no ambiguity for any scenario.
  `REPORT.md` lists six contract sentences it judged ambiguous in general; no scenario exercises them.
  Implementations use it for differential testing; it is not product code.
  `REPORT.md` mentions `run_all.sh`, the driver used in the writer's directory; it is omitted here because it
  assumes that directory's layout and would overwrite `REPORT.md`. Run `replay.py <scenario.json>` directly.

An implementation change may not edit anything in this directory. Changing the contract is its own
reviewed change and invalidates the expected answers.

- `codec/`: the byte-level encoding of these records in trace v5 (`CODEC.md`), written by Astra and revised (r2) after an
  independent challenge by Fable (`CHALLENGE-fable.md`, dispositions in `REVISION-codec-r2.md`). `WORKED.md` gives three
  hand-worked examples. `fixtures/ex{1,2,3}.bin` are those bytes as produced by an independent encoder written from CODEC.md
  alone (`oracle/enc.py`, `oracle/values.json`); Fable's independent encoder (`oracle/fable_check.py`) produced the same 646
  bytes. Caveat: the second writer saw WORKED.md's derived sizes and checksums (not its hex) before writing its encoder.
  CONTRACT.md is unchanged by the codec. The fault-handling question this codec raised (CONTRACT.md §5 against the
  reference evaluator and the C replay, which both refused the whole input) is settled by revision r3 (`REVISION-r3.md`,
  scenarios in `r3/`). The C replay on master still implements r2 and refuses those inputs until its r3 unit lands.

Two U7b review strikes exposed the redelivery gap. Both advisers agreed it was
a contract gap but differed on the rule; the lead resolved it for **prefix union**
from CODEC.md:554–556 (at `03694b8b`), requiring later published blocks, including
retroactive VOID, to be incorporated. Astra drafted the rule, Fable challenged it
(**ACCEPT-WITH-CHANGES**, seven points), and r2 accepted all seven. The accepted
revision is [codec/REDELIVERY.md](codec/REDELIVERY.md), with the source question,
answer, challenge and blank oracle inputs in
[codec/redelivery-oracle/](codec/redelivery-oracle/); independent answers follow.
See [Refs #868, comment 6868500311](https://github.com/DmitryNFomin/pg_wait_tracer/issues/868#issuecomment-6868500311).
