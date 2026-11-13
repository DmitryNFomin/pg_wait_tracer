## Row 29
Output N: VOID12[C0,unknown_evidence]
Output Q: VOID12[C0,unknown_evidence]
Exit: no
Reasoning: Sort key is t first, so D9 (t=9) sorts before FORK_D10 and INIT_D10 (t=10).
DECISION 25a: the proof takes effect only if FORK and INIT precede the generation's first TRANS in validated output order. They do not.
D has no attachment proof, so D is unknown_evidence in both N and Q ("in both N and Q modes").
At BOUNDARY(L,10) of S(10), W=10 and D9 (t=9) is eligible. Batch validation fails, giving VOID with d=12 and C0. No F9 and no DATA is emitted.
S(11) then arrives while suspended: output is suppressed and nothing new faults. Same answer if detection were deferred to S(11).

## Row 30
Output N: F10 -> VOID15[Ce,perf_lost]   (REARM accepted, no output; S(5) no output)
Output Q: F10 -> VOID15[Ce,perf_lost]   (same)
Exit: no
Reasoning: The first three inputs follow row 27: S(11) gives F10, LOSS gives VOID15[Ce,perf_lost], and REARM is accepted with baseline Ce (epoch e1 active, cutoffs 0).
S(5) is only BOUNDARY input; the max accepted boundary (11) is retained, and weaker boundaries do not retract.
Cutoff can reach at most W=5, so F=4. That is at or below the published F10 and never repeats or regresses.
There is no DATA, so no seal_violation. C=Ce equals the readable baseline Ce, so no counter_change. No output.

## Row 31
Output N: VOID12[Ce,counter_change]
Output Q: VOID12[Ce,counter_change]
Exit: no
Status: the REARM call returns `rearm_rejected`.
Reasoning: The epoch is active, so REARM is rejected with no state change (DECISION 28) and returns the distinct status rearm_rejected (28a).
Its C=Ce is still processed as CHECK(d=12,C=Ce).
Ce differs from the last observed C0, so this is a newly observed change: counter_change, VOID with d=12. The epoch suspends.

## Ambiguities
None blocking. Row 29 could detect the fault at S(10) or S(11), and row 30's S(5) could be read as keeping or lowering the cutoff, but the outputs are identical either way.
