## 3. EXPECTED (not shown to answerers)

`G(c)` means closure committed with persisted end `P_close=c`, so the persisted startup gap is `[0,c)`. `G(open)` means it remains open. `P=n` is the unresolved census-lifetime count. Gap publications are separate from merge DATA/FRONTIER output.

`abandoned=n` is the capture-cumulative `discovery_abandoned_records` diagnostic. It counts records consumed under Decision 45, not records merely staged, rejected on arrival, or discarded by ordinary fault handling. It survives REARM.

`uncovered +n` counts cookie-less records consumed as uncovered, separately from `abandoned=n`.

`[U]` and `[T]` mean `prior_interval_certified=false/true`. They describe output semantics; this amendment adds no discovery storage carrier. `F<n>` and `VOID<d>[C,reason]` retain §5’s notation.

Unless specified otherwise, final census completeness is known. Rows faulting before any lifetime discharge retain `G(open), P=1`. No listed row requires nonzero process exit. A11 additionally invalidates the epoch containing the disproved F15; among the rows before R1 no other row requires epoch invalidation. R12 requires it explicitly.

U rows: Use `P=L+Q` for pending lifetimes plus numbers; write the certification latch separately as `latch`. Uncovered counts are capture-cumulative increments. Unless stated, there is no additional DATA, fault, VOID, discharge, END, abandonment consumption or epoch invalidation.

