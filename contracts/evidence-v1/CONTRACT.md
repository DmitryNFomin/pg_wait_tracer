# Evidence records v1 — proposed trace v5 storage contract

Status: normative DRAFT, not a description of implemented capture. Source evidence, unverified
assumptions, and unresolved capture mechanisms are separated in `notes.md`. Frozen wait_time v1
is unchanged except the proposed lifetime-start clarification. No oracle answers accompany this draft.

## 1. Domain, container, and ordering

Times are JSON integer tokens / unsigned 64-bit monotonic nanoseconds; arithmetic is exact.
Intervals are half-open. `null` is unknown where specified; zero is a valid timestamp.
Trace v5 retains v4 TRANSITIONS (type 0), SAMPLES (type 1), and sample-weight semantics, and adds
exactly one block type: EVIDENCE (type 2), payload version 1, containing the five kinds below.
No kernel birth token, event sequence number, or periodic per-backend open checkpoint is added.
This contract freezes logical fields and logical replay semantics; binary offsets/encoding are not frozen.
FRONTIER barriers, coherent rereads, and coherent END instants are proof obligations, not demonstrated
capture mechanisms (notes N2, N4, N5). Qualification cannot be inferred from this logical freeze.
A v5 codec must provide bounded lengths, record counts, schema version, integrity checks, and an
ordered file chain. The proposed file descriptor is `{version, boot, capture, file_no, previous,
qualified, loss_baseline}`: boot/clock domain and capture IDs; ordinal starting at 0; predecessor
identity/integrity reference (`null` for 0); qualification boolean; initial 15-counter vector.
A successor repeats the descriptor and retains capture identity. Restart creates a new capture.
For a successor, loss_baseline must restate the running baseline at that file boundary (null if
unknown), never reset it or report loss; a mismatch stops at that descriptor under §6.
Within one ordered logical input, a capture ID belongs to the boot named by its first validated descriptor; reuse by another boot is a chain fault under §6. The first-boot/prefix rules in §§5–7 and REVISION-r3 Q5 apply to ordered logical fixtures. A codec delivery multiset instead follows REDELIVERY.md clause 2a, irrespective of the order in which deliveries were supplied. For an ambiguously reused capture ID, clause 2a’s whole-capture withholding overrides ordinary prefix, LIFE and certificate retention, including §§4–6 and §7’s first-descriptor target binding. For unambiguous captures, this contract continues to govern logical replay.
Qualification asserts the capture prerequisites in section 3; absence/false permits no certificates.
The descriptor and every dependency of a certificate must survive in the same validated prefix.
A certificate is published only after its dependencies are flushed; power-loss durability additionally
requires ordered successful persistence. File clean-close/rotation flags are not certificates.

Replay uses physical append order across the validated file chain. Time is not append order:
LIFE may arrive after a transition from a different ring. Before applying a FRONTIER, resolve all
LIFE/transition dependencies through its time; unresolved identity prevents its affected certification.
Per PID, order transitions by timestamp, preserving physical order at ties. The collector must preserve
actual per-backend transition order at ties; it cannot reconstruct it by sorting event names.
Deduplicate only repeated delivery of the SAME physical `(capture,file_no,block_offset,record_offset)`.
Different positions remain distinct, including identical zero-length waits. Rewriting records into a
successor is forbidden; rotation links files instead. Position is storage identity, not a BPF sequence.

## 2. Logical records and existing transitions

Each evidence record has `kind,t`; `t` is the observation instant (VOID: detection; FRONTIER: cutoff).
Per-backend records also have `pid,fork`. `pid` is positive uint32; `fork` is a kernel fork timestamp
or null for the one pre-existing provisional generation of that PID in this capture.

