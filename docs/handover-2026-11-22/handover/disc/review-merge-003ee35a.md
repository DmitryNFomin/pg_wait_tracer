READY-with-nits

# Review: agent/988-discovery-merge, round 12 (003ee35a; parent ee95f1a6, contracts only, on f3ee42e3)

Paths. SP = SP
- Tree under test: SP/rv18/wt and SP/rv18/mt (both `git worktree add --detach … 003ee35a`), plus SP/rv18/base5f (5fd6a519) for the legacy byte check. I committed, pushed and stashed nothing. The only file I put in a tree was the scratch file tests/rv18_probe_host.c, and I deleted it (`git status --short` is empty in both trees).
- Driver: SP/rv18/probe/p18.c. It extends the previous review's p17.c and is appended to the host file with main renamed (SP/rv18/build.sh). All inputs go through the public `pgwt_capture_*` entry points and the real writer, tracker and reader.
  - My own crash-cut oracle is `icut`. For every block boundary written during one input, it copies the file and sets the writer's committed-block meta to k. It then runs the real reader on every 1 ns cell in [0,100), once on the real bytes and once with a synthetic WATERMARK at 100.
  - It flags any cell the reader calls complete unless the public view after that input says closed and the cell is at or after the closure.
- Logs: SP/rv18/logs/p-<case>.log, host.log (full suite), impl-mutants.log, mymut18.log (my mutants), leg-*.out, other-*.log.
- Time (date -u): 2026-11-21T22:26Z.

## What changed
- The startup-gap END is now the last write of a release attempt. A closing release publishes TEXT/DATA, then the advancing FRONTIER and its counter checkpoint, then END (merge.c:1313-1318). Only plain state assignments follow it.
- A failed attempt writes no END and no rollback OPEN: the rollback branch is removed from capture_sink (merge_capture.c:276).
- Closure by a later terminal control moves out of discovery_control.
  - The control sets `terminal_candidate` (merge.c:870-873).
  - END is then published by release(): either at the end of a full release (merge.c:1338-1341), or by the new `discovery_terminal_only` when the batch is held (merge.c:963-973, 1010, 1183).
- A separately false TERM after a reported enable/EXIT contradiction is now rejected with its own fault and is not retained as the claim (merge.c:815-824).
- A matched TERM whose only dispute is the enable/termination contradiction is now recorded as the final MATCHED disposition, including for a cookie-less EXIT and for a TERM that arrives before its EXIT (merge_capture.c:1050-1058, two clauses removed).

## Evidence
- Check stamp: none for this tree. The main checkout's `.pgwt-check.stamp` is 15c86b15…; `scripts/tree-hash.sh` on 003ee35a gives a53686cc…. The brief forbids `make check`. NOT RUN.
- box-check: none. `ls tests/results/box-check-*.log` in the main checkout returns "no matches found". Host-only review; nothing eBPF-related was run.
- Host suite, ASan/UBSan, rc=0 (VERIFIED, logs/host.log):
  - "183/183 IDs and verbatim inputs/answers match"
  - "183 oracle rows, 226 named cases passed"
  - "239 (183 rows; 65 R tuples; 29 second-amendment variants; 34 H executions)"
  - 1,534 CRASH_CUT lines; 149 of them cut at an END record, across 118 executions.
- Legacy (VERIFIED, `cmp`): test_core_merge_contract (127 lines), test_core_merge_adapter (113), test_transport_wire (6) and test_percpu_transport (168) give byte-identical output on 003ee35a and on 5fd6a519; all rc=0.
- Other suites, all rc=0 (VERIFIED, logs/other-*.log):
  - core_merge_mutants 36/36 killed
  - core_merge_adapter_mutants 27/27, guard sweep 31/31
  - test_core_merge_queue PASS
  - test_capture_bookkeeping (6/6 killed; old/new identity PASS)
  - test_capture_recovery 19/19
  - test_transport_startup PASS
  - transport_diagnostics_mutants 5/5
  - test_exact_perf_coverage --mutants PASS
