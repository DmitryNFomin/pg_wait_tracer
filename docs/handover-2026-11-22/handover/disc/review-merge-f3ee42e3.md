READY-with-nits

# Review: agent/988-discovery-merge, round 11 (6125c589..f3ee42e3)

Paths. SP = SP
- Tree under test: SP/rv17/wt (`git worktree add --detach SP/rv17/wt f3ee42e3`). Nothing was committed or pushed. I added and later deleted a scratch file, tests/rv17_probe_host.c.
- My drivers: SP/rv17/probe/p17.c. It is appended to the host file with `main` renamed (SP/rv17/build.sh), and it calls only the host's input helpers, which go through the public `pgwt_capture_*` entry points and the views.
- Binaries: SP/rv17/bin. Logs: SP/rv17/logs/p-<schedule>.log.
- My mutants: SP/rv17/mymut17.py, with output in logs/mymut17.log. The implementer sample is in logs/impl-mutants.log. The legacy comparison is legacy.sh with leg-*.out; the other suites are in other.sh with other-*.log.
- Time (date -u): 2026-11-21T20:57Z.

## What changed
- A cookie-zero unresolved disposition that names the EXIT behind a committed matched discharge now revokes that discharge, and the historical MATCHED disposition is kept (B1).
- A valid ADMIT recorded after a fault now counts as `admitted`. That lifetime's records are released uncertified, and ABANDON is refused. After a REARM, a lifetime with no current-epoch attachment proof faults at its first eligible batch, through the new `current_epoch_continuity` input.
- A rejected control records nothing. Quiet candidates survive VOID and REARM. Each claim is refuted once. A late-dated ADMIT is contradictory in either arrival order.
- Discovery VOIDs are now delivered only after the whole input has been evaluated. Before a VOID is exposed, the capture persists the revoked startup OPEN, or a plain rollback OPEN if the attempt failed after END. END now precedes the advancing FRONTIER.

## Evidence
- Check stamp: none exists (no `.pgwt-check.stamp` in the tree, and none in /private/tmp/pgwt-codex/agent%2F988-discovery-merge). The brief forbids `make check`. NOT RUN.
- box-check: no `tests/results/box-check-*.log` exists in the main checkout (zsh: "no matches found"). Host-only review; nothing eBPF-related was run. NOT RUN.
- Host suite (VERIFIED, ASan/UBSan, logs/host.log, rc=0):
  - "180/180 IDs and verbatim inputs/answers match"
  - "180 oracle rows, 217 named cases passed"
  - "236 (180 rows; 65 R tuples; 29 second-amendment variants; 31 H executions)"
  - 467 VOID callback entries were each checked against the views and the persisted reader.
- Guards (VERIFIED): "OK: test-registration guard ... append-only invariant holds" and "OK: runner-registration guard ...". `git diff --check 6125c589 f3ee42e3` gives rc=0.
- Legacy (VERIFIED, logs/leg-*):
  - test_core_merge_contract (127 lines), test_core_merge_adapter (113), test_transport_wire (6) and test_percpu_transport (168) are byte-identical (`cmp`) to the 5fd6a519 build; all rc=0.
  - The base tree SP/rv13/base5f was re-checked file by file against `git show 5fd6a519:` for the eight sources involved.
- Other suites: see "Other suites" below.

## Findings

No blockers.

### S1 (should-fix). merge.c:841-842: after an enable/EXIT contradiction, a separately false terminal control gets no fault
**Contract (DECISION 36, amended):** "A matched terminal control for that same independently bound EXIT, first recorded after this enable/EXIT contradiction was reported, is recorded as refuted ... and adds no report solely for that contradiction; separately false or conflicting terminal controls retain their ordinary fault consequences."

**Mechanism (read from source):**
- `if (a->discovery.enable_exit_refuted) a->discovery.terminal_refuted = true; if (a->discovery.terminal_refuted) return m->report.status;` returns before `discovery_refute_terminal` (:856).
- :856 is the check that faults when the claimed time differs from the bound EXIT stamp.
- The capture's `refuted_on_delivery` (merge_capture.c:1051-1061) only skips recording; it does not fault.

