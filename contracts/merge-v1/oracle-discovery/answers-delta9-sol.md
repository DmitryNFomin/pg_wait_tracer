# Independent answer set (sol): merge-v1 delta 9

Written from the amended contract, ROWS.md and TASK-delta9.md alone.

## First packet (11 executions)

All results below derive only from the four supplied files. No implementation or platform qualification is inferred.

Notation used throughout:

- `O` = the initial persisted plain startup `OPEN(boot_start=0)`.
- `F<n>` = `FRONTIER(n,C0)`.
- `V<d>` = `VOID(d,C0,unknown_evidence)`.
- `END<p>` = startup `GAP_CLOSE` with endpoint `p` and reserved field zero.
- `A/0`, `S/1` = active/unlatched and suspended/latched.
- Admission `—`, `valid`, `refuted` means respectively: no recorded ADMIT; historical `admitted=true` with unrefuted proof; historical `admitted=true` with refuted proof.
- `M(t,D)` = accessor `MATCHED`, naming the specified EXIT transport identity, timestamp `t`, and binding to D. A superscript “refuted” describes its retained terminal claim.
- `Q` lists rejected-on-delivery control-refutation history, separately from the retained final claim.
- `payloads` counts staged, unpublished payload records after the input. It does not count retained evidence summaries or final-disposition obligations.
- Every `L:1` below denotes the exact EXIT transport identity in that execution, including capture, source and position.

Unless explicitly changed in a table, **after every input**: all `b_i` and `x_i` are absent; pending is **1 lifetime + 0 numeric = 1**; census is known complete at fence 5; the startup gap remains open under `O`; historical and current `C_close/P_close` are undefined. These statements also apply at the corresponding VOID callback entries.

Every input observes `C0`. All required checks and publications succeed. There are **no rejected publication calls, TEXT publications, UNTRACED publications, abandonment consumption, uncovered consumption, storage-counter changes, epoch invalidations, revoked OPENs, cache invalidations, or nonzero-exit requirements** in these executions. Ordinary faults suspend rather than require exit. REARM publishes no OPEN and does not clear the certification latch.

At each VOID callback entry, the faulting input’s evidence retention, claim rejection/refutation and payload disposal have already occurred. The prefix listed below excludes the VOID currently being published.

Decision 41’s END rule applies throughout: a failed END publication means **no complete reader-visible END exists**, so candidate commitments and closure do not commit. A complete reader-visible END commits them even if the process stops before updating memory. A failure observed afterward is a separate observation, not a failed closing attempt. An unrelated later fault does not automatically revoke an existing closure; an applicable revocation rule must require it.

## H11g

Primitive inputs and state:

| # | Input | Epoch/latch | Admission | Final claim and accessor | Q | payloads | Fault total |
|---|---|---|---|---|---|---:|---:|
| 1 | `ADMIT_DISCOVERY(D,α,e=19,proof,20,C0)` | A/0 | valid | None; `NONE` | ∅ | 0 | 0 |
| 2 | Cookie-less `EXIT(pid=30,t=18,L:1)`, observed at 20 | A/0 | valid | None; `NONE`; EXIT still unbound | ∅ | 1 | 0 |
| 3 | `TERM_D18`, naming exactly `L:1`, at 20 | S/1 | refuted | Retained MATCHED claim `(D,L:1,18)`, refuted; `M(18,D)` | ∅ | 0 | 1 |
| 4 | `REARM(21,C0,fresh A/B proofs and qualified D continuity)` | A/1 | refuted | Same refuted claim; `M(18,D)` | ∅ | 0 | 1 |
| 5 | `BOUNDARY(P0,26,p=1)`, at 22 | A/1 | refuted | Same; `M(18,D)` | ∅ | 0 | 1 |
| 6 | `BOUNDARY(P1,26,p=1)`, at 22 | A/1 | refuted | Same; `M(18,D)` | ∅ | 0 | 1 |
| 7 | `BOUNDARY(E,26,p=1)`, at 22 | A/1 | refuted | Same; `M(18,D)` | ∅ | 0 | 1 |
| 8 | `BOUNDARY(L,26,p=2)`, at 22 | A/1 | refuted | Same; `M(18,D)` | ∅ | 0 | 1 |
| 9 | `finish(40,C0)` | A/1 | refuted | Same; `M(18,D)` | ∅ | 0 | 1 |

