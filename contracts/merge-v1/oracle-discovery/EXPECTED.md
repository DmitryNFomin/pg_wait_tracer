## 3. EXPECTED (not shown to answerers)

`G(c)` means closure committed with persisted end `P_close=c`, so the persisted startup gap is `[0,c)`. `G(open)` means it remains open. `P=n` is the unresolved census-lifetime count. Gap publications are separate from merge DATA/FRONTIER output.

`abandoned=n` is the capture-cumulative `discovery_abandoned_records` diagnostic. It counts records consumed under Decision 45, not records merely staged, rejected on arrival, or discarded by ordinary fault handling. It survives REARM.

`[U]` and `[T]` mean `prior_interval_certified=false/true`. They describe output semantics; this amendment adds no discovery storage carrier. `F<n>` and `VOID<d>[C,reason]` retain §5’s notation.

Unless specified otherwise, final census completeness is known. Rows faulting before any lifetime discharge retain `G(open), P=1`. No listed row requires nonzero process exit. A11 additionally invalidates the epoch containing the disproved F15; no other listed row requires epoch invalidation.

| ID | Expected output and state |
|---|---|
| D01 | `D12[U] → D15[T] → F15`; `G(13), P=0`. X in `[12,16)` is unavailable. In `[13,16)`, X duration 2, count 1. The completed-Y count in `[12,13)` is unavailable, not zero. |
| D02a | `F11 → F12 → D15[T] → F15`. At `S(12)`, confirmation remains pending and `P=1`. At `S(13)`, commit `G(12), P=0`. X duration 3, count 1. |
| D02b | `F15`; `G(open), P=1`. The reread establishes no numerical baseline. |
| D02c | `D9[U] → D10[T] → F12`; `G(10), P=0`. The ordinary reread establishes nothing. X is established open at 10; the row supplies no closed X interval or independent open-wait snapshot establishing its duration through 13. |
| D02d | `VOID20[C0,unknown_evidence]`; no accepted confirmation. |
| D02e | `F15`; `G(open), P=1`. Retry exhaustion establishes no lower bound. |
| D03 | Hold DATA and frontier before AD. Then `D12[U] → D15[T] → F15`; `G(13), P=0`. |
| D04a-install | `VOID20[C0,unknown_evidence]`; no trusted DATA/frontier or lower bound. |
| D04a-state | `VOID20[C0,unknown_evidence]`; no trusted DATA/frontier or lower bound. |
| D04a-binding | `VOID20[C0,unknown_evidence]`; no trusted DATA/frontier or lower bound. |
| D04a-address | `VOID20[C0,unknown_evidence]`; no trusted DATA/frontier or lower bound. |
| D04a-cookie | `VOID20[C0,unknown_evidence]`; no trusted DATA/frontier or lower bound. |
| D04b | `F15`; `G(open), P=1`. |
| D04c | `VOID20[C0,unknown_evidence]`; the claimed binding supplies no discovery admission. |
| D04d | No merge output. Tombstone remains; insertion returns EEXIST. `G(open), P=1`. Kernel terminal accounting occurs, but no delivered matching witness/fence discharges D in this row. |
| D04e | `VOID20[Cf,counter_change]`. Both late insertions encounter tombstone; R receives no discovery cookie. No trusted discovery output or gap closure. |
| D04f | Reservations return exactly `2^63−1` and `2^64−1`. Both subsequent requests allocate nothing and neither wraps. `VOID20[C0,allocation_fail]`. |
| D04g | Advanced producer fields remain intact. `VOID20[Cs,counter_change]`. The failed insertion authorizes no attachment or lower bound for its target. |
| D07a | `F20`; `G(20), P=0`. |
| D07b | `F20 → F25`. At `S(21)`, both lifetime lower bounds are established, `P=0`, but the gap remains open because completeness is not strictly beyond the census fence. At `S(26)`, commit `G(25)`. |
| D08a | `F17 → EXIT_H18 → F18`. At `S(18)`, `G(open), P=1`; H’s EXIT is not yet eligible. At `S(19)`, `G(18), P=0`. No terminal TRANS or UNTRACED is invented. |
| D08b | `F17 → F18`; `G(open), P=1`. ESRCH does not discharge H. |
| D09 | `D12[U] → D15[T] → terminal_D18[T] → EXIT_D18 → FORK_R20 → R22[U] → F22`; startup `G(13), P=0`. Delayed D records retain D’s cookie. Cleanup leaves R intact; discovery reinsertion encounters tombstone. R retains Decision 25c’s separate restriction. |
| D10 | `D12[U] → D15[T] → F15 → F30`. Before the late-unlisted observation, `G(13), P=0`; X in `[12,16)` is unavailable, X in `[13,16)` has duration 2/count 1, and the completed-Y count in `[12,13)` is unavailable. Detection at 40 revokes base coverage from 0, invalidates affected cached answers and leaves `G(open), P=1` for U. The uncovered-record observation alone emits no new DATA or invented VOID; census completeness must be re-established. |
| D13a | `VOID20[Ce,counter_change]`; D12 is not released. |
| D13b | A2 result `Unavailable/untraced_invalid`. Discovery adds neither a birth nor an omission. No merge output is specified by this coverage-only row. |
| D13c | `VOID20[C0,unknown_evidence]`; no discovery admission for R and no release of its staged FORK. |
| D13d | `VOID20[Cw,writer_push_fail]`; D12 is absent from successful outputs. No F12, committed lower bound or gap closure. |
| D13e | `VOID20[C0,unknown_evidence]`; Q discovery remains unqualified. |
| Q01 | `D15[T] → F15`; `G(12), P=0`. Supplied X duration 3, count 1; CPU 1. |
| Q02 | No quiet confirmation. Producer advances survive rejection without rollback or replacement. `D15[U] → F15`; `G(16), P=0`. D15’s incoming duration, CPU and completion are unavailable for numerical use; the requested `[12,16)` result is unavailable. |
| Q03 | No quiet confirmation. `D15[U] → F15`; `G(16), P=0`. Incoming interval quantities and the requested `[12,16)` result are unavailable. |
| Q04 | No quiet confirmation. `D15[U] → F15`; `G(16), P=0`. Equal counters and state letters supply no inactivity proof. The requested `[12,16)` result is unavailable. |
| Q04b | No quiet confirmation. `D15[U] → F15`; `G(16), P=0`. Constant schedstat values supply no inactivity proof. The requested `[12,16)` result is unavailable. |
| Q05 | No quiet confirmation and no synthetic loss increment from the optional witness denial. `D15[U] → F15`; `G(16), P=0`. The requested `[12,16)` result is unavailable. |
| Q06 | No quiet confirmation. The independently qualified final attachment uses the fresh-record route: `D15[U] → F15`; `G(16), P=0`. The requested `[12,16)` result is unavailable. |
| Q07 | Seed retains `on_cpu_ts=0`, numeric CPU base 0 and `base_valid=true`. `D15[T] → F15`; `G(12), P=0`. CPU for `[12,15)` is 1, neither 0 nor 3. |
| Q08 | No quiet confirmation or ADMIT. `F15`; `G(open), P=1`. The address mismatch supplies no lower bound. |
| Q09 | `D9[U] → F11 → D15[T] → F15`; `G(11), P=0`. D9 establishes nothing and does not replace the final seed. D15’s X duration is 4, count 1. The committed lower bound remains 11. |
| D14a | `A12[T] → D12[U] → D15[T] → F15`; `G(13), P=0`. The completed-Y count across A and D in `[12,13)` is unavailable, not zero. A12 retains its certified record semantics. In `[13,16)`, X duration 2, count 1. |
| D14b | `D12[U] → D15[T] → F15`; `G(15), P=0`. In `[15,16)`, X duration 0, count 1. D15’s certified completion belongs to the window although its interval has zero overlap with it. |
| D14c | `D11[U] → F11 → D15[T] → F15`; `G(12), P=0`. The earlier-attempt D11 establishes nothing and does not replace the α3 seed. The committed lower bound remains 11. X in `[11,16)` is unavailable; in `[12,16)`, X duration 3, count 1. |
| A01 | No output through `S(13)`. AB permits `F12`, then `S(16)` produces `F15`: `F12 → F15`. No D DATA is published. `G(open), P=1, abandoned=2`; no `b_D`. |
| A02 | `F12 → F15 → F19`; no D DATA. `G(open), P=1, abandoned=3`; no `b_D`. |
| A03 | `F12 → F15 → F17 → EXIT_D18 → F18`. At `S(18)`, `G(open), P=1, abandoned=2`. At `S(19)`, the terminal TRANS is consumed without publication, EXIT releases, and terminal discharge commits: `C_close=18`, `G(19), P=0, abandoned=3`. No `b_D` or UNTRACED is created. |
| A04 | `D12[U] → F12 → VOID20[C0,unknown_evidence]`. D’s lower bound committed before AB; H remains unresolved. `G(open), P=1, abandoned=0`. AB is rejected because D already has accepted ADMIT. The earlier published output is not erased. |
| A05 | `VOID20[C0,unknown_evidence]`; no D DATA or frontier. `G(open), P=1, abandoned=0`. D9 was staged at W=0 when the later ADMIT contradicted abandonment. |
| A06 | `VOID20[C0,unknown_evidence]`; `G(open), P=1, abandoned=0`. H is not an inventoried identity. |
| A07 | `H15[T] → F15`; `G(open), P=1, abandoned=1`. H’s committed lower bound is 12. D remains unresolved with no lower bound; its D9 is not published. |
| A08 | `VOID20[Ce,counter_change]`; no DATA or frontier. `G(open), P=1, abandoned=0`. Abandonment does not forgive the counter change. |
| A09 | `F12 → F15 → F17 → EXIT_D18 → F18`; `G(19), P=0, abandoned=3`. The duplicate AB after closure produces no additional output, fault, disposition or diagnostic increment. |
| A10 | `F12 → F15 → VOID20[C0,unknown_evidence]`; `G(open), P=1, abandoned=2`. CONFIRM contradicts the retained abandonment; it supplies no lower bound. |
| A11 | `F12 → F15 → VOID20[C0,seal_violation]`; `G(open), P=1, abandoned=2`. Reject D14 before discard consumption. Its timestamp does not regress below the preceding P0 DATA timestamp 12, but violates P0’s accepted boundary 16 and disproves F15. Mark and report that epoch invalid. |
| A12 | `F12 → F15 → VOID20[C0,allocation_fail] → F17`; `G(open), P=1, abandoned=3`. REARM is accepted; abandonment, the existing discard count, retained arrival boundaries and the last published frontier survive. D17 is consumed without publication. No discovery lower bound is created. |
| A13 | No merge output. E’s cutoff remains zero. `G(open), P=1, abandoned=0`; D9 remains staged. |
| A14 | `VOID20[unknown,counter_unknown]`; no DATA or frontier. `G(open), P=1, abandoned=0`. Incomplete failure accounting is not repaired by AB. |
| A15 | `F12 → F15 → F17 → VOID20[Cw,writer_push_fail]`. The terminal TRANS was consumed, so `abandoned=3`; the rejected EXIT is not a successful output. `G(open), P=1`; no F18, committed terminal discharge or gap closure. |
| A16 | `VOID20[C0,unknown_evidence]`; no DATA or frontier. `G(open), P=1, abandoned=0`. AB cannot establish the uncertain record attribution. |
| A17 | `F12 → F15 → F17 → EXIT_D18 → F18 → FORK_R20 → R22[U] → F22`; startup `G(19), P=0, abandoned=3`. R does not inherit D’s abandonment. R22 retains Decision 25c’s separate restriction. |
| A18 | `VOID20[C0,allocation_fail]`; no later DATA or frontier. `G(open), P=1, abandoned=0`. AB does not cancel the fault or rearm the epoch. D9 was discarded by ordinary fault handling, not consumed under Decision 45. |
| A19 | `F15`; no TEXT, TRANS or MARKER publication. `G(open), P=1, abandoned=3`. The valid metadata dependency is reconciled before consumption; TEXT is not assigned a timestamp. |
| A20 | `EXIT_D18 → F18 → VOID20[C0,unknown_evidence]`; `G(18), P=0, abandoned=0`. Terminal discharge closes the gap without an increment. The first AB arrives after closure and is rejected; it is not an idempotent retransmission. |

