| # | Fully specified input / starting state | Expected output |
|---|---|---|
| 21 | Start active: all cutoffs 0, no frontier, empty queues; baseline and last observed C0; Q next values A=1, B=1. `SE(20), SE(10)`, then `A15@E` consumed at `d=30,C=C0`, then `S(21)` observed at `d=31,C=C0`. | `VOID30[C0,seal_violation]`; reject A15 and suspend; no DATA or frontier. |
| 22 | Start active: all cutoffs 0, no frontier, empty queues; baseline and last observed C0; Q next values A=1, B=1. `A10@P0(Q:q2), S(11)` observed at `d=20,C=C0`. | N: `A10 → F10`. Q: `VOID20[C0,sequence_gap]`; no DATA or frontier. |
| 23 | Start suspended: all cutoffs 6, no published frontier, empty queues, A/B frozen with no valid frontier; baseline and last observed C0; Q next values A=1, B=1. `CHECK(d=10,C=C0)`. | `hold`; unchanged counters cause no new VOID, and suspension suppresses FRONTIER. |

M6 — Row 21 separates retracting an accepted boundary: the broken output is `A15 → F20` in both modes.

M11 — Row 22 separates accepting an eligible sequence gap: the broken Q output is `A10 → F10`; N is unchanged.

M12 — Row 23 separates frontier publication during suspension: the broken output is `F5` in both modes.
