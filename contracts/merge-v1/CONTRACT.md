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
- **DECISION:** Direct-attach revision r2 (2026-11-13): replace DECISIONS 25/25a, add 25b/25c, qualified fork-seed input and prior-interval output markings, adapter obligations and rows 32–43. Fable challenged the draft; r2 accepts all six points.

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
A generation newly created during the current epoch establishes creation through either valid FORK+INIT records or a valid FORK carrying the qualified fork-seed proof of 25b. Either route initializes Q-mode `next_q=1` before validating that generation’s TRANS/MARKER sequence. First arrival never establishes the sequence baseline. (DECISION 25, revised 2026-11-13 in direct-attach r2.)
This admission applies only to a `(pid,g)` with no prior sequence state or lifecycle history in this or earlier epochs, apart from the records constituting this creation proof. Every previously known generation of that PID must have EXIT ordered before this FORK. All records required by the selected proof must precede its first TRANS/MARKER in validated output order; otherwise `unknown_evidence` causes VOID in both modes.
A qualified fork-seed FORK selects 25b even when a genuine INIT also exists. INIT neither reinitializes sequence state nor removes 25c’s coverage restriction. An invalid direct-attachment attempt cannot be laundered into the legacy route by a later INIT. (DECISION 25a, revised 2026-11-13 in direct-attach r2, accepting Fable’s challenge.)

A qualified proof binds the FORK, fork-created state, producing child task and attached task to the same nonzero lifetime cookie `g`. One shared allocation authority serves BPF and daemon allocation paths: `(pid,g)` is never reissued to a different lifetime within retained history, including across REARM; allocator reset, wrap or exhaustion cannot silently reuse it. REARM retains an existing task’s cookie.
Each `on_fork` installs fresh state and a fresh cookie, replacing stale PID state before child evidence production; failure is counted and disqualifies the proof. Neither inherited state nor state created outside `on_fork` qualifies. Each queued record retains its producing task’s cookie; resolving it from the PID’s current map entry is forbidden.
Attachment order is: open the watchpoint disabled on a lifetime-bound task → read fork-state cookie → compare with the consumed FORK and target task → seed without overwriting cookie, sequence or concurrent producer updates → enable. Task binding and state identity must remain valid throughout; a later matching read cannot repair an earlier enable.
Cleanup deletes only the matching lifetime’s state and closes only its attachment. Cookie comparison and deletion must be atomic or serialized against replacement; a userspace lookup followed by unconditional PID deletion is insufficient.
In Q, allocation starts at 1 and covers every TRANS/MARKER emission attempt across all sources, including failures; attachment, INIT, reseeding and migration cannot reset it. All sources must establish the continuous post-enable observation regime required by 25c. (DECISION 25b, added 2026-11-13 in direct-attach r2.)

Add input `bool fork_seed_proved` to `pgwt_merge_record`, meaningful only on FORK and default false; keep `identity_known` separate.
Reuse `g` under 25b’s shared allocation rule. Existing attachment proofs continue to govern other routes. No fabricated INIT, new input kind, active-epoch REARM or counter-vector extension is required.

**Adapter truth table:** Each negative case changes only the stated fact from a fully qualified case; unknown facts count as false. A failed attempted direct attachment must additionally report `FAULT(unknown_evidence,d,C)` before trusted release, preventing fallback through a real INIT.

| Producer fact | `fork_seed_proved` | Required mutant |
|---|---:|---|
| All 25b/25c obligations proved | true | Force false; row 32’s successful output must fail |
| State created outside `on_fork` | false | Accept any existing map entry |
| Enable preceded cookie visibility, later read matches | false | Accept the later matching read |
| FORK cookie differs from attached task/state | false | Resolve identity using current PID alone |
| Stale fork entry adopted, cookie reused, or cleanup can delete a replacement | false | Accept `BPF_NOEXIST` fallback or unguarded deletion |
| Cookie/q overwritten by preseed or INIT | false | Treat reseeded state as preserving provenance |
| First marker cannot establish fresh post-enable state | false | Treat marker arrival alone as a coverage boundary |

**Producer obligations:** Implement the allocation authority, fresh fork replacement, task-bound attachment order, serialized conditional cleanup, emission-time cookies, preserved sequence allocation and reliable failure accounting. Qualify first-record state establishment across every source; make INIT handling idempotent or report the coverage break. A pure adapter cannot manufacture these facts.

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

For a 25b generation, the first successfully released TRANS/MARKER carries `prior_interval_certified=false`. Its old_event, duration and other quantities describing the incoming interval are unavailable for numerical use, including completed-wait counts. Preserve their raw bytes; do not zero, clamp or repair them.
That record establishes the generation’s coverage lower bound at its semantic timestamp and ordered position. Certification extends forward only; before it, coverage is unavailable. Subsequent TRANS/MARKER records carry `prior_interval_certified=true`, subject to ordinary validity rules. FRONTIER cannot certify time before this lower bound; VOID and REARM retain their existing effects.
The first record must establish fresh, coherent state for subsequent intervals after observation is enabled. If it is a MARKER, the producer must also refresh the wait-state/time baselines it would otherwise leave stale. Pre-enable markers or stale marker snapshots cannot establish this boundary; without this qualification, the direct route remains unknown evidence. (DECISION 25c, added 2026-11-13 in direct-attach r2.)

A genuine INIT retains its original bootstrap-observation timestamp and is released normally; handling it for an already directly attached generation must be idempotent, with no disable, reattach or state overwrite.

Add output-only `bool prior_interval_certified` to `pgwt_merge_output`, meaningful on TRANS/MARKER. The core computes it in validated release order and commits its first-record state only on successful publication. (Added by the lead after review, 2026-11-13:) Every TRANS/MARKER of a generation not admitted through 25b (an attached generation, or one created by FORK+INIT under 25) carries `prior_interval_certified=true`, so rows 1-31 are unchanged; only a 25b generation's first released TRANS/MARKER carries false.
Persist the output marking with the record; downstream consumers must retain its lower-bound meaning.

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

Any subsequent attachment interruption or destructive reseed remains a counted coverage fault, delivered as FAULT(unknown_evidence,d,C) before further trusted release; it requires suspension and explicit recovery. Missing FORK, identity, provenance or attachment ordering remains unknown evidence except for the complete A2 proof below. Reliable loss counters, including actual INIT loss, independently cause VOID. Cookies never cancel loss, revive epochs or establish boundaries.

