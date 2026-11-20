# Answer set (opus), discovery-admission oracle rows

Conventions: `gap open(n)` = startup gap open with n unresolved inventoried lifetimes, census completeness known (complete) unless stated; `gap closed@C (pending 0)`. Controls (AD/B/TRY/TERM/AOP/LATE/CHECK) produce no DATA output themselves. Release evaluation after every input; S(w) releases only after its L constituent (W=min).

D01: `D12[U] → D15[T] → F15` (all at S(16)'s L boundary; nothing before). b_D=12 by route (a) fresh-record (D12 at its ordered position). gap closed@12 (pending 0). X in [12,16): duration 3 ns, completed-wait count 1 (D15 completion at 15, certified). The completion at 12 (incoming Y interval of D12) is uncertified and equals C_close: not counted (0), duration unavailable.
why: D36 (records wait for/use ADMIT e=8), D37(a), D41 C_close=max(5,12)=12 with W=16>12, D42 endpoint exclusion; merge §3 release/W rule.

D02a: `F11` (S(12)) → `F12` (S(13); quiet bound committed) → `D15[T] → F15` (S(16)). b_D=12 by route (b) quiet-baseline; commitment at S(13) because S(12) gives only W=12, not completeness strictly beyond t=12. gap closed@12 at S(13) (pending 0; open(1) through S(12)). X in [12,16): duration 3 ns, count 1, from the seed baseline X@12.
why: D37(b) acceptance needs matching ADMIT + completeness strictly beyond t; subsequent intervals need no first-record exclusion (D37 last paras); D41.

D02b: `F15`. No b_D (AD_D8 selects only the fresh-record route; no qualifying TRANS/MARKER released; READ_D is not a record and not a quiet bracket). gap open(1).
why: D37(a)/(b) are the only routes; D38 "pending until a qualifying record actually releases"; D41.

D02c: `D9[U] → D10[T] → F12`. b_D=9 by route (a). gap closed@9 (pending 0). X in [10,13): no completed X wait — duration 0 ns completed, count 0; the X wait entered at 10 is still open and is not quantified (no open-wait snapshot supplied; READ_D(12,X) supplies no observation). D9's incoming X interval (from the stale seed) is uncertified; D10's Y interval (1 ns) is certified.
why: D37(a), D42; ROWS note "no unstated open-wait snapshot"; READ_D is ordinary read, not seed/record.

D02d: `VOID20[C0,unknown_evidence]` at B_D12 delivery; S(13) then produces no output (suspended). No b_D. gap open(1).
why: D37(b) step 6/CPU-base rules (CPU base must be the preserved accumulator at the bracket; sampled at 7 with intervening accumulation is incoherent) and D37 "a control that falsely claims completed coherence reports FAULT(unknown_evidence,d,C)".

D02e: `F15`. Both brackets failed (v0≠v1); retry limit 2 exhausted, no third bracket. AD_D14 admits the final attachment for the fresh-record route only; no qualifying record, so no b_D; gap open(1). READ_D(15,Y) is not a record; no loss/fault is manufactured.
why: D38 (finite N, exhaustion → ADMIT for fresh-record route, remains pending; failed bracket manufactures no loss), D37(a).

D03: hold through `S(16)` (D12, D15 eligible but dependent on ADMIT: records and their frontier wait). At AD_D8: `D12[U] → D15[T] → F15`. b_D=12 route (a). gap closed@12 (pending 0).
why: D36 "a valid control may arrive after dependent DATA; those records and their release frontier wait for it"; D37(a); D41.

D04a-install: `VOID20[C0,unknown_evidence]` at AD_D8; D12 and S(13) produce no output. No b_D. gap open(1).
why: D36 (proof includes installation visibility before enable; a claimed proof containing a false required fact → FAULT(unknown_evidence,d,C)).

D04a-state: `VOID20[C0,unknown_evidence]` at AD_D8; then nothing. No b_D. gap open(1).
why: D36 (preserved producer state required), D35 (provisional state must not overwrite producer updates).

D04a-binding: `VOID20[C0,unknown_evidence]` at AD_D8; then nothing. No b_D. gap open(1).
why: D36 (Decision 31–35 incl. D33 lifecycle binding; unknown facts are false).

D04a-address: `VOID20[C0,unknown_evidence]` at AD_D8; then nothing. No b_D. gap open(1).
why: D36 address qualification; D39 (unobserved relocation cannot qualify).

D04a-cookie: `VOID20[C0,unknown_evidence]` at AD_D8; then nothing. No b_D. gap open(1).
why: D36 includes D32 (allocators never cross partitions).

D04b: `F15`. D is not classified as pre-existing (upper bound equal to boot_start does not establish precedence: within-margin → pending); no ADMIT, no b_D, no discharge. gap open(1); census completeness known.
why: D33 ("establish that it precedes boot_start"; "within-margin start remains pending"); D31/D41 pending count.

D04c: `VOID20[C0,unknown_evidence]` at the AD. The FD is bound to O, not R; the numeric-PID /proc read does not preserve the binding; R is a post-start lifetime that retains (and needs) fork provenance and cannot use discovery. No b_i for R or D. gap open(1) (default candidate D unresolved).
why: D33 (pinned handle; reopening numeric PID does not preserve binding; start-time test not sufficient; fork stamped before boot keeps fork provenance), D30, D36 false/contradictory provenance → FAULT.

D04d: no output (no boundary input). Tombstone refuses the BPF_NOEXIST insertion; the reserved cookie stays burned (not recycled); no ADMIT possible. No terminal discharge (no terminal witness delivered; PID tombstone/map state is not identity evidence). gap open(1); no b_D, no x_D. Counters unchanged (C0), no VOID.
why: D32 (burn, no recycle), D34 (exit tombstone even without mutable state; tombstone refuses installation), D40 (discharge needs a resolved kernel terminal witness).

D04e: `VOID20[Cf,counter_change]`. Late and retried D insertions are refused by the tombstone (retry does not remove it); R's fork-cookie allocation failure is counted (fork_state_update_fail); R's emission has no valid fork cookie and the tombstone supplies none, so it is never attributed to D. No b_D, no discharge delivered. gap open(1).
why: D34 (tombstones survive; fork path tombstones before allocation), D35 (identity resolution; tombstone supplies no cookie), D43, merge §4 counter_change.

D04f: `VOID20[C0,allocation_fail]`. Reservations returned fork 2^63−1 and discovery 2^64−1; both further requests fail without wrap (fork never enters 2^63, discovery never wraps to 0/1). Only the one supplied fault input produces a VOID; no second VOID is emitted in this row. Capture paused.
why: D32 (partitions, exhaustion reports allocation failure without wrapping), merge §4 (allocation failure → VOID, paused capture; one VOID per delivered report).

D04g: `VOID20[Cs,counter_change]`. Inspected CPU/query/command fields retain the producer's advanced values (identity insertion overwrote nothing). The failed discovery-map insertion enters reliable accounting (Cs).
why: D35 (installation must not replace state_map or overwrite producer-owned fields), D34/D43 (map-operation failure keeps fault accounting), merge §4.

D07a: `F20` (at S(21)). b_D=12 and b_H=20, both route (b). C_close=max(5,12,20)=20; W=21>20 → gap closed@20 (pending 0).
why: D37(b) acceptance (W>t for each), D41.

D07b: at S(21): `F20`; b_D=12, b_H=20 committed (route (b)), unresolved count 0, gap still open because C_close=max(25,12,20)=25 needs completeness strictly beyond 25. At S(26): `F25`; gap closed@25 (pending 0).
why: D41 (C_close includes census_fence; completeness strictly beyond C_close).

D08a: S(18): `F17` (b_D=12 committed route (b); EXIT_H18 not eligible at W=18). S(19): `EXIT_H18 → F18`; H discharged x_H=18 on successful publication (no invented terminal TRANS). C_close=max(5,12,18)=18; W=19>18 → gap closed@18 (pending 0). Before S(19) gap open(1).
why: D40 (terminal discharge without state; publish lifecycle witness), D37(b), D41.

D08b: S(18): `F17` (b_D=12 route (b)). S(19): `F18`. H not discharged (PID-only ESRCH insufficient). gap open(1) (H unresolved); no VOID.
why: D40 ("ENOENT, ESRCH ... alone are insufficient"), D41.

D09: at S(23): `D12[U] → D15[T] → TRANS_D18[T] (terminal) → EXIT_D18 → FORK_R20 → R22[U] → F22`. b_D=12 route (a); D's exit at 18 does not replace it. R uses its fork cookie g=1 (25b; D's EXIT precedes R's FORK); R22 is R's first 25b record → [U]. Delayed cleanup and retry insertion do not remove D's tombstone and affect neither D nor R. gap closed@12 (pending 0; R is a post-start birth, not inventoried).
why: D37(a), D41 ("exit after b_i does not replace it"), D34/D35 (tombstone, fork cookie precedence), merge 25/25a/25b/25c and rank ordering.

D10: `D12[U] → D15[T] → F15` (as D01; gap closed@12), then S(31): `F30`. LATE_U(40): base coverage revoked from boot_start (the closure at 12 no longer certifies startup coverage), affected cached answers invalidated. No VOID (no FAULT/LOSS input), no REARM, no recovery-time admission of U.
why: D43 (late-unlisted startup revocation retained; no recovery-time admission/automatic REARM), merge §4 (VOID only on fault report).

D13a: `VOID20[Ce,counter_change]` at CHECK; D12 (staged at W=0) discarded; S(16) produces nothing. No b_D. gap open(1).
why: merge §4 counter_change; D41/D43 (no lower bound without successful publication; no forgiveness of loss).

D13b: Coverage status Unavailable, reason untraced_invalid (A2 declaration with fork_ns<boot_start; discovery creates no A2 omission and contributes nothing to N_k/U_k); no cohort assignment (no clamp to J_0, no negative cohort); omission note withheld (inventory invalid).
why: coverage-a2 "fork_ns < boot_start is invalid", reason precedence 1, note withholding; D43.

D13c: `VOID20[C0,unknown_evidence]` at the AD (post-start birth cannot use discovery to replace missing fork evidence: contradictory provenance); FORK_R10 (staged) discarded; S(16) produces nothing. No b_R. gap open(1) (default D unresolved).
why: D30, D36 (contradictory provenance → FAULT), merge §4.

D13d: at S(13): release attempt of D12 rejected by the writer → `VOID20[Cw,writer_push_fail]`; rejected D12 absent; no F12; capture paused. b_D not committed; gap open(1).
why: merge §4/§5 row 19 writer failure; D41 ("publication failure commits neither the affected lower bound nor gap closure").

D13e: `VOID20[C0,unknown_evidence]` at AD_D8 (qualified discovery admission is claimed in mode Q, where discovery remains unqualified: false required fact). No b_D. gap open(1).
why: D30 ("initially applies only to ... mode N"; "Q-mode discovery remains unqualified"), D36.

Q01: `D15[T] → F15` (at S(16)). b_D=12 route (b); gap closed@12 (pending 0). In [12,16): X duration 3 ns, count 1, CPU 1 ns (measured from valid base 0).
why: D37(b) (seed X@12, CPU base 0 valid, acceptance at W=16>12), D41.

Q02: `D15[U] → F15`. Bracket fails (v0=7≠v1=8): no confirmation, no fault. Inspected CPU/query/command fields show the producer's advances (no rollback). AD_D13 admits the fresh-record route: b_D=15 route (a); gap closed@15 (pending 0). In [12,16): Unavailable ([12,15) lies in [boot_start,15)); D15's X duration/CPU/count are uncertified and its completion at 15=C_close is excluded.
why: D37(b) pass conditions, D35/D38 (failed bracket does not roll back producer writes), D37(a), D41/D42.

Q03: `D15[U] → F15`; failed bracket (S1 running is not an inactivity proof); b_D=15 route (a); gap closed@15; X/CPU in [12,16) Unavailable, completion at 15 excluded.
why: D37(b) (both inactivity proofs must succeed), D37(a), D42.

Q04: `D15[U] → F15`; failed bracket (pcount and state letters alone are insufficient); b_D=15 route (a); gap closed@15; X/CPU in [12,16) Unavailable.
why: D37(b) supported-interface paragraph ("state letter ... or scheduler counter alone is insufficient"), D42.

Q04b: `D15[U] → F15`; failed bracket (schedstat counters alone insufficient); b_D=15 route (a); gap closed@15; X/CPU in [12,16) Unavailable.
why: D37(b) supported-interface paragraph, D42.

Q05: `D15[U] → F15`; failed bracket (denied/unreadable proof), no loss increment and no VOID (counters C0); b_D=15 route (a); gap closed@15; X/CPU in [12,16) Unavailable.
why: D37(b) ("denied, unreadable ... is a failed bracket"), D38 (no manufactured loss), D42.

Q06: `D15[U] → F15`; failed bracket (Threads=2 during bracket; endpoint counts cannot repair it); b_D=15 route (a); gap closed@15; X/CPU in [12,16) Unavailable.
why: D37(b) single-threaded throughout, D39 (endpoint thread counts alone do not prove history), D37(a)/D38.

Q07: `D15[T] → F15`. Seed: wait 0, t=12, off-CPU, on_cpu_ts=0, CPU base 0 with validity true (zero is a valid base). b_D=12 route (b); gap closed@12 (pending 0). CPU for [12,15) = 1 ns (measured consumption, not 3 ns of wall time, not unavailable because base is 0).
why: D37(b) CPU paragraph (on_cpu_ts=0 even when X=0; validity bit; no wall-time substitution).

Q08: `F15`. Bracket rejected (armed A1 ≠ resolved A2); no AD, so D remains pending without ADMIT; no fault. No b_D; gap open(1).
why: D37(b) step 3/pass conditions, D39 (address mismatch rejects; does not authorize admission), D38.

Q09: S(12): `D9[U] → F11`; b_D=11 route (b) via α3 (t=11, W=12>11); D9 (origin α1, earlier attempt) is uncertified and does not modify the baseline, which remains the α3 seed (X, t=11). gap closed@11 (pending 0). S(16): `D15[T] → F15`; X duration 4 ns (11→15) from the α3 baseline; baseline after release is state 0 at 15. Final lower bound b_D=11.
why: D36 (membership from retained attachment evidence, not timestamp), D37(b) (earlier-attempt records uncertified, must not modify confirmed baseline; reconcile staged records), D38, D41/D42.
