# Independent answer set (sol): merge-v1 delta 7

Written from the amended contract, ROWS.md and the TASK alone. Two packets: the first on delta 7 as first written (28 executions H1a-H10b), the second, by a fresh answerer, on the text after settlements 7b and 7c (34 selected executions including H11a-H11c).

## First packet (delta 7 as first written; rows H1a-H10b)

All conclusions below use only the four files in the current directory.

Notation and shared rules:

- `F(n,C)` means `FRONTIER(n,C)`. `V(d,C,r)` means `VOID(d,C,r)`.
- `[U]` means `prior_interval_certified=false`; `[T]` means true. This marking does not apply to lifecycle EXIT.
- `s(w,p)` means the primitive `BOUNDARY(s,w,p)`. Boundary lists below are processed individually, from left to right.
- Every execution begins with persisted startup `OPEN(0)`. Unless overridden, the initial state is: no recorded admission, no bound or discharge, pending `1=1+0`, census known through fence 5, latch off, epoch e0 active.
- State not changed by a listed primitive remains unchanged. Every first fault turns the latch on permanently and suspends its epoch. Successful REARM starts e1 with release cutoffs zero while retaining arrival-validation history.
- Every reported fault increments its own diagnostic by exactly one. Unknown-evidence and ordering faults add nothing to the 15 storage counters. `K` observes the supplied change to `Cr`, whose `wait_read_fail` component is one.
- Abandonment is unaccepted, `discovery_abandoned_records=0`, and uncovered consumption is zero in every execution except the expressly stated abandonment in H3d. No TEXT or UNTRACED is published anywhere below.
- None of these executions requires epoch invalidation or nonzero exit. Revocation of discovery coverage and invalidation of cached numerical answers are distinguished from merge-epoch invalidation.

### H1a

**Merge stream:** `EXIT_D18 → F(18,C0) → V(20,C0,unknown_evidence)`.

1. `TERM_D18` records the matching terminal claim; no discharge yet.
2. `EXIT_D18@L`, position 1, stages the EXIT.
3. `P0(19,1)`, `P1(19,1)`, `E(19,1)` each leave `W=0`, producing nothing.
4. `L(19,2)` establishes `W=19`: EXIT and F18 publish successfully; discharge `x_D=18` commits. Startup `END(18)` commits.
5. `AD_D19` contradicts the independently bound EXIT timestamp. Reject it before recording admission. Increment `unknown_evidence` by one and publish the VOID.

The independently supported terminal discharge remains valid: the rejected ADMIT supplies no proof supporting that discharge and does not refute its independent terminal binding. There is no revoked OPEN.

**Final:** `admitted=false`; no numerical bound; `x_D=18`; pending `0=0+0`; census known; latch on; e0 suspended; persisted gap remains `[0,18)`. One ordinary EXIT was published.

### H1b

**Merge stream:** `V(20,C0,unknown_evidence)`.

1. `AD_D19` is initially recorded: `admitted=true`, with no committed bound.
2. `EXIT_D18@L`, position 1, establishes that enable completion 19 followed this lifetime’s exit at 18. Increment `unknown_evidence` by one; refute the recorded admission proof and suspend e0.
3. `P0(20,1)`, `P1(20,1)`, `E(20,1)`, `L(20,2)` each produce nothing during suspension.

No proof had committed, so there is no revoked OPEN or census reset.

**Final:** historical `admitted=true`, admission proof refuted; no bound or discharge; pending `1=1+0`; census known; latch on; e0 suspended; startup OPEN remains unclosed. EXIT publication and consumption counts are zero.

### H1c

**Merge stream:** `EXIT_D18 → F(18,C0)`.

1. `TERM_D18` records the matching claim.
2. `EXIT_D18@L`, position 1, stages.
3. `P0(19,1)`, `P1(19,1)`, `E(19,1)` produce nothing, each with `W=0`.
4. `L(19,2)` releases EXIT and F18; `x_D=18` and startup `END(18)` commit.
5. `AD_D18` is valid and recorded. Equality of enable and EXIT timestamps is expressly permitted. The previously committed discharge remains the resolving proof; no bound commits.

**Final:** `admitted=true`, unrefuted admission; `x_D=18`; no bound; pending `0=0+0`; census known; latch off; e0 active; gap `[0,18)`. No fault.

### H1d

**Merge stream:** `EXIT_D18 → F(18,C0)`.

1. `AD_D18` records valid admission without establishing a bound.
2. `TERM_D18` records matching terminal evidence.
3. `EXIT_D18@L`, position 1, stages.
4. `P0(19,1)`, `P1(19,1)`, `E(19,1)` each produce nothing.
5. `L(19,2)` releases EXIT and F18. No numerical bound is committable, so discharge `x_D=18` and `END(18)` commit.

**Final:** `admitted=true`, unrefuted admission; no bound; `x_D=18`; pending `0=0+0`; census known; latch off; e0 active; gap `[0,18)`. No fault.

### H2a

**Merge stream:** `V(20,Cr,counter_change) → F(24,Cr) → D26[U] → F(26,Cr)`.

1. `K` observes C0→Cr: `counter_change +1`; latch on; e0 suspended. Census remains known because `wait_read_fail` is outside the lifecycle-loss class.
2. `RA` starts e1 active with baseline Cr.
3. `P0(25,1)`, `P1(25,1)`, `E(25,1)` produce nothing.
4. `L(25,1)` establishes `W=25` and publishes F24 at detection time 22.
5. `AD_D6` is validly recorded under the latch. `admitted=true`; no bound commits.
6. `D26@P0`, position 2, establishes implicit cutoff 26. The other cutoffs remain 25, so nothing advances.
7. `P0(27,3)`, `P1(27,2)`, `E(27,2)` each leave `W=25`, producing nothing.
8. `L(27,2)` establishes `W=27`: publish D26[U], then F26, at detection time 22.

**Final:** admission recorded and unrefuted; no committed bound or discharge; pending `1=1+0`; census known; latch on; e1 active; startup OPEN remains unclosed. D26’s refused discovery commitment cannot certify its incoming interval.

### H2b

**Merge stream:** `V(20,Cr,counter_change) → F(24,Cr) → D26[U] → F(26,Cr)`.

1. `K`: `counter_change +1`, latch on, e0 suspended; census retained.
2. `AD_D6` is validly recorded during suspension. This sets `admitted=true` and removes the missing-ADMIT dependency without rearming.
3. `RA` starts e1 active, baseline Cr, including D’s continuing attachment proof.
4. `P0(25,1)`, `P1(25,1)`, `E(25,1)` produce nothing; `L(25,1)` publishes F24.
5. `D26@P0`, position 2, stages without advancing F24.
6. `P0(27,3)`, `P1(27,2)`, `E(27,2)` produce nothing; `L(27,2)` publishes D26[U] and F26.

All post-REARM outputs occur at detection time 22.

**Final:** the same commitments, pending counts, census, latch and gap state as H2a; e1 active. No second fault.

### H2c

**Merge stream:** `V(20,Cr,counter_change)`.

1. `K`: `counter_change +1`; latch on; e0 suspended.
2. `AD_D6` is validly recorded during suspension: `admitted=true`.
3. `D26@P0`, position 1, cannot receive trusted release during suspension.
4. `P0(27,2)`, `P1(27,1)`, `E(27,1)`, `L(27,1)` each produce nothing.

**Final:** admission recorded and unrefuted; no bound or discharge; pending `1=1+0`; census known; latch on; e0 suspended; startup OPEN remains unclosed. No DATA publication or validated numerical consumption occurs.

### H2d

**Merge stream:** `V(20,Cr,counter_change) → V(22,Cr,unknown_evidence)`.

1. `K`: `counter_change +1`; latch on; e0 suspended.
2. `RA` starts e1 active.
3. `AD_D6` is validly recorded under the latch.
4. `AB_D` is invalid because admission has already been recorded. Reject abandonment; increment `unknown_evidence` by one; suspend e1.

**Final:** `admitted=true`, unrefuted numerical admission; no bound or discharge; pending `1=1+0`; census known; latch on; e1 suspended; startup OPEN remains unclosed. No abandonment is accepted and no revocation occurs.

### H2e

**Merge stream:** `V(20,Cr,counter_change) → V(22,Cr,unknown_evidence)`.

1. `K`: `counter_change +1`; latch on; e0 suspended.
2. `RA` starts e1 active.
3. `ADc6` records valid route-(c) admission under the latch. No CONFIRM or committed bound is supplied.
4. `AB_D` is invalid because admission was recorded. Increment `unknown_evidence` by one and suspend e1.

**Final:** historical `admitted=true`, unrefuted route-(c) admission; no bound or discharge; pending `1=1+0`; census known; latch on; e1 suspended; startup OPEN remains unclosed. No accepted abandonment or revoked OPEN.

### H3a

**Merge stream:** `V(20,C0,unknown_evidence)`.

