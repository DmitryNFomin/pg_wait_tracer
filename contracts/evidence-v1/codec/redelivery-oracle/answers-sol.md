Thirty cases have determined codec results. Case 29 has an unresolved RF stop-eligibility condition in clause 7; both resulting codec outcomes are given below. Exact certificate survival after the explicit VOID also depends on replay rules absent from the permitted inputs.

All references below are to the supplied files. No linked material was consulted.

I use these physical-file identities:

| Symbol | Physical file |
|---|---|
| `F0` | `("b","a",0)` |
| `F1` | `("b","a",1)` |
| `F2` | `("b","a",2)` |
| `FM` | `("b","m",0)` |
| `FX` | `("b","x",0)` |

A record written as `F@(B,R)` has the full identity `(boot,capture,file_no,B,R)`, using the corresponding file tuple above. Scalar stop offsets mean `(offset,null)`.

The following definitions expand every record name used in the completed table. `Z15` means **known=true with exactly fifteen zero counters**. Fork `present(0)` differs from an absent fork.

- `L(p)` = `LIFE(t=0,pid=p,fork=present(0),start=0,start_known=true,exit=absent)`.
- `B(t)` = `ATTACH_OK(t=t,pid=1,fork=present(0),method=reread[1],seed=null[0],state=null[0],counters=Z15)`.
- `F(t)` = `FRONTIER(t=t,counters=Z15)`.
- `V` = `VOID(t=30,counters=Z15,reason="x",invalid_from=present(20))`.

| Record | Full physical identity, by expansion | Full contents, by expansion |
|---|---|---|
| `a0` | `F0@(193,8)` | `L(1)` |
| `a1` | `F0@(305,8)` | `B(0)` |
| `a2` | `F0@(537,8)` | `F(10)` |
| `a3` | `F0@(747,8)` | `F(20)` |
| `a4` | `F0@(957,8)` | `V` |
| `n0` | `F1@(241,8)` | `L(2)` |
| `n1` | `F1@(353,8)` | `L(3)` |
| `n2` | `F1@(465,8)` | `L(4)` |
| `n3` | `F1@(577,8)` | `L(5)` |
| `l0` | `F2@(241,8)` | `L(6)` |
| `l1` | `F2@(353,8)` | `L(7)` |
| `l2` | `F2@(465,8)` | `L(8)` |
| `l3` | `F2@(577,8)` | `L(9)` |
| `x0` | `FX@(193,8)` | `L(10)` |
| `x1` | `FX@(305,8)` | `L(11)` |
| `x2` | `FX@(417,8)` | `L(12)` |
| `x3` | `FX@(529,8)` | `L(13)` |
| `m0` | `FM@(201,0)` | `TRANS(t=1,pid=1,fork=absent,old=null[0],new="w"[1],duration=0,query_id=0,cpu_ns=0)` |
| `m2` | `FM@(377,8)` | `L(1)` |
| `m3` | `FM@(489,0)` | `TRANS(t=3,pid=1,fork=absent,old="w"[1],new=null[0],duration=2,query_id=0,cpu_ns=0)` |
| `r0` | `F0@(193,8)` | `L(1)`; identical to `a0` |
| `r1` | `F0@(305,8)` | `B(0)`; identical to `a1` |
| `r2` | `F0@(537,8)` | `B(1)` |
| `r3` | `F0@(769,8)` | `F(20)` |

TRANS counters are unused. Other unused logical adapter fields have CODEC §7’s zero/false initialization.

In the table:

- `S(F,o,r;W)` is a normalized codec stop.
- `d(delivery,o,r)` is an independent diagnostic.
- `S` in a diagnostics cell expands to **all underlying diagnostics for that row’s stops**, with their witnesses and link evidence described in its derivation.
- BC/HC witness pairs are unordered. `predecessor→successor` preserves SC roles.
- Diagnostic cells denote deduplicated sets, canonically ordered under clause 20 after expansion.
- `Ac2t1 ≡ At1`: these are two names for the same delivery bytes.
- `V-effect` means the retained `a4` must be replayed with its encoded fields, including for a query ending before detection time 30. The limitation on determining its exact surviving certificate set is stated below.

The completed table is:

| # | Input multiset | Codec records, in physical order | Normalized codec stops | Diagnostics / explicit replay effects |
|---:|---|---|---|---|
| 1 | `At1,At3` | `[a0,a1,a2]` | `S(F0,747,T;{At3})` | `S + d(At1,305,T)` |
| 2 | `A,At1` | `[a0,a1,a2,a3]` | None | `d(At1,305,T)` |
| 3 | `Ag,At1` | `[a0,a1,a2,a3]` | `S(F0,957,T;{Ag})` | `S + d(At1,305,T)` |
| 4 | `A,Acrc2` | `[a0,a1,a2,a3]` | None | `d(Acrc2,537,CRC)` |
| 5 | `A,Ac2t1` | `[a0,a1,a2,a3]` | None | `d(Ac2t1≡At1,305,T)` |
| 6 | `A,Ac2t3` | `[a0,a1]` | `S(F0,537,BC;{(A,Ac2t3)})` | `S + d(Ac2t3,747,T)` |
| 7 | `A,Ah` | `[]` | `S(F0,0,HC;{(A,Ah)})` | `S` |
| 8 | `A,Ag,Nt2` | `[a0,a1,a2,a3,n0,n1]` | `S(F1,465,T;{Nt2})` | `S + d(Ag,957,T)` |
| 9 | `At3,N` | `[a0,a1,a2]` | `S(F0,747,T;{At3}); S(F1,0,PU;{N})` | `S` |
| 10 | `At1,At3,A` | `[a0,a1,a2,a3]` | None | `d(At1,305,T), d(At3,747,T)` |
| 11 | `At1,X,At3` | Capture `"a"`: `[a0,a1,a2]`; independently, capture `"x"`: `[x0,x1,x2,x3]` | `S(F0,747,T;{At3})` | `S + d(At1,305,T)` |
| 12 | `A,A,Af,Ao` | `[a0,a1,a2,a3]` | None | None |
| 13 | `At3,Acrc3` | `[a0,a1,a2]` | `S(F0,747,CRC;{Acrc3})` | `S + d(At3,747,T)` |
| 14 | `M,Mt3` | `[m0,m2,m3]` | None | `d(Mt3,489,T)`; sample omitted; no attachment is supplied |
| 15 | `M,Mc1t3` | `[m0]` | `S(FM,292,BC;{(M,Mc1t3)})` | `S + d(Mc1t3,489,T)`; conflict is in the omitted sample block |
| 16 | `A,A+,N` | `[a0,a1,a2,a3]` | `S(F0,957,SC;{(A+→N)})` | `S`; no PU; `a4` excluded, so no explicit VOID replay |
| 17 | `A,Z` | `[a0,a1,a2,a3]` | None | `d(Z,0,IH)`, unassigned to a physical file |
| 18 | `A+,N` | `[a0,a1,a2,a3]` | `S(F0,957,SC;{(A+→N)})` | `S`; no PU; `a4` excluded |
| 19 | `Ag,N` | `[a0,a1,a2,a3]` | `S(F0,957,T;{Ag}); S(F1,0,PU;{N})` | `S` |
| 20 | `At1,N,L,Lc2t3` | `[a0]` | `S(F0,305,T;{At1}); S(F1,0,PU;{N}); S(F2,465,BC;{(L,Lc2t3)})` | `S + d(Lc2t3,577,T)` |
| 21 | `Ag,Ag′` | `[a0,a1,a2,a3]` | `S(F0,957,T;{Ag,Ag′})` | `S`; rejected tail bytes supply no BC |
| 22 | `A,A+` | `[a0,a1,a2,a3,a4]` | None | No codec diagnostics; `V-effect` |
| 23 | `A,L` | `[a0,a1,a2,a3]` | `S(F2,0,PU;{L})` | `S` |
| 24 | `N` | `[]` | `S(F1,0,PU;{N})` | `S`; no root is manufactured |
| 25 | `Aft` | `[a0,a1,a2,a3]` | `S(F0,957,T;{Aft})` | `S` |
| 26 | `A,Aft` | `[a0,a1,a2,a3]` | None | `d(Aft,957,T)` |
| 27 | `A+,Nb` | `[a0,a1,a2,a3,a4]` | `S(F1,0,PU;{Nb})` | `S`; no SC; `V-effect` |
| 28 | `A+,Ni` | `[a0,a1,a2,a3,a4]` | `S(F1,0,PU;{Ni})` | `S`; no SC; `V-effect` |
| 29 | `Au,Auc3` | **Undetermined U29:** `[a0]` if RF is stop-eligible; otherwise `[a0,a2]` | **U29:** RF-eligible: `S(F0,(305,8),RF;{Au,Auc3})`; otherwise `S(F0,747,BC;{(Au,Auc3)})` | Always: `d(Au,(305,8),RF)`, `d(Auc3,(305,8),RF)`, and BC at 747 witnessed by `(Au,Auc3)` |
| 30 | `R,Rc3` | `[r0,r1,r2]` | `S(F0,769,BC;{(R,Rc3)})` | `S`; separate replay stop at `F0@(537,8)` for overlapping attachment; effective cutoff is that replay position |
| 31 | `Z` | `[]` | None | `d(Z,0,IH)`, with no trusted file/capture identity; failed decode, not empty success |

