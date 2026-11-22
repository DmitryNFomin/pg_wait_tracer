# Brief: discovery admission, merge unit, round 13 (fresh implementer): a terminal control contradicted on delivery, two pinned rows, one count, one bound

## Authorised by

Owner decision, 2026-11-20, recorded verbatim on #988: "a" (build discovery admission). Round 12 (003ee35a) made END the last write of a release attempt; its independent review is READY-with-nits and found no file prefix that reads complete early: SP/a2go/disc/review-merge-003ee35a.md (read it first: should-fix S1, S2, S3, its nits, its measurement of a failed revoked-OPEN write; probes under scratchpad/rv18/). An exhaustive enumeration then left one family of differences that turned out to be a place the contract did not decide: SP/a2go/disc/m12/triage.md. Both design advisers decided it and the contract was amended (commit 5aa79a7e on this branch, contracts only; two independent answer sets agree with its expected outputs). You are a fresh implementer. The enumeration, extended, will be run against your result.

Issue #988; live comments (refreshed at dispatch):

```console
$ gh api repos/DmitryNFomin/pg_wait_tracer/issues/988/comments --paginate --jq '.[] | {id: .id, body: .body}'
{"body":"**Quiet-baseline mechanism: agreed by both design advisers, and its control passed on both kernels (2026-11-20)**\n\nAn idle process found at startup (an idle pooled connection, an auxiliary asleep) never makes a transition, so it needs a baseline that does not wait for one. Both advisers rejected \"seed, enable, re-read the wait word\" (a transition and its reverse can hide in buffers) and my first replacement (scheduler counters plus the state letter: the kernel sets the sleeping state before it decides to sleep).\n\nAgreed mechanism (Fable found it; Astra had named the same kernel primitive): a bracket in which the kernel itself proves the task was off every runqueue.\n- `/proc/\u003cpid\u003e/syscall` prints \"running\" unless `wait_task_inactive` succeeds under the runqueue lock (not on a CPU, not queued). The task must read inactive at both ends of the bracket, and its voluntary context-switch count must be equal at both ends.\n- Inside the bracket: resolve the wait address and compare it with the armed one, read the wait word, take a fresh timestamp t, install the seed (that wait value, time t, off-CPU, a valid zero-consumption CPU base), enable the watchpoint and confirm.\n- If the bracket holds, the task executed nothing in it, so the seed is its exact state at t and the first later record needs no exclusion. A failed, denied or unreadable proof is a failed bracket, never a pass; after bounded retries the process falls back to waiting for its first record.\n- Today's seeding is not coherent for this (it opens an on-CPU stretch when the wait word is 0 and has no valid CPU base), so the producer unit changes it.\n\nControl on the real kernels (tests/results/session-20261120/quiet-witness/analysis.md on the lead's Mac): on EL8 4.18.0-553 and on Ubuntu 6.8, a sleeper reads inactive with an unchanged count, a spinner reads \"running\" in 200 of 200 reads, a sleeper woken once between the reads shows the count move from 1 to 2, and 1,000 quiet brackets gave 0 anomalies. Not measured: a PostgreSQL backend as the subject, and reads under load.\n\nNext: the consolidated merge-v1 amendment (DECISIONs 30-44 with this route), two independent answer sets from the rows alone, an isolated contract commit, then the producer and merge units.\n\nRefs #988\n","id":7047038197}
{"body":"**Owner decisions, 2026-11-21 12:11 UTC, verbatim: \"B b a\"**\n\nAnswering three a-or-b questions put in this order:\n\n1. **The three PostgreSQL 13 helper processes with no wait state (logger, statistics collector, archiver).** Options as put: a = list each as \"present, not measured\" (needs a new record in the capture files, reader rules and a contract amendment); b = do not list or count them, and say so in the documentation. Answer: **b**.\n2. **What counts as a backend.** Options as put: a = count every connection process wherever PostgreSQL keeps a wait state; b = count a process only while it holds a PostgreSQL process slot, on every version (health checks, cancel requests and silent clients are then \"not a backend\"). Answer: **b**.\n3. **Real-time priority for the tracer's main loop** (issue 989). Options as put: a = on by default wherever the per-CPU transport is on, with `--no-rt-priority` to turn it off; b = opt-in only. Answer: **a**.\n\nWhat follows from 1 and 2 here:\n- Contract merge-v1: the no-wait-word route (DECISION 37(c)) keeps its proof and its bound but loses the durable support-publication precondition and the per-process \"present, not measured\" line (DECISION 43); rows N1, N10 and N14 get new expected answers through the usual two independent answer sets. Documentation sentence: \"On PostgreSQL 13 the logger, the statistics collector and the archiver never hold a PostgreSQL process slot. They are not backends: they are not listed and not counted.\"\n- The rule for fork-born processes that never initialise (contract delta 3b) is finalised under the process-slot rule.\n- No new record kind is added to the capture files for this.\n\nCopied here from the comment on the churn issue (comment 7062393007 there) now that no implementation round in flight cites this issue. State on 2026-11-21 17:55 UTC: the contract part of 1 is on master (6f18da28: the no-wait-word route without role-support publication; rows N1, N10, N14); the process-slot population rule of 2 is written as a draft delta and belongs to the bounded-omissions unit, not applied yet.\n\nRefs #988\n","id":7066825885}
```

CONTROLLING SCOPE DECISION: #988 comment 7047038197 "If the bracket holds, the task executed nothing in it, so the seed is its exact state at t and the first later record needs no exclusion."

## Starting point

