- **CLARIFICATION:** Qualified perf DATA implicitly supplies a strict boundary; all example perf producers have that qualification.
- **DECISION:** Evaluate release after every individual input; v2 did not specify batching, so row 4 now includes `F9`.
- **CLARIFICATION:** Advancing FRONTIERs are emitted even for empty eligible batches, as required by v2’s examples.
- **DECISION:** Q mode requires an explicit initial expected sequence and checks gaps during eligible-batch validation; arrival order cannot establish the baseline.
- **DECISION:** Emit VOID for each separately delivered fault report, including during suspension; unchanged counter rereads are not new reports.
- **CLARIFICATION:** A disproved published frontier requires both VOID and epoch invalidation; invalidation does not replace VOID.
- **DECISION:** Expected outputs count successful publication, excluding rejected writer calls; v2 did not define that observation point.
- **DECISION:** Internal breaches require a VOID publication attempt followed by nonzero exit; v2’s normative rule only restricted when exit was allowed.
- **DECISION:** Check duplicate identity before timestamp faults, and report perf timestamp regression before the overlapping seal violation; this fixes diagnostic precedence.
- **CLARIFICATION:** Examples now specify CPU membership, generations, timestamps, sequence baselines, fault detection times, counters and starting state; these fill missing inputs.

## 2. Inputs and ordering

Inputs are:

- `DATA(s,o,t,pid,g,kind,payload[,q])`: copied record with source `s`, unique increasing source position `o`, semantic timestamp `t`, and resolved generation `g`.
- `TEXT(L,o,payload)`: untimed metadata; never invent a timestamp.
- `BOUNDARY(s,w,p)`: consumed source position `p`, proving completeness strictly below `w`.
- `LOSS(s,n,d,C)` / `FAULT(reason,d,C)`: immediate control input with detection time `d` and observed counter vector `C`, or `unknown`.
- `REARM(d,C,attachment_proofs)`: explicit request for a new merge epoch; never implied by a later boundary.
- `CHECK(d,C)`: an out-of-band counter observation, including during transport silence; this notation need not be a transport record.

Sources are fixed CPU sources `P0…Pn`, plus `E` and `L`. Generation resolution is an input obligation; ambiguity causes VOID, never PID-based guessing.
Times, positions and counters are exact unsigned integers. Preserve event timestamps, durations and payloads; no clamping, fabricated timestamps or merger-side duration repair.
Processing observations carry detection time separately from semantic timestamps and boundary values. A detected fault uses its observation’s `d`; never infer `d` from `t` or `w`.

Kinds have ranks `FORK=0`, `INIT=1`, `TRANS/MARKER=2`, `EXIT=3`. A terminal TRANS precedes its matching lifecycle EXIT.
Sort DATA by `(t,pid,g,rank,q,s,o)`, with numeric CPU source order followed by `E,L`; in sequence-free mode use `q=0`, subject to the ambiguity refusal below.
For valid same-generation TRANS/MARKER ties, causal order overrides arbitrary source ordering. Never sort tied markers by marker name or event ID.
Different physical records remain distinct, including zero-duration transitions. Duplicate delivery of the same source position is an internal invariant breach.

Before choosing the sequence envelope, measure the original trace corpus.
Group physical TRANSITIONS records by exact `(capture,pid,timestamp_ns)` across every file and block of each capture.
Let `M` contain PLAN_START/END, EXEC_START/END and CMD_START/END; exclude PID-less escalation markers.
Let `T` contain genuine non-marker transitions, including terminal transitions; exclude samples and administrative flush records.
Count every marker/transition pair: `N = Σgroups |M| × |T|`, not merely adjacent pairs.
Report nonempty mixed groups, records scanned, and pair counts with marker physically before/after transition.
Do not deduplicate equal payloads. Report ambiguous generation groups separately.

- Complete scan, `N=0`: use sequence-free mode. Any same-generation TRANS/MARKER tie spanning sources causes counted `ambiguous_tie` → VOID; this includes transition/transition migration ties.
- `N>0`: use sequence-bearing mode. Add transient uint64 per-backend `q` to every TRANS/MARKER emission attempt across all sources; allocate before publication, preserve across migration, and account for failed attempts.
- Missing/incomplete corpus or unresolved groups: the result is unknown, not zero; complete measurement before freezing the implementation branch.
- Same-source ties retain source order in sequence-free mode. Sequence-bearing mode preserves causal `q` order; gaps cause VOID and unexplained reuse is an internal breach.
- A plain `st->seq++` requires proved single-thread ownership and no concurrent/resetting writer. The prototype sequence need not become a storage column.