| Kind | Additional required fields | Meaning |
|---|---|---|
| LIFE | `pid,fork,start,start_known,exit` | Lifetime assertion/update. `start_known=true` requires `start=fork`, an observed fork. Otherwise `fork=null`, `start` is a proven lower bound, default 0. `exit` is a kernel exit timestamp or null. |
| ATTACH_OK | `pid,fork,method,seed,state,counters` | Exact tracing verified active at `t`; method is `reread` or `transition`. Counters equal the current loss baseline. `seed` is the pre-enable word, `state` the verified state at `t`. |
| FRONTIER | `counters` | Global certificate: all required evidence through and INCLUDING `t` is present, including ties and exits. Vector equals current baseline. Frequency about 1 Hz; no maximum freshness guarantee. |
| VOID | `counters,reason,invalid_from` | Global loss or unknown loss state. New vector (null if unknown); opaque nonempty diagnostic reason. `invalid_from=null` preserves prior certificates; a uint64 time withdraws prior certification from that time inclusively (§4). Detection time is NOT the loss time. |
| END | `pid,fork,reason` | Coherent last exact observation at `t`; reason is `detach`, `deescalate`, or `stop`. Ends this attachment, never the process or a wait. |

`state`/`seed` are a resolved, nonempty event name or null for wait-word zero (not waiting).
Storage VOID reasons are opaque annotations, not merge-v1's closed FAULT/VOID reason enum; replay
never branches on their spelling. An adapter preserves a merge reason or adds a storage diagnostic
(including counter-read restoration). This does not add an accepted reason to frozen merge-v1.
An unreadable, reset, wrapped, or incompletely accounted vector is encoded as `counters=null`.
The storage adapter must persist merge-v1's out-of-band epoch invalidation in VOID.invalid_from;
it must not silently serialize that report as an ordinary VOID with invalid_from=null.
Choose a conservative start no later than the earliest attachment proof of every affected epoch
and no later than the offending late DATA time; use 0 if the affected extent cannot be established.
This may revoke additional epochs. Merge DATA/FRONTIER kinds and the merge-v1 contract are unchanged.
If the invalidation cannot be persisted, the affected capture must be reported unavailable out of
band; its old file prefix alone cannot convey the newly discovered revocation.
Counter vectors have exactly 15 uint64 entries; flags are booleans. Duration is uint64 and <=t.
Require attachment a <= END e and LIFE.start <= a; no attach at/after known exit.
Zero here does not certify CPU time. Names compare exactly. `$EXIT` is reserved for transition exit.
Numeric trace event IDs must resolve using the capture's authoritative name mapping; unresolved IDs
cannot be interpreted as not waiting. No query/category marker, sample, or summary is a wait edge.
LIFE first assertion has `exit=null` or a known exit; a later assertion repeats immutable fields and
may supply the exit. Identical LIFE assertions are idempotent; contradictory ones invalidate identity.
Require `start <= t`, `start <= exit <= t` if exit is set, and observed `fork <= t`.
Discovery/INIT/attach times are never substituted for the start of a pre-existing backend.
Use 0 if no tighter lower bound is proved; a /proc discovery instant is an upper bound, not a lower bound.

For scenarios, an existing TRANSITIONS entry is written as
`{kind:"TRANS",t,pid,old,new,duration}`; duration is `duration_ns`, t is `timestamp_ns`.
`old`/`new` use names/null; `new="$EXIT"` is the existing exit sentinel. Other v4 columns are irrelevant.
A genuine transition closes old at t and enters new at t; its stored old start is `t-duration`.
Administrative same-event flushes are not real closes and MUST NOT be emitted into this v5 exact
stream; replace them with END. A redundant same-word write is not a new wait. Ordinary same-event
TRANS is invalid. Distinct transitions at the same t can produce real, separately counted zero waits.

## 3. Capture obligations: when certificates are allowed

