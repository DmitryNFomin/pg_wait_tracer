## 2. ORACLE ROWS (inputs only)

Each row is independent. Use merge-v1 §5 source positions, ordering, strict boundaries, counter observations and release evaluation after every input.

Mode is N unless overridden. Sources are exactly `{P0,P1,E,L}`. Times are integer nanosecond offsets from a positive `boot_start`, displayed as zero. Initially all cutoffs are zero, the epoch is active, queues are empty, no frontier exists, and the startup gap is open. A and B retain §5’s attached defaults.

Unless overridden, the complete census fence is 5 and its only discovery candidate is `D=(pid=30,g=2^63)`. `H=(pid=40,g=2^63+1)`. Counter observations are `C0`; observations use detection time 20. Publications and required checks succeed unless a row overrides them.

Notation additions:

- `AD_De` abbreviates `ADMIT_DISCOVERY(D,α,e,proof,20,C0)` for the final attachment `α`; admission facts satisfy Decision 36 unless overridden.
- `B_Dt{…}` is a quiet-confirmation candidate containing the complete recorded pre-ADMIT bracket and requesting Decision 37(b) validation. It does not assert that overridden facts passed. Its default witness has `S0=inactive`, `S1=inactive`, `v0=v1=7`, `Threads=1` throughout, nonterminal status, one pinned handle, no counter wrap, matching armed/resolved address, wait X, fresh timestamp `t`, CPU accumulator/base 0 with validity true, and `on_cpu_ts=0`. Its `e` and attachment generation match the row’s AD. All initialization, lifecycle, ownership and address-continuity facts otherwise hold.
- Physical seed installation in a B witness precedes its enable and ADMIT, regardless of later control-delivery order. Failed witnesses retain their actual producer history.
- `TRY_D(t,e;…)` records a physical attempt and its observations without an ADMIT. It begins disabled and follows the complete bracket order. A successive TRY includes disable and complete fencing/reconciliation of the preceding attempt.
- `D12(Y→X)` is a TRANS at 12 establishing fresh post-record state. `D15(X→0,3,cpu=1)` additionally specifies incoming duration 3 and CPU consumption 1.
- `READ_D(t,X)` is an ordinary userspace wait-word read.
- `TERM_H18` is complete matching kernel terminal-binding evidence at 18. `EXIT_H18@L` is its lifecycle DATA.
- `K_R20` abbreviates `FORK_R20@L{fork_seed_proved=true}`.
- `LATE_U(40)` is the existing uncovered-record observation at detection time 40 for an unlisted pre-existing lifetime U.
- `AOP(...)` supplies the stated adapter/kernel operation history and observations; it has no implicit transport position or merge DATA.
- `Cf=C0` except `fork_state_update_fail=1`; `Cs=C0` except `state_map_update_fail=1`. `Ce` and `Cw` retain §5’s definitions.
- AD, B, TRY, TERM, AOP and LATE are controls or adapter observations; they consume no transport source positions.
- All stated numerical evaluations request results only from the supplied records and coverage evidence. No unstated open-wait snapshot is supplied.
- `AB_D` abbreviates `ABANDON_DISCOVERY(pid_D,g_D,20,C0)`; `AB_H` substitutes H.
- In the new A rows, D’s discovery identity is installed and unambiguously bound to its census entry. Its attachment attempts have permanently ended without a valid final attachment unless the row supplies ADMIT or overrides these facts.
- `TERM_Dt` and `EXIT_Dt@L` substitute D and timestamp `t` in the existing terminal notation.
- `TERMU_Dt` is the final terminal disposition `unresolved` for the cookie-less EXIT of D's PID at t (Decision 40); it consumes no source position.
- `M_Dt@P0` is a MARKER belonging to D at semantic timestamp `t`.
- `TEXT_Dx@L` is untimed metadata x belonging exclusively to D’s numerical stream. A `{requires x}` annotation specifies a dependency on that metadata.
- AB is a control and consumes no transport source position.
- Every new A row requests the final `discovery_abandoned_records` diagnostic, initially zero.
- Numerical evaluations and startup-gap observations use the emitted coverage records decoded by the existing reader. Supply otherwise valid coverage, attribution and A2 evidence except for restrictions explicitly present in the row. Do not substitute an in-memory discovery lower bound for the persisted gap.

