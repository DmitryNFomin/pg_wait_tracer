# Answers (Sonnet) - redelivery r3b, cases 1-50

Clause numbers (c#) are REDELIVERY.md clauses 0-20. K = CONTRACT.md, R3 = REVISION-r3.md, C = CODEC.md. All cases are evaluated as unordered multisets, so arrival order cannot change a result. Identical byte strings count as one delivery.

## Notation used

- Physical record identity is `(boot,capture,file,block,rec)`. Within one case the boot and capture are fixed, so I abbreviate to `name@file:block:rec`.
  - `a0..a3@0:{193,305,537,747}:8`; `a4=v4=vu4@0:957:8`; `w4@0:957:8`.
  - `n0..n3@1:{241,353,465,577}:8`, and `q0..q3` sit at the same offsets in file 1.
  - `x0..x3@0:{193,305,417,529}:8` in capture x.
  - `m0@0:201:0`, `m2@0:377:8`, `m3@0:489:0`. `m1` is a valid sample (omitted from the array, surviving offsets unchanged).
  - `r0..r3@0:{193,305,537,769}:8`.
- Stop `S(F,o,r;W)` follows the case definition. `d(delivery,o,r)` is a diagnostic. `[sup]` marks a suppressed diagnostic. `pair(P,Q)` is a conflict proving pair, with predecessor first for SC. `U(c;W)` and `P(F;expected,declared)` are as defined.
- "Replay" gives the effective result of replaying the codec-admitted stream. F is the latest still-valid frontier, HW the publication high-water, and "LIFE kept" the verified LIFE.
- Baseline columns: B0 = 15 zeros, B1 = slot 4 set, null = unknown.
- Witness ordering is canonical by byte identity (hash). I cannot compute hashes, so witness sets are given as unordered sets.

## Answer table (cases 1-50)

| # | Input | Codec records | Normalized codec stops | Diagnostics / replay effect | Aggregate |
|--|--|--|--|--|--|
| 1 | At1,At3 | a0,a1,a2 | S(A,747,T;{At3}) | S; d(At1,305,T) [sup]. Replay: F=10, HW=10, run [0,10]; LIFE(1) | FAILED |
| 2 | A,At1 | a0,a1,a2,a3 | none | d(At1,305,T) [sup]. Replay: F=20, HW=20 | PASS |
| 3 | Ag,At1 | a0..a3 | S(A,957,T;{Ag}) | S; d(At1,305,T) [sup]. Replay: F=20, HW=20 | FAILED |
| 4 | A,Acrc2 | a0..a3 | none | d(Acrc2,537,CRC) [sup]. Replay: F=20 | PASS |
| 5 | A,Ac2t1 | a0..a3 | none | d(At1,305,T) [sup] (Ac2t1 = At1 bytes). Replay: F=20 | PASS |
| 6 | A,Ac2t3 | a0,a1 | S(A,537,BC;{pair(A,Ac2t3)}) | S; d(Ac2t3,747,T) [sup]. Replay: LIFE(1) only, no frontier, HW absent | FAILED |
| 7 | A,Ah | none | S(A,0,HC;{pair(A,Ah)}) | S. Replay: none | FAILED |
| 8 | A,Ag,Nt2 | a0..a3,n0,n1 | S(N,465,T;{Nt2}) | d(Nt2,465,T); d(Ag,957,T) [sup]. Link N: witness {A}, admitted. Replay: N baseline B0 = B0 ok; LIFE 1,2,3; F=20 | FAILED |
| 9 | At3,N | a0,a1,a2 | S(A,747,T;{At3}); S(N,0,PU;{N}) | S; PU(N: declared pred A@957, candidate group {At3} validates only to 747, no witness, no seal). Replay: F=10, HW=10 | FAILED |
| 10 | At1,At3,A | a0..a3 | none | d(At1,305,T) [sup]; d(At3,747,T) [sup]. Replay: F=20 | PASS |
| 11 | At1,X,At3 | a: a0,a1,a2; x: x0,x1,x2,x3 | S(A,747,T;{At3}) (capture a only) | S; d(At1,305,T) [sup]. Replay a: F=10; x: LIFE 10..13, no coverage | FAILED |
| 12 | A,A,Af,Ao | a0..a3 | none | none. Source {A,Af,Ao}. Replay: F=20 | PASS |
| 13 | At3,Acrc3 | a0,a1,a2 | S(A,747,CRC;{Acrc3}) | S; d(At3,747,T) [sup]. Replay: F=10 | FAILED |
| 14 | M,Mt3 | m0,m2,m3 (m1 omitted) | none | d(Mt3,489,T) [sup]. Replay: LIFE(1); no run, no coverage; TRANS outside every run ignored | PASS |
| 15 | M,Mc1t3 | m0 | S(M,292,BC;{pair(M,Mc1t3)}) | S; d(Mc1t3,489,T) [sup]. Replay: m0 TRANS only; no LIFE | FAILED |
| 16 | A,A+,N | a0..a3 | S(A,957,SC;{pair(A+,N)}) | S. A is also a clean witness but seal proof exists, so SC and no PU. a4 and N excluded. Replay: F=20, HW=20 | FAILED |
| 17 | A,Z | a0..a3 | none | d(Z,(0,null),IH), unassigned. Replay: F=20 | FAILED |
| 18 | A+,N | a0..a3 | S(A,957,SC;{pair(A+,N)}) | S. Replay: F=20, a4 excluded | FAILED |
| 19 | Ag,N | a0..a3 | S(A,957,T;{Ag}); S(N,0,PU;{N}) | S; PU candidate group {Ag}: remaining byte, no block beyond D, so no seal proof. Replay: F=20 | FAILED |
| 20 | At1,N,L,Lc2t3 | a0 | S(A,305,T;{At1}); S(N,0,PU;{N}); S(L,465,BC;{pair(L,Lc2t3)}) | S; d(Lc2t3,577,T) [sup]. L links to N witness (N valid to 689). Replay: LIFE(1) only | FAILED |
| 21 | Ag,Ag′ | a0..a3 | S(A,957,T;{Ag,Ag′}) | S. Replay: F=20 | FAILED |
| 22 | A,A+ | a0,a1,a2,a3,a4 | none | none. Replay: a4 VOID(inv=20) revokes F20; F=10, HW=20; run [0,10] frozen; baseline B0; lifecycle uncertain at t=30 | PASS |
| 23 | A,L | a0..a3 | S(L,0,PU;{L}) | S; predecessor group (b,a,1) absent. Replay: F=20 | FAILED |
| 24 | N | none | S(N,0,PU;{N}) | S; predecessor absent, no root. Replay: none | FAILED |
| 25 | Aft | a0..a3 | S(A,957,T;{Aft}) | S. Replay: F=20 | FAILED |
| 26 | A,Aft | a0..a3 | none | d(Aft,957,T) [sup]. Replay: F=20 | PASS |
| 27 | A+,Nb | a0..a4 | S(Nb,0,PU;{Nb}) | S; digest mismatch, A+ not exact to D, no seal. Replay as case 22 | FAILED |
| 28 | A+,Ni | a0..a4 | S(Ni,0,PU;{Ni}) | S; D=956 not a block boundary. Replay as case 22 | FAILED |
| 29 | Au,Auc3 | a0 | S(A,(305,8),RF;{Au,Auc3}) | S; d(pair(Au,Auc3),747,BC) retained as diagnostic (later position in the same file). Replay: LIFE(1) only | FAILED |
| 30 | R,Rc3 | r0,r1,r2 | S(R,769,BC;{pair(R,Rc3)}) | S. Replay: r2 ATTACH_OK(t=1) overlaps r1's run, so replay stops at r2 (537,8). Kept: LIFE(1); no certificates | FAILED |
| 31 | Z | none | none | d(Z,(0,null),IH), unassigned | FAILED |
| 32 | A,A2 | none | none | U(a;{A,A2}), boots {b,c}. Each header-valid delivery is a witness. No replay | FAILED |
| 33 | A,N2 | none | S(N2,0,PU;{N2}) | U(a;{A,N2}), boots {b,c}; PU: N2's own group (c,a,0) is empty. No replay | FAILED |
| 34 | A,A2,X | x: x0..x3 (a withheld) | none | U(a;{A,A2}), boots {b,c}. Capture x PASS | FAILED |
| 35 | A,A2,A2 | none | none | as 32 (duplicate A2 counts once) | FAILED |
| 36 | V,Q | a0..a3,v4,q0..q3 | none | Replay: v4 revokes F20 so F=10, HW=20, baseline B1; Q declares B1 matches; LIFE 1..5 kept | PASS |
| 37 | V,Q0 | a0..a3,v4,q0..q3 | none | P(Q0;B1,B0). Replay prefix: F=10, HW=20, LIFE(1); q's ignored | FAILED |
| 38 | A,N | a0..a3,n0..n3 | none | Replay: B0 = B0; F=20, HW=20; LIFE 1..5 | PASS |
| 39 | A,V,V,Q | a0..a3,v4,q0..q3 | none | as 36. Link witness {V}. A's blocks are a prefix of V's | PASS |
| 40 | V,N1 | a0..a3 | S(V,957,SC;{pair(V,N1)}) | S. N1 excluded, so no P reached. Replay: F=20 | FAILED |
| 41 | A,N1 | a0..a3,n0..n3 | none | P(N1;B0,B1). Replay prefix: F=20, HW=20 | FAILED |
| 42 | A,Z2 | a0..a3 | none | d(Z2,(0,null),IH), unassigned; cannot prove reuse. Replay: F=20 | FAILED |
| 43 | Vu,Tu | a0..a3,vu4 | none | Replay: baseline null; Tu declares null (unknown = unknown), match. F=10, HW=20 | PASS |
| 44 | Vu,T0 | a0..a3,vu4 | none | P(T0;null,B0). Replay prefix: F=10, HW=20 | FAILED |
| 45 | W,Qw | a0..a3,w4,q0..q3 | none | Replay: w4 B1 differs from B0, so consumed as VOID(t=30,B1,null), no frontier. Baseline B1; Qw declares B1, match. F=20 frozen, HW=20; LIFE 1..5 | PASS |
| 46 | W,Qw0 | a0..a3,w4,q0..q3 | none | P(Qw0;B1,B0). Replay prefix: F=20, HW=20, LIFE(1) | FAILED |
| 47 | A,A2t0 | none | S(A2t0,(193,null),T;{A2t0}) | U(a;{A,A2t0}), boots {b,c}; d(A2t0,193,T) | FAILED |
| 48 | Wi,Qwi0 | a0..a3,w4,q0..q3 | none | Unqualified: certificates ignored, w4 not a VOID, baseline stays B0; Qwi0 B0, match. LIFE 1..5, no coverage, no HW | PASS |
| 49 | Wi,Qwi1 | a0..a3,w4,q0..q3 | none | P(Qwi1;B0,B1). Replay prefix: LIFE(1), no coverage | FAILED |
| 50 | Vi,Qvi | a0..a3,v4,q0..q3 | none | Unqualified: certificates ignored, explicit VOID still sets baseline B1; Qvi B1, match. LIFE 1..5, no coverage | PASS |

## Derivations

### Truncation and CRC cases

**1.**
- Group (b,a,0), compatible headers; At1's blocks are a prefix of At3's, so there is no BC (c9).
- E=747 (At3). Source {At3} (c10). At1's tail at 305 < E is never a candidate (c12).
- At3's T at 747 is the only terminal at E, and nobody ends clean there, so one candidate T@747 with W={At3} (c12). Normalized by c17.
- Cutoff c18 gives a0..a2.
- Capture FAILED by c0; aggregate FAILED.
- Incremental: At1 alone gives T@305 (a0). Adding At3 moves the stop to 747 and extends the prefix (monotone extension).

**2, 4, 5, 10, 26.** A attains E at clean EOF, so no terminal failure at E becomes a candidate (c12). Shorter, bad-CRC and torn variants are suppressed diagnostics only (c0). Rejected blocks supply no conflict evidence (c9), so Acrc2 yields no BC. Aggregate PASS. Case 5 relies on Ac2t1 being byte-identical to At1. Case 26 relies on torn footers having no special clean status, while A itself ends clean.

**3, 21, 25.**
- A tail that is not EOF or an exact footer, and is shorter than a 64-byte block header, gets T at E=957 (c6).
- In case 3, Ag alone attains E, so S(A,957,T;{Ag}). At1 is suppressed.
- In case 21, Ag and Ag′ are distinct byte strings with the same reason, so W={Ag,Ag′}.
- In case 25 a 17-byte torn footer is a T tail.
- All records a0..a3 survive because the stop is at D.

**13.** At3 (T) and Acrc3 (CRC) both attain E=747. The lowest rank wins (CRC 4 < T 5), and W is all deliveries with that reason, {Acrc3}. At3's T becomes a diagnostic (c12, c17).

**14.**
- M is clean. Mt3's tail at 489 < 580 is suppressed.
- Records follow c11: m1 is a valid sample and is omitted. Offsets are 0 for rows and 8 for m2.
- Replay: TRANS before LIFE resolves by dependency. No ATTACH_OK means no run, so both TRANS are ignored (R3 rule 6).

**15.** Mc1 changes block 1 and re-chains blocks 2 and 3. The first differing block is 1, at 292, so BC. The records before it are m0.

### Conflicts

**6.** Ac2 changes block 2, and the re-chain also changes block 3's preceding CRC. The first differing block is at 537, so BC. Source A, cut before 537. Replay has LIFE(1) and ATTACH_OK, with no frontier and no certificate.

**7.** Different immutable header (start_time) gives HC at 0. No records are retained from that group (c8).

**29.**
- Block 1 holds kind 255 inside intact framing, so the block stays in P. Both Au and Auc3 carry it, so RF is a candidate at (305,8) with W={Au,Auc3} (c7). K §5 rule 1 requires stopping.
- Au and Auc3 differ at block 3, giving BC@747.
- c17 keeps one stop per file, the earliest position, which is RF at 305. BC is kept as a diagnostic.
- The RF stop preserves earlier records in its validated block, and there are none before offset 8. Only a0 remains.

**30.** The BC at 769 is a codec stop and a cutoff, which leaves r0,r1,r2. In replay r2 is an ATTACH_OK overlapping the run from r1 (K §5 rule 2), so replay stops at r2, which is before the codec cutoff. LIFE(1) is kept and no frontier exists.

### Links

**8.** Ag has a remaining byte, so it is not a witness. A is a witness for N (valid exactly to 957, clean). Nt2's own tail at 465 is the only terminal at E for that file, so S(N,465,T). n0 and n1 survive. Ag's tail is suppressed because A ends clean at E.

**9, 19.** PU is generated exactly when no witness exists (c15). At3 is too short; Ag is dirty and has no block beyond D, so no seal proof. Both PU and the file-0 stop are reported (c16, c18 "later intrinsic stops remain reportable"; see U2).

**16, 18, 40.** The seal proof needs the static match, D a block boundary, a matching digest, and a block beyond D. Case 16 has the clean witness A, but seal proofs force SC and not PU (c15). SC sits at (D,null) in the predecessor, with the predecessor's extension and the successor excluded. Case 40 holds even though N1's baseline differs (c14): the baseline is never evaluated, and N1 is excluded, so no P is reached.

**20.** The earliest stop is T@305 (file 0). PU for N and BC for L (file 2) are retained (c16, c18). L links to N, which has a clean witness. Lc2t3's T at 577 < 689 is suppressed.

**23, 24.** The predecessor group is empty, so PU at the successor header, with the predecessor reported absent. In case 24 there is no root, so nothing is admitted.

**27, 28.** Case 27: a flipped digest bit gives no seal proof and no witness, so PU. Case 28: D=956 is not a validated block boundary, so there is no seal proof and no witness, so PU.

**22.** A+ attains E=1179 and supplies a4. Replay of VOID(t=30,B0,inv=20): prior certificates at or after 20 are revoked, so F rolls back to 10 and HW stays 20. The run is recomputed to [0,10] and frozen. The baseline is replaced with B0, and non-null invalid_from means lifecycle uncertainty at 30.

**12.** Af differs only in flags, and Ao has an exactly valid 68-byte footer, so there is no effect on records or stops. Source provenance is {A,Af,Ao}.

### Header cases

**17, 31, 42.** An invalid header supplies no trusted identity and cannot form a stop or prove reuse (c2, c17, 2a). The unassigned header failure alone fails the aggregate (c0). In 17 and 42, A's capture passes.

### Cross-boot cases (32-35, 47)

- Capture a has headers with boots b and c, so exactly one U with boot set {b,c} (c2a). Each header-valid delivery naming the capture is a witness (identity, boot, file ordinal, (0,null)).
- Records, LIFE, certificates and coverage for capture a are withheld. Independent inspection continues, so PU in 33 and T in 47 are reported.
- 33: N2 is in group (c,a,0), which is empty, so PU. A is not a candidate because the boot differs.
- 34: capture x is unaffected and passes.
- 35: the duplicate A2 counts once.
- 47: A2t0's header validates, so it counts for reuse. It also has a T tail at E=193.
- Incremental: adding A2 retracts A's exposed records. Dropping it restores them.

### Baseline cases

- 36/39: V is a clean witness and Q declares B1, which equals the running baseline after v4. No mismatch.
- 37: Q0 declares B0 but replay holds B1, so P(Q0;B1,B0) at Q0's descriptor, before q0. Codec records are unchanged.
- 41: A's baseline is B0 and N1 declares B1, so P(N1;B0,B1).
- 43: both sides unknown, equal. 44: unknown (null) differs from known B0, so P(T0;null,B0).
- 45: a FRONTIER with a readable counter change is consumed only as VOID (K §5), so no frontier is published. Q's declaration of B1 matches. 46: Qw0 declares B0, so P(Qw0;B1,B0).
- 48-50: in an unqualified capture, certificates are ignored and do not move the baseline (K §5, c19). Explicit VOID still applies, so Vi gives baseline B1. The successor check still applies. Qwi1 declares B1 against B0, so P(Qwi1;B0,B1). Qwi0 and Qvi match.

### Incremental and retraction notes

- Retractions occur on:
  - HC, BC and SC (suffix removal), as when N arrives after A+ in case 16 and a4 is dropped.
  - A boot-ambiguity header (whole-capture retraction).
  - A new longer prefix removing a former tail stop.
  - A new clean witness removing a PU.
  - A replay stop where later VOID or records change the baseline.
- Successor-before-predecessor accumulation: a lone N gives PU, which retracts once A arrives (cases 38, 41).
- VOID revocation is retroactive when a longer copy arrives (A then A+).

## Values the text leaves undetermined or interpretive

- **U1 (c4 / c17, cases 29).** BC at 747 sits in the same file as the RF stop at (305,8). c17 allows only one stop per file (earliest), so BC drops to a diagnostic. c18's "later intrinsic codec stops remain reportable" could also be read to allow a second stop for the same file. I used one per file.
- **U2 (c15 / c17 / c18, cases 9, 19, 20, 23, 24, 27, 28, 33).** A PU stop on a successor after an earlier cutoff is reported as its own normalized stop for that successor's file. c17 and c18 support this, but no clause says PU on a file excluded by an earlier cutoff counts as an eligible candidate. A grader might drop those PU stops to diagnostics.
- **U3 (cases 30 and similar, c0 / c19).** No notation or name exists for a non-baseline replay stop (K §5 rule 2 overlapping ATTACH_OK). Only `P(...)` is defined for baseline mismatches. I wrote it as "replay stops at r2 (537,8)". This cell is not mechanically defined.
- **U4 (c2a, cases 32-35, 47).** Canonical order of witnesses and boots is "by byte identities". Without hashes, the order cannot be computed here. The sets themselves are determined.
- **U5 (c20, PU "validation/link-check results").** The exact field layout of PU candidate-group results is not defined. I described the results informally.
- **U6 (case 24).** With no file 0, no file is admitted, yet a PU stop is still reported for file 1. c18 requires starting at file 0 and does not say whether a rootless capture reports PU or a missing-root diagnostic.
- **U7 (cases 36, 45, 50, c19 and K §4).** Lifecycle-uncertainty effects (non-null invalid_from at VOID t=30) on pids 2..5 (fresh fork 40, no ATTACH_OK) are not observable in the exact columns. LIFE is retained under K §4's recovery rule, but whether those LIFEs count as "verified" for tainted generations is not stated for the case table.

Cases filled: 50 of 50 (17, 31, 32-50 as in CASES-blank, plus 1-30 revalidated).