- Guards (VERIFIED):
  - "OK: test-registration guard … append-only invariant holds"
  - "OK: runner-registration guard …"
  - `git diff --check f3ee42e3 003ee35a` rc=0
  - `git diff --stat f3ee42e3 003ee35a -- src/core/pgwt_discovery.h src/merge_capture.h tests/unit_tests.list` is empty, so the headers are byte-identical to f3ee42e3.

## Findings

No blockers.

### S1 (should-fix, test). host:3253 via r9_term (host:2371-2379): H11e is not executed as ROWS.md states, so the time branch of the new rejection is unpinned
- **ROWS H11e:** "TERM_D17{names that EXIT's source position} … all its other binding facts hold."
- **What the test does:** `r9_term(h,17)` looks for an input EXIT stamped 17, finds none, and falls back to `position=h->pos[3]+1`. That is 2, while the EXIT is at position 1.
  - So the host's TERM_D17 is false by EXIT identity (merge.c:822-823), not by timestamp (merge.c:821).
- **Mutant my2** changes merge.c:821 `exit_stamp != d->terminal_time` to `exit_stamp < d->terminal_time`. The full host suite stays GREEN (logs/mymut18.log, "my2_false_check_only_later_time: GREEN(survived)").
- **The row as written, run under my2** (my Y4: `AD_D19, EXIT_D18, TERM_D17{pos of EXIT}, TERM_D16, TERM_D18`):
  - TERM_D17 adds no fault and becomes the retained claim. TERM_D18 then faults as a conflict and is not recorded.
  - Accessor NONE at the end; expected MATCHED.
- **The product is correct on the row as written** (VERIFIED, p-Y4.log, p-O90.log):
  - TERM_D17 → second Vu with accessor NONE; TERM_D16 → third Vu.
  - TERM_D18 → recorded MATCHED with no fault.
  - A later TERM_D17 conflicts with the retained claim 18 → fourth Vu.
- **Fix:** name the EXIT's actual position in the H11e driver, as the row says.

### S2 (should-fix, test). merge.c:1183: the terminal-only path under a non-cached (discovery) hold has no case
- **Mutant my8** replaces `return discovery_terminal_only(m, obs);` at :1183 with the old `return m->report.status;`. The full suite stays GREEN (logs/mymut18-my8.log).
- **Schedule T51** (VERIFIED, p-T51.log): census {D,H}; `EXIT_H18(cookie)`, `S19`, `D20` (D not yet admitted, so discovery-held), `S25`, `TERM_H18`, `AD_D6`, `S30`.
  - Product: TERM_H18 commits x_H=18, so P goes from 2 to 1. AD_D6 then gives `D20[U] F24 END21` with closed C=20.
  - Under my8: x_H never commits and no END is ever published (`tl='… D20[U] F24[C0] F29[C0]'`). The failure direction is safe, but the gap stays open for the rest of the capture and P stays at 1.
- **Coverage today:** the cached-hold path at :1010 is covered (my7 is killed by G8). The :1183 path is new in this commit and has no case.
- **Fix:** add T51 as a named case.

### S3 (should-fix). merge_capture.c:1517-1518 (made reachable by :1050-1058): the matched cookie-less EXIT that H11f disposes of during suspension is counted as staged
- **Mechanism (read from source):**
  - TERM_D18 now records MATCHED. It faults at the core, and the VOID resets `c->staged=0` (:414).
  - The held cookie-less EXIT is then released and fed to the suspended core, which drops it. But :1517-1518 increments `c->staged`, because only an *unresolved* EXIT is exempted while suspended.
- **Observed (VERIFIED):**
  - **Y14 = H11f exactly:** after finish, `pgwt_capture_pending_records()=1` and `pgwt_capture_stats().unfinished_records=1`.
    - Control Y15 (H11a, cookie EXIT): 0 and 0.
    - H11f EXPECTED says the EXIT payload is disposed "without publication or consumption" and "final state equals the VOID-entry state".
    - The daemon's finish (merge_capture.c:2220) would print "unfinished_records=1; VOID, not complete".
  - **Y12 = H11f + REARM(D proof) + S(26) + finish(40):**
    - Expected (DECISION 40: "capture finish reports no missing-disposition fault for that EXIT"): no fault at finish.
    - Actual: `out='VOID20[C0,unknown_evidence] F25[C0] VOID40[C0,unknown_evidence]'`, unknown_evidence=2.
    - Cause: `pgwt_capture_waiting()` sees staged=1 with an active epoch (:1664, :1706).