In sequence-bearing mode, attachment proofs establish `next_q` separately for each `(pid,g)`.
For a newly created generation, allocation starts at 1; attachment to an existing generation requires an explicit proved starting value.
The first received DATA never establishes this value. Missing sequence initialization is unknown evidence and prevents trusted release.
Do not declare a gap merely because a higher `q` arrives first.
During complete eligible-batch validation, each generation’s records, in their prospective output order, must have consecutive values beginning at its `next_q`.
A missing expected value at that point causes `sequence_gap` → VOID. Advance sequence state only with successful release.
Validate cross-source tie ambiguity at the same eligible-batch stage.
For a generation proved newly created during the current epoch by valid FORK and INIT records, those records supply its sequence-initialization proof: initialize its `next_q` to 1 before eligible-batch validation. (DECISION 25, added 2026-11-11 after the P1 review.)
DECISION 25 applies only to a `(pid,g)` with no sequence state and no earlier lifecycle record in this or any earlier epoch; any other generation of that pid must have its EXIT ordered before this FORK; and the proof takes effect only when FORK and INIT precede the generation's first TRANS/MARKER in validated output order. Otherwise the generation is `unknown_evidence`, in both N and Q modes. (DECISION 25a, lead, 2026-11-11, from Fable's challenge.)

## 3. Boundary and release rules

`BOUNDARY(s,w,p)` means every source record with `t<w` has completed publication or recorded failure, the consumer has passed its relevant position, and no unfinished producer can later produce such a record.
All covered failures must be visible in reliable out-of-band accounting. Stable counter reads alone do not prove completion of an unfinished failure path.

| Source kind | Required boundary mechanism |
|---|---|
| Shared ring `E` or `L` | Each producer reserves before taking its semantic timestamp. A boundary producer reads BPF time `w`, then reserves and submits its boundary record. Consumption includes all preceding reservations; BUSY records cannot be skipped. |
| Multi-output producer | Reserve every required destination before taking a shared timestamp. Preserve the terminal TRANS/lifecycle EXIT timestamp relationship; do not restamp only the lifecycle record. Count partial reservation failures and discard unused reservations. |
| Per-CPU perf `Pi` | With the qualified user-mode watchpoint-only data producer, consuming DATA at `t` also supplies strict boundary `w=t`, without a separate BOUNDARY input. It does not release itself or its ties. |
| Idle perf CPU | Execute a same-CPU, non-NMI BPF boundary producer whose timestamp-to-output section cannot overlap an unfinished permitted data producer. Consume its successful output. A userspace clock read or arbitrary interrupting heartbeat is insufficient. |
| Per-CPU ringbuf experiment | Use CPU-exclusive rings and the reserve-before-timestamp rule, including idle boundaries; verify that no writer can select another CPU’s ring. |

Qualification requires documented arguments for the actual kernel, permitted producers, completion, clock ordering and memory ordering; a zero-violation benchmark is insufficient.
The ring proof requires ordered clock observations between the boundary clock read and later reservations’ timestamps; the perf proof requires ordered same-CPU observations.
Neither follows merely from the name “monotonic.”
Retaining a pre-reservation semantic timestamp requires an additional completion fence covering it.
Counter visibility must cover failed reservations with no BUSY record, including completion of failed producer paths.

Process inputs sequentially. Complete fault handling or release evaluation for one input before consuming the next; transport polling batches do not change this rule.
On DATA arrival, check duplicate identity first. For qualified perf DATA, a timestamp below its preceding consumed DATA timestamp reports `clock_regression`; otherwise DATA below its accepted source boundary reports `seal_violation`.
Reject offending DATA and handle the fault before updating its implicit boundary.
Accepted boundaries accumulate completeness: `w[s] := max(w[s],w)`. An equal or weaker boundary does not retract an earlier promise.

After each non-faulting input in an active epoch, let `W=min(w[s])`; an unbounded source has cutoff 0 and blocks release.
Release only DATA with `t<W`. At `W=0`, stage DATA/TEXT and emit no frontier.
For `W>0`, the inclusive storage-style frontier is `F=W−1 ns`; this subtraction never changes event timestamps.

