# Task: update the independent redelivery reference evaluator to the revised contract (r3b)

You work only in this directory. It contains:
- an earlier reference evaluator, written from the contract alone by an isolated author: ref_redelivery.py,
  replay_effects.py, model.md, model.json, build_cases.py, cases.json, check_cases.py, check_props.py,
  check_mutants.py, REPORT.md;
- the revised contract: REDELIVERY.md, CODEC.md, CONTRACT.md, REVISION-r3.md.
Read nothing outside this directory.

The earlier REPORT.md raised two issues that the contract has since resolved:
- a capture id under two boots: REDELIVERY clause 2a, `CAPTURE_BOOT_AMBIGUITY`;
- the successor baseline: the running baseline, checked at replay under clause 19, and no longer a static link
  field in clauses 13-14.
REDELIVERY.md now has cases 1-50 with an aggregate-status column, failure levels (delivery, capture, aggregate) and
new fixtures.

1. Update the model and evaluator to the revised clauses, citing clause numbers in comments:
   - remove the whole-multiset AmbiguityError for cross-boot reuse;
   - implement 2a scoped to the capture;
   - move the baseline from the static link check to the replay check;
   - add the failure levels and the aggregate status;
   - restrict predecessor candidates to the successor's own `(boot,capture,file_no-1)` group.
   Where a clause still leaves something undetermined, raise an explicit error naming it; never guess.
2. Express cases 32-50 in the model, and update 17 and 31 for the aggregate column. `check_cases.py` must report
   n/50 against REDELIVERY.md's expected columns, cell by cell, with every disagreement listed.
3. Extend `check_props.py`: second boots, non-root reuse, baseline-changing VOIDs before a successor, unknown
   baselines, a FRONTIER that moves the running baseline, unqualified captures, and successor-before-predecessor
   orders. Permutation invariance and monotonicity must hold, except where 2a retracts by design; state the exact
   exception you assert. Print counts.
4. Add mutants for 2a, for the baseline move and for the failure levels to `check_mutants.py`. Each must be caught.
Rewrite REPORT.md: cases n/50, property counts, mutants, every clause you still find ambiguous, and anything you
believe is wrong in the revised contract.