- **Reach:**
  - Reachable through the oracle's REARM input (`pgwt_capture_replay_control`, the same entry r11_rearm uses).
  - INFERRED not reachable on the daemon path: `pgwt_capture_rearm` requires a capacity pause (:1757), and both the capacity VOID (:414) and `pgwt_capture_discard_backlog` (:1745) reset `staged`. Production does not enable discovery anyway.
- **Severity:** no wrong coverage (the latch is set and the gap stays open). It is a wrong pending count plus a spurious refusal. Under RULE 2 the lead may treat a wrong number as a blocker; I rated it should-fix because it errs toward refusal.

### Nits
1. The committed-state END route at merge_capture.c:254 has no crash-cut case. In that route `discovery_progress(c,false,…)` publishes END after the input, for example when census completion arrives while a batch is held.
   - The product is correct (VERIFIED, p-CE0.log): `CENSUS(5,proved)` publishes `END6`, every cut reads I until END and [6,8) after it, and CE4 (END rejected) gives `VOID20[Cw]` with no END.
   - r12_cuts never sees this END, so the harness's own guard fires (`ASSERT … every published END execution has every persisted record cut checked (line 122)`).
   - The suite therefore has no execution on this route. The guard is loud, not silent.
2. The commit message again ends with `Co-authored-by: Codex <noreply@openai.com>` (VERIFIED with `git log -1 --format=%B 003ee35a`).

## Premise: every publication path, cut at every record boundary (VERIFIED; `bin/p18 <case>`, logs/p-<case>.log)
"real" means the reader on persisted bytes; "synth" adds a WATERMARK at 100. Every row below has violations real=0 and synth=0.

| Execution | Timeline | Reader on prefixes during the input |
|---|---|---|
| RS0 successful release `AD6 B6 S8` | `OPEN F7 END6` | k=2 (before), k=3 (after WM8): none complete; k=4 (after END): [6,8) |
| RS1 F7 rejected | `OPEN VOID20[Cw]` | none at every cut |
| RS2 counter (lifecycle) after F7 | `OPEN F7 VOID20[Ce]` | none |
| RS3 counter (non-lifecycle) after F7 | `… F7 VOID20[Cr]` | none |
| RS4 END rejected | `OPEN F7 VOID20[Cw]` | none |
| RD0-RD4 (D7 setup: earlier WM8, A9 held on TEXT) | success `… F18 END6`; F18 rejected / Ce / Cr / END rejected all without END | success: [6,19) only at k=6 (after END); failures: none |
| T10 later TERM, EXIT already released | `OPEN EXIT_D18 F18 END18` | [18,19) only after END |
| T14 same, END rejected | `… F18 VOID20[Cw]` | none |
| T20-T24 cookie-less EXIT held until TERM, which releases EXIT, FRONTIER and END | success `EXIT_D18 F18 END18`; F rejected `EXIT_D18 VOID20[Cw]`; counter `… F18 VOID20[Ce]`; END rejected `… F18 VOID20[Cw]` | END last; none complete before END |
| T30/T34 TERM while an unrelated batch is cache-held (terminal-only path) | `… F18 END18`, then TEXT releases A20, F24 | correct; END rejected: none |
| NW0-NW4 no-wait-word route | `OPEN F7 END6 F11`; N14 shape `OPEN F7 VOID20[Cw]`; F rejected; counter | END last; failures none |
| CE0/CE4 committed-state route (nit 1) | `OPEN F7 END6` / `F7 VOID20[Cw]` | correct |
| V0/V1 revoking fault (PTR non-NULL / late TRY after closure) | `… END6 O! VOID20` | only the pre-input cut k=4 reads [6,19) (D43 limit, nothing written yet); every cut from O! onward reads none |
| V2 two faults from one observation (D12 gen2: seal + contradiction) | `… END18 O! VOID20[seal] VOID20[unknown]` | k=4 pre-input [18,19); k=5..7 none |

