The workspace is read-only, and TASK.md specifies no answer filename. The completed answer follows inline. I read only the six authorized files.

All sets below are mathematical sets: duplicate deliveries contribute once, and their presentation does not imply arrival order. Record arrays are ordered within each capture. Independent captures are returned separately.

Let `D` denote REDELIVERY’s numbered clauses and `K` denote CONTRACT’s sections.

**Record identities and contents**

These definitions expand every record abbreviation used in the tables. A record is `(boot,capture,file_no,block_offset,record_offset; contents)`.

`B0` is fifteen zeros. `B1` has only slot 4 equal to one. Every LIFE below has `start_known=true` and `exit=null`.

| Records | Full physical identity and contents |
|---|---|
| `a0` | `(b,a,0,193,8; LIFE,t=0,pid=1,fork=0,start=0)` |
| `a1` | `(b,a,0,305,8; ATTACH_OK,t=0,pid=1,fork=0,method=reread,seed=null,state=null,counters=B0)` |
| `a2` | `(b,a,0,537,8; FRONTIER,t=10,counters=B0)` |
| `a3` | `(b,a,0,747,8; FRONTIER,t=20,counters=B0)` |
| `a4` | `(b,a,0,957,8; VOID,t=30,counters=B0,reason="x",invalid_from=20)` |
| `v4` | `(b,a,0,957,8; VOID,t=30,counters=B1,reason="x",invalid_from=20)` |
| `vu4` | `(b,a,0,957,8; VOID,t=30,counters=null,reason="x",invalid_from=20)` |
| `w4` | `(b,a,0,957,8; FRONTIER,t=30,counters=B1)` |
| `n0…n3` | `(b,a,1,o,8; LIFE,t=0,pid=p,fork=0,start=0)`, ordered `(o,p)=(241,2),(353,3),(465,4),(577,5)` |
| `l0…l3` | `(b,a,2,o,8; LIFE,t=0,pid=p,fork=0,start=0)`, ordered `(o,p)=(241,6),(353,7),(465,8),(577,9)` |
| `q0…q3` | `(b,a,1,o,8; LIFE,t=40,pid=p,fork=40,start=40)`, ordered `(o,p)=(241,2),(353,3),(465,4),(577,5)` |
| `x0…x3` | `(b,x,0,o,8; LIFE,t=0,pid=p,fork=0,start=0)`, ordered `(o,p)=(193,10),(305,11),(417,12),(529,13)` |
| `m0` | `(b,m,0,201,0; TRANS,t=1,pid=1,old=null,new="w",duration=0,query_id=0,cpu_ns=0)` |
| `m2` | `(b,m,0,377,8; LIFE,t=0,pid=1,fork=0,start=0)` |
| `m3` | `(b,m,0,489,0; TRANS,t=3,pid=1,old="w",new=null,duration=2,query_id=0,cpu_ns=0)` |
| `r0,r1` | Respectively identical to `a0,a1`, including physical identities |
| `r2` | `(b,a,0,537,8; ATTACH_OK,t=1,pid=1,fork=0,method=reread,seed=null,state=null,counters=B0)` |
| `r3` | `(b,a,0,769,8; FRONTIER,t=20,counters=B0)` |

TRANS has no wire fork. Its absent fork does not assert the provisional generation.

For boot-c files, the corresponding records have boot `c` in their identities. None is emitted in these complete cases because of capture ambiguity. `N1` retains the `n` record contents; the Q variants retain the `q` contents. Qualification changes do not change record identities or contents.

The valid M sample at `(b,m,0,292,0)` is always omitted from the exact array. It still participates in validation and conflicts.

In the tables:

- `A[0:k]` means the ordered array `[a0,…,ak]`; similarly for `N`, `Q`, and `X`.
- `S(f,o,r;W)` uses the verified physical file `f=(boot,capture,file_no)`. Scalar offsets mean `(offset,null)`.
- **S** in diagnostics expands to every diagnostic proving the listed stops, with the complete witness sets shown, plus the applicable link evidence below.
- All unlisted codec stops, delivery failures, conflicts and replay stops are absent.

**Replay-state abbreviations**

These fully specify the replay effects referenced by each case.