The cookie-less EXIT has no independent lifetime binding at input 2. Input 3 supplies that binding and establishes `18<19`, the Decision 36 enable/termination contradiction. It records the final MATCHED disposition, refutes both recorded claims, reports one fault and disposes of the EXIT payload. The final disposition remains recorded despite its refuted proof.

REARM restores an active merge epoch, not either refuted proof. The first three boundaries leave at least one fresh cutoff zero. The L boundary establishes `W=26` and publishes `F25` for an empty eligible batch.

Successful merge stream:

`V20 → F25`

Combined publication timeline:

`O → V20 → [fresh boundaries; required C0 release/accounting checks] F25`

At the `V20` callback entry, the table’s input-3 state is already public and the persisted prefix is exactly `O`. No revocation is required because neither proof committed. The reader reports startup coverage unavailable for `[50,60)`.

At finish:

- Accessor: `MATCHED(18,D,L:1)`, terminal claim refuted.
- Pending payload records: **0**, continuously from TERM through finish.
- `unfinished_records`: **0**.
- Unresolved inventory: **1 lifetime**.
- Unsatisfied final-disposition obligations: **0**.
- Fault total: **1**; finish adds no fault.

The unresolved inventory and open startup gap do not turn the disposed, finally disposed-of EXIT into an unfinished payload.

**Targeted mutant:** retain the disposed EXIT as unfinished across REARM. Input 9 must still have `unfinished_records=0` and fault total 1; an added finish VOID distinguishes the mutant.

## H12a

| # | Primitive input, all observed at 20 | Epoch/latch | Admission | Final claim and accessor | Q | payloads | Fault total |
|---|---|---|---|---|---|---:|---:|
| 1 | Cookie-bearing `EXIT_D18@L`, position 1 | A/0 | — | None; `NONE`; EXIT independently bound to D | ∅ | 1 | 0 |
| 2 | `TERM_D19`, naming exactly that `L:1` | S/1 | — | None; `NONE` | Rejected `(D,L:1,19)` | 0 | 1 |
| 3 | Qualified `TERM_D18`, naming exactly that `L:1` | S/1 | — | Unrefuted final MATCHED `(D,L:1,18)`; `M(18,D)` | Rejected T19 retained | 0 | 1 |

Input 2 falsely claims timestamp 19 for retained DATA stamped 18. Decision 40 rejects it on delivery: it creates no final claim, binding or disposition. The independently established EXIT binding remains.

Input 3 is therefore a valid replacement, not a conflicting final claim. It is recorded during suspension under the latch, without discharge or another fault. There is no ADMIT and no enable/termination contradiction.

Successful merge stream: `V20`.

Combined timeline: `O → V20`. Required control counter observations remain C0.

At VOID entry, input-2 state is already public; prefix `O`; `[50,60)` is unavailable because the startup gap remains open. One EXIT payload is disposed of, with no release consumption.

**Targeted mutant:** retain rejected T19 as the final claim. At input 3 the contract requires unrefuted `MATCHED(18,D)` and fault total 1; a conflicting-control fault or retained timestamp 19 identifies the mutant.

## H12b

