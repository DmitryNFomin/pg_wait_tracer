NOT-READY

# Review: agent/988-discovery-merge, round 9 (3e5cb29a..89b6cba4)

All paths are under the scratchpad SP.
- Tree under test: `git archive 89b6cba4` in rv13/tree. The worktree wt-rv13 was clean at 89b6cba4.
- Comparison trees: `git archive 5fd6a519` in rv13/base5f (legacy bytes) and `git archive 249e1c83` in rv13/old (previous round).
- Probes: rv13/probe/probes.c is appended to the host file with `main` renamed (rv13/probe/tests/probe_host.c). rv13/old/probe_old.c and rv13/probe/probe_new_st3.c hold the stale-plan schedule for both trees. My mutants are in rv13/mymut13.py.
- Logs are in rv13/logs: probe.log, probe-old-249e1c83.log, probe-st-89b6cba4.log, disc-mutants.log, mymut13*.log, leg-*.out, tcov-*.log and other-*.log.
- Timestamp (date -u): 2026-11-21T15:22Z.

## What changed
- Every fault in a discovery-enabled capture now sets a permanent certification latch first (merge.c:238). The bound, discharge and closure commit sites test one predicate each. The latch is public as `certification_latched`.
- Each lifetime keeps an evidence summary that is written on receipt and is never erased by VOID or REARM. Before ADMIT, a bounded stamp pool keeps the record origins. Records dropped while suspended, before proof, in a fatal epoch or on allocation failure feed this summary.
- Terminal refutation is sticky and works in either order for every lifetime. One revocation function persists one revoked OPEN per input. A seal violation now revokes a discharge whose EXIT release epoch it invalidates.
- The role-support callback, its fault and the "present, not measured" accessor are removed. The new accessor is `pgwt_capture_discovery_state`. Startup END is now published through a checked "prepare" callback before proofs commit.
- A cookie-bearing EXIT of a never-admitted lifetime is now published (DECISION 40).
- Size: 11 files, +2,450/-352. 123 rows, 65 R tuples, 161 named cases and 234 mutants.

## Blocker

**B1. src/core/merge.c:1707-1723 (`pgwt_merge_discovery_candidate_state`), reached from the TERMINAL path at merge.c:796-803, with src/merge_capture.c:656. A terminal discharge prepared outside a release reads uncommitted plan state from an earlier held release. The startup gap then closes, and stays closed, while another lifetime is still unresolved. The persisted reader answers COMPLETE.**
- **Mechanism (read from source, then reproduced):**
  - `planned[]` is reset only at the start of a non-resumed release (`reset_plan`, :176). A held release returns at :1079 and leaves both of these behind:
    - prospective copies whose `has_lower_bound` came from the validation-only anchor at :1042;
    - `quiet_ready` set at :903.
  - A later TERMINAL control calls `discovery_prepare`. For every lifetime that still has a stale plan slot, the candidate accessor returns that prospective copy (:1715). It also adds a bound for every lifetime with `quiet_ready` (:1719).
  - The preparing branch of `discovery_progress` uses `diagnostic_cutoff` (merge_capture.c:656), not the published frontier. It publishes END, and `capture_core_feed` commits the closure (merge_capture.c:249-250).
- **Contract:**
  - DECISION 41: each inventoried lifetime "remains unresolved until it supplies either a committed lower bound ... or an accepted terminal discharge". "Compute C_close only after census completeness and discharge of every inventory entry."
  - DECISION 43: a closed gap stays closed under the latch.
- **Reproduced (VERIFIED, rv13/logs/probe.log ST1/ST1b/ST2, probe-st-89b6cba4.log ST3).** The census is {D, H} with fence 5. D is never admitted (in ST3, D is admitted with `AD_D6`).
  - ST1 input: `EXIT_D18@L, S(19)`, then `AD_H8`, then an ordinary pid-10 record at 20 whose query-text dependency is unavailable (MA_WAIT), then `H21@P0{final}`, `S(22)` (held), then `TERM_D18`.
  - Expected: D is discharged at 18, H has no committed bound, P=1, the gap is open and the reader says INCOMPLETE.
  - Actual: `x_D=18`, H `b=—`, P=1+0, **closed=1, END at 21**, base_verdict(50,60) = **COMPLETE (0)**.
  - ST1b: add K, REARM and S40. The latch is set, H still has no bound and P=1, but **the gap stays closed and the reader still says COMPLETE**. If H's held record is later lost or refuted, the wrong answer is permanent.
  - ST2 (quiet route): `AD_H21` and `B_H20{e=21}` behind the same held release, then `TERM_D18`. Result: closed at 20 with P=1+0, reader COMPLETE.
  - ST3: D admitted (`AD_D6`), otherwise ST1. Result: closed at 21 with P=1, COMPLETE.
  - Control ST0 (no held H record): the gap stays open with P=1, which is correct.
