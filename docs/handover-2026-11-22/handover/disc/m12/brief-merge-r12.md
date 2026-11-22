# Brief: discovery admission, merge unit, round 12 (fresh implementer): END published last, three pinned rows, two missing tests

## Authorised by

Owner decision, 2026-11-20, recorded verbatim on #988: "a" (build discovery admission). Round 11 (f3ee42e3) implemented the third contract amendment; its independent review is READY-with-nits with two should-fix items: SP/a2go/disc/review-merge-f3ee42e3.md (read it first; probes and drivers under scratchpad/rv17/). The review also measured that a file cut between a startup-gap END and the rollback OPEN of a failed attempt reads complete for windows the capture never committed (its paragraph on D7 and D71). Both design advisers judged that a false completeness claim, and the contract was amended again (commit ee95f1a6 on this branch, contracts only; two independent answer sets agree with its expected outputs). You are a fresh implementer. An exhaustive enumeration by another implementer will be run against your result.

Issue #988; live comments (refreshed at dispatch):

```console
$ gh api repos/DmitryNFomin/pg_wait_tracer/issues/988/comments --paginate --jq '.[] | {id: .id, body: .body}'
{"body":"**Quiet-baseline mechanism: agreed by both design advisers, and its control passed on both kernels (2026-11-20)**\n\nAn idle process found at startup (an idle pooled connection, an auxiliary asleep) never makes a transition, so it needs a baseline that does not wait for one. Both advisers rejected \"seed, enable, re-read the wait word\" (a transition and its reverse can hide in buffers) and my first replacement (scheduler counters plus the state letter: the kernel sets the sleeping state before it decides to sleep).\n\nAgreed mechanism (Fable found it; Astra had named the same kernel primitive): a bracket in which the kernel itself proves the task was off every runqueue.\n- `/proc/\u003cpid\u003e/syscall` prints \"running\" unless `wait_task_inactive` succeeds under the runqueue lock (not on a CPU, not queued). The task must read inactive at both ends of the bracket, and its voluntary context-switch count must be equal at both ends.\n- Inside the bracket: resolve the wait address and compare it with the armed one, read the wait word, take a fresh timestamp t, install the seed (that wait value, time t, off-CPU, a valid zero-consumption CPU base), enable the watchpoint and confirm.\n- If the bracket holds, the task executed nothing in it, so the seed is its exact state at t and the first later record needs no exclusion. A failed, denied or unreadable proof is a failed bracket, never a pass; after bounded retries the process falls back to waiting for its first record.\n- Today's seeding is not coherent for this (it opens an on-CPU stretch when the wait word is 0 and has no valid CPU base), so the producer unit changes it.\n\nControl on the real kernels (tests/results/session-20261120/quiet-witness/analysis.md on the lead's Mac): on EL8 4.18.0-553 and on Ubuntu 6.8, a sleeper reads inactive with an unchanged count, a spinner reads \"running\" in 200 of 200 reads, a sleeper woken once between the reads shows the count move from 1 to 2, and 1,000 quiet brackets gave 0 anomalies. Not measured: a PostgreSQL backend as the subject, and reads under load.\n\nNext: the consolidated merge-v1 amendment (DECISIONs 30-44 with this route), two independent answer sets from the rows alone, an isolated contract commit, then the producer and merge units.\n\nRefs #988\n","id":7047038197}
{"body":"**Owner decisions, 2026-11-21 12:11 UTC, verbatim: \"B b a\"**\n\nAnswering three a-or-b questions put in this order:\n\n1. **The three PostgreSQL 13 helper processes with no wait state (logger, statistics collector, archiver).** Options as put: a = list each as \"present, not measured\" (needs a new record in the capture files, reader rules and a contract amendment); b = do not list or count them, and say so in the documentation. Answer: **b**.\n2. **What counts as a backend.** Options as put: a = count every connection process wherever PostgreSQL keeps a wait state; b = count a process only while it holds a PostgreSQL process slot, on every version (health checks, cancel requests and silent clients are then \"not a backend\"). Answer: **b**.\n3. **Real-time priority for the tracer's main loop** (issue 989). Options as put: a = on by default wherever the per-CPU transport is on, with `--no-rt-priority` to turn it off; b = opt-in only. Answer: **a**.\n\nWhat follows from 1 and 2 here:\n- Contract merge-v1: the no-wait-word route (DECISION 37(c)) keeps its proof and its bound but loses the durable support-publication precondition and the per-process \"present, not measured\" line (DECISION 43); rows N1, N10 and N14 get new expected answers through the usual two independent answer sets. Documentation sentence: \"On PostgreSQL 13 the logger, the statistics collector and the archiver never hold a PostgreSQL process slot. They are not backends: they are not listed and not counted.\"\n- The rule for fork-born processes that never initialise (contract delta 3b) is finalised under the process-slot rule.\n- No new record kind is added to the capture files for this.\n\nCopied here from the comment on the churn issue (comment 7062393007 there) now that no implementation round in flight cites this issue. State on 2026-11-21 17:55 UTC: the contract part of 1 is on master (6f18da28: the no-wait-word route without role-support publication; rows N1, N10, N14); the process-slot population rule of 2 is written as a draft delta and belongs to the bounded-omissions unit, not applied yet.\n\nRefs #988\n","id":7066825885}
```

