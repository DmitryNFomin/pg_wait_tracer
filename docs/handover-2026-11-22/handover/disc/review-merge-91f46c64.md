# Review: agent/988-discovery-merge, round 4 delta (4b5903d7..91f46c64)

Verdict: **READY-with-nits**. No blockers. One should-fix (contract text, fails loud, unreachable until the integration unit issues dispositions) and one nit (new guards without a red test).

## What changed
- A startup process that exits before its discovery identity is installed now gets one final disposition, matched or unresolved. An unresolved EXIT is counted once as uncovered and never published. It causes no VOID. The process stays pending and the startup gap stays open. The capture no longer stalls behind that EXIT.
- A capture that finishes while such an EXIT still waits for its disposition reports FAULT(unknown_evidence), and matched-after-unresolved (or the reverse) is a FAULT. A second, identical disposition changes nothing.
- A terminal discharge and a fresh lower bound now commit only after the whole release attempt succeeds, including every counter checkpoint and the FRONTIER. The order of TERM and EXIT no longer matters.
- 27 named guard cases and 35 new mutants were added (110 in total). The round-3 assertion "successful EXIT survives counter fault" is inverted, not deleted.

## Evidence
- Check stamp: not run (read-only brief). No `make check`.
- box-check: none for 91f46c64. I ran no box (read-only brief). The C unit tier and live tiers on Linux are **not verified**.
- All runs below are mine, on `git archive` exports of 91f46c64 and 5fd6a519 (scratchpad/rv4/{head,base5f}), with ASan/UBSan unless noted:
  - `test_discovery_admission.py --mutants` (PYENV 3.11.7): rc=0; drift check 70/70; 70 oracle rows + 27 named cases PASS; empty census PASS; **110/110 mutants RED**. All 10 mutants that EXPECTED.md names for T1-T4/V1-V4 are RED on their named row (log rv4/logs/disc-mutants.log).
  - `core_merge_adapter_mutants.sh`: rc=0, 27/27 killed, guard sweep 31/31; the four re-anchored percpu mutants (skip_final_budget_edge_check, skip_periodic_edge_check, skip_source_edge_observation, skip_sweep_edge_check) KILLED.
  - `core_merge_mutants.sh`: rc=0, 36/36 killed.
  - Byte-identical to 5fd6a519 (cmp): test_core_merge_contract (127 lines, sha256 ddf62059…), test_percpu_transport (168 lines, 2eae924a…), test_transport_wire (6 lines, 4d9390d3…), test_core_merge_adapter (113 lines, 636f53fc…). The hashes equal the implementer's evidence.
  - T-cov (`test_exact_perf_coverage.py --mutants`): rc=0 on both trees, with 18 RED. Its MUTANT/PASS lines are identical to 5fd6a519 (diff empty).
  - Also rc=0 on 91f46c64: test_capture_bookkeeping, test_capture_recovery (19/19 recovery mutants killed), test_transport_startup, test_pilot_route (default mode) and test_discovery_admission without sanitizers.
  - Not run by me: core-status.sh, --require-browser and the evidence-v1 suites. UNVERIFIED: the implementer reports core-status 21/21 PASS.
- `git diff --check 4b5903d7 91f46c64`: clean. `git diff --quiet 4b5903d7 91f46c64 -- contracts/`: rc=0. tests/unit_tests.list and tests/run_all.sh are unchanged. The commit message ends with "Refs #988".
- Registration guards: not re-run by me. UNVERIFIED: the implementer's evidence.txt shows both OK.

## Brief items (reproduce or refute)
1. **Final terminal disposition: VERIFIED.**
   - Round-3 stall probe with a TERMU added after round 3 (rv4/probe/tests/p_disp.c P1): the four held A records and frontiers publish at once and the capture then keeps flowing. 25 rounds give A10..A58 and F11..F59. Final state: unresolved=1, census complete, gap open, uncovered=1, no VOID, finish rc=0.
   - Without a disposition the original p_stall still holds everything and finishes `VOID200[C0,unknown_evidence]`. That is now the contract (T2), not a defect.
   - A TERMU naming a time at which no EXIT ever arrives (P3), and likewise a matched TERM (P3m): finish passes without a fault. The gap stays open and the entry stays unresolved. The contract requires a FAULT only for an EXIT that has no disposition, so this case is unspecified and conservative.
   - A disposition for a PID not in the inventory (P4): `VOID20[C0,unknown_evidence]`. It refuses loudly.
   - Two cookie-less EXITs for one PID: see finding 1.
   - Carrier: `PGWT_DISC_TERMINAL_UNRESOLVED`, delivered as capture control plus MA_CHECK. The EXIT's position is consumed through the new core input `PGWT_MERGE_UNCOVERED` (merge.c:1092).
   - `TERMINAL` with `terminal_binding_proved=false` is still a no-op. It is unproved and not final, so the EXIT stays held and finish faults.
