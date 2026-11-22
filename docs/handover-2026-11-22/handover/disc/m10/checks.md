# Four checks on b5ae3fb6 (discovery admission, merge side), 2026-11-21T18:01Z

Root `CK` = `SP/a2go/disc/m10/checks/` (SP = the session scratchpad). These are all scratch worktrees made with `git worktree add --detach CK/wt-X b5ae3fb6`. Nothing in them was committed or pushed.
- `wt-a`: unmodified product, used as the control.
- `wt-b`: Check B patch.
- `wt-c`: Check C patch.
- `wt-d`: unmodified product plus the file-writer driver.
- `wt-e`: B patch plus a latch-blind control mutant.
- The enumeration files were copied into a, b, c and e with `git checkout 5347ee51 -- tests/discovery_enum_*` (five files).

Drivers, logs and patches are in `CK/drv/`:
- `chk_abc.c`, built by `build.sh`: appended to the host file with `main` renamed. It calls only the host's input helpers, and those call the public `pgwt_capture_*` entry points and views.
- `chkd.c`, `chkd_dump.c`, `build_d.sh`, `run_d.sh`: the Check D drivers.
- `patch-b.diff`, `patch-c.diff`, `patch-e.diff`.

Builds:
- The A/B/C driver: cc -std=c11, ASan+UBSan, `-DPGWT_TRANSPORT_HOST_TEST`.
- The 149-row suite: its own runner, `python3 tests/test_discovery_admission.py`, with its own flags (-Werror).

Schedule notation, with times relative to DB:
- K = counter fault: snapshot component 3 changes, then CHECK. Output `VOID20[Cr,counter_change]`.
- REARM: d=21, with fresh proofs for pids 10, 20 and 30.
- AD_D6 = `admit(30,DG,e=6,gen=1)`.
- D12 = a TRANS of D at t=12 (gen 1, baselines refreshed).
- S19 = all 4 sources reach w=19.
- AB = ABANDON(30,DG).

## Check A: a valid ADMIT after a fault holds that lifetime and the whole frontier

Command: `CK/drv/bin-a chkA` (run in wt-a). Log: `CK/drv/A-unmod.log`. Every output below is VERIFIED; I ran it.

```
A1  K, REARM, AD_D6, D12, S19:
    after AD_D6: admitted=0 admission_recorded=1 latch=1
    after S19:   out='VOID20[Cr,counter_change]'  frontier=none  (no DATA, no FRONTIER)
    AB (rc=0, accepted, abandoned=1):      out='VOID20[Cr,counter_change] F18[Cr]'   (D12 is NOT published; it is dropped as abandoned)
    S25 -> '... F18[Cr] F24[Cr]'   S30 -> '... F29[Cr]'
C1  AD_D6, K, REARM, D12, S19:  admitted=1; out='VOID20[Cr,counter_change] D12[U] F18[Cr]'
    AB -> VOID22[Cr,unknown_evidence] (refused, status suspended)
C2  K, REARM, S19 (no AD, no D record):  out='VOID20[Cr,counter_change] F18[Cr]'
    AB -> accepted;  S25 -> '... F18[Cr] F24[Cr]'
A1b A1 plus A14 (pid 10, an unrelated attached lifetime): after S19 and after S25, out='VOID20[...]' only, frontier=none.
    AB -> 'VOID20[...] A14[T] F24[Cr]'   S30 -> '... F29[Cr]'
A1c K, REARM, AD_D6, S19 with no D record: 'VOID20[...] F18[Cr]'. The hold needs a D record staged.
```

The code that holds (VERIFIED by reading b5ae3fb6):
- merge.c:238: `fault()` sets `discovery_certification_latched` on every fault when discovery is enabled.
- merge.c:839: `admission_recorded = true`.
- merge.c:845-846: `admitted = status == PGWT_MERGE_OK && !discovery_certification_latched`. This is false for every ADMIT made after any fault.
- merge.c:985-993, in release (the line at :986 is `(m->config.discovery_enabled && !generation->discovery.admitted)`): a cookie-ful record of a non-admitted, non-abandoned lifetime sets `held` and `continue`s. "No DATA or frontier until matching control".
- No later path sets `admitted`. A second ADMIT faults at merge.c:832 because `admission_recorded` is already set. "For the rest of the capture" is therefore INFERRED from the code; I measured it up to S25 without AB.
- merge.c:748-752: ABANDON is refused only when `admitted` is set, so it is accepted here. That releases the hold, which matches triage P1.

REPRODUCED. A valid post-fault ADMIT plus one record of D holds every record and the frontier, including the unrelated A14, up to S25. ABANDON releases the frontier, and D12 is then dropped rather than published. Without a D record (A1c) nothing is held.

## Check B: a recorded ADMIT counts as admitted under the latch

