NOT-READY

# Review: agent/988-discovery-merge, round 8 (44612ec2..249e1c83)

All paths are under the scratchpad SP.
- Tree under test: `git archive 249e1c83` in rv11/tree (worktree clean, HEAD 249e1c83aa28). Comparison tree: `git archive 44612ec2` in rv11/old44/tree.
- Probes: rv11/probe/probes.c and rv11/probe/probes2.c, each appended to that tree's own host file with `main` renamed (rv11/probe/{new,old44}/tests/probe*_host.c), built with ASan/UBSan.
- Logs: rv11/logs (probe-249e1c83.log, probe-44612ec2.log, probe2-new.log, probe2-old44.log, disc-mutants.log, oldmerge-counter.log, leg-*.out, tcov-*.log).

## What changed
- `discovery_control` (src/core/merge.c:532-535) now returns after the counter checkpoint only on EXIT_REQUIRED. A counter-faulted control is then handled like a control that arrives in an already suspended epoch.
- The suspension guard moved below the TERMINAL block (:646-648). A matched TERMINAL is now recorded while the epoch is suspended, whether a separate CHECK or the control's own observation suspended it. It discharges an already committed EXIT only while the epoch is OK (:638).
- A false completed CONFIRM_NO_WAIT_WORD claim now faults ahead of the guard (:607-610), as quiet CONFIRM already did (:604-606).
- 42 new named cases (24 pointer placements and 18 per-kind cases) and 4 new mutants. 15 mutant entries with identical patches were coalesced.

## Blocker

**B1. src/core/merge.c:532-535 together with :617-634. A counter fault on a matched TERMINAL control now hides the TERMINAL's contradiction, and the refuted match later discharges the lifetime and closes the startup gap.**
- **Mechanism (read from source):**
  - `check()` → `fault()` (:226-237) discards the whole staged queue.
  - Only after that does the TERMINAL block run. It sets `terminal_proved`/`terminal_at` (:624-625), then scans the now-empty queue for staged records of this lifetime stamped after `terminal_at` (:629-634). That scan can no longer fire.
  - At 44612ec2 the early return dropped the TERMINAL, so no proof survived.
- **Contract:**
  - DECISION 40: "A contradictory match supplies no discharge."
  - DECISION 42: "contradictory evidence retains its existing fault consequences."
  - DECISION 43: "Preserve existing loss counters and fault precedence."
- **Reproduced (VERIFIED; probe-249e1c83.log X3 and X3b, probe2-new.log X3s and X3n; same probes on 44612ec2 in probe-44612ec2.log and probe2-old44.log):**
  - Input: census {D} with fence 5. `AD_D6` (numerical route), `D20@P0` (staged, W≤20), then `TERM_D18` whose observation carries counter 3 (Cr). Then REARM(21), `EXIT_D18@L`, S(25), S(26).
  - Expected: the counter VOID plus `VOID20[Cr,unknown_evidence]` for the contradiction, no discharge, P=1, gap open, reader INCOMPLETE.
  - Actual at 249e1c83: `VOID20[Cr,counter_change] EXIT_D18 F24[Cr] F25[Cr]`, terminal_released=1, P=0+0, census known, **GAP_CLOSE at 18**. `base_verdict` is **COMPLETE (0)** for [50,60) and [99,100). No unknown_evidence fault is ever reported.
  - At 44612ec2, the same input gives `VOID20[Cr,counter_change]`, P=1, gap open, INCOMPLETE.
  - The same new outcome occurs with counter 12 (Cs, X3s), for an abandoned lifetime (`AB_D` instead of `AD_D6`, X3b), and for a never-admitted installed identity (X3n).
- **Reachability:**
  - SEARCHED: MA_DISCOVERY_CONTROL and PGWT_MERGE_*_DISCOVERY producers in src/. They are produced only at merge_capture.c:981, behind the `discovery_enabled` check at :901.
  - SEARCHED: assignments of `discovery_enabled` in src/ and tests/. It is set only in tests/discovery_admission_host.c.
  - Not user-reachable today. It is on the path the integration unit wires up.
  - NOT SEARCHED: other branches.
