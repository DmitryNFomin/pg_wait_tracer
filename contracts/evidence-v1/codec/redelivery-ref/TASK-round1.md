# Task: an independent reference evaluator for the evidence-v1 codec redelivery rule

You work only in this directory. Inputs: REDELIVERY.md (the normative rule, with 31 cases and their frozen expected
values), CODEC.md (the v5 byte format), CONTRACT.md and REVISION-r3.md (replay semantics, for the replay-effect
column only). Read nothing else; no product code exists for you.

The evaluator is the oracle a codec implementation will be tested against over generated delivery multisets. It
works on an abstract model, not on bytes.

1. **Abstract model (`model.md` + JSON).** Define a JSON form for one delivery multiset that can express every fixture
   in REDELIVERY.md's notation block and every variant its test obligations list:
   - header validity and physical identity;
   - immutable-header content identity;
   - flags;
   - blocks, each with start/end offset, type, a content identity standing for its exact bytes, validity or terminal
     reason, and records with payload offsets and structural faults;
   - tail or footer state;
   - successor link fields: predecessor file_no, D, and the digest as "the digest of predecessor content through D"
     or a deliberately wrong one.
   Two deliveries' bytes are equal at a position exactly when their content identities are equal there. Say how the
   model maps to bytes, so a test harness can encode each abstract delivery with a real v5 encoder.
2. **`ref_redelivery.py`** (Python 3, standard library only). Given one multiset in that JSON, return:
   - the codec records (full physical identity and content id);
   - the normalized codec stops (file, position, reason, witnesses);
   - the diagnostics;
   - the admission decisions.
   Implement REDELIVERY.md's clauses literally, citing the clause number in comments. Where a clause leaves something
   undetermined, raise an explicit error naming the clause; never guess.
3. **`cases.json` and `check_cases.py`.** Express all 31 cases in your model and compare your evaluator's output with
   REDELIVERY.md's expected columns, cell by cell. Report every disagreement. Do not change the evaluator to fit a
   case without saying which clause forced the change.
4. **`check_props.py`.** A generator of bounded multisets: sizes up to 4, the variants in the test obligations, and
   successor-before-predecessor orders. For every multiset it checks that every permutation gives the identical
   result, and that adding a copy never removes a record unless it adds a conflict (monotonicity). Print counts.

Write `REPORT.md`: the cases result (n/31), the property results with counts, every clause you found ambiguous, and
anything in REDELIVERY.md you believe is wrong, with the clause.
