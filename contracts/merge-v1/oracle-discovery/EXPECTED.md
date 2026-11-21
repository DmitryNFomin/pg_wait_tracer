## 3. EXPECTED (not shown to answerers)

`G(c)` means closure committed with persisted end `P_close=c`, so the persisted startup gap is `[0,c)`. `G(open)` means it remains open. `P=n` is the unresolved census-lifetime count. Gap publications are separate from merge DATA/FRONTIER output.

`abandoned=n` is the capture-cumulative `discovery_abandoned_records` diagnostic. It counts records consumed under Decision 45, not records merely staged, rejected on arrival, or discarded by ordinary fault handling. It survives REARM.

`uncovered +n` counts cookie-less records consumed as uncovered, separately from `abandoned=n`.

`[U]` and `[T]` mean `prior_interval_certified=false/true`. They describe output semantics; this amendment adds no discovery storage carrier. `F<n>` and `VOID<d>[C,reason]` retain §5’s notation.

Unless specified otherwise, final census completeness is known. Rows faulting before any lifetime discharge retain `G(open), P=1`. No listed row requires nonzero process exit. A11 additionally invalidates the epoch containing the disproved F15; among the rows before R1 no other row requires epoch invalidation. R12 requires it explicitly.

R rows: `b=—, x=—` means neither commitment exists. `L=1` denotes the permanent certification latch. `O!` denotes one persisted revoked OPEN at boot start, including when the gap was already open. Unless stated: `census=known`, `abandoned=0`, uncovered increment 0, no additional publication/fault/invalidation, and no nonzero exit. Every row begins with the ordinary startup OPEN. For q, `Vq` is empty or `VOID20[Cr,counter_change]`; a subsequent contradiction uses the current vector. `Vu=VOID20[current-vector,unknown_evidence]`. These are startup-gap expectations, not claims that a whole reader window is COMPLETE despite an ordinary VOID. Reader comparisons include all supplied ordinary coverage restrictions.

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
| T1 | No output at S(19); after TERMU `F18`, then after S(21) `A20[T] → F20`; EXIT never published, uncovered +1, `G(open), P=1`, census complete still known, no VOID. |
| T2 | No output at all, then `VOID50[C0,unknown_evidence]`, `G(open), P=1`. |
| T3 | T1's output then `VOID20[C0,unknown_evidence]`, `G(open), P=1`. |
| T4 | `F18`, no hold, uncovered +1, `G(open), P=1`. |
| V1 | `EXIT_D18 → VOID20[Ce,counter_change] → F19`; `G(open), P=1`, no closure in either order. |
| V2 | `EXIT_D18 → VOID20[Ce,counter_change] → F19`; `G(open), P=1`, no closure in either order. |
| V3 | `EXIT_D17 → VOID20[Cw,writer_push_fail] → F19` (A18 not published; F19 after REARM, under Cw); `G(open), P=1` after REARM and TERM. |
| V4 | `EXIT_D18 → VOID20[Cw,writer_push_fail] → F19` (F18 not published; F19 after REARM, under Cw); `G(open), P=1`. |
| N1 | `F5 → F7`; at `S(6)`, no bound, `G(open), P=1+0`; at `S(8)`, `b_D=6, C_close=6, G(6), P=0+0, census=known`. Attempts 0. No backend listing or role-support publication; no contribution to `N_k`, `U_k` or numerical numerators. |
| N2 | `F7`; no ADMIT or bound; `G(open), P=1+0, census=known`; attempts 0, pointer watch retained. |
| N3 | `F7`; no ADMIT or bound; `G(open), P=1+0, census=known`; attempts 0; the pre-enable read does not establish the required witness. |
| N4 | `F9`; no ADMIT or bound; `G(open), P=1+0, census=known`; attempts 0, C0 unchanged; the initial zero-fire requirement fails without manufacturing a loss. |
| N5 | `F5 → F7 → VOID20[C0,unknown_evidence]`; the pointer contradiction reports `FAULT(unknown_evidence,20,C0)`. Revoke `b_D` and coverage from 0, invalidate affected cached answers; final `G(open), P=1+0, census=unknown`. No F13 or handoff gap. |
| N6 | `F5 → F7 → EXIT_D12 → F12`; `b_D=6, C_close=6, G(6), P=0+0, census=known`; C0 unchanged, no terminal TRANS, no numerical contribution. |
| N7 | `F12`; `b_D=11, C_close=11, G(11), P=0+0, census=known`; one wait-word attempt, started after the non-NULL observation. PTR9 establishes no bound. |
| N8 | `EXIT_D9 → F9`; no `b_D`, accepted `x_D=9`, `C_close=9, G(9), P=0+0, census=known`; attempts 0, no terminal TRANS or UNTRACED. |
| N9 | `VOID20[Cf,counter_change]`; no bound or frontier; `G(open), P=1+0, census=unknown`; attempts 0. |
| N10 | `F5 → F7`; final `b_D=6, C_close=6, G(6), P=0+0, census=known`; attempts 0. No backend listing or role-support publication; no contribution to `N_k`, `U_k` or numerical numerators. |
| N11 | `F7`; no ADMIT or bound; `G(open), P=1+0, census=known`; attempts 0. The unqualified role value supplies no permanence proof. |
| N12 | `VOID20[C0,unknown_evidence]`; no accepted admission, bound or frontier; `G(open), P=1+0, census=known`. |
| N13 | `H10[U] → F10`; `b_D=6, b_H=10, C_close=10, G(11), P=0+0, census=known`. H10 requires the endpoint increment; D contributes no numerical record. |
| N14 | `VOID20[Cw,writer_push_fail]`; no frontier or committed bound; `G(open), P=1+0, census=known`. |
| N15 | `VOID20[Cr,counter_change]`; no bound or frontier; `G(open), P=1+0, census=known`; no synthetic successful NULL observation. |
| N16 | `F15`; no ADMIT or bound; `G(open), P=1+0, census=known`; attempts 0. Report exceeded waiting limit; deadline expiration discharges nothing. |
| J1 | `F3 → EXIT_U4 → F7`; at `S(4)`, matched disposition is retained but no discharge commits, `G(open), P=1+0, census=known`; final `x_U=4, C_close=5, G(5), P=0+0, census=known`. No terminal TRANS or UNTRACED. |
| J2 | `F7`; `C_close=5, G(5), P=0+0, census=known`; no inventory lifetime, terminal discharge or EXIT is invented. |
| J3 | `F7`; `G(open), P=0+1, census=unknown`; no bound, terminal discharge or gone-proof. |
| J4 | `F7`; `G(open), P=0+1, census=unknown`; EACCES and EPERM establish no resolution. |
| J5 | `VOID20[Cf,counter_change]`; no frontier or G resolution; `G(open), P=0+1, census=unknown`. |
| J6 | No merge output; no matched disposition or discharge; `G(open), P=0+1, census=unknown`. L remains unbounded and completion proof remains absent. |
| J7 | `VOID20[Ck,counter_change]`; no discovery EXIT or discharge; `G(open), P=0+1, census=unknown`. Equality with `F0` does not pass the pre-existing exit test. |
| J8 | `F7`; no lifecycle EXIT or terminal disposition; `C_close=5, G(5), P=0+0, census=known`; C0 unchanged. Required thread-history bookkeeping is retained. |
| J9 | `F7`; no lifecycle EXIT or terminal disposition; `C_close=5, G(5), P=0+0, census=known`; C0 unchanged. |
| J10 | `F7`; no lifecycle EXIT or terminal disposition; `C_close=5, G(5), P=0+0, census=known`; C0 unchanged. |
| J11 | `VOID20[C0,unknown_evidence]`; no discovery EXIT, match or frontier; `G(open), P=0+1, census=unknown`. Raw zero is not a pre-`F0` timestamp. |
| J12 | `FORK_R3 → R6[U] → F7`; `C_close=5, G(5), P=0+0, census=known`; no discovery admission for R. R's Decision 25c numerical lower bound is 6; R6's incoming numerical evidence is unavailable. R contributes `dN_0=1, dU_0=0`. |
| J13 | `U7[U] → F7`; `b_U=7, C_close=7, G(8), P=0+0, census=known`. The number becomes one bound discovery lifetime before admission. |
| J14 | `F7`; `C_close=5, G(5), P=0+0, census=known`; no discovery lifetime or terminal discharge is created. |
| J15 | `EXIT_U4 → FORK_R6 → R8[U] → F8`; accepted `x_U=4`, `C_close=5, G(5), P=0+0, census=known`. R retains fork provenance and does not replace U's terminal identity. R's Decision 25c numerical lower bound is 8; R8's incoming numerical evidence is unavailable. R contributes `dN_0=1, dU_0=0`; U contributes nothing to `N_k,U_k`. |
| J16 | `VOID20[C0,unknown_evidence]`; no matched discharge or frontier; `G(open), P=1+0, census=known`. PID equality cannot repair the start-identity contradiction. |
| J17 | `VOID20[C0,unknown_evidence]`; no matched discharge or frontier; `G(open), P=1+0, census=known`. The journal establishes U, but the claimed terminal timestamp agreement is false. |
| J18 | `VOID20[Cw,writer_push_fail]`; EXIT_U4 is not a successful output; no frontier or discharge; `G(open), P=1+0, census=known`. |
| J19 | `EXIT_U4 → VOID20[Cf,counter_change]`; no frontier or committed discharge; `G(open), P=1+0, census=unknown`. The published EXIT belongs to the VOIDed suffix. |
| J20 | `F7`; initially `G(5), P=0+0, census=known`; detection at 40 revokes coverage from 0 and affected cached answers, leaving `G(open), P=1+0, census=unknown`. The late-unlisted observation alone adds no DATA or VOID. |
| J21 | `F7`; `G(open), P=0+1, census=unknown`; the pre-open ESRCH does not satisfy G. |
| J22 | `FORK_R1 → R6[U] → F7`; `C_close=5, G(5), P=0+0, census=known`; observed fork provenance wins, with no discovery admission or journal-based discovery discharge. R's Decision 25c numerical lower bound is 6; R6's incoming numerical evidence is unavailable. R contributes `dN_0=1, dU_0=0`, using FORK timestamp 1. |
| R1 | `D14[U] → F14 → Vu`; `b=—, x=—, G(open), P=1+0, L=1`; no O!. Retained released evidence rejects B6 in both variants. |
| R2 | Without K: `Vu`; with K: `VOID20[Cr,counter_change] → Vu`. No DATA/frontier; `b=—, x=—, G(open), P=1+0, L=1`. No abandonment consumption or O!, in all identity modes and arrival orders. The contradiction is detected at the second member of the TERM/DATA pair. |
| R3 | `F7 → Vq → Vu`; remove `b=6`, final `b=—, x=—, G(open), P=(1+h)+0, L=1`, one O!. Before contradiction, h=0 has historical `G(6)`, h=1 remains open. DATA variant retains known census; pointer/break variants make census unknown under existing Decision 43. |
| R4 | `F13 → Vq → Vu`; remove `b=12`, final `b=—, x=—, G(open), P=(1+h)+0, L=1`, one O!, census known. Historical `G(12)` exists iff h=0. |
| R5 | `EXIT_D18 → F18 → Vq → Vu`; remove `x=18`, final `b=—, x=—, G(open), P=(1+h)+0, L=1`, one O!, census known. Historical `G(18)` exists iff h=0. Abandonment variant still has abandoned=0: D20 is rejected, not consumed. |
| R6 | `EXIT_D18 → F20 → Vu`; `b=—, x=—, G(open), P=1+0, L=1, abandoned=1`; no O!. D20's completed discard remains terminal-refutation evidence. |
| R7 | (a) `EXIT_D18 → F18 → VOID20[Cr,counter_change] → F24[Cr]`; retain `x=18, G(18), P=0+0, L=1`, exactly one historical END and no O!. (b) `D12[U] → F12 → VOID20[Cr,counter_change] → F24[Cr]`; retain `b=12, G(13), P=0+0, L=1`, exactly one END and no O!. |
| R8 | All: `b=—, x=—, G(open), P=1+0, L=1`, no END or O!. Without RA, only `VOID20[Cr,counter_change]`. With RA, (a) adds `F24[Cr] → D26[U] → F26[Cr]`; (b,c) add `F24[Cr]`. No refused candidate supplies certified numerical evidence. |
| R9 | All: `b=—, x=—, G(open), P=1+0, L=1`, no END or O!. (a) `EXIT_D18 → F18 → VOID20[Cr,counter_change]`, plus `F24[Cr]` iff RA. (b) `VOID20[Cr,counter_change]`, plus `EXIT_D26 → F26[Cr]` iff RA. Valid TERM adds no fault. |
| R10 | `VOID20[C0,unknown_evidence] → VOID20[C0,unknown_evidence]`; `b=—, x=—, G(open), P=1+0, census=known, L=1`; no O!. The admitted permanence contradiction and the separately false CN claim each fault. |
| R11 | `EXIT_D18 → F18`; `b=—, x=—, G(open), P=1+0, census=known, L=0`; no hold, END, fault or uncovered increment. |
| R12 | `EXIT_D18 → F18 → VOID20[C0,seal_violation]`; invalidate the supporting epoch, remove `x=18`, retain historical END 18 followed by O!; final `b=—, x=—, G(open), P=1+0, census=known, L=1`. No additional unknown-evidence fault: D12 does not postdate EXIT18. |
| P1 | N1’s amended output and state; absence of the publisher does not hold confirmation or invoke writer failure. |
| P2 | `F5 → F7 → VOID20[C0,unknown_evidence]`; revoke b_D and coverage from boot, invalidate affected caches; `G(open), P=1+0, census=unknown`. |
| P3 | `F5 → F7 → EXIT_D12 → F12`; persisted `G(6)` unchanged; no backend listing or contribution to N/U or numerical values. |

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
| Release the held EXIT on a boundary or after N rounds | T2 |
| Let unresolved go through the late-unlisted branch | T1 |
| Accept matched-after-unresolved | T3 |
| Discharge from the suffix of a VOIDed attempt | V1 |
| Make discharge depend on TERM/EXIT order | V2 |
| Discharge before the release attempt ends | V3, V4 |
| Require a quiet bracket or CPU seed for no-wait-word confirmation | N1, N10 |
| Accept NULL alone as a permanent-role proof | N2, N11 |
| Accept the NULL read before pointer-watch enable | N3 |
| Ignore an initial same-value pointer fire when testing zero fires | N4 |
| Commit a no-wait-word bound at source-completeness equality | N1 |
| Disable or silently replace the pointer watch after confirmation | N5, N12 |
| Convert later non-NULL initialization into a fresh-record handoff gap | N5 |
| Revoke coverage only from the contradiction's detection time | N5 |
| Count a qualified later NULL fire as `wait_read_fail` | N6 |
| Replace an established no-wait-word bound with its exit timestamp | N6 |
| Manufacture a terminal TRANS or UNTRACED for a no-wait-word lifetime | N6, N8 |
| Consume wait-word attempts while the pointer is NULL | N2, N7, N8, N16 |
| Treat initialization notification itself as a lower bound | N7 |
| Close startup coverage when authentication timeout expires | N16 |
| Ignore journal-loss counter changes during no-word qualification | N9 |
| Treat a required failed pointer read as a successful NULL observation | N15 |
| Add no-wait-word lifetimes to numerical population, numerators, `N_k` or `U_k` | N1, N10 |
| Display missing individual quantities as measured zero | N1, N10 |
| Always add one nanosecond for a no-wait-word confirmation | N1, N6, N10 |
| Suppress another lifetime's endpoint increment when route (c) exists | N13 |
| Commit a no-word bound or closure despite rejected supporting publication | N14 |
| Invent an inventory lifetime from an unreadable PID number | J2, J3, J4 |
| Discharge a journal lifetime before strict EXIT eligibility | J1 |
| Use open ENOENT, liveness success or EPERM as G | J3, J4 |
| Accept a gone-proof taken before the failed open | J21 |
| Accept G despite lifecycle/journal loss | J5 |
| Match from journal contents without completed producer accounting | J6 |
| Accept `start_time==F0` as pre-existing | J7 |
| Silently drop an unaccounted post-`F0` child without counting missing-fork loss | J7 |
| Allow a non-leader through the discovery exit filter | J8 |
| Allow a tombstone to establish exit membership | J9 |
| Allow a stale live discovery identity to establish exit membership | J10 |
| Interpret raw zero/unknown start time as preceding `F0` | J11 |
| Put a valid fork-route replacement into discovery inventory | J12, J22 |
| Drop a successfully rebound pre-existing target as a non-member | J13 |
| Keep a fully reconciled, rebound non-member pending | J14 |
| Match an old EXIT using the replacement PID's current map value | J15, J16 |
| Ignore journal/EXIT timestamp agreement | J17 |
| Discharge after rejected EXIT publication | J18 |
| Discharge from an EXIT in a subsequently VOIDed release attempt | J19 |
| Let G suppress later-unlisted revocation | J20 |
| Prefer task-start age over observed fork provenance | J22 |

Pointer-ABA, stale-marker refresh, all-emission-site cookie consistency, writer synchronization and actual EL8 verifier acceptance remain producer qualification obligations. Their qualification fixtures must exercise the corresponding physical operation; merge oracle agreement alone cannot mark them qualified.

Remaining owner decision: none