1. `D12@P0`, position 1, stages and supplies implicit cutoff 12. Other sources remain at zero.
2. `ADc6` claims permanent no-wait-word provenance despite retained, independently attributable numerical DATA from D. Reject the control before recording admission; increment `unknown_evidence` by one.
3. `TRY_NW_D6` follows a rejected ADMIT, not a recorded ADMIT. The late-attempt rule therefore does not apply. It supplies no admission or bound and causes no additional fault.

There is no committed proof to revoke.

**Final:** `admitted=false`; no accepted route, bound or discharge; pending `1=1+0`; census known; latch on; e0 suspended; original startup OPEN only. D12 was not published or consumed.

### H3b

**Merge stream:** `V(20,C0,unknown_evidence)`.

1. `D12@P0`, position 1, stages.
2. `ADc6` is rejected for contradiction with D12: `unknown_evidence +1`.
3. `PTR_D14(non-NULL)` encounters no accepted route-(c) bound or recorded admission proof to refute. It records the pointer observation and can establish eligibility for a later numerical attachment; it supplies no bound or DATA and causes no second fault.

**Final:** `admitted=false`; no bound or discharge; pending `1=1+0`; census known; latch on; e0 suspended; startup OPEN unclosed.

### H3c

**Merge stream:** `V(20,C0,unknown_evidence)`.

1. `D12@P0`, position 1, stages.
2. `ADc6` is rejected for contradiction with D12: `unknown_evidence +1`.
3. `BREAK_D` cannot revoke an accepted route-(c) proof: the rejected ADMIT recorded none. No separate FAULT or counter failure is supplied. It produces no additional discovery-refutation report.

**Final:** `admitted=false`; no bound or discharge; pending `1=1+0`; census known; latch on; e0 suspended; startup OPEN unclosed. No revoked OPEN.

### H3d

**Merge stream:** `V(20,C0,unknown_evidence) → F(26,C0)`.

1. `D12@P0`, position 1, stages.
2. `ADc6` is contradicted by D12 and rejected: `unknown_evidence +1`. Its fault disposes of the tentative payload without validated abandonment consumption.
3. `AB_D` is accepted. There was no recorded ADMIT; the row independently supplies the installed identity and permanent end of unsuccessful attachment attempts.
4. `RA` starts e1 active. Abandonment and the latch survive.
5. `D26@P0`, position 2, is attributed to an earlier attempt. Abandonment permits its validated discard, subject to release.
6. `P0(27,3)`, `P1(27,1)`, `E(27,1)` produce nothing.
7. `L(27,1)` permits validated consumption of D26 without publication: `discovery_abandoned_records` becomes **1**. F26 publishes at detection time 22.

**Final:** `admitted=false`; abandonment accepted permanently; no bound or discharge; pending `1=1+0`; census known; latch on; e1 active; startup OPEN unclosed. D12 contributes no discard count; uncovered consumption remains zero.

### H4a

**Merge stream:** `V(20,C0,unknown_evidence) → A26[T] → F(26,C0)`.

1. `AD_D6` records valid numerical admission.
2. `TRY_NW_D6` is delivered after recorded admission. Increment `unknown_evidence` by one and reject the report without retaining its attempt or pointer history. Because no qualified non-NULL pointer fire was recorded, it also refutes the admission’s address proof. No bound had committed, so no revoked OPEN or census reset occurs.
3. `RA` starts e1 active.
4. `B_D6` records the supplied historical quiet candidate. It cannot restore the refuted admission proof or commit under the latch; it causes no repeated report of the already established admission contradiction.
5. `A26@P0`, position 1, stages.
6. `P0(27,2)`, `P1(27,1)`, `E(27,1)` produce nothing.
7. `L(27,1)` publishes A26[T] and F26 at detection time 22.

**Final:** historical `admitted=true`, numerical admission proof refuted; no bound or discharge; pending `1=1+0`; census known; latch on; e1 active; startup OPEN unclosed.

### H4b

**Merge stream:** `V(20,C0,unknown_evidence) → V(22,C0,unknown_evidence)`.

1. `AD_D6` records admission.
2. First `TRY_NW_D6`: mandatory late-attempt fault, `unknown_evidence +1`; reject the report and refute the numerical address proof. No committed bound exists.
3. `RA` starts e1 active.
4. `B_D6` records the quiet candidate without restoring the refuted admission or committing a bound.
5. Second `TRY_NW_D6`: Decision 38 expressly faults even a repeated historical report after recorded admission. Increment `unknown_evidence` by another one; retain no attempt or pointer history; suspend e1.
6. `P0(27,1)`, `P1(27,1)`, `E(27,1)`, `L(27,1)` each produce nothing during suspension.

**Final:** historical `admitted=true`, admission proof refuted; no bound or discharge; pending `1=1+0`; census known; latch on; e1 suspended; startup OPEN unclosed. Total `unknown_evidence=2`; no revoked OPEN.

### H5a

**Merge stream:** `V(20,Cr,counter_change) → V(22,Cr,unknown_evidence)`.

1. `B_D6{α2}` records a quiet candidate before ADMIT; it cannot commit.
2. `K`: `counter_change +1`; latch on; e0 suspended. The candidate survives as evidence.
3. `RA` starts e1 active.
4. `D14@P0`, position 1, belongs to earlier attachment α1. Its provenance-uncertified interval ends at 14, after candidate bound 6. This refutes the retained quiet claim: `unknown_evidence +1`; suspend e1.

The candidate never committed, so no revoked OPEN or census reset occurs.

**Final:** `admitted=false`; quiet candidate refuted; no bound or discharge; pending `1=1+0`; census known; latch on; e1 suspended; startup OPEN unclosed. D14 is not published or consumed.

### H5b

**Merge stream:** `V(20,Cr,counter_change) → V(22,Cr,unknown_evidence)`.

1. `K`: `counter_change +1`; latch on; e0 suspended.
2. `RA` starts e1 active.
3. `B_D6{α2}` records a valid quiet candidate under the latch, without committing.
4. `D14@P0`, position 1, with earlier origin α1, contradicts candidate bound 6. Increment `unknown_evidence` by one; refute the candidate; suspend e1.

**Final:** the same as H5a: no admission, bound or discharge; pending `1=1+0`; census known; latch on; e1 suspended; original startup OPEN only. Recording the candidate after the latch does not remove its evidentiary consequences.

### H6a

**Merge stream:** `V(20,C0,unknown_evidence) → EXIT_D18 → F(24,C0)`.

1. `TERM_D19` records the prospective matching terminal claim for L position 1.
2. `D20@P0`, position 1, is independently attributable and stamped after 19. It refutes that terminal claim immediately: `unknown_evidence +1`; no discharge had committed.
3. `RA` starts e1 active.
4. `EXIT_D18@L`, position 1, arrives with timestamp 18. The retained terminal claim is already refuted and cannot be restored. This further contradiction of that same claim does not generate another report solely for its refutation.
5. `P0(25,2)`, `P1(25,1)`, `E(25,1)` produce nothing.
6. `L(25,2)` publishes the independently bound cookie-bearing EXIT and F24 at detection time 22. No ADMIT or terminal disposition is required for this ordinary EXIT publication. No discharge commits.

**Final:** `admitted=false`; terminal claim 19 retained as refuted; no bound or discharge; pending `1=1+0`; census known; latch on; e1 active; startup OPEN unclosed. One EXIT published; no revoked OPEN.

### H6b

**Merge stream:** `V(20,C0,unknown_evidence) → EXIT_D18 → F(24,C0)`.

1. `D20@P0`, position 1, stages.
2. `TERM_D19` is contradicted on delivery by retained D20. Reject it before recording an accepted terminal disposition; increment `unknown_evidence` by one and retain the required refutation history.
3. `RA` starts e1 active.
4. `EXIT_D18@L`, position 1, supplies no new accepted terminal claim and does not restore the rejected one.
5. `P0(25,2)`, `P1(25,1)`, `E(25,1)` produce nothing.
6. `L(25,2)` publishes EXIT and F24 at detection time 22.

**Final:** no admission, bound or discharge; no accepted terminal disposition; rejected-claim/refutation history retained; pending `1=1+0`; census known; latch on; e1 active; startup OPEN unclosed. No second fault or revoked OPEN.

### H6c

**Determined prefix:** `V(20,C0,unknown_evidence)`.

1. `TERM_D19` records the prospective terminal claim.
2. `D20@P0`, position 1, refutes it: `unknown_evidence +1`; latch on; e0 suspended. No discharge committed.
3. `RA` starts e1 active.
4. `TERM_D21` names the same prospective EXIT position with a different terminal timestamp.

**The last control is not fully decided by the contract.**

- **Conflicting-final-claim reading:** retaining the first final-disposition fact makes TERM21 an impermissible conflicting claim. It is rejected with `V(22,C0,unknown_evidence)`, adding one diagnostic and suspending e1.
- **Committed-discharge-condition reading:** the explicit different-time/different-EXIT rule requires a claim supporting a **committed discharge**. None exists here. D20 does not contradict terminal time 21. On this reading TERM21 may be recorded without another fault, leaving e1 active, though the latch prevents any eventual new discharge.