**V6, failed revoked-OPEN write (DECISION 43 limit, not a defect of this commit):**
- After `AD6 B6 S19` the file holds `WM19 END6`. The late TRY's O! is rejected: rc=-5, void_failed=1, persistence_failed=1. The VOID gap is not written either, because capture_sink returns before coverage_gap (:284-288).
- The reader then says **[6,19) complete**: exactly from P_close=6 to the last persisted WATERMARK.
- On the daemon path it cannot grow (INFERRED):
  - the epoch stays suspended, so no FRONTIER is published;
  - `pgwt_capture_rearm` refuses, because :1757 has no capacity pause for unknown_evidence and :1792 returns -EIO on void_failed;
  - every later capture call returns -5.
- Through the test-only replay REARM it does grow: RA, then S30 publishes F29 and the reader says [6,30) while the view says open.

**XA, END bytes persisted but the publisher returns false (excluded: crash-atomic storage):**
- The capture counts a writer failure and publishes `VOID20[Cw]` with the view open.
- The VOID-entry prefix reads [6,7) complete. The implementer's crash-cut guard fires (host line 544).
- cov_publish_startup_gap (open_snapshot.c:663-666) can return false after appending GAP_CLOSE; I did not check whether those bytes are reader-visible. The contract sentence covers a process stop, not an error return after a durable write. This is a storage-protocol question for the lead, not this unit.

**Previous review's probes, re-run (VERIFIED):**
- D70 (F18 rejected): `OPEN F7 TEXTx A9[T] VOID20[Cw]`; every cut of the failing input reads I. Round 11's END6 / rollback-OPEN window is gone.
- D71 (counter after F18): `… F18 VOID20[Ce]`; every cut reads I.
- O9a (S1): `AD_D19, EXIT_D18, TERM_D17{EXIT pos}` now gives the second Vu (unk=2), and TERM_D16 a third. O9b gives the same result. O9c control is unchanged (one Vu).
- O8/O8' (S2): `VOID20[Cr] F24[Cr] VOID22[Cr,unknown_evidence]` and `… VOID20[Cr,unknown_evidence]`; nothing of the batch is published.
- O70 (old nit 2): disposition MATCHED, and finish adds no fault.
- O71 (same with RA): finish adds VOID40 (S3).

## Is END the last fallible step? (read from source; INFERRED unless stated)
**Release path:** merge.c:1316 → capture_discovery_prepare → discovery_progress → `startup_publish(END)` succeeds. After it:
- merge_capture.c:716-717 (two assignments), then :727 (`*post`).
- merge.c:1319-1341: plain assignments (commit of the plan, quiet list, terminal candidate), then return.
- pgwt_merge_feed delivers no deferred VOID: release runs only when status==OK (:980), and every fault() sets the status inactive, so deferred_count is 0.
- capture_core_feed:250-253: `startup_closed=true` if the latch is clear, and it cannot have become set after END.
- capture_core_feed:254 returns 0 at :675.

**Terminal-only path** (:967-970): the same, with two assignments after the prepare.

**Committed-state route** (:254 → :716-718): commits immediately.

**Fallible step after END, but outside the merge attempt:**
- mapped() calls flush_outputs (merge_capture.c:1543, :455-469). If the output-sink flush fails, it feeds WRITER_REJECTED and a VOID follows.
- Probe XF (VERIFIED): the END stays committed. View: closed=1, latch=1; reader [6,7)=C, with the VOID gap after it, so view and reader agree.
- Only the host's universal "[50,60) complete iff closed" check (host:596) fires, because it does not allow a later non-revoking VOID. Harness limit, not a product defect.

**If the capture's own bookkeeping fails after a successful END write:** only assignments follow, so nothing in that bookkeeping can fail. The publisher-false-after-persist case is XA above.