| State | Baseline, LIFE, certificates, publication and coverage |
|---|---|
| `J(L)` | Baseline `B0`; retain precisely the LIFE records in `L`; no accepted FRONTIER, `F=HW=absent`, no certified run or coverage, no lifecycle uncertainty. Any retained attachment without a FRONTIER supplies no coverage. |
| `T10(L)` | Baseline `B0`; retain LIFE `L`; attachment `a1` and FRONTIER 10 survive; `F=HW=10`; pid-1 coverage `[[0,10]]`; no lifecycle uncertainty. |
| `T20(L)` | Baseline `B0`; retain LIFE `L`; attachment `a1` and FRONTIERs 10 and 20 survive; `F=HW=20`; pid-1 coverage `[[0,20]]`; no lifecycle uncertainty. |
| `V0(L)` | Apply `a4`: baseline `B0`; retain LIFE `L`; revoke FRONTIER 20; FRONTIER 10 and attachment 0’s portion through 10 survive; `F=10, HW=20`; pid-1 coverage `[[0,10]]`; freeze that run permanently. Lifecycle uncertainty at 30 follows from nonnull `invalid_from`, despite unchanged counters. |
| `V1(L)` | As `V0(L)`, applying `v4` and ending with baseline `B1`. |
| `VU(L)` | As `V0(L)`, applying `vu4` and ending with unknown baseline. Recovery remains unavailable while the baseline is unknown. |
| `C1(L)` | Consume `w4` only as `VOID(30,B1,ρ,null)`. Baseline `B1`; retain LIFE `L`; FRONTIERs 10 and 20 survive; `F=HW=20`; freeze pid-1 coverage at `[[0,20]]`. FRONTIER 30 is never published. Slot-4-only loss with null `invalid_from` causes no lifecycle uncertainty. |
| `I0(L)` | Unqualified: baseline `B0`; retain LIFE `L`; ignore all ATTACH_OK/FRONTIER/END certificates; `F=HW=absent`, coverage empty, no lifecycle uncertainty. |
| `IV1(L)` | Unqualified: ignore certificates, but apply explicit `v4`; baseline `B1`; retain LIFE `L`; lifecycle uncertainty at 30; `F=HW=absent`, coverage empty. No previously accepted FRONTIER exists to revoke. |
| `Ø` | No admitted root: no initialized replay baseline, retained LIFE, certificates or coverage. |
| `U` | Do not replay capture a; expose none of its LIFE, certificates, coverage or metrics. Header declarations remain available as codec evidence. |

Every retained generation without the pid-1 certified run has empty coverage. All represented waits are `[]` and all terminal open waits are `null`. Retained LIFE has life `[start,null]` from its record above.

In states with `v4`, the Q generations have fresh forks 40, appended after the uncertainty VOID at 30 with a readable baseline, so their LIFE is retained. They receive no coverage because they have no attachment.

**Completed expected tables, cases 1–16**