The sentence preserving a separately delivered conflicting claim’s “own fault consequences” does not explicitly settle whether this particular conflict faults before any discharge has committed.

**Under both readings:** `admitted=false`; claim 19 remains refuted; no bound or discharge; pending `1=1+0`; census known; latch on; startup OPEN unclosed; no revoked OPEN or consumption. Total `unknown_evidence` is respectively two or one.

### H7a

**Merge stream:** `V(20,C0,unknown_evidence)`.

1. `AD_D6` records admission.
2. `D12@P0`, position 1, stages as final-attachment DATA.
3. `D14@P0`, position 2, stages with earlier-attempt origin.
4. `A20@P0`, position 3, stages with missing dependency x.
5. `P0(40,4)`, `P1(40,1)`, `E(40,1)` each leave `W=0`.
6. `L(40,1)` establishes `W=40`. Complete eligible-batch validation must continue despite missing x. Prospective fresh-record bound 12 conflicts with the earlier-attempt interval ending at 14. Increment `unknown_evidence` by one and suspend e0.

Neither the hold nor prospective readiness commits D12, a bound, END or FRONTIER.

**Final:** `admitted=true`, admission itself unrefuted; prospective numerical bound refuted, none committed; pending `1=1+0`; census known; latch on; e0 suspended; original startup OPEN only. No DATA was published or consumed.

### H7b

**Merge stream:** `V(20,C0,unknown_evidence)`.

1. `AD_D6` records admission.
2. `A10@P1`, position 1, stages with missing x.
3. `D12@P0`, position 1, stages with final membership.
4. `D14@P0`, position 2, stages with earlier membership.
5. `P0(40,3)`, `P1(40,2)`, `E(40,1)` produce nothing.
6. `L(40,1)` establishes `W=40`. Although the missing dependency belongs to the first DATA in output order, validation must still discover the contradiction between prospective bound 12 and earlier-attempt D14. Increment `unknown_evidence` by one.

**Final:** the same as H7a. No DATA, bound, END or FRONTIER commits; no revoked OPEN or census reset.

### H8a

**Merge stream:** `F(18,C0) → V(20,C0,unknown_evidence)`.

1. `AD_D6` records numerical admission.
2. `B_D6` records the coherent quiet candidate.
3. `P0(19,1)`, `P1(19,1)`, `E(19,1)` produce nothing.
4. `L(19,1)` permits successful commitment of quiet bound `b_D=6`, startup `END(6)`, and F18. Pending becomes `0=0+0`. No discovery DATA at C=6 requires an endpoint increment.
5. `TRY_NW_D6` is a mandatory late-attempt fault: `unknown_evidence +1`. With no recorded qualified non-NULL fire, it also refutes the numerical admission’s address proof and revokes bound 6.

**Required persistence order:** initial `OPEN(0) → END(6) → revoked OPEN(0) → VOID`.

At VOID-callback entry, every discovery view must already show:

- historical `admitted=true`, with the admission proof refuted;
- no valid committed bound or discharge;
- pending `1=1+0`;
- census still known;
- startup gap reopened from zero;
- certification latch on.

The persisted reader must already observe the reopened hard gap and withhold numerical answers for capture windows intersecting it. The saved prefix ending at callback entry contains the revoked OPEN even though it does not yet contain this VOID. Evaluating that saved prefix with **no subsequent publication** must still yield Unavailable from the reopened gap; historical END6 cannot authorize coverage.

**Final:** e0 suspended; the above discovery state persists. Revocation invalidates affected cached answers, but does not disprove F18’s source-completeness claim.

### H8b

**Merge stream:** `F(18,C0) → V(20,C0,unknown_evidence)`.

`NW_D6` expands into three separate observations:

1. `TRY_NW_D6` records the pointer-watch qualification history.
2. `ADc6` records route-(c) admission.
3. `CN6` records its matching confirmation candidate.
4. `P0(19,1)`, `P1(19,1)`, `E(19,1)` produce nothing.
5. `L(19,1)` commits `b_D=6`, `END(6)` and F18. Pending becomes zero.
6. `PTR_D12(non-NULL)` refutes the committed permanent-role proof: `unknown_evidence +1`; revoke bound 6 and reset census completeness to unknown.

**Required persistence order:** `OPEN(0) → END(6) → revoked OPEN(0) → VOID`.

At VOID-callback entry:

- historical `admitted=true`; route-(c) proof refuted;
- no committed bound or discharge;
- pending `1=1+0`;
- census **unknown**, requiring re-establishment;
- startup gap reopened from zero;
- latch on.

Both the live persisted reader and an independently evaluated saved callback-entry prefix must return Unavailable for windows intersecting the reopened gap, without needing the impending VOID or any later publication.

**Final:** e0 suspended. The pointer fire supplies neither transport DATA nor a boundary and does not itself invalidate the merge epoch’s F18. There are no wait/CPU observations or A2 population contributions from route (c).

The contract expressly sequences the discovery fields above before the callback. It does not separately specify the exact callback-time transition of a generic epoch-suspension flag; e0 is unambiguously suspended when the primitive finishes.

### H9a

**Merge stream:** `F(7,C0) → V(20,C0,unknown_evidence)`.

1. `PTR_D5(non-NULL)` records qualified initialization evidence; no DATA or bound.
2. `AD_D6` records valid numerical admission.
3. `B_D6` records its coherent quiet candidate.
4. `P0(8,1)`, `P1(8,1)`, `E(8,1)` produce nothing.
5. `L(8,1)` commits `b_D=6`, `END(6)` and F7; pending becomes zero.
6. `TRY_NW_D6` is rejected as a late report: `unknown_evidence +1`. It contributes no retained attempt or pointer history. Because a qualified non-NULL fire was already recorded, Decision 38’s additional numerical-address refutation condition is absent.

**Final:** historical admission and committed bound 6 remain unrefuted; no discharge; pending `0=0+0`; census known; latch on; e0 suspended; persisted gap remains `[0,6)`. No revoked OPEN. At the VOID callback the latch is already on, but this fault does not revoke the bound or reopen the gap.

### H9b

**Merge stream:** `F(7,C0) → V(20,C0,unknown_evidence)`.

1. `PTR_D5(non-NULL)` records the qualifying pointer evidence.
2. `AD_D6` records admission.
3. `B_D6` records the quiet candidate.
4. `P0(8,1)`, `P1(8,1)`, `E(8,1)` produce nothing.
5. `L(8,1)` commits bound 6, `END(6)` and F7.
6. `WAIT_D6`, although historical rather than a newly executed bracket, is expressly covered by the late-attempt rule. Reject it and increment `unknown_evidence` by one. The retained non-NULL fire prevents the additional address-proof refutation.

**Final:** admission and bound 6 retained; pending `0=0+0`; census known; latch on; e0 suspended; gap `[0,6)`; no revoked OPEN. The rejected report adds no retained attempt.

### H10a

**Merge stream:** `EXIT_D18{no cookie} → F(18,C0) → V(20,C0,unknown_evidence)`.

1. `TERM_D18` independently binds the prospective cookie-less EXIT to installed D and records the matched disposition.
2. `EXIT_D18@L`, position 1, stages with that disposition.
3. `P0(19,1)`, `P1(19,1)`, `E(19,1)` produce nothing.
4. `L(19,2)` publishes EXIT and F18; discharge `x_D=18` and `END(18)` commit.
5. `TERMU_D18` conflicts with the matched disposition supporting that committed discharge. Reject the new disposition, increment `unknown_evidence` by one, and revoke `x_D=18`.

**Required persistence order:** `OPEN(0) → END(18) → revoked OPEN(0) → VOID`.

At VOID-callback entry: `admitted=false`; no bound or discharge; pending `1=1+0`; census known; startup gap reopened from zero; latch on. The persisted reader already withholds coverage because of the revoked OPEN.

**Final:** e0 suspended. The original matched final disposition remains historical; the conflicting control neither replaces it nor consumes EXIT again. One ordinary EXIT was published; uncovered consumption remains zero.

### H10b

Initial override: pending `1=0+1`, census unknown, no bound lifetime for PID 50.

**Merge stream:** `EXIT_U4{no cookie} → A6[T] → F(7,C0) → V(20,C0,unknown_evidence)`.

1. `X_4@L`, position 1, stages without a terminal disposition.
2. `A6@P0`, position 1, stages and supplies implicit cutoff 6.
3. `P0(8,2)`, `P1(8,1)`, `E(8,1)` produce nothing.
4. `L(8,2)` establishes `W=8`, but X4’s missing disposition holds the complete eligible batch and its frontier.
5. `J_U4` independently establishes inventory lifetime U, resolves the numeric observation, establishes census completeness through fence 5, and supplies the matched disposition. Release evaluation now publishes X4, A6[T], then F7. Discharge `x_U=4` commits; pending becomes `0=0+0`. `C_close=max(5,4)=5`, so startup `END(5)` commits.
6. `TERMU_U4` conflicts with the matched disposition supporting the committed discharge. It also cannot qualify as a first numeric-only unresolved disposition after J has resolved the number and registered U. Reject it, increment `unknown_evidence` by **one**, and revoke the discharge.