## Rows H11a/d/e/f in other orders (VERIFIED; logs/Y-all.txt; `bin/p18 Y<n>`)
- **Y0 (H11a):** accessor NONE after EXIT, MATCHED after TERM. A duplicate TERM is silent.
- **Y1 (H11d + duplicates):** one Vu at TERM, accessor MATCHED at once. EXIT adds nothing; the duplicate TERM is silent.
- **Y2:** `AD, K, RA, TERM18, EXIT18`. Vk20 then Vu22 at TERM; EXIT is silent.
- **Y3:** `AD, TERM18, K, RA, EXIT18, S25`. One Vu. After RA, EXIT_D18 is published as an ordinary lifecycle record (F24[Cr]); no discharge and no second fault.
- **Y4:** `AD, EXIT18, TERM17, TERM16, TERM18, TERM17`. Faults at EXIT, T17 and T16, all with accessor NONE. T18 is recorded MATCHED with no fault. The final T17 conflicts with the retained claim 18 and faults. Total 4.
- **Y5:** a false TERM17, then K, RA, then TERM18: MATCHED, no fault.
- **Y6:** `TERM18, duplicate, TERM17`: conflict fault, disposition kept.
- **Y7:** `AD, TERM17` before `EXIT18`: one fault. EXIT at 18 adds none (reconciliation against an already refuted claim). TERM18 conflicts with the retained claim 17 and faults.
- **Y8 (H11f + duplicate):** finish rc=0 with no VOID40.
- **Y9:** two lifetimes; D's EXIT is cookie-less, H's carries a cookie on the same source, so H's EXIT is held behind D's. One fault per lifetime at its TERM, both MATCHED, finish adds no fault.
- **Y10 control:** a cookie-less EXIT with no TERM still faults at finish.
- **Y11:** cookie-less EXIT before the late ADMIT: one fault at TERM, MATCHED, finish clean.
- **Y12, Y14:** S3.

## The enumeration's word `EXIT18, TERM19_CONFLICT, TERM18` (VERIFIED; Z0/Z1/Z2)
**Cookie-carrying EXIT** (Z0, and Z1 with S19 between):
- TERM19 → Vu. merge.c:818's rejection needs `enable_exit_refuted`, so the claim is recorded at :825-831 and refuted at :866/:580 (`claimed=1 t=19 refuted=1`). The capture does not record a disposition (:1056).
- TERM18 → a second Vu. The capture rejects it on delivery (:1051-1052, retained claim 19≠18), and the core faults it as a conflict (:832-835). Accessor NONE.

**Cookie-less EXIT** (Z2):
- TERM19 is recorded as the final MATCHED disposition at 19. The capture cannot see the held EXIT's stamp at :1056.
- The released EXIT then refutes the claim (Vu). TERM18 → second Vu. Accessor MATCHED (the 19 claim); finish is clean.

**Contract sentences the product follows:**
- DECISION 40: "A separately delivered terminal control that conflicts … with a retained final terminal claim reports FAULT(unknown_evidence,d,C), including when the retained claim is refuted and no discharge ever committed". It treats the on-delivery-refuted TERM19 as that retained claim.

**Competing text:**
- DECISION 40: "…including when the claim was rejected on delivery and only its refutation history was retained".
- DECISION 36 (ee95f1a6): "Reject a separately false terminal control before recording a final terminal claim …, so a later otherwise qualified match is not rejected merely for disagreeing with that rejected control". It sits in the enable/EXIT paragraph; the product applies it only there.

**What the suite pins:** the product's reading, through G_r11_rejected_terminal_then_conflict. My mutant my9 removes `enable_exit_refuted &&` at :818; it is RED only there (host:3483). Under my9, Z0 gives one Vu and MATCHED at 18. Classification is left to the other agent.

## Untouched consumers / discovery disabled (VERIFIED by cmp + reading)
- pgwt_merge_feed returns `feed()` at once when discovery is disabled, so the candidate stays NULL.
- `discovery_terminal_only` with a NULL candidate equals the old `return m->report.status`.
- The moved prepare call has no output effect when disabled: discovery_progress returns at :674, and capture_discovery_prepare only round-trips `c->observation`, which capture_sink sets on every output anyway.
- The four legacy outputs are byte-identical to 5fd6a519, and the base merge contract tests pass unchanged.
- Nothing outside merge.c and merge_capture.c consumes `terminal_candidate` or `preparing_release`.