| # | Codec records | Normalized codec stops | Diagnostics / explicit replay effect | Aggregate | Short derivation |
|---:|---|---|---|---|---|
| 1 | `A[0:2]` | `S((b,a,0),747,T;{At3})` | **S** + `d(At1,305,T)`; `T10({a0})` | FAILED | At3 supplies the longer prefix; its terminal truncation remains eligible. |
| 2 | `A[0:3]` | `[]` | `d(At1,305,T)` suppressed; `T20({a0})` | PASS | Clean A supplies the full prefix. |
| 3 | `A[0:3]` | `S((b,a,0),957,T;{Ag})` | **S** + `d(At1,305,T)`; `T20({a0})` | FAILED | Ag is longest but ends with a one-byte rejected tail. |
| 4 | `A[0:3]` | `[]` | `d(Acrc2,537,CRC)` suppressed; `T20({a0})` | PASS | Acrc2 contributes no validated conflicting block 2; clean A wins by prefix length. |
| 5 | `A[0:3]` | `[]` | `d(Ac2t1,305,T)` suppressed; `T20({a0})` | PASS | Ac2t1 is byte-identical to At1; its intended later change is absent. |
| 6 | `A[0:1]` | `S((b,a,0),537,BC;{(A,Ac2t3)})` | **S** + `d(Ac2t3,747,T)`; `J({a0})` | FAILED | Both copies validate different block-2 bytes; exclude that entire block. |
| 7 | `[]` | `S((b,a,0),0,HC;{(A,Ah)})` | **S**; `Ø` | FAILED | Immutable creation metadata differs in validated headers. |
| 8 | `A[0:3] ++ [n0,n1]` | `S((b,a,1),465,T;{Nt2})` | **S** + `d(Ag,957,T)` suppressed; `T20({a0,n0,n1})` | FAILED | A is a clean predecessor witness despite Ag’s dirty tail; Nt2’s own truncation stops file 1. |
| 9 | `A[0:2]` | `S((b,a,0),747,T;{At3})`; `S((b,a,1),0,PU;{N})` | **S**; `T10({a0})` | FAILED | At3 cannot witness N’s declared predecessor end 957. |
| 10 | `A[0:3]` | `[]` | `d(At1,305,T)`, `d(At3,747,T)` suppressed; `T20({a0})` | PASS | Clean A extends both torn prefixes. |
| 11 | `a:A[0:2]`; `x:X[0:3]` | `S((b,a,0),747,T;{At3})` | **S** + `d(At1,305,T)`; a=`T10({a0})`; x=`J({x0,x1,x2,x3})` | FAILED | Capture x remains independently usable. |
| 12 | `A[0:3]` | `[]` | No failures; `T20({a0})` | PASS | Duplicate A, advisory flags and exact footer do not duplicate or conflict records. |
| 13 | `A[0:2]` | `S((b,a,0),747,CRC;{Acrc3})` | **S** + `d(At3,747,T)` suppressed by reason rank; `T10({a0})` | FAILED | Both maximal prefixes end at 747; CRC outranks T. |
| 14 | `[m0,m2,m3]` | `[]` | `d(Mt3,489,T)` suppressed; `J({m2})`; both TRANS ignored outside eligible runs | PASS | Sample omitted; delayed LIFE retained; no attachment exists. |
| 15 | `[m0]` | `S((b,m,0),292,BC;{(M,Mc1t3)})` | **S** + `d(Mc1t3,489,T)`; `J(∅)`; m0 ignored outside eligible runs | FAILED | A differing validated sample block causes conflict even though samples are omitted. |
| 16 | `A[0:3]` | `S((b,a,0),957,SC;{(A+,N)})` | **S**; `T20({a0})`; no VOID applied | FAILED | N authenticates end 957 while A+ validates an additional block. SC excludes a4 and N. |

**Completed expected tables, cases 17–31**

| # | Codec records | Normalized codec stops | Diagnostics / explicit replay effect | Aggregate | Short derivation |
|---:|---|---|---|---|---|
| 17 | `A[0:3]` | `[]` | Unassigned `d(Z,0,IH)` for header CRC; capture a passes; `T20({a0})` | FAILED | Z supplies no trusted identity; its failure cannot become a capture stop. |
| 18 | `A[0:3]` | `S((b,a,0),957,SC;{(A+,N)})` | **S**; `T20({a0})`; no VOID applied; no PU | FAILED | A clean predecessor ending at D is unnecessary for the authenticated sealing contradiction. |
| 19 | `A[0:3]` | `S((b,a,0),957,T;{Ag})`; `S((b,a,1),0,PU;{N})` | **S**; `T20({a0})` | FAILED | Ag’s dirty tail disqualifies it as N’s clean witness; the tail proves no SC. |
| 20 | `[a0]` | `S((b,a,0),305,T;{At1})`; `S((b,a,1),0,PU;{N})`; `S((b,a,2),465,BC;{(L,Lc2t3)})` | **S** + `d(Lc2t3,577,T)`; `J({a0})` | FAILED | Earlier root cutoff excludes N/L records, but independent inspection still finds L’s later BC. |
| 21 | `A[0:3]` | `S((b,a,0),957,T;{Ag,Ag′})` | **S**; `T20({a0})` | FAILED | Rejected tail bytes do not establish BC; both deliveries witness the same terminal T. |
| 22 | `A[0:3] ++ [a4]` | `[]` | No failures; `V0({a0})` | PASS | Compatible longer A+ retains the VOID and retracts FRONTIER-20 results. |
| 23 | `A[0:3]` | `S((b,a,2),0,PU;{L})` | **S**; `T20({a0})` | FAILED | File 1 is absent; L cannot bridge the gap. |
| 24 | `[]` | `S((b,a,1),0,PU;{N})` | **S**; `Ø` | FAILED | N cannot be reinterpreted as a root. |
| 25 | `A[0:3]` | `S((b,a,0),957,T;{Aft})` | **S**; `T20({a0})` | FAILED | A 17-byte footer fragment is an incomplete block header, not a valid footer. |
| 26 | `A[0:3]` | `[]` | `d(Aft,957,T)` suppressed; `T20({a0})` | PASS | Clean A suppresses the equal-end terminal failure. |
| 27 | `A[0:3] ++ [a4]` | `S((b,a,1),0,PU;{Nb})` | **S**; `V0({a0})` | FAILED | Wrong digest prevents both a clean witness and SC; a4 remains admitted. |
| 28 | `A[0:3] ++ [a4]` | `S((b,a,1),0,PU;{Ni})` | **S**; `V0({a0})` | FAILED | Referenced D=956 is not a validated boundary; no SC excludes a4. |
| 29 | `[a0]` | `S((b,a,0),(305,8),RF;{Au,Auc3})` | **S** + BC diagnostic at 747 with pair `{(Au,Auc3)}`; `J({a0})`; no synthetic VOID | FAILED | Unknown kind 255 stops at its record offset. Continued inspection finds the later BC. |
| 30 | `[r0,r1,r2]` | `S((b,a,0),769,BC;{(R,Rc3)})` | **S**; separate replay stop at `(b,a,0,537,8)` for overlapping r2; `J({r0})`; no synthetic VOID | FAILED | Codec admits both attachments; replay stops before the second, earlier than the codec BC. |
| 31 | `[]` | `[]` | Unassigned `d(Z,0,IH)` for header CRC; no verified capture or LIFE | FAILED | Invalid identity fields cannot attribute a normalized stop. |

