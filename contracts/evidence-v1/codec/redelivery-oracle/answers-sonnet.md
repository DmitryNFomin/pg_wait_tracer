# Answers (Sonnet), redelivery cases

Notation. Physical identity `(boot,capture,file_no,block_offset,record_offset)`, shown as `b.c.f@block/rec`.
File names: A0=(b,a,0), A1=(b,a,1), A2=(b,a,2), M0=(b,m,0), X0=(b,x,0).
Records of A: a0=A0@193/8 LIFE(1); a1=A0@305/8 ATTACH(0); a2=A0@537/8 FRONTIER(10); a3=A0@747/8 FRONTIER(20); a4=A0@957/8 VOID(t=30,zero,"x",invalid_from=20).
Records of M: m0=M0@201/0 TRANS(t=1,pid=1,null->"w",dur=0); m2=M0@377/8 LIFE(1); m3=M0@489/0 TRANS(t=3,pid=1,"w"->null,dur=2). The sample at 292 is omitted, with no renumbering.
`S(F,o,r;W)` = normalized codec stop. `d(x,o,r)` = diagnostic. `Src` = source provenance of the record prefix.
Replay column: "no replay stop" means the admitted stream replays without a physical replay stop. The text does not specify any other replay effect for that case, and I invented none.

## Table

| # | Input | Codec records | Normalized codec stops | Diagnostics / explicit replay effect |
|--:|---|---|---|---|
| 1 | At1,At3 | a0,a1,a2 (Src {At3}) | S(A0,747,T;{At3}) | d(At1,305,T), d(At3,747,T); S underlying; no replay stop |
| 2 | A,At1 | a0,a1,a2,a3 (Src {A}) | none | d(At1,305,T) |
| 3 | Ag,At1 | a0..a3 (Src {Ag}) | S(A0,957,T;{Ag}) | d(Ag,957,T), d(At1,305,T) |
| 4 | A,Acrc2 | a0..a3 (Src {A}) | none | d(Acrc2,537,CRC) |
| 5 | A,Ac2t1 (=At1) | a0..a3 (Src {A}) | none | d(At1,305,T) |
| 6 | A,Ac2t3 | a0,a1 (Src {A,Ac2t3}, identical bytes) | S(A0,537,BC;{(A,Ac2t3)}) | BC pair diagnostic at 537, d(Ac2t3,747,T) (suppressed, E<Emax) |
| 7 | A,Ah | none | S(A0,0,HC;{(A,Ah)}) | HC pair diagnostic |
| 8 | A,Ag,Nt2 | a0..a3 (Src {A,Ag}), n0,n1 (Src {Nt2}) | S(A1,465,T;{Nt2}) | d(Ag,957,T), d(Nt2,465,T); link N: witness A, admitted; no replay stop |
| 9 | At3,N | a0,a1,a2 (Src {At3}) | S(A0,747,T;{At3}); S(A1,0,PU;{N}) | d(At3,747,T), d(N,0,PU) with predecessor ref (A0,D=957,digest) and check result "no delivery reaches D (At3 E=747)" |
| 10 | At1,At3,A | a0..a3 (Src {A}) | none | d(At1,305,T), d(At3,747,T) |
| 11 | At1,X,At3 | A0: a0,a1,a2 (Src {At3}); X0: x0..x3 (Src {X}) | S(A0,747,T;{At3}) | d(At1,305,T), d(At3,747,T); captures a and x independent, no X stop |
| 12 | A,A,Af,Ao | a0..a3 (Src {A,Af,Ao}) | none | none (no failures; A,A is one delivery; flag and footer differences are inert) |
| 13 | At3,Acrc3 | a0,a1,a2 (Src {At3,Acrc3}) | S(A0,747,CRC;{Acrc3}) | d(Acrc3,747,CRC), d(At3,747,T) (suppressed, loses on rank) |
| 14 | M,Mt3 | m0,m2,m3 (Src {M}) | none | d(Mt3,489,T) |
| 15 | M,Mc1t3 | m0 (Src {M,Mc1t3}) | S(M0,292,BC;{(M,Mc1t3)}) | BC pair diagnostic at 292, d(Mc1t3,489,T) (suppressed) |
| 16 | A,A+,N | a0..a3 (cut at 957; Src {A+}) | S(A0,957,SC;{(pred A+,succ N)}) | SC pair diagnostic; A is an actual clean witness for N, but the link is rejected by SC; no PU; a4 and n0..n3 excluded |
| 17 | A,Z | a0..a3 (Src {A}) | none | d(Z,0,IH); Z has no trusted identity and is never a stop (see U1) |
| 18 | A+,N | a0..a3 (cut at 957; Src {A+}) | S(A0,957,SC;{(pred A+,succ N)}) | SC pair diagnostic; no witness (A+ has bytes beyond D); no PU |
| 19 | Ag,N | a0..a3 (Src {Ag}) | S(A0,957,T;{Ag}); S(A1,0,PU;{N}) | d(Ag,957,T), d(N,0,PU) with predecessor ref and result "Ag dirty tail 00 beyond D, digest matches, no clean witness, no complete block beyond D" |
| 20 | At1,N,L,Lc2t3 | a0 (Src {At1}) | S(A0,305,T;{At1}); S(A1,0,PU;{N}); S(A2,465,BC;{(L,Lc2t3)}) | d(At1,305,T), d(N,0,PU), BC pair at 465, d(Lc2t3,577,T) (suppressed); L has witness N, so no PU for L; capture cutoff at 305 |
| 21 | Ag,Ag′ | a0..a3 (Src {Ag,Ag′}) | S(A0,957,T;{Ag,Ag′}) | d(Ag,957,T), d(Ag′,957,T) |
| 22 | A,A+ | a0,a1,a2,a3,a4 (Src {A+}) | none | none; the retained VOID a4 must be incorporated by replay (see U2) |
| 23 | A,L | a0..a3 (Src {A}) | S(A2,0,PU;{L}) | d(L,0,PU) with predecessor ref (file 1, D=689, digest), no file-1 delivery |
| 24 | N | none | S(A1,0,PU;{N}) | d(N,0,PU) with predecessor ref (A0,D=957), no predecessor delivery; not read as a root |
| 25 | Aft | a0..a3 (Src {Aft}) | S(A0,957,T;{Aft}) | d(Aft,957,T) |
| 26 | A,Aft | a0..a3 (Src {A,Aft}) | none | d(Aft,957,T) |
| 27 | A+,Nb | a0..a4 (Src {A+}) | S(A1,0,PU;{Nb}) | d(Nb,0,PU): predecessor A+ not a witness (extra block), digest mismatch so no SC proof; VOID a4 incorporated |
| 28 | A+,Ni | a0..a4 (Src {A+}) | S(A1,0,PU;{Ni}) | d(Ni,0,PU): D=956 is not a block boundary or end of any delivery, so no witness and no SC; VOID a4 incorporated |
| 29 | Au,Auc3 | a0 (Src {Au,Auc3}) | S(A0,(305,8),RF;{Au,Auc3}) | d(Au,(305,8),RF), d(Auc3,(305,8),RF); BC at (747,null) between Au and Auc3 is a suppressed diagnostic (see U3) |
| 30 | R,Rc3 | r0,r1,r2 (Src {R,Rc3}; r3 excluded by BC) | S(A0,769,BC;{(R,Rc3)}) | BC pair diagnostic. Replay stop (separate domain) at (537,8), the overlapping ATTACH r2; effective cutoff = (537,8); r0, r1 replayed; r2, r3 and later not replayed (see U4) |
| 31 | Z | none | none | d(Z,0,IH); failed decode, not an empty success |

