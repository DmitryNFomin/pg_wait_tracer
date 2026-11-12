## Row 21
Output N: `VOID30[C0,seal_violation]`  
Output Q: `VOID30[C0,seal_violation]`  
Exit: no  
Reasoning: E’s accepted cutoff remains 20 after the weaker boundary 10. A15 is below that cutoff and is rejected, causing immediate VOID at detection time 30. Other cutoffs remain 0 before suspension, so nothing precedes VOID. Subsequent boundaries cannot rearm the epoch.

## Row 22
Output N: `A10 → F10`  
Output Q: `VOID20[C0,sequence_gap]`  
Exit: no  
Reasoning: A10’s implicit perf boundary does not release itself. Completing S(11) makes A10 eligible. N releases it and advances the frontier to 10. Q validates the complete eligible batch against next_q(A)=1; the missing q1 causes VOID at detection time 20.

## Row 23
Output N: `hold`  
Output Q: `hold`  
Exit: no  
Reasoning: CHECK observes the unchanged vector C0, so it reports no counter change. The epoch remains suspended without REARM, suppressing DATA, TEXT and FRONTIER outputs.

## Ambiguities
None.
