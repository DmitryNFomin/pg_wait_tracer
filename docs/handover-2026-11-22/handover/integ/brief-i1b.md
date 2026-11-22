# Brief: discovery admission, integration round I1 (continued): the coordinator and the translation

## Authorised by

Owner decision, 2026-11-20, recorded verbatim on #988: "a" (build discovery admission). The producer and the merge side exist as separate units; this is the first round of the unit that connects them. Both design advisers agreed the interface in two challenge rounds; its frozen text is SP/a2go/integ/spec-frozen.md (233 lines; read all of it). This round is "I1" in its last section. You are a fresh implementer. A first attempt at this round (ae8b80e9) wrote the frozen header and its inclusion test and then stopped, as its brief told it to, because the coordinator needs one read-only accessor the capture does not have: /private/tmp/988-discovery-integration-I1-report.md (section "Required capture API change"). This brief authorises that accessor; everything else is unchanged.

Issue #988; live comments (refreshed at dispatch):

```console
$ gh api repos/DmitryNFomin/pg_wait_tracer/issues/988/comments --paginate --jq '.[] | {id: .id, body: .body}'
{"body":"**Quiet-baseline mechanism: agreed by both design advisers, and its control passed on both kernels (2026-11-20)**\n\nAn idle process found at startup (an idle pooled connection, an auxiliary asleep) never makes a transition, so it needs a baseline that does not wait for one. Both advisers rejected \"seed, enable, re-read the wait word\" (a transition and its reverse can hide in buffers) and my first replacement (scheduler counters plus the state letter: the kernel sets the sleeping state before it decides to sleep).\n\nAgreed mechanism (Fable found it; Astra had named the same kernel primitive): a bracket in which the kernel itself proves the task was off every runqueue.\n- `/proc/\u003cpid\u003e/syscall` prints \"running\" unless `wait_task_inactive` succeeds under the runqueue lock (not on a CPU, not queued). The task must read inactive at both ends of the bracket, and its voluntary context-switch count must be equal at both ends.\n- Inside the bracket: resolve the wait address and compare it with the armed one, read the wait word, take a fresh timestamp t, install the seed (that wait value, time t, off-CPU, a valid zero-consumption CPU base), enable the watchpoint and confirm.\n- If the bracket holds, the task executed nothing in it, so the seed is its exact state at t and the first later record needs no exclusion. A failed, denied or unreadable proof is a failed bracket, never a pass; after bounded retries the process falls back to waiting for its first record.\n- Today's seeding is not coherent for this (it opens an on-CPU stretch when the wait word is 0 and has no valid CPU base), so the producer unit changes it.\n\nControl on the real kernels (tests/results/session-20261120/quiet-witness/analysis.md on the lead's Mac): on EL8 4.18.0-553 and on Ubuntu 6.8, a sleeper reads inactive with an unchanged count, a spinner reads \"running\" in 200 of 200 reads, a sleeper woken once between the reads shows the count move from 1 to 2, and 1,000 quiet brackets gave 0 anomalies. Not measured: a PostgreSQL backend as the subject, and reads under load.\n\nNext: the consolidated merge-v1 amendment (DECISIONs 30-44 with this route), two independent answer sets from the rows alone, an isolated contract commit, then the producer and merge units.\n\nRefs #988\n","id":7047038197}
```

CONTROLLING SCOPE DECISION: #988 comment 7047038197 "If the bracket holds, the task executed nothing in it, so the seed is its exact state at t and the first later record needs no exclusion."

## Starting point

Run scripts/lead/codex-task.sh on the EXISTING branch agent/988-discovery-integration at ae8b80e9 (the merge side, round 7, plus the frozen header and its test). First run `git log --oneline -1`; it must print ae8b80e9. Otherwise print WRONG_HEAD <sha> and stop.

Commit with messages whose LAST line is "Refs #988". Do not push. Do not run `make check`. Do not touch contracts/. Everything in this round builds and runs on the Mac as host C tests; nothing eBPF runs anywhere in this round. Do not run browser tests.

## What exists and what does not