G rows: Unless stated otherwise: `census=known`, `abandoned=0`, uncovered increment 0, no epoch invalidation or additional output. Every execution starts with the ordinary startup OPEN; END and O! are separate gap publications. `Vu=VOID20[C0,unknown_evidence]`; `L` is the certification latch.

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
| V4 | Merge stream: `EXIT_D18 → VOID20[Cw,writer_push_fail] → F19[Cw]`; rejected F18 is absent. Combined publication timeline: `OPEN → EXIT_D18 → VOID20[Cw,writer_push_fail] → F19[Cw]`. At the L boundary of S(19), EXIT succeeds and F18 is rejected before END can be published. No END or rollback OPEN is published; the initial plain OPEN remains unclosed and O! count is zero. At VOID entry: epoch suspended, `admitted=false, b=—, x=—, G(open), P=1+0, census=known, latch=1`; public views, callback reader and independently reopened saved prefix agree that startup coverage is unavailable, including `[50,60)` with otherwise valid supplied coverage. RA restores an active epoch; the repeated TERM adds no fault or discharge, and the L boundary of S(20) publishes F19 under Cw. Final discovery state is unchanged from VOID entry; epoch active, `abandoned=0`, uncovered increment 0. Exactly one writer_push_fail diagnostic increment and its stated storage-counter increment; no additional fault, END, O!, epoch invalidation or nonzero exit. |
| N1 | `F5 → F7`; at `S(6)`, no bound, `G(open), P=1+0`; at `S(8)`, `b_D=6, C_close=6, G(6), P=0+0, census=known`. Attempts 0. No backend listing or role-support publication; no contribution to `N_k`, `U_k` or numerical numerators. |
| N2 | `F7`; no ADMIT or bound; `G(open), P=1+0, census=known`; attempts 0, pointer watch retained. |
| N3 | `F7`; no ADMIT or bound; `G(open), P=1+0, census=known`; attempts 0; the pre-enable read does not establish the required witness. |
| N4 | `F9`; no ADMIT or bound; `G(open), P=1+0, census=known`; attempts 0, C0 unchanged; the initial zero-fire requirement fails without manufacturing a loss. |
| N5 | `F5 → F7 → VOID20[C0,unknown_evidence]`; the pointer contradiction reports `FAULT(unknown_evidence,20,C0)`. Revoke `b_D` and coverage from 0, invalidate affected cached answers; final `G(open), P=1+0, census=unknown`. No F13 or handoff gap. |
| N6 | `F5 → F7 → EXIT_D12 → F12`; `b_D=6, x=—, C_close=6, G(6), P=0+0, census=known`; C0 unchanged, no terminal TRANS, no numerical contribution. |
| N7 | `F12`; `b_D=11, C_close=11, G(11), P=0+0, census=known`; one wait-word attempt, started after the non-NULL observation. PTR9 establishes no bound. |
| N8 | `EXIT_D9 → F9`; no `b_D`, accepted `x_D=9`, `C_close=9, G(9), P=0+0, census=known`; attempts 0, no terminal TRANS or UNTRACED. |
| N9 | `VOID20[Cf,counter_change]`; no bound or frontier; `G(open), P=1+0, census=unknown`; attempts 0. |
| N10 | `F5 → F7`; final `b_D=6, C_close=6, G(6), P=0+0, census=known`; attempts 0. No backend listing or role-support publication; no contribution to `N_k`, `U_k` or numerical numerators. |
| N11 | `F7`; no ADMIT or bound; `G(open), P=1+0, census=known`; attempts 0. The unqualified role value supplies no permanence proof. |
| N12 | `VOID20[C0,unknown_evidence]`; no accepted admission, bound or frontier; `G(open), P=1+0, census=known`. |
| N13 | `H10[U] → F10`; `b_D=6, b_H=10, C_close=10, G(11), P=0+0, census=known`. H10 requires the endpoint increment; D contributes no numerical record. |
| N14 | Merge stream: `F7[C0] → VOID20[Cw,writer_push_fail]`; combined timeline: `OPEN → F7[C0] → VOID20[Cw,writer_push_fail]`. The first three boundaries of S(8) publish nothing; its L boundary publishes F7 and completes its counter checkpoint before END6 is rejected. The rejected END is absent from successful publications. At VOID entry and finally: epoch suspended, `admitted=true, b=—, x=—, G(open), P=1+0, census=known, latch=1`; the initial plain OPEN remains unclosed, and public views, callback reader and independently reopened saved prefix agree that startup coverage is unavailable, including `[50,60)` with otherwise valid supplied coverage. F7 remains historically observable but supplies no committed discovery bound or closure. Exactly one writer_push_fail diagnostic increment and the stated Cw storage-counter increment. Attempts 0, `abandoned=0`, uncovered increment 0; no DATA, END, rollback OPEN, O!, epoch invalidation or nonzero exit; no backend listing, role-support publication or numerical-population contribution. |
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
| R10 | Both pointer and continuity-break variants: `VOID20[C0,unknown_evidence] → VOID20[C0,unknown_evidence]`; `b=—, x=—, G(open), P=1+0, census=known, L=1`; no O!. The first observation refutes the accepted admission before any bound commits; the separately false CN claim faults again. Neither candidate-only refutation revokes coverage or resets the census. |
| R11 | `EXIT_D18 → F18`; `b=—, x=—, G(open), P=1+0, census=known, L=0`; no hold, END, fault or uncovered increment. |
| R12 | `EXIT_D18 → F18 → VOID20[C0,seal_violation]`; invalidate the supporting epoch, remove `x=18`, retain historical END 18 followed by O!; final `b=—, x=—, G(open), P=1+0, census=known, L=1`. No additional unknown-evidence fault: D12 does not postdate EXIT18. |
| P1 | N1’s amended output and state; absence of the publisher does not hold confirmation or invoke writer failure. |
| P2 | `F5 → F7 → VOID20[C0,unknown_evidence]`; revoke b_D and coverage from boot, invalidate affected caches; `G(open), P=1+0, census=unknown`. |
| P3 | `F5 → F7 → EXIT_D12 → F12`; `x=—`; persisted `G(6)` unchanged; no backend listing or contribution to N/U or numerical values. |
| U1 | `VOID20[C0,unknown_evidence] → A26[T] → F27 → F29`; no DATA/frontier between RA and TERMU; uncovered +1, `P=0+1, census=unknown, G(open), latch=1`; no EXIT publication or discharge. |
| U2 | No output before TERMU; then `A6[T] → F7`; uncovered +1, `P=0+1, census=unknown, G(open), latch=0`. |
| U3 | `A6[T] → F7 → VOID20[C0,unknown_evidence]`; conflicting matched disposition faults; uncovered +1, `P=1+0, census=known, G(open), latch=1`; retain J's inventory identity and numeric resolution, no discharge. |
| U4 | `VOID20[C0,unknown_evidence]`; invalid unnamed disposition faults; uncovered +0, `P=0+1, census=unknown, G(open), latch=1`. |
| U5 | `VOID20[C0,unknown_evidence]`; invalid PID obligation faults; uncovered +0, `P=0+1, census=unknown, G(open), latch=1`; no PID-60 inventory entry. |
| U6 | `A6[T] → F7 → F9`; uncovered +1, `P=0+0, census=known, G(open), latch=0`; G resolves only the number; no END or discharge. |
| U7 | `A6[T] → F7 → F9`; uncovered +1, `P=0+0, census=known, G(open), latch=0`; R resolves only the number as a non-member; no END or discharge. |
| U8 | No DATA/frontier or fault; EXIT remains undecided and held; uncovered +0, `P=0+1, census=unknown, G(open), latch=0`. |
| U9 | Only `VOID20[C0,allocation_fail]`; after RA the EXIT remains undecided and holds the new DATA/frontier; uncovered +0, `P=0+1, census=unknown, G(open), latch=1`. |
| U10 | `A6[T] → F7`; the named EXIT has no undecided-disposition hold; uncovered +1, `P=0+1, census=unknown, G(open), latch=0`. |
| U11 | `A6[T] → F7 → F9`; neither duplicate adds output, fault or consumption; uncovered +1, `P=0+0, census=known, G(open), latch=0`. |
| U12 | `D12[U] → D15[T] → F15 → F30 → A44[T] → F44 → F45`; historical `G(13)`, then one revoked OPEN from boot at LATE_U; no output from X_42 through S(45); uncovered +2 total, `P=1+0, census=unknown, G(open), latch=0`; no VOID, extra inventory entry or discharge. |
| U13 | No output or unresolved disposition before J; then `EXIT_U4 → A6[T] → F7`; uncovered +0, `x_U=4, P=0+0, census=known, G(5), latch=0`. |
| U14 | No DATA/frontier, disposition or fault; EXIT remains held; uncovered +0, `P=0+1, census=unknown, G(open), latch=0`. |
| U15 | Only `VOID20[C0,unknown_evidence]`; unresolved disposition is accepted, but EXIT is neither consumed uncovered nor published because ordinary release remains suspended; uncovered +0, `P=0+1, census=unknown, G(open), latch=1`; no discharge. |
| O1 | `VOID20[C0,clock_regression] → VOID20[C0,unknown_evidence]`; no DATA or frontier; `b=—, x=—, G(open), P=1+0, census=known, L=1`, latched by the first VOID; no O!, no epoch invalidation, abandoned=0; diagnostic counts clock_regression +1, seal_violation +0, unknown_evidence +1. |
| G1 | `Vu` at CN6; CN6 is not retained; ADc6 is subsequently recorded without another fault; final `b=—, x=—, G(open), P=1+0, L=1`; no DATA, FRONTIER, END or O!. |
| G2 | `Vu` at TRY; TRY is not recorded, CN6 is recorded without another fault; final `b=—, x=—, G(open), P=1+0, L=1`; no DATA, FRONTIER, END or O!; prefixed variant adds exactly `VOID20[C0,allocation_fail]` before Vu and has the same final state. |
| G2b | `F18 → Vu`; before TRY, `b_D=6, G(6), P=0+0`; TRY revokes the bound; historical END 6 is followed by one O!; final `b=—, x=—, G(open), P=1+0, L=1`. |
| G3 | `EXIT_D18 → F18`; `b_D=6, x=—, C_close=6, G(6), P=0+0, L=0`; one END 6; revocation variant then adds Vu and one O!, removes b, and ends `b=—, x=—, G(open), P=1+0, L=1`; retained terminal evidence is not promoted. |
| G4 | Both variants produce `EXIT_D18 → F18 → Vu`; remove `x_D=18`; historical END 18 is followed by one O!; final `b=—, x=—, G(open), P=1+0, L=1`. |
| G4b | `EXIT_D18 → F18 → Vu`; reject the conflicting unresolved disposition and revoke `x_D=18`; historical END 18 is followed by one O!; final `b=—, x=—, G(open), P=1+0, L=1`; uncovered increment remains 0. |
| G5 | `EXIT_D18 → F18`; AD_D6 is recorded without fault and supplies no bound; TERM commits `x_D=18`; final `b=—, C_close=18, G(18), P=0+0, L=0`; one END 18, no O!. |
| G6 | `Vu` at AD_D6; no accepted admission; final `b=—, x=—, G(open), P=1+0, L=1`; no DATA, FRONTIER, END or O!. |
| G7 | `EXIT_D18 → F18`; discharge and END 18 commit at S(19); later AD_D6 and B_D6 are recorded without fault and commit no bound; final `b=—, x_D=18, C_close=18, G(18), P=0+0, L=0`; exactly one END, no O!. |
| G8 | `EXIT_D18 → F18`; S(22) holds A20, H21 and F21; TERM commits only `x_D=18`; final `b_D=—, b_H=—, x_H=—, G(open), P=1+0, L=0`; no END, O! or fault; the persisted reader remains INCOMPLETE, including for `[50,60)`. |

