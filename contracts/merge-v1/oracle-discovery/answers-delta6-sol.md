U1: `VOID20[C0,unknown_evidence] → A26[T] → F27 → F29`. `S(28)` holds EXIT24 and A26; `TERMU_U24` then permits uncovered consumption of EXIT24. EXIT never published; uncovered=1. `P=L+Q=0+1=1`; census=unknown; startup gap=open permanently; certification latch=set; discharge=none.
why: Decisions 40–41 permit final unresolved disposition after permanent reconciliation loss, preserving the numeric obligation and prohibiting closure. Decision 43 permits this disposition after the latch and ordinary release after REARM.

U2: `A6[T] → F7`, released upon `TERMU_U4`; preceding inputs hold EXIT4 and A6. EXIT4 consumed uncovered, never published; uncovered=1. `P=L+Q=0+1=1`; census=unknown; startup gap=open permanently; certification latch=not set; discharge=none.
why: Decision 40 accepts the explicitly named numeric-only EXIT after completed cut 5≥4. Decisions 31 and 41 preserve the numeric observation and capture-long closure prohibition.

U3: `A6[T] → F7 → VOID20[C0,unknown_evidence]`. EXIT4 remains consumed uncovered; uncovered=1. J establishes U’s inventory identity and resolves the numeric observation, but its conflicting matched disposition faults. `P=L+Q=1+0=1`; census=known; startup gap=open permanently; certification latch=set; discharge=none.
why: Decisions 31 and 40 retain independently established J identity, numeric resolution and census facts; matching an already unresolved EXIT faults and supplies no discharge.

U4: `VOID20[C0,unknown_evidence]`. No EXIT DATA delivered or consumed; uncovered=0. `P=L+Q=0+1=1`; census=unknown; startup gap=open; certification latch=set; discharge=none.
why: Decision 40 requires the unresolved control to explicitly identify the particular EXIT’s capture, source, position, PID and timestamp; `names_exit=false` fails that requirement.

U5: `VOID20[C0,unknown_evidence]`. No EXIT DATA published or consumed; uncovered=0. PID 60 gains no inventory entry. `P=L+Q=0+1=1`; census=unknown; startup gap=open; certification latch=set; discharge=none.
why: Decision 40 permits numeric-only unresolved disposition only when the named PID has an unresolved numeric observation. PID 60 has neither that observation nor an inventory entry.

U6: `A6[T] → F7 → F9`. EXIT4 consumed uncovered upon `TERMU_U4`, never published; uncovered=1. G removes the numeric observation. `P=L+Q=0+0=0`; census=known; startup gap=open permanently; certification latch=not set; discharge=none.
why: Decision 31(G) resolves the number without supplying an inventoried lifetime or `x_i`. Decisions 31 and 41 preserve the closure prohibition despite resolution of every pending obligation.

U7: `A6[T] → F7 → F9`. EXIT4 consumed uncovered, never published; uncovered=1. R resolves PID 50 as a non-member through fence 9. `P=L+Q=0+0=0`; census=known; startup gap=open permanently; certification latch=not set; discharge=none.
why: Decision 31(R) permits the independently proved non-member resolution. The final unresolved EXIT disposition continues to prohibit closure under Decisions 31 and 41.

U8: `hold`; no DATA, FRONTIER, VOID or FAULT. EXIT4 and A6 remain held; uncovered=0. `P=L+Q=0+1=1`; census=unknown; startup gap=open; certification latch=not set; discharge=none.
why: Decision 40 excludes transient failure, elapsed time and deadline exhaustion as finalisation grounds. An attempted coordinator evaluation submits no disposition; no capture-finish input occurs.

U9: `VOID20[C0,allocation_fail]`; no subsequent DATA or FRONTIER. After REARM, EXIT24 and A26 remain held through `S(30)`; uncovered=0. `P=L+Q=0+1=1`; census=unknown; startup gap=open; certification latch=set; discharge=none.
why: Decision 40 says the certification latch alone cannot authorise finalisation while reconciliation remains pending. Decision 43 preserves the latch across REARM.