**Completed expected tables, cases 32–50**

The `U` witnesses below expand to `(delivery,boot,file_no,(0,null))`:

- `A → (A,b,0,(0,null))`
- `A2 → (A2,c,0,(0,null))`
- `N2 → (N2,c,1,(0,null))`
- `A2t0 → (A2t0,c,0,(0,null))`

Every ambiguity below has observed boot set `{b,c}`.

| # | Codec records | Normalized codec stops | Diagnostics / explicit replay effect | Aggregate | Short derivation |
|---:|---|---|---|---|---|
| 32 | `[]` | `[]` | `U(a;{A,A2})`; replay=`U` | FAILED | Distinct boots reuse capture a; neither root wins. |
| 33 | `[]` | `S((c,a,1),0,PU;{N2})` | **S** + `U(a;{A,N2})`; replay=`U`; no baseline check | FAILED | N2’s validated header proves reuse despite its absent predecessor. |
| 34 | `x:X[0:3]` | `[]` | `U(a;{A,A2})`; a=`U`; x=`J({x0,x1,x2,x3})` | FAILED | Withhold a entirely; x passes independently. |
| 35 | `[]` | `[]` | `U(a;{A,A2})`; replay=`U` | FAILED | Duplicate A2 adds neither a witness nor records. |
| 36 | `A[0:3] ++ [v4] ++ Q[0:3]` | `[]` | No failures; Q baseline matches expected `B1`; `V1({a0,q0,q1,q2,q3})` | PASS | Explicit VOID determines the successor baseline and revokes FRONTIER 20. |
| 37 | `A[0:3] ++ [v4] ++ Q[0:3]` | `[]` | `P(Q0;expected=B1,declared=B0)` at `(b,a,1,0,null)`; `V1({a0})` | FAILED | Codec linkage succeeds; replay excludes all q records at Q0’s descriptor. |
| 38 | `A[0:3] ++ N[0:3]` | `[]` | No failures; N expected=`B0`, declared=`B0`; `T20({a0,n0,n1,n2,n3})` | PASS | Ordinary clean rotation preserves baseline and coverage. |
| 39 | `A[0:3] ++ [v4] ++ Q[0:3]` | `[]` | No failures; Q expected=`B1`, declared=`B1`; `V1({a0,q0,q1,q2,q3})` | PASS | A is a shorter compatible copy; V is retained once and its VOID cannot be suppressed. |
| 40 | `A[0:3]` | `S((b,a,0),957,SC;{(V,N1)})` | **S**; `T20({a0})`; no v4 or baseline-mismatch replay effect | FAILED | N1 authenticates A’s end while V extends it. Baseline differences do not defeat SC. |
| 41 | `A[0:3] ++ N[0:3]` | `[]` | `P(N1;expected=B0,declared=B1)` at `(b,a,1,0,null)`; `T20({a0})` | FAILED | A is a valid codec witness; N1’s declaration fails only during replay. |
| 42 | `A[0:3]` | `[]` | Unassigned `d(Z2,0,IH)`; no U; capture a passes; `T20({a0})` | FAILED | Z2’s invalid header cannot establish boot c. |
| 43 | `A[0:3] ++ [vu4]` | `[]` | No failures; Tu expected=`null`, declared=`null`; `VU({a0})` | PASS | Explicit null counters report unknown loss; header-only Tu correctly restates it. |
| 44 | `A[0:3] ++ [vu4]` | `[]` | `P(T0;expected=null,declared=B0)` at `(b,a,1,0,null)`; `VU({a0})` | FAILED | Known zero differs from unknown, even in an empty successor. |
| 45 | `A[0:3] ++ [w4] ++ Q[0:3]` | `[]` | Counter-loss diagnostic at `(957,8)`; Qw expected=`B1`, declared=`B1`; `C1({a0,q0,q1,q2,q3})` | PASS | Readable FRONTIER counter change becomes VOID, not RF or FRONTIER 30. |
| 46 | `A[0:3] ++ [w4] ++ Q[0:3]` | `[]` | Same counter-loss diagnostic; `P(Qw0;expected=B1,declared=B0)` at `(b,a,1,0,null)`; `C1({a0})` | FAILED | The consumed-as-VOID record changes baseline before the successor check. |
| 47 | `[]` | `S((c,a,0),193,T;{A2t0})` | **S** + `U(a;{A,A2t0})`; replay=`U` | FAILED | A complete boot-c header proves reuse even with zero validated blocks. |
| 48 | `A[0:3] ++ [w4] ++ Q[0:3]` | `[]` | Ignore all certificate effects, including w4’s differing vector; Qwi0 expected=`B0`, declared=`B0`; `I0({a0,q0,q1,q2,q3})` | PASS | Unqualified certificates cannot update baseline or become counter-loss VOID. |
| 49 | `A[0:3] ++ [w4] ++ Q[0:3]` | `[]` | Same ignored-certificate handling; `P(Qwi1;expected=B0,declared=B1)` at `(b,a,1,0,null)`; `I0({a0})` | FAILED | Ignoring certificates does not exempt successor baseline continuity. |
| 50 | `A[0:3] ++ [v4] ++ Q[0:3]` | `[]` | Ignore certificate effects; apply explicit v4; Qvi expected=`B1`, declared=`B1`; `IV1({a0,q0,q1,q2,q3})` | PASS | Explicit VOID still changes baseline and lifecycle eligibility in an unqualified capture. |