H rows: `Vk=VOID20[Cr,counter_change]`; `Vu20=VOID20[C0,unknown_evidence]`; `Vu22[C]=VOID22[C,unknown_evidence]`. Frontiers use C0 unless marked Cr. Every execution begins with the ordinary startup OPEN.

Unless a row states otherwise, its final state is `b=—, x=—, G(open), P=1+0, census=known, latch=1`; `abandoned=0`, uncovered increment 0, no END, no revoked OPEN, no epoch invalidation and no nonzero exit. Listed output streams are complete. Each listed VOID increments only its named fault diagnostic; discovery faults add no storage-counter increment. `admitted` is false unless expressly stated.

For every listed revocation, remove the affected commitment, update public views, invalidate dependent cached answers and persist exactly one O! before the associated VOID callback begins. At entry to every H-row VOID callback the epoch is already suspended. Historical END and DATA remain observable. The persisted reader rejects coverage dependent on the revoked proof, including `[50,60)` with otherwise valid supplied coverage. In every H-row release attempt that closes the startup gap, supporting TEXT and DATA precede any advancing FRONTIER and all remaining counter checkpoints, and END is published last; successful END publication commits the affected bound, discharge and closure, with no fallible step of that attempt afterward.

| ID | Expected output and state |
|---|---|
| H1a | `EXIT_D18 → F18 → Vu20`. At S(19), `x_D=18, G(18), P=0+0, latch=0`; AD_D19 is rejected without recording admission. Remove x and persist O! before Vu20. Final default state; historical END 18 remains. |
| H1b | `Vu20` at EXIT arrival; EXIT is not published. `admitted=true` records the earlier accepted ADMIT, whose proof is now refuted. No bound or discharge ever commits; final default state otherwise. |
| H1c | `EXIT_D18 → F18`. `admitted=true, b=—, x_D=18, G(18), P=0+0, census=known, latch=0`; exactly one END 18; no fault or O!. |
| H1d | `EXIT_D18 → F18`. `admitted=true, b=—, x_D=18, G(18), P=0+0, census=known, latch=0`; exactly one END 18; no fault or O!. |
| H2a | `Vk → F24[Cr] → D26[U] → F26[Cr]`. `admitted=true` after AD; final default commitment/pending/gap state. The final epoch remains active. |
| H2b | `Vk → F24[Cr] → D26[U] → F26[Cr]`. `admitted=true` immediately after AD during suspension and after RA; final default commitment/pending/gap state. The final epoch remains active. |
| H2c | Only `Vk`. `admitted=true`; epoch remains suspended. D26 supplies no publication or consumption; no implicit REARM. |
| H2d | `Vk → Vu22[Cr]`, the second fault at AB. `admitted=true`; abandonment is rejected and records no disposition or consumption. |
| H2e | `Vk → Vu22[Cr]`, the second fault at AB. `admitted=true`; abandonment is rejected and records no disposition or consumption. No route-(c) bound commits. |
| H3a | Only `Vu20`, at ADc6. No admission, route-(c) baseline or permanence claim is recorded. The subsequent TRY is not a late-ADMIT violation and produces no additional fault. |
| H3b | Only `Vu20`, at ADc6. The pointer observation produces no additional fault from the rejected admission; census remains known. |
| H3c | Only `Vu20`, at ADc6. The continuity observation produces no additional fault from the rejected admission; census remains known. |
| H3d | `Vu20 → F26`. ADc6 is not recorded; AB is accepted during suspension without another fault. D12 was dropped by ordinary fault handling and is not counted. After RA, D26 is consumed without publication; `abandoned=1`. Final epoch active; no bound, discharge or END. |
| H4a | `Vu20 → A26[T] → F26`. `admitted=true`; TRY is not recorded. B6 is recorded without another fault, supplies no commitment, and does not hold ordinary A release. Final epoch active. |
| H4b | `Vu20 → Vu22[C0]`. `admitted=true`; B6 itself adds no fault. The second late TRY independently violates Decision 38 and faults; neither TRY is recorded. No DATA or frontier. |
| H5a | `Vk → Vu22[Cr]`, the second fault at D14 arrival. The pre-K quiet candidate survives K and RA as evidence, is refuted once and supplies no readiness or commitment. No O!, because it never committed. |
| H5b | `Vk → Vu22[Cr]`, the second fault at D14 arrival. The post-K quiet candidate is retained and refuted with exactly the same consequences as H5a. |
| H6a | `Vu20 → EXIT_D18 → F24`. The first fault occurs at D20 and refutes the recorded terminal claim. EXIT18 supplies no repeated contradiction report and no discharge; final epoch active. |
| H6b | `Vu20 → EXIT_D18 → F24`. The first fault occurs at TERM_D19, which retained D20 already contradicts; no accepted terminal disposition or discharge results. EXIT18 adds no discovery fault; final epoch active. |
| H6c | `Vu20 → Vu22[C0]`. D20 refutes the first terminal claim. The separately delivered conflicting TERM_D21 faults independently; it neither replaces nor restores the first claim. |
| H7a | Only `Vu20`, during S(40)'s eligible-batch validation. The proposed fresh bound 12 conflicts with earlier-attempt D14 despite the unavailable TEXT dependency. `admitted=true`; no DATA, TEXT, frontier or committed bound. |
| H7b | Only `Vu20`, during S(40)'s eligible-batch validation. Encountering the unavailable A10 dependency earlier in output order does not stop validation of D12 and D14. `admitted=true`; no DATA, TEXT, frontier or committed bound. |
| H8a | `F18 → Vu20`. Before TRY: `admitted=true, b_D=6, G(6), P=0+0, census=known, latch=0`, one END 6. Before entry to the VOID callback: b removed, `closed=false, P=1+0, census=known, latch=1`, O! persisted and dependent cached answers invalidated. Both the callback's reader and the independently reopened prefix report INCOMPLETE. Final default state with `admitted=true`. |
| H8b | `F18 → Vu20`. Before PTR: `admitted=true, b_D=6, G(6), P=0+0, census=known, latch=0`, one END 6. Before entry to the VOID callback: b removed, `closed=false, P=1+0, census=unknown, latch=1`, O! persisted and dependent cached answers invalidated. Both the callback's reader and the independently reopened prefix report INCOMPLETE. Final `b=—, x=—, G(open), P=1+0, census=unknown, latch=1, admitted=true`. |
| H9a | `F7 → Vu20`. Retain `admitted=true, b_D=6, x=—, G(6), P=0+0, census=known, latch=1`; exactly one END 6 and no O!. The historical late TRY faults but the retained qualified non-NULL fire prevents address-proof revocation. |
| H9b | `F7 → Vu20`. Retain `admitted=true, b_D=6, x=—, G(6), P=0+0, census=known, latch=1`; exactly one END 6 and no O!. The historical late wait-attempt report faults but the retained qualified non-NULL fire prevents address-proof revocation. |
| H10a | `EXIT_D18 → F18 → Vu20`. At S(19), `x_D=18, G(18), P=0+0`. Reject the cookie-zero unresolved control, retain the historical MATCHED disposition, remove x and persist O! before Vu20. Final default state; historical END 18 remains; uncovered increment 0. |
| H10b | No output before J; then `EXIT_U4 → A6[T] → F7 → Vu20`. J establishes U, resolves the number and commits `x_U=4, G(5), P=0+0, census=known`. Reject TERMU, retain the historical MATCHED disposition, revoke x and persist O! before Vu20. Final `b_U=—, x_U=—, G(open), P=1+0, census=known, latch=1`; historical END 5 remains; uncovered increment 0. |