Every silent failure capable of omitting/mislabeling evidence must increment a reliably readable
counter outside the lossy transports BEFORE a certificate can pass. Include every source, every read/map
failure, callback/queue/allocation failure, and every writer/flush/publication failure. The M0 vector
has these 15 named slots, in order (additional failures must feed a suitable slot or a global VOID):
`lifecycle_fork_drop,lifecycle_init_drop,lifecycle_exit_drop,wait_read_fail,query_text_drop,
pg13_query_text_drop,exact_query_update_fail,exact_cmd_update_fail,exact_phase_update_fail,
accum_update_fail,fork_state_update_fail,event_ring_drop,state_map_update_fail,
seen_query_ids_update_fail,writer_push_fail`.
Compare every component, not a total; unreadability, wrap, reset, or incomplete CPU accounting is loss.
Even query-text-only loss triggers the global rule. No per-backend exemption in v1.

For FRONTIER t: establish a producer boundary through t, consume every source (merge-v1's fixed
P0…Pn, E, L, including every participating CPU) through that boundary, resolve ordering/identity,
flush all dependent records, and verify every counter remained equal to its baseline across the
operation. No in-flight producer may later publish
or lose an event stamped <=t. Busy reservations, budget-limited consumption, and a temporarily empty
ring do not establish this. If the proof cannot be made, emit no frontier; no guessed grace period.
The qualification assumes non-PREEMPT_RT kernel execution and validated producer completion/memory
ordering. PREEMPT_RT and unqualified configurations certify nothing. Non-RT alone is NOT a proof of
this boundary. M0's zero counters measure frequency, not correctness of the protocol (see notes N2).

ATTACH_OK requires successful state setup AND watchpoint enable AND generation validation.
`reread`: a coherent post-enable reread equals seed; reconcile all intervening transitions so that
`state=seed` really is current at t. Equality does not prove absence of A→B→A or a wait's entry time.
`transition`: in the generation's timestamp-ordered history, the first TRANS at anchor t has
new=state and is the first VERIFIED genuine post-enable state transition. Preserve TRANS append
order at timestamp ties. This anchor TRANS may physically precede or follow ATTACH_OK; both must
precede the FRONTIER that certifies the anchor. It supplies the new state's entry instant.
The anchor's old side is untrusted (seed race); discard it. A merely emitted stale-seed repair or
same-value write is insufficient to establish a genuine entry. No coverage before the anchor.
Failed attach, failed reread, or seed mismatch writes no ATTACH_OK. Subsequent successful verified
transition may supply one; until then the backend has no active exact run. No inferred success.
END requires a coherent observation while tracing still supplies state, with producers quiesced and
drained consistently at that backend's t. Sequential disabling does not justify a later common t.
Assign transitions to runs by generation and timestamp, not position relative to ATTACH_OK or END.
All transitions through END.t, including ties, must precede the certifying FRONTIER; they may be
appended after END. Resolve the entire run through END.t before applying END. A reread anchor
already reconciles transitions through its t; a transition anchor includes its anchor and later ties.
Transitions after END.t require a new attachment; ambiguous assignment at touching runs with tied
transitions withholds affected certification. Merge-v1 need not rank or carry ATTACH_OK, END, or LIFE:
the storage adapter joins these records with merged DATA before publishing a storage FRONTIER.
END is pending until a FRONTIER reaches t; it cannot independently advance certification.
Daemon stop should write its END records and a final FRONTIER. An unverified shutdown writes neither
an optimistic END nor a synthetic close; the preceding frontier remains the only bound.

## 4. Generation assignment and replay state machine