| # | Primitive input | Epoch/latch | Admission | Final claim and accessor | Q | payloads | Fault total |
|---|---|---|---|---|---|---:|---:|
| 1 | Cookie-less `EXIT(pid=30,t=18,L:1)`, at 20 | A/0 | — | None; `NONE`; EXIT unbound | ∅ | 1 | 0 |
| 2 | False `TERM_D19`, naming exactly `L:1`, at 20 | S/1 | — | None; `NONE`; EXIT remains unbound | Rejected `(D,L:1,19)` | 0 | 1 |
| 3 | Qualified `TERM_D18`, naming exactly `L:1`, at 20 | S/1 | — | Unrefuted MATCHED `(D,L:1,18)`; `M(18,D)` | Rejected T19 retained | 0 | 1 |
| 4 | `finish(40,C0)` | S/1 | — | Same unrefuted claim; `M(18,D)` | Same | 0 | 1 |

Input 2 is rejected using the retained raw EXIT timestamp. Its otherwise qualified binding assertions are **not accepted**. Input 3 supplies the valid lifetime binding and final disposition. It records an unrefuted claim because no ADMIT exists.

Successful merge stream: `V20`.

Combined timeline: `O → V20`.

At VOID entry, input-2 state is public, including the EXIT’s lack of accepted lifetime binding. Prefix: `O`; startup availability for `[50,60)` is unavailable. Fault handling disposes of one EXIT payload.

At finish:

- Accessor: unrefuted `MATCHED(18,D,L:1)`.
- Pending payload records and `unfinished_records`: **0**.
- Unresolved inventory: **1 lifetime**.
- Final-disposition obligation: **satisfied**.
- Fault total: **1**, unchanged by finish.

Accepting the final disposition while suspended does not consume or publish the discarded EXIT and supplies no `x_D`.

**Targeted mutant:** accept T19’s binding despite rejecting its timestamp. Immediately after input 2, the contract requires an unbound EXIT and accessor `NONE`. A recorded binding or MATCHED disposition distinguishes the mutant.

## H12c

| # | Primitive input | Epoch/latch | Admission | Final claim and accessor | Q | payloads | Fault total |
|---|---|---|---|---|---|---:|---:|
| 1 | Cookie-less `EXIT(pid=30,t=18,L:1)`, at 20 | A/0 | — | None; `NONE`; EXIT unbound | ∅ | 1 | 0 |
| 2 | False `TERM_D19`, naming exactly `L:1`, at 20 | S/1 | — | None; `NONE`; EXIT remains unbound | Rejected `(D,L:1,19)` | 0 | 1 |
| 3 | `finish(40,C0)` | S/1 | — | None; `NONE` | Same | 0 | 2 |

Input 2 rejects the control and disposes of the EXIT payload. It leaves the cookie-less EXIT’s final-disposition obligation unsatisfied.

Finish reports a **new missing-disposition fault**. It is not another report of T19’s timestamp contradiction, and it records no disposition.

Successful merge stream:

`V20 → V40`

Combined timeline:

`O → V20 → V40`

VOID callback entries:

| Callback | Public state | Already persisted prefix | Reader for `[50,60)` |
|---|---|---|---|
| `V20` | Input-2 state | `O` | Unavailable |
| `V40` | Input-3 state; accessor still `NONE` | `O → V20` | Unavailable |

At finish, pending payload records and `unfinished_records` are **0**; unresolved inventory remains **1 lifetime**; the separate final-disposition obligation remains **unsatisfied**. That obligation justifies the finish fault. One payload was disposed of; consumption is zero.

**Targeted mutant:** erase the final-disposition obligation when disposing of its payload. Input 3 must publish `V40`, leave the accessor `NONE`, and produce fault total 2.

## H12d

| # | Primitive input | Epoch/latch | Admission | Final claim and prospective EXIT accessor | Q | payloads | Fault total |
|---|---|---|---|---|---|---:|---:|
| 1 | `D20@P0`, TRANS, position 1, at 20 | A/0 | — | None; prospective `L:1` has `NONE` | ∅ | 1 | 0 |
| 2 | `TERM_D19`, naming prospective `L:1`, at 20 | S/1 | — | None; `NONE` | Rejected `(D,L:1,19)` | 0 | 1 |
| 3 | `REARM(21,C0,fresh valid A/B proofs)` | A/1 | — | None; `NONE` | Same | 0 | 1 |
| 4 | Qualified `TERM_D21`, naming the same prospective `L:1`, at 22 | A/1 | — | Unrefuted MATCHED `(D,L:1,21)`; `M(21,D)` | Rejected T19 retained | 0 | 1 |

