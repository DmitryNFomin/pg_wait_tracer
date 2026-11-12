## Row 24
Output N: F10 -> VOID12[Ce,counter_change] -> VOID13[Ce,seal_violation]
Output Q: F10 -> VOID12[Ce,counter_change] -> VOID13[Ce,seal_violation]
Exit: no
Reasoning: S(11) gives W=11 after the last boundary, so the empty batch emits F10. CHECK sees Ce != C0, which gives VOID12 and suspends the epoch.
A9@P1 has 9 < accepted P1 boundary 11, so it is a seal_violation. Fault reporting continues while suspended, so VOID13 with the observed vector Ce. The counter checkpoint is already Ce, so no second counter_change.
9 <= published frontier 10, so A9 is rejected and the epoch is also marked invalid out of band (not an output record). Same in N and Q.

## Row 25
Output N: FORK10 -> INIT10 -> TRANS10 -> F10
Output Q: FORK10 -> INIT10 -> TRANS10 -> F10
Exit: no
Reasoning: D is newly created this epoch by valid FORK and INIT, so per DECISION 25 next_q(D)=1 is initialized before batch validation.
D's TRANS has q1, which is consecutive from 1, so there is no sequence_gap. Ranks FORK=0, INIT=1, TRANS=2 give the order. No cross-source TRANS/MARKER tie.
W=11 after S(11), so the batch is released, then F10. The same applies in N.

## Row 26
Output N: TEXTx -> A10 -> F10 -> F49
Output Q: TEXTx -> A10 -> F10 -> F49
Exit: no
Reasoning: Before S(11) completes, W=0, so A10 and TEXT x are staged with no output. After the last S(11) input W=11 and x is available.
Release: pending TEXT first, then DATA A10 (q1 valid), then F10. S(50) makes W=50, F=49, an empty batch with an advancing frontier, so F49.

## Row 27
Output N: F10 -> VOID15[Ce,perf_lost] -> VOID20[Ce,seal_violation]
Output Q: F10 -> VOID15[Ce,perf_lost] -> VOID20[Ce,seal_violation]
Exit: no
Reasoning: S(11) gives F10 in e0. LOSS gives VOID15 and suspends. REARM is accepted (suspended), with no output; cutoffs reset to 0 but the maximum accepted boundary 11 is kept.
A9@P1 at 9 < retained P1 boundary 11 gives seal_violation, VOID20 with C=Ce (matches the new baseline, so no counter_change). A9 is rejected and the current epoch is suspended again.
9 <= e0's published F10, so e0 is additionally marked invalid out of band (a report, not an output record).
S(12) arrives in a suspended epoch: frontiers are suppressed, so no output.

## Row 28
Output N: A10 -> F10
Output Q: A10 -> F10
Exit: no
Reasoning: A10 is staged at W=0 with no output. REARM in an active epoch is rejected (DECISION 28) with no output and no state change.
That keeps the staged A10, the cutoffs, the baseline and next_q(A)=1; the proposed next_q=2 is ignored. S(11) then gives W=11, releases A10 (q1 is consecutive from 1), then F10.

## Ambiguities
None.