| ID | Expected output and state |
|---|---|
| H11a | Merge stream: `Vu20`; combined timeline: `OPEN → Vu20`. AD_D19 records `admitted=true` while the epoch remains active and latch=0. EXIT_D18 arrival establishes the enable/EXIT contradiction, refutes the recorded admission proof, sets latch=1 and suspends the epoch; EXIT is never published. At that VOID entry, `admitted=true, b=—, x=—, G(open), P=1+0, census=known, latch=1`; public views and the persisted reader agree that startup coverage is unavailable, including `[50,60)`. No terminal disposition has yet been recorded, so the named EXIT's disposition accessor returns NONE. TERM_D18 is then recorded as the final MATCHED disposition with its terminal claim refuted; the accessor returns MATCHED, and TERM adds no fault, discharge or publication. Final epoch suspended and the same discovery state, with both recorded proof claims refuted. Exactly one unknown_evidence diagnostic increment, C0 unchanged, `abandoned=0`, uncovered increment 0; no DATA, FRONTIER, END, O!, epoch invalidation or nonzero exit. |
| H11b | Merge stream: `EXIT_D18 → F18 → Vu20`; combined timeline: `OPEN → EXIT_D18 → F18 → END18 → O! → Vu20`. TERM and EXIT receipt commit nothing; the first three boundaries of S(19) commit nothing, and its L boundary publishes EXIT, F18, completes the remaining counter checkpoints, then publishes END18, committing `x_D=18, G(18), P=0+0, census=known, latch=0`. AD_D19 is rejected without recording admission; it refutes the recorded matched terminal claim, revokes x and dependent coverage, invalidates affected cached answers and persists one O! before its VOID callback. At VOID entry and finally: epoch suspended, `admitted=false, b=—, x=—, G(open), P=1+0, census=known, latch=1`; public views and the persisted reader agree that startup coverage is unavailable, including `[50,60)`. D20 arrives during suspension, retains attributable evidence and has its payload disposed of without consumption; the terminal claim is already refuted, so D20 adds no fault or publication. Exactly one unknown_evidence diagnostic increment, C0 unchanged, `abandoned=0`, uncovered increment 0; one historical END18 and one O!, no epoch invalidation or nonzero exit. |
| H11c | Merge stream: `Vk → F24[Cr] → Vu22[Cr]`; combined timeline: `OPEN → Vk → F24[Cr] → Vu22[Cr]`. K suspends the epoch and sets latch=1; at its VOID entry `admitted=false, b=—, x=—, G(open), P=1+0, census=known`. RA restores an active epoch. The first three boundaries of S(25) publish nothing; its L boundary publishes F24 under Cr. AD_D6 records `admitted=true` without a fault but supplies no current-epoch D attachment proof. D26 is staged; its arrival and the first three boundaries of S(27) do not make it eligible. At S(27)'s L boundary, D26's first eligible-batch validation finds the missing continuity proof and produces Vu22[Cr], disposing of D26 without publication or consumption. At this VOID entry and finally: epoch suspended, `admitted=true, b=—, x=—, G(open), P=1+0, census=known, latch=1`. At both VOID entries, public views and the persisted reader agree that startup coverage is unavailable, including `[50,60)`. Diagnostics: counter_change +1, unknown_evidence +1; the vector remains Cr after K, with no synthetic storage-counter increment. `abandoned=0`, uncovered increment 0; no D DATA, F26, END, O!, epoch invalidation or nonzero exit. |

