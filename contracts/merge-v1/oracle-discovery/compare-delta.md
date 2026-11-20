# Oracle2 comparison (sol vs opus vs EXPECTED)

Counts: ALL-AGREE 33; ANSWERS-AGREE-EXPECTED-DIFFERS 0; ANSWERS-DIFFER 1 (A15).

| ID | class | difference |
|---|---|---|
| D01 | ALL-AGREE | none |
| D02c | ALL-AGREE | none |
| D03 | ALL-AGREE | none |
| D09 | ALL-AGREE | none |
| D10 | ALL-AGREE | none |
| Q02 | ALL-AGREE | none |
| Q03 | ALL-AGREE | none |
| Q04 | ALL-AGREE | none |
| Q04b | ALL-AGREE | none |
| Q05 | ALL-AGREE | none |
| Q06 | ALL-AGREE | none |
| D14a | ALL-AGREE | none |
| D14b | ALL-AGREE | none |
| D14c | ALL-AGREE | none |
| A01 | ALL-AGREE | none |
| A02 | ALL-AGREE | none |
| A03 | ALL-AGREE | none |
| A04 | ALL-AGREE | none |
| A05 | ALL-AGREE | none |
| A06 | ALL-AGREE | none |
| A07 | ALL-AGREE | none |
| A08 | ALL-AGREE | none |
| A09 | ALL-AGREE | none |
| A10 | ALL-AGREE | none |
| A11 | ALL-AGREE | none |
| A12 | ALL-AGREE | none |
| A13 | ALL-AGREE | none |
| A14 | ALL-AGREE | none |
| A15 | ANSWERS-DIFFER | sol: terminal discharge x_D=18 accepted, pending=0; opus: not accepted, pending=1. EXPECTED matches opus (P=1) |
| A16 | ALL-AGREE | none |
| A17 | ALL-AGREE | none |
| A18 | ALL-AGREE | none |
| A19 | ALL-AGREE | none |
| A20 | ALL-AGREE | none |

Notes (not differences): D09/A17 EXPECTED mentions R's Decision 25c restriction; sol states R's direct-fork lower bound 22, opus cites 25c marking; same substance. Opus's "Rows resting on reading" list is commentary only.

## A15

SOL: A15: F12 → F15 → F17 → VOID20[Cw,writer_push_fail]. EXIT_D18 is rejected; no F18. Suspended. b_D absent; terminal discharge x_D=18 is accepted. Startup gap remains open with no persisted close; pending=0; census complete. discovery_abandoned_records=3. No UNTRACED.
sol why: DECISIONs 40–42 and 45 distinguish accepted terminal discharge from successful closure publication. The completed terminal discard remains counted, but rejected supporting EXIT prevents gap closure.

OPUS: A15: A01's outputs (F12 -> F15), S(18) -> F17; S(19) L: D18 discard completes (counted), EXIT_D18 rejected -> VOID20[Cw,writer_push_fail]; no F18; capture paused. Terminal discharge not accepted (publication failed), gap not closed. discovery_abandoned_records=3. Gap open, pending 1.
opus why: D45 ("Count each completed discard once even if a subsequent publication ... fails"; EXIT under successful-publication rules), D41 (publication failure commits no closure), §4 writer failure.

EXPECTED: | A15 | `F12 → F15 → F17 → VOID20[Cw,writer_push_fail]`. The terminal TRANS was consumed, so `abandoned=3`; the rejected EXIT is not a successful output. `G(open), P=1`; no F18, committed terminal discharge or gap closure. |

Row (ROWS.md): A15 | A03's inputs, except the writer rejects EXIT_D18 at detection time 20 and observes Cw. Other publications, including VOID, succeed.

Sentences of AMENDMENT.md:
- Sol side (D40): "For an unresolved census lifetime, a complete matching kernel terminal proof at `x_i` may discharge its inventory entry without establishing numerical evidence." and (D41) "Each inventoried lifetime remains unresolved until it supplies either a committed lower bound `b_i` or an accepted terminal discharge `x_i`."
- Opus side (D45): "Its matching lifecycle EXIT is released normally under strict eligibility and successful-publication rules; it is not discarded." and "Count each completed discard once even if a subsequent publication in the same release attempt fails."
- Not resolved here.