**Required mutants**

Each mutant must build, reach the named case and fail the intended assertion. A build failure, skipped case or unrelated failure does not count as detection. Adapter/kernel mutations must be exercised through the relevant adapter/kernel fixture; assigning an abstract proof value is not a substitute.

All gap-end assertions must inspect the emitted GAP_CLOSE endpoint and zero reserved field. Availability assertions must use the existing reader on those emitted records. An in-memory `C_close` or `b_i` cannot stand in for either check.

| Mutation | Row(s) that must turn red |
|---|---|
| Certify equal wait rereads or absence of released DATA | D02b, D02c |
| Accept `W=t` as completeness through t | D02a |
| Accept a CPU base sampled before intervening consumption | D02d |
| Close on timeout or retry exhaustion | D02e |
| Assume a failed bracket must eventually produce a record | D02e |
| Select the first arriving record instead of validated release order | D03 |
| Permit enable before identity installation | D04a-install |
| Treat overwritten producer state as preserved | D04a-state |
| Omit lifecycle/task binding | D04a-binding |
| Accept unobserved pointer relocation | D04a-address |
| Accept a cookie outside its allocation authority | D04a-cookie |
| Admit a start time within the required margin | D04b |
| Use an old start timestamp or current numeric-PID read as sufficient FD binding | D04c |
| Delete/recycle tombstones or allow userspace replacement | D04d, D04e |
| Return from EXIT before accounting/tombstoning when mutable state is absent | D04d, D08a |
| Borrow discovery identity after failed fork allocation | D04e |
| Cross allocator partitions or wrap after exhaustion | D04f |
| Replace mutable state during identity insertion | D04g |
| Suppress discovery-map failure accounting | D04g |
| Close at enable or the first admitted/confirmed lifetime | D07a |
| Ignore the census fence or accept equality with its completeness cutoff | D07b |
| Discharge on receipt of EXIT before strict release eligibility | D08a |
| Treat ESRCH as a terminal proof | D08b |
| Resolve queued cookies from the PID’s current entry | D09 |
| Let delayed cleanup retire a replacement lifetime | D09 |
| Revoke late-unlisted coverage only from detection time | D10 |
| Forgive an actual loss through discovery | D13a |
| Admit pre-existing discovery into A2 birth/omission accounting | D13b |
| Admit a post-start birth through discovery | D13c |
| Commit a lower bound or closure after rejected record publication | D13d |
| Admit unqualified Q-mode discovery | D13e |
| Always persist `C_close+1` | D02a, D07a, D07b, D08a, Q01, Q07, Q09, D14b, A20 |
| Never persist `C_close+1` | D01, D02c, D03, D09, Q02–Q06 including Q04b, D14a, D14c, A03 |
| Increment only for a fresh-record anchor; ignore earlier-attempt records at C | D14c |
| Ignore discarded numerical records when testing the endpoint | A03 |
| Increment whenever any uncertified discovery record exists, even strictly below C | Q09, D14b |
| Treat lifecycle EXIT alone as an uncertified numerical endpoint | D08a, A20 |
| Replace the unavailable instant with a global endpoint-exclusion flag or a nonzero reserved field | D01, D14a |
| Report the unknown completed count at C as zero | D01, D14a |
| Drop a certified completion at C when no uncertified discovery record is there | D14b |
| Use `C_close` or `b_i` instead of persisted `P_close` for numerical availability | D01, D14c |
| Let abandonment supply a lower bound, discharge the inventory entry or close the gap | A01, A07 |
| Keep the missing-ADMIT dependency after accepted abandonment | A01, A07 |
| Publish abandoned staged or future TRANS records | A01, A02 |
| Publish abandoned TEXT or MARKER records | A19 |
| Omit, double-count or reset consumed-discard diagnostics | A01, A09, A12, A19 |
| Count a merely staged or rejected record as an abandonment discard | A05, A11, A13 |
| Discard the matching lifecycle EXIT | A03 |
| Discharge on terminal-control receipt or at `W=x_i` | A03 |
| Accept ABANDON after accepted ADMIT while the startup gap is still open | A04 |
| Accept ADMIT after abandonment | A05 |
| Accept CONFIRM after abandonment | A10 |
| Accept an uninventoried identity | A06 |
| Treat a first abandonment after closure as valid | A20 |
| Reject or reapply an identical abandonment after terminal discharge/closure | A09 |
| Forgive a loss-counter change through abandonment | A08 |
| Manufacture a storage loss increment for ordinary abandonment | A01 |
| Skip arrival-boundary validation for abandoned records, or suppress invalidation of a disproved frontier | A11 |
| Forget abandonment across REARM | A12 |
| Let AB supply missing source completeness | A13 |
| Permit release with incomplete failure accounting | A14 |
| Commit terminal discharge, closure or F18 after rejected EXIT publication | A15 |
| Let abandonment repair unknown record attribution | A16 |
| Apply abandonment by PID rather than retained lifetime identity | A17 |
| Let AB cancel a fault or implicitly rearm | A18 |
| Reject every quiet witness or retain first-record exclusion after valid confirmation | Q01 |
| Accept changed voluntary-switch counts | Q02 |
| Roll back or overwrite producer advances after a failed bracket | Q02 |
| Accept a `running` endpoint | Q03 |
| Accept counter/state-only inactivity evidence | Q04 |
| Accept constant-zero schedstat as inactivity evidence | Q04b |
| Treat denied proof as passed | Q05 |
| Ignore the single-thread requirement | Q06 |
| Infer on-CPU state from wait word zero | Q07 |
| Treat numeric CPU base zero as invalid | Q07 |
| Replace a measured zero/partial CPU delta with elapsed wall time | Q07 |
| Skip the in-bracket address comparison | Q08 |
| Let an earlier-attempt or pre-enable record establish `b_i` | Q09 |
| Let delayed earlier-attempt DATA overwrite the confirmed baseline | Q09 |
| Reset/reuse an attachment generation across retries | Q09 |

Pointer-ABA, stale-marker refresh, all-emission-site cookie consistency, writer synchronization and actual EL8 verifier acceptance remain producer qualification obligations. Their qualification fixtures must exercise the corresponding physical operation; merge oracle agreement alone cannot mark them qualified.

Remaining owner decision: none