**Required persistence order:** `OPEN(0) → END(5) → revoked OPEN(0) → VOID`.

At VOID-callback entry: U’s independent identity and numeric resolution remain established; `admitted=false`; no bound or discharge; pending `1=1+0`; census known; startup gap reopened from zero; latch on. The persisted reader already returns Unavailable for windows intersecting that gap.

**Final:** e0 suspended. Both successful DATA publications remain historically observable. Neither is converted into uncovered consumption, and EXIT is not consumed again.

The strengthened publication ordering also applies to the earlier revocation executions in ROWS:

- N5, P2 and the pointer/continuity variants of R3 require revoked OPEN and the applicable census reset before the revoking VOID. For R3’s attributable-DATA variant, the added refutation rule preserves census completeness.
- R4, committed-discharge variants of R5, G2b, G3’s revocation variant, G4 and G4b require the corresponding proof revocation, updated pending state and revoked OPEN before VOID.
- In R12, the source-seal violation invalidates the epoch that supplied EXIT-release evidence and revokes its discharge. Its revoked OPEN must precede the seal-violation VOID. D12≤18 does not independently establish the post-terminal-timestamp contradiction needed for an additional unknown-evidence report.
- Candidate-only refutations such as R1, R6 and O1 have no committed proof to revoke and therefore do not acquire a revoked OPEN merely from this amendment. O1 retains its ordered clock-regression and unknown-evidence faults.
- D10 and J20 retain their late-unlisted reopening behavior without manufacturing a merge VOID.
- A counter fault alone, as in the relevant R7 executions, preserves independently valid committed proofs and does not reopen their closed gap.

These ordering requirements do not themselves add or remove DATA/TEXT/FRONTIER/VOID outputs or change otherwise specified final outcomes. They do change any earlier answer that allowed the revoking callback—or a persisted prefix saved there—to observe the old closed gap or revoked proof.

This is one independent answer set. No second answer set was supplied, so comparison between two sets has not been performed.

## Second packet (after settlements 7b and 7c; fresh answerer; 34 selected executions)

All 34 executions are decided by the amended contract.

The following notation makes the traces compact while retaining the state after every primitive input:

- `O0`: plain startup OPEN from `boot_start=0`.
- `R0`: **revoked** startup OPEN from 0.
- `E<n>`: startup END with persisted endpoint `n`.
- `F<n>[C]`: `FRONTIER(n,C)`.
- `V<d>[C,r]`: `VOID(d,C,r)`.
- `[U]` / `[T]`: incoming numerical interval uncertified / certified.
- `B(s,w,p)`: the individual primitive `BOUNDARY(s,w,p)`.
- `a`: historical `admitted`; `v`: admission-proof validity.
- `Tn`: recorded matched terminal claim at `n`; `Qn`: recorded quiet candidate at `n`; `CNn`: recorded no-wait-word confirmation.
- `+` means unrefuted; `−` means refuted. Rejected controls are explicitly identified and create no accepted claim.
- Pending is written `L+Q`: unresolved inventoried lifetimes plus unresolved numeric observations.

Each trace is an exact state-transition trace. **Every unmentioned field carries forward unchanged.** `=` therefore reports the same complete state as after the preceding primitive. Where several boundary primitives share a table row, **each independently** has the stated result; they are not processed as a batch.

Unless overridden below, the initial state is:

`a=false; v=absent; claims=none; b=x=none; pending=1+0; census=known; latch=off; epoch 0 active; startup gap=open from 0; abandonment/uncovered counts=0/0`.

Every execution starts with successful publication `O0`. D is the installed, independently bound discovery lifetime `(30,2^63)`. Controls and adapter observations consume no transport position. All observations use `d=20,C0`, except explicit CHECK/REARM inputs and observations following REARM, which use `d=22` and the current vector.

At every VOID callback described below, suspension, the latch, evidence retention, proof revocation and any required OPEN publication have **already occurred**. The persisted reader agrees with the public startup-gap view at that entry.

No selected execution invalidates a merge epoch or exits nonzero. No selected execution publishes TEXT, FORK, INIT or UNTRACED. Counts remain `0/0` except the one explicitly identified abandonment discard in H3d.

## 1. H1a

**Combined:** `O0 → EXIT_D18 → E18 → F18[C0] → R0 → V20[C0,unknown_evidence]`  
**Merge only:** `EXIT_D18 → F18[C0] → V20[C0,unknown_evidence]`

| Primitive input | State after input |
|---|---|
| Matching terminal control `TERM_D18` | Record `T18+`; no discharge commitment. |
| `EXIT_D18@L,o=1` | `=`; EXIT staged. |
| `B(P0,19,1)`; `B(P1,19,1)`; `B(E,19,1)` | Each `=`. |
| `B(L,19,2)` | Publish EXIT, END18, F18; commit `x=18`, pending `0+0`, gap closed at 18. |
| `ADMIT_DISCOVERY(D,α,19,proof,20,C0)` | Reject ADMIT: enable 19 contradicts bound EXIT18. `a=false`, `v=absent`; refute `T18`, revoke `x`; pending `1+0`; census stays known; latch on; epoch 0 suspended; gap reopened by `R0`. |

The last primitive establishes the contradiction and increments `unknown_evidence` once; no storage counter changes.

**VOID entry:** public `a=false,x=none,pending=1+0,census=known`, latch on, suspended, gap open. Invalidate cached answers depending on the revoked discharge/closure. Persisted history is `O0,E18,R0`; the reader reports an open gap.

## 2. H2a

**Combined:** `O0 → V20[Cr,counter_change] → F24[Cr] → D26[U] → F26[Cr]`  
**Merge only:** `V20[Cr,counter_change] → F24[Cr] → D26[U] → F26[Cr]`

| Primitive input | State after input |
|---|---|
| `CHECK(20,Cr)` | `wait_read_fail` becomes 1; `counter_change +=1`; latch on; epoch 0 suspended. |
| `REARM(21,Cr,fresh A/B proofs)` | Epoch 1 active; fresh release cutoffs zero; latch remains on. |
| `B(P0,25,1)`; `B(P1,25,1)`; `B(E,25,1)` | Each `=`. |
| `B(L,25,1)` | Publish `F24[Cr]`; no discovery resolving proof commits. |
| `ADMIT_DISCOVERY(D,α,6,proof,22,Cr)` | Record valid startup ADMIT, including qualified continuation through REARM/current observation: `a=true,v=valid`. |
| `D26@P0,o=2,{final}` | Stage DATA; no bound commits. |
| `B(P0,27,3)`; `B(P1,27,2)`; `B(E,27,2)` | Each `=`. |
| `B(L,27,2)` | Publish `D26[U],F26[Cr]`. Latch prevents `b=26` and END commitment. |

Final pending remains `1+0`, census known, gap open. The successful D26 publication is not a committed discovery bound.

**VOID entry at CHECK:** initial discovery state with latch on and epoch 0 suspended; persisted history `O0`, reader open; no coverage cache revocation.

## 3. H3c

**Combined:** `O0 → V20[C0,unknown_evidence]`  
**Merge only:** `V20[C0,unknown_evidence]`

| Primitive input | State after input |
|---|---|
| `D12@P0,o=1,{final}` | Stage DATA and retain attributable timestamp evidence. |
| Route-(c) `ADMIT_DISCOVERY(D,α,6,proof,20,C0)` | Reject before recording: D12 contradicts the proposed permanent no-wait-word provenance. `a=false,v=absent`; latch on; epoch 0 suspended. Dispose staged D12. |
| Demonstrated pointer-observation continuity break | Retain the observation; otherwise `=`. No accepted unrefuted route-(c) claim exists to refute. |

The ADMIT primitive increments `unknown_evidence` once. The break produces **no repeated or independent discovery-refutation report** under the specified facts.

**VOID entry:** pending `1+0`, census known, no bound/discharge, gap open; history `O0`; no revoked OPEN or cache invalidation.

## 4. H3d

**Combined:** `O0 → V20[C0,unknown_evidence] → F26[C0]`  
**Merge only:** `V20[C0,unknown_evidence] → F26[C0]`

| Primitive input | State after input |
|---|---|
| `D12@P0,o=1,{final}` | Stage DATA. |
| Route-(c) ADMIT at 6 | Reject for contradiction with D12; `a=false,v=absent`; latch on; epoch 0 suspended; `unknown_evidence +=1`; dispose D12. |
| `ABANDON_DISCOVERY(D,20,C0)` | Accept permanent abandonment. Installed identity and final-disposition facts independently qualify. No discharge or bound. |
| `REARM(21,C0,fresh A/B proofs)` | Epoch 1 active; abandonment retained. |
| `D26@P0,o=2,{earlier}` | Stage abandoned-stream DATA; no bound proposed. |
| `B(P0,27,3)`; `B(P1,27,1)`; `B(E,27,1)` | Each `=`. |
| `B(L,27,1)` | Validated discard of D26; `discovery_abandoned_records=1`; publish F26. |