Before release, validate the complete eligible batch, identities, dependencies, tie order, sequences and counters.
A dependency not yet available holds the batch and its frontier; a lost or contradictory dependency causes VOID.
Output pending TEXT in lifecycle position order, then eligible DATA in key order.
After callbacks/publication succeed and the counter vector still matches the epoch baseline, emit `FRONTIER(F,C)` if it advances the last published frontier.
An empty eligible batch still permits an advancing FRONTIER. Never repeat a non-advancing frontier.

Here, an output means successful publication to the designated output sink after required callbacks succeed.
A rejected writer call is not a DATA output. If an earlier output in the same release attempt succeeded before a later failure, it remains observable but belongs to the uncertified suffix discarded by VOID.
No frontier certifies a partially failed release.

Idle time, an empty buffer, newest observed shared-ring timestamp and elapsed grace periods never substitute for boundaries.
Failure to establish a boundary means hold, not exit or speculative release.

## 4. Loss and refusals

Retain this storage counter vector, in order:

`lifecycle_fork_drop, lifecycle_init_drop, lifecycle_exit_drop, wait_read_fail, query_text_drop, pg13_query_text_drop, exact_query_update_fail, exact_cmd_update_fail, exact_phase_update_fail, accum_update_fail, fork_state_update_fail, event_ring_drop, state_map_update_fail, seen_query_ids_update_fail, writer_push_fail`.

Failed wait-transition perf output feeds `event_ring_drop`; keep transport/site/errno diagnostics separately.
Count every failed output, including boundary output, outside the lossy transport using nesting-safe accounting.
Count read/map, staging, callback and publication failures as well. Ordering faults have separate diagnostic counts and do not fabricate increments in this storage vector.
Read each component across participating CPUs. Change, unreadability, reset, wrap or incomplete accounting causes VOID.
Positive perf LOST independently causes VOID; helper-failure counts and LOST counts must not be summed as distinct missing events.
Counter checks must continue during transport silence, so a final helper failure is detected without requiring a LOST callback.

On a fault report, immediately emit `VOID(d,C,reason)`, discard the tentative merge suffix, suspend the epoch, and freeze all existing attachments at their last valid frontier.
`reason` is exactly one of: `counter_change` (a CHECK observes a vector different from the last observation), `counter_unknown` (unreadable, reset, wrapped or incomplete accounting), `perf_lost` (a positive perf LOST report), `clock_regression`, `seal_violation`, `ambiguous_tie`, `sequence_gap`, `unknown_evidence` (missing identity, generation or sequence initialization), `dependency_lost`, `writer_push_fail` (writer or callback rejected a release), `allocation_fail`, and, for internal breaches only, `duplicate_identity`, `sort_order_breach`, `sequence_reuse`, `malformed_envelope`, `queue_ownership`. (Added by the lead after the v3 oracle round, 2026-11-11: both answer authors found the labels used in §5 undefined; clarification, no behaviour change.)
Use the observed vector, including an increment caused by the failure; use `unknown` if it cannot be established.
`d` is detection time, never the missing event’s timestamp or a new coverage endpoint.
Each separately delivered LOSS/FAULT report causes a VOID even if the epoch is already suspended.
A counter observation reports a newly observed change once; rereading the same vector does not generate another report. This observation checkpoint does not replace the epoch’s valid baseline.
While suspended, suppress trusted DATA/TEXT callbacks and FRONTIERs; continue fault reporting.

