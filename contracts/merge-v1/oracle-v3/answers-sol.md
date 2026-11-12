`DATA(...)` denotes the complete, unchanged input record, including its sequence value in Q. Reasons inside VOID brackets are descriptive.

## Row 1
Output: N/Q: DATA(A10@P0) → DATA(B12@P1) → F12.  
Exit: no  
Reasoning: Once S(13) completes, W=min(13,13,13,13)=13; F=13−1=12. Timestamp order releases A before B.

## Row 2
Output: N/Q: DATA(A10@P1) → DATA(B10@P0) → F10.  
Exit: no  
Reasoning: W=min(11,11,11,11)=11; F=11−1=10. Equal timestamps sort by pid, so A precedes B.

## Row 3
Output: N/Q: F9 → DATA(A10@P0) → F10.  
Exit: no  
Reasoning: S(10) gives W=10, F=10−1=9; timestamp 10 is ineligible. S(11) gives W=11, F=11−1=10, releasing A.

## Row 4
Output: N/Q: F9 → DATA(A10@P0) → F11.  
Exit: no  
Reasoning: A10 supplies W=min(10,20,20,20)=10; F=10−1=9. A12 then supplies W=min(12,20,20,20)=12; F=12−1=11, releasing only A10.

## Row 5
Output: N/Q: hold.  
Exit: no  
Reasoning: W=min(100,0,0,0)=0. No DATA or frontier can be released.

## Row 6
Output: N/Q: DATA(A10@P0) → F99.  
Exit: no  
Reasoning: W stays 0 until SL(100), then W=min(100,100,100,100)=100; F=100−1=99.

## Row 7
Output: N/Q: DATA(A11@E) → DATA(B12@E) → F12.  
Exit: no  
Reasoning: The shared-ring inversion involves unrelated producers and is sortable. S(13) gives W=13; F=13−1=12.

## Row 8
Output: N/Q: TEXT(x@L) → DATA(A10@P0) → F10.  
Exit: no  
Reasoning: Both remain staged while W=0. S(11) gives W=11; F=11−1=10. The available TEXT dependency is published before DATA.

## Row 9
Output: N/Q: DATA(FORK_A10@L) → DATA(INIT_A10@L) → DATA(terminal TRANS_A10@E) → DATA(EXIT_A10@L) → F10.  
Exit: no  
Reasoning: The complete batch sorts by lifecycle rank: FORK, INIT, TRANS, EXIT. W=11; F=11−1=10.

## Row 10
Output: N/Q: DATA(A enters X at 10@P0) → DATA(A leaves X at 10@P0) → F10.  
Exit: no  
Reasoning: N retains same-source order; Q uses q1 then q2. Both physical records survive, preserving the zero duration. W=11; F=11−1=10.

## Row 11
Output: N: VOID20[C0,ambiguous_tie]. Q: DATA(MARKER_A10@E,q3) → DATA(TRANS_A10@P0,q4) → F10.  
Exit: no  
Reasoning: W=11; F=11−1=10. N refuses the same-generation cross-source tie and publishes no frontier. Q orders q3 before q4, consecutive from next_q=3.

## Row 12
Output: N: VOID20[C0,ambiguous_tie]. Q: DATA(A first TRANS10@P1,q1) → DATA(A second TRANS10@P0,q2) → F10.  
Exit: no  
Reasoning: W=11; F=11−1=10. N refuses the cross-source transition tie. Q validates q1,q2 in prospective output order; q2’s earlier arrival is not itself a gap.

## Row 13
Output: N/Q: DATA(A enters X at 4@P0) → F5 → VOID9[Ce,counter change].  
Exit: no  
Reasoning: S(6) gives W=6; F=6−1=5. CHECK detects the subsequent failure at d=9 and suspends the epoch without fabricating an exit from X.

## Row 14
Output: N/Q: VOID12[Ce,positive LOSS].  
Exit: no  
Reasoning: Each separately delivered LOSS emits VOID even while suspended and even when Ce was already observed. Existing F5=6−1 is not reprinted.

## Row 15
Output: N/Q: VOID9[Ce,counter change].  
Exit: no  
Reasoning: CHECK detects the failure during transport silence. Existing W=6 gives F=6−1=5, already published; no new frontier appears.

## Row 16
Output: N/Q: VOID20[C0,clock_regression].  
Exit: no  
Reasoning: P0’s timestamp decreases from 12 to 11. The perf clock-regression check takes precedence over the boundary check. Other cutoffs remain 0, so W=0 and no frontier exists.

## Row 17
Output: N/Q: F10 → VOID12[C0,seal_violation].  
Exit: no  
Reasoning: S(11) gives W=11; F=11−1=10. A9 violates P1’s accepted boundary; because 9≤10, the epoch is additionally marked and reported invalid out of band.

## Row 18
Output: N/Q: F10 → DATA(A11@P1) → F11.  
Exit: no  
Reasoning: S(11) gives W=11; F=11−1=10. A11 equals its boundary and remains staged. S(12) gives W=12; F=12−1=11, releasing A11.

## Row 19
Output: N/Q: VOID9[Cw,writer failure].  
Exit: no  
Reasoning: Existing F5=6−1 is not repeated. S(8) eventually gives W=8 and candidate F=8−1=7, but rejecting A7 produces no DATA output or frontier; capture pauses.

## Row 20
Output: N/Q: VOID20[C0,duplicate transport identity].  
Exit: yes  
Reasoning: Duplicate identity is checked first and constitutes an internal invariant breach. The first record remains staged at W=0; the duplicate is rejected, VOID succeeds, and the daemon exits nonzero.

## Ambiguities

None. All 20 rows determine the requested output order and exit behavior in both modes.
