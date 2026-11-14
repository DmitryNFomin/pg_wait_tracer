# Redelivery table comparison (Ref = Astra REDELIVERY.md, sol, Sonnet)

Notational variants treated as equal (file labels F0/A0/A, S(F,...) names, provenance wording, "S underlying" expansions of d(At3,747,T) etc.).

| # | Records | Stops | Diagnostics / replay effect |
|--:|---|---|---|
| 1 | AGREE | AGREE | AGREE |
| 2 | AGREE | AGREE | AGREE |
| 3 | AGREE | AGREE | AGREE |
| 4 | AGREE | AGREE | AGREE |
| 5 | AGREE | AGREE | AGREE |
| 6 | AGREE | AGREE | AGREE |
| 7 | AGREE | AGREE | AGREE |
| 8 | AGREE | AGREE | AGREE |
| 9 | AGREE | AGREE | AGREE |
| 10 | AGREE | AGREE | AGREE |
| 11 | AGREE | AGREE | AGREE |
| 12 | AGREE | AGREE | AGREE |
| 13 | AGREE | AGREE | AGREE |
| 14 | AGREE | AGREE | AGREE |
| 15 | AGREE | AGREE | AGREE |
| 16 | AGREE | AGREE | AGREE |
| 17 | AGREE | AGREE | AGREE |
| 18 | AGREE | AGREE | AGREE |
| 19 | AGREE | AGREE | AGREE |
| 20 | AGREE | AGREE | AGREE |
| 21 | AGREE | AGREE | AGREE |
| 22 | AGREE | AGREE | DIFFER. Ref: no codec diagnostics; replay revokes frontier 20, retains frontier 10, coverage [0,10), publication high-water stays 20. sol: only "V-effect" (a4 replayed with encoded fields), exact surviving certificates undetermined. Sonnet: "none; a4 incorporated by replay", numeric effect undetermined (U2). Neither author states the Ref's frontier/coverage/high-water effect. |
| 23 | AGREE | AGREE | AGREE |
| 24 | AGREE | AGREE | AGREE |
| 25 | AGREE | AGREE | AGREE |
| 26 | AGREE | AGREE | AGREE |
| 27 | AGREE | AGREE | AGREE (Ref has no explicit replay effect; both authors add only "V-effect"/"a4 incorporated") |
| 28 | AGREE | AGREE | AGREE (same note as 27) |
| 29 | DIFFER. Ref: a0. Sonnet: a0. sol: undetermined (U29), a0 if RF stop-eligible, else a0,a2. | DIFFER. Ref: S(A,(305,8),RF;{Au,Auc3}). Sonnet: same. sol: undetermined (U29), same RF stop if eligible, else S(F0,747,BC;{(Au,Auc3)}). | AGREE (RF diag for both deliveries plus BC at 747 pair (Au,Auc3); Sonnet calls the BC suppressed, same content) |
| 30 | AGREE (r0,r1,r2) | AGREE (BC at 769 pair (R,Rc3)) | DIFFER. Ref: separate replay stop at (537,8); LIFE retained, no coverage. sol: replay stop at (537,8), effective cutoff there; no "LIFE retained, no coverage". Sonnet: replay stop at (537,8); r0,r1 replayed, r2 not applied (assumed, U4); no "LIFE retained, no coverage". Both authors omit the Ref's LIFE/coverage effect. |
| 31 | AGREE | AGREE | AGREE |

All-three-columns agreement: 28 of 31 cases (all except 22, 29, 30).

## Cells marked undetermined by an independent author

sol:
- U29, case 29 records and stops: clause 7 requires RF candidates "where the governing contract requires stopping" but does not say which structural faults qualify; two conditional outcomes given (RF stop with a0, or BC at 747 with a0,a2). Diagnostics determined.
- Exact certificate survival after VOID, cases 22, 27, 28 (replay effect): CONTRACT/r3 rules on revocation, certificates and F/H state were not supplied; only "a4 admitted, V-effect" stated.

Sonnet:
- U1, cases 17 and 31 diagnostics: unclear whether an unassigned header failure makes the whole multiset (A,Z) a failed decode or only Z; answered per-delivery for 17, whole failed decode for 31.
- U2, cases 22 (and 27, 28) replay effect: numeric effect of VOID invalid_from=20 needs CONTRACT.md (inclusive/exclusive boundary), not supplied; gave only "a4 retained and delivered to replay".
- U3, case 29 records/stops: eligibility of RF as a stop depends on the unsupplied contract; assumed unknown kind stops; alternative BC at 747 with a0,a1,a2 (note: differs from sol's alternative records a0,a2).
- U4, case 30 replay stop: no notation or reason name for replay stop; unclear whether faulting r2 contributes partial effects; assumed not applied.
- U5, cases 6 and 15 provenance: clause 10 literally gives {A} / {M}; Sonnet wrote the wider identical-bytes set {A,Ac2t3} / {M,Mc1t3}; narrower reading noted.
- U6, PU detail cells (cases 9, 19, 23, 24, 27, 28 and other PU stops): no fixed format for "available predecessor validation/link-check results", described in words only.