Replay state: last still-valid global frontier F (initially absent), retained frontier history,
highest ever published frontier H (initially absent), current counter baseline, and per generation
LIFE plus attachment runs. A run stores anchor a, optional END e, transition history, and verified
state/entry (entry may be unknown). Runs are evidence segments of ONE process, not new generations.
A key is `(boot,capture,pid,fork)`; an observed fork starts a different generation after PID reuse.
An exit update addresses that key and never retires whichever PID happens to be current now.
For TRANS, resolve pid/t against known fork/exit history, not callback arrival or mutable PID lookup.
An exit sentinel at x belongs only to the generation with LIFE.exit=x. Other edges must lie before
that generation's exit and at/after its fork (or discovery for a provisional generation).
If timestamps or lifecycle gaps permit multiple assignments, withhold affected certification; never guess.
With no lost lifecycle evidence, fork/exit ordering separates delayed records across PID reuse.
For replay, lifecycle loss means a change in any of these zero-based counter slots:
0 lifecycle_fork_drop, 1 lifecycle_init_drop, 2 lifecycle_exit_drop, 10 fork_state_update_fail,
12 state_map_update_fail, or 14 writer_push_fail (shared failure slots are conservatively included).
Compare VOID.counters with the preceding known baseline before replacing it. A null vector, an
unknown preceding baseline, or nonnull invalid_from also means lifecycle uncertainty. Failures that
could lose identity but cannot be represented by those slots must use a null vector. Other slot
changes alone, including event_ring_drop and query_text_drop, do not taint lifecycle identity.
Lifecycle uncertainty blocks new runs for every existing generation; ATTACH_OK and a restored
counter baseline cannot repair it. Require a new LIFE with start_known=true and a fresh observed
fork >= the latest lifecycle-uncertainty VOID.t, appended after that VOID with a readable baseline,
then a fresh ATTACH_OK for that generation. A delayed old fork or a provisional key is insufficient.
The fresh fork assigns subsequent transitions to the new generation; a tainted old generation does
not compete for those timestamps. This establishes no exit time for the old generation.
Retain previously verified LIFE facts and surviving certified prefixes; do not accept new lifetime
updates for a tainted old generation. Do not extend its runs. No separate continuity proof exists.
After §5 schema/counter handling and the recovery-time check below, an ATTACH_OK blocked by
lifecycle uncertainty is ignored without creating a run or stopping the chain.
A restart's provisional key is a capture-local handle, NOT proof of a different actual process; do not
join or sum it with the prior capture's same PID. This version offers no cross-restart identity claim.

LIFE establishes/refines lifetime independently of coverage. An exit without a certified terminal
transition is still a known exit; it does not create an exit coverage certificate or close a wait.
ATTACH_OK creates a new tentative run, replacing no earlier run. Overlapping attachments as defined
in §5 rule 2 are invalid and stop that capture's chain; touching runs retain §3's tied-transition rule.
For reread, state=null gives a known nonwaiting state; state=event has UNKNOWN entry time.
For a transition anchor, set state=new and entry=t (null if new=null), discarding the anchor's old side.
Process later genuine transitions in run order: require old=current state; if entry is known, require
`t-duration=entry` and append `[old,entry,t]` when old is a wait. If entry is unknown, discard that
old interval, record an unrepresentable close at t, then set new state/entry=t. For null old, no wait
is appended and its duration is not used to invent a wait. A mismatch stops the chain under §5.
EXIT requires matching LIFE.exit, closes a known current wait normally, then leaves no open state.
Unknown-entry EXIT is an unrepresentable close; the exit is not certified for the core.

