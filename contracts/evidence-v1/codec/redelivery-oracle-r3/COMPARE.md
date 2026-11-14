# redeliv3 comparison: Reference (Astra) vs sol vs Sonnet, cases 17, 31, 32-50

Legend: AGREE = all three equal (notational variants treated as equal). Replay-state names: RA=T20, RV=V1, RU=VU, RW=C1, RI=I0, RVI=IV1, LX=J(x), LQ/LN = the retained q/n LIFE sets.
"Note" = a difference of wording or level of detail that I did not count as a value disagreement.

| # | Codec records | Normalized stops | Diagnostics / replay effect | Aggregate |
|--:|---|---|---|---|
| 17 | AGREE (a0..a3) | AGREE (none) | AGREE (Z unassigned IH; RA/T20; Sonnet states F=20 only) | AGREE FAILED |
| 31 | AGREE (none) | AGREE (none) | AGREE (Z unassigned IH, no trusted capture) | AGREE FAILED |
| 32 | AGREE (none) | AGREE (none) | AGREE U("a";{A,A2}), no replay (sol and Sonnet also give boots {b,c}) | AGREE FAILED |
| 33 | AGREE (none) | AGREE S(N2,0,PU;{N2}) (sol writes file as (c,a,1)) | AGREE S + U("a";{A,N2}), no replay | AGREE FAILED |
| 34 | AGREE (x0..x3) | AGREE (none) | AGREE U("a";{A,A2}), x passes. Note: Sonnet says only "capture x PASS" and does not spell out LX | AGREE FAILED |
| 35 | AGREE (none) | AGREE (none) | AGREE U("a";{A,A2}), duplicate counts once | AGREE FAILED |
| 36 | AGREE (a0..a3,v4,q0..q3) | AGREE (none) | AGREE RV+LQ, Q baseline B1 passes. Note: Sonnet does not mention lifecycle uncertainty at 30 | AGREE PASS |
| 37 | AGREE | AGREE (none) | AGREE P(Q0;B1,B0), RV retained, q records excluded | AGREE FAILED |
| 38 | AGREE (a0..a3,n0..n3) | AGREE (none) | AGREE RA+LN, B0/B0 | AGREE PASS |
| 39 | AGREE (a0..a3,v4,q0..q3) | AGREE (none) | AGREE RV+LQ, witness {V} (sol also gives source set {V}) | AGREE PASS |
| 40 | AGREE (a0..a3) | AGREE S(.,957,SC;{(V,N1)}). Note: Sonnet labels the file "V", reference and sol label it A / (b,a,0) | AGREE S; RA; v4 and N1 excluded; no baseline check | AGREE FAILED |
| 41 | AGREE (a0..a3,n0..n3) | AGREE (none) | AGREE P(N1;B0,B1), RA retained, N1 records excluded | AGREE FAILED |
| 42 | AGREE (a0..a3) | AGREE (none) | AGREE Z2 unassigned IH, no U, RA | AGREE FAILED |
| 43 | AGREE (a0..a3,vu4) | AGREE (none) | AGREE RU, Tu null/null passes | AGREE PASS |
| 44 | AGREE (a0..a3,vu4) | AGREE (none) | AGREE P(T0;null,B0), RU retained | AGREE FAILED |
| 45 | AGREE (a0..a3,w4,q0..q3) | AGREE (none) | AGREE RW+LQ, Qw B1 passes (sol and Sonnet name the counter-loss/VOID treatment of w4; reference calls it a semantic annotation) | AGREE PASS |
| 46 | AGREE | AGREE (none) | AGREE P(Qw0;B1,B0), RW retained, q records excluded | AGREE FAILED |
| 47 | AGREE (none) | AGREE stop at 193 reason T, witness {A2t0}. Note: reference/sol file label A2 / (c,a,0); Sonnet writes "A2t0" | AGREE S + U("a";{A,A2t0}). Note: Sonnet 47 does not say "no replay for a" (it does in 32/33) | AGREE FAILED |
| 48 | AGREE (a0..a3,w4,q0..q3) | AGREE (none) | AGREE RI+LQ, w4 ignored, B0/B0 passes | AGREE PASS |
| 49 | AGREE | AGREE (none) | AGREE P(Qwi1;B0,B1), RI retained, successor excluded | AGREE FAILED |
| 50 | AGREE (a0..a3,v4,q0..q3) | AGREE (none) | AGREE RVI+LQ, explicit v4 sets B1, successor passes. Note: Sonnet does not mention lifecycle uncertainty at 30 | AGREE PASS |

Cases where all three agree in every column: 21 of 21 (17, 31, 32-50). No value-level disagreement found; the notes above are label or detail-level only.

## Cells marked undetermined by an author

sol (says no requested cell is undetermined; "finer values" it lists):
- Cases 45-46: exact opaque diagnostic reason string of the VOID consumed from w4 (written rho); K section 5 requires a reason but gives no string.
- All diagnostic-bearing cases (particularly 30, 48-49): literal diagnostic message text / serialization schema not fixed (D3, D20, CODEC section 9).
- Any numerical query metric: no target/query supplied, so none can be computed.

Sonnet:
- U1 (case 29): one stop per file (RF kept, later BC demoted to diagnostic) versus c18 allowing a second stop; chose one per file.
- U2 (cases 9, 19, 20, 23, 24, 27, 28, 33): whether a PU stop on a file excluded by an earlier cutoff counts as a normalized stop or only a diagnostic; chose normalized stop.
- U3 (case 30): no notation for a non-baseline replay stop (written informally as "replay stops at r2 (537,8)").
- U4 (cases 32-35, 47): canonical ordering of witnesses/boots by byte identity cannot be computed without hashes; sets given unordered.
- U5 (PU cells): exact field layout of PU candidate-group results undefined.
- U6 (case 24): whether a rootless capture reports PU or a missing-root diagnostic.
- U7 (cases 36, 45, 50): observability of lifecycle-uncertainty effects on pids 2..5 not fixed by the table.

## Cases 1-30: Sonnet re-derivation versus reference table

No difference in record sets, stop position/reason/witness sets, replay effect or aggregate. Label-level differences only:
- Case 5: Sonnet names the suppressed diagnostic d(At1,305,T) (noting Ac2t1 has the same bytes); reference lists d(Ac2t1,305,T).
- Cases 27, 28: Sonnet writes the stop as S(Nb,0,PU;{Nb}) / S(Ni,0,PU;{Ni}); reference writes S(N,0,PU;{Nb}) / S(N,0,PU;{Ni}) (file label N versus delivery name).
- Case 40 (in range above): same file-label pattern, S(V,...) versus S(A,...).
(Reference aggregate cells for 1-16 and 18-30 are em dashes; Sonnet gives PASS/FAILED there, not comparable.)