## Derivations

Block sizes from the starts: A blocks 112/232/210/210; D=957.

1. At1: E=305, tail T at 305. At3: E=747, tail T at 747. Emax=747, attained by At3 only, which has no clean EOF. So one terminal candidate T at 747 with W={At3} (clause 12). At1's T is E<Emax, so diagnostic only. Prefixes are nested, so there is no BC. Cut before 747 gives a0..a2.
2. A is clean at Emax=957, so no terminal stop (clause 12). At1 is a diagnostic only. No successors, so no PU.
3. Ag attains 957 with a one-byte tail. A footer needs at least four exact bytes, so this is T. No clean delivery attains E, so T at 957 W={Ag}. At1's T is suppressed (E<Emax). Records are all four blocks.
4. Acrc2: blocks 0 and 1 validated, block 2 complete with bad CRC, so CRC at 537. E=537<957, so not a candidate. Acrc2's validated prefix is a prefix of A, so no BC (rejected blocks give no conflict evidence). A is clean.
5. Ac2t1 is byte-identical to At1, so this is case 2.
6. Both validate through block 2 (Ac2t3 E=747). Headers match. Block 2 differs, so BC at 537 (clause 9). The cutoff excludes block 2 and later, giving a0,a1. Ac2t3's T at 747 has E<Emax, so it is not a candidate. Src is {A,Ac2t3}: Emax of the group is 957 (A) but both are identical before the cut. By clause 10 the provenance is the set attaining E, which is {A}. I list both as identical-byte sources for the retained prefix; strictly the clause says {A} (see U5).
7. Ah has a different start_time_ns. Comparing headers omitting only [6,8) gives different bytes, so HC at 0. No records are retained from the group (clause 8). The stop is the HC pair.
8. A is clean, so Ag's tail at 957 yields no stop. File 0 is sourced from {A,Ag}. N's header is valid (file 1). A is an actual predecessor delivery validating exactly through D=957 with no extra bytes and a matching digest, so it is a witness (Ag has a stray byte, so it is not one). No SC, since neither A nor Ag has a block beyond D. The link is admitted. Nt2 has E=465 (blocks 0 and 1), T at 465, sole delivery of file 1, no clean EOF, so S(A1,465,T). Records a0..a3, n0,n1. n2 and n3 are not validated.
9. At3 reaches only 747<957, so there is no witness and no SC. PU at N's header (clause 15). File 0 gets T at 747 W={At3}. The earliest capture stop is 747 in file 0, so N's records are excluded. Per clause 16 the PU is still generated independently. Records a0..a2.
10. A is clean, so no stops. The shorter copies only supply diagnostics.
11. Different capture "x" gives a separate file group. Capture a is as in case 1. X is clean, so all of x0..x3 are retained.
12. A,A is one delivery. Af differs only in flags [6,8), which are omitted, so there is no HC. Ao has an exactly valid footer, which is clean. All of {A,Af,Ao} attain E=957. There are no failures, so no diagnostics.
13. Both attain E=747. Neither is clean. Acrc3 has a complete block 3 with bad CRC (CRC, rank 4). At3 has T (rank 5). The lowest rank wins and the witness set is the deliveries with that reason, {Acrc3}. Src at E=747 is {At3,Acrc3}. Records a0..a2. At3's T is retained as a diagnostic.
14. M is clean at E=580. Mt3's E=489. The exact array omits the sample at 292 and keeps the original offsets. Records are m0 (0), m2 (8), m3 (0). The sample, being a point observation, is not a record.
15. Mc1 changes block 1's query_id and repairs chaining. Mc1t3 validates blocks 0..2 (E=489). Block 1 differs from M's, so BC at 292. The cutoff excludes block 1 and later, leaving m0. The sample is omitted regardless.
16. A: clean witness for N. A+ has a complete validated block beyond D=957, D is a block boundary of A+, and N's digest matches A+'s bytes through D. So SC is proven for the pair (A+,N) (clause 14). SC is placed in the predecessor at D, retaining only blocks before D (a0..a3) and excluding a4 and N. Clause 15: seal proofs exist, so reject the link and generate no PU. A's witness status is a local fact only. Emax=1179 (A+), but the cut at 957 limits records.
17. Z has an invalid header, so IH is a diagnostic (clause 17: no stop attributed using unvalidated identity). A's decode is unaffected.
18. As case 16 without A. A+ is not a witness (it has extra blocks), but it does prove SC.
19. Ag: E=957, T. N: witness search fails because Ag has a trailing byte 00, which is not an exact footer. SC requires a complete validated block beyond D, which is absent, so there is no SC. PU at N's header. Stops: T in file 0, PU in file 1. N's records are excluded by the cutoff and by PU.
20. Capture a. File 0: only At1 (E=305), T at 305, a candidate. File 1: N is clean E=689 with no predecessor reaching 957, so PU. File 2: L and Lc2t3 both link to N. N independently validates through its own D=689 and is clean, so it is a witness for L (clause 13: local fact regardless of the earlier stop). BC at 465 between L and Lc2t3. Lc2t3's T at 577 has E<689 and is not a candidate. The earliest cutoff is 305 in file 0, leaving a0. Later stops stay reportable (clause 18).
21. Both attain E=957 with no clean copy. Both give T at 957, the same position and reason, so the witnesses union (clause 17): {Ag,Ag′}.
22. A+ attains E=1179 and is clean, so no terminal stop. A is a prefix of A+. Records include a4.
23. L's predecessor (file 1) is absent, so there is no witness and no SC, giving PU at L's header. L is not read as a root. Missing file 1 is not manufactured, and A is untouched.
24. N alone: no predecessor delivery, so PU. N is not a root, so there are no records.
25. 17 bytes of footer is an incomplete block header, so T at 957. Not clean.
26. A is clean at the same E, so Aft's T is only a diagnostic.
27. Nb's digest is wrong, so SC cannot be proved (digest must match). A+ is not a witness (extra bytes). So PU. A+ is clean with a4 retained. The PU is not an SC, and no earlier cutoff applies.
28. Ni declares D=956, which is not a validated block boundary of A+ and is not exactly any delivery's end. No witness and no SC, so PU.
29. Both attain E=957 and are clean. Block 1 is identical in both (same repaired change), so the first differing block is 3, with BC at 747. Both have an RF at (305,8). The two RF candidates are at the same position and reason, so W={Au,Auc3}. Normalization gives one stop per file: earliest position, (305,8). The BC at (747,null) is a diagnostic. The structural record stop preserves preceding records in that block (none in block 1 before the faulty record), and block 0's a0 survives.
30. Codec: both clean, E=979. Block 3 differs, so BC at 769. Codec records are r0,r1,r2. Replay receives these and finds the overlapping ATTACH at (537,8). That is a replay stop, earlier than the codec cutoff 769, so it is the effective cutoff. Replay stops go in a separate domain and are not fed back into the codec normalization (clause 19). BC inspection continues independently.
31. Header CRC bad, so IH. No identity, no stop. Failed decode with no assigned capture.