The per-case derivations, including source provenance and local predecessor decisions, are:

1. **`At1,At3`.** Their independently validated ends are 305 and 747. The shorter validated prefix agrees with the longer one. Source provenance is `F0:{At3}`. With no clean maximum-prefix delivery, clause 12 selects `At3`’s T at 747. `At1`’s T at 305 remains diagnostic only. The root is admitted through `a2`.

2. **`A,At1`.** Ends are 957 and 305; provenance is `F0:{A}`. Clean EOF on `A` supplies the complete compatible prefix. The shorter failure cannot be a stop candidate. All four A records are admitted.

3. **`Ag,At1`.** Ends are 957 and 305; provenance is `F0:{Ag}`. The extra byte is an incomplete block header, hence T at 957. No maximum-prefix delivery ends cleanly, so that T is eligible. Earlier records survive, including `a3`; the shorter T is diagnostic only.

4. **`A,Acrc2`.** `Acrc2` validates through 537, then fails block 2’s CRC. `A` validates cleanly through 957. Provenance is `F0:{A}`. The rejected block supplies no conflict evidence, and its shorter terminal failure is ineligible. The CRC diagnostic remains.

5. **`A,Ac2t1`.** Truncation occurs before the changed block exists in the delivery; `Ac2t1` is exactly `At1`. Ends are therefore 957 and 305, and provenance is `F0:{A}`. No validated disagreement exists. The result equals case 2.

6. **`A,Ac2t3`.** Ends are 957 and 747; provenance is `F0:{A}`. Both deliveries validate block 2 at 537, whose FRONTIER contents differ (`t=10` versus `t=11`). Clause 9 produces BC there. The whole block and subsequent records are excluded. `Ac2t3`’s first rejected tail at 747 remains diagnostic.

7. **`A,Ah`.** Both headers independently validate, but immutable `start_time_ns` differs. Clause 8 yields HC at zero, witnessed by the pair, and retains no records. Blocks are not compared across these incompatible headers. Clause 10’s source-provenance rule applies only to groups without HC, so no source provenance is assigned here.

8. **`A,Ag,Nt2`.** Root deliveries both have end 957, so root provenance is `F0:{A,Ag}`. Clean `A` suppresses `Ag`’s terminal stop candidacy. `Nt2` has end 465 and provenance `F1:{Nt2}`. Its declared predecessor is file 0 at D=957 with A’s digest. The actual clean witness set is `{A}`; `Ag` fails the no-rejected-tail requirement. No SC exists because neither predecessor has a validated block beyond 957. The link is admitted, then T at 465 excludes `n2` onward.

9. **`At3,N`.** Provenance is `F0:{At3}`, `F1:{N}`, with ends 747 and 689. N declares predecessor file 0, D=957, with A’s digest. `At3` is short and dirty at 747, so it supplies neither an exact clean witness nor an SC proof. N receives PU at its header. Both intrinsic stops remain reportable; only the root’s first three records are admitted.

10. **`At1,At3,A`.** Ends are 305, 747 and 957. Provenance is `F0:{A}`. All validated prefixes are compatible, and the maximum prefix ends cleanly. Both shorter T failures remain diagnostics without stopping the capture.