FRONTIER validates its dependencies and, if H exists, must exceed H; set H=F=t on acceptance.
H survives VOID and rollback, matching merge-v1's publication rule across REARM. For each eligible
run not frozen by VOID (including a run with pending END)
anchored no later than F, its candidate endpoint b is min(F, END if present, LIFE.exit if known).
Only transitions through b enter that candidate; later records cannot extend its open observation.
At a non-exit b, the terminal open state is the last transition's new event (or reread state), at b.
No per-backend frontier record is needed. A frontier before any attach certifies no backend coverage.
END fixes e and stops the run; it never changes state, entry, or lifetime. A new attach creates a new run.
VOID with invalid_from=null freezes ALL active runs at the last F, even if the suspected loss
concerned a different PID. Drop their uncertified suffix and tentative ENDs after F;
unanchored/uncommitted runs contribute nothing. Already frozen runs never extend with a later F.
For nonnull invalid_from=r, first revoke every prior certificate at or after r, including those in
earlier loss/rearm epochs; roll F back to the latest surviving published frontier strictly below r,
or absent if none. Do not invent a frontier at r-1. Recompute every existing run (including frozen
or ended runs) using only surviving certificates, capped at its former endpoint; discard runs with
no surviving certificate at/after their anchor. Remove revoked waits, open observations and ENDs.
Then freeze active runs as above. Revocation is permanent for those runs; a future FRONTIER cannot
restore them. Queries and cached projections must incorporate all later VOID records in the read
capture, even when VOID.t lies outside the query. Retract results dependent on revoked certificates.
Never use VOID.t as b. Replace the baseline with VOID.counters, including null for unknown;
no recovery while it remains unknown.
A later readable VOID can establish a baseline. Recovery attach.t must be >= the latest VOID.t.
After §5 schema/counter handling, a recovery ATTACH_OK with t below the latest VOID.t stops at that
ATTACH_OK under §5's prefix rule, before checking lifecycle eligibility.
Only a fresh ATTACH_OK with identity eligible under the lifecycle rule starts each recovered run;
FRONTIER alone cannot revive old attachments. Only certificates surviving revocation remain usable.

## 5. Projection into frozen core evidence (apply separately to each query)

The core object has exactly `life:[start,exit],coverage:[[a,b],...],waits:[[event,s,e],...],open:null|[event,s,b]`.
Keep the full LIFE, including its lower bound, for every projection. Never shrink life to an attachment.
First form the maximal core-representable portion of EACH certified run independently:
1. Reread of a nonzero seed has no known wait start: omit its occupied prefix and its eventual close.
   If it never leaves that wait, this run has no core portion. When it leaves at u, begin at u+1.
2. A transition anchor at a discards an untrusted old side: if old is nonnull, begin at a+1, otherwise a.
   This 1 ns exclusion is necessary because coverage certifies closes AT its left edge, including
   unknown-start/zero-length closes. It trims the projection, not the verified tracking start.
   Never saturate u+1 at UINT64_MAX: no portion remains if addition overflows or its start exceeds b.
3. Otherwise begin at a. Preserve all known-entry waits closed through b in that run, even if their
   starts precede the trimmed left edge. Do not clip a real wait or fabricate a close at a, b, or F.
4. At non-exit b, emit open=[state,entry,b] only for a named state with known entry; otherwise null.
   At exit b require a certified EXIT with known entry (or nonwaiting old); open=null. If absent,
   use the last earlier certified frontier strictly before exit instead, or discard this portion.
   Thus no open has as_of==life_end and no span ending at exit claims a missing terminal close.
5. Emit coverage=[[trimmed_a,b]]; empty spans are allowed only when their point's closes are certified.
   Keep distinct runs separate even if touching. Never merge across END, VOID, restart, or a file gap.

Select ONE candidate for the query `{event,from,to}` using these ordered rules: prefer a candidate
that covers R=[from,to) intersect life AND certifies exit if exit lies in the query; otherwise prefer
largest length of its intersection with R; ties choose smallest a, then b, then attachment append order.
For malformed queries use the earliest candidate and let the core validate the query normally.
Pass only that candidate's coverage, closed waits, and terminal open to the core. Discard other runs
from this projection, not from storage; diagnostics refer to the projected evidence only.
With valid LIFE but no candidate, pass coverage=[], waits=[], open=null. Missing/ambiguous LIFE
sets the adapter's LIFE_INVALID flag (JSON reference equivalent: life=null), never fabricated [0,null].
Contradictory LIFE invalidates that generation. Invalid certificates never certify their dependencies.

