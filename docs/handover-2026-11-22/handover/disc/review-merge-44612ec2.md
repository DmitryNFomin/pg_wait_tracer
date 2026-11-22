NOT-READY

# Review: agent/988-discovery-merge, round 7 (748218d7..44612ec2)

Every path below is under the scratchpad base SP.
- The tree under test is `git archive 44612ec2`, extracted to rv8/tree. The worktree was clean.
- The probes are in rv8/probe/tests/probe_host.c: the host file with `main` renamed, plus probe scenarios P1–P10. The same probe file was also built against `git archive 748218d7` in rv8/probe748.
- The logs are in rv8/logs.

## What changed
- A non-NULL pointer fire or a pointer-continuity break now resets the census and publishes a "revoked" startup gap only when the no-wait-word lifetime already has a committed bound (`merge_capture.c:911-913`). A contradiction that arrives before commitment still faults and still prevents the bound, but it leaves the census known and makes one startup-gap publication.
- There are 14 new named host cases (101 in total). Each places a non-NULL fire or a break before TRY, before ADMIT, before CONFIRM, before commit, after commit, after commit with another lifetime still pending, and before REARM followed by CONFIRM.
- There are 4 new mutant entries (218 in total).

## Blocker

**B1. src/core/merge.c:532-535, exposed by src/merge_capture.c:911-913.**
- **What goes wrong.** Suppose a POINTER contradiction is delivered with an observation that also carries an independent, non-lifecycle counter change.
  - The core returns from the counter check before it records the pointer evidence.
  - After the normal explicit REARM, the CONFIRM guard at :666 (the guard finding 1 was about) finds nothing to refuse.
  - The route-c bound commits and the startup gap closes. The persisted reader then reports base coverage as COMPLETE for a lifetime whose claim of never using a wait word was contradicted.
- **Contract.** DECISION 43 gives the pointer contradiction its consequences "in addition to independently established counter faults". DECISIONs 37(c) and 43 forbid a bound and require the lifetime to stay pending.
- **Reproduced** (rv8/logs/probe3.log P9, probe4.log P10; VERIFIED by running):
  - Input: TRY_NW_D6, route-c ADMIT, then POINTER(9, non-NULL) with the snapshot's counter 3 (wait_read_fail, as in row N15) at 1. Then REARM(21), CONFIRM_NO_WAIT_WORD, S(23).
  - Expected: no bound and P=1+0. The gap stays open, and `base_verdict` is INCOMPLETE for [6,7), [50,60) and [99,100).
  - Actual: `VOID20[Cr,counter_change] F22[Cr]`, b_D=6 committed, pointer_nonnull=0, P=0+0, census known, closed. GAP_CLOSE is published at 6 (detected 22), and `base_verdict` is **COMPLETE (0)** for all three windows.
  - The same result occurs with counter 12 instead of 3, and with a continuity break instead of a non-NULL value.
- **Attribution** (VERIFIED: same probe, rv8/logs/probe-748218d7.log and probe4-748218d7.log):
  - The root cause (the early return before the `PGWT_DISC_POINTER` case) predates this round.
  - At 748218d7, the before-CONFIRM placement was masked: the old trigger reset the census, so the gap stayed open and the reader answered INCOMPLETE.
  - **This round removes that mask, so the before-CONFIRM placement now certifies coverage.**
  - The before-TRY and before-ADMIT placements give the same COMPLETE answer at both commits (pre-existing).
  - The before-commit placement stays INCOMPLETE at both commits.
- **Post-commit variant** (P8; pre-existing, same cause):
  - Input: N1, then PTR_D12(non-NULL) with counter 3 changed, S14. The capture revokes, but the core keeps b_D=6 and reports P=0+0, whereas N5 requires P=1+0.
  - After REARM(30), a fresh census declaration and S40, the gap re-closes at 6 and the reader answers COMPLETE.
  - With a lifecycle counter (0, 1, 2 or 10), `lifecycle_lost` latches, the census cannot be re-established, and the result is safe (P1).
- **Reachability.** Discovery is enabled nowhere in shipped code. SEARCHED: `discovery_enabled` in src/ and tests/; it is set only in tests/discovery_admission_host.c. NOT SEARCHED: branches other than this one. The defect is therefore not user-reachable today. It is on the path the next unit (daemon integration) wires up.
- **What the fix must cover (not a design).**
  - Retain the pointer evidence, and its effect on the bound, even when the same control's observation faults on counters.
  - Name cases for every placement with a non-lifecycle counter change, plus the post-commit REARM-and-redeclare case.
  - Add a product mutant that restores the early return.