## Undetermined values

- U1. Clause 20, cases 17 and 31, "Diagnostics" cell. "An unassigned header failure remains a failed decode, not an empty success" does not say whether the multiset A,Z as a whole reports a failed decode, or only Z's delivery does (A decodes fine). I gave the per-delivery reading in case 17 and a whole failed decode in case 31.
- U2. Clause 20 and the "retroactive VOID" bullet, case 22 (and 27, 28), explicit replay effect. The text says the update must incorporate the VOID but does not state its numeric effect. VOID invalid_from=20 on a3=FRONTIER(20) would need CONTRACT.md for the inclusive/exclusive revocation boundary, and that file is not supplied. I give only "a4 is retained and delivered to replay".
- U3. Clause 7, case 29. RF candidates exist "where the governing contract requires stopping", but the contract is not supplied. I took an unknown record kind as stopping, supported by the fixture's "RF at (305,8)" and clause 18's "structural record stop". If it does not stop, the stop would be BC at 747 and the records a0,a1,a2.
- U4. Clauses 19 and 7, case 30. The replay stop has no defined notation or reason name, and the text does not say whether the faulting record r2 contributes partial effects. I left "replay stop at (537,8)" unnamed and assumed r2 is not applied.
- U5. Clause 10, case 6 (and 15). Source provenance is "all deliveries attaining E" (E = group maximum). For the retained prefix cut at a BC, the identical-bytes sources can include the shorter delivery, but the clause literally gives {A} only. I wrote the wider set in the table. The narrower reading is {A} for case 6 and {M} for case 15.
- U6. Clause 20, PU details. "Available predecessor validation/link-check results" has no fixed format, so I described the content in words only.