U10: `A6[T] → F7`. The unresolved disposition is accepted before DATA; EXIT4 is subsequently consumed uncovered at `S(8)`, never published; uncovered=1. `P=L+Q=0+1=1`; census=unknown; startup gap=open permanently; certification latch=not set; discharge=none.
why: Decision 40 permits disposition before its precisely named EXIT arrives. Strict release eligibility still applies; Decisions 31 and 41 preserve the numeric obligation and closure prohibition.

U11: `A6[T] → F7 → F9`. EXIT4 consumed uncovered exactly once; uncovered=1. Both identical unresolved retransmissions are idempotent, including the one after G resolves the number. `P=L+Q=0+0=0`; census=known; startup gap=open permanently; certification latch=not set; discharge=none.
why: Decision 40 preserves duplicate idempotence after original numeric eligibility disappears. Decision 31(G) resolves the number without discharge; Decision 41 retains the closure prohibition.

U12: `D12[U] → D15[T] → F15 → F30 → A44[T] → F44 → F45`. Startup gap initially closes with persisted `P_close=13`, then reopens from 0 at `LATE_U(40)`, revoking dependent base coverage and cached answers. EXIT42 and A44 wait until `TERMU_U42`; EXIT42 is consumed uncovered, never published. Uncovered count: 1 at LATE, then 2 at EXIT42 consumption. Final `P=L+Q=1+0=1`; census=unknown; startup gap=open permanently; certification latch=not set; discharge=none; D retains `b_D=12`.
why: Decisions 37, 41–42 give the original endpoint 13. Decision 43’s late-unlisted clarification reopens coverage without merge VOID. Decision 40 permits unresolved disposition for the existing cookie-zero inventory entry without creating another entry or requiring a numeric observation.

U13: `EXIT_U4 → A6[T] → F7`, released upon J resolution; EXIT published normally; uncovered=0. Before J, EXIT4 and A6 remain held. J resolves the number, establishes U and supplies committed `x_U=4` after the successful release attempt. Final `P=L+Q=0+0=0`; census=known; startup gap=closed with persisted `P_close=5`; certification latch=not set.
why: Decisions 31 and 40 require consuming the eligible retained J witness before unresolved finalisation and give J precedence. Decisions 41–42 yield `C_close=max(5,4)=5`, with no endpoint increment.

U14: `hold`; no DATA, FRONTIER, VOID or FAULT. EXIT4 and A6 remain held through `S(10)`; uncovered=0. `P=L+Q=0+1=1`; census=unknown; startup gap=open; certification latch=not set; discharge=none.
why: Decision 40 requires a completed reconciliation cut at or after EXIT4. Cut 3 does not qualify, and attempted finalisation supplies no terminal disposition.

U15: `VOID20[C0,unknown_evidence]`; no subsequent DATA or FRONTIER. EXIT24 and A26 are suppressed during suspension; `TERMU_U24` is accepted and recorded, but no validated uncovered release consumption occurs. EXIT never published; uncovered=0. `P=L+Q=0+1=1`; census=unknown; startup gap=open permanently; certification latch=set; discharge=none.
why: Decision 40 allows unresolved disposition after permanent reconciliation loss and after the latch, while retaining suspension and release rules. Decision 43 says disposition acceptance neither rearms the epoch nor restores certification.

## Re-check after the settlement (rows U4 U5 U9 U15 O1, answered from the settled text alone)

U4: `VOID20[C0,unknown_evidence]`. No DATA or FRONTIER; no EXIT delivered; uncovered=0. `P=L+Q=0+1=1`; census=unknown; startup gap=open, no PERSISTED end; certification latch=unset after CUT, set before VOID; no discharge or accepted disposition.
why: DECISION 40 explicitly rejects a TERMINAL_UNRESOLVED naming no EXIT, even without a false assertion. CUT supplies neither a boundary nor numeric resolution. DECISIONs 41, 43 preserve the pending number and permanently set the latch.

