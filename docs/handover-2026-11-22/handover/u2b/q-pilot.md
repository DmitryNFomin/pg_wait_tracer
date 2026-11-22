# Question: how does the evidence-v1 pilot route combine with the coverage label?

Read-only. Answer in at most 20 lines; mark every claim VERIFIED (file:line read) or INFERRED.

Spec r2 §3 "Other" (SP/m871/spec-coverage-label-r2.md) says: "pilot_wait_metrics :8916 ANDs its evidence verdict". §1 says the trace reader accepts {2,3,4,6} and rejects v5 (evidence-v1 codec); COVERAGE blocks exist only in v6.

The U2b implementer (branch agent/947-coverage-server at 4caaa54f, clone /private/tmp/pgwt-codex/agent%2F947-coverage-server; base master 4059fcde) stopped: the pilot route (src/server.c `pilot_buffers` and the pilot_wait_metrics handler, around :9175-9250 on the branch) reads v5 evidence-v1 files, which carry no COVERAGE records, and the frozen pilot tests (tests/test_pilot_route.py) require valid answers from v5 evidence. ANDing with a coverage verdict built from v6 files would withhold every pilot answer.

Questions:
1. What does the pilot route actually read, and can a v5 evidence file and a v6 coverage-carrying trace describe the same capture/window in one directory today?
2. Which is correct: (a) the pilot keeps its own evidence-v1 verdict and the coverage verdict does not apply to it (the label certifies v6 trace data only); (b) AND, as the spec says, with the consequence that every pilot answer is withheld; (c) something else (say what). Give the failure each wrong choice would cause.
3. If (a): what must the pilot response say so that it is not mistaken for a coverage-certified answer, and which test would catch a regression?