D12 was fault disposal and contributes no discard count. Final state: abandonment accepted, `a=false`, pending `1+0`, census known, latch on, epoch 1 active, gap open.

**VOID entry:** before abandonment was submitted; no accepted admission/abandonment, pending `1+0`, known census, history `O0`, reader open.

## 5. H5a

**Combined:** `O0 → V20[Cr,counter_change] → V22[Cr,unknown_evidence]`  
**Merge only:** the two VOID publications.

| Primitive input | State after input |
|---|---|
| `CONFIRM_DISCOVERY(D,α2,6,witness e=6,20,C0)` | Record `Q6+`; no ADMIT, bound or commitment readiness. |
| `CHECK(20,Cr)` | `counter_change +=1`; `wait_read_fail=1`; latch on; epoch 0 suspended. Candidate survives as evidence. |
| `REARM(21,Cr,fresh A/B proofs)` | Epoch 1 active. |
| `D14@P0,o=1,{earlier}` | Earlier-attempt interval ending at 14 contradicts proposed quiet bound 6. Refute `Q6`; `unknown_evidence +=1`; epoch 1 suspended; dispose DATA. |

Throughout: `a=false,v=absent,b=x=none,pending=1+0,census=known,gap=open`.

**VOID entries:** first has `Q6+`, second `Q6−`; both have latch on and their respective epoch suspended. No committed proof was revoked, no revoked OPEN, no census reset. Persisted history remains `O0`.

## 6. H5b

**Combined:** `O0 → V20[Cr,counter_change] → V22[Cr,unknown_evidence]`  
**Merge only:** the two VOID publications.

| Primitive input | State after input |
|---|---|
| `CHECK(20,Cr)` | Storage `wait_read_fail=1`; `counter_change +=1`; latch on; epoch 0 suspended. |
| `REARM(21,Cr,fresh A/B proofs)` | Epoch 1 active. |
| `CONFIRM_DISCOVERY(D,α2,6,witness e=6,22,Cr)` | Record `Q6+` despite latch; no commitment. |
| `D14@P0,o=1,{earlier}` | Refute `Q6`; `unknown_evidence +=1`; epoch 1 suspended; dispose DATA. |

The second fault occurs on **D14**, not candidate receipt. The public and persisted states at both callbacks are as in H5a, except no candidate exists at the first callback. Counts `0/0`.

## 7. H6a

**Combined:** `O0 → V20[C0,unknown_evidence] → EXIT_D18 → F24[C0]`  
**Merge only:** `V20[C0,unknown_evidence] → EXIT_D18 → F24[C0]`

| Primitive input | State after input |
|---|---|
| `TERM_D19`, naming the prospective EXIT position | Record `T19+`; no discharge. |
| `D20@P0,o=1` | Refute T19 because attributable DATA is after claimed termination. `unknown_evidence +=1`; latch on; epoch 0 suspended; dispose DATA. |
| `REARM(21,C0,fresh A/B proofs)` | Epoch 1 active; refutation retained. |
| Independently bound `EXIT_D18@L,o=1` | Stage EXIT. It supplies no new terminal-control claim and no repeated discovery fault solely for the already-refuted inconsistency. |
| `B(P0,25,2)`; `B(P1,25,1)`; `B(E,25,1)` | Each `=`. |
| `B(L,25,2)` | Publish EXIT18 and F24. No discharge commits. |

Final: `a=false,b=x=none,T19−,pending=1+0,census=known,latch=on`, gap open.

**VOID entry on D20:** epoch 0 suspended, T19 already refuted, no resolving proof or cache revocation; history `O0`, reader open.

## 8. H6b

**Combined:** `O0 → V20[C0,unknown_evidence] → EXIT_D18 → F24[C0]`  
**Merge only:** the same without O0.

| Primitive input | State after input |
|---|---|
| `D20@P0,o=1` | Stage DATA; retain timestamp evidence. |
| `TERM_D19`, naming prospective EXIT position | Contradicted on delivery by retained D20; reject terminal claim and retain its refutation history. `unknown_evidence +=1`; latch on; epoch 0 suspended; dispose D20. |
| `REARM(21,C0,fresh A/B proofs)` | Epoch 1 active. |
| `EXIT_D18@L,o=1` | Stage EXIT; no new fault solely for reconciling it with the previously rejected/refuted claim. |
| `B(P0,25,2)`; `B(P1,25,1)`; `B(E,25,1)` | Each `=`. |
| `B(L,25,2)` | Publish EXIT18 and F24; no discharge. |

Unlike H6a, T19 was never accepted as an unrefuted recorded claim. Its rejection/refutation history nevertheless survives.

**VOID entry:** suspended, latch on, pending `1+0`, census known, gap open, history `O0`; no revoked OPEN.

## 9. H6c

**Combined:** `O0 → V20[C0,unknown_evidence] → V22[C0,unknown_evidence]`  
**Merge only:** the two VOID publications.

| Primitive input | State after input |
|---|---|
| `TERM_D19` | Record `T19+`. |
| `D20@P0,o=1` | Refute T19; first `unknown_evidence` increment; latch on; epoch 0 suspended; dispose DATA. |
| `REARM(21,C0,fresh A/B proofs)` | Epoch 1 active. |
| `TERM_D21`, naming the same prospective EXIT position | Conflicts with retained final T19. Reject T21 without replacing/restoring T19; second `unknown_evidence` increment; epoch 1 suspended. |

The last fault is a **separately conflicting control**, not a repeated report of D20’s contradiction. Neither terminal claim supplies discharge.

**Both VOID entries:** no committed proof, pending `1+0`, known census, gap open, history `O0`; no cache invalidation or revoked OPEN.

## 10. H1b

**Combined:** `O0 → V20[C0,unknown_evidence]`  
**Merge only:** the VOID.

| Primitive input | State after input |
|---|---|
| `ADMIT_DISCOVERY(D,α,19,proof,20,C0)` | Record ADMIT: `a=true,v=valid`; no bound. |
| Independently bound `EXIT_D18@L,o=1` | Establish enable/termination contradiction immediately. Refute admission proof: `a=true,v=refuted`; `unknown_evidence +=1`; latch on; epoch 0 suspended; dispose EXIT. |
| `B(P0,20,1)`; `B(P1,20,1)`; `B(E,20,1)`; `B(L,20,2)` | Each `=`; no release. |

No TERM is needed to establish the contradiction. There is no recorded terminal claim or committed discharge to revoke.

**VOID entry:** historical admission true but proof refuted; pending `1+0`, known census, gap open; history `O0`, reader open.

## 11. H1c

**Combined:** `O0 → EXIT_D18 → E18 → F18[C0]`  
**Merge only:** `EXIT_D18 → F18[C0]`

| Primitive input | State after input |
|---|---|
| `TERM_D18` | Record `T18+`. |
| `EXIT_D18@L,o=1` | Stage EXIT. |
| `B(P0,19,1)`; `B(P1,19,1)`; `B(E,19,1)` | Each `=`. |
| `B(L,19,2)` | Publish EXIT, END18, F18; commit `x=18`, pending `0+0`, gap closed at 18. |
| `ADMIT_DISCOVERY(D,α,18,proof,20,C0)` | Record valid ADMIT: `a=true,v=valid`. Equality with EXIT time is allowed. Existing discharge wins; no bound commits. |

No fault, latch remains off, epoch 0 active. The terminal proof and closure committed; the later admission did not create another resolving proof.

## 12. G4 — final TERM_D19 names the same EXIT

**Combined:** `O0 → EXIT_D18 → E18 → F18[C0] → R0 → V20[C0,unknown_evidence]`  
**Merge only:** `EXIT_D18 → F18[C0] → V20[C0,unknown_evidence]`

| Primitive input | State after input |
|---|---|
| `TERM_D18` | Record `T18+`. |
| `EXIT_D18@L,o=1` | Stage EXIT. |
| `B(P0,19,1)`; `B(P1,19,1)`; `B(E,19,1)` | Each `=`. |
| `B(L,19,2)` | Commit `x=18`, pending `0+0`, gap closed at 18; publish EXIT, END18, F18. |
| `TERM_D19`, naming that same EXIT | Conflicting terminal time: reject T19, refute the discharge-supporting claim, revoke x and dependent closure; `unknown_evidence +=1`; pending `1+0`; latch on; epoch 0 suspended; persist R0. |

The original final disposition is not replaced.

**VOID entry:** `a=false,x=none,pending=1+0,census=known`, gap open. Invalidate dependent caches. History `O0,E18,R0`; reader open.