U5: `VOID20[C0,unknown_evidence]`. No DATA or FRONTIER; no EXIT DATA delivered; uncovered=0. `P=L+Q=0+1=1` (PID 50); census=unknown; startup gap=open, no PERSISTED end; certification latch=unset after CUT, set before VOID; no discharge or accepted disposition.
why: DECISION 40 rejects the first numeric-only unresolved disposition because PID 60 has no unresolved numeric observation. It creates no inventory entry or numeric obligation. DECISIONs 31, 41, 43 leave PID 50 pending.

U9: `VOID20[C0,allocation_fail]`; RA succeeds. No DATA or FRONTIER thereafter: `X_24` remains held for its terminal disposition, and `A26[T]` remains unpublished in the held batch. Uncovered=0; `P=L+Q=0+1=1`; census=unknown; startup gap=open, no PERSISTED end; certification latch=set throughout RA and subsequent inputs; no discharge.
why: DECISION 40 forbids finalisation based solely on the latch or pending reconciliation. The AOP requests evaluation and submits no disposition, so it causes no additional fault. DECISION 43 preserves the latch across REARM; boundaries cannot replace the missing disposition.

U15: `VOID20[C0,unknown_evidence]` from PF. No DATA or FRONTIER thereafter. `TERMU_U24` is accepted; its undecided-terminal hold is removed, but suspension prevents EXIT publication or validated uncovered consumption. Uncovered=0; `A26` is unpublished; `P=L+Q=0+1=1`; census=unknown; startup gap=open, no PERSISTED end; certification latch=set; no discharge.
why: DECISION 40 permits unresolved disposition after established permanent reconciliation loss and after the latch, but counts an uncovered EXIT only upon validated consumption in an active epoch. No REARM occurs. DECISIONs 31, 41 preserve the numeric obligation and prohibit startup closure.

O1: `VOID20[C0,clock_regression] → VOID20[C0,unknown_evidence]`. No DATA or FRONTIER; D20’s tentative payload is discarded, and D14 is rejected without staging or consumption. No EXIT; uncovered=0; abandoned=0. `P=L+Q=1+0=1`; census=known; startup gap=open, no PERSISTED end; no discharge, committed bound, revoked OPEN or epoch invalidation. Latch=unset through B_D6 and D20, set before the first VOID and still set after each VOID. Diagnostic increments: clock_regression=1, unknown_evidence=1, seal_violation=0; storage vector remains C0.
why: §3 selects clock_regression because D14 follows consumed perf DATA D20. DECISION 42 makes the attributable earlier-attempt interval ending at 14 contradict proposed bound 6.
why: DECISION 43 requires exactly these two faults, in this order, retaining contradictory evidence before rejection. Refuting an uncommitted candidate neither publishes revoked OPEN nor resets census completeness.

## Second packet (rows G1 G2 G2b G3 G4 G4b G5 G6 G7 G8 N6 P3)

G1: `VOID20[C0,unknown_evidence]`; no DATA or FRONTIER. `CN6` faults and is not retained because no matching ADMIT was previously recorded. `TRY_NW_D6` is recorded; subsequent `ADc6` is valid and recorded under the latch. `b_D=none; x_D=none; P=1=1+0; census=known; latch=set; gap=OPEN(0); uncovered=0; abandoned=0`. No revocation or epoch invalidation.
why: DECISION 37(c) requires ADMIT before CN; DECISION 43 permanently prevents subsequent discovery commitments and suppresses trusted release while suspended.

G2: Base: `VOID20[C0,unknown_evidence]`. Prefixed variant: `VOID20[C0,allocation_fail] → VOID20[C0,unknown_evidence]`. In both executions, `ADc6` is recorded; the later `TRY_NW_D6` faults and is not recorded; `CN6` is valid and recorded but commits nothing. No DATA or FRONTIER. `b_D=none; x_D=none; P=1=1+0; census=known; latch=set; gap=OPEN(0); uncovered=0; abandoned=0`. No revocation or epoch invalidation.
why: DECISION 38 rejects every attempt report delivered after recorded ADMIT, including ADMIT recorded under the latch; after route-(c) ADMIT this revokes nothing. DECISIONS 37(c), 43 govern CN and the latch.

