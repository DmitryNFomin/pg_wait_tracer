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
- Existing source, counter, detection-time and publication defaults apply. `F0=boot_start=0` in displayed offsets; their absolute clock values are positive. Lifecycle activation completes before `F0`.
- `TRY_NW_D(e;…)` is an adapter observation of the pointer-watch qualification sequence, not a claimed successful admission and not a wait-word attachment attempt.
- Its default history is: bound pre-existing leader D; parent postmaster; qualified PG13 logger role 13; pointer watch opened disabled and made visible before enable; successful enable at `e`; pinned NULL read after `e`; completed fence after the read; zero fires through that fence; complete ownership, role, lifecycle and counter proofs.
- The N-row fixture supplies independently qualified permanent-role entries for logger 13 and stats collector 12. These are synthetic qualification inputs, not a claim that platform qualification has occurred.
- `NW_De{…}` expands to that physical history, `AD_De{route=c}`, then `CONFIRM_NO_WAIT_WORD(D,α,e,witness,20,C0)`. Overrides describe actual facts; an explicit false completed-proof claim is identified in the row.
- `PTR_Dt(value)` records a qualified pointer fire at semantic time `t`, observed at detection time 20 with C0 unless overridden. It supplies no implicit transport DATA or boundary.
- `Cr=C0` except `wait_read_fail=1`; `Ck=C0` except `lifecycle_fork_drop=1`.
- N rows inspect attempt count, committed bound, persisted gap, backend listing and numerical-population contributions.
- Unless overridden, J rows replace the default D census with one numeric observation of PID 50: listed at 2, failed open at 3, no bound lifetime, proposed census fence 5.
- `U` denotes a lifetime subsequently bound to PID 50; its discovery cookie, when installed, is `2^63`. `R=(pid=50,g=1)` denotes a fork-route replacement.
- `J_U4` supplies a retained kernel witness for PID 50, known start offset −100, leader, parent postmaster, no fork provenance, exit timestamp 4, complete reconciliation through fence 5 and C0. It requests J resolution and a matched disposition for the identified EXIT transport record.
- `EXIT_U4@L{no cookie}` is that lifecycle DATA. A journal witness supplies no transport position by itself.
- `G_50` supplies an ESRCH result at 4 after the failed open, no eligible J witness, and a completed lifecycle fence at 5 with unchanged C0 over `[F0,5]`.
- `R_50{…}` supplies a new pinned binding and completed lifecycle reconciliation through fence 5. All required identity and history facts hold except stated overrides.
- `KEXIT{…}` exercises the kernel exit filter. It supplies the actual task facts and exposes any emitted records and counter changes to the adapter. Default detection time is 20.
- All J witnesses retain the corresponding EXIT transport identity. No current-map lookup supplies their identity.
- In expected states, `P=L+Q` separates unresolved lifetime count L from unresolved numeric-observation count Q. `census=known/unknown` is explicit. Unless listed, no VOID, FAULT, UNTRACED or additional DATA is produced.
- R rows (forward-only certification latch, order-independent refutation): Use existing source, ordering, publication and detection defaults. D is installed and independently bound; numeric pending is zero. Each parameter combination below is a separate execution, with its parameter tuple retained in the result.
- `ADc6`, `CN6` are the separate controls from `NW_D6`; `TRY_NW_D6` supplies that physical history. Controls explicitly asserting completed proof are identified below.
- `K=CHECK(20,Cr)`; subsequent observations retain Cr. `RA=REARM(21,current-vector,fresh attachment_proofs)`; subsequent observations use detection time 22. Explicit REARM uses the abstract replay entry.
- `h∈{0,1}` means H absent or inventoried and unresolved, respectively. Census fence remains 5 and completeness is known.
- `p∈{empty,AD_D6,AB_D}`. `q∈{empty,K}`. `r∈{empty,RA}`. Each TERM names its exact EXIT transport record.
- `{earlier}` means retained origin α1 against final α2; `{final}` proves final membership. In R1 also execute the membership-unproved variant.
- Every row observes public commitments, pending split, census, latch, closure, persisted gap history, faults, invalidations and consumption counts after every primitive input.
- P rows use the amended N-row defaults; no role-support publisher exists.

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
| N1 | `NW_D6, S(6), S(8)`. D runs during qualification; no inactivity observations or CPU seed are supplied. Inspect backend listing, population contributions and absence of role-support publication. |
| N2 | `TRY_NW_D(6;role=client backend, permanent-role proof absent), S(8)`. Pointer remains NULL. |
| N3 | `TRY_NW_D(6;the only NULL read occurs at 5 before enable), S(8)`. |
| N4 | `TRY_NW_D(6;one pointer fire at 7 observes NULL; pinned NULL read at 8; completed fence at 9), S(10)`. |
| N5 | N1's inputs, then `PTR_D12(non-NULL), S(14)`. All observations remain C0; detection time is 20. |
| N6 | N1's inputs, then `PTR_D12(NULL), TERM_D12, EXIT_D12@L, S(13)`. No mutable numerical state exists. |
| N7 | `TRY_NW_D(4;role=client backend, permanent-role proof absent), PTR_D9(non-NULL), AD_D12, B_D11{e=12}, S(13)`. The ordinary numerical attachment has one complete quiet-bracket attempt. |
| N8 | `TRY_NW_D(4;role=client backend, permanent-role proof absent), TERM_D9, EXIT_D9@L, S(10)`. EXIT_D9 carries D's installed discovery cookie `g=2^63`, snapshotted before tombstoning. No wait-word attachment attempt occurs. |
| N9 | `TRY_NW_D(6;the completion checkpoint observes Cf), CHECK(20,Cf), S(8)`. Subsequent observations use Cf. |
| N10 | N1's amended inputs with role changed to the independently qualified stats-collector role 12. |
| N11 | `TRY_NW_D(6;role=archiver, numeric value 11, no qualified permanent-role entry), S(8)`. |
| N12 | `NW_D6{pointer watch was silently disabled between enable and the NULL read; AD explicitly claims continuous observation; CONFIRM truthfully records the disabling and makes no claim of completed continuity}, S(8)`. Only AD supplies a false-proof fault; no separate fault report is delivered. |
| N13 | Census contains D and H. `NW_D6, AD_H8, H10(Y→X)@P0, S(11)`. |
| N14 | `NW_D6, S(8)`. During the release attempt, before bound commitment or FRONTIER publication, the writer rejects the required startup GAP_CLOSE with endpoint 6, at detection 20 with Cw. |
| N15 | `TRY_NW_D(6;required pinned pointer read fails and increments wait_read_fail), CHECK(20,Cr), S(8)`. Subsequent observations use Cr. |
| N16 | `TRY_NW_D(4;role=authenticating client, permanent-role proof absent, qualified remaining authentication deadline=14), AOP(deadline 14 expires; no initialization or matching terminal evidence), S(16)`. |
| J1 | `J_U4, EXIT_U4@L{no cookie}, S(4), S(8)`. No `/proc` binding succeeds. |
| J2 | `G_50, S(8)`. |
| J3 | Failed open returns ENOENT; `AOP(kill(50,0) at 4 returns 0; no journal witness; completed lifecycle fence 5), S(8)`. |
| J4 | Failed open returns EACCES; `AOP(kill(50,0) at 4 returns EPERM; no journal witness; completed lifecycle fence 5), S(8)`. |
| J5 | `G_50{fence observes Cf}, CHECK(20,Cf), S(8)`. Subsequent observations use Cf. |
| J6 | Journal contains J_U4's task facts, but its relevant producer has entered and not completed. No matching control or lifecycle boundary is supplied. `BOUNDARY(P0,8,p), BOUNDARY(P1,8,p), BOUNDARY(E,8,p)`. |
| J7 | `KEXIT{pid=tid=tgid=50,parent=postmaster,start offset=0,exit=4,no fork cookie,no earlier accounting of the missing fork}, CHECK(20,Ck), S(8)`. Subsequent observations use Ck. |
| J8 | Override: complete empty census, no numeric observations. `KEXIT{tid=50,tgid=60,parent=postmaster,start offset=-100,exit=4,no fork cookie,live stale discovery-map entry for 50}, S(8)`. |
| J9 | Override: complete empty census, no numeric observations. `KEXIT{pid=tid=tgid=50,parent=unrelated,start offset=2,exit=4,no fork cookie,discovery-map tombstone for 50}, S(8)`. |
| J10 | J9's inputs with a stale live discovery identity instead of the tombstone. |
| J11 | `KEXIT{pid=tid=tgid=50,parent=postmaster,successfully read raw start value=0 meaning unknown,exit=4,no fork cookie}, S(8)`. Counters remain C0. |
| J12 | `R_50{current holder R is leader,parent=postmaster,start offset=2,valid fork cookie}, K_R3, R6@P0, S(8)`. Use `H_A2>6 ns`; report R's Decision 25c numerical lower bound and `dN_0,dU_0`. |
| J13 | `R_50{current holder U is leader,parent=postmaster,start offset=-100,no fork provenance; ordinary discovery binding and installation succeed}, AD_U6, U7(Y→X)@P0, S(8)`. |
| J14 | `R_50{current holder is a proved non-child; no earlier eligible target terminal witness}, S(8)`. |
| J15 | `J_U4, EXIT_U4@L{no cookie}, K_R6, R8@P0, S(9)`. R reuses PID 50, starts at 6 and has valid fork provenance. Use `H_A2>6 ns`; report R's Decision 25c numerical lower bound and each lifetime's `N_k,U_k` contributions. |
| J16 | Override: U is already inventoried with retained kernel start offset −200; census complete. A supplied terminal control explicitly claims that J_U4's start offset −100 matches U. Then `EXIT_U4@L{no cookie}, S(8)`. |
| J17 | `J_U4`, but its named lifecycle DATA is `EXIT_U5@L{no cookie}`; the terminal control explicitly claims equal journal/DATA timestamps. Then `S(8)`. |
| J18 | J1's inputs without `S(4)`; the writer rejects EXIT_U4 at detection 20 with Cw. |
| J19 | J1's inputs without `S(4)`; EXIT_U4 publication succeeds, but the following checkpoint observes Cf at detection 20 before FRONTIER publication. |
| J20 | J2's inputs, then `LATE_U(40)` for an unlisted pre-existing target lifetime using PID 50. No additional transport DATA is supplied. |
| J21 | `G_50{the only ESRCH observation occurs at 2 before the failed open at 3}, S(8)`. |
| J22 | `R_50{current holder R is leader,parent=postmaster,start offset=-100,valid observed fork cookie}, K_R1, R6@P0, S(8)`. Retained history proves this observed fork despite its pre-`F0` task-start timestamp. Use `H_A2>6 ns`; report R's Decision 25c numerical lower bound and `dN_0,dU_0`. |
| R1 | `AD_D6{α2}, D14@P0{earlier}, S(15), B_D6{e=6,α2; explicitly claims completed reconciliation}, S(25)`. Repeat with D14's final-attachment membership unproved. |
| R2 | For every p, execute both `p, TERM_D18, D20@P0, EXIT_D18@L, S(25)` and `p, D20@P0, TERM_D18, EXIT_D18@L, S(25)`. For each, also insert K immediately before the TERM/DATA pair, or between its two members. No REARM. |
| R3 | For every h and q: `NW_D6, S(8), q, D12@P0`. Repeat replacing D12 with `PTR_D12(non-NULL)` and with a demonstrated pointer-observation continuity break. |
| R4 | For every h and q: `AD_D13, B_D12, S(14), q, D15@P0{earlier}`. |
| R5 | For every h, p and q: `p, TERM_D18, EXIT_D18@L, S(19), q, D20@P0`. |
| R6 | `AB_D, EXIT_D18@L, D20@P0, S(21), TERM_D18`. |
| R7 | (a) `TERM_D18, EXIT_D18@L, S(19), K, RA, S(25)`. (b) `AD_D6, D12@P0{final}, S(13), D20@P0{final}[Cr], RA, S(25)`. |
| R8 | For each r: (a) `AD_D6, K, r, S(25), D26@P0{final}, S(27)`; (b) `AD_D6, B_D6, K, r, S(25)`; (c) `NW_D6, K, r, S(25)`. Bounds have no source completeness before K. RA's fresh attachment proofs include D's attachment. |
| R9 | For each r: (a) `AD_D6, EXIT_D18@L, S(19), K, r, TERM_D18, S(25)`; (b) `AD_D6, TERM_D26, K, r, EXIT_D26@L, S(27)`. Terminal bindings are independently valid; their controls make no false claim. |
| R10 | `TRY_NW_D6, ADc6, PTR_D9(non-NULL), CN6, S(10)`. CN6 explicitly asserts completed permanent-role/continuity reconciliation despite PTR9. Repeat with the demonstrated continuity break. |
| R11 | D never receives ADMIT or ABANDON: `EXIT_D18@L, S(19)`. No disposition or capture-finish input follows. |
| R12 | D never receives ADMIT or ABANDON: `TERM_D18, EXIT_D18@L, S(19), D12@P0`. |
| P1 | N1, with no role-support publisher installed. |
| P2 | N1, then a demonstrated pointer-observation continuity break at detection 20, C0; `S(14)`. |
| P3 | N6, persisted and read back; inspect startup endpoint and backend/omission populations. |