D20 supplies its qualified perf boundary at 20, but the other sources remain unbounded, so it does not release.

T19 is contradicted **on delivery** by retained attributable D20. It is rejected, rather than becoming a retained refuted final claim. Fault handling disposes of D20.

T21 is consistent with D20 and does not conflict with any retained final claim. It is recorded under the latch. No EXIT ever arrives, so no release evidence or discharge exists.

Successful merge stream: `V20`.

Combined timeline: `O → V20`.

At VOID entry, input-2 state is public; prefix `O`; `[50,60)` is unavailable. REARM resets release cutoffs; no subsequent boundary or DATA establishes a positive global cutoff.

Compared with H6c, this execution has **one fault**, because T19 was rejected before recording. H6c has a retained final claim and therefore rejects the later conflicting T21.

**Targeted mutant:** retain T19 as a final claim despite its delivery-time contradiction. At input 4, the contract requires `MATCHED(21,D)` unrefuted and fault total 1. Use H6c as the guard against indiscriminately permitting replacement of every refuted claim.

## H12e

| # | Primitive input, all observed at 20 | Epoch/latch | Admission | Final claim and accessor | Q | payloads | Fault total |
|---|---|---|---|---|---|---:|---:|
| 1 | Cookie-bearing `EXIT_D18@L`, position 1 | A/0 | — | None; `NONE`; independently bound EXIT | ∅ | 1 | 0 |
| 2 | False `TERM_D19`, naming exactly `L:1` | S/1 | — | None; `NONE` | Rejected T19 identity retained | 0 | 1 |
| 3 | Identical retransmission of that rejected T19 | S/1 | — | None; `NONE` | Same | 0 | 1 |
| 4 | Qualified `TERM_D18`, naming exactly `L:1` | S/1 | — | Unrefuted MATCHED `(D,L:1,18)`; `M(18,D)` | Rejected T19 retained | 0 | 1 |
| 5 | Another identical retransmission of rejected T19 | S/1 | — | Same unrefuted T18; `M(18,D)` | Same | 0 | 1 |

Input 3 is recognized against rejected-control history and adds no contradiction report. Input 4 records a valid replacement without erasing that history. Input 5 must again recognize rejected-retransmission identity **before** testing conflict with retained T18.

These are control retransmissions, not duplicate DATA envelopes. They cause no `duplicate_identity` diagnostic or exit. Their ordinary counter observations still occur and remain C0.

Successful merge stream: `V20`.

Combined timeline: `O → V20`.

At VOID entry: input-2 state, prefix `O`, startup unavailable for `[50,60)`. Exactly one payload is disposed of.

**Targeted mutant:** check conflict with the later final claim before rejected-retransmission identity. Input 5 must leave T18 unchanged and fault total 1; an additional VOID distinguishes the mutant.

## H13

Here the census is `{D,H}`. Both admissions are initially absent. Numeric pending is always zero. H’s only named EXIT is `(L:1,t=18,H)`; D has **no terminal disposition or named EXIT throughout**.