G2b: `F18 → VOID20[C0,unknown_evidence]`. ADMIT and B are recorded; `S(19)` commits `b_D=6`, persists `GAP_CLOSE(6)`, and changes pending to `P=0=0+0`. The subsequent TRY faults, is not recorded, and revokes the numerical bound and dependent base coverage; persist revoked `GAP_OPEN(0)` and invalidate affected cached answers. Final: `b_D=none; x_D=none; P=1=1+0; census=known; latch=set; gap=OPEN(0); uncovered=0; abandoned=0`. No merge-epoch invalidation.
why: DECISIONS 37(b), 41–42 commit the quiet bound and endpoint 6. DECISIONS 38, 43 make the post-ADMIT TRY, without a qualified non-NULL fire, refute the numerical admission; this refutation does not itself invalidate census completeness.

G3: Base: `EXIT_D18 → F18`; ADMIT, B and TERM are recorded. The same successful release commits `b_D=6`, with `x_D=none`, and persists `GAP_CLOSE(6)`. Final: `P=0=0+0; census=known; latch=not set; uncovered=0; abandoned=0`; no faults or revocation.
Revocation variant: append `VOID20[C0,unknown_evidence]` for TRY, which is not recorded. Revoke `b_D`, dependent coverage and affected caches; persist revoked `GAP_OPEN(0)`. Final: `b_D=none; x_D=none; P=1=1+0; census=known; latch=set; uncovered=0; abandoned=0`. The historical EXIT and F18 remain observable; no merge-epoch invalidation.
why: DECISION 41 gives the bound priority when bound and EXIT become committable together and forbids promoting retained terminal evidence after bound revocation. DECISIONS 38, 43 govern the TRY fault and revocation.

G4: Both variants: `EXIT_D18 → F18 → VOID20[C0,unknown_evidence]`. The first TERM is recorded; successful EXIT release commits `x_D=18` and persists `GAP_CLOSE(18)`, giving `P=0=0+0`. The final TERM faults whether it claims time 19 for the same EXIT or time 18 for a different EXIT identity; it does not replace the retained disposition. Revoke the discharge and dependent coverage, invalidate affected caches, and persist revoked `GAP_OPEN(0)`. Final: `b_D=none; x_D=none; P=1=1+0; census=known; latch=set; uncovered=0; abandoned=0`. No merge-epoch invalidation.
why: DECISION 40 explicitly makes either changed terminal time or changed EXIT identity refute a committed discharge. DECISION 43 requires revocation while preserving independently established census facts.

G4b: `EXIT_D18 → F18 → VOID20[C0,unknown_evidence]`. The matched disposition is recorded and commits `x_D=18`, with persisted `GAP_CLOSE(18)` and `P=0=0+0`. The later unresolved disposition faults, does not replace the matched disposition, and consumes nothing. Revoke the discharge and dependent coverage, invalidate affected caches, and persist revoked `GAP_OPEN(0)`. Final: `b_D=none; x_D=none; P=1=1+0; census=known; latch=set; uncovered=0; abandoned=0`. No merge-epoch invalidation.
why: DECISION 40 explicitly makes an unresolved disposition conflicting with a committed matched discharge fault and revoke that discharge; DECISION 43 supplies the revocation behavior.

G5: `EXIT_D18 → F18`. After `S(19)`: `b_D=none; x_D=none; P=1=1+0; gap=OPEN(0)`. Later ADMIT is valid and recorded without a bound. TERM is valid and recorded; backed by the previously completed EXIT release, it commits `x_D=18` and persists `GAP_CLOSE(18)`. Final: `b_D=none; P=0=0+0; census=known; latch=not set; uncovered=0; abandoned=0`. No faults, additional DATA/FRONTIER, revocation or epoch invalidation.
why: DECISIONS 36, 40 allow late ADMIT and publication of the independently bound cookie-bearing EXIT before any disposition. DECISION 41 permits the subsequent discharge commitment using its completed release evidence.

