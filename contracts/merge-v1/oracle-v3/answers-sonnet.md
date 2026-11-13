# Answers (Sonnet), from CONTRACT.md only

Convention: W = min of the four cutoffs; F = W-1. Implicit boundary w=t from each qualified P-source DATA. Reason labels not named in the contract are written `<label?>`.

## Row 1
Output: A10 -> B12 -> F12 (N and Q identical)
Exit: no
Reasoning: B12@P1 gives w[P1]=12, A10@P0 gives w[P0]=10; W stays 0 (E, L at 0). S(13): P0, P1, E leave W=0; BOUNDARY(L,13) gives W=min(13,13,13,13)=13, F=12. Release t<13 sorted by t: A10 (t=10), B12 (t=12). Q: A q1, B q1, each starts at next_q=1, valid.

## Row 2
Output: A10@P1 -> B10@P0 -> F10 (N and Q identical)
Exit: no
Reasoning: Implicit w[P0]=w[P1]=10. S(11) reaches W=11 only at BOUNDARY(L,11), F=10. Same t=10, so order by pid: A(10) before B(20). Different generations, so no tie issue and no cross-source ambiguity. Q: each generation's q=1.

## Row 3
Output: F9 -> A10 -> F10 (N and Q identical)
Exit: no
Reasoning: A10@P0 gives w[P0]=10. S(10): W becomes 10 only at L (P0 stays 10 via max). Empty eligible batch (t<10 none), F=9 advances, so F9. S(11): W stays 10 until BOUNDARY(L,11), then W=11, release A10, F=10 advances, so F10. No repeats of non-advancing frontiers.

## Row 4
Output: F9 -> A10 -> F11 (N and Q identical)
Exit: no
Reasoning: S1(20), SE(20), SL(20) with P0=0 gives W=0, nothing. A10@P0: w[P0]=10, W=min(10,20,20,20)=10, F=9; empty batch (A10 is not t<10) still permits advancing frontier, so F9. A12@P0: w[P0]=12, W=12, F=11; release A10 (t<12); A12 is held. Q: A10 q1, A12 q2; A10 is q1 at next_q 1, valid.

## Row 5
Output: hold
Exit: no
Reasoning: A10@P0 gives w[P0]=10; S0(100) gives w[P0]=100. P1, E, L are unbounded (cutoff 0), so W=0: stage only, emit no frontier. Same for Q.

## Row 6
Output: A10 -> F99 (N and Q identical)
Exit: no
Reasoning: After S0, S1, SE, W is still 0 because L is 0. SL(100): W=min(100,100,100,100)=100, F=99. Release A10 (t=10<100), then F99.

## Row 7
Output: A11 -> B12 -> F12 (N and Q identical)
Exit: no
Reasoning: E has no implicit boundary (shared ring, not qualified perf); the B12 then A11 inversion on E is sortable and not a clock fault. S(13): W=13 only at L, F=12. Sort by t: A11 before B12. Q: A q1, B q1.

## Row 8
Output: TEXTx -> A10 -> F10 (N and Q identical)
Exit: no
Reasoning: TEXT is untimed and staged. W=0 until BOUNDARY(L,11) (P0 =max(10,11)=11), then W=11, F=10. Dependency x is available (TEXT came first), so pending TEXT is output first, then eligible DATA A10, then F10. Q: A10 q1.

## Row 9
Output: FORK_A10 -> INIT_A10 -> TRANS_A10 -> EXIT_A10 -> F10 (N and Q identical)
Exit: no
Reasoning: All four have t=10, same pid, g. Sort key rank: FORK=0, INIT=1, TRANS=2, EXIT=3 (terminal TRANS precedes its EXIT). Sources differ (L, E) but only one TRANS/MARKER exists, so there is no cross-source tie. S(11): W=11 at L, F=10. Q: the TRANS has q1; lifecycle records take no q.

## Row 10
Output: enter X (o=1) -> leave X (o=2) -> F10 (N and Q identical)
Exit: no
Reasoning: Both TRANS at t=10 from P0; equal timestamps are not a regression and not below the boundary. Same-source tie keeps source order in N (o=1 then o=2); in Q q1 then q2 gives the same order. Distinct physical records, so no dedup. W=11 at L, F=10.

## Row 11
Output (Q): MARKER_A10@E(q3) -> TRANS_A10@P0(q4) -> F10
Output (N): VOID20[C0, ambiguous_tie]
Exit: no
Reasoning: Q: at S(11)/L W=11, F=10. Sort key (t,pid,g,rank,q,s,o): both rank 2, q orders q3 (E) before q4 (P0). Values 3,4 are consecutive from next_q=3, so no gap (q4 arriving first is not a gap); next_q becomes 5. N (starting state is Q, so N is not clearly applicable): a same-generation TRANS/MARKER tie spanning sources P0/E gives ambiguous_tie VOID at the validation on BOUNDARY(L,11), d=20. AMBIGUOUS (N applicability).

