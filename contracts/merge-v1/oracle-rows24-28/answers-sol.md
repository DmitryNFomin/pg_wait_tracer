## Row 24

Output N: `F10 → VOID12[Ce,counter_change] → VOID13[Ce,seal_violation]`  
Output Q: `F10 → VOID12[Ce,counter_change] → VOID13[Ce,seal_violation]`  
Exit: no  
Reasoning: `S(11)` publishes F10. CHECK detects the counter change and suspends the epoch. A9 violates P1’s accepted boundary 11; fault reporting continues during suspension. Reject A9 and mark/report the epoch invalid because 9 ≤ F10.

## Row 25

Output N: `FORK_D10 → INIT_D10 → D10 → F10`  
Output Q: `FORK_D10 → INIT_D10 → D10 → F10`  
Exit: no  
Reasoning: Valid FORK and INIT establish D’s creation and, under Decision 25, initialize `next_q(D)=1` before batch validation. D’s q1 passes. At W=11, rank order places FORK and INIT before TRANS.

## Row 26

Output N: `TEXTx → A10 → F10 → F49`  
Output Q: `TEXTx → A10 → F10 → F49`  
Exit: no  
Reasoning: TEXT x is available before A10 becomes eligible. At W=11, publish TEXTx before A10, then F10. `S(50)` advances the frontier to F49 with an empty eligible batch.

## Row 27

Output N: `F10 → VOID15[Ce,perf_lost] → VOID20[Ce,seal_violation]`  
Output Q: `F10 → VOID15[Ce,perf_lost] → VOID20[Ce,seal_violation]`  
Exit: no  
Reasoning: LOSS suspends e0 after F10. Accepted REARM resets release cutoffs but retains P1’s accepted boundary 11. A9 violates that boundary, suspends the new epoch, and additionally invalidates e0. Subsequent boundaries produce no output while suspended.

## Row 28

Output N: `A10 → F10`  
Output Q: `A10 → F10`  
Exit: no  
Reasoning: REARM in an active epoch is rejected without output or state change. A10 remains staged, and Q retains `next_q(A)=1`. `S(11)` makes A10 eligible; q1 passes validation.

## Ambiguities

None.