G6: `VOID20[C0,unknown_evidence]`; no DATA or FRONTIER. TRY is recorded. Numerical `AD_D6` contradicts the retained NULL-pointer qualification history without a qualified non-NULL fire; it faults and is not accepted as an ADMIT. `b_D=none; x_D=none; P=1=1+0; census=known; latch=set; gap=OPEN(0); uncovered=0; abandoned=0`. No revoked OPEN or epoch invalidation.
why: DECISIONS 36, 38–39 require valid numerical address qualification and the qualified non-NULL transition from the pending NULL candidate. DECISION 43 gives the fault its latch consequence; no committed proof exists to revoke.

G7: `EXIT_D18 → F18`. TERM is recorded; successful release commits `x_D=18` and persists `GAP_CLOSE(18)`. Subsequent ADMIT and B are valid and recorded, but establish no lower bound. Final: `b_D=none; x_D=18; P=0=0+0; census=known; latch=not set; gap=CLOSED(18); uncovered=0; abandoned=0`. No faults, further DATA/FRONTIER, revocation or epoch invalidation.
why: DECISIONS 36, 41 permit late controls but retain the first committed resolving proof. A committed discharge prevents a later bound and endpoint replacement.

G8: `EXIT_D18 → F18`. EXIT publication initially leaves `P=2=2+0`. `AD_H8` is recorded. At the end of `S(22)`, `W=22`, but the complete eligible batch `{A20,H21}` and advancing `F21` are held for missing TEXT x; neither DATA record releases, and `H21` commits no fresh-record bound. Final TERM is valid and recorded; it commits `x_D=18` using D’s previously completed EXIT release. Final: `b_D=none; x_D=18; b_H=none; x_H=none; P=1=1+0; census=known; latch=not set; gap=OPEN(0); uncovered=0; abandoned=0`. No faults or revocation. The held batch and F21 remain held; a reader window after the last boundary is not complete.
why: Merge §3 holds the complete eligible batch and frontier for an unavailable dependency. DECISIONS 37(a), 40–41 permit D’s discharge but forbid closure using H’s uncommitted anchor or the diagnostic cutoff.

N6: `F5 → F7 → EXIT_D12 → F12`. NW’s TRY, route-(c) ADMIT and CN are recorded. At `S(6)`, `b_D=none; P=1=1+0`; at `S(8)`, commit `b_D=6` and persist `GAP_CLOSE(6)`, giving `P=0=0+0`. `PTR_D12(NULL)` produces only internal observation accounting. TERM is recorded; EXIT releases normally and supplies no discharge. Final: `b_D=6; x_D=none; census=known; latch=not set; gap=CLOSED(6); uncovered=0; abandoned=0`. Wait-word attempts=0; pointer qualification sequences=1. D is absent from backend listings and measured populations, contributes `ΔN_k=ΔU_k=0`, and supplies no wait, count, CPU or off-CPU measurement. No role-support publication, UNTRACED, faults or revocation.
why: DECISION 37(c) requires completeness strictly beyond 6 and permits later NULL fires. DECISIONS 41–43 retain endpoint 6 after EXIT and exclude the qualified logger from backend and numerical populations.

P3: `F5 → F7 → EXIT_D12 → F12`; persisted startup history is `GAP_OPEN(0) → GAP_CLOSE(6)`, with reserved field zero. Replay retains endpoint 6 and unavailable interval `[0,6)`; EXIT at 12 does not extend it. `b_D=6; x_D=none; P=0=0+0; census=known; latch=not set; uncovered=0; abandoned=0`. Controls are recorded as in N6; wait-word attempts=0 and pointer qualification sequences=1. D is neither a listed backend nor an A2 omission: `ΔN_k=ΔU_k=0`, no measured-population contribution, no omission note, and no numerical observations or zero-work assertion. No faults, revocation or epoch invalidation.
why: DECISIONS 41–43 require persisted endpoint 6, exclude the no-wait-word lifetime from backend and numerical populations, and require replay to preserve that coverage meaning. No per-lifetime role-support publisher is required.