11. **`At1,X,At3`.** For capture `"a"`, the reasoning is case 1: provenance `F0:{At3}`, T at 747, shorter T diagnostic. Independent capture `"x"` has provenance `FX:{X}`, clean end 641, and admits all four X records. The supplied rules require separate capture streams; no cross-capture replay stitching occurs.

12. **`A,A,Af,Ao`.** Identical A bytes count once. The three distinct deliveries have end 957, giving provenance `F0:{A,Af,Ao}`. Flags are omitted from immutable header comparison, and Ao’s exact footer is accepted. Their validated blocks agree. There are no conflicts or failures, and each physical record is emitted once.

13. **`At3,Acrc3`.** Both independently validate through 747, giving provenance `F0:{At3,Acrc3}`. Neither ends cleanly. Their terminal reasons tie on position, and CRC outranks T, so the sole terminal candidate is CRC with witness `{Acrc3}`. The T diagnostic is preserved. Neither rejected block can establish BC.

14. **`M,Mt3`.** Ends are 580 and 489; provenance is `FM:{M}`. The shorter prefix agrees, and clean M prevents a terminal stop. All blocks participate in validation, but the valid sample is omitted from the exact array. Thus the surviving offsets are 201, 377 and 489. No attachment is present; omission of the sample does not create one.

15. **`M,Mc1t3`.** Ends are 580 and 489; provenance is `FM:{M}`. Both validate the sample block at 292, but its stored query ID differs. Clause 9 compares stored block bytes even though samples are omitted from exact output. BC at 292 excludes that block and everything later, leaving only `m0`. The shorter delivery’s T at 489 remains diagnostic.

16. **`A,A+,N`.** Ends are 957, 1179 and 689; provenance is `F0:{A+}`, `F1:{N}`. N declares file 0, D=957, with A’s digest. Its actual clean predecessor witness set is `{A}`. Independently, `(A+→N)` proves SC: static fields match, 957 is a validated boundary, the digest matches through 957, and A+ has a validated block beyond it. Clause 15 rejects the link despite the clean witness and emits no PU. SC excludes `a4` and N.

17. **`A,Z`.** Only A establishes a verified identity and validated end. Provenance is `F0:{A}`. Z’s failed header CRC receives IH at zero without trusted file attribution. Z cannot participate in HC or contribute blocks. A’s four records remain admitted.

18. **`A+,N`.** Provenance is `F0:{A+}`, `F1:{N}`, with ends 1179 and 689. There is no clean predecessor ending at N’s declared D=957. Nevertheless `(A+→N)` satisfies every SC condition, including the matching digest and complete extension block. Clause 15 requires SC instead of an additional PU. The retained root prefix ends before `a4`.

19. **`Ag,N`.** Provenance is `F0:{Ag}`, `F1:{N}`, with ends 957 and 689. Ag’s T at 957 is eligible. N’s declared predecessor fields and digest match the validated A prefix through 957, but Ag has rejected tail bytes, so it is not an actual clean witness. Those bytes are not a complete validated extension block, so they establish no SC. N receives PU, and all four root records survive.

20. **`At1,N,L,Lc2t3`.** Provenance is `F0:{At1}`, `F1:{N}`, `F2:{L}`; ends are respectively 305, 689, and maximum 689 for file 2. N declares A at D=957; the sole available root delivery is short and dirty at 305, producing PU for N. Both L headers declare N at D=689 with N’s digest, and the actual witness set for each is `{N}`: N independently validates cleanly through that boundary even though its own incoming link fails. Their local links therefore add no PU. File 2’s validated blocks first differ at 465, producing BC; `Lc2t3` also has T at 577. All three normalized file stops remain reportable, while the earliest capture cutoff permits only `a0`.

21. **`Ag,Ag′`.** Both ends are 957, and provenance is `F0:{Ag,Ag′}`. Their validated prefixes agree. Neither maximum-prefix delivery is clean; both have T at 957, so both belong to its witness set. Different rejected bytes cannot establish BC.

22. **`A,A+`.** Ends are 957 and 1179; provenance is `F0:{A+}`. A is an exact validated prefix of A+, and no successor asserts sealing at 957. There is no conflict or stop. `a4` is admitted and must affect updated replay, including a query ending before t=30. Its explicit `invalid_from=20` and readable zero counters must be preserved.

