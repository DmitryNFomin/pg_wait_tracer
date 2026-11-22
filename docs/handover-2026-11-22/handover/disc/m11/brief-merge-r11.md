# Brief: discovery admission, merge unit, round 11 (fresh implementer): the third contract amendment, the review's blocker, and publication order

## Authorised by

Owner decision, 2026-11-20, recorded verbatim on #988: "a" (build discovery admission). Round 10 (b5ae3fb6) implemented closure from committed evidence, one resolving proof per lifetime, control ordering and the numeric-only EXIT disposition. Its independent review confirmed those through its own schedules and found one blocker and two should-fix items: SP/a2go/disc/review-merge-b5ae3fb6.md (read it first; probes under scratchpad/rv15/). An independent exhaustive enumeration and four measurements on the built code then found more: SP/a2go/disc/m10/triage.md (causes P1, P2 and the contract gaps) and SP/a2go/disc/m10/checks.md (checks A to D, each with the commands that reproduce it). The contract was amended for all of it (commit 6125c589 on this branch, contracts only; two fresh independent answer sets agree with its expected outputs). You are a fresh implementer. The enumeration, extended, will be run against your result.

Issue #988; live comments (refreshed at dispatch):

```console
$ gh api repos/DmitryNFomin/pg_wait_tracer/issues/988/comments --paginate --jq '.[] | {id: .id, body: .body}'
{"body":"**Quiet-baseline mechanism: agreed by both design advisers, and its control passed on both kernels (2026-11-20)**\n\nAn idle process found at startup (an idle pooled connection, an auxiliary asleep) never makes a transition, so it needs a baseline that does not wait for one. Both advisers rejected \"seed, enable, re-read the wait word\" (a transition and its reverse can hide in buffers) and my first replacement (scheduler counters plus the state letter: the kernel sets the sleeping state before it decides to sleep).\n\nAgreed mechanism (Fable found it; Astra had named the same kernel primitive): a bracket in which the kernel itself proves the task was off every runqueue.\n- `/proc/\u003cpid\u003e/syscall` prints \"running\" unless `wait_task_inactive` succeeds under the runqueue lock (not on a CPU, not queued). The task must read inactive at both ends of the bracket, and its voluntary context-switch count must be equal at both ends.\n- Inside the bracket: resolve the wait address and compare it with the armed one, read the wait word, take a fresh timestamp t, install the seed (that wait value, time t, off-CPU, a valid zero-consumption CPU base), enable the watchpoint and confirm.\n- If the bracket holds, the task executed nothing in it, so the seed is its exact state at t and the first later record needs no exclusion. A failed, denied or unreadable proof is a failed bracket, never a pass; after bounded retries the process falls back to waiting for its first record.\n- Today's seeding is not coherent for this (it opens an on-CPU stretch when the wait word is 0 and has no valid CPU base), so the producer unit changes it.\n\nControl on the real kernels (tests/results/session-20261120/quiet-witness/analysis.md on the lead's Mac): on EL8 4.18.0-553 and on Ubuntu 6.8, a sleeper reads inactive with an unchanged count, a spinner reads \"running\" in 200 of 200 reads, a sleeper woken once between the reads shows the count move from 1 to 2, and 1,000 quiet brackets gave 0 anomalies. Not measured: a PostgreSQL backend as the subject, and reads under load.\n\nNext: the consolidated merge-v1 amendment (DECISIONs 30-44 with this route), two independent answer sets from the rows alone, an isolated contract commit, then the producer and merge units.\n\nRefs #988\n","id":7047038197}
{"body":"**Owner decisions, 2026-11-21 12:11 UTC, verbatim: \"B b a\"**\n\nAnswering three a-or-b questions put in this order:\n\n1. **The three PostgreSQL 13 helper processes with no wait state (logger, statistics collector, archiver).** Options as put: a = list each as \"present, not measured\" (needs a new record in the capture files, reader rules and a contract amendment); b = do not list or count them, and say so in the documentation. Answer: **b**.\n2. **What counts as a backend.** Options as put: a = count every connection process wherever PostgreSQL keeps a wait state; b = count a process only while it holds a PostgreSQL process slot, on every version (health checks, cancel requests and silent clients are then \"not a backend\"). Answer: **b**.\n3. **Real-time priority for the tracer's main loop** (issue 989). Options as put: a = on by default wherever the per-CPU transport is on, with `--no-rt-priority` to turn it off; b = opt-in only. Answer: **a**.\n\nWhat follows from 1 and 2 here:\n- Contract merge-v1: the no-wait-word route (DECISION 37(c)) keeps its proof and its bound but loses the durable support-publication precondition and the per-process \"present, not measured\" line (DECISION 43); rows N1, N10 and N14 get new expected answers through the usual two independent answer sets. Documentation sentence: \"On PostgreSQL 13 the logger, the statistics collector and the archiver never hold a PostgreSQL process slot. They are not backends: they are not listed and not counted.\"\n- The rule for fork-born processes that never initialise (contract delta 3b) is finalised under the process-slot rule.\n- No new record kind is added to the capture files for this.\n\nCopied here from the comment on the churn issue (comment 7062393007 there) now that no implementation round in flight cites this issue. State on 2026-11-21 17:55 UTC: the contract part of 1 is on master (6f18da28: the no-wait-word route without role-support publication; rows N1, N10, N14); the process-slot population rule of 2 is written as a draft delta and belongs to the bounded-omissions unit, not applied yet.\n\nRefs #988\n","id":7066825885}
```