## 13. R10 — qualified non-NULL pointer fire

**Combined:** `O0 → V20[C0,unknown_evidence] → V20[C0,unknown_evidence]`  
**Merge only:** the two VOID publications.

| Primitive input | State after input |
|---|---|
| Pointer-watch physical qualification history at enable 6 (`TRY_NW_D6`) | Retain observational history; no admission or bound. |
| Route-(c) `ADMIT_DISCOVERY(D,α,6,proof,20,C0)` | Record `a=true,v=valid`. |
| Qualified pointer fire `PTR_D9(non-NULL)` | Refute accepted permanence claim; `v=refuted`; first `unknown_evidence` increment; latch on; epoch 0 suspended. |
| `CONFIRM_NO_WAIT_WORD(D,α,6,witness,20,C0)`, falsely claiming completed reconciliation | Reject false confirmation; second `unknown_evidence` increment. No candidate recorded. |
| `B(P0,10,1)`; `B(P1,10,1)`; `B(E,10,1)`; `B(L,10,1)` | Each `=`. |

No bound had committed, so neither fault publishes R0 or resets census completeness.

**Both VOID entries:** historical admission true, proof refuted, pending `1+0`, known census, open gap, history `O0`. At the second entry, the false CN has additionally been rejected.

## 14. R10 — demonstrated continuity break

**Combined:** `O0 → V20[C0,unknown_evidence] → V20[C0,unknown_evidence]`  
**Merge only:** the two VOID publications.

| Primitive input | State after input |
|---|---|
| Pointer-watch qualification history at 6 | Retain observation. |
| Route-(c) ADMIT at 6 | `a=true,v=valid`. |
| Demonstrated pointer-observation continuity break | Refute accepted admission claim; `v=refuted`; first `unknown_evidence` increment; latch on; epoch 0 suspended. |
| CN6 explicitly asserting completed continuity reconciliation | Reject separately false confirmation; second `unknown_evidence` increment. |
| `B(P0,10,1)`; `B(P1,10,1)`; `B(E,10,1)`; `B(L,10,1)` | Each `=`. |

Commitments, callback states, census, gap history and counts are the same as execution 13. The first fault’s establishing primitive is the **break**, not a pointer fire.

## 15. H3b

**Combined:** `O0 → V20[C0,unknown_evidence]`  
**Merge only:** the VOID.

| Primitive input | State after input |
|---|---|
| `D12@P0,o=1,{final}` | Stage DATA and retain evidence. |
| Route-(c) ADMIT at 6 | Reject contradicted admission before recording; `a=false,v=absent`; `unknown_evidence +=1`; latch on; epoch 0 suspended; dispose D12. |
| `PTR_D14(non-NULL)` | Retain observation; no accepted unrefuted route-(c) claim exists, so no additional discovery-refutation fault. |

No bound/discharge commits. Pending `1+0`, known census, open gap.

**VOID entry:** initial discovery obligations retained, latch on, suspended; history `O0`; no revoked OPEN or dependent cache invalidation.

## 16. N5

**Combined:** `O0 → F5[C0] → E6 → F7[C0] → R0 → V20[C0,unknown_evidence]`  
**Merge only:** `F5[C0] → F7[C0] → V20[C0,unknown_evidence]`

| Primitive input | State after input |
|---|---|
| Qualified pointer-watch physical history: enable 6, subsequent pinned NULL read, subsequent completed fence | Retain observational proof; no wait-word attempt. |
| Route-(c) ADMIT at 6 | `a=true,v=valid`. |
| `CONFIRM_NO_WAIT_WORD(D,α,6,witness,20,C0)` | Record `CN6+`; no commitment without strict completeness. |
| `B(P0,6,1)`; `B(P1,6,1)`; `B(E,6,1)` | Each `=`. |
| `B(L,6,1)` | Publish F5; equality does not commit b6. |
| `B(P0,8,2)`; `B(P1,8,2)`; `B(E,8,2)` | Each `=`. |
| `B(L,8,2)` | Publish END6 then F7; commit `b=6`, pending `0+0`, gap closed at 6. |
| `PTR_D12(non-NULL)` | Refute admission/confirmation, revoke b and closure; `a=true,v=refuted,CN6−`; pending `1+0`; **census becomes unknown**; latch on; epoch 0 suspended; persist R0; `unknown_evidence +=1`. |
| `B(P0,14,3)`; `B(P1,14,3)`; `B(E,14,3)`; `B(L,14,3)` | Each `=`; no release. |

No `+1 ns` endpoint increment applies: no uncertified discovery TRANS/MARKER was released at 6.

**VOID entry:** b already revoked, gap open, census unknown, pending `1+0`; invalidate dependent caches. History `O0,E6,R0`, reader open.

No numerical state, role-support record, birth or omission is created. The qualified PG13 logger supplies no backend or numerical-population contribution.

## 17. H7a

**Combined:** `O0 → V20[C0,unknown_evidence]`  
**Merge only:** the VOID.

| Primitive input | State after input |
|---|---|
| Numerical ADMIT at 6 | Record `a=true,v=valid`. |
| `D12@P0,o=1,{final}` | Stage DATA. Staging alone proposes no fresh-record bound. |
| `D14@P0,o=2,{earlier}` | Retain earlier-attempt evidence; still no fresh bound proposed. |
| `A20@P0,o=3,{requires x}` | Stage DATA; TEXT x unavailable. |
| `B(P0,40,4)`; `B(P1,40,1)`; `B(E,40,1)` | Each `=`; W remains zero. |
| `B(L,40,1)` | Complete eligible-batch validation derives prospective fresh bound 12. Earlier-attempt D14 ends after it and contradicts it. `unknown_evidence +=1`; latch on; epoch 0 suspended; dispose staged suffix. |

The missing TEXT dependency does **not** defer this decidable contradiction. Nothing is published or committed, including no b12, END or F39. The ADMIT remains historically recorded; the prospective lower bound is rejected by reconciliation.

**VOID entry:** `a=true`, no committed b/x, pending `1+0`, known census, latch on, suspended, gap open; history `O0`. No committed coverage proof was revoked.

## 18. R2 — p=empty, no K, TERM then DATA

**Combined:** `O0 → V20[C0,unknown_evidence]`  
**Merge only:** the VOID.

| Primitive input | State after input |
|---|---|
| `TERM_D18` | Record `T18+`. |
| `D20@P0,o=1` | Refute terminal claim immediately; `unknown_evidence +=1`; latch on; epoch 0 suspended; dispose DATA. |
| `EXIT_D18@L,o=1` | Retain/validate evidence, then suspension disposal. No repeated terminal-contradiction report. |
| `B(P0,25,2)`; `B(P1,25,1)`; `B(E,25,1)`; `B(L,25,2)` | Each `=`. |

Final: `a=false,T18−,b=x=none,pending=1+0,census=known,gap=open`.

**VOID entry:** T18 already refuted, suspended, latch on, history `O0`; no revocation publication because no proof committed.

## 19. O1

**Combined:** `O0 → V20[C0,clock_regression] → V20[C0,unknown_evidence]`  
**Merge only:** the two VOID publications.

| Primitive input | State after input |
|---|---|
| `CONFIRM_DISCOVERY(D,α2,6,witness e=6,20,C0)` | Record `Q6+`; no ADMIT or committed bound. |
| `D20@P0,o=1,{final}` | Stage DATA; no contradiction with proposed quiet bound 6. |
| `D14@P0,o=2,{earlier}` | Arrival fails because 14<preceding consumed DATA timestamp 20. Retain independent provenance evidence: earlier-attempt interval ending 14 also contradicts Q6. Refute Q6, reject D14, suspend, latch on, dispose D20. Publish arrival VOID first, discovery VOID second. |

Diagnostics: `clock_regression=1`, `unknown_evidence=1`, `seal_violation=0`. Storage vector stays C0. D14 is never staged or consumed.

**At both VOID entries:** Q6 already refuted; latch on; epoch 0 suspended; `a=false,b=x=none,pending=1+0,census=known,gap=open`; history `O0`. The first callback already sees the complete refutation state.

## 20. N14

**Combined:** `O0 → V20[Cw,writer_push_fail]`  
**Merge only:** the VOID.

| Primitive input | State after input |
|---|---|
| Qualified pointer-watch history at enable 6 | Retain proof. |
| Route-(c) ADMIT at 6 | `a=true,v=valid`. |
| `CONFIRM_NO_WAIT_WORD(D,α,6,witness,20,C0)` | Record `CN6+`. |
| `B(P0,8,1)`; `B(P1,8,1)`; `B(E,8,1)` | Each `=`. |
| `B(L,8,1)` | Required END6 writer call is rejected before bound commitment or FRONTIER publication. `writer_push_fail=1`; diagnostic `writer_push_fail +=1`; latch on; epoch 0 suspended. |

END6 is **not** a successful publication. Neither b6 nor closure commits; F7 is not published. Because no END succeeded, this case requires no rollback plain OPEN and no revoked OPEN.

