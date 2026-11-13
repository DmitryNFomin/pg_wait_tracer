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

Scenarios 34-64 live here, not in `../scenarios/`, because the current harness requires exactly the 33 frozen
scenarios. The implementation unit extends the harness to read this directory as well. It does not move or edit
these files. Until then, the reference evaluator (`../reference/`) and the C replay implement r2, so they abstain or
refuse on these inputs. That is fail-safe, but it is not yet r3.