**Reproduced (VERIFIED, logs/p-O90.log, p-O91.log, p-O92.log; `bin/p17 O90|O91|O92`):**
- O9a: `AD_D19, EXIT_D18@L` gives Vu20 (correct). Then `TERM_D17` names EXIT_D18's position, so it claims time 17 for an EXIT stamped 18.
  - Expected: a second `FAULT(unknown_evidence)`, the ordinary consequence for a time-mismatched TERM.
  - Actual: `out='VOID20[C0,unknown_evidence]'`, unk=1, rc=0. No fault. The false claim 17 becomes the retained claim.
- O9b (`EXIT_D18, S19, AD_D19`, rejected) gives the same result: TERM_D17 produces no fault.
- Control O9c (the same EXIT with no ADMIT): TERM_D17 produces `VOID20[C0,unknown_evidence]`.

**Impact:** one missing loud refusal and diagnostic for a false control. The epoch is already suspended and latched, the gap stays open, and the reader says INCOMPLETE. I found no wrong coverage number.

**Not covered:** no row covers this. EXPECTED H11a covers only the matching TERM_D18.

### S2 (should-fix, test gap). merge.c:1043-1044: the new "admitted but no current-epoch proof" fault has no failing case
- My mutant rv_my2 deletes the two lines, and the full host suite stays GREEN (logs/mymut17.log: `rv_my2_drop_admitted_unproved_fault: GREEN(survived) rc=0`).
- Why it survives: H11c's first record is a TRANS. The older bootstrap check at :1054-1059 faults it anyway, so for H11c the new check is redundant.
- The check is not redundant for an EXIT or an exclusive TEXT: :1055-1056 exempt cookie EXITs, and :1045 `continue`s TEXT.
- Reproduced (VERIFIED). O8 = `K, RA(A,B only), S25, AD_D6{current_epoch_continuity=false}, EXIT_D26 (cookie DG), S27`:
  - Product: `... F24[Cr] VOID22[Cr,unknown_evidence]`. This is correct: DECISION 43 says "before publishing any part of that batch".
  - Under rv_my2 (bin/p17-my2 O80): `... F24[Cr] EXIT_D26 F26[Cr]`, meaning the EXIT and the frontier are published.
  - The same mutant with an exclusive TEXT as the first record (O81) publishes the TEXT, and the host's dsink assertion fires.
- Fix: add O8, both variants, as named cases.

### Nits
1. The commit message again ends with `Co-authored-by: Codex <noreply@openai.com>` after `Refs #988`. VERIFIED with `git log -1 --format=%B f3ee42e3 | tail`.
2. Mutant rv_my12 survives. It removes `retained.enable_exit_refuted ||` from merge_capture.c:1057.
   - Under it, the TERM in H11a's order is added to `c->dispositions`, so the accessor would answer MATCHED instead of NONE. No test notices.
   - Related product behaviour (VERIFIED, p-O70.log): with a cookie-less EXIT (`AD_D19, EXIT_D18{no cookie}, TERM_D18` naming it), the TERM is refuted on delivery and never recorded. `pgwt_capture_finish` then reports a second `VOID40[C0,unknown_evidence]` for a "missing disposition".
   - The contract says this TERM "is recorded as refuted" but does not state the accessor value or the finish behaviour for this order, so I do not call it a defect. If the lead wants it decided, it is an adviser question under RULE 1, not an owner question.

## Premise re-runs (all VERIFIED on f3ee42e3, outputs in SP/rv17/logs)

**checks.md Check A** (bin/chkabc chkA, logs/A.log):
- A1 (`K, RA, AD_D6, D12, S19`): admitted=1 right after AD; S19 gives `'VOID20[Cr,counter_change] D12[U] F18[Cr]'`; AB is refused with `VOID22[Cr,unknown_evidence]`.
- A1b releases the unrelated A14: `D12[U] A14[T] F18[Cr]`, then F24.
- C1 and C2 are unchanged.
- This is the Check-B outcome, which is now the product. The P1 hold is gone.

**Check C** (logs/C.log): both orders now fault.
- C-a (`B_D6 gen2, K, RA, D14`): `'VOID20[Cr,counter_change] VOID22[Cr,unknown_evidence]'`.
- C-b gives the identical result.
- Before this commit, C-a gave no fault.

**Check D (real writer and real reader):** SP/rv17/probe/p17.c `d1`. The VOID gap is persisted through `pgwt_cov_tracker_capture_gap`, as the daemon's cov_capture_gap does, onto the same real event_writer as the startup records.
- At the VOID callback entry the views are closed=0, P=1+0, census=0 (pointer), latch=1.
- The reader on the bytes persisted at that moment gives `[6,7)=I [10,18)=I [19,20)=I`.
- Block cuts of the revoking input (logs/p-D1.log):
  - k=4, the file before the input: [6,7)=C.
  - k=5: the revoked OPEN at a=0 is persisted and the VOID gap is not. Result I.
  - k=6: I.