**VOID entry:** `a=true,v=valid,CN6+`, b/x absent, pending `1+0`, census known, gap open; history `O0`; reader open. No committed END-dependent coverage exists to revoke.

## 21. G5

**Combined:** `O0 → EXIT_D18 → F18[C0] → E18`  
**Merge only:** `EXIT_D18 → F18[C0]`

| Primitive input | State after input |
|---|---|
| `EXIT_D18@L,o=1` | Stage independently bound cookie-bearing EXIT. |
| `B(P0,19,1)`; `B(P1,19,1)`; `B(E,19,1)` | Each `=`. |
| `B(L,19,2)` | Publish EXIT and F18 without ADMIT or disposition. No discharge: pending `1+0`, gap open. |
| Numerical ADMIT at 6 | Record `a=true,v=valid`; no numerical bound. |
| `TERM_D18` | Record T18+; previously completed successful EXIT release permits `x=18` to commit. Publish END18 during this control’s evaluation; pending `0+0`, gap closed at 18. |

No repeated F18 is published. No fault, latch off, epoch 0 active.

## 22. G7

**Combined:** `O0 → EXIT_D18 → E18 → F18[C0]`  
**Merge only:** `EXIT_D18 → F18[C0]`

| Primitive input | State after input |
|---|---|
| `TERM_D18` | Record T18+. |
| `EXIT_D18@L,o=1` | Stage EXIT. |
| `B(P0,19,1)`; `B(P1,19,1)`; `B(E,19,1)` | Each `=`. |
| `B(L,19,2)` | Publish EXIT, END18, F18; commit x18, pending `0+0`, gap closed at 18. |
| Numerical ADMIT at 6 | Record `a=true,v=valid`; existing discharge prevents another resolving proof. |
| `CONFIRM_DISCOVERY(D,α,6,witness,20,C0)` | Record valid Q6+; no bound commits and no publication follows. |

The terminal discharge remains the selected first committed proof. No fault or revocation.

## 23. V4

**Combined:**  
`O0 → EXIT_D18 → E18 → O0 → V20[Cw,writer_push_fail] → F19[Cw]`

The second O0 is a **plain rollback OPEN**, not a revoked OPEN.

**Merge only:** `EXIT_D18 → V20[Cw,writer_push_fail] → F19[Cw]`

| Primitive input | State after input |
|---|---|
| `ABANDON_DISCOVERY(D,20,C0)` | Accept abandonment. |
| `TERM_D18` | Record T18+; no discharge. |
| `EXIT_D18@L,o=1` | Stage EXIT. |
| `B(P0,19,1)`; `B(P1,19,1)`; `B(E,19,1)` | Each `=`. |
| `B(L,19,2)` | EXIT18 and END18 publish successfully. Writer rejects F18, changing storage vector to Cw. Complete attempt fails: x and closure never commit. Persist plain O0 before VOID; invalidate cached answers relying on provisional END18; latch on; epoch 0 suspended; pending `1+0`, census known. |
| `REARM(21,Cw,fresh attachment_proofs)` | Epoch 1 active; latch remains on; failed release supplies no later discharge evidence. |
| Identical `TERM_D18` at d22 | Idempotent retained terminal claim; no restoration or discharge. |
| `B(P0,20,2)`; `B(P1,20,2)`; `B(E,20,2)` | Each `=`. |
| `B(L,20,3)` | Publish F19[Cw]; no x or END commitment. |

Fault diagnostic: `writer_push_fail +=1`; storage `writer_push_fail=1`. The rejected F18 is absent from both publication streams.

**VOID entry:** epoch 0 suspended; latch on; abandonment and T18 retained; `a=false,b=x=none,pending=1+0,census=known`, gap open.

**Independently saved prefix:** `O0,EXIT_D18,E18,O0`. Even with no later publication, its reader reports the startup gap **open**, agreeing with the public view. Historical END18 remains observable but never committed. Revoked-OPEN count is zero.

## 24. A18

**Combined:** `O0 → V20[C0,allocation_fail]`  
**Merge only:** the VOID.

| Primitive input | State after input |
|---|---|
| `D9@P0,o=1` | Stage DATA awaiting admission/final disposition. |
| `FAULT(allocation_fail,20,C0)` | `allocation_fail +=1`; latch on; epoch 0 suspended; fault disposal of D9. |
| `ABANDON_DISCOVERY(D,20,C0)` | Accept abandonment during suspension; no recovery or consumption. |
| `B(P0,16,2)`; `B(P1,16,1)`; `B(E,16,1)`; `B(L,16,1)` | Each `=`. |

Final abandonment count is **0**, because D9 was disposed by fault handling before abandonment consumption. Pending `1+0`, census known, gap open.

**VOID entry:** AB has not yet arrived; no admission/abandonment, latch on, suspended, history `O0`, reader open. No storage increment is fabricated for allocation failure.

## 25. U15

This execution starts with the J/U numeric defaults: no inventoried D, one unresolved numeric observation of PID50, pending `0+1`, census unknown.

**Combined:** `O0 → V20[C0,unknown_evidence]`  
**Merge only:** the VOID.

| Primitive input | State after input |
|---|---|
| AOP: established permanent reconciliation loss; attempts stopped; retained valid journal witnesses consumed; none names X24 | Record finalisation evidence; pending remains `0+1`, census unknown. |
| `FAULT(unknown_evidence,20,C0)` | `unknown_evidence +=1`; latch on; epoch 0 suspended. |
| Cookie-less `EXIT_U24@L,o=1` | Arrival validation/evidence retention, then suspension disposal. No uncovered consumption. |
| `A26@P0,o=1` | Arrival validation, then suspension disposal. |
| `B(P0,28,2)`; `B(P1,28,1)`; `B(E,28,1)`; `B(L,28,2)` | Each leaves discovery state unchanged; no release. |
| `TERMINAL_UNRESOLVED(cookie=0,names exactly X24,20,C0)` | Accept final unresolved disposition using established permanent-loss evidence. No inventory entry/cookie, numeric resolution, discharge or consumption is created. Capture-long startup-closure prohibition applies. |
| `B(P0,30,3)`; `B(P1,30,2)`; `B(E,30,2)`; `B(L,30,3)` | Each `=`. |

Uncovered count stays **0**: acceptance of the disposition does not consume the already-disposed EXIT. No REARM occurs.

**VOID entry:** suspended, latch on, pending `0+1`, census unknown, gap open, no terminal disposition yet; persisted history `O0`.

## 26. H2b

**Combined:** `O0 → V20[Cr,counter_change] → F24[Cr] → D26[U] → F26[Cr]`  
**Merge only:** the same without O0.

| Primitive input | State after input |
|---|---|
| `CHECK(20,Cr)` | Storage wait_read_fail becomes 1; `counter_change +=1`; latch on; epoch 0 suspended. |
| Numerical ADMIT at 6, observed with Cr | Record valid ADMIT during suspension: `a=true,v=valid`. |
| `REARM(21,Cr,fresh A/B and qualified continuing D proofs)` | Epoch 1 active; historical admission retained. |
| `B(P0,25,1)`; `B(P1,25,1)`; `B(E,25,1)` | Each `=`. |
| `B(L,25,1)` | Publish F24[Cr]. |
| `D26@P0,o=2,{final}` | Stage DATA. |
| `B(P0,27,3)`; `B(P1,27,2)`; `B(E,27,2)` | Each `=`. |
| `B(L,27,2)` | Publish D26[U], F26[Cr]; no bound or END commits because latch is set. |

Final pending `1+0`, census known, gap open.

**VOID entry at CHECK:** ADMIT has not yet arrived; initial discovery state, latch on, epoch 0 suspended, history `O0`. The later valid ADMIT creates no additional fault.

## 27. H4a

**Combined:** `O0 → V20[C0,unknown_evidence] → A26[T] → F26[C0]`  
**Merge only:** the same without O0.

| Primitive input | State after input |
|---|---|
| Numerical ADMIT at 6 | Record `a=true,v=valid`. |
| `TRY_NW_D6` history report | A report after recorded ADMIT must fault and is rejected. With no qualified non-NULL fire, it also refutes the numerical admission’s address proof: `v=refuted`; `unknown_evidence +=1`; latch on; epoch 0 suspended. No attempt/pointer history is accepted from the rejected report. |
| `REARM(21,C0,fresh attachment_proofs)` | Epoch 1 active; admission refutation retained. |
| `CONFIRM_DISCOVERY(D,α,6,witness e=6,22,C0)` | Record own-facts-valid Q6+. Refuted ADMIT alone does not make this candidate false. It restores neither admission validity nor commitment readiness and adds no hold on A. |
| `A26@P0,o=1` | Stage ordinary attached-A DATA. |
| `B(P0,27,2)`; `B(P1,27,1)`; `B(E,27,1)` | Each `=`. |
| `B(L,27,1)` | Publish A26[T], F26[C0]. No discovery bound/END. |