| # | Primitive input, all observed at 20 | Epoch/latch | Admission D/H | H accessor | Committed proofs | Pending lifetime + numeric | payloads | Startup state |
|---|---|---|---|---|---|---|---:|---|
| 1 | Cookie-bearing `EXIT_H18@L`, position 1 | A/0 | — / — | `NONE` | None | 2+0=2 | 1 | Open |
| 2 | `BOUNDARY(P0,19,p=1)` | A/0 | — / — | `NONE` | None | 2+0=2 | 1 | Open |
| 3 | `BOUNDARY(P1,19,p=1)` | A/0 | — / — | `NONE` | None | 2+0=2 | 1 | Open |
| 4 | `BOUNDARY(E,19,p=1)` | A/0 | — / — | `NONE` | None | 2+0=2 | 1 | Open |
| 5 | `BOUNDARY(L,19,p=2)` | A/0 | — / — | `NONE` | None | 2+0=2 | 0 | Open |
| 6 | `D20@P0{final}`, TRANS, position 2 | A/0 | — / — | `NONE` | None | 2+0=2 | 1 | Open |
| 7 | `BOUNDARY(P0,25,p=3)` | A/0 | — / — | `NONE` | None | 2+0=2 | 1 | Open |
| 8 | `BOUNDARY(P1,25,p=2)` | A/0 | — / — | `NONE` | None | 2+0=2 | 1 | Open |
| 9 | `BOUNDARY(E,25,p=2)` | A/0 | — / — | `NONE` | None | 2+0=2 | 1 | Open |
| 10 | `BOUNDARY(L,25,p=3)` | A/0 | — / — | `NONE` | None | 2+0=2 | 1 | Open; missing-ADMIT hold |
| 11 | Qualified `TERM_H18`, naming exactly `L:1` | A/0 | — / — | Unrefuted `M(18,H)` | `x_H=18` | 1+0=1 | 1 | Open |
| 12 | `ADMIT_DISCOVERY(D,α,e=6,proof,20,C0)` | A/0 | valid / — | Unrefuted `M(18,H)` | `x_H=18`, `b_D=20` | 0+0=0 | 0 | Closed: C=20, P=21 |
| 13 | `BOUNDARY(P0,30,p=4)` | A/0 | valid / — | Unrefuted `M(18,H)` | Same | 0+0=0 | 0 | Same closure |
| 14 | `BOUNDARY(P1,30,p=3)` | A/0 | valid / — | Unrefuted `M(18,H)` | Same | 0+0=0 | 0 | Same closure |
| 15 | `BOUNDARY(E,30,p=3)` | A/0 | valid / — | Unrefuted `M(18,H)` | Same | 0+0=0 | 0 | Same closure |
| 16 | `BOUNDARY(L,30,p=4)` | A/0 | valid / — | Unrefuted `M(18,H)` | Same | 0+0=0 | 0 | Same closure |

Census remains known throughout; no admission or terminal claim is refuted, and rejected-control history is empty. `b_H` and `x_D` remain absent.

Input 5 establishes `W=19`. H’s independently bound, cookie-bearing EXIT may publish without ADMIT or terminal disposition. Its successful release and required `F18` complete, but it supplies no discharge yet.

At input 10, D20 becomes eligible at `W=25`; missing ADMIT holds the batch and `F24`. Its prospective fresh-record bound is not committed evidence.

Immediately before TERM:

- H’s EXIT release is completed and retained.
- D20 remains staged under the missing-ADMIT hold.
- Neither lifetime is resolved; persisted prefix is `O → EXIT_H18 → F18`.

Input 11 records H’s unrefuted MATCHED claim and commits `x_H=18` using **H’s previously completed EXIT release**, including its successful FRONTIER and checks. D’s missing-ADMIT hold does not destroy that evidence. D remains pending; closure cannot use D’s held prospective bound. No publication occurs during TERM.

Input 12 removes D’s missing-ADMIT dependency. The complete release publishes D20 and `F24`, passes the remaining checks, then publishes END last:

`C_close = max(5, x_H=18, b_D=20) = 20`.

D20 is a released fresh-record anchor with uncertified incoming numerical evidence exactly at C, so:

`P_close = 21`.

The emitted `GAP_CLOSE` has endpoint **21** and reserved field **0**. Successful END commits `b_D=20` and closure. H’s discharge was already committed at TERM.

D20 preserves its raw payload. Its discovery incoming interval is uncertified and excluded through the persisted startup gap. The output’s existing `prior_interval_certified` boolean is **true** under the contract’s rule for generations not admitted through 25b; no discovery storage flag is added.

