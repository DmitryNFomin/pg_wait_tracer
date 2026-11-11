# Merge-stage contract (issue #871) — normative text for expected-answer authors

## 2. Inputs and ordering

**Decision — inputs:**

- `DATA(s,o,t,pid,g,kind,payload[,q])`: copied record; source `s`, unique increasing source position `o`, timestamp `t`, resolved generation `g`.
- `TEXT(L,o,payload)`: untimed metadata; never invent a timestamp.
- `BOUNDARY(s,w,p)`: consumed source position `p`, proving completeness strictly below `w`.
- `LOSS(s,n,d,C)` / `FAULT(reason,d,C)`: immediate control input; detection time `d`, counter vector `C` or unknown.
- `REARM(d,C,attachment_proofs)`: explicit new merge epoch after fault handling; never implied by a later boundary.

Sources are `P0…Pn`, `E`, `L`; CPU membership is fixed for the prototype. Generation resolution is an input obligation; ambiguity causes VOID, never PID-based guessing.
Times, positions and counters are exact unsigned integers. Preserve event timestamps, durations and payloads; no clamping, fabricated timestamps or merger-side duration repair.
Kinds have ranks `FORK=0`, `INIT=1`, `TRANS/MARKER=2`, `EXIT=3`. A terminal TRANS precedes its matching lifecycle EXIT.
Sort DATA by `(t,pid,g,rank,q,s,o)`, with numeric CPU source order followed by `E,L`; in sequence-free mode use `q=0`, subject to the ambiguity refusal below.
For valid same-generation TRANS/MARKER ties, causal order overrides any arbitrary source ordering. Never sort tied markers by marker name or event ID.
Different physical records remain distinct, including zero-duration transitions. Duplicate delivery of the same source position is an internal invariant breach.

**Decision — sequence measurement, before choosing the envelope:**
For every original trace capture in the measurement corpus, group physical TRANSITIONS records by exact `(capture,pid,timestamp_ns)`, across all its files and blocks.
Let `M` contain PLAN_START/END, EXEC_START/END and CMD_START/END; exclude PID-less escalation markers. Let `T` contain genuine non-marker transitions, including terminal transitions; exclude samples and administrative flush records.
Count **every marker/transition pair**, not just adjacent records: `N = Σgroups |M| × |T|`. Also report nonempty mixed groups, records scanned, and pair counts with marker physically before/after transition.
Do not deduplicate equal payloads. Report ambiguous generation groups separately; do not silently drop them to obtain zero.

- Complete scan, `N=0`: prototype without `q`; any same-generation TRANS/MARKER tie spanning sources causes counted `ambiguous_tie` → VOID. This includes transition/transition migration ties.
- `N>0`: add a transient uint64 per-backend sequence to every TRANS/MARKER emission attempt across all sources; allocate before transport publication, preserve across migration, and include failures in sequence accounting.
- Missing/incomplete corpus or unresolved groups: result is **unknown**, not zero; complete the measurement before freezing the implementation branch.
- Same-source ties retain source order in sequence-free mode. In sequence mode, preserve `q` order; gaps cause VOID, and unexplained reuse is an internal breach.
- A plain `st->seq++` is sufficient only after proving single-thread ownership and no concurrent/resetting writer. The prototype sequence need not become a storage column.

## 3. Boundary and release rules

**Decision:** `BOUNDARY(s,w,p)` means every source record with `t<w` has completed publication or recorded failure, the consumer has passed its relevant position, and no unfinished producer can later produce such a record.
All failures covered by the boundary must be visible in reliable out-of-band accounting. Stable counter reads alone do not prove that an unfinished failure path has completed.

| Source kind | Required boundary mechanism |
|---|---|
| Shared ring `E` or `L` | Each producer reserves before taking the semantic timestamp. A boundary producer reads BPF time `w`, then reserves and submits its boundary record. Consuming it must include all preceding reservations; BUSY records cannot be skipped. |
| Multi-output producer | Reserve every required destination before taking a shared timestamp. In particular, preserve the terminal TRANS/lifecycle EXIT timestamp relationship; do not restamp only the lifecycle record. Count partial reservation failures and discard unused reservations. |
| Per-CPU perf `Pi` | With the qualified user-mode watchpoint-only data producer, a consumed record at `t` supplies a strict boundary `w=t`; it does not release itself or its ties. |
| Idle perf CPU | Execute a same-CPU, non-NMI BPF boundary producer whose timestamp-to-output section cannot overlap an unfinished permitted data producer. Consume its successful output. A userspace clock read or an arbitrary interrupting heartbeat is insufficient. |
| Per-CPU ringbuf experiment | Use CPU-exclusive rings and the reserve-before-timestamp ring rule, including idle boundary records; verify that no writer can select another CPU’s ring. |

The ring proof requires ordered clock observations between the boundary’s clock read and later reservations’ timestamps. The perf proof requires ordered same-CPU observations. Neither follows merely from the name “monotonic.”
If retaining any pre-reservation semantic timestamp, supply an additional completion fence covering it; the cheap ring seal is then insufficient.
Counter visibility must also cover failed reservations with no BUSY record. Audit this explicitly for both successful and failed producer paths.