2. **Release-scoped discharge and lower bound: VERIFIED** for rows V1-V4, A03, A20 and the two lower-bound cases.
   - My mutants all go RED:
     - `x`: commit exit_release_committed mid-attempt → A03.
     - `y`: TERM ignores the commit flag → A03.
     - `z`: write back the lower bound mid-attempt → bound_rejected_later_record.
   - Hunt for other state committed mid-attempt that survives a VOID (read from source, INFERRED harmless):
     - `published->discovery.has_uncertified/uncertified_through` (merge.c:892-897) survives a VOID. It can only add +1 to the persisted end, or fault on a record stamped after the terminal time. Both are conservative.
     - `discovery_abandoned_records` increments mid-attempt. The contract requires this: A15 says `abandoned=3` with the EXIT rejected later in the same attempt.
     - `exited/exit_record`, `first_released` and `fork_seed_admitted` are lifecycle history (DECISION 25c). Discharge is gated separately.
     - The GAP_CLOSE record is persisted only from `capture_core_feed` after a feed that returns OK (merge_capture.c:198), and only from committed core state.
     - FRONTIER flushes run synchronously inside capture_sink/daemon_sink, so a flush failure faults the same attempt.
     - Inventory flags are set by controls, not by outputs.
   - Found nothing else.
3. **Round-3 reviewer's own mutants (rv3/mymut.py, all 15 run against 91f46c64): every one RED.**
   - 13 RED unchanged.
   - Two had anchors that no longer exist; I re-anchored them on the new code and both are RED:
     - o (capture terminal conflict) → G_terminal_control_conflict.
     - r (overflow) → G_endpoint_overflow.
   - The :568-570 case (AB_D, D12, EXIT10, TERM10) is G_consumed_past_terminal, with complete output `EXIT_D10 F12[C0] VOID20[C0,unknown_evidence]`. Survivors: none. Log: rv4/logs/mymut.log.
4. **Discovery unused: VERIFIED** byte-identical on the four outputs above.
5. **percpu_tick_harness.h / percpu_transport_mutants.py: VERIFIED mechanical.**
   - The tick harness mirrors the new `struct retained` field, `rearm_at` and the `mapped(..., bool)` signature. It also adds `get_le`, because the verbatim flush_source/feed bodies now use them.
   - The four mutants' anchors and replacements only add `, false` to the new parameter. The mutations are semantically identical and all four are KILLED. Nothing is weakened.

## Findings
1. **should-fix — src/merge_capture.c:728-734 (with :730 and :505/:525): the disposition is stored once per inventory entry, but the contract specifies one per EXIT.** Reproduced (rv4/probe/tests/p_disp.c):
   - P2b input: D (pid 30, cookie DG) is inventoried and never installed. `EXIT_pid30@18{no cookie}, S(19), TERMU(pid30,18)`. Then a second cookie-less EXIT of pid 30 at 25 (a reused PID whose fork-cookie allocation failed), `S(26), TERMU(pid30,25)`.
     - Expected (DECISION 40): every cookie-less EXIT of an inventoried PID gets its own disposition. Unresolved is consumed as uncovered with no VOID. FAULT applies only to conflicting outcomes "for the same EXIT".
     - Actual: `F18[C0] VOID20[C0,unknown_evidence]`. Line 730 treats the different time as a conflict.
     - If the second disposition is omitted (P2), the second EXIT holds source L until finish gives `VOID50`.
   - P5 input: D admitted, then D12; the gap closes and unresolved=0. Then a cookie-less EXIT of pid 30 at 25 and `TERMU(pid30,25)`.
     - Expected: "the pending count and census completeness are unchanged".
     - Actual: D's entry is flipped to unresolved, so unresolved=1 while closed=1.
     - With a second census entry still pending, D would block the gap from closing for the rest of the capture (INFERRED).
   - Fail-safe holds: the result is a loud VOID or a gap left open, never a wrong number.
   - Reachability: discovery is enabled nowhere in src/, and nothing consumes `pgwt_capture_discovery_status`. A recursive grep of src/ for `discovery_enabled = ` and for `pgwt_capture_discovery_status` found only the definition and the header. Not searched: tests/.
   - No oracle row covers a second EXIT for the same PID. Fix before the integration unit issues dispositions: key the disposition by (pid, EXIT time), and do not let an unresolved disposition for another EXIT change a resolved entry. Or state in the contract that this cannot happen.
2. **nit — guards new in round 4 that no test turns red** (my mutants, rv4/logs/mymut2.log; the code is correct today):
   - merge_capture.c:737 `if (!found)`: P4 behaviour is correct, but the mutant survives.
   - merge_capture.c:696 `c->finished` guard in discovery_control.
   - merge_capture.c:505 and :525 `v->terminal_unresolved ||`: equivalent except in finding 1's scenario.
   - merge.c:1092-1094: UNCOVERED consuming its position.
   - merge_capture.c:1019 `*hold = false`.
   - merge_capture.c:541: call site of `discovery_endpoint`. The helper itself is tested; the case needs a timestamp of UINT64_MAX.

No blockers.

## Design questions for the owner
None. Finding 1 is a code-versus-contract-text gap; the lead can settle it in the integration unit's brief.