- **Attribution (VERIFIED, rv13/logs/probe-old-249e1c83.log):** ST3 at 249e1c83 leaves the gap open with P=1. ST1/ST2 at 249e1c83 never publish D's EXIT. **This round introduces the defect** through the new prepare/candidate path.
- **Reachability:**
  - Query-text dependency holds are the ordinary MA_WAIT binding path (merge_adapter). A TERM that arrives after its EXIT was released is a normal order (DECISION 40).
  - Discovery is enabled nowhere in src/. SEARCHED: `discovery_enabled` assignments in src/; the only one is merge_capture.c:548, which copies the capture config. NOT SEARCHED: other branches.
  - The defect is on the path the integration unit wires up.
- **What the fix must cover (not a design):**
  - Outside a release attempt, a prepared closure may use only committed proofs plus the one candidate being committed.
  - Every way of computing the closure must use the same completeness test. The non-preparing branch requires the published frontier; the preparing branch uses diagnostic_cutoff.
  - Named cases ST1, ST2 and ST3, asserting closed, pending, gap history and reader, plus a mutant that restores the stale read.
  - The independent enumeration did not cover a held release, so its clean latch invariant does not exercise this.

## Should-fix

**S1. src/core/merge.c:810 (`a->lifecycle_seen`), newly reachable through merge.c:960 and :975-977. A valid ADMIT delivered after the lifetime's published cookie-bearing EXIT reports FAULT(unknown_evidence) and latches certification for the whole capture.**
- Input: census {D}. `EXIT_D18@L, S(19)` publishes EXIT_D18 and F18, as R11 requires. Then a valid numerical `AD_D6` (generation 1, all facts true), then `TERM_D18, S(20)`.
- Expected (DECISION 36): FAULT only for "contradictory provenance or a claimed admission proof containing a false required fact". The ADMIT has neither, and TERM+EXIT should discharge D as in control A0 (closed at 18).
- Actual: `EXIT_D18 F18 VOID20[C0,unknown_evidence]`, latch set, P=1, the gap never closes. VERIFIED (probe.log A1/A1x: lifecycle_seen=1 at the time of the ADMIT; control A0 closes at 18).
- At 249e1c83 that EXIT was held until ADMIT or a terminal proof, so the ADMIT was accepted. This is fail-safe (loud), but one valid control-versus-data ordering permanently disables certification. Removing the check (my k30) breaks no case and produces no wrong answer in my probes.
- If an ADMIT after a published EXIT is meant to be invalid, DECISION 36 needs that sentence. That is an adviser question under RULE 1, not an owner question.

**S2. tests: the fatal-epoch control path (merge.c:1287-1294) has no failing case.**
- My mutant k28 makes EXIT_REQUIRED drop every discovery control. The whole suite stays green.
- The product is correct today. VERIFIED, probe.log FX2: N1 closed at 6, then FAULT(duplicate_identity) (EXIT_REQUIRED), then a non-NULL or break POINTER. The result is revoked b, a revoked OPEN, census unknown and INCOMPLETE. Under k28, FX2 ends closed with COMPLETE.
- DECISION 43 says "In any epoch status". Add FX2 as a named case with that mutant.

## Nits
1. The commit message ends `Refs #988`, blank line, `Co-authored-by: Codex <noreply@openai.com>`. The brief requires `Refs #988` as the last line, and this is the fifth round in a row. VERIFIED with `git log -1 --format=%B | od -c`.
2. P2 omits the row's final `S(14)` (host.c:2236-2238). The R12 assertion does not check that END 18 precedes the revoked OPEN. The product does produce that order (probe.log R12: OPEN, END 18, revoked OPEN), so the test is weaker than the row, not wrong.