- The quiet/late-TRY variant (p-D2.log) gives the same result. The defect checks.md D measured (complete at the VOID-sink prefix) is gone.

**Previous review:**
- I4a (B1): after the cookie-zero TERMU, `out='EXIT_D18 F18[C0] VOID20[C0,unknown_evidence]'`, `tl='OPEN EXIT_D18 END18 F18[C0] O! VOID20...'`, x removed, P=1+0, reader I. The disposition is still MATCHED (=1).
- I4e: x_U=4 becomes -1; tl `... END5 F7[C0] O! VOID20...`; reader I; disposition still MATCHED.
- I3c and I3c' (S1): a late TRY or WAIT after a non-NULL fire gives Vu, b=6 kept, no O!. This matches H9a and H9b.
- I3e (S2): `AD_D19, EXIT_D18` gives Vu20 at EXIT arrival, with admitted=1 and admission_refuted=1. This matches H1b. I3e' gives Vu20 at AD.

## Own schedules for the amended rules (VERIFIED; `bin/p17 <name>`, logs/p-<name>.log)

**Admission under the latch, two lifetimes (census {D,H}):**
- O10: `K, RA(A,B), S25, AD_D6{cont=1}, AD_H8{cont=0}, D26, H28, S30`. Result: `VOID20[Cr] F24[Cr] VOID22[Cr,unknown_evidence]`. The batch faults before D26 is published, as DECISION 43 requires.
- O11: D26 in its own batch is published `[U]`, then H28 faults.
- O12: both ADMITs arrive before RA, and RA carries D and H. Result: `D26[U] H28[U] F29[Cr]`.
- O13: the same, but RA omits both. Result: Vu22 at the first batch.
- O1d (AD{cont=0} after RA1, then a second fault and RA2 carrying D): `D26[U] F26[Cr]`.
- O1d' (AD{cont=1}, then RA2 omitting D): Vu24.
- O1e (epoch 1, cont=0, no fault): normal result, `D12[T] END6`.
- O1f (AD{cont=0} in epoch 1, then K and RA carrying D): `D26[U]`.

**ABANDON after such an ADMIT:** O20 (K, RA, AD{cont=0}, AB) and O21 (AD before RA) are both refused with Vu22. No later DATA is published.

**A rejected ADMIT followed by a valid one:**
- O30: a factless AD gives Vu. The valid AD is then recorded (admitted=1) with no further fault, and AB is refused.
- O31: H1a's rejected AD_D19, then a valid AD_D6. AD_D6 is recorded; a repeated TERM_D18 adds nothing.
- O32: H3's rejected ADc6, then a numerical AD_D6, which is recorded.
- After RA, in all three: `D32[U] F34`.

**Quiet candidate before and after a fault, contradicted twice:** exactly one Vu, and the later contradictions are silent.
- O40: B gen2, K, D14 while suspended, RA, D15.
- O41: K, RA, B, D14, D15, RA2, D16.
- O42: B, K, RA, D14, RA2, D15, B again.

**Late-dated ADMIT in both orders, then a matching and a conflicting TERM:**
- O50 (AD19, EXIT18), O51 (EXIT18, S19, AD19), O52 (x=18 committed, then AD19, which gives O!) and O53 (AD19, TERM18, then EXIT18): the matching TERM_D18 is silent twice, and the conflicting TERM_D17 faults.
- See S1 for the case where the false TERM is the first terminal control.

**V4, then REARM and progress:**
- O60: `OPEN EXIT_D18 END18 OPEN VOID20[Cw,writer_push_fail]`, then F19, F24 and F29. No second END, reader I throughout.
- O61 (the counter changes after F18 is published): `END18 F18 OPEN VOID20[Ce]`, with the gap open.

## Publication order and crash windows (real writer and reader; block-commit cuts, with meta set to k)

Places the capture writes:
- OPEN: coverage_begin (:749).
- END: discovery_progress (:710), called from the release prepare (merge.c:1261, after all DATA and before the FRONTIER at :1278), from the TERM control (merge.c:862), or after the input.
- Revoked or rollback OPEN: capture_sink (:277-292), before coverage_gap(VOID) (:304) and before sink.emit.
- VOID gap: :304.
- The other plain OPEN writers (:857-861, :1259) are unchanged and are not tied to a VOID.