The patch (`CK/drv/patch-b.diff`) replaces merge.c:845-846 with `a->discovery.admitted = true;`.
- It is equivalent to "admitted = recorded": both lines run on the same path, after the merge.c:843 fault return (VERIFIED by reading).
- A bound cannot commit under the latch, because every commit site goes through `discovery_bound_allowed`/`discovery_discharge_allowed`. Both test the latch (merge.c:541, 546). The sites are :1072, :1200, :1207, :1262, and :823 through :818. VERIFIED by `grep has_lower_bound = true|terminal_released = true` and by reading.

Check A under B (`CK/drv/A-B.log`, VERIFIED):
```
A1  after S19: out='VOID20[Cr,counter_change] D12[U] F18[Cr]'  admitted=1 b=-1 x=-1 closed=0
    AB -> 'VOID22[Cr,unknown_evidence]' (refused now)
C1  unchanged: '... D12[U] F18[Cr]'      C2 unchanged: '... F18[Cr]'
A1b after S19: 'VOID20[...] D12[U] A14[T] F18[Cr]'; S25 adds F24[Cr]; AB -> VOID22[...,unknown_evidence]
```

The 149 rows (VERIFIED, `CK/drv/rows-B.log`): the runner exits 1 at its first failure, `ASSERT G_counter_control_ADMIT: counter fault refuses admission (line 1986)`.
- To see every failure, I ran each of the 352 cases alone (149 rows plus 203 named cases, names from the unmodified run) through `CK/drv/percase.sh`: `failures: 1 of 352`.
- The only failure is `G_counter_control_ADMIT`, a named guard, not one of the 149 rows. It pins `!state.admitted` after a counter fault, which is the field semantics itself.
- All 149 oracle rows pass. Control: the unmodified tree gives `failures: 0 of 352`, and its runner prints "149 oracle rows, 203 named cases passed".

Enumeration (VERIFIED). Command: `python3 tests/discovery_enum_runner.py --only all --depth 4 --build-dir CK/enum-X`. Logs: `CK/drv/enum-a.log` (unmodified rerun) and `CK/drv/enum-b.log`. The unmodified rerun reproduces full.log exactly.

| class (scope) | unmodified (full.log = my rerun) | Check B |
|---|---|---|
| model.recorded_controls (enum) | 164,463 | **0** |
| model.post_fault_control_VOID (enum) | 23,539 | **16,183** (minimal now `N:H0:[B6,EARLIER_TRANS14]:fault=1`) |
| views.closed_pending_census (enum) | 270 | 270 |
| model.latch / VOID / report / fault_diagnostics (enum) | 12 each | 12 each |
| pins: views.closed_pending_census | 13 | 13 |
| pins: model.recorded_controls / pin.complete_row (G1, G2/prefixed) | 2 / 2 | **0 / 0** |
| positive cohort | [18,9,9,12,6,12,4,12] | same |

- Model-free latch invariant: there is no `latch.bound_gain`, `latch.discharge_gain`, `latch.END`, `latch.closed_gain` or `latch.cleared` failure in any scope, either unmodified or under B (VERIFIED: grep count of `"latch.` is 0 in both logs). So no bound, discharge, END or closed gain appears after a fault anywhere in the 1,726,074 descriptors.
- Non-vacuity of that zero (VERIFIED):
  - `--self-check` on wt-b catches all eight fake latch observations (`CK/drv/enum-b-selfcheck.log`).
  - Control mutant wt-e (B plus the latch removed from `discovery_bound_allowed`) gives `latch.bound_gain 27,568` (minimal `C:H0:[ADc6,CN6]:fault=1`) against the real product (`CK/drv/enum-e.log`).
- Non-vacuity counters that moved under B (VERIFIED):
  - post_REARM_fresh_opportunities: 4,086 → 13,110.
  - closure_opportunities_resolved: 35,726 → 40,726.
  - committed_bound_after_first_VOID is unchanged at 551,270. This counter counts retained bounds, not gains (host :553).

PARTLY. The claimed fix removes the whole recorded_controls class and 7,356 of the post-fault differences: 23,539 → 16,183, the same as triage's P1 count. That is because merge.c:751 reads the same flag, so ABANDON after a latched ADMIT now faults. No bound, discharge or END appears after a fault. It does turn one existing named guard red, G_counter_control_ADMIT, and leaves 16,183 post-fault differences, which are the other triage causes.

## Check C: keep a pending quiet candidate across a fault

The patch (`CK/drv/patch-c.diff`) changes merge.c:259-263 in `fault()`.
- It no longer unlinks the quiet list or clears `quiet_pending`. It only clears `quiet_ready`, by walking the list.
- The candidate cannot commit under the latch, because `quiet_ready` is recomputed at merge.c:926 through `discovery_bound_allowed` (latch-tested), and the commit at :1261 tests it again. VERIFIED by reading.

