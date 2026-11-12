# Task: an executable reference evaluator for the evidence-v1 contract

Work ONLY in this directory. Read only the files under inputs/. Do not read anything outside this directory
(no other repositories, no /private/tmp siblings, no web). There is no product code here and there must be none.

Inputs: inputs/evidence-v1.md (the contract), inputs/wait_time-v1.md and
inputs/wait_time-v1-clarification.md (the metric it feeds), inputs/merge-v1.md (the merge contract it
references), inputs/scenarios/*.json (33 inputs; no expected values).

Write reference/replay.py (Python 3, standard library only) that:
1. Reads one scenario JSON (path as argv[1]) and prints, as one JSON object per line:
   - the projection: per backend generation, its lifetime interval and coverage intervals and any status
     flags the contract defines (state the exact shape at the top of the file);
   - each result object the scenario's query asks for, in the exact result-object shape the contract and
     wait_time-v1 define.
2. Implements the contract literally. Where the contract is ambiguous, do not guess: print
   {"ambiguity": "<section and question>"} for that scenario and continue.
3. Is written for clarity over speed: one function per contract section, each citing its section number.

Also write reference/run_all.sh that runs replay.py over every scenario and writes reference/out/<id>.jsonl.
Run it. Finish with reference/REPORT.md: per scenario one line (id, number of result objects, any ambiguity),
and a list of every contract sentence you found ambiguous. Do not compare with any expected answers; none are
provided.