A2 is the following narrow exception to the failed-direct-attachment refusal in §2. It admits a completely reconciled, never-attached lifetime only when:
(a) an observed FORK produced during this capture and a kernel EXIT bind the same (capture,pid,g), with a nonzero, non-reused lifetime cookie and boot_start <= fork_ns <= exit_ns;
(b) EXIT has DISPOSITION and lacks WP_EVER_LIVE, and complete lifetime-bound userspace history contains no successful wait-watchpoint enable or attachment interruption;
(c) (BOOTSTRAP_FIRED,INIT_STORED) is (0,0) with init_ts=0, or (1,1) with fork_ns <= init_ts <= exit_ns;
(d) retained disposition, attachment history and delivered records agree, and every required identity proof, state observation and loss-accounting obligation is complete;
(e) every unsuccessful attachment operation is expressly admissible under the closed table below; no other required read, write or publication failed or is unknown.

A delivered INIT must agree exactly with the stored INIT timestamp. Stored INIT carried only by EXIT is sufficient when the producer protocol required no separate INIT publication. Never synthesize an INIT from that stored timestamp.
Neither lack of attach opportunity nor a maximum lifetime is an admission criterion. This rule applies to every postmaster child type on every supported OS.
boot_start is the recorded start of this capture's writer boot, in the event clock. Receiving the FORK after the first ring drain or after EXIT does not alter its birth timestamp or disqualify an otherwise complete proof.

Admissible non-attachment evidence is no attach attempt, cancellation after consuming the matching kernel EXIT, or an unsuccessful operation from this closed table:
- target /proc open: ENOENT, with independently retained lifetime/task binding;
- perf_event_open(disabled): ESRCH, with retained lifetime/task binding;
- state-map lookup: ENOENT only when the matching retained EXIT supplies the missing terminal state;
- SET_BPF, ENABLE, state-map update and every other operation: no admissible failure errno.

Every admitted unsuccessful operation additionally requires the matching-cookie kernel EXIT, no successful enable, and no unsuccessful operation outside this table.
Capture operation, target identity, result and original errno before logging or cleanup. PID-only observations, a later negative liveness check, or errno alone are insufficient.
The adapter must qualify each permitted operation/error mapping on the supported kernels. An unqualified mapping supplies unknown evidence. Do not require failure_time >= exit_ns.
An admissible ENOENT lookup whose terminal state is supplied by EXIT is not an unknown terminal-state read; all other required read failures retain their existing treatment.

Pending EXIT evidence holds the affected release batch and its frontier. Terminal reconciliation without the matching kernel EXIT, contradictory evidence, or any non-admissible attachment failure reports FAULT(unknown_evidence,d,C).
In particular, fired-but-not-stored, absent disposition, successful-enable/clear-EVER_LIVE races and observation-only death receive no exception.

For a qualifying lifetime, omit only the synthetic INIT-loss increment whose sole cause would be never attaching. Never subtract, mask or forgive an actual counter change.
The completed A2 proof admits FORK/EXIT without an attachment proof or separately delivered INIT. It does not admit TRANS/MARKER numerical evidence. A TRANS/MARKER lacking an independent valid observation proof remains unknown evidence; an A2 declaration cannot repair it.

After ordinary complete-batch validation, release FORK and any genuine INIT normally, then UNTRACED(pid,g,fork_ns,exit_ns,receive_ns) immediately before the matching EXIT, exactly once.
The declaration's ordering timestamp is exit_ns. receive_ns is the original userspace dequeue time of that lifetime's FORK, in the same monotonic clock; require receive_ns >= fork_ns, but do not require receive_ns <= exit_ns.
UNTRACED declares the whole half-open interval [fork_ns,exit_ns) unobserved. It supplies no wait, CPU time, completed-wait count, zero-work assertion or coverage lower bound.
Publication failure causes writer_push_fail VOID. Successfully published records preceding that failure remain observable only in the uncertified suffix; no frontier certifies a partially published release.

For a new lifetime without another valid creation/attachment proof, hold FORK and its release batch and frontier until the qualifying EXIT is eligible for release.
Eligibility remains t < W, where W is the minimum qualified source boundary; merely receiving EXIT is insufficient. After successful release and unchanged counters, normal FRONTIER(W-1 ns) advancement resumes.
Existing pre-enable marker restrictions, actual loss paths, batch validation and counter checks remain unchanged. A2 presentation failure alone does not suspend the merge epoch.
Presentation follows ../coverage-a2-v1/CONTRACT.md.

A FORK+INIT-created lifetime that meets the A2 proof is an A2 lifetime and receives UNTRACED; the DECISION 25 creation route does not displace it. (Clarification added by the lead after the A2 oracle round, 2026-11-19: both independent answer sets already gave this answer for M46; no behaviour change.)

