# evidence-v1 revision r3: fault handling (Refs #868)

Revision r3 (`../REVISION-r3.md`) settles six questions that `../reference/REPORT.md` listed as ambiguous and that no
scenario exercised. A malformed record, an unknown kind or a structural fault stops that capture's file chain at the
last valid certificate before it: the earlier part is answered, nothing later in that capture is salvaged, and
independent captures stay readable. A readable counter vector that differs from the baseline is consumed as a VOID
and replay continues. Whole-input refusal is not a contract outcome.

How it was made:
- Astra wrote r3 from the contract, its open questions and the inputs only. Fable challenged it by hand-replaying all
  25 new scenarios (NEEDS-REVISION: three text holes and thin "do not stop" coverage). Revision b accepted all eight
  findings, retargeted 57 and added 59-64.
- Two expected-answer sets were written independently from `oracle/TASK.md`'s directory: r3 `CONTRACT.md`, wait_time
  v1, its clarification, merge-v1 and the 64 inputs, with no code and no answers. `oracle/answers-sol.md` is
  gpt-6.1-sol (codex). `oracle/answers-sonnet.md` is Claude Sonnet. Neither reports an ambiguity.
- Their 128 result objects for all 64 scenarios are identical. For 01-33, both sets equal `../expected.json`, and the
  sol projections equal `../projections.json`. The Sonnet projections are prose; compared after parsing, they agree.
- `expected.json` and `projections.json` here hold scenarios 34-64, extracted mechanically from the two sets.

`reference/` is the r3 reference evaluator. gpt-6.1-sol (codex) updated `../reference/replay.py` in an isolated
directory, given only `reference/TASK.md` (verbatim) and its inputs: r3 CONTRACT.md, wait_time v1, the clarification,
merge-v1, the 64 inputs, and the r2 evaluator with its report. It had no answers. In TASK.md, `REPORT-r2.md` is
`../reference/REPORT.md` and `REPORT-r3.md` is `reference/REPORT.md`.
- The lead re-ran it on all 64 inputs. Its results equal `../expected.json` and `expected.json` here (128/128), and
  its target projections equal both `projections.json` files.
- Control: the r2 evaluator on the same inputs gives 65 differences. One is scenario 63, where r2 rules answer a
  complete 5 ns that r3 treats as uncertified.
- One Ambiguity branch remains (§7: a target capture with no validated descriptor and no unique boot). No scenario
  hits it.

Scenarios 34-64 live here, not in `../scenarios/`, because the current harness requires exactly the 33 frozen
scenarios. For the same reason the r3 evaluator lives here, not in `../reference/`: the differential compares the C
replay with `../reference/`, and the C replay still implements r2. The implementation unit points the harness and
the differential at this directory. It does not move or edit these files. Until then, the C replay refuses on these
inputs, and on r2-only answers such as 63 it can differ from r3. No consumer reads the replay yet.