- **What the fix must cover (not a design):**
  - On a counter-faulted TERMINAL, the contradiction checks must see the evidence the core had received before the counter VOID discarded it.
  - A match refuted by that evidence must not later discharge. See finding 2, which has the same lines and must be fixed with B1, or the fix is incomplete.
  - Named cases: X3, X3b and X3n with a non-lifecycle counter change, asserting output, pending count, gap records and the reader after REARM + EXIT. Add a mutant that restores the current order.

## Should-fix

**2. src/core/merge.c:624-634, pre-existing at 44612ec2 (VERIFIED on both trees). A refuted TERMINAL still leaves `terminal_proved` set, so a later EXIT discharges the lifetime.**
- **Mechanism:** :624-625 store the terminal proof before the contradiction checks at :626-634 return the fault.
- **Input:** X3 without the counter change, i.e. `AD_D6, D20@P0` staged, `TERM_D18` → `VOID20[C0,unknown_evidence]` (correct so far). Then REARM, `EXIT_D18`, S(25), S(26).
- **Expected:** no discharge (DECISION 40).
- **Actual:** `EXIT_D18 F24 F25`, terminal_released=1, P=0, GAP_CLOSE at 18, reader COMPLETE.
- **Same result for:** an abandoned lifetime (X3b clean) and a never-admitted identity (X3n clean).
- **Why it matters for this round:**
  - This round makes the defect reachable through two more doors.
    - The counter-faulted TERMINAL in B1.
    - TERMINAL during a suspended epoch. Previously it was dropped at the guard; it is now retained (X2s: `TERM_D18` during a CHECK-suspended epoch, REARM, `EXIT_D18` → close at 18; at 44612ec2 the gap stays open).
  - X2s itself is correct (DECISION 40: "A disposition delivered before the EXIT it names applies when that EXIT is consumed"). What it enlarges is the reach of a refuted proof.

**3. src/core/merge.c:509, pre-existing (VERIFIED on both trees, probe2 X7). A TRANS that arrives after a matched TERMINAL is checked only for abandoned lifetimes.**
- **Input:** `AD_D6, TERM_D18, D20@P0, EXIT_D18, S(25)`.
- **Expected:** FAULT(unknown_evidence). D20 contradicts the match (DECISION 40).
- **Actual:** `EXIT_D18 D20[U] F24`, b_D=20, terminal_released=1, GAP_CLOSE at 21, reader COMPLETE. No fault.
- **Scope:** not introduced by this round. Listed because it is the arrival-order twin of B1/2 and the same fix area.

## Nits
1. The commit message again ends with `Co-authored-by: Codex …` after `Refs #988`, and a `---` line precedes it. The brief requires `Refs #988` as the last line. This is the fourth round in a row. The report's claim that the wrapper preview "retains Refs #988 on the last line" does not match commit 249e1c83 (VERIFIED: `git log -1 --format=%B`).
2. merge.c:666-668: the `claims_completed` branch is now unreachable, because :607-610 returns first. It is harmless.

## Brief items, verified

1. **The B1 of the previous review is closed. VERIFIED by running (probe-249e1c83.log).**
   - P8 (counters 3, 12 and 0; non-NULL and break):
     - After the contradiction: `F5 F7 VOID20[K,counter_change] VOID20[K,unknown_evidence]`, b absent, P=1+0, revoked OPEN at boot (det 20), INCOMPLETE.
     - After REARM, census redeclaration and S40: census known, **not closed**, P=1+0, INCOMPLETE for [6,7), [50,60) and [99,100).
   - P9 (counters 3 and 12, both variants): after REARM, CONFIRM_NW and S23, the output is `… VOID22[K,unknown_evidence]`, no bound, P=1+0, gap open, INCOMPLETE.
   - P10 (all four pre-commit placements × counters 3/12 × both variants): no bound, P=1+0, gap open, INCOMPLETE everywhere.
   - 44612ec2 merge.c restored into the 249e1c83 tree (rv11/m-oldmerge, oldmerge-counter.log):
     - All 24 `G_counter{3,12}_nw_*` cases go RED on "counter and pointer forbid bound and discharge".
     - 35/42 new cases are RED in total. The 7 green ones are CONFIRM, CONFIRM_NW, JOURNAL, REBIND, GONE, EXIT and UNRESOLVED, which are exactly the kinds whose outcome the round did not change.
   - The registered mutant `counter_fault_drops_control` is RED on all 34 of its targets (disc-mutants.log).