Results:
- Revoking fault (D1, D2): no cut at or after the VOID gap reads complete (results above).
- Two faults from one observation:
  - D3: `data(12,gen2)`, a seal violation plus a contradiction, with D bound at 6 and H discharged at 18. Blocks: O!0, then GAP_OPEN 12 (seal), then GAP_OPEN 21 (unknown). Every cut from k=5 reads I. P=2+0 at both VOID entries.
  - D3b: a FAULT input carrying a discard_payload record gives the same result.
- Failed write of each kind:
  - FRONTIER after END (D70) and the counter after FRONTIER (D71): `END6 OPEN VOID20`. The VOID entry reads I.
  - END write rejected: covered by the suite's empty_census and V-rows. In my D42 the END had already committed on the preceding control, so that run says nothing about a rejected END.
  - Revoked OPEN rejected (D6): the VOID is refused, `void_failed=1`, the capture returns -5 and keeps returning it.
- Revocation while a release is held (D5: A9 held on TEXT x, then a late TRY): O! comes before the VOID gap; from k=5 the result is I.

**Observed, not reported as a finding** (the brief excludes a crash-atomic storage protocol):
- D7/D71: `AD6, F7, WM8, A9 held, B6`, then TEXT x, with F18 rejected or the counter changing after F18.
- At cut k=5 (END6 persisted, rollback OPEN not yet written), the reader says `[6,7)=C` using the real WATERMARK 8, while the state after the input is open.
- The contract constrains only the prefix at VOID callback entry, and that reads I. A crash in that window is inherent to "END before FRONTIER, END is not commitment".
- Likewise D6: if the revoked OPEN cannot be written, END6 and WM19 stay readable as complete. The capture refuses loudly (-EIO), which is the existing publication-failure rule.

## Interface (pgwt_discovery.h)

**`current_epoch_continuity` (input):** required by DECISION 43: "When that startup ADMIT is delivered after an accepted REARM, its independently qualified proof supplies the current epoch's attachment evidence ... provided it covers continuity through REARM and through the current observation ... Missing or contradicted continuity reports FAULT(unknown_evidence,d,C) at that lifetime's first eligible-batch validation."
- Zero is the safe value: refusal, not trust.
- A caller written against the old header that leaves it zero:
  - Epoch 1: no change. merge.c:888 `epoch == 1`; O1e VERIFIED.
  - ADMIT recorded before a REARM that carries the lifetime's proof: no change (O1f, O12 VERIFIED).
  - ADMIT after a REARM that does not carry the proof: the ADMIT is recorded with no fault, and the lifetime faults at its first eligible batch (O10, O13, H11c). The ADMIT itself never faults.
- That is what DECISION 43 says. Before this commit, such an ADMIT left `admitted=false` and held the frontier indefinitely (Check A).

**State members** (`admission_refuted`, `quiet_claimed`, `quiet_refuted`, `enable_exit_refuted`):
- They implement "Each recorded proof claim is refuted at most once", "Validly recorded claims, including quiet-baseline candidates, survive VOID, REARM ..." and DECISION 36's enable/EXIT rule.
- Their zero value ("not claimed/not refuted") is the initial state.
- `admitted` now equals `admission_recorded` ("The public per-lifetime `admitted` value means that a valid ADMIT has been recorded ..."). The host asserts this at every VOID entry.

**Unchanged (VERIFIED):** `git diff --stat 6125c589 f3ee42e3 -- src/merge_capture.h` is empty, so both accessor signatures and the exit-disposition enum are unchanged.

## Untouched consumers (VERIFIED by grep and reading)
- Outside merge.c and merge_capture.c, nothing in src/ reads `admitted`, the latch or the discovery views. No production code enables discovery: `discovery_enabled` appears only in merge_capture.c/.h, and `pgwt_capture_discovery_control` has no caller in src/.
- open_snapshot.c, open_snapshot.h and event_reader.c are unchanged in this commit.

**Discovery disabled:**
- `pgwt_merge_feed` calls `feed` directly; `defer_void` stays false, so the sink is called inline.
- The quiet list is empty.
- merge.c:1032 is unchanged for the disabled case, and :1043 is gated.
- The moved `discovery_prepare` returns true because the callback is NULL.
- `startup_persistence_failed` is set only in the gated branch.
- The four legacy suites are byte-identical (above).