| ID | Expected output and state |
|---|---|
| H11d | Merge stream: `Vu20`; combined timeline: `OPEN → Vu20`. AD_D19 records `admitted=true`, with epoch active and latch=0. TERM_D18 establishes the enable/termination contradiction before EXIT DATA arrives, records the final MATCHED disposition and refutes both the recorded admission proof and terminal claim. At its VOID entry: epoch suspended, `admitted=true, b=—, x=—, G(open), P=1+0, census=known, latch=1`; the named EXIT's disposition accessor returns MATCHED. Public views and the persisted reader agree that startup coverage is unavailable, including `[50,60)`. Later EXIT_D18 arrival retains attributable evidence and disposes of its payload during suspension without publication or consumption; it adds no fault. Final state is unchanged from VOID entry. Exactly one unknown_evidence diagnostic increment, at TERM; C0 unchanged, `abandoned=0`, uncovered increment 0; no DATA, FRONTIER, END, additional OPEN, O!, epoch invalidation or nonzero exit. |
| H11e | Merge stream: `Vu20 → Vu20`; combined timeline: `OPEN → Vu20 → Vu20`. AD_D19 records `admitted=true`. EXIT_D18 establishes the enable/EXIT contradiction and produces the first fault, refuting the admission proof and suspending the epoch with latch=1; its payload is not published or consumed. TERM_D17 produces the second fault because its claimed timestamp disagrees with the retained EXIT timestamp; reject it without recording a final terminal claim or disposition, while retaining the required refutation history. At both VOID entries: epoch suspended, `admitted=true, b=—, x=—, G(open), P=1+0, census=known, latch=1`; the EXIT-disposition accessor returns NONE. Public views and the persisted reader agree that startup coverage is unavailable, including `[50,60)`. The later otherwise qualified TERM_D18 is recorded as final MATCHED with its terminal claim refuted; it is not rejected for disagreeing with the rejected TERM_D17 and produces no additional fault, discharge or publication. Finally the accessor returns MATCHED for timestamp 18, both recorded proof claims are refuted, and the other state is unchanged. Exactly two unknown_evidence diagnostic increments, C0 unchanged, `abandoned=0`, uncovered increment 0; no DATA, FRONTIER, END, additional OPEN, O!, epoch invalidation or nonzero exit. |
| H11f | Merge stream: `Vu20`; combined timeline: `OPEN → Vu20`. AD_D19 records `admitted=true`. The cookie-less EXIT arrives without independently established lifetime binding, remains undecided and supplies no fault or publication; its disposition accessor returns NONE. TERM_D18 supplies that binding, establishes the enable/termination contradiction, records final MATCHED and refutes both the admission proof and terminal claim before publishing the one VOID. At VOID entry: epoch suspended, `admitted=true, b=—, x=—, G(open), P=1+0, census=known, latch=1`, disposition MATCHED; public views and the persisted reader agree that startup coverage is unavailable, including `[50,60)`. Fault handling disposes of the EXIT payload without publication or consumption. Capture finish at 40 adds no fault or publication because the EXIT has its final disposition; it does not clear suspension, restore either proof or commit discharge. Final state equals the VOID-entry state. Exactly one unknown_evidence diagnostic increment, at TERM; C0 unchanged, `abandoned=0`, uncovered increment 0; no VOID40, DATA, FRONTIER, END, additional OPEN, O!, epoch invalidation or nonzero exit. |

