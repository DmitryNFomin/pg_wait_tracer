| # | Fully specified input / starting state | Expected output |
|---|---|---|
| 24 | §5 default start. `S(11)` at `d=11,C=C0`; `CHECK(d=12,C=Ce)`; `A9@P1(Q:q1)` consumed at `d=13,C=Ce`. | `F10 → VOID12[Ce,counter_change] → VOID13[Ce,seal_violation]`. Reject A9; mark/report the epoch invalid, including its published F10. Suspension does not disable arrival fault detection. |
| 25 | §5 default start, with only A/B attached. Define `D=(pid=30,g=1)`, newly created at t=10. `FORK_D10@L, INIT_D10@L, D10@P0(Q:q1), S(11)`; all observations use `d=12,C=C0`. Valid creation evidence; no separate attachment/sequence-initialization proof for D. | N: `FORK_D10 → INIT_D10 → TRANS_D10 → F10`. Q: **not determined whether the lifecycle records supply the required initialization proof**; without that proof, `VOID12[C0,unknown_evidence]`. Under DECISION 25 below, Q produces the same output as N. |
| 26 | §5 default start. `A10@P0(Q:q1)` requiring TEXT x; `TEXTx@L`; `S(11)`; `S(50)`. Counters remain C0; x is valid and available when consumed. | `TEXTx → A10 → F10 → F49`. Both initial arrivals occur at W=0 and are staged; the dependency is available before the first eligible-batch validation. |
| 27 | §5 default start, epoch e0. `S(11)` at `d=14,C=C0`; `LOSS(P0,1,d=15,C=Ce)`; `REARM(d=16,C=Ce,attachment_proofs)` with readable baseline Ce and fresh valid A/B proofs, Q `next_q(A)=next_q(B)=1`. No fresh boundaries have yet arrived. `A9@P1(Q:q1)` at `d=20,C=Ce`; `S(12)` at `d=21,C=Ce`. | **Not determined across REARM.** Under DECISION 27: `F10 → VOID15[Ce,perf_lost] → VOID20[Ce,seal_violation]`. Reject A9, suspend new epoch e1, and mark/report e0 invalid because its F10 is disproved. The subsequent S(12) produces no output. |
| 28 | §5 default start. `A10@P0(Q:q1)` at `d=11,C=C0`, staged with W=0; `REARM(d=12,C=C0,attachment_proofs)` with readable baseline and fresh valid A/B proofs, Q proposed `next_q(A)=2,next_q(B)=1`; `S(11)` at `d=13,C=C0`. | **Not determined whether active REARM is accepted or how it treats staging.** Under DECISION 28: reject REARM without changing state or producing output; then `A10 → F10`. Original Q state remains `next_q(A)=1` until release. |

24 — §3 requires arrival seal checks; §4 says suspension continues fault reporting and late DATA at or below a published frontier additionally invalidates the affected epoch.

25 — §2 requires per-generation sequence-initialization proofs and fixes new-generation allocation at 1, but does not specify whether FORK/INIT supplies that proof; §3 governs release ordering.

26 — §3 stages DATA/TEXT at W=0, holds eligible batches only while dependencies remain unavailable, outputs TEXT before DATA, and permits advancing frontiers for empty batches.

27 — §3 preserves accepted boundary promises, while §4 requires fresh boundaries for recovery; neither explicitly defines the lifetime of validation boundaries or frontier history across REARM.

28 — §2 defines REARM as a request for a new epoch; §4 specifies recovery requirements but supplies no active-epoch admission or staged-DATA rule.

**DECISION 25 — insert in §2 after the sequence-initialization paragraph:**

> For a generation proved newly created during the current epoch by valid FORK and INIT records, those records supply its sequence-initialization proof: initialize its `next_q` to 1 before eligible-batch validation.

**DECISION 27 — insert in §4 after the recovery paragraph:**

> An accepted REARM resets release cutoffs to 0 but preserves each source’s maximum accepted boundary for arrival validation, including while fresh qualified boundaries are pending. Retain earlier epochs’ published frontiers for invalidation. Late DATA violating a retained boundary faults the current epoch and additionally invalidates every earlier epoch whose published frontier it disproves.

**DECISION 28 — insert in §4 immediately before the recovery paragraph:**

> REARM is accepted only while the epoch is suspended. Reject REARM in an active epoch without output or any state change, preserving staged DATA/TEXT, sequence state, boundaries and the counter baseline.