A chain fault stops only the affected capture, using §6's prefix rule, without synthesizing a VOID.
The stop boundary is immediately before the offending record (or descriptor), not its timestamp
or a later FRONTIER at which validation detects it. Retain only still-valid certificates before
that boundary, with F the latest of them or absent; prior VOID revocations remain effective.
Discard uncertified run suffixes and tentative ENDs; reconstruct each retained run from its surviving
certificates. Retain independently verified LIFE before the boundary, even if F is absent.
Ignore the offending record and EVERY later record/file with that capture ID, including LIFE,
VOID, reattachments, and otherwise valid FRONTIERs. No new baseline, uncertainty instant, exit,
or certificate is inferred. A different capture ID remains readable independently.
Resolve dependencies by §§1,3 before declaring a semantic fault; physical delay alone is not one.
For pending END/run checks, use the next FRONTIER's dependency set, or the remaining prefix at EOF;
if a fault is found, cut at the offending record and reject that FRONTIER, even if detection is later.

The following rules resolve the six questions in reference/REPORT.md ("Ambiguous contract sentences found"); each chain stop retains the certificates just defined.
Explicit lifecycle-taint, contradictory-LIFE, and ambiguous-assignment rules otherwise remain in force.

1. A malformed logical record (including TRANS with required kind,t,pid,old,new,duration as in §2:
   missing required field, wrong type/range, invalid field value or combination), an unparseable
   record, or an unsupported kind stops at that record. No readable t,
   counters, or per-backend identity is needed to cut a chain already identified by its descriptor.
   Untrusted framing stops at the corrupt block/file boundary under §6; do not scan for a new record.
2. An ATTACH_OK that overlaps a run of the same generation (one not ended by END or frozen by VOID
   after dependency resolution), starts before LIFE.start, or starts at/after known LIFE.exit stops
   at that ATTACH_OK. An END with e<a stops at that END.
3. A schema-valid END addressed to a lifecycle-tainted generation is ignored without changing
   retained evidence or stopping the chain; otherwise an END with zero or multiple addressable
   runs after dependency resolution stops at that END.
   Address by its pid/fork and t against eligible runs not ended by another END or frozen by VOID,
   with a<=t and t<=known exit. The separate touching-run/tied-TRANS ambiguity still only withholds
   affected certification (§3); it is not itself a chain fault.
4. A FRONTIER with t<=H stops at that FRONTIER, including after VOID rollback. It does not update
   F or H. H is the highest ever accepted publication, while only still-valid certificates survive.
5. A descriptor reusing a capture ID with a different boot stops at that descriptor (§6).
   The first boot's surviving prefix remains addressable; no record in the conflicting file or
   any later file with that capture ID is read, even if the original boot reappears.
6. After dependency resolution, a schema-valid TRANS outside every eligible run's timestamp extent
   is ignored without a chain fault; frozen runs end at their retained endpoint, and delayed END
   dependencies and transition anchors still resolve under §3.
   A TRANS failing old=current state or, for a known wait entry, t-duration=entry stops at that
   TRANS after timestamp/run resolution. Discard the transition anchor's old side; do not check
   duration against an unknown entry or use null-old duration to invent a wait (§§3,4).
   No attachment or frontier after the fault can repair it.

A readable counter difference is a loss report, not by itself a chain fault. After schema validation
and the FRONTIER t>H check, a FRONTIER or ATTACH_OK whose complete 15-entry uint64 vector differs
from the baseline (or whose preceding baseline is unknown) is consumed ONLY as a §4 VOID with
t equal to that record's t, counters equal to its vector, invalid_from=null, and an opaque diagnostic
reason. It neither publishes a frontier nor creates a run; do this before ATTACH_OK run checks.
If any component decreased from the known baseline, use counters=null under §2's reset/wrap rule;
the decreased vector cannot establish a baseline. An explicitly null counter field likewise reports
unknown loss state; a missing or wrongly shaped/typed field is malformed and stops the chain.
Freeze at the preceding still-valid F, compare against the old baseline for lifecycle loss, and
continue with §4's recovery requirements. Lifecycle uncertainty uses the offending record's t,
never F; a later readable VOID or counter-difference report can restore an unknown baseline.
No fields are invented for an unreadable record. Ordinary explicit VOID semantics are unchanged.