## Diff read line by line (merge.c, merge_capture.c)
- A fault is now queued while discovery is enabled and delivered after `feed` returns, still inside `pgwt_merge_feed`, so `feeding_data` and the other capture context remain set. The inactive-epoch status is set before the queueing.
- At most two VOIDs are queued; a third sets void_failed. I found no path that produces three faults in one input.
- The TERM path's terminal-claim recording moved after the counter check. It still happens before the VOID callback, because of the deferral.
- `terminal_proved` is now set only after `discovery_refute_terminal` passes.
- With the prepare called before `copy_generations`, `pgwt_merge_discovery_candidate_state` reads the same plan: `planned` is not reset until the next attempt.
- No fault is possible between the TERM-path END (merge.c:862) and the end of that input. INFERRED: the release that follows the control cannot fail its counter checkpoint or publish anything new.

## The enumeration's two reports (INFERRED, not run)
- **V4 "successful startup END after first VOID":** END18 is published (merge.c:1261) in the same input as, and before, the FRONTIER rejection (merge.c:1281-1282), whose VOID now follows the rollback OPEN. EXPECTED V4 requires exactly this timeline. A driver that attributes END and VOID per input, or one written before this order, would flag it. My O60 shows no END after the VOID.
- **"Fault-first" stream differences:** a driver that predates the field leaves `current_epoch_continuity=0`. After its REARM, an ADMIT then faults at the first batch (merge.c:888-889, :1043-1044). And a post-fault ADMIT now releases records it used to hold (merge.c:885; Check A). Either one changes the stream of any schedule that begins with a fault.

## Tests
- H rows assert the complete stream and an ordered timeline string (OPEN, END, O!, FRONTIER, VOID). Every VOID in every case reaches `r11_void_entry`, which checks the views, the revocation count, the reader on the live file and a reopened saved prefix. These are order assertions, not counts.
- My mutants (logs/mymut17.log):
  - RED: rv_my1 (continuity ignored; H11c), my3 (census kept on pointer revocation; N5), my4 (deferred VOIDs reversed; O1), my5 (no deferral; G_r11_fault_two_revocations), my6 (late ADMIT ignores terminal proof; G_terminal_before_admit), my7 (no fault at EXIT arrival; H1b), my8 (B1 requires pid; G_r11_unresolved_exit_identity), my13 (epoch<=2 trusts ADMIT; H11c), my14 (fault clears quiet list; H5a), my15 (prepare after FRONTIER; D01).
  - Survivors: my2 (S2) and my12 (nit 2).
- Implementer mutants: 15 sampled at random from 341 (seed 2026112117), listed in SP/rv17/impl-pick.txt. Result: "15/15 RED; built, reached named row, intended assertion" (logs/impl-mutants.log).
- Changed existing assertions (VERIFIED by reading every removed line):
  - G_counter_control_ADMIT (admitted now true).
  - V4 and lower_bound_failure (ngaps 3 with END and rollback OPEN).
  - G_discharge_frontier_checkpoint (ngaps 3).
  - empty_census rejected close (F15 is now absent because END precedes FRONTIER).
  - D10/V4 detection times.
  - Each traces to the amended DECISION 41 or 43 sentence. None weakens an outcome without one.

## Other suites
All suites returned rc=0 (VERIFIED, SP/rv17/logs/other-*.log, run with SP/rv17/other.sh):
- core_merge_mutants.sh: "36/36 killed".
- core_merge_adapter_mutants.sh: "27/27 killed; guard sweep 31/31 PASS".
- test_core_merge_queue.py: "queue work PASS".
- test_capture_bookkeeping.py: "6/6 killed".
- test_capture_recovery.py: "19/19 killed".
- test_transport_startup.py: PASS.
- transport_diagnostics_mutants.py: "5/5 killed".
- test_exact_perf_coverage.py --mutants: "T-cov host: PASS".

## Not checked
- make check, box-check and anything eBPF-related (excluded).
- 326 of the 341 implementer mutants.
- The 2,242,221-schedule enumeration (not re-run; its two reports are explained above as INFERRED).
- Daemon restart or recovery on a crashed file.
- Byte-level (mid-block) cuts: I cut at block-commit boundaries by rewriting the meta count, which is how the reader decides what is committed.