Existing rows whose ordering expectations are strengthened:

- **N5 and P2:** the pointer/continuity revocation's O! and census change precede VOID publication.
- **R3:** every DATA, pointer and continuity variant, for every h and q, exposes its completed revocation before the contradiction's VOID. Preserve each variant's existing census result.
- **R4:** every h/q variant persists its bound-revocation O! before the contradiction's VOID.
- **R5:** every h/p/q variant commits x_D=18 at S(19), before any optional K, and persists its discharge-revocation O! before D20's contradiction VOID. With h=1 the startup gap never closed, but D's discharge still committed and its revocation still requires O!.
- **R12:** supporting-epoch invalidation and discharge revocation are reflected, with O! persisted, before its seal-violation VOID.
- **G2b:** numerical-bound revocation precedes the late-TRY VOID.
- **G3, revocation variant only:** bound revocation precedes the appended VOID; retained terminal evidence still supplies no replacement discharge.
- **G4, both variants:** discharge revocation precedes the conflicting-control VOID.
- **G4b:** discharge revocation precedes the unresolved-control VOID.

Their merge-only output streams, exact fault counts, historical END values, final states and consumption counts are unchanged. No existing row receives an optional fault count or a changed final answer. Existing two-fault observations preserve their arrival-fault-first diagnostic order.

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
| Reject every numeric-only unresolved disposition or retain its hold after acceptance | U1, U2, U10 |
| Publish the unresolved EXIT, invent its cookie/inventory entry, or discharge its number | U1, U2, U10 |
| Accept an unnamed disposition or one without either matching inventory or numeric obligation | U4, U5 |
| Accept a later matched disposition for the same unresolved EXIT | U3 |
| Erase final unresolved history or permit END after G/R resolves the number | U6, U7 |
| Finalise on transient failure, deadline expiry or certification latch alone | U8, U9 |
| Finalise from a cut before the EXIT or before consuming a retained matching witness | U14, U13, respectively |
| Require DATA before accepting its named disposition | U10 |
| Reapply, recount or reject an identical duplicate, including after numeric resolution | U11 |
| Reject unresolved disposal after the latch, clear the latch, or implicitly REARM | U1, U15 |
| Require a numeric observation for an existing cookie-zero inventory entry, or create another entry during its disposal | U12 |
| Let unresolved disposal bypass ordinary completeness or suspension | U10 before S(8), U15 |
| Emit unknown_evidence before the arrival fault for one observation raising both | O1 |
| Short-circuit after either fault of one observation raising both (one VOID) | O1 |
| Drop the rejected record's evidence (no unknown_evidence) | O1 |
| Report seal_violation instead of clock_regression for the overlapping arrival fault | O1 |
| Hold an early valid CN instead of faulting and rejecting it | G1 |
| Accept late TRY as harmless or idempotent | G2 |
| Enforce late-TRY ordering only for active admission, ignoring ADMIT recorded under the latch | G2 prefixed variant |
| Fault late TRY after numerical ADMIT but retain its contradicted committed bound | G2b |
| Revoke a route-(c) proof solely because its historical TRY arrives late | G2: require no O! |
| Commit or report discharge alongside a bound in the same attempt | G3 |
| Commit discharge after an already committed bound | N6, P3 |
| Promote retained terminal evidence after revoking the bound | G3 revocation variant |
| Preserve discharge after a conflicting terminal timestamp or EXIT identity | G4 |
| Preserve discharge after unresolved contradicts its committed matched disposition | G4b |
| Reject valid discovery ADMIT solely because EXIT already released | G5, G7 |
| Ignore retained NULL qualification when validating numerical ADMIT | G6 |
| Replace or supplement committed discharge with a later bound | G7 |
| Use a held release's prospective bound to close on a later terminal control | G8 |
| Let a diagnostic cutoff bypass closure's completed-release requirements | G8 |
| Prevent a valid late terminal discharge merely because another batch is held | G8 |
| Fault late ADMIT but retain its contradicted committed discharge | H1a |
| Record an ADMIT already contradicted by retained EXIT evidence | H1a |
| Check enable-versus-EXIT provenance only when ADMIT arrives last | H1b |
| Erase historical admission when later EXIT refutes its proof | H1b |
| Treat enable/EXIT timestamp equality as contradictory | H1c, H1d |
| Expose certification eligibility as `admitted`, or leave the missing-ADMIT hold after valid post-latch admission | H2a, H2b |
| Record valid admission only in an active epoch | H2b |
| Let recorded admission implicitly rearm or release during suspension | H2c |
| Accept abandonment after numerical or route-(c) admission recorded under the latch | H2d, H2e |
| Commit a new bound or mark D26 certified after post-latch admission | H2a, H2b |
| Record route-(c) admission before checking retained numerical evidence | H3a, H3b, H3c |
| Treat rejected admission as barring otherwise valid abandonment | H3d |
| Count fault-dropped D12 as an abandonment discard | H3d |
| Retain the rejected late TRY as NULL-pointer history and fault the subsequent otherwise valid B | H4a |
| Suppress a separately delivered repeat late-attempt report after the first fault | H4b |
| Drop quiet evidence on VOID or REARM | H5a |
| Refuse or forget valid quiet evidence recorded under the latch | H5b |
| Re-fault a rearmed epoch solely because EXIT contradicts an already refuted terminal claim | H6a |
| Record a terminal claim already contradicted on delivery as an accepted proof | H6b |
| Suppress a separately delivered conflicting terminal control because an earlier claim was refuted | H6c |
| Return held before validating the batch's prospective-bound contradiction | H7a, H7b |
| Stop validation upon encountering the first missing dependency in output order | H7b |
| Publish a prospective bound, DATA or frontier from the contradictory held batch | H7a, H7b |
| Persist revoked OPEN only after the VOID callback returns | H8a, H8b |
| Update status only after VOID, or leave pointer-driven census invalidation until input return | H8a, H8b |
| Repair only live views while the persisted prefix retains the unretracted END | H8a, H8b |
| Revoke a numerical bound on late TRY despite a retained qualified non-NULL fire | H9a |
| Revoke a numerical bound on late wait-attempt report despite a retained qualified non-NULL fire | H9b |
| Require the conflicting unresolved control's cookie to equal the committed discharge's cookie before revoking | H10a, H10b |
| Replace MATCHED with the rejected unresolved disposition or count another uncovered consumption | H10a, H10b |
| Retain fault-dropped DATA for release or a missing-ADMIT hold after REARM | H3d, H6a, H6b |
| Require D's continuing-attachment evidence inside REARM despite a later independently valid startup ADMIT supplying it | H2a |
| Publish startup END before supporting TEXT/DATA, before the same attempt's advancing FRONTIER, or before its remaining counter checkpoints | H1a, H10b; also the post-FRONTIER counter-failure execution in TASK-delta8.md |
| Expose an active epoch at entry to a VOID publication callback | H8a, H8b, H10a, H10b |
| Manufacture a continuity-refutation fault from H3c's rejected route-(c) admission | H3c |
| Publish revoked OPEN or reset census solely for an uncommitted route-(c) claim's refutation | R10, both variants |
| Defer a retained quiet candidate's contradiction until eligible-batch validation | H5a, H5b |
| Propose a fresh-record bound and fault before complete eligible-batch validation | H7a, H7b |
| Treat an observational TRY after rejected admission as another false permanence claim | H3a |
| Omit discharge revocation because another lifetime kept the startup gap open | R5, every h=1 variant |
| Publish END in an attempt that then faults, even if a later plain OPEN cancels it before VOID | V4 |
| Report another unknown_evidence fault when the matching TERM arrives after the enable/EXIT contradiction was already reported | H11a |
| Revoke the discharge but leave its recorded terminal claim unrefuted, so later attributable D20 faults again | H11b |
| Release D26 despite its missing current-epoch continuity proof | H11c |
| Refute only the terminal claim at TERM_D18 and fault again when the matching EXIT arrives | H11d |
| Defer the enable/termination contradiction until EXIT DATA arrives | H11d |
| Suppress the separately false TERM_D17 because the enable/EXIT contradiction was already reported | H11e |
| Retain rejected TERM_D17 as the final claim and fault the later otherwise qualified TERM_D18 for conflicting with it | H11e |
| Return NONE after recording a matched terminal claim as refuted by the enable/EXIT contradiction | H11a, H11d, H11e, H11f |
| Treat the refuted MATCHED disposition as missing at cookie-less capture finish | H11f |
| Count the cookie-less EXIT as uncovered or publish it while suspended merely because its MATCHED disposition was recorded | H11f |
| Suppress F7 because N14's later END publication fails | N14 |

Pointer-ABA, stale-marker refresh, all-emission-site cookie consistency, writer synchronization and actual EL8 verifier acceptance remain producer qualification obligations. Their qualification fixtures must exercise the corresponding physical operation; merge oracle agreement alone cannot mark them qualified.

Remaining owner decision: none