Let `W=min(w[s])`; an unbounded source blocks release. Release only DATA with `t<W`.
The inclusive storage-style frontier is `F=W−1 ns`; at `W=0`, no frontier exists. This subtracts from the cutoff, never from event timestamps.
Before release, validate the complete eligible batch, identities, tie order and counters. Output pending TEXT in lifecycle position order, then eligible DATA in key order.
After callbacks/publication succeed and the counter vector still matches the baseline, output `FRONTIER(F,C)`. Do not repeat a non-advancing frontier.
A dependency not yet available holds the batch; a lost or contradictory dependency causes VOID.
Idle time, a temporarily empty buffer, newest observed shared-ring timestamp, and elapsed grace periods never substitute for boundaries.
Failure to establish a boundary means **hold**, not exit and not speculative release.

## 4. Loss and refusals

**Decision:** retain the storage vector, in this order (`S:73`):

`lifecycle_fork_drop, lifecycle_init_drop, lifecycle_exit_drop, wait_read_fail, query_text_drop, pg13_query_text_drop, exact_query_update_fail, exact_cmd_update_fail, exact_phase_update_fail, accum_update_fail, fork_state_update_fail, event_ring_drop, state_map_update_fail, seen_query_ids_update_fail, writer_push_fail`.

Failed wait-transition perf output feeds `event_ring_drop`; keep transport/site/errno diagnostics separately.
Count every failed output, including boundary output, outside the lossy transport using nesting-safe accounting. Count read/map, staging, callback and publication failures as well.
Read each counter component across participating CPUs. Change, unreadability, reset, wrap or incomplete accounting causes VOID.
A positive perf LOST notification independently causes VOID. Helper-failure counts and LOST counts must not be summed as distinct missing events.
A final helper failure followed by permanent silence must still be detected without a LOST callback.

On loss, output `VOID(d,C,reason)` immediately; discard the tentative merge suffix and freeze **all** existing attachments at their last valid frontier.
`d` is detection time, never the missing event’s time or a new coverage endpoint. FRONTIER alone cannot revive attachments.
Recovery requires a readable baseline, fresh qualified boundaries and fresh attachment proofs; lifecycle loss also requires repaired identity evidence.
The prototype must expose this reset and suppress further trusted callbacks from the old epoch; full consumer recovery is deferred.

Clock regression, data below an accepted strict source boundary, ambiguous ties, sequence gaps and unknown evidence state cause counted VOID and suspend the epoch; **they do not cause daemon exit**.
A shared-ring timestamp inversion between unrelated producers is sortable and is not itself a clock fault.
If late data disproves an already published frontier, ordinary VOID cannot repair that certificate: mark the affected prototype epoch invalid and report it. Do not advertise the false prefix as certified.
Without a sufficient clock/boundary proof, report merge cutoffs as unqualified diagnostics rather than v5 certificates.
Allocation or writer failure causes VOID/paused capture; if VOID cannot be persisted, report failure out of band and publish no frontier.
Exit nonzero only for internal invariant breaches, such as duplicate transport identity, impossible queue ownership, or emission out of the validated sort order—not merely for permitted kernel clock behavior.
Malformed producer envelopes are internal ABI breaches; a duration underflow attributable to clock regression follows the clock-fault path.

## 5. Hand-computable expected outputs

Each row is independent. Times are integer ns; all payloads/identities are valid unless stated.
`A` has PID 10, `B` PID 20; `A10@P0` means A’s transition at 10 from P0.
`S(w)` supplies valid boundaries for every source; `Si(w)` only for Pi. Initially all cutoffs are 0 and no F exists.
`→` shows output order; `hold` means no DATA release. Sequence-free/sequence-bearing modes are `N/Q`.
Counter checks and publication succeed unless stated. Position order is input order within each source.

| # | Input | Expected output |
|---|---|---|
| 1 | `B12@P1, A10@P0, S(13)` | |
| 2 | `B10@P0, A10@P1, S(11)` | |
| 3 | `A10@P0, S(10), S(11)` | |
| 4 | `A10@P0, A12@P0`; other sources boundary 20 | |
| 5 | `A10@P0, S0(100)`; P1/E/L remain at 0 | |
| 6 | Same setup, then P1/E/L boundary 100 | |
| 7 | E delivers `B12, A11`; then `S(13)` | |
| 8 | L delivers untimed `TEXTx`, then `A10@P0, S(11)` needing x | |
| 9 | Same PID/time: EXIT, terminal TRANS, INIT, FORK arrive reversed; `S(11)` | |
| 10 | Same-source A enters X at 10, then leaves X at 10; `S(11)` | |
| 11 | A’s `TRANS10@P0(q=4)` arrives before `MARKER10@E(q=3)`; `S(11)` | |
| 12 | A’s second transition `10@P0(q=2)` arrives before first `10@P1(q=1)`; `S(11)` | |
| 13 | A enters X at 4; `S(6)`; B also attached; loss detected at 9 | |
| 14 | After row 13, delayed `LOST=1` arrives at 12 | |
| 15 | F5 exists; helper failure increments a counter; that CPU emits nothing further | |
| 16 | P0 receives A12 then A11 while other source cutoffs remain 0 | |
| 17 | `S(11)`, then late P1 DATA at 9 | |
| 18 | `S(11)`, then P1 DATA at 11, then `S(12)` | |
| 19 | F5 exists; eligible A7; writer rejects its publication | |
| 20 | Same source position delivered twice | |