- On your branch: the merge core and capture with the discovery controls (src/core/pgwt_discovery.h, src/core/merge.c, src/merge_capture.{h,c}) and their host harness and oracle (tests/discovery_admission_host.c, tests/test_discovery_admission.py, contracts/merge-v1/oracle-discovery/). Another round is fixing a defect in src/core/merge.c right now: do not edit src/core/. In src/merge_capture.{h,c} you may add exactly ONE thing: a read-only accessor that returns the core's per-lifetime `pgwt_discovery_state` for a (pid, cookie) through the capture (the core already has `pgwt_merge_discovery_state`), with a host case that covers it; change nothing else in those two files. If the coordinator needs any further change in src/core/ or src/merge_capture.*, do NOT stop at the first one: implement everything that does not depend on it, then list every such change together in the report, each with the row or rule that needs it.
- Not on your branch: the producer. Its implementation lives on another branch (read-only reference: SP/wt-pc, src/discovery_producer.c, src/discovery_attempt.c, src/discovery_admission.h as they are today, BEFORE the header delta). A parallel round will implement the header delta of section A there. You write the header as section A specifies it and compile against that.

## Normative text

- spec-frozen.md: section A (the producer header, exact), section B (the translation table), section C (the coordinator), and the "I1" line of the last section. Every row or cell marked [D3] depends on an owner decision that is still open: implement the publisher as an injected interface with both behaviours selectable (a: a checked publisher must succeed before CONFIRM_NO_WAIT_WORD is issued; b: no publication precondition), default (a), and test both.
- contracts/merge-v1/CONTRACT.md §6 (DECISIONs 30-45) and the oracle rows as in your tree.

## Task

1. **The frozen producer header** is already on the branch (src/discovery_admission.h at ae8b80e9, with its inclusion test). Check it once against section A and fix any difference. It contains exactly the declarations of section A (types, enumerators, the tagged record, the function declarations), no implementation. It and src/core/pgwt_discovery.h must be includable in one translation unit; a host test proves it. If section A cannot be written as specified (a missing type, a contradiction with section B), stop on that member, list it in the report, and choose nothing silently.
2. **The coordinator, pure.** New files src/discovery_integration.{h,c}: a portable module with no daemon, BPF, file or clock access of its own. It receives producer handoff records and checkpoint snapshots through an injected interface, decoded transport observations as plain structs, and "now" as a parameter; it calls the real capture API (src/merge_capture.h) to issue controls. Implement section B in full (every record kind, every field mapping, the three-valued derivation of every proof-mask bit, the ordering constraints, INVENTORY for the live pre-existing rebind class, REBIND only for FORK and NONMEMBER with all four identifiers known) and the parts of section C that need no daemon: the per-lifetime state machine, the checkpoint procedure in its stated order (negative updates before release-capable input, registration before positive census, positive census last), the liveness rule with its exact trigger, the terminal-disposition obligations, and the mapping of every operational error to a recorded outcome (no error is returned as fatal, none is swallowed as success). The terminal join from (cpu, destination, sequence) to the reader position and the generation-membership test are declared as injected interfaces here; their real implementations are the next round's.
3. **Tests (host).** Injected producer records -> the coordinator -> the real capture and merge -> assertions on the controls issued, the authoritative pending counts, committed bounds and discharges. Cover: one case per producer record kind in its valid, unknown and contradictory state; every ordering constraint of section B violated once (the coordinator must refuse or reorder as specified, never forward); the liveness trigger and its negative (a transient incomplete fence does not trigger it); both [D3] behaviours; every operational error of section C's table. Then drive the oracle rows that need no transport and no persistence through the coordinator (the spec's row classes say which are reachable; report the list and count you covered, and why each remaining row is not coverable in this round).
4. **Mutants.** One product mutant per proof-mask bit derivation (unknown treated as true), per enum translation, per strict boundary, per ordering rule, per clock-domain rule, and for the liveness trigger; each builds, reaches its named case and fails its intended assertion.
5. **Registration.** New C tests in tests/unit_tests.list and the runner as their neighbours are; both registration guards pass. All 108 oracle rows and the existing discovery cases and mutants of the merge side stay as they are (run them; report totals).

## Excluded

src/core/, src/merge_capture.* beyond the one accessor, the producer implementation (src/discovery_producer.c, src/discovery_attempt.c), src/bpf/, the daemon, src/backend.c, src/open_snapshot.c, src/percpu_transport.*, any persisted record or reader, the server, web/, contracts/, KNOWN_FAILING, retries, tolerances, the fork-born never-initialised lifetime, the native authentication deadline (stays unknown; WAIT_LIMIT is never issued in production paths).

## Done

Report: commit sha, `git diff --stat ae8b80e9`, the list of section A members you could not write as specified (or "none"), a table of section B's rows with implemented / not and the test that covers each, the oracle rows covered through the coordinator with their count, the mutant list with results, the existing suites' totals, both registration guards, `git diff --check`, and every sentence of sections B and C you could not implement as written.

## Budget

Rounds: 1
Wall-clock minutes: 420

## Report by

At the end of the budget, even if incomplete: at most 15 lines.
