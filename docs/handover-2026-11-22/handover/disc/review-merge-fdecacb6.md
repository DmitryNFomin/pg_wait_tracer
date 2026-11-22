# Review: agent/988-discovery-merge 5fd6a519..fdecacb6 (discovery admission, merge/capture side)

Verdict: NOT-READY (1 blocker: four registered gate tests go red; the product logic itself passes its oracle rows)

## What changed
- New controls let the merge accept processes that were already running when capture started: ADMIT_DISCOVERY, CONFIRM_DISCOVERY and TERMINAL, defined in src/core/pgwt_discovery.h. They take no source position, unknown facts count as false, and contradictory proofs give FAULT(unknown_evidence). DATA waits until its ADMIT arrives.
- Lower bounds come by two routes. Fresh record: the first released record of the final attachment is marked [U] and sets b_i. Quiet bracket: the witness is validated, then committed once W > t.
- Closing the startup gap, with discovery enabled only: C = max(census fence, b_i, x_i). It is computed only after the census is complete and every entry is resolved, and published at C+1 through a checked publisher. A failed publication commits nothing.
- A2 population gate (src/core/pgwt_a2_population.h): a discovered process adds no birth and no omission.
- Production never sets `discovery_enabled`, so the daemon behaves as before. T-cov output and its 18 mutants match 5fd6a519 exactly.

## Evidence
- Check stamp: none (`.pgwt-check.stamp` absent; tree-hash 139fcd72…). Not run (read-only brief); see Finding 1, make check cannot pass.
- box-check: none run, no `tests/results/box-check-*.log` for this tip. The C unit tier would fail (Finding 1).
- Host runs on a git-archive export (base 5fd6a519 vs head fdecacb6):
  - `test_discovery_admission.py --mutants`: 39/39 rows PASS; 29/29 mutants RED at their named rows.
  - `test_exact_perf_coverage.py --mutants`: PASS; the MUTANT/PASS lines are byte-identical to base (18 RED). The `ignore_preexisting` mutant is still RED at "preexisting gap begins at boot_start".
  - test_core_merge_contract, test_core_merge_adapter, test_percpu_transport (43/43 N, 49/49), test_transport_diagnostics, test_transport_wire: stdout identical to base.
  - Also green: test_attach_timeline, test_capture_recovery, test_core_merge_queue, test_coverage_drain, test_init_loss_diagnostics, test_transport_startup, core_merge_queue_mutants, transport_diagnostics_mutants.
  - Both registration guards OK; `git diff --check` clean.
- Premise, probe premise.c run on the archive exports:
  - base and head, discovery off: begin(pid 30, no cookie), then W=500 with no EXIT. Output: `OPEN@0` only, the gap stays open.
  - head, discovery on: then bind (30, 2^63), complete census at fence 5, ADMIT e=508, D512, D515, W=516. Output: `closed=1 closure=512`, persisted `CLOSE@513`.

## Findings
1. **blocker**: four registered tests pass on 5fd6a519 and fail on fdecacb6.
   - (a) tests/test_capture_bookkeeping.py (unit_tests.list:1130). Its "new" tree copies a fixed list of files (test_capture_bookkeeping.py:133-140). That list lacks src/core/pgwt_discovery.h and pgwt_a2_population.h, so the build fails: `pgwt_merge.h:8: 'pgwt_discovery.h' file not found`.
   - (b) tests/test_core_merge_index.py (:1089) and core_merge_index_mutants.py (:1090) fail with `index corpus MISMATCH cost release at input 0`. The cost harness (core_merge_index_cases.c:480/506) uses fork cookies `i ^ UINT64_MAX` ≥ 2^63. Since merge.c:649-651 those are refused, which is correct under DECISION 32, so the fixture needs cookies inside the fork range.
   - (c) tests/percpu_transport_mutants.py stops with `ERROR: forget_historical_proof: mutation anchor is not unique`. The anchor `if (!c->lifetime_index[slot]) {` now appears at merge_capture.c:627 and :822. This script runs through core_merge_adapter_mutants.sh:136, which make check calls (scripts/check.sh:122), so the check stamp cannot be produced.
2. **should-fix**: a process that exits before its identity is installed (DECISION 40) blocks startup closure for the rest of the capture (merge_capture.c:752-769, merge.c:799-804).
   - Input: inventory (30, 2^63), complete census, then the kernel's cookie-less EXIT for pid 30. Then TERMINAL(30, 2^63, x=9, proved), W=16, and the complete census sent again.
   - Expected: D stays one pending entry; a proved terminal witness can discharge it (DECISION 40/41).
   - Actual (reproduced, probe exit.c): unresolved goes from 1 to 2 and the census is no longer complete. The added cookie-0 entry cannot be resolved by any API, because inventory() matches the bound entry first and TERMINAL needs an EXIT that carries a cookie. Result: `unresolved=2 closed=0`, which never changes.
3. **should-fix**: the persisted gap is [0,C+1) for every route (merge_capture.c:512), so pgwt_coverage_verdict refuses the windows the oracle gives numbers for.
   - Reproduced (probe reader.c): D01 [12,16), Q01 [12,16) and Q09 [11,16) all read INCOMPLETE. EXPECTED says "X duration 3, count 1" (D01, Q01) and "X duration 4, count 1" (Q09).
   - The 39-row numbers pass only because released_metrics (discovery_admission_host.c:340) takes its coverage from the in-memory lower bound instead of the persisted gap.
   - The test also asserts `at==close+1` (line 281), a value taken from the code. The oracle's G(c) means [0,c).
   - For the quiet, census-fence and terminal closures there is no uncertified completion at C to exclude.
4. **nit**: mutant `zero_wait_infers_on_cpu` only changes a stored copy of the seed (merge.c:573) that no consumer logic reads. It turns red only because the test reads that same field back, so it does not show a product failure.

Point checks with no defect found: DECISION 36 (unknown fact counts as false; FAULT on contradiction; no REARM or fault cancel; DATA and its frontier wait) and DECISION 37(b) (W > t strictly; nothing is committed while a record is held; no pre-e b_i; a failed or denied witness gives no bound and no fault). DECISION 41 C/publication ordering holds. The row-ID and verbatim drift check is real. The facts listed in discovery_oracle_assertions.txt as not encoded are producer/physical facts. Checked against the oracle by hand: the close, lower bound, pending count, route and census columns.

## Design questions for the owner
1. DECISION 42 conflicts with the existing window-level reader. Excluding the completion at C requires refusing [C,C+1), which then refuses every window that starts at C, against "durations use ordinary half-open clipping" and the D01/Q01/Q09 numbers. Two choices:
   - Accept C+1 and correct those oracle windows.
   - Apply C+1 only when some fresh-route b_i equals C.
2. Staged DATA for a discovery cookie that never gets ADMIT (DECISION 38: "remains pending without ADMIT") holds the merge's global frontier indefinitely. DECISION 36 says such records "wait", and the contract has no way to abandon them.
