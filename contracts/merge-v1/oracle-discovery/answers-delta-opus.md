# Answer set (opus), discovery-admission oracle rows, contract-only

Common: mode N, observations d=20,C0 unless stated. Each S(w) constituent is its own release evaluation; W stays 0 until the L constituent unless noted. "gap" = persisted startup gap per DECISIONs 41-42; "pending" = unresolved inventoried lifetimes; census completeness known (fence 5) unless stated.

D01: D12[U] -> D15[T] -> F15 (all at S(16)'s L constituent; no earlier frontier). b_D=12 (fresh-record route, D12 is first released TRANS of final attachment, t=12>=e=8). C_close=max(5,12)=12; W=16>12; P_close=13 (D12[U] at exactly C_close). Gap closed, persisted end 13, pending 0. X in [12,16): Unavailable (window starts before P_close). X in [13,16): available; count 1 (completion at 15), duration 2 ns (certified [12,15) clipped half-open to [13,15)). Completed-Y count in [12,13): Unavailable (unknown, not zero).
why: D36 (ADMIT gates release), D37(a), D41 C_close, D42 P_close increment + half-open clipping; §3 release.

D02c: D9[U] -> D10[T] -> F12 (at S(13) L). b_D=9 (fresh-record; D9 t=9>=e=8, first released record of final attachment); READ_D(12,X) establishes nothing. C_close=max(5,9)=9; P_close=10 (D9[U] at C_close). Gap closed, persisted end 10, pending 0. X in [10,13): window available; X completed count 0 and no X duration contribution (X opened at 10 is still open; not reported as zero duration; no open-wait snapshot supplied). (Y [9,10) duration 1 is certified on D10.)
why: D37(a), D41-42, clarification "open wait contributes no completed count and no duration".

D03: D15@P1, D12@P0, S(16) -> hold (W=16 but D records and their frontier wait for ADMIT; no F15). AD_D8 -> D12[U] -> D15[T] -> F15. b_D=12; C_close=12; P_close=13; gap closed, persisted end 13, pending 0.
why: D36 ("records and their release frontier wait for matching ADMIT"), D37(a), D41-42; §2 sort key orders D12 before D15 across sources.

D09: S(23) L constituent: D12[U] -> D15[T] -> D18(terminal TRANS)[T] -> EXIT_D18 -> FORK_R20{fork_seed_proved=true} -> R22[U] -> F22. No VOID/FAULT. D: b_D=12 (fresh-record); its exit after b_D is not a discharge and does not replace b_D. R resolves to its fork cookie g=1 (25b; D's EXIT at 18 is ordered before R's FORK at 20), R22 carries [U] as a 25b first record. AOP delayed_cleanup/retry_insert(D): refused by the tombstone, no effect on D or R, no output. C_close=max(5,12)=12; P_close=13; gap closed, persisted end 13, pending 0.
why: D34 (tombstone refuses; cleanup/retry do not remove), D35 (fork cookie precedence), D41 ("exit after b_i does not replace it"), §2 DECISION 25/25b, §3 25c marking.

D10: D01's outputs (D12[U] -> D15[T] -> F15, gap closed P_close=13), then S(31): P0/P1/E constituents give W=16 (non-advancing, no repeat), L gives F30. LATE_U(40): no merge output and no VOID; base coverage revoked from boot_start including cached answers: gap open again from boot_start, pending 1 (U), census completeness no longer established. D01's previously available [13,16) X answer is now Unavailable.
why: D43 late-unlisted revocation; clarification for row D10.

Q02: D15[U] -> F15 (at S(16) L). B_D12 fails (v0=7 != v1=8): no confirmation, no FAULT (candidate requests validation, does not claim coherence), no loss increment. Lower bound by fresh-record route: b_D=15 (D15, t=15>=e=13). Inspected CPU/query/command fields after rejection and before D15: they hold the producer-advanced values (not rolled back, not restored to the seed snapshot; no reseed since ADMIT already arrived). C_close=max(5,15)=15; P_close=16 (D15[U] at C_close); gap closed, persisted end 16, pending 0. X interval and CPU in [12,16): Unavailable (D15's incoming duration/CPU/count uncertified; window starts before P_close).
why: D35 (failed bracket does not roll back producer writes), D37(b) pass conditions, D37 "failed witness supplies no confirmation", D38, D37(a), D41-42.

Q03: D15[U] -> F15. S1=running is not a successful inactivity proof -> failed bracket, no FAULT, no loss increment; fresh-record b_D=15; C_close=15; P_close=16; gap closed, persisted end 16, pending 0. X and CPU in [12,16): Unavailable.
why: D37(b) (both inactivity proofs must succeed), D38, D41-42.

Q04: D15[U] -> F15. pcount equality + state letters S,S are not the qualified /proc/syscall wait_task_inactive proof -> failed bracket; b_D=15 (fresh-record); C_close=15; P_close=16; gap closed, persisted end 16, pending 0. X and CPU in [12,16): Unavailable.
why: D37(b) "A state letter, unlocked runqueue field or scheduler counter alone is insufficient".

Q04b: D15[U] -> F15. schedstat "0 0 0" at both endpoints is a scheduler counter, not an inactivity proof -> failed bracket; b_D=15; C_close=15; P_close=16; gap closed, persisted end 16, pending 0. X and CPU in [12,16): Unavailable.
why: D37(b), D38, D41-42.

Q05: D15[U] -> F15. S1 EACCES = denied proof = failed bracket; no FAULT and no counter increment (counters stay C0 per row); b_D=15 (fresh-record); C_close=15; P_close=16; gap closed, persisted end 16, pending 0. X and CPU in [12,16): Unavailable.
why: D37(b) "denied ... proof is a failed bracket"; D38 "does not by itself manufacture a loss increment".

Q06: D15[U] -> F15. Threads=2 during the bracket -> not single-threaded -> failed bracket; no FAULT (lifecycle accounting complete). Fresh-record route via the independently valid final attachment: b_D=15; C_close=15; P_close=16; gap closed, persisted end 16, pending 0. X and CPU in [12,16): Unavailable.
why: D37(b) single-thread condition, D39, D38 exhaustion/fresh-record, D41-42.

D14a: A12[T] -> D12[U] -> D15[T] -> F15 (at S(16) L; t=12 tie ordered by pid 10 < 30, different lifetimes, no tie fault). b_D=12; C_close=12; P_close=13; gap closed, persisted end 13, pending 0. Completed-Y count across A and D in [12,13): Unavailable (window contains instant 12 inside [boot_start,13); A's certified completion does not make it available; count unknown, not zero). X in [13,16): available; count 1, duration 2 ns ([12,15) clipped to [13,15)).
why: D42 ("no window containing it is available"), D41, D37(a), §2 sort key.

D14b: D12[U] -> D15[T] -> F15. b_D=12; C_close=max(15,12)=15; W=16>15; no uncertified discovery record at 15 (D15 is [T]) -> P_close=15. Gap closed, persisted end 15, pending 0. X in [15,16): available; count 1 (certified completion at 15 keeps ordinary window membership), duration 0 ns (interval [12,15) has no overlap with [15,16) under half-open clipping).
why: D41 C_close, D42 ("otherwise ... certified completions there retain their ordinary window membership"; half-open clipping).

D14c: TRYs/AD_D12{α3}/B_D11{α3} are controls; B_D11 passes (v0=v1=9, defaults). S(12) L constituent: D11[U] -> F11. D11 (origin α2, before α2's fence and α3 seed) is an earlier-attempt record: released uncertified, cannot rebase. Lower bound after this release: b_D=11 via quiet-baseline route (matching ADMIT, W=12>t=11, reconciliation succeeds); baseline = α3 seed (wait X at t=11, off-CPU, on_cpu_ts=0, CPU base 0 valid), unmodified by D11. C_close=max(5,11)=11; P_close=12 (earlier-attempt D11[U] at exactly C_close); gap closed, persisted end 12, pending 0. S(16) L constituent: D15[T] -> F15; after this release b_D=11 unchanged, baseline advanced by D15 to wait 0 at 15. X in [11,16): Unavailable. X in [12,16): available; count 1, duration 3 ns (certified X [11,15) duration 4, clipped to [12,15)).
why: D36 (earlier attempt / timestamp equality), D37(b) acceptance and "must not modify the confirmed baseline", D38, D42 increment for earlier-attempt record.

A01: D9, D12, S(13) -> hold (no ADMIT/ABANDON; frontier held). AB_D accepted -> D9, D12 consumed as discards (not published) -> F12. S(16) -> F15 (empty batch, advancing). No VOID. discovery_abandoned_records=2. Gap open, pending 1 (D), census completeness known.
why: D36, D45 (removes only missing-ADMIT dependency; counted, never published; no b_i, no discharge), §3 empty-batch frontier.

A02: A01's outputs (F12 -> F15), then D17 consumed at S(20) L as a discard -> F19. discovery_abandoned_records=3. Gap open, pending 1.
why: D45 ("staged and future TRANS/MARKER records").

A03: A01's outputs (F12 -> F15); S(18) -> F17; S(19) L: terminal D18 discarded, EXIT_D18 published -> EXIT_D18 -> F18. Terminal discharge x_D=18 accepted (TERM_D18; discards at 9,12,18 all <=18). C_close=max(5,18)=18; W=19>18; P_close=19 (Decision 45 discard at exactly C_close). Gap closed, persisted end 19, pending 0. discovery_abandoned_records=3.
why: D40, D45 (EXIT released normally, not discarded; discard at C_close triggers increment), D41-42.

A04: D12[U] -> F12 (S(13) L; b_D=12). AB_D for an admitted identity -> VOID20[C0,unknown_evidence]; epoch suspended. discovery_abandoned_records=0. Gap open, pending 1 (H unresolved; D resolved by b_D=12).
why: D45 ("An already admitted identity cannot be abandoned"; invalid abandonment reports FAULT(unknown_evidence)), D41.

A05: D9 staged (W=0). AB_D accepted (no consumption at W=0). AD_D8 after abandonment -> VOID20[C0,unknown_evidence]; suspended. S(16): no output. discovery_abandoned_records=0. Gap open, pending 1.
why: D45 ("Any later ADMIT ... for the abandoned identity is contradictory"); discards count only on completed release consumption, which never happened.

A06: AB_H for a non-inventoried identity -> VOID20[C0,unknown_evidence]. discovery_abandoned_records=0. Gap open, pending 1 (D, installed, never admitted, not abandoned).
why: D45 (first abandonment only for an inventoried, bound identity).

A07: AD_H13, B_H12 (passes), AB_D accepted; at S(16) L: D9 discarded, H15[T] -> F15. b_H=12 (quiet-baseline, committed with W=16>12). discovery_abandoned_records=1. Gap open, pending 1 (D).
why: D37(b), D45 (another lifetime unaffected), D41 (abandonment supplies no b_i/discharge).

A08: D9 staged, AB_D accepted, CHECK(20,Ce) -> VOID20[Ce,counter_change]; suspended; S(16) no output. discovery_abandoned_records=0. Gap open, pending 1.
why: §4 counter_change, D45 (acceptance cancels no fault).

A09: A03's outputs (F12 -> F15 -> F17 -> EXIT_D18 -> F18); retransmitted AB_D is idempotent: no output, no second disposition/discard/diagnostic increment, C0 unchanged so no fault. discovery_abandoned_records=3. Gap closed, persisted end 19, pending 0.
why: D45 idempotent duplicate "including after terminal discharge or gap closure".

A10: A01's outputs (F12 -> F15), then B_D12{e=13} (CONFIRM-type control) for the abandoned identity -> VOID20[C0,unknown_evidence]. discovery_abandoned_records=2. Gap open, pending 1.
why: D45 ("Any later ADMIT or CONFIRM for the abandoned identity is contradictory").

A11: A01's outputs (F12 -> F15), then D14@P0 below accepted P0 boundary 16 (no clock regression: 14>=12) -> VOID20[C0,seal_violation]; D14 rejected, not discarded/counted; epoch marked/reported invalid (14 <= published F15). discovery_abandoned_records=2. Gap open, pending 1.
why: §3 seal check order, §4 invalidation, D45 (discards cannot bypass seal checks or conceal disproof).

A12: A01's outputs (F12 -> F15), FAULT -> VOID20[C0,allocation_fail]; REARM(21) accepted (suspended epoch; cutoffs reset, retained boundaries 16); abandonment survives REARM; D17@P0 (17>=16, >=12) accepted; S(18) L: D17 discarded -> F17 (advances past retained F15). discovery_abandoned_records=3. Gap open, pending 1.
why: §4 DECISION 27/27a, D45 (permanent across REARM; trusted release resumes only after valid REARM), D32.

A13: hold; no output (E never bounded, W=0); D9 staged, not consumed. discovery_abandoned_records=0. Gap open, pending 1.
why: §3 unbounded source blocks release; D45 "Missing completion proof holds release".

A14: CHECK(20,unknown) -> VOID20[unknown,counter_unknown]; suspended; S(16) no output. discovery_abandoned_records=0. Gap open, pending 1.
why: §4 counter_unknown; D45.

A15: A01's outputs (F12 -> F15), S(18) -> F17; S(19) L: D18 discard completes (counted), EXIT_D18 rejected -> VOID20[Cw,writer_push_fail]; no F18; capture paused. Terminal discharge not accepted (publication failed), gap not closed. discovery_abandoned_records=3. Gap open, pending 1.
why: D45 ("Count each completed discard once even if a subsequent publication ... fails"; EXIT under successful-publication rules), D41 (publication failure commits no closure), §4 writer failure.

A16: D9{identity_known=false} staged; AB_D accepted; S(16) L constituent: batch validation cannot attribute D9 -> VOID20[C0,unknown_evidence]; no frontier. discovery_abandoned_records=0. Gap open, pending 1.
why: §2 generation resolution / unknown_evidence; D45 ("uncertain record attribution cannot be converted into a successful discard").

A17: A03's outputs (F12 -> F15 -> F17 -> EXIT_D18 -> F18; gap closed P_close=19), then S(23) L: FORK_R20{fork_seed_proved=true} -> R22[U] -> F22. R is not discarded (other lifetime, same PID; D's EXIT at 18 precedes R's FORK). discovery_abandoned_records=3. Gap closed, persisted end 19, pending 0.
why: D45 (does not affect another lifetime using the same PID), §2 25b, D35.

A18: FAULT -> VOID20[C0,allocation_fail]; AB_D accepted (gap open, not admitted) but cancels no fault; S(16) no output. discovery_abandoned_records=0. Gap open, pending 1.
why: D45 ("Acceptance neither cancels a fault nor rearms"), §4.

A19: At S(16) L: TEXT_Dx, D9 and M_D10 consumed as discards (x available, dependencies validated) -> F15. discovery_abandoned_records=3. Gap open, pending 1.
why: D45 (TEXT belonging exclusively to the abandoned stream is discarded and counted; TRANS/MARKER counted).

A20: S(19) L: EXIT_D18 -> F18 (Decision 40 discharge without ADMIT; no invented terminal TRANS). x_D=18; C_close=max(5,18)=18; P_close=18 (lifecycle EXIT alone does not trigger the increment); gap closed, persisted end 18, pending 0. Then AB_D after gap closure -> invalid first abandonment -> VOID20[C0,unknown_evidence]. discovery_abandoned_records=0. Gap remains closed at 18.
why: D40, D41-42, D45 (first abandonment only "while the startup gap is open").

## Rows resting on reading rather than an explicit rule
- D01/D14a/D14c: clipped durations (2, 2, 3 ns) read "ordinary half-open clipping" as clipping the certified interval to the window.
- D14b: duration 0 ns with count 1 at [15,16) combines half-open clipping with "completions retain ordinary window membership".
- D09: C_close=12, not 18: D's EXIT after b_D read as not a terminal discharge (D41 "does not replace that lower bound").
- Q02: the bracket failure is a silent rejection (no FAULT) because B only requests validation; fields keep producer-advanced values.
- A05/A08/A14/A18/A16: abandoned count 0 because discards count only on completed release consumption, never reached before suspension.
- A12: post-REARM F17 assumes the abandoned D needs no fresh attachment proof in the new epoch.
- A16: VOID timed at S(16)'s L constituent (first nonempty batch validation).
- A20: the VOID after closure does not reopen or alter the committed gap end 18.