Successful merge stream:

`EXIT_H18 → F18 → D20[prior_interval_certified=true] → F24 → F29`

Combined publication timeline, with commitment/checkpoint annotations:

`O`
`→ [complete eligible-batch/accounting checks C0] EXIT_H18`
`→ [post-publication checks C0] F18`
`→ [release completion; no x_H yet]`
`→ [TERM: current checks C0; commit x_H=18; no publication]`
`→ [ADMIT: validated batch/checks C0] D20`
`→ [required checks C0] F24`
`→ [all remaining checks C0] END21`
`→ [END success commits b_D=20 and closure; no fallible step follows in that attempt]`
`→ [S(30), final L boundary and required checks C0] F29`.

Reader observations:

- Immediately before and after TERM: same persisted prefix, startup gap open.
- Immediately before ADMIT: same open-gap prefix.
- Immediately before END: `O → EXIT_H18 → F18 → D20 → F24`; startup still unavailable.
- Including END: append `END21`; the persisted unavailable interval is `[0,21)`.
- Timestamp 20, including its unknown incoming completed count, is excluded. Discovery imposes no extra restriction at endpoint **21**; windows beginning at 21 are eligible under the remaining coverage rules.
- For `[50,60)`, the startup restriction is removed after END when the stipulated other coverage evidence is supplied. The emitted F24/F29 alone do not establish future completeness through 60.

There are zero faults and zero payload disposals. Both DATA records are successfully published; D20 is the only numerical TRANS.

**Targeted mutants:**

- Lose H’s terminal discharge because D’s missing-ADMIT batch is held. Immediately after input 11, the contract requires `x_H=18`, pending `1+0`, and no END.
- Use D20’s held prospective bound during TERM. Input 11 must not publish END; supporting D20 and F24 have not completed.
- Emit endpoint 20 or set a reserved flag. Input 12 must persist endpoint **21**, reserved **0**.

## H6b

| # | Primitive input | Epoch/latch | Admission | Final claim and accessor | Q | payloads | Fault total |
|---|---|---|---|---|---|---:|---:|
| 1 | `D20@P0`, position 1, at 20 | A/0 | — | None; prospective `L:1`: `NONE` | ∅ | 1 | 0 |
| 2 | `TERM_D19`, naming prospective `L:1`, at 20 | S/1 | — | None; `NONE` | Rejected `(D,L:1,19)` | 0 | 1 |
| 3 | `REARM(21,C0,fresh valid A/B proofs)` | A/1 | — | None; `NONE` | Same | 0 | 1 |
| 4 | Independently bound cookie-bearing `EXIT_D18@L`, position 1, at 22 | A/1 | — | None; `NONE` | Same | 1 | 1 |
| 5 | `BOUNDARY(P0,25,p=2)`, at 22 | A/1 | — | None; `NONE` | Same | 1 | 1 |
| 6 | `BOUNDARY(P1,25,p=1)`, at 22 | A/1 | — | None; `NONE` | Same | 1 | 1 |
| 7 | `BOUNDARY(E,25,p=1)`, at 22 | A/1 | — | None; `NONE` | Same | 1 | 1 |
| 8 | `BOUNDARY(L,25,p=2)`, at 22 | A/1 | — | None; `NONE` | Same | 0 | 1 |

T19 is rejected on delivery because D20 already contradicts it. It names the exact position later occupied by EXIT18; no different-position contradiction is introduced.

The later independently bound lifecycle EXIT is not another terminal-control claim. Reconciling it with the already rejected/refuted T19 history causes no additional discovery fault solely for that inconsistency.

At input 8, `W=25`; EXIT18 publishes normally and the release completes with F24. It supplies no discharge: there is no recorded final matched disposition, and the latch also forbids a new commitment.

Successful merge stream:

`V20 → EXIT_D18 → F24`

Combined timeline:

`O → V20 → [validated release/checks C0] EXIT_D18 → [required checks C0] F24`.

At VOID entry: input-2 state; prefix `O`; startup unavailable for `[50,60)`. D20 is disposed of once. EXIT18 is subsequently published once; no abandoned or uncovered consumption occurs.

## H6c

| # | Primitive input | Epoch/latch | Admission | Final claim and prospective accessor | Rejected-control history | payloads | Fault total |
|---|---|---|---|---|---|---:|---:|
| 1 | Qualified `TERM_D19`, naming prospective `L:1`, at 20 | A/0 | — | Unrefuted MATCHED `(D,L:1,19)`; `M(19,D)` | None | 0 | 0 |
| 2 | `D20@P0`, position 1, at 20 | S/1 | — | Retained T19 now refuted; `M(19,D)` | None | 0 | 1 |
| 3 | `REARM(21,C0,fresh valid A/B proofs)` | A/1 | — | Same refuted T19; `M(19,D)` | None | 0 | 1 |
| 4 | `TERM_D21`, naming the same prospective `L:1`, at 22 | S/1 | — | Same refuted T19; `M(19,D)` | T21 rejected for conflict; no replacement claim | 0 | 2 |

Unlike H12d, T19 is validly recorded **before** contradictory D20 arrives. D20 then refutes that recorded terminal claim and causes V20. Its evidence survives payload disposal and REARM.

T21 conflicts with the retained final claim’s timestamp, even though that claim is refuted. It is rejected, causes V22 and does not replace or restore T19. This is not the special rejected-on-delivery T19 history used in H12d.

Successful merge stream:

`V20 → V22`

Combined timeline:

`O → V20 → V22`.

VOID entries:

| Callback | Public state | Persisted prefix | `[50,60)` |
|---|---|---|---|
| `V20` | Input-2 state, retained refuted `MATCHED19` | `O` | Unavailable |
| `V22` | Input-4 state, still retained refuted `MATCHED19` | `O → V20` | Unavailable |

Exactly one D20 payload is disposed of. No EXIT arrives and no discharge commits.

This control guards against a mutant that suppresses all conflicts with refuted claims: such a mutant would incorrectly accept T21 and omit V22.

## H11e

| # | Primitive input, all observed at 20 | Epoch/latch | Admission | Final claim and accessor | Q | payloads | Fault total |
|---|---|---|---|---|---|---:|---:|
| 1 | `ADMIT_DISCOVERY(D,α,e=19,proof,20,C0)` | A/0 | valid | None; `NONE` | ∅ | 0 | 0 |
| 2 | Independently bound cookie-bearing `EXIT_D18@L`, position 1 | S/1 | refuted | None; `NONE` | ∅ | 0 | 1 |
| 3 | False `TERM_D17`, naming exactly that `L:1` | S/1 | refuted | None; `NONE` | Rejected `(D,L:1,17)` | 0 | 2 |
| 4 | Qualified `TERM_D18`, naming exactly that `L:1` | S/1 | refuted | Refuted final MATCHED `(D,L:1,18)`; `M(18,D)` | Rejected T17 retained | 0 | 2 |

Input 2 independently establishes `EXIT18<enable19`, refutes the recorded admission proof and causes the first fault. Its EXIT payload is disposed of.

Input 3 has a separate false timestamp claim. Decision 40’s rejection-on-delivery rule applies before same-contradiction suppression, so it adds one fault without recording a final claim.

Input 4 records the final MATCHED disposition as **refuted**, because its EXIT participates in the already reported enable/termination contradiction. It adds no report solely for that contradiction.

Successful merge stream:

`V20 → V20`

Combined timeline:

`O → V20 → V20`.

VOID entries:

| Callback | Public state | Persisted prefix | `[50,60)` |
|---|---|---|---|
| First V20 | Input-2 state: admission refuted, no terminal claim | `O` | Unavailable |
| Second V20 | Input-3 state: admission refuted, rejected T17 history, accessor `NONE` | `O → V20` | Unavailable |

