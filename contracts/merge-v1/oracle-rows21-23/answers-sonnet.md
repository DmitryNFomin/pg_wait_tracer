## Row 21
Output N: VOID30[C0,seal_violation]
Output Q: VOID30[C0,seal_violation]
Exit: no
Reasoning: SE(20) sets w[E]=20; SE(10) is weaker, max keeps 20 (no retraction), W stays 0 so nothing released.
A15@E is E (shared ring, not qualified perf) DATA with t=15 < accepted boundary 20, so seal_violation (not clock_regression); DATA rejected, VOID at d=30.
No frontier was ever published, so no epoch invalidation. Epoch is now suspended.
S(21) at d=31 are boundaries, not fault reports: no new VOID, and FRONTIER/DATA are suppressed while suspended. Same in N and Q.

## Row 22
Output N: A10 -> F10
Output Q: VOID20[C0,sequence_gap]
Exit: no
Reasoning: N: single record, no ties; P0 DATA at 10 gives w[P0]=10, S(11) raises all to 11, W=11, release A10 then F10.
Q: next_q(A)=1 but A's only record has q2, so the first q is not next_q. Higher q arriving first is not a gap by itself, but at complete eligible-batch validation (when W reaches 11 on the last S(11) boundary) the missing q1 gives sequence_gap.
VOID uses d=20 (applies to S(11) constituents), C0; nothing released, no frontier.

## Row 23
Output N: hold
Output Q: hold
Exit: no
Reasoning: Epoch is suspended; CHECK(d=10,C0) observes the same vector as the last observation (C0), so no newly observed change and no report ("rereading the same vector does not generate another report").
No VOID, no frontier (suspended suppresses them anyway), no recovery without REARM. Same in N and Q.

## Ambiguities
- Row 21: whether a weaker SE(10) after SE(20) could itself be a fault is read as no (contract: equal or weaker boundary does not retract). Alternative reading (not supported by text): VOID on regress. Also assumed S(21) while suspended yields no output.