---

# Round 2 (cases 22, 27-30, with CONTRACT.md and REVISION-r3.md also supplied)

# Answers (cases 22, 27, 28, 29, 30) -- Sonnet author 2

Notation: record identity = (boot b, capture, file_no, block_offset, record_offset). Files: A-family "a" file 0 = F0a, N-family file 1 = F1a, M/x unused.
Citations: R = REDELIVERY.md clause number; K = CONTRACT.md line; R3 = REVISION-r3.md line; C = CODEC.md line.

| # | Input | Codec records | Normalized codec stops | Diagnostics / replay effect |
|--:|---|---|---|---|
| 22 | A,A+ | a0 (b,a,0,193,8) LIFE(1); a1 (…,305,8) ATTACH(0); a2 (…,537,8) FRONTIER(10); a3 (…,747,8) FRONTIER(20); a4 (…,957,8) VOID(t=30,zero,"x",invalid_from=20). Source provenance {A+} (only delivery attaining E=1179). | none | No codec diagnostics (A ends at clean EOF at E(A)=957<E, no failure; A is a prefix of A+, no BC). Replay: no replay stop, no cutoff. a2 accepted (F=H=10), a3 accepted (F=H=20), then a4 revokes certificates at/after 20 (K:189-193): a3 revoked, F rolls back to a2 (10), H stays 20 (K:178-179), run recomputed capped at b=10, then frozen (K:186-188). Survives: LIFE [0,null], certificate a2, coverage [[0,10]], waits [], open null (K:218-222; state null reread). Revoked: a3 / coverage to 20 (result of A alone is retracted, R20). Baseline stays zero; nonnull invalid_from makes lifecycle uncertainty at t=30 (K:148-149), so no new runs. |
| 27 | A+,Nb | a0..a4 as in 22 (A+ only). Provenance {A+}. Nb's n0..n3 excluded (whole file 1 cut at its header). | S(F1a, (0,null), PU; {Nb}) | d(Nb,(0,null),PU) carrying declared predecessor (file 0, D=957, digest with first bit flipped) and link-check results: candidate predecessor A+ validates through 957 and beyond, D=957 is a block boundary, static fields match, digest MISMATCH (R13), A+ has a block beyond D so is not a witness; no SC proof because digest does not match (R14); so PU not SC (R15). Replay effect identical to 22 (file 0 fully admitted, VOID a4 retained): coverage [[0,10]], a3 revoked, no replay stop. Chain stop at file 1 start (K:307-308) removes no file-0 evidence. |
| 28 | A+,Ni | a0..a4 as in 22. Provenance {A+}. n0..n3 excluded. | S(F1a, (0,null), PU; {Ni}) | d(Ni,(0,null),PU): declared D=956 is not a validated block boundary of A+ (or A), no predecessor validates exactly through 956 (R13), no SC (R14: D not a boundary, so no seal proof even though stored digest equals hash of A+ bytes through 956), PU (R15). Replay effect identical to 22/27. |
| 29 | Au,Auc3 | a0 (b,a,0,193,8) LIFE(1) only. Provenance {Au,Auc3} (both attain E=957). Block 1 stops at its offending record (305,8), first record of block 1, so nothing of block 1 survives; blocks 2,3 beyond cutoff. | S(F0a, (305,8), RF; {Au,Auc3}) | Diagnostics: d(Au,(305,8),RF), d(Auc3,(305,8),RF) (unsupported kind 255 in trustworthy framing; R7, R3:202, K:251-255). Suppressed (+): BC at (747,null), proving pair {(Au,Auc3)}: blocks 0-2 byte-equal, block 3 differs (t 20 vs 21); stop elected by earlier position (R17) so BC is a diagnostic only. No terminal failure (both clean EOF at 957). Replay: sees only a0. No replay stop; effective cutoff = codec cutoff (305,8). No FRONTIER/ATTACH survives: F absent, H absent, no certificates; verified LIFE retained: life [0,null], coverage [], waits [], open null (K:240,312). No synthetic VOID (R3:202; K:235). |
| 30 | R,Rc3 | r0 (b,a,0,193,8) LIFE(1); r1 (…,305,8) ATTACH(0); r2 (…,537,8) ATTACH(1). r3/FRONTIER excluded (whole block 3 cut by BC). Provenance {R,Rc3}. | S(F0a, (769,null), BC; {(R,Rc3)}) -- only codec stop; the overlap is NOT a codec stop (R7, R19). | Diagnostics: BC (769,null) proof pair {(R,Rc3)} (blocks 0-2 equal, block 3 differs). Both deliveries clean EOF at 979, no terminal failure. Replay stop (separate domain): overlapping ATTACH_OK r2 at file 0 (537,8): r1's run (pid1 fork0, anchor 0) is not ended or frozen, so r2 overlaps (K:256-258, K:167-168); counters zero equal baseline so no counter-VOID path first (K:278-282). Effective cutoff = min(codec (769,null), replay (537,8)) = (537,8) (R19). Surviving: no accepted FRONTIER before the stop (r3 is excluded both by codec and replay), so F absent, H absent, no certificates; tentative run from r1 discarded (K:239); verified LIFE retained: life [0,null], coverage [], waits [], open null. Same result for R and Rc3 since the differing frontier is never reached. The replay stop is not fed back to codec normalization (R19). |

## Derivation notes
- Prefix/provenance: 22: E(A)=957, E(A+)=1179; max attained by A+ only (R5, R10). 27/28: file 0 = A+ only. 29: Au and Auc3 identical through block 2, E=957 for both. 30: both E=979.
- Predecessor witness for 27/28: none. A+ is not a witness since it has bytes beyond D (R13); A alone is not in these multisets. Nb has wrong digest; Ni names non-boundary D.
- 29: RF at (305,8) rank 8 versus BC at (747,null) rank 1; position wins first (R17), so RF is the stop.

## Undetermined cells
1. Case 30, replay-effect cell: the replay stop has no defined name/reason label or position encoding (R3 says replay failures are a "separate result domain" but no enumeration is given). I state it descriptively as "overlapping ATTACH_OK at (537,8)". Count: 1.
