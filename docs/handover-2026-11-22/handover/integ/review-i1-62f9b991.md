NOT-READY

# Review: discovery-admission integration I1 (coordinator), 62f9b991 (base 44612ec2)

Reviewer: Claude (Opus), fresh context; did not write the code. Read-only on the worktree.
Probes and logs: SP/rv12/
(probe.c/probe1.log, probe2.c/probe2.log, probe4.c/probe4.log, mut.py + spec1-3.json -> mut1-3.log, distinct.py).
Every probe copies src/ and tests/ into a temp dir and edits only the copy.

## Evidence (re-run at 62f9b991, 2026-11-21 ~13:20Z)
- `python3 tests/test_discovery_integration.py`: 173 named cases PASS, 86 oracle rows. VERIFIED (rv12/base-run.log).
- `--mutants`: 86/86 RED. VERIFIED (rv12/mutants-run.log). All 86 mutated texts are distinct. VERIFIED (rv12/distinct.py).
- ASan/UBSan run: 173 PASS. VERIFIED. `tests/test_discovery_headers.py`: 2/2 inclusion orders PASS. VERIFIED.
- `tests/test_discovery_admission.py` (existing): 108 oracle rows, 101 named cases. VERIFIED (rv12/existing.log). Its 218 mutants: NOT re-run (implementer's log only).
- Both registration guards OK; `git diff --check 44612ec2 62f9b991` clean. VERIFIED.
- `.pgwt-check.stamp`: none in the worktree. No `tests/results/box-check-*.log`. Not run: the brief excludes `make check`, and the round is host-only. So no gate evidence exists for this tip.

## Blockers

### B1. A clock contradiction is turned into PREEXISTING proof. A post-F0 lifetime without fork provenance gets ordinary INVENTORY, and the census becomes complete
- Where: src/discovery_integration.c:57-65 (`preexisting()`; `either(exact,bound)` at :63) and :382-388 (`preexisting_r`). Reached through `resolve()` :741-745 -> `inventory()` :405-438.
- Input: a NUMERIC observation for pid 30, then a RESOLUTION R record with qualified exact monotonic start `lifetime_start_ns=F0+5` (`start_mono_known` true). The same record carries a qualified boot-clock bound that says the lifetime is older than F0: `clock_bound`, `predates_boot` and `start_upper_ns(F0+300) < boot_lower_ns(F0+400)`. All other evidence is valid.
- Expected (spec C error table "Clock/namespace/identity contradiction -> Unknown-evidence fault; affected proof withheld"; spec B "a post-F0 target lacking fork provenance remain[s] pending"): the coordinator records a contradiction fault, withholds PREEXISTING and leaves the number pending.
- Actual, reproduced (rv12/probe1.log, probe A):
  - The translation gives PREEXISTING known=1 truth=1.
  - The merge's own classifier says `ma_discovery_exit` = MISSING_FORK (3).
  - The coordinator still issues ordinary INVENTORY. The capture records `start=1005 > F0=1000`.
  - Results: `unresolved_numeric` 1 -> 0, `census_complete=1`, 0 contradiction outcomes, 0 faults.
- Why it is a blocker: the coordinator certifies the census on contradictory evidence. That violates the fail-safe rule: refuse loudly, never trace garbage. Kleene OR is correct for false-or-unknown. It is wrong when both alternatives are qualified and disagree.
- Required fix:
  - When both the exact start and the bound are qualified and disagree, report a CONTRADICTION fault and treat PREEXISTING as unknown.
  - Or let a qualified exact start decide.
  - Either way, `preexisting_r` must not pass a target that `ma_discovery_exit` classifies MISSING_FORK.

### B2. `pgwt_di_receive` keeps every lifecycle EXIT of every backend in a fixed registry that is never discharged. After `capacity` ordinary exits the capture is suspended with ALLOCATION_FAIL
- Where: src/discovery_integration.c:1208-1213. Every MA_LIFECYCLE EXIT is passed to `pgwt_di_terminal_received`, whatever its cookie. :625-644 appends to `i->terminals` and faults with CAPACITY at :642. No entry is ever removed, including MATCHED and UNRESOLVED ones.
- Input: coordinator capacity 4, one registered discovery lifetime (pid 30). Five ordinary fork-born backends (pids 60-64, fork cookie 1, below COOKIE_MIN) each send FORK and then EXIT through `pgwt_di_receive`.
- Expected: these are not discovery obligations, so no coordinator state is consumed and no fault occurs. Control G0 feeds the same schedule directly to the capture: 0 allocation faults, status OK.
- Actual, reproduced (rv12/probe1.log, probe G): the terminal registry grows 1,2,3,4. The fifth exit gives `capacity_outcomes=1 tracking_failed=1 alloc_faults=1 capture_status=1` (suspended).
- Why it is a blocker: `pgwt_di_receive` is this unit's public receipt API, tested by the receipt_* cases. Once I2/I3 call it per decoded record, connection churn suspends every capture after `capacity` exits (at most 262,144), and certification stays blocked for the rest of the capture.
- Required fix:
  - Retain only EXITs that can be discovery terminal obligations: cookie-less EXITs, or cookie >= COOKIE_MIN that matches a registered lifetime.
  - Release an entry once its disposition is final and consumed.
  - Add a churn case: many more ordinary exits than capacity, no fault.

## Should-fix

### S1. The candidate registry is never pruned and keeps one entry per (pid, selector). It overflows below the declared capacity
- src/discovery_integration.c:895-908, :1079-1081. RESOLUTION and TERMINAL records share one array of `capacity`. The key includes `numeric_resolution`, so one number can take up to 4 slots. Resolved and matched entries are never removed.
- Input: capacity 4, three numbers (3 <= 4). Each gets a RESOLUTION PENDING record and later a qualified G record.
- Expected: three GONE controls, numbers resolved.
- Actual (rv12/probe1.log, probe B): `GONE issued=0 unresolved_numeric=3 capacity_outcomes=2 tracking_failed=1 certification_blocked=1 alloc_faults=2 status=1`.
- The failure mode is safe (it refuses), but it is a false allocation failure on legitimate input.

### S2. A final ADMIT that the merge refuses is re-sent: twice in the same batch, then once per later batch. Each re-send publishes another fault/VOID
- src/discovery_integration.c:552-560, together with the end-of-batch retry loop :1092-1095. The loop retries whenever `final_valid && attempts_ended && !confirmation_sent`, and a refused ADMIT never sets `committed.admitted`.
- Spec B ATTEMPT: "send one numerical ADMIT".
- Input: a matched TERMINAL at F0+18 is accepted. Then a final ATTEMPT arrives with e=F0+25, which the merge refuses because e > terminal_at. Then three empty batches.
- Actual (rv12/probe4.log): ADMIT attempted 2 -> 5, VOID outputs 2 -> 5, unknown_evidence faults 2 -> 5.
- The same thing happens in mode Q (rv12/probe2.log probe Q: two ADMITs and four VOIDs in one batch).
- Expected: one ADMIT, one recorded refusal. `attempted_controls` already holds the bit, so use it to suppress the re-send.

### S3. Running out of the shared deadline while releasing staged records is not recorded
- src/discovery_integration.c:1164 `if(deadline && i->now>=deadline) return;` returns without recording anything.
- Input: one staged TRACE record; `pgwt_di_receive(..., now=F0+20, deadline=F0+20)`.
- Actual (rv12/probe2.log probe D): `staged=1 incomplete=0 deadline_outcomes=0`.
- Expected (spec C "Shared deadline exhaustion -> Incomplete shutdown and remaining obligations recorded"): a PGWT_DI_DEADLINE outcome and `incomplete=true`.

### S4. Mutation testing of the translation and mask code: 32 of my 42 mutants survive
Full list in rv12/mut1.log and mut3.log. Each mutant is a plausible single-operand edit. The implementer's per-bit mutants force the result to true at the `bit()` sink, so they cannot detect a dropped conjunct.
- Killed (10): R02, R08, R09, R15, R17, R21, R27, R31, R34, R37.
- Equivalent, or masked end-to-end by a capture/core re-check (7): R20, R22, R23, R32, R33, R36, R39. INFERRED from reading the capture guards.
- Real gaps (25), each surviving all 173 cases:
  - Spec-named rules:
    - R38: REBIND is issued for any exit class, not just FORK/NONMEMBER. Probe H in rv12/probe1.log shows the current code is correct; no test pins it.
    - R35: the census `registered` gate is dropped.
    - R42: `preexisting_r` no longer requires known journal ineligibility. This bypasses J precedence on the INVENTORY path at :422.
    - R30: `preexisting_r` drops parent==postmaster.
    - R41: the census invalidation-count check is dropped.
  - Clock order and strictness:
    - R12: the INSTALL_BEFORE_ENABLE order is dropped.
    - R24: no-word visibility is compared with the numerical `e` instead of `pointer_enabled_ns`.
    - R10: RECONCILED drops `e <= cut`.
    - R19: `completed` drops `fence <= cut`.
  - Dropped operands:
    - R01: PREEXISTING history.
    - R03: predates_boot.
    - R04: fd_bound.
    - R05: postmaster_member.
    - R06: evidence_fault in accounting.
    - R07: handoff_complete in accounting.
    - R11: RECONCILED COMPLETE.
    - R13: ENABLED generation.
    - R14: cookie >= MIN.
    - R16: on_cpu_ts==0.
    - R18: fires accounting.
    - R25: no-word PINNED continuity.
    - R26: OWNERSHIP history.
    - R28: LIFECYCLE vector.
    - R29: address stable/relocated.
    - R40: `read_ok` NULL fact.
- The current code is correct at each of these points (VERIFIED by reading). These are coverage gaps: any future edit at these points passes the suite.
- Fix: add a `pgwt_di_translate` table test that turns each operand unknown and false in turn, and add end-to-end cases for R38, R35 and R42.

## Nits
- N1. An INVENTORY record with pid 0 is dropped without an outcome (:416). Probe Z in rv12/probe2.log: 0 contradiction, 0 unclassified, 0 faults. The safe outcome only comes from the census count mismatch.
- N2. A RESOLUTION R record with cookie 0 that passes `preexisting_r` is reported as allocator exhaustion (:417 CAPACITY/ENOSPC). For an R record this is a contradiction, not exhaustion. Read from source, not reproduced.
- N3. A retained eligible J is re-delivered to the capture on every batch: JOURNAL attempted 1 -> 4 over 4 batches (rv12/probe1.log probe E). It is idempotent today (0 faults) but is O(candidates) work per batch.
- N4. `kinds` covers the "contradictory" state only as contradictory tags (`census_record`/`numeric_record`), never as contradictory evidence. B1 is exactly that kind of untested contradiction.
- N5. The unit_tests.list comment for test_discovery_integration.py lacks "Refs #988" (cosmetic).

## 1. src/discovery_admission.h against spec section A: differences only
- None in members, C types, enumerator values or function declarations.
  - VERIFIED by diffing against the pre-delta producer header (wt-pc, 6e6034c7 lineage).
  - VERIFIED by the `_Static_assert` type/value/signature checks in tests/discovery_headers_host.c, compiled in both inclusion orders.
  - Renames are complete (`pgwt_producer_fact`, PGWT_PRODUCER_RUNNING/DENIED/MALFORMED = 2/3/4).
  - PGWT_DH_* = 0..11, PGWT_DREC_* = 0..2.
  - All new attempt members are present.
  - `pre_enable`, `binding_snapshot` and `checkpoint` are exact.
  - The 4 new functions are present; existing signatures are unchanged.
  - No `listed_at`/`holder`/`bound_at` member was added.
- Clock-domain notes: section A's units and clock domains (monotonic ns for the new timestamps; boot-clock bounds; dimensionless facts) do not appear as comments in the header. Only the pre_enable comment is present. This is a documentation difference, not a declaration difference.

## 2. Translation (section B), row by row
VERIFIED by reading unless marked.
- CENSUS (:965-982, :994-1005, :1104-1106):
  - Uses the record's `reconciliation_ns`, never `census_cut_ns` (mutant positive_census_uses_scan_start).
  - Requires enumeration, lifecycle, complete reconciliation, lifecycle accounting equal to current C, both handoff flags, all obligations registered (untested, see R35) and no unresolved numbers.
  - Negative update first; positive census last. OK.
- INVENTORY (:405-438):
  - Issued before dependent controls; registration is reordered to come first (`ordering`).
  - `start_identity` is called only with a qualified exact start.
  - Binding is retained independently of installation (`immutable_identity`).
  - An unresolved number is gated through pre-existing R or eligible J. Defect B1 sits on that gate.
- INSTALLED (:439-449): requires IDENTITY_VISIBLE|EMISSION_IDENTITY; implies nothing else. OK.
- NUMERIC (:1020-1030):
  - `numeric(pid,0,failed_open)` with cookie 0; deduplicated by pid.
  - Delivered before J/R/G (`numeric_order`). OK.
- J (:718-734):
  - Eligible exact witness plus unique join, complete reconciliation.
  - Precedence over R/G (`journal_late`).
  - `exit_time=terminal_ns` is kept separate from the joined time (`journal_time`). OK.
- Pre-existing R (:742-745): INVENTORY then INSTALLED, never REBIND (`resolution`). Defect B1 is in `preexisting()`.
- Other R (:746-749):
  - REBIND only for FORK/NONMEMBER, with all four IDs nonzero, a retained handle, `reliable()` and known journal ineligibility. Correct, but the class rule is untested (R38).
- G (:750-753):
  - ESRCH strictly after failed_open (mutant allow_equal_esrch), fence >= gone_at, cookie 0, no eligible J. OK.
- ATTEMPT (:827-865):
  - One WAIT_ATTEMPT per physical ordinal; a qualified non-NULL POINTER is sent first (:1067-1073, `ordering`).
  - Final ADMIT is sent with generation, e and mask; CONFIRM only for a complete quiet witness.
  - Defect S2: a refused ADMIT is re-sent.
- NO_WORD [D3] (:866-884, :563-578):
  - TRY_NO_WAIT_WORD, never WAIT_ATTEMPT.
  - Route-c ADMIT with `e=pointer_enabled_ns`.
  - Under (a), CONFIRM_NO_WAIT_WORD waits for checked support plus `acceptance_gate`; under (b), no publication. OK.
- POINTER (:491-506):
  - `read_ok=snapshot_valid`; value is Boolean; time 0.
  - Unknown-only continuity is withheld; a non-NULL read with unknown continuity is sent as `continuous=true`. Safe: the merge (merge.c POINTER case) and the capture read only `!pointer_continuous`.
  - Revocation happens before delivery.
- TERMINAL (:762-776): exact retained identity/start, unique join, `terminal_binding_proved`, `names_exit`, source/position, DATA time. Unproved matches stay candidates. OK.
- END (:886-891, :609-619):
  - Irreversible; the final admission comes first.
  - ABANDON only for installed, bound, never-admitted lifetimes while startup is open (`end_final`, `startup_closed`).
  - Uninstalled lifetimes stay pending. OK.
- FAULT (:1011-1016, :914-952):
  - Totals jump, idempotent high-water marks; no fabricated loss counter (`record_fault`, `faults`). OK.
- Three-valued derivation:
  - Places where unknown becomes true: only B1, a contradiction rather than an unknown.
  - Places where a known-false prerequisite fails to make the fact false: none found.
  - Summary flags promoted to proof: none. `quiet_valid` and `handoff_complete` appear only as conjuncts; `evidence_fault` is used as "no fault" only together with known counts.
  - Several `yesno(enum==COMPLETE)` operands make a PENDING reconciliation known-false where spec A says PENDING means unknown. There is no reachable effect: every consumer uses `pgwt_discovery_has` only. VERIFIED by grep of merge.c, merge_capture.c and merge_adapter.c.
- Enum casts between the producer and merge enumerations: none. The inactivity enum uses an explicit switch (:47-56). VERIFIED by grep.
- Clock domains:
  - No mixing found (`start_upper` vs `boot_lower`, start vs F0, cut vs DATA time, d vs cut).
  - The no-word visibility order uses the correct clock, but nothing pins it (R24).

## 3. State machine and checkpoint order (section C)
- Order: barrier -> `refresh_until(deadline)` -> cursor-zero census -> current checkpoint -> separate append cursor (`checkpoint`: "[RZC"). Then negative checkpoint, census and faults, then registration, then resolutions and controls, then terminal finalisation, then positive census last, then release of staged records. VERIFIED by reading and the `checkpoint`, `census_order` and `fault_order` cases.
- The receipt path applies the same barrier twice (:1188, :1230). See the design note below on cost.
- Liveness trigger:
  - PERMANENT_FAILURE from refresh or checkpoint stops attempts first (`permanent_fault_order`).
  - It then selects final ADMIT/ABANDON and finalises undecided EXITs without a new fence.
  - PENDING records TRANSIENT only (`liveness_transient`).
  - Exact non-permanent trigger: current state COMPLETE and `checkpoint.reconciliation_ns >= EXIT time`. This is conservative: a retained earlier completed cut alone does not trigger it. VERIFIED.
- Terminal outcomes cannot switch: :689-691 and oracle T3. VERIFIED.
- Error mapping:
  - No API returns a fatal value (all void; `create` returns NULL plus a caller-owned outcome).
  - Swallowed: S3 (deadline) and N1 (pid 0). Repeated: S2.
  - No loop is unbounded.
  - The only possible "hang" is API gap 1: a held numeric-only cookie-less EXIT that the capture cannot dispose of. It is reported as `terminal_finalisation_incomplete`.

## 4. Tests
- They drive the real capture and merge (`pgwt_capture_replay_create`). Assertions cover controls issued, the capture's pending counts, committed bounds/discharges and the merge's fault classes. VERIFIED.
- Ten oracle rows checked: D01, D02a, Q03, A03, A07, N5, N14, J13, J17, T3.
  - Each entry in tests/discovery_oracle_expected.h matches contracts/merge-v1/oracle-discovery/EXPECTED.md: output string, closure/persisted, pending, census. That file is unchanged since base.
  - The borrowed `full_state` + `numerical_outputs` / amended assertions check:
    - the complete output sequence;
    - gap publication, closure, pending, census and fence;
    - lower bound and route, terminal discharge;
    - abandonment counts and final status.
  - That is the row's complete expected output. VERIFIED.
  - All ten fail when the coordinator's controls are suppressed: P1 (deliver no-op) or P3 (resolve no-op) for J13/J17, in rv12/mut1.log and mut2.log.
- Nuance: 27 of 86 rows still pass with every `deliver()` control suppressed. 16 of them are J rows, whose controls go through `resolve()`. The other 11 (D02b, D02d, D02e, D04b, D04d, D04e, D04g, D13a, D13b, Q08, T2) are negative rows or registration-only rows. "Through the coordinator" means registration only for those. This is not a defect.
- The implementer's 86 mutants: distinct, built, each reaches its named case and fails its named assertion (the runner asserts both). VERIFIED.
  - Coverage gaps are in S4.
  - `revocation_after_contradiction` fails because the revocation never happens (the bound is already gone), not through an order-specific assertion. Acceptable.

## 5. The implementer's API gaps and "not coverable" rows
- Gap 1 (numeric-only held EXIT): right. The capture's TERMINAL_UNRESOLVED requires an inventory entry for (pid,cookie), otherwise it faults (merge_capture.c ~950-963). `numeric_terminal_limit` passes and shows the EXIT still held. Because a held head blocks source L, this is the liveness gap spec C warns about. It needs an owner/contract decision.
- Gap 2 (accepted vs committed): right. `pgwt_discovery_state` does not expose `quiet_pending` or `terminal_proved` (core/pgwt_discovery.h).
- Gap 3 (`pointer_continuous` three-valued): overstated. The current encoding has no reachable failure, because the merge and the capture only read `!pointer_continuous`. The change becomes required only if a consumer ever treats `true` as evidence.
- The 22 rows:
  - Right: D04a-* and D04c (the producer has no ADMIT-level claim flag), D04f, D13c, A04, A06, A20, A16, N12, N16 and J7-J11.
  - Wrong: D13e. The coordinator does not check the mode. It forwards ADMIT and the capture refuses it: 2 ADMITs and 4 VOIDs in rv12/probe2.log probe Q. So the row is reachable; it currently produces 4 VOIDs where the row expects 1 (S2).
  - Partly wrong: Q04 and Q04b. The producer would encode pcount/state-letter or schedstat evidence as UNKNOWN inactivity. That reaches the row's expected outcome, which is the same as Q03's. The exact consumer enum is unreachable, but the outcome is coverable.

## [D3] and latch amendment: places that must change (not defects on this branch)
- The publisher gate becomes "no publication precondition":
  - final_attachment :568-576;
  - `pgwt_di_publish_support` :592-600;
  - `revoke()` passing the variant :485-486;
  - header `pgwt_di_ops.support/revoke`, `enum pgwt_di_variant`, `cfg.variant` (:19, :63-68, :81);
  - create validation :1296;
  - PGWT_DI_SUPPORT_UNAVAILABLE/REJECTED handling :300-301, :311.
  - Tests: `publisher()` with p_absent/p_failed/p_b; mutants confirm_without_support, variant_b_requires_support, support_after_confirmation, invalid_variant_as_no_support; the oracle N14 `support_reject` path and the `h->supporting` check in discovery_integration_oracle.h:150.
- "Nothing commits after the first fault": these still issue commit-capable controls after a fault:
  - ADMIT :552-560 plus the retry loop :1092-1095;
  - ABANDON in `end_attempts` :609-619 (the merge accepts ABANDON while suspended);
  - JOURNAL/REBIND/GONE in `resolve()` :712-761;
  - TERMINAL/UNRESOLVED :681-699, :777-806;
  - staged release :1152-1171.
- The positive census (:972) and CONFIRM (:561, :591) are gated on `certification_blocked`. That flag is not set by contradiction or transport faults: `fault()` sets only `incomplete`. A distinct first-fault latch is needed.
- Oracle A12 (explicit REARM) will need new answers.

## Notes for the next coordinator round
1. Receipt cost. Each `pgwt_di_receive` runs `checkpoint_body` twice: refresh_until, cursor-zero census, checkpoint, a `binding()` copy of about 12 KB per registered lifetime, a capture CHECK and the O(lifetimes) sync. At transport rates this is unusable. Spec C asks for the barrier only when attachment produces handoff during a poll, so probe the append cursor first and drain only on change.
2. One `capacity` is used for six registries of different sizes: lifetimes, numbers, terminals, candidates, per-checkpoint batch and staged wire records.
   - A checkpoint with more than `capacity` appended records raises an ALLOCATION fault (:1145-1148) instead of draining in chunks. A startup scan writes several records per backend, so a population-sized capacity overflows on the first checkpoint.
   - Separate the capacities, drain in chunks, and prune final entries (B2, S1).
3. Memory. `struct pgwt_discovery_attempt` is 11,960 bytes (MAX_CPUS ack arrays). A coordinator lifetime holds two copies (about 24 KB), and the batch and candidate arrays hold full records. At PGWT_CAPTURE_LIFETIME_CAPACITY (262,144) the three `calloc`s alone come to about 12.5 GB. Retain translated, compact evidence instead of raw records (measured with rv12/sz.c).
4. I2 will need one owner for the terminal map. Today `pgwt_di_receive` builds its own (cpu,dest,seq) registry next to the injected `terminal_join`, and B2 lives in that duplicate.
5. I3 should gate the permanent-failure ABANDON on the checkpoint's verified `attempts_stopped`. Today it reads the gate after selecting ABANDON (:1096-1101). That is harmless only while the producer is single-threaded inside the barrier.
6. Use the `attempted_controls` bits to suppress re-sends (S2, N3) before wiring a real checkpoint cadence.
7. Three-valued purity: PENDING reconciliation currently gives known-false operands. Make it unknown before any consumer starts distinguishing known-false from unknown.