In cases 48–49, certificate diagnostics may identify w4’s `B1` versus running `B0`, but the record is ignored: it creates neither a loss event nor a baseline change.

**Independent prefix validation and source provenance**

This ledger fixes each delivery’s independently validated exclusive end `E(d)` and terminal classification.

| Deliveries | `E(d)` | Terminal classification |
|---|---:|---|
| `A,Af,Ah,Ao` | 957 | Clean EOF, or exact footer for Ao |
| `A+,V,Vi` | 1179 | Clean EOF |
| `Vu` | 1059 | Clean EOF |
| `W,Wi` | 1167 | Clean EOF |
| `Au,Auc3` | 957 | Clean EOF; independent RF at `(305,8)` |
| `A2` | 957 | Clean EOF |
| `At1,Ac2t1` | 305 | T at 305; these names identify identical bytes |
| `At3,Ac2t3` | 747 | T at 747 |
| `Acrc2` | 537 | CRC at 537 |
| `Acrc3` | 747 | CRC at 747 |
| `Ag,Ag′,Aft` | 957 | T at 957 |
| `A2t0` | 193 | T at 193 |
| `N,N1,N2,Nb,Ni,Q,Q0,Qw,Qw0,Qwi0,Qwi1,Qvi` | 689 | Clean EOF |
| `Nt2` | 465 | T at 465 |
| `L` | 689 | Clean EOF |
| `Lc2t3` | 577 | T at 577 |
| `X` | 641 | Clean EOF |
| `M` | 580 | Clean EOF |
| `Mt3,Mc1t3` | 489 | T at 489 |
| `R,Rc3` | 979 | Clean EOF |
| `Tu` | 121 | Clean header-only EOF |
| `T0` | 241 | Clean header-only EOF |
| `Z,Z2` | Unavailable | IH; no verified physical-file group |

D10 source provenance is the set of all deliveries attaining the group’s maximum E, even if a later decision excludes their records.

Here `a0`, `a1`, `a2` in the **group** column mean physical file groups `(b,a,0)`, `(b,a,1)`, `(b,a,2)`, not record names. `c0,c1` mean `(c,a,0)`, `(c,a,1)`.

