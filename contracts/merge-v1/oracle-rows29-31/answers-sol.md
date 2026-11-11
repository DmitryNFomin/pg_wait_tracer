## Row 29

Output N: `VOID12[C0,unknown_evidence]`  
Output Q: `VOID12[C0,unknown_evidence]`  
Exit: no  
Reasoning: At `S(10)`, D9 becomes eligible before FORK_D10 and INIT_D10. DECISION 25a requires those lifecycle records to precede D’s first TRANS in validated output order; otherwise D is `unknown_evidence` in both modes. The epoch suspends, so `S(11)` produces no output.

## Row 30

Output N: `F10 → VOID15[Ce,perf_lost]`  
Output Q: `F10 → VOID15[Ce,perf_lost]`  
Exit: no  
Reasoning: Accepted REARM resets release cutoffs while preserving the published F10. Fresh `S(5)` gives W=5, but F4 cannot advance F10 and is suppressed. Weaker boundaries cause no fault. REARM is accepted; the contract requires no named success status.

## Row 31

Output N: `VOID12[Ce,counter_change]`  
Output Q: `VOID12[Ce,counter_change]`  
Exit: no  
Reasoning: REARM is rejected because the epoch is active, and its feed call returns `rearm_rejected`. Its counter observation must still be processed as CHECK. Ce differs from C0, causing the VOID and suspension.

## Ambiguities

None.