| ID | Input sequence / starting-state override |
|---|---|
| D01 | `AD_D8, D12(Y→X)@P0, D15(X→0,3)@P0, S(16)`. Evaluate X duration/count in both `[12,16)` and `[13,16)`, and evaluate the completed-Y count in `[12,13)`. |
| D02a | `AD_D13, B_D12, S(12), S(13), D15(X→0,3)@P0, S(16)`. Evaluate X in `[12,16)`. |
| D02b | Earlier seed word X. `AD_D8, READ_D(12,X), S(16)`. No TRANS/MARKER input. |
| D02c | Earlier seed word X. `AD_D8, READ_D(12,X), D9(X→Y)@P0, D10(Y→X,1)@P0, S(13)`. Neither record had been dequeued at the reread. Evaluate X in `[10,13)`. |
| D02d | `AD_D13, B_D12{CPU base sampled at 7; wait/time sampled at 12; intervening CPU accumulation; control explicitly claims completed coherence}, S(13)`. |
| D02e | Retry limit 2. `TRY_D(7,8;v0=7,v1=8), TRY_D(13,14;v0=8,v1=9), AD_D14, READ_D(15,Y), S(16)`. Both attempts read X for their seed. No TRANS/MARKER input. |
| D03 | `D15(X→0,3)@P1, D12(Y→X)@P0, S(16), AD_D8`. |
| D04a-install | AD claims installation preceded enable; actual installation followed enable. `AD_D8, D12(Y→X)@P0, S(13)`. |
| D04a-state | AD claims producer state was preserved; actual initialization overwrote a concurrent producer update. `AD_D8, D12(Y→X)@P0, S(13)`. |
| D04a-binding | AD claims complete lifecycle binding; the required binding evidence is absent. `AD_D8, D12(Y→X)@P0, S(13)`. |
| D04a-address | AD claims continuously correct address; the pointer relocated without observation. `AD_D8, D12(Y→X)@P0, S(13)`. |
| D04a-cookie | AD claims valid cookie authority; the cookie was issued outside its allocator partition. `AD_D8, D12(Y→X)@P0, S(13)`. |
| D04b | Task start-time upper bound equals `boot_start`. No AD input. `S(16)`. |
| D04c | `AOP(R receives start timestamp before boot; R stalls before PID allocation; disabled FD opens on O; O exits; R acquires O’s PID; a numeric-PID /proc read observes R’s pre-boot start timestamp)`. Submit AD claiming FD and R identify one lifetime. |
| D04d | `AOP(reserve D’s cookie; D exits with no mutable state; kernel exit tombstones PID; daemon attempts BPF_NOEXIST discovery insertion)`. No terminal witness is delivered to the consumer. |
| D04e | `AOP(insert D identity; D exits and PID is tombstoned; attempt late D insertion; R forks on same PID and fork path tombstones it; fork allocation fails; retry D insertion; R attempts emission), CHECK(20,Cf)`. |
| D04f | Fork allocator next value `2^63−1`; discovery allocator next value `2^64−1`. `AOP(reserve once from each; request once more from each), FAULT(allocation_fail,20,C0)`. One fault input represents the first exhausted request. |
| D04g | `AOP(producer advances CPU/query/command fields; daemon successfully inserts discovery identity; inspect fields; a separate discovery-map insertion fails), CHECK(20,Cs)`. |
| D07a | Census contains D and H. `AD_D13, B_D12, AD_H21, B_H20, S(21)`. |
| D07b | D07a’s inputs with census fence 25, then `S(26)`. |
| D08a | Census contains D and H. H never enabled and has no mutable state. `AD_D13, B_D12, TERM_H18, EXIT_H18@L, S(18), S(19)`. |
| D08b | Census contains D and H. H never enabled and has no mutable state. `AD_D13, B_D12, AOP(PID-only ESRCH observation for H), S(18), S(19)`. |
| D09 | Census contains D. R is `(pid=30,g=1)` with valid fork-seed provenance. `AD_D8, D12(Y→X)@P0, terminal_D18(Y→EXIT,3)@E, EXIT_D18@L, K_R20, R22@P0, D15(X→Y,3)@P1, AOP(delayed_cleanup(D); retry_insert(D)), S(23)`. |
| D10 | D01’s inputs, then `S(31), LATE_U(40)`. U was absent from the census. |
| D13a | `AD_D8, D12(Y→X)@P0, CHECK(20,Ce), S(16)`. |
| D13b | Coverage evaluator receives otherwise valid base coverage and A2 certificates, plus an UNTRACED declaration for discovery lifetime D with `fork_ns<boot_start`. |
| D13c | `FORK_R10@L{fork_seed_proved=false}` for an observed post-start birth; no INIT or valid fork attachment proof. Submit discovery AD for R, then `S(16)`. |
| D13d | `AD_D8, D12(Y→X)@P0, S(13)`. Writer rejects D12 at detection time 20 and observes Cw. |
| D13e | Mode Q. Submit `AD_D8` claiming qualified discovery admission. |
| Q01 | `AD_D13, B_D12, D15(X→0,3,cpu=1)@P0, S(16)`. Evaluate the supplied X interval and CPU in `[12,16)`. |
| Q02 | Q01’s inputs with `v1=8`. During the physical bracket a producer advances the visible provisional generation’s CPU/query/command fields; inspect those fields after witness rejection and before D15. |
| Q03 | Q01’s inputs with `S1=running`. |
| Q04 | Q01’s inputs, replacing both inactivity proofs with equal `pcount` observations and state letters `S,S`. |
| Q04b | Q01’s inputs, replacing both inactivity proofs with `schedstat="0 0 0"` at both endpoints. |
| Q05 | Q01’s inputs with the S1 read returning EACCES; all separately required reads and counter observations remain as specified by the defaults. |
| Q06 | Q01’s inputs with `Threads=2` during the bracket. The extra thread exits before AD delivery; lifecycle accounting is complete, and the final attachment independently satisfies the fresh-record route’s producer obligations. |
| Q07 | `AD_D13, B_D12{X=0, accumulator=0, CPU base=0, base_valid=true, on_cpu_ts=0}, AOP(D remains asleep until 14; switch-in at 14; CPU execution through 15), D15(0→Y,3,cpu=1)@P0, S(16)`. Evaluate CPU for the supplied `[12,15)` interval and inspect the seed. |
| Q08 | `B_D12{e=13, armed_address=A1, resolved_address=A2}, S(16)`, with `A1≠A2`. No AD or DATA input. |
| Q09 | `TRY_D(7,8;α1,v0=7,v1=8), TRY_D(9,10;α2,v0=8,v1=9), TRY_D(11,12;α3,v0=9,v1=9), AD_D12{α3}, B_D11{α3,v0=9,v1=9}, D9(Y→Z,1)@P0{retained origin α1}, S(12), D15(X→0,4)@P0{origin α3}, S(16)`. The D9 record arrives after AD and B. Inspect final lower bound and baseline after each release. |
| D14a | `AD_D8, D12(Y→X)@P0, A12(Y→0,2)@P1, D15(X→0,3)@P0, S(16)`. A’s incoming Y interval has its existing attached provenance. Evaluate the completed-Y count across A and D in `[12,13)`, and X duration/count in `[13,16)`. |
| D14b | Census fence 15. `AD_D8, D12(Y→X)@P0, D15(X→0,3)@P0, S(16)`. Evaluate X duration/count in `[15,16)`. |
| D14c | Q09’s inputs, replacing `D9(Y→Z,1)@P0{retained origin α1}` with `D11(Y→Z)@P0{retained origin α2}`. Retained physical history places this α2 emission before α2’s completion fence and the α3 seed; both the emission and the α3 seed have timestamp 11. Evaluate X duration/count in `[11,16)` and `[12,16)`, and inspect the final lower bound and baseline after each release. |
| A01 | `D9(Y→X)@P0, D12(X→Y,3)@P0, S(13), AB_D, S(16)`. |
| A02 | A01’s inputs, then `D17(Y→X,5)@P0, S(20)`. |
| A03 | A01’s inputs, then `terminal_D18(Y→EXIT,6)@E, EXIT_D18@L, TERM_D18, S(18), S(19)`. |
| A04 | Census contains D and H. `AD_D8, D12(Y→X)@P0, S(13), AB_D`. No H control or DATA is supplied. |
| A05 | `D9(Y→X)@P0, AB_D, AD_D8, S(16)`. |
| A06 | `AB_H`. H is absent from the census. |
| A07 | Census contains D and H. `D9(Y→X)@P0, AD_H13, B_H12, AB_D, H15(X→0,3)@P1, S(16)`. |
| A08 | `D9(Y→X)@P0, AB_D, CHECK(20,Ce), S(16)`. |
| A09 | A03’s inputs, then an identical retransmission of its `AB_D`. |
| A10 | A01’s inputs, then `B_D12{e=13}`. |
| A11 | A01’s inputs, then `D14(Y→X,2)@P0`. |
| A12 | A01’s inputs, then `FAULT(allocation_fail,20,C0), REARM(21,C0,attachment_proofs), D17(Y→X,5)@P0, S(18)`. REARM supplies fresh valid A/B attachment proofs and a readable C0 baseline; it supplies no discovery ADMIT or CONFIRM. Observations after REARM use detection time 22. |
| A13 | `D9(Y→X)@P0, AB_D, BOUNDARY(P0,16,p), BOUNDARY(P1,16,p), BOUNDARY(L,16,p)`, each using its next source position. No E boundary is supplied. |
| A14 | `D9(Y→X)@P0, AB_D, CHECK(20,unknown), S(16)`. The CHECK cannot establish completed failure accounting. |
| A15 | A03’s inputs, except the writer rejects `EXIT_D18` at detection time 20 and observes Cw. Other publications, including VOID, succeed. |
| A16 | `D9(Y→X)@P0{identity_known=false}, AB_D, S(16)`. The record’s attribution to the installed identity cannot be established. |
| A17 | A03’s inputs, then `K_R20, R22@P0, S(23)`. R is `(pid=30,g=1)` with valid fork-seed provenance. |
| A18 | `D9(Y→X)@P0, FAULT(allocation_fail,20,C0), AB_D, S(16)`. |
| A19 | `TEXT_Dx@L, D9(Y→X)@P0{requires x}, M_D10@P0{requires x}, AB_D, S(16)`. Metadata x is valid; no other lifetime depends on it. |
| A20 | D was never enabled and has no mutable interval state. `TERM_D18, EXIT_D18@L, S(19), AB_D`. |
| T1 | `EXIT_pid30@18@L{no cookie}, S(19), TERMU_D18, A20@P0, S(21)`. D's identity is never installed. |
| T2 | `EXIT_pid30@18@L{no cookie}, S(19), A20@P0, S(21), S(40)`, then capture finish at detection 50. D's identity is never installed. |
| T3 | T1's inputs, then `TERM_D18`. |
| T4 | `TERMU_D18, EXIT_pid30@18@L{no cookie}, S(19)`. D's identity is never installed. |
| V1 | `AB_D, EXIT_D18@L, S(19)`, the checkpoint after EXIT_D18's publication observes Ce at 20; `REARM(21,Ce,attachment_proofs), TERM_D18, S(20)`, observations after REARM at 22. |
| V2 | V1 with `TERM_D18` also delivered before `EXIT_D18@L`. |
| V3 | `AB_D, TERM_D17, EXIT_D17@L, A18@P0, S(19)`, writer rejects A18 at 20 observing Cw; then `REARM(21,Cw,attachment_proofs), TERM_D17, S(20)`, observations after REARM at 22. |
| V4 | `AB_D, TERM_D18, EXIT_D18@L, S(19)`, writer rejects F18 at 20 observing Cw; then `REARM(21,Cw,attachment_proofs), TERM_D18, S(20)`, observations after REARM at 22. |