CONTROLLING SCOPE DECISION: #988 comment 7047038197 "If the bracket holds, the task executed nothing in it, so the seed is its exact state at t and the first later record needs no exclusion."

## Starting point

Run scripts/lead/codex-task.sh on the EXISTING branch agent/988-discovery-merge at 6125c589. First run `git log --oneline -1`; it must print 6125c589. Otherwise print WRONG_HEAD <sha> and stop.

The LAST line of every commit message, and of .codex-commit-msg, is exactly "Refs #988" (round 10's ended with another trailer: do not repeat that). Do not push. Do not run `make check`. Do not touch contracts/. Everything builds and runs on the Mac as host C tests; run nothing eBPF-related and no browser tests.

**Do not stop on a scope question.** If an item needs something this brief does not allow, finish every other item and list all such questions together in the report.

## Normative text

contracts/merge-v1/CONTRACT.md sections 3, 4 and 6 and contracts/merge-v1/oracle-discovery/{ROWS.md,EXPECTED.md} as at 6125c589 (180 rows). `git show 6125c589 -- contracts/merge-v1/CONTRACT.md` shows exactly what changed against the text round 10 implemented: section 3, section 4 and DECISIONs 36, 40, 41, 42, 43 and 45. contracts/merge-v1/oracle-discovery/compare-delta7.md says why.

## What was measured on b5ae3fb6 (not instructions)

- After a counter fault and REARM, a valid ADMIT for D followed by a record of D and source progress produced only the fault's VOID: no DATA, no FRONTIER, and an unrelated process's record was held too; ABANDON released the frontiers and dropped D's record (checks.md A).
- With the bytes cut after a revoking fault's VOID was written and before its revoked OPEN, the real reader returned complete for windows the revoked proof had covered; after the call returned, incomplete (checks.md D). The public views read closed with one pending inside that VOID callback.
- In one release attempt the FRONTIER is published before the END (review of delta 7b, SP/a2go/disc/delta7/fable-challenge7b.md section 2); the row tests count ENDs, not their order.
- A quiet candidate recorded before a fault is dropped by the fault; the same candidate recorded after it is kept (checks.md C).

## Task

1. **The review's blocker B1 (rows H10a, H10b).** An unresolved disposition naming the EXIT that backs a committed matched discharge faults and revokes that discharge whatever cookie it carries; the recorded MATCHED disposition is not replaced and nothing is consumed twice.
2. **Admission recorded under the latch (DECISIONs 43, 45; rows H2a-H2e, H11c).** One per-lifetime fact, "a valid ADMIT was recorded", exposed as `admitted` by `pgwt_capture_discovery_state`; it removes the missing-ADMIT hold, that lifetime's records release under the ordinary rules, uncertified, and commit no bound; it bars abandonment; nothing rearms or releases by itself during suspension; missing continuation evidence faults at the lifetime's first eligible-batch validation as the amended text says.
3. **Rejected and refuted claims (DECISIONs 36, 40, 43; rows H1a-H1d, H3a-H3d, H4a, H4b, H5a, H5b, H6a-H6c, H9a, H9b, H11a, H11b).** A control rejected for its own facts or ordering records no accepted claim, admission, route, baseline or disposition (a route-(c) ADMIT is checked against retained evidence before anything is recorded); a recorded quiet candidate survives VOID and REARM as evidence in both arrival orders; each recorded claim is refuted once; an ADMIT dated after its lifetime's EXIT is contradictory in either arrival order and the EXIT-backed discharge is revoked (the review's S2 is the bound side of this rule); a later matching terminal control is recorded as refuted with no further report; a second conflicting terminal control faults before any commitment.
4. **Publication order (section 4, DECISIONs 41, 43; rows H8a, H8b, V4, and every existing row that revokes).** At entry to a VOID callback the epoch is no longer active. Within one release attempt: supporting TEXT/DATA, then END, then the advancing FRONTIER. Before a revoking fault's VOID is published its revoked OPEN is persisted and every public view (status, state, pending split, census, latch) already reflects the revocation; when one observation raises two faults all its revocations complete before the first VOID. If an attempt fails after its END was published, a plain startup OPEN is persisted before that failure's VOID.
5. **Validation while held (section 3, DECISION 42; rows H7a, H7b).** Confirm against the amended sentences; change only what differs.
6. **Tests.**
   - All 180 rows through the persisted record and the reader, every variant a row names, asserting the complete expected output, with the ORDER of OPEN, END, revoked OPEN, FRONTIER and VOID asserted (not counts).
   - For every execution that contains a revoking fault or a failed attempt after END: at entry to each VOID callback, read the public views AND run the reader on the bytes persisted so far; both must already say what EXPECTED says. A mutant that persists the revoked OPEN after the callback, one that updates the views after it, and one that publishes FRONTIER before END are each red.
   - The review's S1 (a case that fails under the reviewer's mutant my1) and its mutants my6 and my9; the mutants EXPECTED.md lists for the new rows plus one per item above.
   - Existing cases change only where an amended sentence changes their outcome (for example the case that pinned "a counter fault refuses admission"): list each with the sentence.
