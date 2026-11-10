# Tag checklist v1

`tests/tag_checklist.tsv` is the finite release contract, one user guarantee per stable ID.
IDs are append-only: never delete or reuse one; compare the Git merge base (master workflow uses its parent).
To retire a line, add `state` and `reason` columns, set `retired`, and explain why.
Retired lines remain printed as RETIRED and never count as GREEN.
Each line declares its scope, required kernel/PG cells, test selectors, tolerance and red proof.
Tests use `file::name`, separated as CSV inside the TSV cell (quote names containing commas).
Names are callables, exact Node test titles, or the named top-level script suite.
Evidence is one shared `workflow-file/job-display-name/step-name`; split differing evidence into lines.
The audited CI bindings and adviser scope corrections are in `TAG_CHECKLIST_AUDIT.md`.
GREEN requires every named test's step to succeed on exactly the requested SHA, a red proof, and no blocker.
GREEN-UNPROVEN means the steps succeeded but `red_proof` is NONE; it cannot count toward the tag.
RED means a failed evidence step or an open `tag-blocker` labelled `line:Lnnn`.
NOT-RUN means a skipped, missing, cancelled or unfinished step, or box-check-only evidence.
NO-TEST means no test or no evidence step; uncovered guarantees stay visible.
A mapped blocker makes its line RED immediately, even if its CI steps passed (shown as `test gap`).
An unmapped open blocker adds its own RED line; an unknown `line:*` ID refuses the entire report.
Run `scripts/tag-checklist.sh <full-sha>` for grouped Markdown, or add `--json` for structured output.
`--markdown-output <path>` writes Markdown from the same snapshot as JSON; `--run-id` pins a CI run.
Only GREEN contributes to `GREEN n / total N`; all states and unmapped blockers remain in the denominator.
Offline fixtures contain `run-list.json`, `run-<id>.json`, `issues.json` and merge-base `base.tsv`.
Wrong SHAs, void jobs, malformed inputs, unavailable Git/gh evidence and removed IDs refuse with exit 2.
A valid non-GREEN report exits 0: evaluator success does not mean release readiness.
V1 sees step conclusions, not per-test terminal records or internal skips; box results cannot be typed in.
EL8 containers share a host kernel and do not prove the EL8 4.18 contract; native matrix lines remain uncovered.
The new workflow uploads Markdown/JSON after master push CI; it never edits issues or labels.
Blocker changes require a fresh invocation; v1 does not continuously recompute old artifacts.
Deferred: SHA/platform/test result records in runners, a scheduled full run on real boxes, and remaining gates.
Issues without `tag-blocker` are outside v1’s input; labels are never inferred.