CONTROLLING SCOPE DECISION: #988 comment 7047038197 "If the bracket holds, the task executed nothing in it, so the seed is its exact state at t and the first later record needs no exclusion."

## Starting point

Run scripts/lead/codex-task.sh on the EXISTING branch agent/988-discovery-merge at ee95f1a6. First run `git log --oneline -1`; it must print ee95f1a6. Otherwise print WRONG_HEAD <sha> and stop.

The LAST line of every commit message, and of .codex-commit-msg, is exactly "Refs #988", with nothing after it (the two previous rounds left another trailer after it). Do not push. Do not run `make check`. Do not touch contracts/. Everything builds and runs on the Mac as host C tests; run nothing eBPF-related and no browser tests.

**Do not stop on a scope question.** If an item needs something this brief does not allow, finish every other item and list all such questions together in the report.

## Normative text

contracts/merge-v1/CONTRACT.md and contracts/merge-v1/oracle-discovery/{ROWS.md,EXPECTED.md} as at ee95f1a6 (183 rows). `git show ee95f1a6 -- contracts/merge-v1/CONTRACT.md contracts/merge-v1/oracle-discovery/EXPECTED.md contracts/merge-v1/oracle-discovery/ROWS.md` shows exactly what changed against the text round 11 implemented (section 3; DECISIONs 36, 40, 41, 43; rows V4, N14, H11b, H11d, H11e, H11f; the ordering mutants). contracts/merge-v1/oracle-discovery/compare-delta8.md says why.

## Task

1. **END last (section 3, DECISION 41; rows V4, N14 and every row that closes the startup gap).** In a release attempt that closes the gap: supporting TEXT and DATA, then any advancing FRONTIER and every remaining counter checkpoint of the attempt, then END. A successfully published END is the commitment of the bound, discharge and closure: no fallible step of that attempt follows it. An attempt that fails publishes no END and no rollback OPEN (remove the rollback). The same on the path where a later terminal control closes the gap. The revoked OPEN before the VOID of a revoking fault stays exactly as round 11 made it.
2. **A terminal control before its EXIT (DECISIONs 36, 40; row H11d).** One fault at the control, both recorded claims refuted there, nothing further when the EXIT arrives.
3. **The review's S1 (row H11e).** After a reported enable/EXIT contradiction, a separately false terminal control keeps its own fault and does not become the retained claim; a later otherwise qualified terminal control is recorded as refuted without another fault for the already reported contradiction.
4. **A refuted match on a cookie-less EXIT (row H11f; also row H11a's accessor answer).** It is the final MATCHED disposition: `pgwt_capture_exit_disposition` returns MATCHED and finish reports no missing-disposition fault.
5. **Tests.**
   - All 183 rows through the persisted record and the reader, every variant, with the ORDER of OPEN, DATA/TEXT, FRONTIER, END, revoked OPEN and VOID asserted.
   - Crash-cut checks with the real writer and the real reader, as the review's D7 probe did: for every execution that publishes an END or fails a release attempt, take the persisted bytes after EACH record the capture writes during that input, run the reader on each prefix, and assert that no prefix returns complete for a window that the state after the input does not cover. Include a rejected FRONTIER write, a failed counter checkpoint after a successful FRONTIER, a rejected END write, and the terminal-control closure path. Report the table (execution, cut point, reader result).
   - Mutants, each building, reaching its case and failing its intended assertion: END before FRONTIER; END before the post-FRONTIER checkpoint; suppressing a successfully published FRONTIER after a later END rejection; a rollback OPEN written after a failed attempt; plus those EXPECTED.md lists for H11d, H11e, H11f.
   - The review's S2: two cases for the missing-continuity fault (an EXIT first, and an exclusive TEXT first), failing under the reviewer's mutant my2; a case that fails under my12.
   - Existing cases change only where an amended sentence changes their outcome: list each with the sentence.
6. **Regression.** Round 11's rows and 341 mutants stay as they are or are listed as changed with the reason; legacy outputs byte-identical to 5fd6a519; discovery disabled: every changed line unreachable or behaviour-neutral (say how you checked); the base merge's own contract tests pass unchanged; every test that scripts/check.sh and tests/unit_tests.list run and that touches src/core/, src/merge_capture.* or src/open_snapshot.*, each listed with its result. src/core/pgwt_discovery.h and src/merge_capture.h do not change in this round; if an item cannot be done without changing one, say so and do the rest.

## Excluded

BPF, src/backend*.c, the producer (src/discovery_*.c), the daemon, the server, web/, contracts/, examples.json, KNOWN_FAILING, retries, tolerances, recovery of discovery certification after a fault, any reader change (src/coverage.c, src/open_snapshot.* reader paths), a new persistence format or crash-atomic storage protocol, the retraction-write limit the contract now states (a failed or unwritten revoked OPEN), files named tests/discovery_enum_* (another implementer's).

## Done

Report: commit sha, `git diff --stat ee95f1a6`, for each of items 1-4 the functions that implement it, the 183-row result with the variant count, the crash-cut table, the changed existing cases with reasons, the mutant list with results, the byte comparison, the existing tests run with results, both registration guards, `git diff --check`, every contract sentence you could not implement as written, and the list of scope questions if any.

## Budget

Rounds: 1
Wall-clock minutes: 300

## Report by

At the end of the budget, even if incomplete: at most 15 lines.