Final `a=true,v=refuted,Q6+`, pending `1+0`, census known, latch on, gap open.

**VOID entry:** admission already refuted, no bound ever committed, history `O0`; no revoked OPEN or census reset.

## 28. H3a

**Combined:** `O0 → V20[C0,unknown_evidence]`  
**Merge only:** the VOID.

| Primitive input | State after input |
|---|---|
| `D12@P0,o=1,{final}` | Stage DATA. |
| Route-(c) ADMIT at 6 | Reject contradicted control before recording; `a=false,v=absent`; `unknown_evidence +=1`; latch on; epoch 0 suspended; dispose D12. |
| `TRY_NW_D6` physical-history report | Since no ADMIT has ever been recorded, retain it as observational history. It is not an accepted permanence claim and creates no additional fault solely from contradictory role evidence. |

The report cannot restore the rejected admission. Pending `1+0`, census known, gap open.

**VOID entry:** no accepted admission, bound or discharge; suspended, latch on, history `O0`, reader open.

## 29. G6

**Combined:** `O0 → V20[C0,unknown_evidence]`  
**Merge only:** the VOID.

| Primitive input | State after input |
|---|---|
| `TRY_NW_D6` physical-history report | Retain history showing the qualified NULL-pointer sequence; no admission. |
| Numerical ADMIT at 6, with no qualified non-NULL fire | Required numerical address provenance is contradicted by retained history. Reject ADMIT before recording: `a=false,v=absent`; `unknown_evidence +=1`; latch on; epoch 0 suspended. |
| `B(P0,19,1)`; `B(P1,19,1)`; `B(E,19,1)`; `B(L,19,1)` | Each `=`. |

No accepted route or bound exists. The original observational TRY history remains retained.

**VOID entry:** pending `1+0`, known census, gap open, latch on, suspended; history `O0`; no revoked OPEN.

## 30. R5 — h=1, p=empty, q=empty

Initial census contains D and unresolved H, so pending is `2+0`.

**Combined:** `O0 → EXIT_D18 → F18[C0] → R0 → V20[C0,unknown_evidence]`  
**Merge only:** `EXIT_D18 → F18[C0] → V20[C0,unknown_evidence]`

| Primitive input | State after input |
|---|---|
| `TERM_D18` | Record T18+. |
| `EXIT_D18@L,o=1` | Stage EXIT. |
| `B(P0,19,1)`; `B(P1,19,1)`; `B(E,19,1)` | Each `=`. |
| `B(L,19,2)` | Publish EXIT18 and F18; commit D’s `x=18`; pending `1+0` for H. Gap stays open because H is unresolved. |
| `D20@P0,o=2` | Valid arrival, but refutes D’s committed terminal proof. Revoke x, refute T18, persist R0; pending `2+0`; census stays known; latch on; epoch 0 suspended; `unknown_evidence +=1`; dispose DATA. |

The resolving proof committed even though gap closure did not. Its subsequent revocation requires R0 even though the gap was already open.

**VOID entry:** D and H pending, x absent, known census, latch on, suspended. Invalidate any dependent cached coverage. History `O0,R0`; reader open.

## 31. R5 — h=1, p=AB_D, q=K

Initial pending `2+0`.

**Combined:**  
`O0 → EXIT_D18 → F18[C0] → V20[Cr,counter_change] → R0 → V20[Cr,unknown_evidence]`

**Merge only:** the same without O0 and R0.

| Primitive input | State after input |
|---|---|
| `ABANDON_DISCOVERY(D,20,C0)` | Accept abandonment. |
| `TERM_D18` | Record T18+. |
| `EXIT_D18@L,o=1` | Stage EXIT. |
| `B(P0,19,1)`; `B(P1,19,1)`; `B(E,19,1)` | Each `=`. |
| `B(L,19,2)` | Publish EXIT/F18; commit D x18; pending `1+0` for H; gap remains open. |
| `CHECK(20,Cr)` | Storage wait_read_fail becomes 1; `counter_change +=1`; latch on; epoch 0 suspended. Existing x18 remains committed and unrefuted. |
| `D20@P0,o=2` | Retain attributable contradiction before suspension disposal. Refute T18, revoke x, persist R0; pending `2+0`; `unknown_evidence +=1`. Census stays known. |

D20 is **not** an abandonment discard: it never passes active validated release consumption. Abandonment count remains 0.

**First VOID entry:** abandonment/T18 retained, x18 still committed, pending H only, known census, open gap, history `O0`.

**Second VOID entry:** x revoked, both lifetimes pending, history `O0,R0`; dependent caches invalidated and reader open.

## 32. H11a

**Combined:** `O0 → V20[C0,unknown_evidence]`  
**Merge only:** the VOID.

| Primitive input | State after input |
|---|---|
| Numerical ADMIT at 19 | Record `a=true,v=valid`. |
| Independently bound `EXIT_D18@L,o=1` | Establish enable/EXIT contradiction immediately. Refute admission proof; `a=true,v=refuted`; `unknown_evidence +=1`; latch on; epoch 0 suspended; dispose EXIT. |
| `TERM_D18`, naming exactly that EXIT | Record the matched claim **as refuted**. It supplies no discharge and adds no report solely for the already-established enable/EXIT contradiction. |

No boundary is needed to establish the contradiction, and no boundary follows. Final pending `1+0`, census known, gap open.

**VOID entry:** historical admission true, proof already refuted, no terminal control recorded yet, no b/x, suspended with latch on; history `O0`, reader open.

## 33. H11b

**Combined:** `O0 → EXIT_D18 → E18 → F18[C0] → R0 → V20[C0,unknown_evidence]`  
**Merge only:** `EXIT_D18 → F18[C0] → V20[C0,unknown_evidence]`

| Primitive input | State after input |
|---|---|
| `TERM_D18` | Record T18+. |
| `EXIT_D18@L,o=1` | Stage EXIT. |
| `B(P0,19,1)`; `B(P1,19,1)`; `B(E,19,1)` | Each `=`. |
| `B(L,19,2)` | Publish EXIT, END18, F18; commit x18, pending `0+0`, gap closed at 18. |
| Numerical ADMIT at 19 | Reject before recording; refute disputed terminal claim, revoke x and closure; `unknown_evidence +=1`; pending `1+0`; latch on; epoch 0 suspended; persist R0. `a=false,v=absent`. |
| `D20@P0,o=2,{final}` | Arrival is valid. Retain attributable evidence, then suspension disposal. The terminal claim is already refuted; rejected ADMIT established no accepted admission claim. **No new discovery fault**, no new OPEN, no consumption. |

D20 does not disprove F18 because its timestamp is 20. It neither restores a proof nor repeats the established contradiction.

**VOID entry:** occurs on ADMIT19, with the same fully revoked state and history `O0,E18,R0` as H1a. Dependent caches are already invalidated; reader open. No callback occurs on D20.

## 34. H11c

**Combined:**  
`O0 → V20[Cr,counter_change] → F24[Cr] → V22[Cr,unknown_evidence]`

**Merge only:** the same without O0.

| Primitive input | State after input |
|---|---|
| `CHECK(20,Cr)` | Storage wait_read_fail becomes 1; `counter_change +=1`; latch on; epoch 0 suspended. |
| `REARM(21,Cr,fresh A/B proofs only)` | Epoch 1 active; D continuation is not yet established. |
| `B(P0,25,1)`; `B(P1,25,1)`; `B(E,25,1)` | Each `=`. |
| `B(L,25,1)` | Publish F24[Cr]; no D DATA is eligible. |
| Historical startup `ADMIT_DISCOVERY(D,α,6,proof,22,Cr)` lacking continuation evidence | Record valid original admission facts: `a=true`. Original admission proof is not falsely claimed or refuted; **current-epoch continuity remains missing**. No immediate fault. Missing-ADMIT hold is removed. |
| `D26@P0,o=2,{final}` | Stage DATA. Identity/final membership do not establish missing continuity. No fault yet because t26 is not eligible at W25. |
| `B(P0,27,3)`; `B(P1,27,2)`; `B(E,27,2)` | Each `=`; W remains 25. |
| `B(L,27,2)` | First eligible-batch validation of D26 finds missing current-epoch continuity. Before publishing any batch member, `unknown_evidence +=1`; epoch 1 suspended; dispose D26. No D26, F26, bound or END publication. |

Final: historical admission true; original historical facts valid, current continuation unproved; b/x absent; pending `1+0`; census known; latch on; startup gap open.

**First VOID entry:** initial discovery state, latch on, epoch 0 suspended, history `O0`.

**Second VOID entry:** historical admission recorded, current continuity missing, epoch 1 suspended at last published frontier 24; pending `1+0`, census known, gap open. History remains `O0`; no committed discovery proof was revoked and no additional OPEN is required. Both public and persisted views report the gap open.

## Undecided list

**Empty.** The amended contract determines every requested outcome in these 34 executions.