| # | Source-provenance sets by physical-file group |
|---:|---|
| 1 | a0=`{At3}` |
| 2 | a0=`{A}` |
| 3 | a0=`{Ag}` |
| 4 | a0=`{A}` |
| 5 | a0=`{A}` |
| 6 | a0=`{A}` |
| 7 | No selectable source: HC; independently A and Ah each reach 957 |
| 8 | a0=`{A,Ag}`; a1=`{Nt2}` |
| 9 | a0=`{At3}`; a1=`{N}` |
| 10 | a0=`{A}` |
| 11 | a0=`{At3}`; x0=`{X}` |
| 12 | a0=`{A,Af,Ao}` |
| 13 | a0=`{At3,Acrc3}` |
| 14 | m0=`{M}` |
| 15 | m0=`{M}` |
| 16 | a0=`{A+}`; a1=`{N}` |
| 17 | a0=`{A}`; Z unassigned |
| 18 | a0=`{A+}`; a1=`{N}` |
| 19 | a0=`{Ag}`; a1=`{N}` |
| 20 | a0=`{At1}`; a1=`{N}`; a2=`{L}` |
| 21 | a0=`{Ag,Ag′}` |
| 22 | a0=`{A+}` |
| 23 | a0=`{A}`; a2=`{L}` |
| 24 | a1=`{N}` |
| 25 | a0=`{Aft}` |
| 26 | a0=`{A,Aft}` |
| 27 | a0=`{A+}`; a1=`{Nb}` |
| 28 | a0=`{A+}`; a1=`{Ni}` |
| 29 | a0=`{Au,Auc3}` |
| 30 | a0=`{R,Rc3}` |
| 31 | None; Z unassigned |
| 32 | a0=`{A}`; c0=`{A2}` |
| 33 | a0=`{A}`; c1=`{N2}` |
| 34 | a0=`{A}`; c0=`{A2}`; x0=`{X}` |
| 35 | a0=`{A}`; c0=`{A2}` |
| 36 | a0=`{V}`; a1=`{Q}` |
| 37 | a0=`{V}`; a1=`{Q0}` |
| 38 | a0=`{A}`; a1=`{N}` |
| 39 | a0=`{V}`; a1=`{Q}` |
| 40 | a0=`{V}`; a1=`{N1}` |
| 41 | a0=`{A}`; a1=`{N1}` |
| 42 | a0=`{A}`; Z2 unassigned |
| 43 | a0=`{Vu}`; a1=`{Tu}` |
| 44 | a0=`{Vu}`; a1=`{T0}` |
| 45 | a0=`{W}`; a1=`{Qw}` |
| 46 | a0=`{W}`; a1=`{Qw0}` |
| 47 | a0=`{A}`; c0=`{A2t0}` |
| 48 | a0=`{Wi}`; a1=`{Qwi0}` |
| 49 | a0=`{Wi}`; a1=`{Qwi1}` |
| 50 | a0=`{Vi}`; a1=`{Qvi}` |

For case 7, D10 applies only to groups without HC. “No selectable source” is therefore inapplicability, not an unresolved choice between A and Ah.

**Predecessor candidates, clean witnesses and admission**

Every non-root delivery is covered below. All other cases contain only roots or invalid headers, so they have no predecessor candidate or witness requirement.

A witness means the complete D13 witness, including clean ending, exact D, digest and static fields. SC pairs retain predecessor/successor roles.