Without qualification ignore ATTACH_OK/FRONTIER/END certificates; verified LIFE is retained.
Here verified means an intact v5 LIFE in the validated file chain satisfying §2's field, timestamp,
immutability and generation rules, with unambiguous identity under §4's lifecycle-loss rule.
This check is independent of exact-capture qualification; PREEMPT_RT alone does not invalidate LIFE.
Such a target projects its retained life with coverage=[], waits=[], open=null; failed LIFE validation
uses LIFE_INVALID. Certificate-shaped records in an unqualified capture cannot alter this choice.

Qualification does not exempt descriptor continuity checks. Every capture initializes its running baseline from file 0, updates it through applicable explicit VOID records, and checks each reachable successor descriptor against it. In an unqualified capture, ignored ATTACH_OK, FRONTIER and END records neither update the baseline nor become counter-loss VOID records. Explicit VOID retains its ordinary baseline and lifecycle effects. A successor-baseline mismatch stops the capture even when qualification is false; ignoring certificates never creates coverage.

The unchanged core computes both metrics; two escalation windows cannot be combined into a falsely
complete answer. No event-specific projection optimization: unknown waits impair every event query.

## 6. Crash, file boundaries, restart, and legacy inputs

EOF/crash without END preserves only the latest validated FRONTIER and its reconstructed terminal
open wait. Later transitions, latest record time, wall-clock file time, and recovered/clean flags do
not advance b. A partial evidence record/block and every dependent certificate are discarded.
Rotation with a complete validated same-capture chain changes no lifecycle, wait, or run. It needs no
END/ATTACH and may split an open wait. Reading a suffix without its predecessor gives no continuity.
Missing/corrupt/unsupported file or block stops chain replay at the last valid prefix certificate;
this v1 does not salvage later files of that capture. Independent later captures remain readable.
The same prefix rule applies to §5 record faults and a descriptor whose boot conflicts with the
boot already bound to its capture ID. A boot conflict stops before that descriptor's records;
keep the earlier boot's surviving certificates and ignore all later uses of that capture ID.
With no surviving FRONTIER there is no exact coverage; independently verified LIFE is still usable.
Chain termination never rejects unrelated captures and never resumes within the stopped capture.
Restart always requires LIFE and fresh ATTACH_OK under a new capture, even within the same boot;
no stitching over downtime, no inherited frontiers, no exit inferred for survivors. A boot change
creates a different clock domain; windows cannot cross it. Loss history resets only with that new capture.

v2/v3/v4 traces have no v1 evidence records. They retain their legacy decoding/sample semantics, but
for this exact adapter have unknown LIFE, coverage=[], waits=[], open=null, and LIFE_INVALID.
Do not infer life from first/last transition, PID metadata, escalation markers, synthetic flushes,
legacy checkpoints, or closed-clean flags. They yield unavailable, never a measured zero from absence.
Old readers must reject v5; callers must surface the unavailable file instead of bridging its gap.

## 7. Scenario interchange

Each JSON file contains `id,description,files,target,query`; all are INPUTS, with no expected fields.
`files` are ordered descriptors plus `records`; `previous` names the preceding file_no for these
logical fixtures (a real codec also validates integrity). `present:false` denotes a missing file.
`qualified` asserts section 3 for a scripted input, not an actual measured platform property.
Each valid record has t. `target` is `{capture,pid,fork}`; capture selects the first validated
descriptor's boot (§1), never a later conflicting boot. The query is the unchanged core query and requests
both metrics. TRANS abbreviations above are existing block records, not extra EVIDENCE kinds.
Fixture JSON framing is intact; `present:false` denotes an unavailable file. Logical records may violate
the schema or semantic rules to exercise §5 faults. EOF is literal end of the records array.