Run scripts/lead/codex-task.sh on the EXISTING branch agent/988-discovery-merge at 5aa79a7e. First run `git log --oneline -1`; it must print 5aa79a7e. Otherwise print WRONG_HEAD <sha> and stop.

The LAST line of .codex-commit-msg is exactly "Refs #988". Do not push. Do not run `make check`. Do not touch contracts/. Everything builds and runs on the Mac as host C tests; run nothing eBPF-related and no browser tests.

**Do not stop on a scope question.** If an item needs something this brief does not allow, finish every other item and list all such questions together in the report.

## Normative text

contracts/merge-v1/CONTRACT.md and contracts/merge-v1/oracle-discovery/{ROWS.md,EXPECTED.md} as at 5aa79a7e (190 rows). `git show 5aa79a7e -- contracts/merge-v1/CONTRACT.md contracts/merge-v1/oracle-discovery/EXPECTED.md contracts/merge-v1/oracle-discovery/ROWS.md` shows exactly what changed (section 3; DECISIONs 36, 40, 41; rows H11g, H12a-H12e, H13). contracts/merge-v1/oracle-discovery/compare-delta9.md says why.

## What was measured on 003ee35a (not instructions)

- `EXIT_D18, TERM_D19{names it}, TERM_D18` with no ADMIT: two faults; the exit-disposition accessor reads NONE; with a cookie-less EXIT the first control is recorded as MATCHED at 19 for an EXIT stamped 18 (review section 6; triage).
- After row H11f's inputs the capture counts one unfinished record, and a finish after REARM adds a fault (review S3).
- A failed revoked-OPEN write: the reader returns complete over [6,19); through the test-only replay REARM and further progress it grows to [6,30) (review, probe V6).
- The host test for row H11e delivers its false control naming the wrong EXIT position, and a mutant on the timestamp check survives the suite (review S1). No case fails when the terminal-control path taken while a discovery hold is active is removed (review S2, its mutant my8).

## Task

1. **A terminal control contradicted on delivery (DECISIONs 36, 40; rows H12a-H12e; controls H6b, H6c, H11e).** In every admission state such a control reports one fault and records no final claim, binding or disposition; the exit-disposition accessor is unchanged by it; its identical retransmission is silent, before and after a valid replacement; a later otherwise qualified control is recorded without a fault for disagreeing with it. A control validly recorded before its contradiction was established stays the retained refuted claim (H6c unchanged). This includes the comparison against a retained, still undecided cookie-less EXIT in the capture layer, not only the core's check; with a cookie-less EXIT and no later valid control, finish reports the missing disposition (H12c). The existing case that pinned the other reading changes: name it and the sentence.
2. **Row H13 (a terminal control while another batch is held).** Pin the complete expected output; add the case that fails under the reviewer's mutant my8.
3. **Row H11g (review S3).** An EXIT disposed during suspension is not an unfinished record: pending payload records 0 after the control's VOID, and no fault at a finish after REARM.
4. **Review S1.** Correct the H11e host case so its false control names the EXIT the row names; the reviewer's mutant on the timestamp check is red.
5. **The END publisher's result (DECISION 41's new sentence).** Show from the code, and with a host case that injects a failure at each fallible step of the real startup publisher, that it reports failure for END only when no complete END is reader-visible. Change the publisher only if a step can fail after the END is visible.
6. **Bound the failed-retraction exposure.** Once the persistence of a startup OPEN or revoked OPEN has failed, the capture refuses every later output kind, not only VOID, so that no further record, frontier or watermark is published for that capture. Re-run the reviewer's V6 probe followed by REARM and progress with the real writer and reader and report the window the reader returns: it must not grow past what it was at the failure. This does not remove the limit DECISION 43 states; do not add a reader rule. If any contract sentence forbids refusing those outputs, do not implement this item and say which sentence.
7. **Tests and regression.** All 190 rows through the persisted record and the reader with the order of records asserted and the accessor's value after every input where a row states it; a crash-cut case for the closure-from-committed-state END route (the review's nit); the mutants EXPECTED.md lists for the new rows plus one per item above, each building, reaching its case and failing its intended assertion. Round 12's rows, its 351 mutants and its 1,534 reader prefixes stay as they are or are listed as changed with the reason; legacy outputs byte-identical to 5fd6a519; discovery disabled: every changed line unreachable or behaviour-neutral (say how you checked); the base merge's own contract tests pass unchanged; every test that scripts/check.sh and tests/unit_tests.list run and that touches src/core/, src/merge_capture.* or src/open_snapshot.*, each listed with its result. src/core/pgwt_discovery.h and src/merge_capture.h do not change; if an item cannot be done without changing one, say so and do the rest.

## Excluded

BPF, src/backend*.c, the producer (src/discovery_*.c), the daemon, the server, web/, contracts/, examples.json, KNOWN_FAILING, retries, tolerances, recovery of discovery certification after a fault, any reader rule (src/coverage.c), a new persistence format or crash-atomic storage protocol, files named tests/discovery_enum_* (another implementer's).

## Done

Report: commit sha (from the wrapper), `git diff --stat 5aa79a7e`, for each of items 1-6 the functions that implement it, the 190-row result with the variant count, the accessor table for H12a-H12e, item 5's fallible-step table, item 6's reader window before and after, the changed existing cases with reasons, the mutant list with results, the byte comparison, the existing tests run with results, both registration guards, `git diff --check`, every contract sentence you could not implement as written, and the list of scope questions if any.

## Budget

Rounds: 1
Wall-clock minutes: 300

## Report by

At the end of the budget, even if incomplete: at most 15 lines.