Schedules (`chkC`). The enumeration's EARLIER_TRANS14 is modelled as B_D6 = CONFIRM t=6, e=6, gen 2, with D14 as a TRANS of gen 1. Logs: `CK/drv/C-unmod.log` and `C-C.log`. VERIFIED.
```
                      unmodified                                   Check C
C-a B_D6,K,REARM,D14  'VOID20[Cr,counter_change]'  (no fault)      'VOID20[Cr,counter_change] VOID22[Cr,unknown_evidence]'
C-b K,REARM,B_D6,D14  '... VOID22[Cr,unknown_evidence]'            '... VOID22[Cr,unknown_evidence]' (same)
```
- Rows: `failures: 0 of 352`. The runner prints "149 oracle rows, 203 named cases passed", rc=0 (`CK/drv/rows-C.log`, `fail-c.txt`).
- Enumeration (`CK/drv/enum-c.log`):
  - model.post_fault_control_VOID: 23,539 → **16,485**.
  - Every other class is unchanged: recorded_controls 164,463, closed_pending_census 270, the four model.* classes 12 each, pins 13/2/2, positive cohort the same.
  - No `latch.*` failure.
  - candidate_refutation_without_commit: 490,466 → 495,069.

REPRODUCED. The product's outcome depends on order (C-a gives no fault, C-b faults), and the change makes both orders fault. It removes 7,054 post-fault differences and breaks no row or named case. INFERRED: the 7,054 differs from triage's "G2 alone 8,884" because triage's switch was in the model and attributed by necessity. I did not reconcile the two.

## Check D: a kill between the VOID record and the revoked OPEN

The setup (`CK/drv/chkd.c`, built by `build_d.sh`, run by `run_d.sh`; logs `CK/drv/D-e1.log` and `D-quiet.log`) uses the real code on both sides:
- `merge_capture.c` (included by the host).
- The daemon's own sinks `cov_capture_gap` and `cov_publish_startup_gap`, extracted verbatim from open_snapshot.c (the test_exact_perf_coverage.py method).
- `pgwt_cov_tracker_*` (open_snapshot.c, `-DPGWT_SERVER`) and `event_writer.c`, writing real `current.trace` files.
- The reader is `tests/coverage_trace_verdict.c`, the live reader adapter built with event_reader.c and coverage.c, and the shared verdict.

DB is CLOCK_MONOTONIC + 120 s, so BOOT's observation precedes boot_start, as in exact_perf_coverage_host.

The schedule is probe E1: ADc6, CN6, S19, giving END at C=6 and F18. Then one good tick writes WATERMARK W=19 and publishes. Then PTR_NONNULL@12.
- The capture copies the file in two places:
  - (a) Inside the VOID sink, after the tracker has written GAP_OPEN(VOID) and fflush+fsync'd it (event_writer.c:1664, VERIFIED by reading). The core has not yet returned to capture_core_feed, which writes the revoked OPEN (merge_capture.c:262-266).
  - (b) After the call returns.
- Variant `quiet`: G2b's prefix (AD6, B6, S19) and then a late TRY. It gives the same results.

Records in each file (from `chkd_dump`, real reader, times relative to DB; VERIFIED):
```
at_void (548 bytes, 5 blocks, flags=0x3: LZ4|CLOSURE_TRACKED, not CLOSED_CLEAN):
  0 BOOT   1 GAP_OPEN a=0 (startup, id 1)   2 GAP_CLOSE a=6 (END, id 1)   3 WATERMARK 19/19/19   4 GAP_OPEN a=21 (VOID unknown_evidence, id 2)
after_return (644 bytes, 6 blocks): the same five + 5 GAP_OPEN a=0 (revoked startup OPEN, id 3). No DATA blocks: E1 has no TRANS records.
```
Reader, with now=DB+1000:
```
            [6,7)      [10,18)    [1,5)        [19,20)/[21,22)
at_void     complete   complete   incomplete   incomplete (capture_loss)
after_ret   incomplete incomplete incomplete   incomplete (capture_loss)
```
- Views inside the VOID sink: `closed=1 P=1+0` (E1). After the return: `closed=0 P=1+0`, `O!0`.

REPRODUCED. The bytes on disk at the kill point read COMPLETE for [6,18), a window the revoked bound covered. The VOID's own GAP_OPEN starts at 21 (certified_end), so it does not cover the window. Only the revoked OPEN at 0 makes the file INCOMPLETE.
- INFERRED: no restart or recovery path amends a crashed boot's coverage records. I did not run one.

## Not run / not checked
- make check, box-check and anything eBPF-related (excluded).
- A real SIGKILL: Check D copies the bytes after the fsync instead. Also not run: daemon restart/recovery on the crashed file, and the daemon's own `pgwt_coverage_tick` (I wrote the WATERMARK with `pgwt_writer_coverage_watermark` directly).
- The rows and named cases were run individually in my ASan build. The official runner, without sanitizers, was run once per tree and stops at the first failure.
- Mutant suites (`--mutants`) under B or C.
- Whether the remaining 16,183 / 16,485 post-fault differences split as triage says.
