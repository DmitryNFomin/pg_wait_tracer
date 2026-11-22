# Design request: the discovery-admission INTEGRATION unit (daemon wiring between the producer and the merge)

Read-only: do not edit any file. Write the design as your final message, at most 160 lines. Mark every statement about existing code VERIFIED (file:line read) or INFERRED. scratchpad = SP

## Context
Discovery admission (contracts/merge-v1/CONTRACT.md §6, DECISIONs 30-45 with clarifications, 108 oracle rows in contracts/merge-v1/oracle-discovery/) admits processes that were already running when a capture starts. It was built as two units on two branches, each from master, each excluding the other:
- MERGE side, complete and reviewed READY-with-nits: worktree scratchpad/wt-discm (748218d7, branch agent/988-discovery-merge). Its last review: scratchpad/a2go/disc/review-merge-748218d7.md. It consumes controls (kinds with the PGWT_DISC_ prefix: CONFIRM_NO_WAIT_WORD, TRY_NO_WAIT_WORD, POINTER, WAIT_ATTEMPT, WAIT_LIMIT, JOURNAL, REBIND, GONE, EXIT_OBSERVATION, ADMIT/CONFIRM, ABANDON_DISCOVERY, terminal dispositions) through its host harness only; nothing in the daemon issues them yet.
- PRODUCER side: worktree scratchpad/wt-pc (6e6034c7, branch agent/988-discovery-producer); one more round is fixing a false loss count and test gaps, with no interface change expected. Its last review: scratchpad/a2go/disc/review-producer-6e6034c7.md; section 6 there, "Notes for the integration unit", lists what the producer exposes (the handoff pgwt_discovery_next and its record kinds, pending counts, the evidence-fault counter outside the 15-counter vector, the exit filter, 37(c) entries with wp_fd == -1, the two clock domains, the unknown authentication deadline) and that the two units' headers conflict.
- Two nits carried from the merge review: merge.c:666 (the CONFIRM_NO_WAIT_WORD pointer guard has no failing test) and merge_capture.c:910 (census known/unknown depends on POINTER order; it fails safe).
- Lead's handoff notes from the merge rounds: the journal time field; POINTER ordering; REBIND is the only call that resolves an enumerated PID; the capture baseline must be at or before F0.
- Both branches are behind master (346ccc95), and the contract commits are on master or in the open contracts PR 997 (agent/988-discovery-contract-2).
- The per-CPU transport these units run on is on branch agent/933-tickfix (0a73d7d0, worktree scratchpad/wt-tickfix), not on master; the plan is a fresh transport branch from master carrying transport plus discovery. Say what your design assumes about that order.

## Not in this unit (do not design them)
The fork-born never-initialised lifetime (an owner decision on what counts as a backend is pending); the A2 bounded-omissions verdict and display; the real-time priority change; the server and the web UI.

## What I need
1. **One interface.** The reconciled header(s): which unit's types win for each concept, what is renamed or deleted, and the translation table from each producer handoff record kind to the merge control(s) it becomes, including every field (clock domain conversions included) and the ordering constraints between controls.
2. **Who issues what, and when.** The daemon-side state machine that turns producer records into merge controls: admission (quiet, census-fence, fresh-record, no-wait-word routes), WAIT_ATTEMPT/WAIT_LIMIT, JOURNAL/REBIND/GONE for unbound PIDs, EXIT_OBSERVATION, the final terminal disposition of every inventoried lifetime (matched or unresolved) and when ABANDON_DISCOVERY is issued. For each: the trigger, the precondition, and what must already have been released or fenced.
3. **Census changes after the first handoff record.** The producer rewrites its census proof in place at cursor 0; a consumer past it never sees a later invalidation. Choose: re-read, or a new producer record kind; say which and why.
4. **Faults.** How the producer's evidence-fault counter and evidence_fault records map to FAULT(unknown_evidence) and to DECISION 43 revocation, and what keeps a fault from being lost between ticks.
5. **Failure and shutdown.** What happens at capture finish with pending entries, with an unresolved terminal, with a full lifetime store, and on daemon exit mid-attempt. No path may fail or raise; each must end in a stated, recorded outcome.
6. **Tests.** The host tests (which oracle rows go end to end through producer records -> daemon translation -> merge -> persisted gap -> the coverage reader), the mutants, and the live qualification on EL8 4.18 and Ubuntu with PG13 and PG18 (what is asserted, what would be vacuous). Include tests that close the two carried nits.
7. **Split.** Whether this is one implementation unit or two (name the files each touches so they can run in parallel without overlap), with an order.
8. **Open questions** you could not settle from the contract, each with your recommended answer.
