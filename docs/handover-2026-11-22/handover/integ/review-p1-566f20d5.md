NOT-READY

# Review: discovery producer P1, commit 566f20d5 (parent 704cb631), branch agent/988-discovery-producer

Reviewer: Claude (Opus), fresh. I did not write this code. I worked read-only on a detached worktree at
scratchpad/rv19/wt. I logged in to no machine and ran no `make check` and nothing eBPF.
Probes and logs are in scratchpad/rv19/ (scripts/probe.py, scripts/probes.c, scripts/probes2.c, mut/*, *.log).
The verdict covers the code as it stands. Linux qualification belongs to the other round.

## What changed
- The producer now hands over typed records. Every record has a `kind` (INVENTORY, INSTALLED, NUMERIC, RESOLUTION, ATTEMPT, NO_WORD, POINTER, TERMINAL, END, FAULT), and cursor 0 is always the CENSUS.
- Each bound lifetime gets a binding token and a discovery cookie when it is first inventoried, before installation. A journal-only lifetime also gets a cookie at creation.
- Records now carry task IDs, namespace, reconciliation-cut and accounting facts, the installed seed (accumulator, on-CPU time), the finality flag (`attempts_ended`), the proof-claim flags and the evidence totals. The pre-enable acknowledgement vector is stored once per generation.
- Fixes from earlier reviews:
  - Fence-only refusals no longer use up the setup budget, and an exhausted setup emits END.
  - PostgreSQL 17 now qualifies and watches the local wait word, as 18 does.
  - Failed pinned reads count a loss only if the process is still alive.
  - The live window now spans 11 s, more than two refresh ticks.
- P2 functions now exist as conservative stubs (checkpoint, refresh_until, binding, stop_attempts).

## Evidence
- Tree: `git rev-parse 566f20d5^{tree}` and `scripts/tree-hash.sh` both print `24ec70632b1510fdb7b582a918c4c6ba2d7ac18e`. That is the "Tested final tree" in the implementer's report. VERIFIED.
- Check stamp: none, because the brief forbade `make check` on the Mac. No box-check log exists. Linux qualification is PARTIAL per /private/tmp/988-p1-evidence/report.md:
  - EL8 final10, live run 7, FAILED at `AssertionError: ('unrelated reuse emitted L record', [2, 0, 2, 2], [2, 1, 3, 2])`.
  - Final-tree full tiers and verifier matrices were not run.
  - I did not re-attribute this failure; that belongs to the other round.
- Mac host suites on the committed tree (VERIFIED, ran; logs in rv19/):
  - `tests/test_discovery_handoff.py` exit 0: `P1 handoff mutants: 78/78 RED at intended assertions`.
  - `tests/test_discovery_producer.py` exit 0: `combined discovery mutants: 81/81 RED`, including the 6 `P1-` defect mutants.
  - `tests/transport_producers_host.py` exit 0: `producer mutants: 29/29 killed`, and `default-off migration: 5fd6a519 54 wire/raw records fingerprint aa355bf875917a3d PASS`.
  - `tests/test_discovery_headers.py`: `2/2 inclusion orders PASS`.
  - `test_discovery_admission` (C, built with cc on the Mac): `discovery engine: PASS`.
- Header: `git show 566f20d5:src/discovery_admission.h | cmp - <(git show ae8b80e9:src/discovery_admission.h)` prints IDENTICAL. VERIFIED.
  - Both inclusion orders also compile with -Werror -pedantic against the merge header at 003ee35a. VERIFIED (rv19/hdr).
- Excluded paths: `git diff 704cb631 566f20d5 -- src/bpf | wc -l` prints 0. No change under contracts/, src/core/, merge_capture.*, daemon.c, backend.c, open_snapshot.c or percpu_transport.*. VERIFIED.
- Guards: test-registration OK, runner-registration OK, and `git diff --check` is clean. VERIFIED (ran).
- UI gallery: not applicable (web/ is untouched).

## Findings

### Blockers

**B1. A number re-bound to a fork-born or non-member holder now produces an INVENTORY record.** src/discovery_producer.c:1709-1733 (`bind_inventory` at :1710 → `publish_inventory` at :1620).
- What the contract says:
  - CONTRACT.md:316 (R) classifies the holder as an ordinary candidate, a fork-route lifetime or a non-member.
  - The merge's own REBIND handling for FORK/NONMEMBER only resolves the number, without inventory (003ee35a src/merge_capture.c, `discovery_resolution`).
  - Spec B maps every INVENTORY record to `inventory(pid,cookie)`.
- New in this commit. At 704cb631, `(!member || fork || (before && bind_inventory(...)))` short-circuited and bound nothing for these two classes.
- Input (host shim, REPRODUCED, rv19/probes.log). PID 42 fails to open at enumeration and is later held by:
  - a fork-born backend (fork_cookie 7), or
  - a non-member (parent 77).
- Expected: NUMERIC, then RESOLUTION R; no INVENTORY for that holder.
- Actual, both cases:
  - `rec[1] kind=INVENTORY pid=42 cookie=8000000000000001 token=1 ... parent=1` (fork case) and `... parent=77` (non-member case), followed by `RESOLUTION res=2`.
  - The entry is then marked `excluded=1 fork_route=1`.
  - The census record says `inventory=0`, so it disagrees with the records that were handed over.
- Effect, read from the merge source (not reproduced end to end): `pgwt_capture_discovery_inventory` registers a new (pid, discovery cookie) lifetime. If the startup gap was already closed, it reopens it (`startup_closed = census_complete = false`). Nothing will ever discharge that lifetime:
  - the entry is excluded, so it never gets INSTALLED or END;
  - in the fork case, its EXIT carries fork cookie g, not the discovery cookie.
  - So the startup gap stays open for the rest of the capture.
- Fix direction: give the R handle a binding token without publishing INVENTORY, or publish only the RESOLUTION.

**B2. The CENSUS record's reconciliation cut is never refreshed, so one transient scan-time fence failure leaves it uncertifiable for the whole capture.** src/discovery_producer.c:1807 (`s->census_proof = fence`), :1858-1866 (refresh completes a cut but does not copy it into census_proof), :1774-1783 (`census_diagnostics` updates `census_fenced` only).
- Input (host shim, REPRODUCED): the scan runs while the helper fence does not complete (`withhold_helper`). Under connection churn, `history()` fails the same way: this is F1's trigger. Then a later refresh reconciles and an exit is noted.
- Expected: either the census stays explicitly unproved, or it is proved and carries the completed cut that established it.
- Actual: `census_fenced=1/1 reconciliation=0 recon_ns=0 recon_count=0`, while the global cut is `1310000000/1`. This is a contradictory record. Spec B requires completed reconciliation before the coordinator may call `pgwt_capture_discovery_census`, so the census can never be applied in this capture. The daemon's own DISCOVERY-CENSUS line shows fence=1 at the same time.
- A related case, also REPRODUCED, which I did not count as a separate finding:
  - The scan reconciles at 1007000000 with a number still pending.
  - The number resolves G at cut 1021000000.
  - The census record then becomes `census_fenced=1/1` but still carries the scan cut 1007000000. Whether that is wrong depends on how the merge reads the cut; see design question 1.

### Should-fix

**S1. `stop_attempts` also refuses ordinary and fork-born backends.** src/discovery_producer.c:1426 (the gate) sits before :1431-1432 (the fork-route `-ENOENT` return). backend.c:387-388 treats anything other than -ENOENT as final.
- REPRODUCED (rv19/probes.log): `PROBE STOP fork_route before=-2 after=-1 ; uninventoried pid before=-2 after=-1`.
- After a stop, every new connection's ordinary watchpoint attach therefore returns -EPERM and is never traced. Spec C calls stop_attempts on permanent failure while the capture continues.
- Not reachable at 566f20d5: `git grep pgwt_discovery_stop_attempts src` shows only the definition. It becomes a data-loss path as soon as P2 or I3 adds a caller.
- Fix: return -EPERM only for an inventoried discovery entry, after the fork-route check.

**S2. A failed pointer-watch enable records a demonstrated continuity break that never happened.** src/discovery_producer.c:1300-1302 sets `a.pointer_continuous = fact(false)` before any observation existed. The spec says "Unknown is not a demonstrated break".
- REPRODUCED (rv19/probes2.log): PG13 no-word route, first enable fails with EIO, the retry succeeds. Records:
  - `kind=8 err=5 pointer_continuous=1/0`
  - then `kind=8 pointer_continuous=1/1`
  - then `kind=7 claims_no_word=1 pointer_continuous=1/1`
- The retained `inventory.pointer_continuous` is overwritten to 1/1 at :829.
- Effect, read from source: the merge makes `pointer_discontinuous` sticky on POINTER (003ee35a merge.c:690-692) and rejects CONFIRM_NO_WAIT_WORD afterwards. So one transient enable failure permanently refuses route c for that lifetime.
- Real-world reachability of an `ioctl(ENABLE)` failure is low (INFERRED).
- Fix: leave continuity unknown when the enable fails.

**S3 (item 4). A fire the observer missed, on a lifetime that exits before the next snapshot, ends in nothing.** src/discovery_producer.c:1061 (`pointer_snapshot` returns before reading the perf count once the task is reaped or exiting), :1871.
- REPRODUCED (rv19/probes2.log): admitted no-word lifetime; set `perf_count_missing`, then reap it before refresh.
- Result: `broken=0 user_evidence_delta=0 records=4`, with no new record.
- The implementer's loss table lists this path as ending in a break plus an evidence total. That holds only while the lifetime is still alive at the next snapshot.
- A second unlisted way: once `s->fault` is set (sticky), `history()` fails forever, so refresh returns at :1861 and no further perf-versus-observer comparison happens. The implementer flagged this as P2 (scope question 3).

### Nits
- N1. Test gaps: three of my mutants survive.
  - R3: the non-init-namespace refusal in `namespace_fact` (:887) is removed.
  - R4: snapshots across a changed perf cut (:1087) are accepted.
  - R9: the contradictory `no_wait_word` tag check in `disc_yield` is dropped.
  - Also, 53 of the 78 P1 mutants overwrite a field at the append site (`emitted(...)`). They prove a case reads the field, not that the producer derives it correctly.
- N2. "P1 detach carried" calls `detach_pointer` on an installed, enabled entry. Production never reaches that state: close_pending detaches only `!entry->installed` (:1418), and enable implies installed. So the loss table's "unexpected detach" row has no production instance; the case tests a helper, not a path.
- N3. `pointer_snapshot` now runs a full helper fence (up to 100 ms) for every live pointer-enabled entry on every refresh tick, and admitted entries keep the pointer watch. With N admitted backends that is N fences per tick on the main loop. The cost is unmeasured beyond 13-14 candidates in the live arm. INFERRED from reading.
- N4. Process: one commit. The brief asked for the header as its own commit and a commit per item. The implementer reports that the wrapper made this impossible.
- N5. The live test now also requires pinned PG17 build IDs (`QUALIFIED_LOCAL_BUILDS`). I did not verify the gate boxes' PG17 builds. This is a loud tripwire, not a silent pass.

## Design questions for the owner
1. Which cut should the mutable CENSUS carry once `census_fenced` becomes true at a later refresh: the scan's completed cut, or the later cut at which the last number resolved? Spec A says "newer totals cannot upgrade an older census cut", and the code currently does exactly that (B2, second case). This needs the spec author's reading.

## Verified as correct
- F1 (VERIFIED, host):
  - The rv14 schedule (6 churn ticks, then quiet) gives `setup_tries_after_churn=0 after_churn_rc=0 final=1`.
  - 40 fence refusals followed by a real attempt gives `rc=0 tries=0 final=1`.
  - Fence refusals interleaved with real local-enable failures end after exactly 4 real failures (`polls_to_end=5 real_failures=4`), and further polls add nothing. The last record is END with `attempts_ended=1 err=EIO`.
  - After the gate (`attempted`), alternating churn adds no physical attempt (`watch_opens=0 seeds=0`).
  - `entry->attempted` is set immediately before `pgwt_discovery_brackets` (:1384), and every failure in between that touches kernel state also sets `s->fault`, which ends setup. INFERRED-safe from reading.
- F2/N2 (VERIFIED, host; kernel ordering INFERRED):
  - Exit during a read does not count on either route.
  - A live failure counts on both routes, and through the bracket path on PG17 (`counted=3` for three failed quiet attempts).
  - The kernel sets PF_EXITING (exit_signals) before exit_mm, so a read failure caused by mm teardown is always followed by `terminal` or a reaped handle.
  - The re-check can hide a real failure only if `disc_status` itself fails on a live task. The attempt still fails, with `a->error` set, so this is fail-safe.
  - PG17 logger role 15 matches the REL_17 BackendType order. PG18 archiver 9 and logger 17 match the Ubuntu live log.
- Header (VERIFIED):
  - Byte-identical to ae8b80e9.
  - The renames keep their values (`_Static_assert` 2/3/4, and consumer values 2/3/6).
  - `pgwt_discovery_address_verdict` and its struct moved to src/discovery_qualification.h, which only discovery_producer.c and the shim include.
  - The BPF-visible part (above `#ifndef __BPF__`) is unchanged.
- P1 facts, verified by reading plus a case (unless the finding notes say otherwise):
  - The cookie is burned once, in `new_inventory`. Re-enumeration and reconcile_journal re-match the existing entry, so nothing burns twice. The old install-time allocation is gone.
  - B vectors: one slot per generation, written once after a completed fence; no other writer (mutant immutable-B RED); retained until close.
  - Tokens are monotone per new entry, with no wrap (mutant token-overflow RED).
  - Seed fields are copied from the actual installed `seed` (the zero-seed case and the 37 ns case pass).
  - The strict `pointer_read_ns > a.e` is decisive (strict-after-enable RED under constant_clock).
  - The pre-existing R route emits ordinary INVENTORY with `start_mono_known` unknown.
  - The append-failure cursor is stable (offset 1, records append-only).
- P2 stubs (VERIFIED by reading and cases): they cannot certify a census, a membership or a shutdown.
  - refresh_until/checkpoint return PENDING, a zero cut, unknown counts, `handoff_complete=false` and ENOSYS, with no work done.
  - binding returns false with zeroed output.
  - `attempts_stopped` reflects only the actual gate.
  - stop_attempts is enforced in scan (:1791), attach (:1426), attach_entry (:1194), the refresh retry loop (:1890) and resolve_numbers. Watches stay active. S1 is about the gate's placement, not its absence.
- Flag off (VERIFIED by reading): every entry point is gated on `exact_perf_discovery` (daemon.c:263/543/1560/1724, backend.c:386), and `pgwt_discovery_start` returns 0 first. No non-discovery code embeds the changed structs. The host default-off fingerprint passes.
- The live test (VERIFIED, diff read) weakens no assertion:
  - `rows()` now also covers NUMERIC/RESOLUTION, which only widens `all(fault==0)`.
  - It adds the kind/cookie assertions, PG17 in the main arms, and the 11 s/16 s window.
  - `--major` is new and is not used by run_all.sh:636.
- Mutants: 12 picked at random (seed 19) from the implementer's 84:
  - counter-vectors-swapped, gone-esrch, handoff-integrity, history_complete, invent-tick-start, kernel-evidence, lost-fire, postmaster, pre-enable-time, pre-enable-valid, reconciliation_count, stop-gate.
  - All are RED at their named assertion (the harness requires `FAIL <assertion>`).
  - lost-fire, stop-gate and the pre-enable mutants change product logic. The rest overwrite a field at append time.
- My own 8 mutants (rv19/reviewer-mutants.log):
  - RED: R1 (disc_word exit read counted; the implementer's mutant changes only disc_resolve), R2 (incomplete cut claims accounting), R5 (bound inventory invents exact start), R10 (J claims ineligible).
  - Survived: R3, R4, R9 (N1).
  - Equivalent: R6, because `history()` succeeds only when `history_known`.

## Item 5: pointer-observation loss paths
| Path (report's table) | Ends in | Basis |
|---|---|---|
| Perf event not reaching the observer | break plus user total while alive; **nothing if the lifetime exits first** (S3) | host probe |
| Failed read on a live task | wait_read_fail, user total, FAULT record | existing case |
| Failed pinned read across exit | nothing, by design (no observation is owed after exit) | host case |
| Full or failed append | writer_push_fail, user total, sticky `handoff_complete=false` | case |
| Detach | break plus total in the helper; no production path (N2) | read |
| Failed enable/re-arm | wp_attach_failures plus user total, plus a false known break (S2) | host probe |
| Kernel read failure/contradiction | BPF WAIT_READ counter or `discovery_unknown_evidence` | read src/bpf on_discovery_pointer |
| Unlisted: refresh returns early on a fence/history failure (permanent under a sticky `s->fault`) | no perf/observer comparison; kernel contradictions still reach the top of refresh | read; implementer's scope question 3 |

## Item 8: `current_epoch_continuity` (answered from the code; not a fix request)
- No fact in the frozen header lets a coordinator set it truthfully for a numerical attachment after a REARM.
  - The ATTEMPT/ADMIT record is a single witness taken at admission.
  - `pointer_continuous` and `local_continuous` describe the pointer and local-word watches, not the wait-word watch (`be->wp_fd`) or its generation state.
  - The evidence and loss totals are global, so after a loss-caused VOID they cannot clear this one lifetime.
- For route c, a POINTER record whose `pointer_fence_ns` is after the REARM, with `pointer_snapshot_valid`, `pointer_continuous={true,true}`, unchanged fires and nonnull since the original baseline, and the same token and generation, would come closest. But P1 emits POINTER only when the observation changes (:1093-1106), so a quiet lifetime gets no post-REARM snapshot. The P2 binding snapshot is likewise untimed.
- Missing: a per-binding, timestamped continuity witness that can be refreshed on demand. For example, a continuity-through time plus a known fact covering:
  - the wait-word watch staying enabled;
  - the generation never being replaced;
  - for route c, pointer coverage;
  - the absence of any applicable loss counted against this lifetime since the original enable.

## Not checked
- Anything on Linux, including attribution of the EL8 run-7 lifecycle-vector failure, the verifier matrices and the full tiers.
- The gate boxes' PG17/PG18 build IDs.
- discovery_binary.c (Linux-only fixture).
- Sanitizers.
- Cost of N3 at scale.
- Whether a merge consumer treats duplicate INVENTORY records as idempotent. I read `pgwt_capture_discovery_inventory` and it matches on (pid, cookie), but did not run it.
