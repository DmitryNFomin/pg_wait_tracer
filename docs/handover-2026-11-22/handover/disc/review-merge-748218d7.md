# Review: agent/988-discovery-merge, round 6 delta (d5ad2ac3..748218d7)

Verdict: **READY-with-nits**. No blockers. Two should-fix items and two nits are listed below.

All paths are under the scratchpad base SP.
- Probes are in rv6/probe/tests/{p5,p6,p7,p7b,p8,p_disp}.c, built against a `git archive 748218d7` copy in rv6/probe. Each probe file was checked against the commit with cmp.
- Logs are in rv6/logs.

## What changed
- A route-(c) ADMIT is now refused when retained pointer evidence (a non-NULL fire or a continuity break) contradicts it, whatever order the controls arrive in. A numerical ADMIT, or a quiet CONFIRM, for a candidate watched as a NULL-pointer candidate is refused.
- Pointer evidence is kept even after the fault it causes. TRY cannot clear a pointer watch.
- An ADMIT or CONFIRM whose enable time falls after a proved terminal time is refused, in both orders.
- An unresolved terminal disposition consumed while the startup gap is open keeps the gap open for the rest of the capture.
- Binding a PID listed by `coverage_begin(pids)` now discharges its numeric observation.
- Header handoff notes are added. There are 49 new named cases (87 in total) and the suite has 214 mutants.

## Evidence
- Check stamp and box-check: neither run by me. The brief is read-only with no `make check`, and no box is configured.
  - The implementer's own report says its run STOPPED when `test_pilot_route.py --require-browser` failed: Chromium was denied a Mach port by the sandbox.
  - That report also lists checks it never ran: evidence/codec mutants, core-status and the pilot mutants.
  - The C unit tier and the live tiers are **NOT VERIFIED**.
- `test_discovery_admission.py --mutants` (ASan/UBSan, PYENV 3.11.7) gave rc=0 (rv6/logs/disc-mutants.log):
  - "108/108 IDs and verbatim inputs/answers match"
  - "108 oracle rows, 87 named cases passed"
  - "discovery mutants: 214/214 RED"
  - **VERIFIED**
- Discovery unused, compared against 5fd6a519. The baseline export rv5/base5f was cmp-checked against 5fd6a519. **VERIFIED**:
  - test_core_merge_contract: 127 lines, ddf62059…, byte-identical
  - test_percpu_transport: 168 lines, 2eae924a…, byte-identical
  - test_transport_wire: 6 lines, 4d9390d3…, byte-identical
  - test_core_merge_adapter: 113 lines, 636f53fc…, byte-identical
  - T-cov `--mutants`: the log is byte-identical to the 5fd6a519 run
- The commit message contains "Refs #988", but a Co-authored-by trailer follows it (nit 2).