| Case | Successor(s); declared predecessor | Candidate group | Clean witness set | Link/admission decision and complete material evidence |
|---:|---|---|---|---|
| 8 | Nt2; file 0, D=957, A digest | `{A,Ag}` | `{A}` | A passes every static/digest check. Ag validates through D with the same digest but has rejected tail bytes. Link admitted; Nt2 contributes records before its T at 465. |
| 9 | N; file 0, D=957, A digest | `{At3}` | `∅` | At3 validates only through 747 and is shorter than D. No authenticated D boundary or extension; PU. |
| 16 | N; file 0, D=957, A digest | `{A,A+}` | `{A}` | A is a clean witness. A+ matches static fields and digest through validated boundary 957, then validates a whole extra block. SC pair `(A+,N)` rejects the link; no PU. |
| 18 | N; file 0, D=957, A digest | `{A+}` | `∅` | Same SC proof from A+; its extension prevents it being a clean witness. No PU. |
| 19 | N; file 0, D=957, A digest | `{Ag}` | `∅` | D and digest match, static fields match, but rejected tail bytes violate cleanliness. No validated extension beyond D; PU, no SC. |
| 20 | N; file 0, D=957, A digest | `{At1}` | `∅` | At1 validates only through 305 and is shorter than D; PU. |
| 20 | L and Lc2t3; file 1, D=689, N digest | `{N}` | `{N}` for each | Both links pass locally. Earlier file-0 cutoff/file-1 PU prevents chain admission; no additional local link failure. File-2 BC remains independently reportable. |
| 23 | L; file 1, D=689, N digest | `∅` | `∅` | Candidate group absent; PU. |
| 24 | N; file 0, D=957, A digest | `∅` | `∅` | Candidate group absent; PU. |
| 27 | Nb; file 0, D=957, bit-flipped A digest | `{A+}` | `∅` | Static fields and boundary match, but digest through D does not. A+ also extends beyond D. Wrong digest prevents SC; PU. |
| 28 | Ni; file 0, D=956, digest through 956 | `{A+}` | `∅` | Static fields and declared prefix digest match, but 956 is inside a block, not a validated boundary; predecessor ends at 1179. Neither clean witness nor SC; PU. |
| 33 | N2; boot c file 0, D=957, A2 digest | `∅` | `∅` | A belongs to boot b and is excluded from the candidate group. PU persists alongside U. |
| 36 | Q; file 0, D=1179, V digest | `{V}` | `{V}` | Codec admitted; expected/declared baseline `B1/B1`. |
| 37 | Q0; file 0, D=1179, V digest | `{V}` | `{V}` | Codec admitted; replay stops on `B1/B0`. |
| 38 | N; file 0, D=957, A digest | `{A}` | `{A}` | Codec and replay admitted; `B0/B0`. |
| 39 | Q; file 0, D=1179, V digest | `{A,V}` | `{V}` | A is short; V is the actual clean witness. Codec and replay admitted; `B1/B1`. Duplicate V contributes once. |
| 40 | N1; file 0, D=957, A digest | `{V}` | `∅` | Static fields and digest through boundary 957 match; V has a validated extension. SC pair `(V,N1)` rejects link. Baseline equality is excluded from this test. |
| 41 | N1; file 0, D=957, A digest | `{A}` | `{A}` | Codec admitted; replay stops on `B0/B1`. |
| 43 | Tu; file 0, D=1059, Vu digest | `{Vu}` | `{Vu}` | Header-only successor admitted; `null/null`. |
| 44 | T0; file 0, D=1059, Vu digest | `{Vu}` | `{Vu}` | Codec admitted; replay stops on `null/B0`, despite having no records. |
| 45 | Qw; file 0, D=1167, W digest | `{W}` | `{W}` | Codec admitted; consumed-as-VOID w4 establishes `B1/B1`. |
| 46 | Qw0; file 0, D=1167, W digest | `{W}` | `{W}` | Codec admitted; replay stops on `B1/B0`. |
| 48 | Qwi0; file 0, D=1167, Wi digest | `{Wi}` | `{Wi}` | Both qualified=false; ignored w4 leaves `B0/B0`; admitted. |
| 49 | Qwi1; file 0, D=1167, Wi digest | `{Wi}` | `{Wi}` | Static qualification agrees; codec admitted. Replay stops on `B0/B1`. |
| 50 | Qvi; file 0, D=1179, Vi digest | `{Vi}` | `{Vi}` | Both qualified=false; explicit VOID establishes `B1/B1`; admitted. |

All admitted roots above remain subject to their normalized stops and capture ambiguity. In ambiguous cases, independent codec validation and normalization continue, but logical records are withheld under D2a.

**Capture outcomes**

The aggregate column follows D0. More specifically:

- Cases **17 and 42**: capture a **PASS**, aggregate **FAILED** because of an unassigned invalid-header delivery.
- Case **31**: no verified capture outcome; aggregate **FAILED** because of the unassigned invalid-header delivery.
- Cases **11 and 34**: capture x **PASS**, capture a **FAILED**.
- Every other case’s sole verified capture has the same PASS/FAILED outcome as its aggregate.
- An empty array in cases 7, 24, 31–33, 35 and 47 is accompanied by its explicit failure outcome.

**Incremental accumulation and retraction**

