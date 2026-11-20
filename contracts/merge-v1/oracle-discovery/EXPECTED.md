## 3. EXPECTED (not shown to answerers)

`G(c)` means the persisted startup gap is `[0,c)` and closure has committed. `G(open)` means it remains open. `P=n` is the unresolved census-lifetime count. Gap publications are separate from merge DATA/FRONTIER output.

`[U]` and `[T]` mean `prior_interval_certified=false/true`. They describe output semantics; this amendment adds no discovery storage carrier. `F<n>` and `VOID<d>[C,reason]` retain §5’s notation.

Unless specified otherwise, final census completeness is known. Rows faulting before any lifetime discharge retain `G(open), P=1`. No listed row requires nonzero process exit or epoch invalidation.

| ID | Expected output and state |
|---|---|
| D01 | `D12[U] → D15[T] → F15`; `G(12), P=0`. X duration 3, count 1. The completion at 12 contributes no count. |
| D02a | `F11 → F12 → D15[T] → F15`. At `S(12)`, confirmation remains pending and `P=1`. At `S(13)`, commit `G(12), P=0`. X duration 3, count 1. |
| D02b | `F15`; `G(open), P=1`. The reread establishes no numerical baseline. |
| D02c | `D9[U] → D10[T] → F12`; `G(9), P=0`. The ordinary reread establishes nothing. X is established open at 10; the row supplies no closed X interval or independent open-wait snapshot establishing its duration through 13. |
| D02d | `VOID20[C0,unknown_evidence]`; no accepted confirmation. |
| D02e | `F15`; `G(open), P=1`. Retry exhaustion establishes no lower bound. |
| D03 | Hold DATA and frontier before AD. Then `D12[U] → D15[T] → F15`; `G(12), P=0`. |
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
| D09 | `D12[U] → D15[T] → terminal_D18[T] → EXIT_D18 → FORK_R20 → R22[U] → F22`; startup `G(12), P=0`. Delayed D records retain D’s cookie. Cleanup leaves R intact; discovery reinsertion encounters tombstone. R retains Decision 25c’s separate restriction. |
| D10 | D01’s output followed by `F30`. Detection at 40 revokes base coverage from 0, invalidates affected cached answers and leaves `G(open), P=1` for U. The uncovered-record observation alone emits no new DATA or invented VOID; census completeness must be re-established. |
| D13a | `VOID20[Ce,counter_change]`; D12 is not released. |
| D13b | A2 result `Unavailable/untraced_invalid`. Discovery adds neither a birth nor an omission. No merge output is specified by this coverage-only row. |
| D13c | `VOID20[C0,unknown_evidence]`; no discovery admission for R and no release of its staged FORK. |
| D13d | `VOID20[Cw,writer_push_fail]`; D12 is absent from successful outputs. No F12, committed lower bound or gap closure. |
| D13e | `VOID20[C0,unknown_evidence]`; Q discovery remains unqualified. |
| Q01 | `D15[T] → F15`; `G(12), P=0`. Supplied X duration 3, count 1; CPU 1. |
| Q02 | No quiet confirmation. Producer advances survive rejection without rollback or replacement. `D15[U] → F15`; `G(15), P=0`. D15’s incoming duration, CPU and completion are unavailable for numerical use. |
| Q03 | No quiet confirmation. `D15[U] → F15`; `G(15), P=0`. Incoming interval quantities are unavailable. |
| Q04 | No quiet confirmation. `D15[U] → F15`; `G(15), P=0`. Equal counters and state letters supply no inactivity proof. |
| Q04b | No quiet confirmation. `D15[U] → F15`; `G(15), P=0`. Constant schedstat values supply no inactivity proof. |
| Q05 | No quiet confirmation and no synthetic loss increment from the optional witness denial. `D15[U] → F15`; `G(15), P=0`. |
| Q06 | No quiet confirmation. The independently qualified final attachment uses the fresh-record route: `D15[U] → F15`; `G(15), P=0`. |
| Q07 | Seed retains `on_cpu_ts=0`, numeric CPU base 0 and `base_valid=true`. `D15[T] → F15`; `G(12), P=0`. CPU for `[12,15)` is 1, neither 0 nor 3. |
| Q08 | No quiet confirmation or ADMIT. `F15`; `G(open), P=1`. The address mismatch supplies no lower bound. |
| Q09 | `D9[U] → F11 → D15[T] → F15`; `G(11), P=0`. D9 establishes nothing and does not replace the final seed. D15’s X duration is 4, count 1. The committed lower bound remains 11. |

**Required mutants**

Each mutant must build, reach the named case and fail the intended assertion. A build failure, skipped case or unrelated failure does not count as detection. Adapter/kernel mutations must be exercised through the relevant adapter/kernel fixture; assigning an abstract proof value is not a substitute.

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
| Count the uncertified completion exactly at startup closure | D01 |
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