## Row 12
Output (Q): "A first" TRANS10@P1(q1) -> "A second" TRANS10@P0(q2) -> F10
Output (N): VOID20[C0, ambiguous_tie]
Exit: no
Reasoning: Q: both rank 2, sort by q: q1 (P1) before q2 (P0); consecutive from next_q=1, valid; W=11 at L, F=10. N: transition/transition cross-source same-generation tie is ambiguous_tie, VOID at d=20 on the release evaluation; nothing released, no frontier. AMBIGUOUS (N applicability).

## Row 13
Output: A4 -> F5 -> VOID9[Ce, <label?>]
Exit: no
Reasoning: A4@P0 gives w[P0]=4; S(6) reaches W=6 at L, F=5; the counters still match baseline C0 at that moment, so release A4 then F5. The helper failure at time 8 changes C to Ce; CHECK(d=9,C=Ce) reports a newly observed change once: VOID(9,Ce,reason). The reason label for a counter change is not named in the contract. AMBIGUOUS (label only).

## Row 14
Output: VOID12[Ce, <label?>]
Exit: no
Reasoning: The epoch is suspended; each separately delivered LOSS causes a VOID even if already suspended. d=12 and C=Ce are taken from the report, not from t/w. No DATA/TEXT/FRONTIER while suspended; attachments stay frozen at 5. No REARM so no recovery. Label for loss not specified. AMBIGUOUS (label only).

## Row 15
Output: VOID9[Ce, <label?>]
Exit: no
Reasoning: Active epoch, cutoffs 6, F5 already published. Transport silence so no LOST callback; CHECK(d=9,C=Ce) sees the new vector and reports it once, giving VOID9 with Ce. Tentative suffix discarded, epoch suspended, attachments frozen at F5. Label not named. AMBIGUOUS (label only).

## Row 16
Output: VOID20[C0, clock_regression]
Exit: no
Reasoning: A12@P0 sets w[P0]=12. A11@P0 is qualified perf DATA with t=11 below the preceding consumed DATA timestamp 12, so clock_regression (checked before seal_violation). Offender rejected, VOID at d=20 with C0. No frontier was ever published, so no epoch invalidation. Q: irrelevant (A11 would be q2).

## Row 17
Output: F10 -> VOID12[C0, seal_violation]
Exit: no
Reasoning: S(11): W=11 at L, empty batch, F=10 published. A9@P1: not a duplicate; no preceding P1 DATA, so not clock_regression; t=9 < accepted w[P1]=11, so seal_violation. Reject, VOID at d=12. Because t=9 <= published frontier 10, additionally mark and report the epoch invalid out of band (not an output record).

## Row 18
Output: F10 -> A11 -> F11 (N and Q identical)
Exit: no
Reasoning: S(11): W=11 at L, F10. A11@P1: t=11 not below w[P1]=11, accepted, w[P1] stays 11; W=11, A11 not t<11, no release, no new frontier. S(12): W stays 11 until L(12), then W=12, release A11, F=11 advances.

## Row 19
Output: VOID9[Cw, <label?>]
Exit: no
Reasoning: A7@P0 gives w[P0]=7; S(8) reaches W=8 at L, release is attempted for A7 (7<8). Pre-release counters read C0, but the writer rejects A7 at d=9, incrementing C to Cw. A rejected writer call is not a DATA output; no frontier certifies a failed release, so no F7. VOID(9,Cw,reason); F5 remains the last valid frontier. Label not named. AMBIGUOUS (label only).

## Row 20
Output: VOID20[C0, <label?>]
Exit: yes (nonzero)
Reasoning: The second envelope has the same source position o=1 as the first: duplicate transport identity is checked first and is an internal invariant breach. Reject the duplicate, stop ordinary output, attempt VOID(20,C0,reason), exit nonzero regardless of persistence success. No DATA released (P1, E, L at 0, W=0). N and Q identical (q1 both). Label not named. AMBIGUOUS (label only).

## Ambiguities

1. Rows 13, 14, 15, 19, 20: the contract defines the form `VOID(d,C,reason)` but names reason strings only for clock_regression, seal_violation, ambiguous_tie and sequence_gap. Reasons for counter change (CHECK), perf LOSS, writer failure and duplicate identity are unspecified (readings: a free-form diagnostic vs a fixed token such as loss / writer_push_fail / duplicate). Structure, d and C are determined; the label is not. Marked AMBIGUOUS.
2. Rows 11, 12: the starting state is Q-specific, yet the header says "answer both modes". Reading A: N is not applicable. Reading B: N has no q, so the cross-source same-generation TRANS/MARKER tie gives ambiguous_tie VOID20[C0]. Marked AMBIGUOUS (N part only; Q answers are determined).
3. Row 17 (non-blocking): "preceding consumed DATA timestamp" for clock_regression is read per source; if it meant a global previous DATA, A9@P1 (after no DATA at all) is still not a regression, so the answer is the same.
4. Row 13 (non-blocking): the contract does not state whether the helper failure at time 8 could affect release at S(6); read as ordered by input sequence (release first, then CHECK), since the contract processes inputs sequentially.
5. Row 4 (non-blocking): an empty-batch F9 is emitted after A10 arrives (W 0 to 10); read per "An empty eligible batch still permits an advancing FRONTIER".