D1 requires fresh reconciliation of every accumulated submultiset. These rules cover successor-first, reversed-chain and interleaved-capture arrivals without assigning precedence to arrival order.

| Cases | Required incremental behavior |
|---|---|
| 1–5, 8–14, 20, 25–26 | Compatible longer deliveries can extend the validated prefix and suppress earlier terminal-stop candidates. Independent failure diagnostics remain. A shorter/torn/CRC-bad delivery cannot remove already validated positions. |
| 6 | Adding the conflicting copy retracts records from block 537 onward, including any earlier exposed FRONTIER-derived coverage. |
| 7 | Adding the incompatible validated header retracts the entire physical-file group and all its derived evidence. |
| 15 | The sample-block conflict retracts from block 292, including later LIFE, despite omission of the sample from the exact array. |
| 16, 18, 40 | Once successor and extending predecessor coexist, SC cuts at 957. If the extension’s VOID was previously replayed, recompute from the newly admitted prefix: exclude that VOID and its effects. |
| 19 | Adding dirty Ag cannot supply a clean predecessor witness for N. Adding N retains Ag’s root prefix but produces PU. |
| 21 | Both rejected tails produce one T stop with both witnesses; they cannot create BC. |
| 22, 36–37, 39, 43–44, 50 | Adding the retained later explicit VOID revokes all certificates at/after 20. Previously exposed FRONTIER-20 coverage must retract to 10 in qualified captures. Shorter A cannot suppress the VOID. |
| 23–24 | A successor without its immediate predecessor remains unavailable. No suffix becomes a root. |
| 27–28 | Adding Nb/Ni produces PU without excluding the predecessor’s VOID. Its revocations remain effective. |
| 29 | RF cuts at `(305,8)` regardless of delivery order. Later conflict inspection still reports BC at 747. |
| 30 | Codec BC cuts at 769; replay independently cuts earlier at overlapping attachment `(537,8)`. A previously exposed frontier from either complete R copy is retracted. |
| 32–35, 47 | The second validated boot retracts **all** capture-a records, LIFE, certificates, coverage and metrics. The complete ambiguity witness set grows with distinct valid-header deliveries; duplicates add nothing. |
| 17, 31, 42 | Invalid headers never establish capture or boot identity. Adding them can fail the aggregate without retracting usable A evidence. |
| 37, 41, 44, 46, 49 | A reachable mismatch retains the preceding replay prefix, including prior loss/revocation, but excludes successor records. It never changes codec records or normalization. |
| 45–46 | Adding w4 freezes the existing run at FRONTIER 20 and updates baseline to B1; it neither publishes 30 nor revokes 20. |
| 48–49 | Ignored unqualified certificates do not move baseline, so adding w4 creates no loss or coverage effect. |

For every successor-first arrival, first report the locally applicable absent-predecessor PU. When a predecessor arrives, recompute candidates and witnesses: remove PU if a clean witness is established, replace it with SC when an authenticated extension proves sealing contradiction, or retain PU for short/dirty/wrong-digest/nonboundary alternatives. Later intrinsic codec diagnostics remain reportable even behind earlier cutoffs.

A baseline mismatch is evaluated only after its descriptor becomes reachable. Thus case 40 reports SC, **not** a speculative mismatch between the retained prefix’s B0 and N1’s B1; case 33 performs no replay under ambiguity.

**Undetermined values**

No requested record array, provenance set, predecessor candidate set, witness set, admission decision, normalized codec stop, baseline comparison, capture outcome or aggregate status remains undetermined.

The following finer values are not fixed by the supplied clauses:

| Case(s) / value | Clause | What remains undetermined |
|---|---|---|
| 45–46: reason of the semantic VOID consumed from w4 | K §5, readable-counter exception | The rule requires an “opaque diagnostic reason” but supplies no exact string. It is denoted `ρ` above. Its timestamp, counters, null `invalid_from`, baseline and lifecycle effects are determined. |
| All diagnostic-bearing cases: literal diagnostic messages/serialization; particularly case 30’s semantic-stop label and cases 48–49’s ignored-certificate diagnostics | D3, D20; CODEC §9 | Codec reason enums and required evidence are fixed; exact diagnostic prose and a serialization schema are not. The tables specify the required meanings and witnesses without inventing message text. |
| Any requested numerical query metric or query-selected projection | K §5 | No target/query is supplied. Retained LIFE, candidate coverage, waits and open state are determined above; a query-specific metric cannot be selected or calculated from an unspecified query. |