23. **`A,L`.** Provenance is `F0:{A}`, `F2:{L}`, with ends 957 and 689. L declares predecessor file 1, D=689, with N’s digest. No file-1 delivery exists; A cannot substitute for that immediate predecessor. L receives PU at zero. No stop is manufactured for missing file 1, and A’s four records remain admitted.

24. **`N`.** Provenance is `F1:{N}`, end 689. Its declared predecessor is missing file 0 at D=957 with A’s digest. There is no witness or SC proof, so N receives PU. It cannot become a root, and none of its records enter the codec-admitted stream.

25. **`Aft`.** Provenance is `F0:{Aft}`, end 957. The 17-byte footer fragment is neither EOF nor the exact complete reconstructed footer. It is an incomplete block header, giving eligible T at 957. All four validated records remain.

26. **`A,Aft`.** Both ends are 957, giving provenance `F0:{A,Aft}`. Their validated prefixes agree. Clean A suppresses terminal candidacy for Aft’s T, but the diagnostic remains. All four records are admitted.

27. **`A+,Nb`.** Provenance is `F0:{A+}`, `F1:{Nb}`, with ends 1179 and 689. Nb declares predecessor file 0 at D=957, but its stored digest differs from A’s. A+ is not an exact clean predecessor ending at 957; moreover the digest mismatch prevents an authenticated SC proof despite the boundary and extension. Nb receives PU with those link-check results. There is no predecessor cutoff at 957, so `a4` is admitted and its explicit replay effect applies.

28. **`A+,Ni`.** Provenance is `F0:{A+}`, `F1:{Ni}`, with ends 1179 and 689. Ni declares predecessor file 0 at D=956, with the correctly computed digest through that byte offset. But 956 is inside the final CRC of A’s fourth block, not a validated block boundary. No exact clean witness exists, and clause 14’s boundary condition prevents SC. Ni receives PU. A+ remains fully admitted, including `a4`.

29. **`Au,Auc3`.** Both independently validate through 957 despite the bounded unknown kind, so provenance is `F0:{Au,Auc3}`. Each produces RF at `(305,8)`. Inspection continues, finding their first validated block disagreement at 747: FRONTIER t=20 versus t=21. Thus the two RF diagnostics and the BC diagnostic are determined. If the RFs are eligible stop candidates, they normalize to RF at `(305,8)` with both deliveries as witnesses, retaining only `a0`. If they are not eligible, BC at 747 is the normalized stop; the unknown record is omitted and the retained ordinary records are `a0,a2`. The eligibility gap is explained below.

30. **`R,Rc3`.** Both ends are 979, giving provenance `F0:{R,Rc3}`. Their last validated blocks differ at 769, producing BC there. The overlapping attachment is a replay-state inconsistency, not codec RF, so the codec array contains `r0,r1,r2`. Replay separately stops at the fixture-specified `r2` position `(537,8)`. This earlier reachable replay stop determines the effective cutoff. Codec inspection still reports the later BC; neither last FRONTIER reaches replay.

31. **`Z`.** Header validation fails, so there is no verified physical-file group, source provenance, predecessor decision or attributed codec stop. The output has no records, but IH remains an unassigned failed decode. It cannot be reported as empty success.

The undetermined values are:

| Item | Clause and affected cases/cells | What is missing |
|---|---|---|
| **U29: RF stop eligibility** | REDELIVERY clause 7; case 29’s codec-record and normalized-stop cells | Clause 7 requires RF candidates **“where the governing contract requires stopping”**, but does not state which structural faults meet that condition. The fixture establishes an RF at `(305,8)`, without explicitly establishing candidate eligibility. The supersession table removes the old continued-global-VOID effect for unknown kinds, but does not itself supply the missing eligibility predicate. CODEC §9’s former continuation wording therefore cannot resolve this. The two conditional codec results above preserve the determined diagnostics without inventing that predicate. |
| **Exact certificate survival after VOID** | REDELIVERY clause 19 and CODEC §§5,9; cases 22, 27 and 28’s explicit replay effects | These files delegate retrospective revocation and surviving-certificate reconstruction to CONTRACT/r3, whose contents were not supplied. They determine admission and processing of `VOID(t=30,invalid_from=20,counters=Z15,reason="x")`, including its effect on earlier queries. They do not specify enough interval/certificate rules to enumerate the exact surviving certificate set, including treatment of a certificate ending exactly at 20 or the resulting F/H state. |