## Brief items 1-6: one place each (VERIFIED by reading, with mutants)
1. **Latch.**
   - The single set point is `fault()` merge.c:238, before queue disposal (:247) and the VOID sink (:276). Every VOID comes from `fault()`; the creation-time VOIDs at :1488 have no capture.
   - Predicates:
     - `discovery_bound_allowed` (:539) gates :903, :1038, :1162, :1226 and the candidate at :1719.
     - `discovery_discharge_allowed` (:544) gates :796, :1171 and the candidate at :1716.
     - `discovery_closure_allowed` (merge_capture.c:627) gates the only `startup_closed = true` (:633) and `discovery_progress` (:637).
   - No other write sets `has_lower_bound`, `terminal_released` or `startup_closed=true`. VERIFIED by grep.
   - REARM never clears the latch: nothing writes the field except :238. Exposed at merge_capture.c:606.
   - Gap B1 is a commit through the closure predicate on wrong candidate inputs. It is not a latch bypass.
2. **Summary.**
   - Written by `discovery_remember_record` (:595) and the top of `discovery_control` (:663-685), with origin reconciliation in `discovery_reconcile_origins` (:581). The feed writes it at :1273-1286, before the EXIT_REQUIRED check and before any validation return.
   - It is fed by every capture drop route: suspended, pre-proof and fatal receipts via merge_capture.c:1274-1288 → `mapped(..,true)` → `discard_payload`, and capture pool exhaustion via `capture_record_full` (:1050).
   - It is monotone: maxima and sticky flags only. `discovery_revoke` clears proofs, never evidence. fault(), REARM and capacity do not touch it.
   - Table full:
     - Core identity table full: allocation_fault, which latches, before the summary is written. That lifetime has no commitments.
     - Stamp pool full: DATA → allocation_fault, which latches. The FAULT-metadata route loses only the per-origin stamp; `attributable_through` and `numerical_seen` are kept.
     - Safe under the latch. INFERRED from :612-626 and :1276-1285.
3. **Refutation.**
   - `discovery_refute_terminal` (:561) runs from the feed (:1279) and from TERM (:794). It has no abandonment restriction, and `terminal_refuted` is sticky.
   - An identical retransmission returns at :784 without a report. VERIFIED: R2 (18 tuples), and r9_expect re-sends every refuted TERM.
4. **Revocation.** `discovery_revoke` (:550) is the only proof-clearing write in the core. VERIFIED: grep of `has_lower_bound =`, `terminal_released =` and `confirmed =`.
   - Callers: late_fault (:306), refutation (:572), embedded witness (:697), POINTER (:713) and feed (:1281).
   - Coalescing: `m->revoking` is reset once per feed (:1270).
   - The OPEN is published once in `capture_core_feed` (:262-275).
   - The census is reset only for pointer revocation of a committed route-c bound (:239-245).
5. **Role support removed.** SEARCHED: publish_no_wait_word, discovery_support, measurement_state, discovery_measurement and reject_support in src, tests, scripts and Makefile. Zero hits.
   - Nothing waits on the removed callback.
   - N14 now rejects the startup GAP_CLOSE, prepared at :1180 before FRONTIER.