There is one disposed EXIT payload, zero release consumption and two `unknown_evidence` diagnostic increments.

The difference from H12a is the recorded ADMIT: H12a’s qualified replacement T18 is unrefuted; H11e’s is recorded refuted. Both preserve rejected-control history without treating it as a final claim.

## H11f

| # | Primitive input | Epoch/latch | Admission | Final claim and accessor | Q | payloads | Fault total |
|---|---|---|---|---|---|---:|---:|
| 1 | `ADMIT_DISCOVERY(D,α,e=19,proof,20,C0)` | A/0 | valid | None; `NONE` | ∅ | 0 | 0 |
| 2 | Cookie-less `EXIT(pid=30,t=18,L:1)`, at 20 | A/0 | valid | None; `NONE`; EXIT unbound | ∅ | 1 | 0 |
| 3 | Qualified `TERM_D18`, naming exactly `L:1`, at 20 | S/1 | refuted | Refuted final MATCHED `(D,L:1,18)`; `M(18,D)` | ∅ | 0 | 1 |
| 4 | `finish(40,C0)` | S/1 | refuted | Same; `M(18,D)` | ∅ | 0 | 1 |

Input 2 cannot establish a contradiction against D’s admission because its lifetime binding is not yet known. Input 3 supplies the binding, establishes the Decision 36 contradiction and records the final MATCHED disposition as refuted. Both recorded proofs are refuted by that one observation.

Successful merge stream: `V20`.

Combined timeline: `O → V20`.

At VOID entry, input-3 state is already public, the final disposition is MATCHED, and the EXIT payload is already disposed of. Prefix: `O`; startup coverage for `[50,60)` is unavailable.

At finish:

- Accessor: refuted `MATCHED(18,D,L:1)`.
- Pending payload records and `unfinished_records`: **0**.
- Unresolved inventory: **1 lifetime**.
- Unsatisfied final-disposition obligation: **0**.
- Fault total: **1**; no finish fault.

The difference from H12b is the recorded enable at 19. H12b has no ADMIT and its valid T18 is unrefuted. H11f’s T18 itself establishes the enable/termination contradiction, but still satisfies the requirement for a final EXIT disposition.

## Unresolved ambiguities

None was needed to derive these executions.

All proposed mutants above are design checks only; none was built or executed. Agreement with these answers establishes neither implementation correctness nor platform qualification.

## After the one-sentence clarification 9b (a fresh answerer; the certification marks of H13, D03, D02a, Q01)

| Execution | Published TRANS/MARKER records, in publication order | `prior_interval_certified` |
|---|---|---|
| H13 | D20 | **false** |
| D03 | D12 → D15 | **false → true** |
| D02a | D15 | **true** |
| Q01 | D15 | **true** |

**H13 D20 and D03 D12:** §3 explicitly decides both:

> “In particular, Decision 37(a)'s first successfully released final-attachment TRANS/MARKER carries false, including when its DATA was received and held before matching ADMIT arrived.”

**D03 D15:** Decision 37(a) states:

> “Its first successfully released TRANS/MARKER belonging to the final continuously enabled attachment establishes coherent post-record state.”

D12 establishes that state, so D15’s incoming interval is certified. §3 supplies the boolean mapping:

> “Discovery is a distinct route: its TRANS/MARKER outputs carry `prior_interval_certified=false` for incoming intervals required to remain uncertified by Decisions 37 and 43, and true for incoming intervals certified under those decisions, subject to ordinary validity rules.”

**D02a D15 and Q01 D15:** Decision 37(b) decides both:

> “Subsequent intervals based on this seed require no discovery-specific first-record exclusion.”

Both D15 intervals use the qualified quiet seed at 12, so the §3 mapping gives **true**. In Q01, the bound commits in the same successful release attempt; Decision 41 expressly permits commitments completing “in that same attempt.”

**No requested marking remains undecided.**