## Diff line by line (merge.c, merge_capture.c)
- **:815-824:** mirrors discovery_refute_terminal's predicate against the incoming control, gated to the first claim after an enable/EXIT contradiction. Correct for the H11e row as written.
- **:870-873:**
  - The candidate is set only if discharge is allowed at control time, and pgwt_merge_feed scopes it to one input (:1606, :1610).
  - :986 (W==0) could drop it only with no released EXIT, so it is unreachable. A counter checkpoint fault at :988-990 latches anyway.
- **:1313-1318:** prepare after FRONTIER and its checkpoint. A failure faults with no END.
- **:1338-1341:** commits the candidate after the plan copy. A same-attempt bound wins, because discharge_allowed checks `!has_lower_bound` after :1322.
- **:1856-1863:** the plan and quiet_ready are visible only in a full release; a terminal-only prepare sees committed state plus its own discharge.
- **merge_capture.c:276:** rollback removed; the revoked-OPEN path is unchanged.
- **merge_capture.c:1050-1058:** two clauses removed, as DECISION 40 requires.
  - Side effect, not checked against a row: a quiet-only claim with e>x now also gets a MATCHED disposition. The core (:853-860, unchanged) refutes only the terminal claim in that case. Read only; no false coverage, because bound_allowed requires !terminal_refuted.

## Tests and mutants
- **Crash-cut table:**
  - r12_cuts covers every committed-block boundary of every input that publishes END or ends non-OK, with every 1 ns cell checked. That is every boundary, not a sample.
  - It excludes inputs that start closed (the revocation limit) and the :254 route (nit 1).
- **Order assertions:**
  - The H rows (34 executions) and nine guards assert the exact combined timeline.
  - All other rows assert order through the dsink/dgap invariants (OPEN first; DATA/TEXT before END; FRONTIER before END; one END per attempt; revoked OPEN before VOID at callback entry) plus exact gap counts.
- **Implementer mutants:** 15 drawn at random from 351 (seed 2026112218; SP/rv18/impl-pick.txt), plus all 12 r12_* and review_my2. Result: "28/28 RED; built, reached named row, intended assertion" (logs/impl-mutants.log, ASan).
- **My mutants in the changed code** (logs/mymut18.log, full suite):

| Mutant | Change | Result |
|---|---|---|
| my1 | drop the :818 block | RED, H11e |
| my2 | `!=`→`<` at :821 | **GREEN (S1)** |
| my3 | drop the :1338 commit | RED, G5 |
| my4 | ignore a failed END prepare at :1318 | RED, N14 |
| my5 | held plan visible to the terminal view at :1856 | GREEN; INFERRED equivalent (a held prospective bound is ≥ the held record's t > the published frontier, so `frontier < close` refuses) |
| my6 | terminal-only path commits nothing | RED, G8 |
| my7 | :1010 old return | RED, G8 |
| my8 | :1183 old return | **GREEN (S2)** |
| my9 (informational) | generalise :818 | RED, G_r11_rejected_terminal_then_conflict |

- **my12 claim:** correct. DECISION 40 now says a TERM "whose only contradiction is the Decision 36 enable/termination contradiction is recorded as the final MATCHED disposition … whether that control establishes the contradiction or follows its earlier report. The exit-disposition accessor returns MATCHED after that recording".
  - my12 (deleting `retained.enable_exit_refuted ||`) is exactly that required change, and the commit makes it (plus the companion `baseline.e > terminal_time` clause needed for H11d).
  - Its inverse, r12_my12_inverse_drop_refuted_MATCHED, is RED on H11a in my run.

## Not checked
- make check, box-check and anything eBPF-related (excluded).
- 323 of the 351 implementer mutants.
- The 2,242,221-schedule enumeration (not re-run).
- Whether open_snapshot's publisher can leave a reader-visible END after returning false (XA).
- The quiet-only e>x MATCHED side effect against any row.
- Daemon restart or recovery on a crashed file.
- Mid-block (byte-level) cuts: I cut at block-commit boundaries via the meta count, as the reader decides.