## Brief items, verified
1. **Finding 1 is closed as asked, for the inputs it names. VERIFIED.**
   - G_nw_{nonnull,break}_REARM_CONFIRM run TRY_NO_WAIT_WORD, route-c ADMIT, POINTER (non-NULL, or a break), REARM(21), CONFIRM_NO_WAIT_WORD, then boundaries at 23.
   - They assert the complete outcome:
     - no bound, not confirmed, not terminal-released
     - evidence retained
     - P=1+0, gap open, census known
     - exact output `VOID20[C0,unknown_evidence] VOID22[C0,unknown_evidence]`
     - SUSPENDED
     - exactly one startup-gap publication, no supporting publication
     - reader INCOMPLETE for [0,1), [6,7) and [99,100)
   - Guard removed by me (rv8/m1, the full line at merge.c:666): **2/209 cases red**, exactly the two REARM_CONFIRM cases. All 195 pre-round cases stay green (rv8/logs/sweep-m1.log).
   - Removing only `pointer_nonnull` reds only the nonnull case, and removing only `pointer_discontinuous` reds only the break case (sweep-m1a, sweep-m1b), so both halves of the guard are covered.
   - Limit: B1 shows that the guard can be bypassed when its input evidence was never retained.
2. **Finding 2 is closed for counter-clean controls. VERIFIED.**
   - The trigger is the committed core state `no_wait_word && has_lower_bound`, read before the control is applied. On the attachment, `has_lower_bound` is written only by committed release paths (merge.c:1052, :1062). For a no-wait-word lifetime, TRANS is inconsistent (:511), so the only route is the quiet commit at :1062. INFERRED from source.
   - Pre-commit placements (before TRY, ADMIT, CONFIRM and commit, and REARM+CONFIRM): census known, one gap publication, no bound. Post-commit: revoked from 0 with detection 20, census unknown, P=1+0 (P=2+0 when H is also pending), output `F9[C0] VOID20[C0,unknown_evidence]`.
   - Old trigger restored by me (rv8/m2): **6/209 red**, namely G_nw_{nonnull,break}_{before_CONFIRM, before_commit, REARM_CONFIRM}, with 0 pre-round cases red. The before-TRY and before-ADMIT cases cannot distinguish the old trigger from the new one, because `no_wait_word` is still false there.
   - Further probes (probe.log):
     - P4: two post-commit contradictions give exactly one revoked publication.
     - P2/P2b: a post-commit contradiction while suspended, or after REARM, with counters unchanged, revokes correctly.
     - P7: CONFIRM, PTR, ADMIT keeps the census known.
     - P6: a failed read is not treated as a contradiction.
   - The only under-revocation found is B1.
   - "(or a closed startup gap)": not implemented as a separate trigger. P3 (route-c ADMIT, terminal discharge closes at 9, then PTR non-NULL) now gives VOID20 with the gap still closed and the census known; 748218d7 revoked in this case. DECISION 43 revokes only after an accepted 37(c) bound, and DECISION 41 accepts the terminal discharge, so I read the new behaviour as contract-conformant. INFERRED; no oracle row covers it.
3. **Rerun on the committed tree. VERIFIED** (rv8/logs/disc-mutants.log, rc=0, ASan/UBSan):
   - "108/108 IDs and verbatim inputs/answers match"
   - "108 oracle rows, 101 named cases passed"
   - "discovery mutants: 218/218 RED"
   - Each `_nonnull`/`_break` mutant pair applies a byte-identical patch, so there are 216 distinct product mutations. My split sweeps cover the difference.
   - Byte comparison against 5fd6a519, my own build of both trees (VERIFIED):
     - test_core_merge_contract: 127 lines, ddf62059
     - test_core_merge_adapter: 113 lines, 636f53fc
     - test_transport_wire: 6 lines, 4d9390d3
     - test_percpu_transport: 168 lines, 2eae924a
     - `test_exact_perf_coverage.py --mutants`: 3116 bytes, 4a916bf4
     - All are identical, and all match the implementer's hashes.
   - The test- and runner-registration guards print OK, and `git diff --check 748218d7 44612ec2` is clean.
4. **The 5 production lines.**
   - With discovery disabled, `pgwt_capture_discovery_control` returns -EINVAL at merge_capture.c:900, before the changed lines. VERIFIED by reading, and by the byte-identical legacy outputs above.
   - The only defect involving them is B1 (they remove the census reset that had masked the core's dropped evidence).

## Pilot tests
- `test_pilot_route.py` builds SERVER_SRCS (Makefile:159-178, parsed by tests/server_build.py), plus tests/pilot_fixture_writer.c and the src/core evidence files.
- None of merge_capture.c, merge.c or the discovery tests is a build input, so this diff cannot affect those tests. VERIFIED by reading. I did not run Chromium.

## Nits
1. The commit message's "Refs #988" is still followed by a `Co-authored-by` trailer, although the brief requires it on the last line. This is the third round in a row.

## Design questions for the owner
None.