## Brief items, verified by running
1. **Blocker 1 is fixed. VERIFIED** (rv6/logs/p6.log, p8.log).
   - p6 A runs TRY_NW_D6, POINTER(9, non-NULL), ADMIT, CONFIRM_NO_WAIT_WORD, S(10). It now gives `VOID20[C0,unknown_evidence]`: P=1+0, no bound, gap open, pointer_nonnull retained. Round 5 gave `F9`, b=6, closed.
   - The sibling, p6 B (TRY of a NULL candidate without a role, then numerical AD12 and quiet B11/12), now gives `VOID20[C0,unknown_evidence]`, P=1+0, gap open.
   - Order sweep (p8): one physical history, with the POINTER (non-NULL, or BREAK) placed in each of 5 positions: before TRY, between TRY and ADMIT, between ADMIT and CONFIRM, between CONFIRM and S, and after closure (N5's order). Then REARM, S23, a second CONFIRM and S24.
     - All 10 schedules fault. None commits a bound or closes the gap after the REARM, and the re-sent CONFIRM faults again in the new epoch.
     - One difference remains: the census report (should-fix 2).
   - Further pairs run, each in both orders and where relevant across an unrelated fault plus REARM:
     - Retained across REARM: POINTER received while suspended, then REARM, then ADMIT c (R1). ADMIT c, POINTER non-NULL or BREAK, REARM, CONFIRM (R2, R3). A watched NULL candidate faulted, REARMed, then numerically admitted (R4). All refuse.
     - Watched NULL candidate vs numerical claims, five orders (TRY/AD/B, TRY/B/AD, B/TRY/AD, B/AD/TRY, AD/TRY/B; Q1–Q5): all refuse. TRY after ADMIT is refused.
     - TERMINAL vs ADMIT, both orders and across REARM (T5–T8): refuse. Legitimate x after b in all three orders (T2–T4): `EXIT_D18 F18`, b=7.
   - **No legitimate sequence faults. VERIFIED.**
     - N7 (TRY of a non-permanent-role candidate, non-NULL at 9, WAIT_ATTEMPT, AD12, B11) gives `F12`, b=11, C_close=11, as EXPECTED.md N7 requires. The same result holds for these variants:
       - POINTER delivered before TRY (L2)
       - a NULL fire before the non-NULL one (L3)
       - B before AD (L4)
       - a pointer break after initialization (L5)
       - the fresh-record route (L6, `D13[U] F13`)
       - an unrelated fault plus REARM before admission (L7, b=21)
     - Route c with a NULL fire before ADMIT, between ADMIT and CONFIRM, after the bound, or with a duplicate TRY (K1–K4): `F7`, b=6, closed, as in N1.
2. **Should-fix 2 is fixed. VERIFIED** (rv6/logs/p5.log, p_disp.log).
   - Q6 (unresolved EXIT@18, AD_D20, D22, S24) gives `F18 D22[U] F23`, b=22, gap **open**. Q4 (U18 then M25) stays open; P7 stays open.
   - G_unresolved_does_not_block_closure now asserts the gap is open.
   - The previous reviewer's P5 still keeps unresolved=0 and closed=1 after the reused-PID `TERMU25`.
3. **Should-fix 3 is fixed. VERIFIED.**
   - p7: `coverage_begin({30})`, inventory(30), census, AD8, D12, S13 gives `D12[U] F12 F39`, P=0+0, census known, closed.
   - p7b: `coverage_begin({30,31})` with only 30 bound gives P=0+1, census unknown, gap open.
4. **The previous reviewer's 42 mutants, re-run on 748218d7 against the whole host binary** (rv6/logs/mymut-pass1.log; n25 re-anchored in rv6/logs/mymut6.log).
   - 37 RED: g1–g6, n1, n2, n4, n5, n7–n17, n19–n27, n29–n33, n35.
   - Survivors, all equivalent or redundant:
     - n3 (no_word_valid counters_unchanged): redundant with the counter checkpoints.
     - n6 (support-checkpoint counter check): redundant with the DATA/FRONTIER checkpoints.
     - n18 (progress ignores numeric): redundant with census_complete.
     - n28 (census ignores lifecycle_lost): redundant with the later CHECK.
     - n34 (CONFIRM baseline e): now equivalent. The new ADMIT check `d->e != d->no_word.enable` (merge.c:598) forces baseline.e to equal no_word.enable, and removing that early check is RED (my r3, G_nw_enable_mismatch). Read from source; equivalence not proven by running.
     - n36 is an unchanged control and survives by construction.
   - Both harness-only mutants were replaced with product mutants; both are RED.
   - My 20 mutants on the round-6 code (rv6/logs/mymut6.log): 17 RED. Survivors:
     - r6b (`>` changed to `>=` in the ADMIT terminal check): an edge where e == x, on which the contract is silent. Not a finding.
     - r15 (latch set even after closure): equivalent while the gap is closed.
     - r7 (the CONFIRM_NO_WAIT_WORD pointer check): a **real gap**, see should-fix 1.
5. All 108 rows pass, the 214 mutants are RED, and the legacy outputs are byte-identical to 5fd6a519 (see Evidence).

## Findings
1. **should-fix: src/core/merge.c:666 — the new CONFIRM_NO_WAIT_WORD guard (`pointer_nonnull || pointer_discontinuous`) has no red test, and it is the only thing that refuses the REARM path.** Reproduced with p8 R2 and R3 against a copy with that line removed (rv6/logs/p8-r7mut.log).
   - Input: TRY_NW_D6, route-c ADMIT, POINTER(9, non-NULL) (fault), REARM(21), CONFIRM_NO_WAIT_WORD, S(23).
   - Expected (DECISION 37(c)/43): no bound, and the lifetime stays pending. The committed code gives `... VOID22[C0,unknown_evidence]`, P=1+0.
   - Without the guard: `VOID20 F22`, status OK, **b_D=6 committed**, P=0+0. The gap stays open only because the census was cleared; a later census declaration would close it on a lifetime known to have initialized.
   - The whole suite stays green with the guard removed. Add the R2 schedule as a named case with a removal mutant.
2. **should-fix: src/merge_capture.c:910 (unchanged code) — the census report depends on control order for one physical history.** Reproduced (p8 Cn0/Cn1 vs Cn2/Cn3, and Cb*).
   - Input: TRY_NW_D6, POINTER(9, non-NULL), ADMIT c, CONFIRM, S10. Then the same history with the POINTER delivered after ADMIT but before S10.
   - Actual:
     - POINTER first: `VOID20`, census **known**, 1 gap.
     - POINTER after ADMIT: `VOID20`, census **unknown**, plus a second "revoked" PREEXISTING gap publication, even though the gap never closed.
   - DECISION 43 re-opens the census only after an *accepted* 37(c) bound; N12 (a refuted ADMIT before acceptance) keeps the census known. So the after-ADMIT order over-reports.
   - The failure is safe: neither order closes or publishes coverage. It is still the order dependence the brief asked to be removed. Fix: gate the census reset on a committed bound or closure, not on `state.no_wait_word`. The brief's phrase "exactly as N5" does not apply literally here, because N5's census=unknown follows from revoking an accepted bound.
   - Not reachable today: discovery is enabled nowhere in src/ (INFERRED, as in rounds 4 and 5).

Nits:
1. Q1 vs Q2 (p8): TRY, AD, B gives one VOID; TRY, B, AD gives two VOID20s in one epoch. The cause is that the ADMIT pre-checks (merge.c:594) run before the suspended-status return (:612), while the quiet-CONFIRM route check (:675) runs after it. The final state is identical. Each refused claim reporting FAULT is consistent with DECISION 36, so this is only noted.
2. The commit message's "Refs #988" is followed by a Co-authored-by trailer, so it is not the last line. Unchanged from round 5.

## Design questions for the owner
None.