REARM is accepted only while the epoch is suspended. Reject REARM in an active epoch without output or any state change, preserving staged DATA/TEXT, sequence state, boundaries and the counter baseline. (DECISION 28, added 2026-11-11 after the P1 review.)
A rejected REARM returns a distinct `rearm_rejected` status to the caller and is never silent; its counter observation `C` is still processed as `CHECK(d,C)`. (DECISION 28a, lead, 2026-11-11, from Fable's challenge.)
Recovery requires explicit REARM, a readable baseline, fresh qualified boundaries and fresh attachment proofs, including sequence initialization where applicable.
DECISION 29 (owner-authorised saturation round, 2026-11-17): after a transient-capacity VOID, the daemon may issue the explicit REARM; nothing else in this contract changes.
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

Rows 32–43 independently use the §5 defaults, with only A/B initially attached and new `D=(pid=30,g=1)` unless overridden.
Every observation uses `d=20,C=C0` unless stated otherwise; rows apply to both modes unless distinguished.
Source positions and `S(w)` expand as above; no additional inputs or outputs are implied. There is no nonzero exit or epoch invalidation.
`K_D10=FORK_D10@L{fork_seed_proved=true}`; ordinary FORK has `fork_seed_proved=false`.
All records have `identity_known=true`, ready dependencies and unchanged payloads. Output FORK preserves its input proof bit.
`[U]` means explicit output `prior_interval_certified=false`; `[T]` means `prior_interval_certified=true`. Listed q values apply only in Q.
`Ci` is C0 except `lifecycle_init_drop=1`.

| # | Fully specified input / starting state | Expected output |
|---|---|---|
| 32 | `K_D10, D12@P0(q1), S(13)` | `FORK_D10 → D12[U] → F12`. |
| 33 | `FORK_D10@L{fork_seed_proved=false}, D12@P0(q1), S(13)` | `VOID20[C0,unknown_evidence]`. This single core row replaces draft r1 rows 33/36; their producer causes are distinguished by the adapter truth table in §2. |
| 34 | Override `D=(pid=10,g=2)`; attached A has no EXIT. `K_D10, D12@P0(q1), S(13)` | `VOID20[C0,unknown_evidence]`. |
| 35 | `D12@P0(q1), S(13)` | `VOID20[C0,unknown_evidence]`; fork-seeded state without a delivered FORK is insufficient even with C0. |
| 36 | `K_D10, INIT_D11@L, D12@P0(q1), S(13)` | `FORK_D10 → INIT_D11 → D12[U] → F12`. |
| 37 | `K_D10, D12@P0(q2), S(13)` | N: `FORK_D10 → D12[U] → F12`. Q: `VOID20[C0,sequence_gap]`. |
| 38 | `D9@P0(q1), K_D10, S(10), S(13)` | `VOID20[C0,unknown_evidence]`. |
| 39 | `D12@P0(q1), K_D10, S(13)` | `FORK_D10 → D12[U] → F12`. |
| 40 | `K_D10, D12@P0(q1), S(13), INIT_D14@L, D15@P0(q2), S(16)` | `FORK_D10 → D12[U] → F12 → INIT_D14 → D15[T] → F15`. |
| 41 | `K_D10, D12@P0(q1), S(13), FAULT(unknown_evidence,20,C0), S(16)` | `FORK_D10 → D12[U] → F12 → VOID20[C0,unknown_evidence]`; models a reported reattachment break. |
| 42 | `K_D10, MARKER_D12@E(q1), D14@P0(q2), S(15)`; marker satisfies 25c’s fresh-state obligation. | `FORK_D10 → MARKER_D12[U] → D14[T] → F14`. |
| 43 | Every input observes Ci. `CHECK(20,Ci), K_D10, D12@P0(q1), S(13)` | `VOID20[Ci,counter_change]`. |

## 6. Discovery admission (DECISIONs 30-45)

Status: INFERRED normative design (2026-11-20). This text asserts neither implementation nor platform qualification. It reverses the earlier exclusion of processes already running at capture start (owner decision recorded on issue 988: "a").

Measured before freezing (tests/results/session-20261120/quiet-witness/analysis.md, one run per kernel, shell subjects, not PostgreSQL backends): on 4.18.0-553.157.1.el8_10 and on 6.8.0-139 a sleeper read inactive through `/proc/<pid>/syscall` with an unchanged voluntary context-switch count, a spinner read "running" in 200 of 200 reads, a sleeper woken once between the reads showed the count move from 1 to 2, and 1,000 quiet brackets gave 0 anomalies. Not measured: a PostgreSQL backend as the subject, reads under load, io_uring worker threads.

**DECISION 30 — Scope and provenance.** Discovery admission is a distinct attachment route for a lifetime proved present at capture startup. It initially applies only to exact_perf, sequence-free mode N. It creates neither FORK nor INIT and does not assert a birth timestamp. A task with valid observed fork provenance follows the existing fork route. A post-start birth, including a fork whose task start timestamp predates capture, cannot use discovery to replace missing fork evidence. Q-mode discovery remains unqualified.

**DECISION 31 — Inventory and census.** Activate reliable lifecycle accounting before startup enumeration and before any discovery installation. After activation completes, record `F0` as an event-clock instant and store it kernel-side before enumeration begins. Establish `F0≤boot_start`; use a qualified conversion when comparing a task start time with either instant. Unknown conversion, zero or unreadable task start time is unknown evidence, never proof that the task precedes `F0`. Observed fork provenance takes precedence over a start-time comparison.

Maintain an inventory of bound lifetimes, including failed and pending attachments. Retain an enumerated PID that cannot be bound as an unresolved numeric census observation, separately from that inventory. It reserves no lifetime identity or discovery cookie. Repeated unresolved observations of the same number do not create additional lifetimes.

Reconcile enumeration, numeric observations, births and exits through a qualified census fence. The proof covers processes appearing or disappearing during enumeration. An unresolved numeric observation prevents census completeness. An empty registry is not a complete empty census. Missing enumeration, lifecycle or completion evidence leaves census completion unknown; elapsed time does not complete it.

Each inventory entry retains its binding evidence, reserved cookie if any, attachment attempts and terminal history. PID alone is not its lifetime identity.

Resolve each unbound numeric observation by exactly one of J, R or G; otherwise retain it pending. All three require consistent PID namespaces, retained lifecycle history from `F0`, completed relevant producers and reliable failure accounting through the resolving fence.

J: A retained kernel terminal witness identifies `(pid,start_ns)` and proves `tid==tgid`, `real_parent->tgid==postmaster`, a known `start_time<F0`, and absence of valid fork provenance. Reconcile all retained fork evidence before selecting this route. Create or identify the corresponding inventory lifetime from the witness and apply Decision 40's matched disposition at its `exit_ns`. No `/proc` read is required. An eligible J witness takes precedence over R or G.

R: A new pinned handle binds the current holder of the number. Its lifetime, leader status, parent, start time and retained fork history classify it as an ordinary discovery candidate, a fork-route lifetime, or a non-member. A post-`F0` target child without valid fork evidence is not a non-member or a discovery substitute. Rebinding a replacement never removes a separate earlier lifetime established by the journal.

G: After the failed open, `kill(pid,0)` returns ESRCH, and a subsequently completed lifecycle fence establishes that no eligible J witness exists and all relevant loss counters remain readable and unchanged over `[F0,fence]`. Drop the numeric observation as a non-member. G discharges no inventoried lifetime and supplies no `x_i`. Open failure, including ENOENT, is not the gone-proof; success, EPERM or another result from `kill(pid,0)` is not ESRCH.

Journal insertion failure, capacity failure, unknown history or incomplete failure accounting authorizes none of these resolutions. Resolving a numeric observation never removes a retained target-lifetime witness or forgives a loss.

For census completeness, the lifecycle-loss counter class comprises `lifecycle_fork_drop`, `lifecycle_init_drop`, `lifecycle_exit_drop` and `fork_state_update_fail`. A newly observed change in this class makes census completeness unknown even after an accepted census fence; retain established inventory identities and numeric-observation resolutions. Other counter changes, including `wait_read_fail`, `writer_push_fail` and `event_ring_drop`, do not by themselves invalidate an established census, although all retain their ordinary VOID consequences. Independently missing enumeration, identity, journal or completion evidence still leaves census completeness unknown.

**DECISION 32 — Allocation authority.** Partition the capture’s nonzero uint64 cookie space: the serialized fork allocator owns `[1,2^63−1]`; the single serialized discovery allocator owns `[2^63,2^64−1]`. Neither crosses its partition. Reserve and burn a discovery cookie before attempting installation; failed reservations/installations do not recycle it. Exhaustion reports allocation failure without wrapping. REARM preserves allocated identities and allocator positions. Retrying attachment to the same proved lifetime retains its installed cookie.

**DECISION 33 — Task binding.** Open the watchpoint disabled. Obtain fresh post-open `/proc` observations and retain one pinned task handle through the attachment attempt. Resolve all subsequent status, syscall, pointer and memory reads through that binding; reopening a numeric PID pathname does not preserve it. Prove that the FD, memory, postmaster membership and inventory entry identify the same lifetime.

Let `h=ceil(10^9/_SC_CLK_TCK)` ns. Use a conservative upper bound on the task’s start time, including one tick and all qualified clock-conversion uncertainty, to establish that it precedes `boot_start`. Prefer comparison in the boot-clock domain with a recorded conservative bound corresponding to `boot_start`. An unknown clock mapping or within-margin start remains pending.

The start-time test is necessary classification evidence, not sufficient binding evidence. Complete lifecycle accounting must exclude exit/replacement across open, reads, installation, enable and baseline establishment. A fork stamped before boot but made visible afterward retains its observed fork provenance. No finite age margin replaces this reconciliation. Dependent records remain staged until binding is complete.

**DECISION 34 — Discovery identity map.** Use a regular, non-evicting PID-keyed map whose values are either an immutable discovery identity or a tombstone. The daemon inserts identities only with `BPF_NOEXIST`; it never replaces or deletes an entry during the capture.

The kernel fork path installs a tombstone before attempting fork-cookie allocation or making the child runnable. The exit path installs a tombstone before returning, including when mutable state or a discovery entry is absent. Lifecycle filtering must cover every task that can be a discovery candidate. Tombstones survive until capture teardown. Their capacity is an explicit resource requirement; insertion/update failure enters reliable fault accounting.

An existing identity is reusable only for the identical retained lifetime and cookie. EEXIST alone proves nothing. A tombstone refuses discovery installation. Neither delayed userspace cleanup nor retry removes it.

**DECISION 35 — Emission and mutable-state ownership.** Resolve identity at emission time: a valid current-lifetime fork cookie takes precedence; otherwise use a live discovery identity. A tombstone supplies no cookie. A stale mutable-state cookie is not a valid fork cookie.

Copy the resolved cookie into the envelope before publication; queued records retain it. Resolve terminal TRANS and EXIT from one identity snapshot. Resolve the child’s identity, not the parent’s, for FORK.

Discovery identity installation must not replace `state_map` or overwrite producer-owned wait, CPU, query, command or sequence fields. Baseline initialization uses isolated provisional state for a new attachment generation. Initialize that state before making it visible to its producers; after visibility, the daemon must not replace it or restore an earlier snapshot. Retain earlier generations, their records and their accounting until their producers are fenced and their evidence reconciled.

A failed bracket rejects the proposed numerical baseline. It does not roll back producer writes. A retry creates another provisional generation; it does not overwrite the failed generation. Preserve attribution and lifetime identity across this operation. Every producer must use the state belonging to its attachment generation, and every producer writer must be covered by the ownership and completion protocol.

All emission sites, including markers and terminal paths, implement the same identity rule. Preserve existing reservation, completion and failure-accounting guards.

**DECISION 36 — Admission control.** Add control input:

`ADMIT_DISCOVERY(pid,g,attachment_generation,e,proof,d,C)`.

Here `e` is the successful enable-completion time of the final attachment. The control establishes discovery provenance and continuous enablement from that attachment, but supplies no coverage lower bound or source boundary. Its proof includes Decisions 31–35, route-specific address qualification, installation visibility before enable, successful enable and preserved producer state. For Decision 37(c), the admitted attachment is the qualified pointer watch; no wait-word address, numerical seed or CPU baseline is asserted. Issue its ADMIT only after the initial NULL witness and permanent-role proof pass. Unknown facts are false.

Issue ADMIT once for the final startup attachment. A valid control may arrive after dependent DATA; those records and their release frontier wait for matching ADMIT or accepted ABANDON_DISCOVERY under Decision 45. Abandonment permits validated consumption of the abandoned numerical stream, not its publication or numerical certification. Matching terminal lifecycle evidence retains Decision 40’s discharge route without ADMIT. Contradictory provenance or a claimed admission proof containing a false required fact reports `FAULT(unknown_evidence,d,C)`.

A record from an earlier attachment attempt, or stamped before `e`, cannot establish the fresh-record lower bound. Resolve attempt membership from retained attachment and producer-fence evidence; timestamp equality alone does not prove membership in the final attachment.

No genuine INIT repairs an invalid discovery attempt. Admission neither rearms a suspended epoch nor cancels a fault.

**DECISION 37 — Three lower-bound routes.** A discovery lifetime obtains its lower bound by exactly one of:

(a) **Fresh-record route.** Its first successfully released TRANS/MARKER belonging to the final continuously enabled attachment establishes coherent post-record state. Mark its incoming interval uncertified, including duration, CPU and completed-wait count. Set `b_i` to that record’s semantic timestamp and ordered position. A marker qualifies only if it refreshes the necessary wait/time/CPU baselines. Earlier-attempt records remain numerically uncertified and do not establish or replace this state.

(b) **Quiet-baseline route.** Prove continuous task inactivity across this bracket, in this order:

1. Read `v0 = voluntary_ctxt_switches`, thread count and nonterminal task status through the pinned task handle.
2. Obtain `S0`, a successful kernel inactivity proof.
3. Resolve the complete wait-address chain inside the bracket and compare its target with the address configured in the disabled watchpoint.
4. Read wait word `X`.
5. Read a fresh event-clock timestamp `t`.
6. Install the isolated provisional seed: wait `X`, baseline time `t`, off-CPU state, preserved attribution, and a valid zero-consumption CPU base.
7. Enable the watchpoint and confirm successful enablement; record completion time `e`.
8. Obtain `S1`, another successful kernel inactivity proof.
9. Read `v1 = voluntary_ctxt_switches`, thread count and nonterminal task status through the same pinned task handle.

The supported inactivity interface is a successful `/proc/<pid>/syscall` observation whose qualified implementation uses `wait_task_inactive()` to establish completed descheduling and absence from the runqueue. A readable non-`running` result is accepted only under that interface qualification. A state letter, unlocked runqueue field or scheduler counter alone is insufficient.

The bracket passes only when both inactivity proofs succeed, `v0=v1` without reset or wrap, the task remains single-threaded and nonterminal, task binding remains continuous, the address comparison succeeds, and all required operations succeed. The ownership assumptions in Decision 39 must hold throughout. A denied, unreadable, unsupported or malformed proof is a failed bracket.

Scheduler CPU accounting must be active and enrolled before `S0`, including all CPUs on which the task can subsequently execute. The seed sets `on_cpu_ts=0` even when `X=0`. If the preserved CPU accumulator is `A`, set the interval CPU base to `A` and set a separate validity bit. Numeric zero is a valid accumulator/base value; it must not mean “no baseline.” Subsequent CPU consumption is measured from this base. Neither a zero wait word nor a zero CPU delta authorizes substituting elapsed wall time for measured CPU.

Add control input:

`CONFIRM_DISCOVERY(pid,g,attachment_generation,t,baseline_witness,d,C)`.

The witness carries `X`, `t`, `e`, both inactivity results, both voluntary-switch observations, task/thread observations, resolved and armed addresses, CPU state/validity, initialization ownership evidence and the attachment binding. It is neither DATA nor a source boundary. Its physical bracket precedes ADMIT; delivery of the confirmation control may follow ADMIT.

Accept the lower bound only after matching ADMIT, qualified source completeness strictly beyond `t`, completed failure accounting, and reconciliation of consumed and staged records with the attachment history. Reconcile silent producer updates as well as DATA. The inactivity proof establishes the bracket’s task silence; it does not replace stream completion, lifecycle reconciliation or publication checks.

On successful commitment set `b_i=t`. Subsequent intervals based on this seed require no discovery-specific first-record exclusion. Records preceding the baseline, or belonging to earlier attempts, remain uncertified and must not modify the confirmed baseline.

A failed witness supplies no confirmation. A control that falsely claims completed coherence, identity or continuity reports `FAULT(unknown_evidence,d,C)`; it cannot convert contradictory evidence into a normal retry.

(c) **No-wait-word route.** This route applies only to a bound discovery lifetime whose qualified permanent role cannot acquire or use a wait word. A NULL pointer alone does not establish that role.

Open the qualified pointer watch disabled on the pinned task, establish identity and observation-state visibility, enable it and record successful completion time `e`. After `e`, read the complete pointer through the pinned memory handle and observe NULL. Complete a producer/lifecycle fence after that read. The retained witness establishes zero pointer-watch fires between enable and that fence, unchanged readable counters, continuous binding and ownership, and a lifetime-bound permanent-role fact obtained through the same binding. No inactivity bracket, voluntary-switch equality, wait-word read or numerical seed is required.

Add control input `CONFIRM_NO_WAIT_WORD(pid,g,attachment_generation,e,witness,d,C)`. The witness includes the ordered enable/read/fence observations, pointer-watch coverage, role qualification, binding, producer completion and reliable failure accounting. It consumes no transport position and supplies no source boundary.

Commit `b_i=e` only after matching route-(c) ADMIT, qualified source completeness strictly beyond `e`, reconciliation of all retained attachment and lifecycle evidence, completed failure accounting, and successful publication of the required supporting coverage evidence. A failed candidate witness supplies no bound. A control falsely claiming completed identity, coherence or continuity reports `FAULT(unknown_evidence,d,C)`.

Keep the pointer watch continuously armed for the remaining lifetime. A subsequent fire whose qualified observation is NULL supplies no numerical or lifecycle event and manufactures no loss; retain its internal observation accounting. A fire observing non-NULL contradicts the permanent-role proof and follows Decision 43. A failed required pointer read retains its ordinary failure accounting. This route creates no TRANS, MARKER, INIT, wait seed or CPU seed.

**DECISION 38 — Confirmation failure and retries.** Use a finite configured maximum of `N` wait-word attachment attempts, recorded in diagnostics. Every such attempt starts with the wait-word watchpoint disabled. The independent pointer watch remains continuously armed where required. Every retry is a disable followed by completion fencing of earlier producers and a complete new bracket in Decision 37(b)’s order. Preserve earlier records, counters and producer updates. Every retry occurs before ADMIT (for a cookie-less EXIT, see the final terminal disposition of Decision 40).

After ADMIT there is no disable, reseed or replacement of the admitted attachment to repair its history. Confirmation-control delivery and fence completion may still be pending; neither authorizes another physical bracket.

A failed bracket, including denied or unreadable proof, supplies no lower bound. It does not by itself manufacture a loss increment. Independently failed required reads, writes, publications or continuity obligations retain their existing fault accounting.

On exhaustion, a lifetime with an independently valid final attachment may receive ADMIT for the fresh-record route. It remains pending until a qualifying record actually releases or a matching terminal discharge is accepted. A task may run briefly and then remain idle indefinitely; “it ran, so a record will come” is false. When attachment attempts permanently cease without a valid final attachment, an installed, never-admitted discovery identity receives ABANDON_DISCOVERY under Decision 45. Its inventory entry remains unresolved until a matching terminal discharge is accepted. A candidate without an installed discovery identity remains pending under the existing inventory and terminal-discharge rules.

The quiet-baseline route is required scope. Ordinary indefinitely idle, otherwise qualified clients and auxiliaries must be exercised by qualification; waiting for their first transition does not satisfy that requirement.

A successfully read NULL pointer is not a failed wait-word address read and consumes no attachment attempt. A NULL candidate without a qualified permanent role receives no ADMIT for Decision 37(c), no numerical baseline and no lower bound. Retain its pointer watch and pending inventory entry.

A qualified non-NULL pointer fire makes that still-pending lifetime eligible for ordinary Decision 37(a) or 37(b) attachment; wait-word attempts start then. The pointer fire itself supplies neither a lower bound nor a numerical record. A matching exit uses Decision 40.

For an authenticating client, the waiting target is at most its remaining qualified `authentication_timeout`. Expiration alone supplies no initialization, terminal discharge, census completeness or coverage bound. If the expected initialization or exit evidence is absent at that limit, retain pending status and report that the waiting limit was exceeded. Do not extend the timeout claim to other unqualified roles.

An accepted Decision 37(c) bound cannot transition to a fresh-record or quiet-baseline route. Later initialization contradicts its permanence proof and requires Decision 43 fault handling and revocation.

**DECISION 39 — Address and ownership qualification.** Apply identical admission semantics to PG13 and PG18 while qualifying address resolution separately. For Decisions 37(a) and 37(b), PG13 qualifies resolved `MyProc` plus the field offset, and PG18 qualifies the resolved wait-pointer target. Reading only the pointer variable or a stale former target is insufficient for either numerical route. Decision 37(c) instead qualifies the complete pointer variable and its permanent-role proof; it does not qualify a NULL target as a wait-word address.

Quiet confirmation assumes one thread throughout the bracket, no foreign writer of the observed wait word or its address chain, and no producer writer outside the qualified initialization/ownership protocol. Thread count comes from status observations through the pinned task handle; lifecycle evidence must also exclude an intervening thread or lifetime change. Endpoint thread counts alone do not prove that history.

Inactivity protects the in-bracket address resolution, not future address stability. A permanent auxiliary dummy may qualify. A transient initialization dummy, unresolved `MyProc`, invalid address or unobserved relocation cannot. Qualify a stable lifecycle phase or complete relocation observation for subsequent continuous attachment. Repeated pointer equality does not exclude pointer ABA.

An address mismatch rejects the bracket. A later ADMIT requires its own valid address proof; the failed bracket does not authorize admission against the wrong address. A demonstrated observation-continuity violation follows the existing fault rules.

For Decision 37(c), qualify a per-major, binary-specific permanent-role set. Read `MyBackendType`, or another independently qualified role discriminator, through the pinned binding and require membership in that set. The qualification establishes that the identified role cannot acquire or use a wait word during the remaining lifetime. A process title, shared executable, one NULL observation or an unqualified numeric role value is insufficient.

The pointer watch covers the complete qualified pointer width and every relevant write, including same-value writes and both legs of ABA. Its observation protocol preserves each relevant written value or reliably reports failure; silently missing a non-NULL value followed by NULL is forbidden. It remains effective across permitted scheduling and migration without silent disabling or multiplexing.

Single-thread ownership, absence of foreign writers, qualified executable identity and complete thread/exec history hold from pointer-watch binding through lifetime termination. Endpoint observations alone do not prove this history. A change invalidating an accepted no-wait-word proof follows Decision 43. Unknown or unsupported assumptions refuse confirmation. Permanent-role qualification for one major does not qualify another major.

**DECISION 40 — Terminal discharge without state.** EXIT accounting must not depend on `state_map` existence. For an unresolved census lifetime, a complete matching kernel terminal proof at `x_i` may discharge its inventory entry without establishing numerical evidence.

If a discovery cookie exists, snapshot it before tombstoning. If exit precedes identity installation, retain a kernel terminal witness and resolve it against retained census/lifecycle history; do not infer identity from the PID’s current map entry. Cookie-less EXIT is not automatically matching evidence. For every cookie-less lifecycle EXIT whose PID matches an inventory entry the daemon issues exactly one final terminal disposition, a control that consumes no source position and supplies no boundary: matched, when retained census/lifecycle history binds the EXIT to exactly one inventoried lifetime, otherwise unresolved. That EXIT and its frontier are held only until the disposition; no elapsed time, idle period or source boundary substitutes for it, and a capture finished without it reports FAULT(unknown_evidence). On unresolved the EXIT is consumed as an uncovered record: counted once, never published, no VOID; the entry stays unresolved, the pending count and census completeness are unchanged, and the startup gap stays open for the rest of the capture. A matched disposition after an unresolved one for the same EXIT, or the reverse, reports FAULT(unknown_evidence); an identical duplicate is idempotent. A disposition delivered before the EXIT it names applies when that EXIT is consumed, which is then not held. A contradictory match supplies no discharge.

Without mutable interval state, emit no invented terminal TRANS. Publish the lifecycle witness with ordinary reserve-before-timestamp and reliable failure-accounting rules. ENOENT, ESRCH, a missing `/proc` entry and later negative liveness checks alone are insufficient to discharge an inventoried lifetime. Decision 31(G) applies only to an unbound numeric observation and additionally requires its completed, loss-free journal proof.

Resolve valid current-lifetime fork provenance first and preserve the existing fork route. Without such provenance, emit a cookie-less lifecycle EXIT only when the exiting task proves all three facts: `tid==tgid`, `real_parent->tgid==postmaster`, and known `start_time<F0`. A discovery cookie is usable only when it is bound to this exact kernel lifetime and the same three facts hold. Neither the existence of a live discovery identity nor a tombstone is a pass condition.

A leader that is a postmaster child with known `start_time>=F0` and no valid fork cookie is a missing-fork loss, never a discovery terminal. Reliably account it in `lifecycle_fork_drop` when that missing fork has not already been accounted; preserve independently recorded failures. Emit no discovery EXIT for it. An unknown start time cannot pass either time comparison; for a postmaster-child leader without valid fork provenance, report `FAULT(unknown_evidence,d,C)` without inventing a loss-counter increment. Non-leaders and non-children emit no discovery lifecycle EXIT; retain any thread/lifecycle journal bookkeeping required for ownership proofs.

Construct terminal witnesses in the exiting task's context, retaining `(pid,start_ns)`, leader and parent facts, fork provenance, and `exit_ns`. The corresponding lifecycle EXIT has that same semantic timestamp and a retained transport identity. Journal insertion and publication obey reserve-before-timestamp, producer-completion and reliable failure-accounting rules.

A J witness from Decision 31 establishes the journal-only inventory lifetime before its matched disposition is applied. Require completed relevant producers, including entered/completed accounting, and readable unchanged relevant counters from `F0` through the terminal reconciliation fence. For an already bound inventory entry, require exact agreement with its retained kernel start identity. Match the particular EXIT transport record and journal witness; never match through the PID's current map value, first-EXIT arrival order or a later replacement.

A matched disposition supplies `x_i=exit_ns` only when the corresponding EXIT passes strict release eligibility and the complete release attempt commits without fault, including all counter checkpoints and any required FRONTIER publication. Control receipt, a journal entry alone, or an EXIT in a subsequently VOIDed release attempt supplies no discharge. Preserve the existing unresolved-disposition and duplicate-disposition rules.

If an otherwise valid J witness's `exit_ns` contradicts its named lifecycle EXIT timestamp, report `FAULT(unknown_evidence,d,C)` and supply no discharge. Retain the J-established inventory lifetime and resolution of the numeric observation; this timestamp mismatch alone changes neither census completeness nor those established identities.

**DECISION 41 — Startup gap and closure.** Open a hard base-coverage gap at `boot_start` before advertising startup coverage. Each inventoried lifetime remains unresolved until it supplies either a committed lower bound `b_i` from Decision 37(a), 37(b) or 37(c), or an accepted terminal discharge `x_i`. An exit after establishment of `b_i` does not replace that lower bound.

Replace T-cov rule 5’s closure condition with:

`C_close = max(census_fence, every committed lower bound b_i, every terminal discharge x_i)`.

Compute `C_close` only after census completeness and discharge of every inventory entry. Require qualified completeness strictly beyond `C_close`, successful validation and accounting of all applicable Decision 45 discards, and successful publication of all supporting records and coverage evidence before committing closure. Persist the gap end `P_close` defined by Decision 42. Publication failure commits neither the affected lower bound nor gap closure. Neither `C_close` nor `P_close` supplies a source boundary or advances FRONTIER.

The interval `[boot_start,P_close)` is unavailable, with `P_close` from Decision 42. Windows beginning at or after `P_close` are eligible only under all other coverage, loss, attribution and A2 rules. Abandonment supplies neither a lower bound nor terminal discharge; an abandoned lifetime remains unresolved until Decision 40 discharges it. Pending diagnostics separately report unresolved inventoried lifetimes and unresolved numeric census observations, and report their sum as the total pending count. A number is not counted again after it is resolved into an inventory lifetime. Separately report whether census completeness is known. A known complete census with no unresolved obligations is distinct from an unknown census.

Decision 37(c) changes neither the `C_close` formula nor its completeness and publication requirements. Its bound alone supplies no uncertified TRANS/MARKER at `C_close` and therefore triggers no Decision 42 increment. Apply Decision 42 to all other applicable records and discards: their presence at `C_close` can still require `P_close=C_close+1 ns`. A later lifecycle EXIT for a no-wait-word lifetime does not change its committed bound or the already persisted startup endpoint.

**DECISION 42 — No discovery storage carrier.** Use the existing persisted startup gap; add no per-record discovery storage carrier. For an admitted lifetime, uncertified incoming intervals, including earlier-attempt intervals, must end at or before its committed `b_i≤C_close`. For an abandoned lifetime resolved by terminal discharge, its discarded timed records must be stamped at or before its accepted `x_i≤C_close`. Reconcile these facts before closure; contradictory evidence retains its existing fault consequences. Preserve the raw payload bytes of published records.

Persist the gap end `P_close`. Set `P_close=C_close+1 ns` if any released discovery TRANS/MARKER whose incoming numerical evidence is uncertified—a fresh-record anchor, an earlier-attempt record, or a record discarded under Decision 45—has semantic timestamp exactly `C_close`; otherwise set `P_close=C_close`. For this test, a Decision 45 discard counts when it passes validated release consumption, although it is never published. Merely staged or rejected records do not count. Untimed TEXT has no semantic timestamp, and a lifecycle EXIT alone does not trigger the increment.

When the increment applies, the number of completions at `C_close` is unknown, not zero. The persisted unavailable interval includes that instant, so no window containing it is available. Otherwise discovery imposes no additional exclusion at `C_close`; certified completions there retain their ordinary window membership. If `C_close+1 ns` overflows, closure does not commit. Store `P_close` in the existing GAP_CLOSE endpoint field and keep its reserved field zero. This requires no format, reader or consumer change. Durations use ordinary half-open clipping.

The quiet-baseline route qualifies only when subsequently certified intervals use its coherent baseline. An earlier-attempt record cannot rebase them. Gap persistence alone supplies no wait or CPU observations. Missing retained coverage evidence remains unavailable, including on replay or cache reuse. Existing Decision 25c persistence obligations are unchanged.

**DECISION 43 — Faults, late discovery and A2.** Preserve existing loss counters and fault precedence. No discovery operation subtracts or forgives a real loss. Failed reads, allocations, map operations and publications retain their applicable accounting and refusal behavior.

Retain the existing late-unlisted startup revocation: discovering an unlisted pre-existing lifetime after closure revokes base coverage from `boot_start`, including affected cached answers. This remains a regression requirement. Discovery does not authorize recovery-time admission or automatic REARM. Independently established faults retain their existing VOID behavior.

Discovery contributes neither an observed birth to `N_k` nor an omission to `U_k`. A pre-existing terminal discharge is not UNTRACED. Apply this clarification to coverage-a2-v1’s population and late-attachment rules.

A Decision 37(c) lifetime contributes nothing to `N_k`, `U_k`, any numerical numerator or the measured population. It supplies no wait duration, completed-wait count, CPU time or off-CPU time. Absence of contributions is not a measurement of zero work.

Show the process as 'present, not measured', with its qualified role. Its individual numerical quantities are unavailable, not zero; identify the exclusion when presenting the measured population. Preserve the lifetime-bound role and coverage evidence required to maintain this meaning on replay and cache reuse. A persisted gap endpoint alone supplies no role or numerical observation.

A non-NULL pointer fire after an accepted Decision 37(c) bound reports `FAULT(unknown_evidence,d,C)`. A demonstrated break in its required continuous observation or ownership proof has the same consequence, in addition to independently established counter faults. Revoke that lower bound and base coverage from `boot_start`, invalidate affected cached answers, reopen the startup gap and return the lifetime to pending status. Census completeness must be re-established. Do not replace revocation with a handoff gap or automatically REARM.

**DECISION 44 — Qualification and evidence.** exact_perf remains non-default and EL8-only until the actual vendor 4.18 kernel passes PG13 and PG18 qualification. Ubuntu qualification is separate. Preserve the separately authorized idle-boundary default. Idle boundaries advance completeness; they do not establish a backend baseline.

The supplied control report records one shell-probe run on each kernel:

| Kernel | Sleeper | Spinner | Woken once | Sleeper brackets |
|---|---|---|---|---|
| `4.18.0-553.157.1.el8_10.x86_64` | Inactive syscall observation; voluntary count `1→1` | `running` in 200/200 reads | Voluntary count `1→2` | 1,000 brackets; zero observed anomalies |
| `6.8.0-139-generic` | Inactive syscall observation; voluntary count `1→1` | `running` in 200/200 reads | Voluntary count `1→2` | 1,000 brackets; zero observed anomalies |

These are measured controls from `quiet-witness/analysis.md`, not universal scheduler proofs or PostgreSQL qualification. The report did not measure a PostgreSQL subject, the narrow state-before-`schedule()` window, reads under load or PG18 io_uring worker threads.

The contract assumes the qualified `/proc/syscall` interface provides the `wait_task_inactive()` semantics required by Decision 37, voluntary-count observations cannot reset/wrap across a bracket, task-handle binding remains stable, and the ownership assumptions in Decision 39 hold. Unsupported or unproved assumptions refuse quiet confirmation.

Before qualification passes, exercise quiet clients and auxiliaries, valid-zero CPU accounting, failed-attempt state preservation, pointer initialization/relocation, fork/open/exit/install races, tombstone capacity failure, every emission site, verifier acceptance and absence of newly introduced NMI reservation hazards. Do not assume pidfds, BPF spin locks, fetch-return atomics or newer helpers. The shell controls satisfy the requested witness control; they do not satisfy these remaining qualification obligations.

Freeze this contract and obtain two independent contract-only answer sets before implementation. Adapter/kernel rows exercise the stated physical facts; abstract merge rows do not establish those facts merely by setting a proof boolean.

**DECISION 45 — Abandoned discovery.** Add control input:

`ABANDON_DISCOVERY(pid,g,d,C)`.

It consumes no transport source position and supplies no source boundary. When startup attachment attempts for an installed discovery identity permanently end, the daemon issues one final disposition: ADMIT for an independently valid final attachment, or ABANDON when none exists. Retransmission of an accepted abandonment is permitted as specified below.

Accept a first abandonment only for an inventoried, unambiguously bound discovery identity `(capture,pid,g)` with no accepted ADMIT in any epoch of the capture, while the startup gap is open. Unknown or contradictory identity facts do not qualify. An invalid abandonment reports `FAULT(unknown_evidence,d,C)`. Acceptance is permanent for that identity throughout the capture and survives REARM. It does not change the identity map, remove a tombstone, recycle a cookie, alter queued cookies or affect another lifetime using the same PID.

An identical duplicate of an accepted abandonment is idempotent, including after terminal discharge or gap closure: it creates no second disposition, discard or diagnostic increment. Continue ordinary counter observations on every control; idempotence does not suppress an independently observed fault. Any later ADMIT or CONFIRM for the abandoned identity is contradictory evidence and reports `FAULT(unknown_evidence,d,C)`. An already admitted identity cannot be abandoned.

From acceptance, that lifetime’s staged and future TRANS/MARKER records and TEXT belonging exclusively to its abandoned numerical stream are consumed under the ordinary release rules, counted once in `discovery_abandoned_records`, and never published. This diagnostic is separate from the existing 15-counter vector. Shared metadata or another lifetime’s records are not discarded by this decision. Untimed TEXT remains untimed. Count each completed discard once even if a subsequent publication in the same release attempt fails.

Abandonment removes only the missing-ADMIT dependency. Before a frontier passes a discarded record, validate its transport identity, source position, arrival timestamp and retained boundaries, and validate the complete eligible batch’s applicable identities, dependencies, ordering and continuity. Reconcile attachment history, producer completion and reliable failure accounting; account for every consumed discard; obtain ordinary qualified source completeness strictly beyond the proposed frontier; and successfully publish all required supporting and coverage evidence. Missing completion proof holds release. Established failures retain their existing fault handling. No discarded record may bypass duplicate, clock, seal, tie, sequence, counter, dependency or publication checks, or conceal evidence disproving an earlier frontier.

Every discarded timed record must remain inside the persisted unavailable interval. While the lifetime is unresolved, the startup gap stays open. Abandonment supplies no `b_i`, does not discharge the inventory entry and does not establish census completeness. Only an accepted Decision 40 terminal discharge resolves the abandoned lifetime. Its matching lifecycle EXIT is released normally under strict eligibility and successful-publication rules; it is not discarded. Terminal reconciliation must establish that the lifetime’s discarded timed records are stamped at or before `x_i`. A discarded record exactly at `C_close` triggers Decision 42’s increment.

Abandonment is not a loss and forgives no loss. Failed enable, binding, read, allocation, map, staging, accounting and publication operations retain their applicable consequences. A refuted admission claim or uncertain record attribution cannot be converted into a successful discard. Acceptance neither cancels a fault nor rearms a suspended epoch; trusted release remains suspended until an independently valid REARM.

### Clarifications after the oracle round (lead, 2026-11-20)

Two independent answer sets (oracle-discovery/answers-sol.md, answers-opus.md) agreed with each other and with the expected outputs on 38 of 39 rows. These sentences decide the remaining points; neither changes an agreed answer.
- After a late unlisted pre-existing lifetime is discovered (row D10), the startup gap is open again from `boot_start`, the pending inventory holds that lifetime (pending 1), and census completeness is no longer established. The observation alone produces no merge VOID.
- A wait that is still open in a row contributes no completed count and no duration; it is not reported as zero duration (row D02c).

After the review of the merge unit found two gaps (the instant C, and an identity installed but never admitted), DECISIONs 36, 38, 41 and 42 were revised and DECISION 45 (abandoned discovery) was added. The persisted gap end follows Fable's rule (C+1 only when an uncertified discovery record is stamped exactly C; no flag in the reserved field); Astra, who had proposed a flag, raised no soundness objection to it. Two fresh independent answer sets for the 34 changed and new rows (oracle-discovery/answers-delta-sol.md, answers-delta-opus.md, compare-delta.md) agreed with each other and with the expected outputs on 33 rows. This sentence decides the remaining one; it changes no agreed answer.
- A terminal discharge commits only when the release attempt that output its EXIT completes without fault, including every counter checkpoint and the FRONTIER publication when one is due; an EXIT output in an attempt that is then VOIDed discharges nothing in that or any later epoch, whatever the order of TERM and EXIT. A rejected EXIT publication likewise leaves the lifetime pending and the startup gap open (rows A15, V1-V4).

### Oracle

Rows (inputs only), the expected outputs, both answer sets and their comparison are in `oracle-discovery/`. Their transcription into `examples.json` comes with the implementation unit, as for rows 32-43.