No other requested codec record, provenance set, local predecessor witness/admission decision, diagnostic or normalized stop is left undetermined by the supplied fixtures and reconciliation clauses.

---

# Round 2 (cases 22, 27-30, with CONTRACT.md and REVISION-r3.md also supplied)

Let `F0=("b","a",0)`, `F1=("b","a",1)`, and `Z15=[0,…,0]` with exactly fifteen entries. Delivery names denote the content-derived identifiers specified by the fixtures.

These record names expand to full physical identities and contents:

| Record | Physical identity `(boot,capture,file_no,block_offset,record_offset)` | Contents |
|---|---|---|
| `a0`, also `r0` | `("b","a",0,193,8)` | `LIFE(t=0,pid=1,fork=0,start=0,start_known=true,exit=absent)` |
| `a1`, also `r1` | `("b","a",0,305,8)` | `ATTACH_OK(t=0,pid=1,fork=0,method=reread,seed=null,state=null,counters=Z15)` |
| `a2` | `("b","a",0,537,8)` | `FRONTIER(t=10,counters=Z15)` |
| `a3` | `("b","a",0,747,8)` | `FRONTIER(t=20,counters=Z15)` |
| `a4` | `("b","a",0,957,8)` | `VOID(t=30,counters=Z15,reason="x",invalid_from=20)` |
| `r2` | `("b","a",0,537,8)` | `ATTACH_OK(t=1,pid=1,fork=0,method=reread,seed=null,state=null,counters=Z15)` |

`S` in the diagnostics column includes every diagnostic underlying the listed normalized stop, as defined in `CASES-blank.md:20`.

| Case | Codec records, in order | Normalized codec stops | Diagnostics |
|---|---|---|---|
| **22** | `[a0,a1,a2,a3,a4]` | None | None |
| **27** | `[a0,a1,a2,a3,a4]` | `S(F1,0,PU;{Nb})` | **S**; predecessor-link evidence detailed below |
| **28** | `[a0,a1,a2,a3,a4]` | `S(F1,0,PU;{Ni})` | **S**; predecessor-link evidence detailed below |
| **29** | `[a0]` | `S(F0,(305,8),RF;{Au,Auc3})` | **S** + suppressed `BC` at `(747,null)`, proving-pair set `{(Au,Auc3)}` |
| **30** | `[r0,r1,r2]` | `S(F0,769,BC;{(R,Rc3)})` | **S**; separate reachable replay stop at `("b","a",0,537,8)` for overlapping attachment |

**22 — provenance, admission, and replay.**  
`E(A)=957`, `E(A+)=1179`; source provenance is `{A+}`. The validated block prefixes are compatible. Both deliveries end cleanly. There is no successor, so there is no predecessor-link decision or sealing proof. Clauses 9–12 of `REDELIVERY.md` retain the complete longer prefix.

Replay accepts frontiers 10 and 20 before applying `a4`. The VOID permanently revokes frontier 20 and all observations dependent on it; frontier 10 survives. Thus `F=10`, while publication high-water `H=20` survives rollback. The attachment at 0 remains the anchor of the run frozen at 10. Its projection is `life=[0,null], coverage=[[0,10]], waits=[], open=null`. The nonnull `invalid_from` also causes lifecycle uncertainty for the existing generation despite unchanged counters; verified LIFE survives, but that generation cannot start a recovered run. There is **no replay stop**. These effects follow from `CONTRACT.md:148–159,169,178–203,216–223`.

**27 — provenance, predecessor decision, and replay.**  
Maximum-prefix source sets are `F0:{A+}` and `F1:{Nb}`; the latter group is excluded from admission. Both deliveries independently validate to clean EOF, at 1179 and 689 respectively.

`Nb` declares predecessor `("b","a",0)`, `D=957`, with A’s digest having its first bit flipped. For the available predecessor `A+`:

- `957` is a validated block boundary, and the other static descriptor/link fields agree.
- The declared digest fails comparison with `A+`’s immutable bytes through 957.
- `A+` also has a complete validated block beyond 957 and does not end cleanly there.

There is no actual witness. The digest mismatch prevents an authenticated sealing proof, so there is **PU at the successor header, not SC at 957** (`REDELIVERY.md` clauses 13–15). No `Nb` records are admitted.

The complete A+ stream, including its VOID, precedes the cutoff. Frontier 20 is revoked; frontier 10, verified LIFE, and the run frozen at 10 survive; `F=10`, `H=20`, with the same projection and lifecycle taint as case 22. The codec cutoff is `F1:(0,null)`; there is **no additional semantic replay stop**. Prior revocations remain effective at a chain cutoff (`CONTRACT.md:189–203,235–243,307–313`).

**28 — provenance, predecessor decision, and replay.**  
Maximum-prefix source sets are `F0:{A+}` and `F1:{Ni}`; `Ni` is excluded from admission. Both independently validate to clean EOF.

`Ni` declares predecessor `("b","a",0)`, `D=956`, and the digest through that offset. Against `A+`, that digest matches the referenced bytes and the other static fields agree, but **956 is inside the block ending at 957**, not a validated block boundary. There is neither a complete predecessor data region ending at 956 nor a clean actual witness. The missing boundary also prevents SC, even though A+ contains later validated data. Consequently the result is **PU at the successor header** (`REDELIVERY.md` clauses 13–15).

A+’s VOID remains admitted. Frontier 20 is revoked; frontier 10 and verified LIFE survive, with the run frozen at 10, `F=10`, `H=20`, and the same projection and lifecycle taint as case 22. The codec cutoff is `F1:(0,null)`; there is **no additional semantic replay stop** (`CONTRACT.md:148–159,178–203,235–243,307–313`).

**29 — provenance, diagnostics, and replay.**  
Both deliveries have `E=957`; source provenance is `{Au,Auc3}`. Their blocks remain wholly validated despite the unknown framed kind. Each independently produces RF at `(305,8)`. Continued conflict inspection finds their first differing block at 747: its FRONTIER is 20 in `Au` and 21 in `Auc3`.

Normalization selects the earlier RF and unions its offending-delivery witnesses. The later BC remains a diagnostic (`REDELIVERY.md` clauses 5, 7, 9–10, 16–18). Neither delivery has a rejected tail; there are no successor links.

Only LIFE reaches replay. **No FRONTIER or certified run survives, and none is revoked**: the later frontiers are excluded before replay. `F` and `H` remain absent; LIFE remains `[0,null]`, with `coverage=[], waits=[], open=null`. The unsupported kind requires an ordered stop without synthetic VOID or invented loss state (`CONTRACT.md:235–255`; `REVISION-r3.md:7,13–15`). That stop is already represented by codec RF; **no separate replay stop is reachable** (`REDELIVERY.md` clause 19).

**30 — provenance, diagnostics, and replay.**  
Both deliveries have `E=979`; source provenance is `{R,Rc3}`. Their first differing block begins at 769, containing FRONTIER 20 versus 21. BC therefore excludes that whole block, leaving `[r0,r1,r2]`. Both tails are clean; there are no successor links.

The second attachment is structurally valid, so it produces **no codec RF** (`REDELIVERY.md` clause 7). Replay nevertheless finds that `r2` overlaps the same generation’s run anchored by `r1`, with no END or VOID separating them. Replay stops immediately before `r2`, at `("b","a",0,537,8)` (`CONTRACT.md:256–258`; `REVISION-r3.md:8,13–15`).

This replay stop precedes the codec cutoff at 769. No FRONTIER was accepted, so **no certified run or frontier survives and none is revoked**; `F` and `H` are absent. The tentative attachment contributes no exact coverage. Verified LIFE survives, yielding `life=[0,null], coverage=[], waits=[], open=null`, without a synthetic VOID (`CONTRACT.md:231–246,312`; `REDELIVERY.md` clause 19).

**Undetermined values:** none within the requested cells. Delivery hashes need not be guessed: fixture names explicitly denote their content-derived identifiers (`CASES-blank.md:20`).