2. **Per kind.** "New path" means a way to commit a bound, discharge a lifetime, close the gap or advance a frontier that did not exist at 44612ec2. Each probe was compared on both trees.
   - **ADMIT** (route-c, numerical): still refused at the guard on a faulted or suspended observation. The false-fact check (:595-603) now VOIDs on a faulted observation too, as it already did in a suspended epoch (DECISION 36). No new path. VERIFIED (G_counter_control_ADMIT/ADMIT_FALSE).
   - **Quiet CONFIRM:** the false claim VOIDs, and a valid witness is still dropped (X6, X6v). This is unchanged from a suspended epoch at 44612ec2. No new path. VERIFIED.
   - **CONFIRM_NO_WAIT_WORD:** in an OK epoch the reordered check is equivalent to the old :666-668, the same condition with the same fault. INFERRED from source; all oracle rows pass.
     - In a suspended epoch a false claim now VOIDs (X5: `VOID20[Cr,unknown_evidence]`, silent at 44612ec2). This matches §4 ("Each separately delivered LOSS/FAULT … VOID even if … suspended") and quiet CONFIRM.
     - An honest failed witness and a valid witness stay silent and are dropped. After REARM without redelivery, no bound (X5). No fault where the contract wants silence, and no new path. VERIFIED.
   - **TRY_NO_WAIT_WORD:** watch and deadline are retained (X4). They only add refusals: after REARM, a WAIT_ATTEMPT on the watched NULL candidate now VOIDs (`VOID22[Cr,unknown_evidence]`; accepted silently at 44612ec2), consistent with DECISION 38 and the counter-clean behaviour. The deadline only feeds the `waiting_limit_exceeded` diagnostic. No new path. VERIFIED.
   - **POINTER:** retained evidence only refuses bounds (B1 above). A retained non-NULL fire on a pending numerical candidate makes it eligible for an ordinary 37(a)/(b) attempt (DECISION 38). The fire itself supplies no bound, and 44612ec2 already retained POINTER in a suspended epoch. No new path. INFERRED from source.
   - **WAIT_ATTEMPT / WAIT_LIMIT:** diagnostics only (G_counter_control_WAIT/LIMIT). No new path. VERIFIED.
   - **Matched TERMINAL:** **new path** (B1, and finding 2's reach).
     - The retention itself is conformant where no contradiction exists. X2 (counters 3 and 12) and X2s (clean TERMINAL while suspended) discharge after REARM + EXIT and close at 18, while 44612ec2 held the EXIT forever.
     - With lifecycle counter 0 the census becomes unknown and nothing closes (X2 counter[0]).
     - **Is the retained proof still proved?** For non-lifecycle counters, DECISION 31 says they "do not by themselves invalidate an established census". The disposition's binding comes from capture-retained history, which the capture already stored for faulted TERMINALs at 44612ec2 (merge_capture.c:943-977). The core now agrees with the capture. INFERRED. The exception is the contradiction check (B1).
   - **ABANDON:** accepted on a faulted observation (X1). Release stays suspended. VOIDed records D9 and D12 are not counted as discards (discards=2: D17 and the terminal D18, matching row A18's rule). The gap closes only after a post-REARM TERMINAL + EXIT discharge (G(19), increment from the discarded D18). With counter 0 nothing closes.
     - The end state is byte-for-byte the same as X1c (AB after a separate CHECK), whose end state is the same on 44612ec2 (VERIFIED).
     - DECISION 45 holds: no loss is forgiven, there is no auto-REARM, and closure needs completed fences and discharge. No new path.

3. **Unchanged kinds.** JOURNAL, REBIND, GONE and EXIT_OBSERVATION are decided in the capture (merge_capture.c:827-897), never in `discovery_control`.
   - With a counter delta, `reliable` is false (:834-838). The control degrades to a CHECK that VOIDs, and no new resolution is made.
   - Established identities and numeric resolutions are never undone. This is what DECISION 31 requires ("authorizes none of these resolutions"; "retain established inventory identities and numeric-observation resolutions").
   - TERMINAL_UNRESOLVED stores its capture disposition (:943-977) before the CHECK.
   - Nothing required is dropped. VERIFIED by reading; the G_counter_control_{JOURNAL,REBIND,GONE,EXIT,UNRESOLVED} cases pass and are green under the old merge.c.

4. **Mutants, rows, cases and legacy (VERIFIED by running):**
   - Registry diff (rv11/cmpmut2.py, which imports both registries):
     - Old: 218 entries, 203 distinct patches. New: 207 entries, 207 distinct patches.
     - **0 old patches lost.** The 15 removed entries were all byte-identical patches (14 groups), and every old (case, assertion) target is retained under the survivor.
     - The one changed target is `o_capture_terminal_conflict`, which now asserts capture history instead of complete output. The reason is genuine: core retention now masks the capture guard in the output.
     - Four new patches.
   - The new mutants are RED for the intended reason:
     - `counter_fault_drops_control`: 34 targets, including the 24 pointer placements on the bound assertion.
     - `counter_terminal_history_dropped`: TERMINAL, on the retained-history assertion.
     - `counter_terminal_commits_prior_EXIT`: TERMINAL_AFTER_EXIT, on the discharge assertion.
     - `counter_no_word_false_claim_ignored`: CONFIRM_NW_FALSE, on the output assertion.
   - Full run with ASan/UBSan (disc-mutants.log, rc=0):
     - "108/108 IDs and verbatim inputs/answers match"
     - "108 oracle rows, 143 named cases passed"
     - "discovery mutants: 207/207 RED"
     - 255 RED target lines
   - Byte comparison against 5fd6a519 (my own builds of both trees):
     - test_core_merge_contract: 127 lines, ddf62059
     - test_core_merge_adapter: 113 lines, 636f53fc
     - test_transport_wire: 6 lines, 4d9390d3
     - test_percpu_transport: 168 lines, 2eae924a
     - `test_exact_perf_coverage.py --mutants`: 3116 bytes, 4a916bf4
     - All are identical.
   - Both registration guards print OK. `git diff --check 44612ec2 249e1c83` is clean.

5. **Discovery disabled.** Every changed product line is inside `discovery_control`. It is reached only from MA_DISCOVERY_CONTROL envelopes, which are built only at merge_capture.c:981, after the `discovery_enabled` check at :901. VERIFIED by grep of src/ (the list is in this review's transcript) and by the byte-identical legacy outputs. The TERMINAL block no longer clears `held_cached` in an OK epoch. That is neutral: every prefix decision that depends on terminal state either forces a replan (:802/:919) or is rechecked by the TERMINAL's own queue scan. INFERRED from source.

## Design question (contract silent; the brief said stop and report, the implementer chose)
- **The question:** should a matched TERMINAL whose own observation faults on a non-lifecycle counter discharge an EXIT that already released in a committed, fault-free attempt?
- **Contract:** the text gives only a necessary condition ("only when … the complete release attempt commits without fault", DECISION 40 and the clarification). That condition is met here.
- **Implementer's choice:** never (merge.c:638; G_counter_control_TERMINAL_AFTER_EXIT asserts the lifetime stays pending and the gap stays open for the rest of the capture).
- **Probe X8 (VERIFIED, both trees):** an identical duplicate TERMINAL after REARM does discharge (P=0, G(18), COMPLETE). So today the answer depends on whether the daemon retransmits, which DECISION 40 makes idempotent.
- **Who decides:** the advisers/owner, under RULE 1.

## Notes for the integration unit
- **Recorded on a faulted observation or in a suspended epoch:**
  - TRY (watch, deadline)
  - POINTER (count, non-NULL, break, route-c revocation)
  - WAIT_ATTEMPT count
  - WAIT_LIMIT diagnostic
  - ABANDON
  - matched TERMINAL history
- **Not recorded and silently dropped:** a valid ADMIT, quiet CONFIRM or CONFIRM_NO_WAIT_WORD. After REARM the coordinator must redeliver them, or the lifetime stays pending (X5, X6v).
- **A matched TERMINAL after its EXIT committed:** if it arrives on a faulted or suspended observation, it discharges only if retransmitted after REARM (X8). This depends on the design question above.
- **REARM and discovery lifetimes (INFERRED from merge.c:1267-1271, :796-803):** REARM clears `identity_proved` for every generation not listed in its attachment proofs. An admitted discovery lifetime omitted from the REARM proofs has its TRANS held with no DATA or frontier, and nothing re-proves it (ADMIT is refused once admitted, :650). Only an EXIT matching a terminal proof passes.

## Design questions for the owner
The TERMINAL_AFTER_EXIT question above; nothing else.