7. **Regression.** Round 10's rows and 288 mutants stay as they are or are listed as changed with the reason; legacy outputs byte-identical to 5fd6a519; discovery disabled: every changed line unreachable or behaviour-neutral (say how you checked, in particular for the section 4 order: the base merge's own contract tests must pass unchanged); every test that scripts/check.sh and tests/unit_tests.list run and that touches src/core/, src/merge_capture.* or src/open_snapshot.*, each listed with its result. Keep both accessor signatures and the exit-disposition enum exactly as they are.

## Excluded

BPF, src/backend*.c, the producer (src/discovery_*.c), the daemon, the server, web/, contracts/, examples.json, KNOWN_FAILING, retries, tolerances, recovery of discovery certification after a fault, a new persistence format or crash-atomic storage protocol, files named tests/discovery_enum_* (another implementer's).

## Done

Report: commit sha, `git diff --stat 6125c589`, for each of items 1-5 the functions that implement it, the 180-row result with the variant count, the callback-entry table (execution, view state, reader result), the changed existing cases with reasons, the mutant list with results, the byte comparison, the existing tests run with results, both registration guards, `git diff --check`, every contract sentence you could not implement as written, and the list of scope questions if any.

## Budget

Rounds: 1
Wall-clock minutes: 480

## Report by

At the end of the budget, even if incomplete: at most 15 lines.