REARM is accepted only while the epoch is suspended. Reject REARM in an active epoch without output or any state change, preserving staged DATA/TEXT, sequence state, boundaries and the counter baseline. (DECISION 28, added 2026-11-11 after the P1 review.)
A rejected REARM returns a distinct `rearm_rejected` status to the caller and is never silent; its counter observation `C` is still processed as `CHECK(d,C)`. (DECISION 28a, lead, 2026-11-11, from Fable's challenge.)
Recovery requires explicit REARM, a readable baseline, fresh qualified boundaries and fresh attachment proofs, including sequence initialization where applicable.
Lifecycle loss additionally requires repaired identity evidence. FRONTIER alone cannot revive attachments.
The prototype exposes this reset; full consumer recovery remains deferred.
An accepted REARM resets release cutoffs to 0 but preserves each source’s maximum accepted boundary for arrival validation, including while fresh qualified boundaries are pending. Retain earlier epochs’ published frontiers for invalidation. Late DATA violating a retained boundary faults the current epoch and additionally invalidates every earlier epoch whose published frontier it disproves. (DECISION 27, added 2026-11-11 after the P1 review.)
Across REARM, source identity and clock domain are unchanged. The last published frontier survives REARM: never emit a FRONTIER at or below any frontier published in this or an earlier epoch. Each source's last consumed DATA timestamp also survives, so the §3 check order (clock_regression before seal_violation) applies unchanged across REARM. A retained boundary that is ahead of the true clock blocks that source until the daemon restarts; this is intended fail-closed behaviour. A FRONTIER published after REARM certifies completeness only from the REARM's fresh attachment proofs onward, not for the interval lost before it; how consumers present that lower bound is deferred to step 3. (DECISION 27a, lead, 2026-11-11, from Fable's challenge.)

Clock regression, DATA below an accepted strict source boundary, ambiguous ties, sequence gaps and unknown evidence cause counted VOID and suspension, not daemon exit.
A shared-ring timestamp inversion between unrelated producers is sortable and is not itself a clock fault.
If offending late DATA has `t≤` an already published frontier, emit VOID and additionally mark and report the affected epoch invalid.
Invalidation is an out-of-band status/report, not a replacement for VOID or a new DATA/FRONTIER record. Do not continue advertising the disproved prefix as certified.
Without sufficient clock/boundary proof, report merge cutoffs as unqualified diagnostics rather than certificates.

Allocation or writer failure causes VOID and paused capture. If VOID cannot be persisted, report failure out of band and publish no frontier.
For an internal invariant breach, reject the offending input, stop ordinary output, attempt `VOID(d,C,reason)`, then exit nonzero whether or not VOID persistence succeeds.
Internal breaches include duplicate transport identity, impossible queue ownership, emission outside validated sort order, unexplained sequence reuse and malformed producer envelopes.
A duration underflow attributable to clock regression follows the clock-fault path instead.

## 5. Hand-computable expected outputs

Each row is independent; only explicitly stated starting state is inherited.
CPU set is exactly `{P0,P1}` and the complete source set is `{P0,P1,E,L}`.
Both perf producers and all boundary mechanisms are qualified as specified in §3; only explicitly injected violations contradict their promises.
`A=(pid=10,g=1)` and `B=(pid=20,g=1)`. Every record named A or B uses that generation, including lifecycle records.
`A10@P0` means A’s TRANS with semantic timestamp 10 from P0. All times are integer ns. TEXT has no timestamp.
Payloads, identities, attachment proofs and dependencies are valid unless explicitly stated; A and B are attached.
Initially all source cutoffs are 0, no frontier exists, queues are empty, and the epoch is active.

`N/Q` denote sequence-free/sequence-bearing mode. Rows apply to both unless separate answers are given.
In Q, initially `next_q(A)=next_q(B)=1`, except explicit overrides.
Unless shown otherwise, assign each generation’s TRANS/MARKER records `q=1,2,…` in listed input order; lifecycle records do not consume sequence values.
In N, omit all `q` fields. Physical source positions start at 1 and advance in source consumption order, including TEXT and boundary records; row 20 explicitly repeats a position.

`C0` is the all-zero 15-component vector; `Ce` is C0 with `event_ring_drop=1`; `Cw` is C0 with `writer_push_fail=1`.
The initial epoch baseline is C0. Counter observations remain C0 unless stated. All checks, callbacks and publications, including VOID, succeed except the stated rejection in row 19.
`F<n>` abbreviates `FRONTIER(n,C0)`; `VOID<d>[C,reason]` abbreviates `VOID(d,C,reason)`.

`S(w)` expands to `BOUNDARY(P0,w,p), BOUNDARY(P1,w,p), BOUNDARY(E,w,p), BOUNDARY(L,w,p)` in that order, each with its next source position.
`S0`, `S1`, `SE`, `SL` supply only the named source’s boundary. Every expanded input gets its own release evaluation.
A stated observation `d` on `S(w)` applies to its constituent observations, including any resulting validation fault.
`→` gives successful output order; `hold` means no output. Starting frontiers are not reprinted.
All rows keep the daemon alive except row 20.

| # | Fully specified input / starting state | Expected output |
|---|---|---|
| 1 | `B12@P1, A10@P0, S(13)` | `A10 → B12 → F12`. |
| 2 | `B10@P0, A10@P1, S(11)` | `A10 → B10 → F10`. |
| 3 | `A10@P0, S(10), S(11)` | `F9 → A10 → F10`; A is held at cutoff 10. |
| 4 | `S1(20), SE(20), SL(20), A10@P0, A12@P0` | `F9 → A10 → F11`; A12 remains held. |
| 5 | `A10@P0, S0(100)`; P1/E/L stay at 0 | `hold`; no frontier. |
| 6 | `A10@P0, S0(100), S1(100), SE(100), SL(100)` | `A10 → F99`; no fabricated idle event. |
| 7 | `B12@E, A11@E, S(13)` | `A11 → B12 → F12`; shared-ring inversion is accepted. |
| 8 | `TEXTx@L, A10@P0` requiring x, `S(11)` | `TEXTx → A10 → F10`. |
| 9 | `EXIT_A10@L, terminal TRANS_A10@E, INIT_A10@L, FORK_A10@L, S(11)`; all four describe A’s same generation | `FORK10 → INIT10 → TRANS10 → EXIT10 → F10`. |
| 10 | `A enters X at 10@P0, A leaves X at 10@P0, S(11)`; Q uses q1 then q2 | `enter-X10 → leave-X10 → F10`; preserve the distinct zero-duration wait. |
| 11 | Q baseline `next_q(A)=3`, with q1–q2 accounted for by attachment proof. `TRANS_A10@P0(Q:q4), MARKER_A10@E(Q:q3), S(11)` observed at `d=20,C=C0` | N: `VOID20[C0,ambiguous_tie]`. Q: `MARKER10 → TRANS10 → F10`. |
| 12 | Q baseline `next_q(A)=1`. `A second TRANS10@P0(Q:q2), A first TRANS10@P1(Q:q1), S(11)` observed at `d=20,C=C0` | N: `VOID20[C0,ambiguous_tie]`. Q: `first10 → second10 → F10`; preserve the zero-duration intermediate state. |
| 13 | `A enters X at 4@P0, S(6)`; P0 helper output fails at time 8, changing C to Ce; `CHECK(d=9,C=Ce)` detects it | `A4 → F5 → VOID9[Ce,counter_change]`; A and B freeze at 5; X is not closed at 9. |
| 14 | Start suspended: all cutoffs 6, last frontier F5, empty queues, A/B frozen at 5, X open; baseline C0, last observed Ce; Q next values A=2, B=1. `LOSS(P0,1,d=12,C=Ce)` | `VOID12[Ce,perf_lost]`; no recovery or additional helper-drop increment. |
| 15 | Start active with all cutoffs 6, existing F5 and empty queues. P0 helper output fails at time 8, changing C to Ce, then P0 stays silent; `CHECK(d=9,C=Ce)` | `VOID9[Ce,counter_change]`; no later frontier or required LOST callback. |
| 16 | `A12@P0`, then `A11@P0` consumed at `d=20,C=C0`; P1/E/L stay at 0 | `VOID20[C0,clock_regression]`; neither DATA is released. |
| 17 | `S(11)`, then `A9@P1` consumed at `d=12,C=C0` | `F10 → VOID12[C0,seal_violation]`; reject A9 and mark/report the epoch invalid. |
| 18 | `S(11), A11@P1, S(12)` | `F10 → A11 → F11`; equality with the strict source boundary is legal. |
| 19 | Start active with all cutoffs 6, existing F5 and empty queues. `A7@P0, S(8)`; pre-release checks see C0; writer rejects A7 at `d=9`, incrementing C to Cw | `VOID9[Cw,writer_push_fail]`; rejected A7 is absent, no F7, capture paused. |
| 20 | `DATA(P0,o=1,t=10,A,TRANS,Q:q1)`, then an exact duplicate of that envelope consumed at `d=20,C=C0`; other cutoffs stay 0 | `VOID20[C0,duplicate_identity] → exit nonzero`; no DATA or frontier precedes exit. |
| 21 | Start active: all cutoffs 0, no frontier, empty queues; baseline and last observed C0; Q next values A=1, B=1. `SE(20), SE(10)`, then `A15@E` consumed at `d=30,C=C0`, then `S(21)` observed at `d=31,C=C0`. | `VOID30[C0,seal_violation]`; reject A15 and suspend; no DATA or frontier. |
| 22 | Start active: all cutoffs 0, no frontier, empty queues; baseline and last observed C0; Q next values A=1, B=1. `A10@P0(Q:q2), S(11)` observed at `d=20,C=C0`. | N: `A10 → F10`. Q: `VOID20[C0,sequence_gap]`; no DATA or frontier. |
| 23 | Start suspended: all cutoffs 6, no published frontier, empty queues, A/B frozen with no valid frontier; baseline and last observed C0; Q next values A=1, B=1. `CHECK(d=10,C=C0)`. | `hold`; unchanged counters cause no new VOID, and suspension suppresses FRONTIER. |
| 24 | §5 default start. `S(11)` at `d=11,C=C0`; `CHECK(d=12,C=Ce)`; `A9@P1(Q:q1)` consumed at `d=13,C=Ce`. | `F10 → VOID12[Ce,counter_change] → VOID13[Ce,seal_violation]`. Reject A9; mark/report the epoch invalid, including its published F10. Suspension does not disable arrival fault detection. |
| 25 | §5 default start, with only A/B attached. Define `D=(pid=30,g=1)`, newly created at t=10. `FORK_D10@L, INIT_D10@L, D10@P0(Q:q1), S(11)`; all observations use `d=12,C=C0`. Valid creation evidence; no separate attachment/sequence-initialization proof for D. | N: `FORK_D10 → INIT_D10 → TRANS_D10 → F10`. Q: same as N (DECISION 25). |
| 26 | §5 default start. `A10@P0(Q:q1)` requiring TEXT x; `TEXTx@L`; `S(11)`; `S(50)`. Counters remain C0; x is valid and available when consumed. | `TEXTx → A10 → F10 → F49`. Both initial arrivals occur at W=0 and are staged; the dependency is available before the first eligible-batch validation. |
| 27 | §5 default start, epoch e0. `S(11)` at `d=14,C=C0`; `LOSS(P0,1,d=15,C=Ce)`; `REARM(d=16,C=Ce,attachment_proofs)` with readable baseline Ce and fresh valid A/B proofs, Q `next_q(A)=next_q(B)=1`. No fresh boundaries have yet arrived. `A9@P1(Q:q1)` at `d=20,C=Ce`; `S(12)` at `d=21,C=Ce`. | `F10 → VOID15[Ce,perf_lost] → VOID20[Ce,seal_violation]`. Reject A9, suspend new epoch e1, and mark/report e0 invalid because its F10 is disproved. The subsequent S(12) produces no output (DECISION 27). |
| 28 | §5 default start. `A10@P0(Q:q1)` at `d=11,C=C0`, staged with W=0; `REARM(d=12,C=C0,attachment_proofs)` with readable baseline and fresh valid A/B proofs, Q proposed `next_q(A)=2,next_q(B)=1`; `S(11)` at `d=13,C=C0`. | REARM rejected without output or state change (DECISION 28); then `A10 → F10`. Q state remains `next_q(A)=1` until release. The REARM input returns status `rearm_rejected` (DECISION 28a). |
| 29 | §5 default start, only A/B attached. `D=(pid=30,g=1)` is new. `D9@P0(Q:q1)`, `FORK_D10@L`, `INIT_D10@L`, `S(10)`, `S(11)`; all observations use `d=12,C=C0`. | `VOID12[C0,unknown_evidence]` in both modes: at W=10 D9 is eligible but D's FORK/INIT (t=10) are not, so no creation proof precedes D's first TRANS (DECISION 25a). The later `S(11)` produces no output (suspended). |
| 30 | Row 27's start and first three inputs: `S(11)` at `d=14,C=C0`; `LOSS(P0,1,d=15,C=Ce)`; `REARM(d=16,C=Ce,attachment_proofs)` accepted. Then `S(5)` at `d=17,C=Ce`. | `F10 → VOID15[Ce,perf_lost]`; after REARM, release cutoffs are 5 but F4 is at or below the retained F10, so no FRONTIER is emitted (DECISION 27a). |
| 31 | §5 default start (active, baseline C0). `REARM(d=12,C=Ce,attachment_proofs)`. | REARM rejected with status `rearm_rejected` (DECISION 28/28a); its observation is processed as `CHECK(d=12,C=Ce)`: `VOID12[Ce,counter_change]`. |
