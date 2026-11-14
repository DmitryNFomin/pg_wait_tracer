# Independent redelivery r3b expected answers

The dispatcher must prepare a fresh isolated directory containing this task and
exactly these five inputs before the answer author starts:

- `CODEC.md`, copied from the revised `contracts/evidence-v1/codec/CODEC.md`.
- `CONTRACT.md`, copied from the revised `contracts/evidence-v1/CONTRACT.md`.
- `REVISION-r3.md`, copied from the revised `contracts/evidence-v1/REVISION-r3.md`.
- `REDELIVERY-blank.md`, copied from this directory: the normative redelivery
  revision, input-only fixture definitions and case inputs, with expected columns
  and case-specific answer commentary removed.
- `CASES-blank.md`, copied from this directory: input notation and blank expected
  cells for amended cases 17 and 31 and new cases 32–50.

Read those inputs and nothing else. Do not follow links or consult other files,
product code, product output, evaluators, prior expected answers, or another
author's work. Never supply the answer-bearing parent `REDELIVERY.md`,
`ANSWER-astra.md`, `CHALLENGE-fable.md`, or `QUESTION-astra.md` to the isolated
directory. Each answer author works independently in a separate directory.

Fill every blank expected cell in `CASES-blank.md`: codec records, normalized
codec stops, diagnostics / explicit replay effects, and aggregate status.
Also revalidate cases 1–31 independently using the input-only table in
`REDELIVERY-blank.md`; return the same expected columns for those cases. Derive
aggregate status for every case, including those without a pre-r3b status cell.
Evaluate all cases as multisets, including successor-before-predecessor order,
and describe incremental accumulation and any required retraction.

Use full physical record identities and contents, preserve record order, and
distinguish codec stops from capture ambiguity and replay stops. For every case,
include source-provenance sets, predecessor candidates, clean witness sets,
link/admission decisions, normalized stops and their complete witnesses,
suppressed and unassigned diagnostics, capture outcomes, aggregate status, and
the specified replay effects. Derive baseline, retained LIFE, surviving
certificates, publication high-water, coverage and lifecycle uncertainty where
relevant. Report expected and declared baselines for any reachable mismatch.

List every clause that leaves a value undetermined, identifying the affected
case and cell and explaining the ambiguity. Do not invent a rule to fill it.
Write the completed tables and derivations to the assigned answer file; do not
edit the inputs. These are independent derivations, not an evaluator run.