6. **Accessor.** merge_capture.h:162 matches the brief's signature exactly. It is read-only (r9_snapshot memcmp).
   - The core also exports `pgwt_merge_discovery_candidate_state`, which is used only during prepare (and is B1's source).

## Previous probes re-run on 89b6cba4 (VERIFIED, rv13/logs/probe.log; every line ends ok(open), bad_count=0 apart from ST)
- **rv6 p8** (5 pointer placements × non-NULL/break, then REARM, S23, a second CONFIRM and S24): no bound and gap open in all 20 snapshots.
  - Placements 0-3: census known, no revoked OPEN.
  - Placement 4 (after closure): one revoked OPEN from 0, census unknown.
  - The census report is now identical for every pre-commit order, so the old should-fix 2 is closed.
- **rv8 P8** (N1 then a contradiction with counter 3, 12 or 0, S14, REARM, census redeclared, S40): b revoked, P=1, not closed, INCOMPLETE. All 6 runs.
- **rv8 P9 and P10** (16 runs): no bound, P=1, gap open, INCOMPLETE.
- **rv11 X3/X3b/X3n** (clean, counter 3, counter 12) and **X3s**: refuted=1, no discharge after REARM+EXIT, P=1, open, INCOMPLETE. X2s-refuted: the same.
- **rv11 should-fix 2** (X3 clean): no discharge. **Should-fix 3 / X7** (admitted, abandoned and never-admitted): VOID, refuted, open.
- **rv11 X8** (the design question): with the latch, a TERM on a counter-faulted observation and a later identical TERM never discharge (P=1, open). DECISION 43's latch resolves the question.

## New attacks
- **Latch in the rest of the faulting observation (VERIFIED, F0-F2).** A counter change at the FRONTIER checkpoint after a quiet, no-word or fresh bound candidate commits nothing.
  - For no-word, the prepared END is followed by a normal OPEN.
- **Latch after REARM (VERIFIED).**
  - L1: quiet b=12 committed while the census was undeclared, then K, REARM and census declared. The gap stays open: the latch forbids a new END.
  - L2: the same with a discharge.
  - LV1: a valid ADMIT and CONFIRM after the latch are recorded without a fault and commit nothing.
- **The latch alone neither reopens nor drops anything.** VERIFIED by R7 and by L1/L2, where the commitments were kept.
- **Revocation:**
  - In a suspended epoch: R3/R4/R5 with q=K.
  - On an EXIT with a different stamp, also while suspended: E1, SE1.
  - In EXIT_REQUIRED: FX1-FX3 and G_fatal_*.
  - All revoke from 0 with exactly one revoked OPEN.
  - A refuted candidate that never committed publishes no revoked OPEN and keeps the census: C1, R2, R10, P10.
  - Source seal: R12 and G_revocation_coalesced, two discharges with one OPEN.
- **Removed callback and N14.** See item 5.

## Changed existing cases (VERIFIED by reading the full test diff; removed lines listed in this review's transcript)
- Each changed case traces to an amended sentence, and none weakens an outcome without one:
  - **D08b guard (EXIT_H18 now published, H pending):** the DECISION 40 never-admitted EXIT sentence. No discharge is still asserted.
  - **G_nw_TRANS (b=6 kept → revoked, P=1, revoked OPEN):** DECISION 43 "An attributable TRANS/MARKER contradicts a permanent no-wait-word proof" plus the any-status revocation. This makes the case stronger.
  - **The second VOID in the pre-commit pointer and NULL-numerical cases:** the controls are no longer silently dropped while suspended (§4 and DECISION 43, "a false claim retains its fault consequences"), consistent with R10.
  - **Support counts set to 0 and presentation asserts removed:** DECISIONs 37(c) and 43.
  - **V1/V2:** a new assertion was added.

## Mutants
- **Implementer's registry (VERIFIED, rv13/logs/disc-mutants.log, ASan/UBSan, rc=0):**
  - "123/123 IDs and verbatim inputs/answers match"
  - R tuple counts 2/18/12/4/12/1/2/6/4/2/1/1 (65)
  - "123 oracle rows, 161 named cases passed"
  - "234/234 RED", with 273 target lines
- **Distinctness:** 234 entries and 234 distinct patches (the runner asserts it, and I re-derived it).
- **Registry diff against 3e5cb29a (rv13/old_tda.py):**
  - 207 → 234 entries.
  - 23 old patches changed. All are re-anchors of removed code, the queue scans and the support callback, onto the replacement logic. Each still targets its original case.
  - One mutant was replaced (no_word_individual_quantities_zero → no_word_inventory_counted_population); 28 are new.
  - Every brief-required mutant exists: per-site latch (prospective, fresh, quiet, no-word, TERM control, release EXIT, END), summary skipped while suspended, abandoned-only refutation, revocation skipped while suspended, latch cleared by REARM.
- **My 31 mutants on the new code, against the full suite and then my probes (rv13/logs/mymut13*.log):**
  - **22 RED:** k01-k05, k08-k18, k20, k21, k23-k25, k31.
  - **Survivors that are equivalent or redundant (read from source):**
    - k06 and k07: each removes one of the two gates on the quiet commit. Removing both (k31) is RED.
    - k19: `exit_conflicting` is also folded into refutation.
    - k22: candidate quiet bound; quiet_ready is already gated.
    - k26: `terminal_refuted` in the bound predicate; every refutation faults, so the latch covers it.
    - k27: the claim at the top of the control is re-recorded at :790-793.
    - k29: the unproved-membership stamp is also caught by `final_discovery_record` or the origin pool.
  - **Real gaps:**
    - k28: the fatal epoch drops controls (S2).
    - k30: the pre-existing `lifecycle_seen` ADMIT check is untested (S1).

## Discovery disabled (VERIFIED)
- Every changed product line is behaviour-neutral or unreachable with discovery disabled:
  - `discovery_control` is reachable only from MA_DISCOVERY_CONTROL, which is built only at merge_capture.c:1010, after the `discovery_enabled` check at :943.
  - The core flag is copied from the capture at :548, and `pgwt_merge_create` is called only at merge_capture.c:551. SEARCHED in src/.
  - Line by line:
    - The latch and the fault() breach at :240 are unreachable: feed returns early on EXIT_REQUIRED (:1267).
    - `position()` returning OK is equivalent, because callers compare only to EXIT_REQUIRED.
    - `final_discovery_record` / `admission_recorded` and the `quiet_head` loops are inert, because they are never set.
    - `discovery_prepare` is NULL.
    - `capture_record_full` returns `capture_full`'s rc.
    - The receipt conditions and `discard_payload` are gated.
    - Pool occupancy is max(0, count).
- **Legacy outputs against 5fd6a519, my own builds:**
  - test_core_merge_contract: 127 lines
  - test_core_merge_adapter: 113 lines
  - test_transport_wire: 6 lines
  - test_percpu_transport: 168 lines
  - All are byte-identical (cmp), with all rc=0.
  - `test_exact_perf_coverage.py --mutants`: byte-identical logs on both trees.
- **Other host tests on the tree (rc=0):** transport_diagnostics_mutants.py (5/5 killed), test_capture_bookkeeping.py, test_capture_recovery.py (19/19), test_transport_startup.py.
- Both registration guards print OK. `git diff --check 3e5cb29a 89b6cba4` is clean.
- **Not run:** make check (read-only brief) and box-check (no box; brief excludes eBPF). This is a host-only review.

## Design questions for the owner
None. S1's contract question goes to the advisers first.

## Notes for the integration unit
- **Latch:**
  - The first fault of any reason (counter, writer, allocation, unknown_evidence, seal, and so on) permanently stops new bounds, discharges and startup END for the capture. REARM restores only ordinary release.
  - Read it from `pgwt_capture_discovery_status().certification_latched`.
  - A gap closed before the latch stays closed, unless a later contradiction revokes it.
- **Accessor:** `pgwt_capture_discovery_state` returns the core per-lifetime state, including the summary fields (`attributable_through`, `terminal_claimed` / `terminal_refuted`, `admission_recorded`). Do not call `pgwt_merge_discovery_candidate_state`: it is prepare-only and is B1's source.
- **Removed publisher:** no role-support or per-process presentation exists. Startup END is published through the checked `publish_startup_gap`. For the no-word route this happens before FRONTIER, and a later FRONTIER failure is followed by a normal OPEN.
- **Recorded in suspended and fatal epochs:**
  - Every control is now processed: TRY, POINTER, WAIT, LIMIT, ABANDON, TERM, ADMIT (recorded; `admitted` only if OK and unlatched) and quiet or no-word CONFIRM (recorded as `quiet_pending`, never committed once latched).
  - False claims fault in every status.
  - Discovery-cookie TRANS/EXIT dropped while suspended, pre-proof or fatal are summarized without payload.
  - EXIT_REQUIRED no longer short-circuits `pgwt_merge_feed` when discovery is enabled. The coordinator must still exit.
- **After the latch:**
  - An ADMIT recorded after the latch leaves that lifetime's TRANS held, so the global FRONTIER stalls until ABANDON (G_latch_REARM_no_ADMIT; LV1: no F26).
  - Lifetimes omitted from REARM attachment proofs lose `identity_proved` and are held as well.
  - The coordinator must ABANDON or include them.
- **Cookie-bearing EXIT of a never-admitted lifetime:** it is now published, so a later ADMIT for that lifetime faults (S1). Deliver ADMIT before such an EXIT can release, or rely on TERM alone.
