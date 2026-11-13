You maintain an executable reference evaluator. Use ONLY the files in this directory. Do not read any other
file, repository or source code, and do not search the filesystem outside this directory. There are no expected
answers here; do not try to obtain any.

replay.py implements evidence-v1 revision r2. REPORT-r2.md is its author's report, which lists six contract
sentences it judged ambiguous; for each, replay.py raises Ambiguity. evidence-v1.md is now revision r3: its
section 5 resolves those six questions (malformed records, unknown kinds, unexpected counters, overlapping
attachments, END without an addressable run, nonadvancing FRONTIER, run mismatch, capture/boot conflict).

Update replay.py so it implements evidence-v1.md (r3) exactly, together with wait_time-v1.md, its clarification
and merge-v1.md. Keep it to the Python standard library. Change only what r3 requires; behaviour on anything r3
did not change must stay identical. Remove an Ambiguity branch only where r3 now determines the answer; keep it
(and say why) where r3 still does not.

Then run `python3 replay.py scenarios/<id>.json` for all 64 scenarios. Write each output to out/<id>.jsonl, in
the same output format replay.py already uses. Write REPORT-r3.md (at most 40 lines): each change with the r3
section it implements, any scenario where you think the contract still admits two readings (give both), and any
ambiguity branch that remains.
