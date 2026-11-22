**A. Producer header delta**

This specification targets producer snapshot `6e6034c7` and merge snapshot `44612ec2`. It defines required behavior, not completed implementation or qualification. Preserve contracts, independent answers, existing guards, transport bytes and BPF programs. Persistence changes require their separately authorised unit. A2 verdict/display, server/UI, priority and fork-born never-initialised membership remain outside integration scope.

Rename only `struct pgwt_discovery_fact` to `struct pgwt_producer_fact`, and producer enumerators `PGWT_DISC_RUNNING`, `PGWT_DISC_DENIED`, `PGWT_DISC_MALFORMED` to `PGWT_PRODUCER_RUNNING`, `PGWT_PRODUCER_DENIED`, `PGWT_PRODUCER_MALFORMED`. Preserve their numeric values. Preserve both `struct pgwt_discovery_attempt` and the merge typedef `pgwt_discovery_attempt`; explicitly translate inactivity values.

Notation: `F` means `struct pgwt_producer_fact { bool known, value; }`, a dimensionless fact with no clock domain; unknown is `{false,false}`. Every existing producer fact member uses the renamed type. Unless specified otherwise, timestamps are `uint64_t`, measured in CLOCK_MONOTONIC nanoseconds, with zero unavailable. IDs, tokens, generations, ordinals, counts, flags and enumerators have no clock domain. Zero is a valid count or accumulator only when its validity evidence holds.

Add `enum pgwt_discovery_handoff_kind` with `PGWT_DH_INVALID=0`, `PGWT_DH_CENSUS=1`, `PGWT_DH_INVENTORY=2`, `PGWT_DH_INSTALLED=3`, `PGWT_DH_NUMERIC=4`, `PGWT_DH_RESOLUTION=5`, `PGWT_DH_ATTEMPT=6`, `PGWT_DH_NO_WORD=7`, `PGWT_DH_POINTER=8`, `PGWT_DH_TERMINAL=9`, `PGWT_DH_END=10`, `PGWT_DH_FAULT=11`.

Add `enum pgwt_discovery_reconciliation` with `PGWT_DREC_PENDING=0`, `PGWT_DREC_COMPLETE=1`, `PGWT_DREC_PERMANENT_FAILURE=2`. PENDING includes unknown or incomplete reconciliation; COMPLETE qualifies only its named completed cut; PERMANENT_FAILURE means reconciliation cannot be restored during this capture.

Add these members to `struct pgwt_discovery_attempt`:

| Member and C type | Unit, validity, unknown and consumer |
|---|---|
| `enum pgwt_discovery_handoff_kind kind` | Required record discriminator; zero invalid. Consumed by dispatch and payload validation. |
| `uint64_t binding_token` | Capture-unique, nonzero retained-binding identifier; never an FD, PID, address or recycled slot. Zero means absent, including journal-only inventory. Consumed by witness handles, resolution and binding lookup. |
| `uint64_t failed_open, gone_at, identity_visible_at` | Monotonic ns: failed-open completion, actual liveness-probe completion, immutable installation visibility. Zero unknown. Consumed by NUMERIC/G ordering and installation proof respectively. |
| `uint32_t tid, tgid, parent, postmaster` | Actual task identifiers in the qualified namespace; zero unknown. Consumed by resolution classification. Existing `pid` remains the enumerated/control PID. |
| `uint64_t fork_cookie` | Validated current-lifetime fork provenance; zero proves absence only with complete reconciled history. Consumed by resolution; a current PID-map lookup is insufficient. |
| `F start_mono_known, namespaces, retained_binding` | Qualify exact monotonic `lifetime_start_ns`, common PID namespaces and retained binding continuity. Consumed by resolution, start identity and PINNED_BINDING. Tick bounds never establish exact start. |
| `F history_complete, producers_complete, journal_complete, journal_eligible, gone_esrch` | Scoped through the record’s resolving fence. Consumed by J/R/G; R/G require positively known journal ineligibility, and G requires the actual post-open ESRCH result. |
| `uint64_t reconciliation_ns, reconciliation_count` | Completed-cut monotonic ns and capture-monotone ordinal. Both initially zero. Preserve the last successful pair after failure; attempted or partial reconciliation never advances them. Consumed by census, resolution and scheduling. |
| `enum pgwt_discovery_reconciliation reconciliation` | State of reconciliation, separate from the retained successful pair. Consumed by liveness and acceptance; PERMANENT_FAILURE supplies no new fence. |
| `uint64_t accumulator, on_cpu_ts; bool seed_valid` | Actual installed seed: CPU-consumption ns and monotonic ns respectively. With `seed_valid=true`, accumulator zero is valid and `on_cpu_ts=0` means off-CPU. False means unavailable. Consumed by quiet/FRESH_COHERENT_SEED proof; preserve separate `cpu_base_valid`. |
| `F install_before_enable, failure_accounting, voluntary_no_wrap` | Actual installation ordering, complete readable accounting through the completion cut, and qualified absence of voluntary-count reset/wrap. Consumed by corresponding masks; endpoint equality alone proves none. |
| `bool attempts_ended` | True is irreversible startup-attempt finality; false does not establish finality. Carried by END and final ATTEMPT/NO_WORD, and retained in binding state. Consumed by final disposition and retry suppression. |
| `bool claims_completed_coherence, claims_completed_no_word` | Explicit proof claims; false asserts no completed proof. Consumed by confirmation validation, independently of whether evidence passes. |
| `bool pointer_snapshot_valid` | Qualifies pointer counts from a completed observation. False means unknown, not zero fires. Consumed by POINTER and no-word proof. |
| `uint64_t kernel_evidence_count, user_evidence_count; bool evidence_counts_known` | Capture-monotone cumulative evidence-fault counts, separate from the 15 loss counters. Zero valid when known; false on unreadability/wrap. Consumed by fault accounting and certification. |
| `bool handoff_complete` | Starts true; permanently false after a lost append. Consumed by certification and fault recovery. Retain it independently of append storage. |

Do not add `listed_at`, `holder`, `bound_at`, a qualification identity, a manifest hash or an acknowledgement array to each record. Preserve the unused `listed_at` argument of the existing merge API by passing zero.

Keep immutable pre-enable acknowledgements in retained per-generation storage. Define these accessor-only structures:

| Structure/member and C type | Meaning and consumer |
|---|---|
| `struct pgwt_discovery_pre_enable`: `uint64_t attachment_generation` | Nonzero generation identifier; zero invalid. Selects the exact generation for transport membership. |
| `uint32_t cpu_count, cpu_ids[MAX_CPUS]` | Complete enrolled CPU set; count bounded by MAX_CPUS, CPU ID zero valid. No qualified set when `pre_enable_valid=false`. Consumed by membership lookup. |
| `struct pgwt_discovery_ack pre_enable[MAX_CPUS]` | Existing acknowledgement type: `u64 request, stamp, attempts[3]`. Request/stamp are monotonic ns; attempts are per-destination sequence cuts, with zero valid. Indexed by `cpu_ids`, not raw CPU number. |
| `uint64_t pre_enable_completed_ns; bool pre_enable_valid` | Monotonic completion time; zero/false unavailable. True only after every enrolled CPU acknowledges the same request and the exclusion premise holds, after active-generation publication and before enable. |
| `struct pgwt_discovery_binding_snapshot`: `struct pgwt_discovery_attempt inventory` | Retained INVENTORY snapshot, including installation, finality and sticky pointer facts; unavailable when lookup returns false. Consumed by coordinator recovery and proof translation. |
| `const struct pgwt_discovery_pre_enable *generations; size_t generation_count` | Borrowed immutable generation entries and element count; NULL/zero means none. Storage remains valid until discovery close. Lookup is by generation, never “latest acknowledgement.” |

Reserve stable generation storage before enabling. Publish each entry once; never replace its B vector with later acknowledgements. Retain older generations through capture teardown. Allocation failure cannot become an enabled generation with an invented B vector.

Changed existing member semantics:

| Existing member and C type | Required semantics and consumer |
|---|---|
| `uint64_t cookie` | Reserve/burn at bound discovery inventory creation and journal-only identity creation, before installation. Numeric-only observations retain zero. Discovery cookies remain in their partition; exhaustion retains an unregistered obligation and reports allocation failure. Consumed by all lifetime controls. |
| `uint64_t lifetime_start_ns` | Exact monotonic start only when `start_mono_known` qualifies it; otherwise unavailable to exact-start consumers. Never fill from a tick upper bound. |
| `uint64_t start_ticks, start_upper_ns, boot_lower_ns` | Start ticks and conservative boot-clock ns bounds, qualified by clock/binding facts. Use the retained bound corresponding to F0 for pre-existing R classification; never compare unlike clock domains directly. |
| `F address_compared` | Reset and recompute on every fresh fallback. A failed quiet bracket’s comparison cannot qualify fresh attachment. |
| `struct pgwt_counter_vector counters_before, counters_after; F counters_unchanged` | Qualified numerical and route-c accounting vectors, including numerical `counters_after`. Unknown components remain unknown. Consumed by masks, resolution and census evidence. |
| `uint64_t pointer_fires_before, pointer_fires_after, pointer_nonnull` | Occurrence counts, qualified by `pointer_snapshot_valid` and nonwrapping accounting. Baseline is immediately before original pointer enable; after-count covers the stated fence. Non-NULL count is not an address. |
| `uint64_t pointer_enabled_ns, pointer_read_ns, pointer_fence_ns` | Qualified monotonic operation times; require strictly `pointer_read_ns > pointer_enabled_ns`. Equality cannot pass. |
| `F pointer_continuous` | Required continuous-observation proof; retain demonstrated breaks even after history failure. Unknown is not a demonstrated break. |
| `bool census_record, numeric_record, evidence_fault, no_wait_word` | Compatibility selectors/payload qualifiers only; `kind` controls dispatch. FAULT may carry scoped no-word/pointer payload without becoming a second record kind. |
| `int error` | Zero means no reported diagnostic error; positive errno describes failure. It never proves success. |

Emit INVENTORY even without a backend slot or successful attachment. Emit INSTALLED separately only after successful immutable installation. Report a retained live pre-existing holder with its conservative pre-F0 proof so the coordinator can issue ordinary INVENTORY. Reject contradictory tags; zero payload unrelated to the record’s kind. A failed-open number subsequently bound to a qualified FORK or NONMEMBER holder produces its NUMERIC record followed by exactly one RESOLUTION R, with a retained nonzero binding token, all four qualified nonzero task IDs, the validated fork cookie when applicable, and discovery cookie zero. Reserve no discovery cookie and emit no INVENTORY for that holder; retain its binding through the resolving fence and handoff. Preserve independently established earlier journal lifetimes.

Classification must precede ordinary inventory publication, including the scan-time fork-provenance path. Every schedule must preserve `!(excluded && inventory_reported && !kernel_only)`, and census `inventory_count` must equal the live INVENTORY obligations handed over. Never repair disagreement by hiding an already published obligation; a later contradictory classification must retain that obligation and report the uncertainty.

Add `struct pgwt_discovery_checkpoint` with exactly these members:

| Members and C types | Meaning and consumer |
|---|---|
| `uint64_t reconciliation_ns, reconciliation_count` | Last successful monotonic cut and ordinal; zero means none. Scheduling and acceptance. |
| `uint64_t kernel_evidence_count, user_evidence_count` | Current cumulative evidence-fault counts; validity below. Fault high-water marks. |
| `enum pgwt_discovery_reconciliation reconciliation` | Current reconciliation state. Liveness. |
| `bool evidence_counts_known, handoff_complete, attempts_stopped` | Count validity, sticky handoff integrity and verified irreversible attempt gate. False never certifies the corresponding property. |
| `int error` | Zero or positive errno; diagnostic only. |

Functions:

| Exact signature | Required behavior and consumer |
|---|---|
| `struct pgwt_discovery_checkpoint pgwt_discovery_refresh_until(struct pgwt_daemon *, uint64_t deadline_ns);` | Absolute monotonic deadline; zero permits no blocking work. Check before each entry and fence wait, and bound waits by remaining time. Refresh scoped faults even when history is unusable. Deadline exhaustion gives PENDING unless independent permanent-failure evidence exists. Coordinator refresh. |
| `void pgwt_discovery_checkpoint(struct pgwt_daemon *, struct pgwt_discovery_checkpoint *);` | Allocation-free, nonblocking current snapshot. Does not manufacture a completed fence. Coordinator acceptance/fault gate. |
| `bool pgwt_discovery_binding(struct pgwt_daemon *, uint64_t binding_token, struct pgwt_discovery_binding_snapshot *);` | Allocation-free retained lookup, including all retained B entries. Zero/unknown token returns false and zeroes output. Snapshots survive append failure. Coordinator recovery and transport membership. |
| `void pgwt_discovery_stop_attempts(struct pgwt_daemon *);` | Immediately and irreversibly forbid new attempts while retaining required watches and fault inspection. Coordinator liveness/shutdown. |
| Existing `void pgwt_discovery_refresh(struct pgwt_daemon *);` | Bounded compatibility wrapper over refresh-until. |
| Existing `bool pgwt_discovery_next(struct pgwt_daemon *, size_t *, struct pgwt_discovery_attempt *);` | Reserve cursor zero for mutable CENSUS from discovery start, including unknown census. Append consumption starts at one and never changes interpretation when readiness changes. |

All other existing function signatures remain unchanged. Record totals and reconciliation cuts qualify that record’s historical census evidence. The fresh checkpoint gates present acceptance and exposes intervening invalidations; newer totals cannot upgrade an older census cut.

Re-issue CENSUS in place at cursor zero. Its establishing cut is the completed reconciliation pair current when the whole census predicate first evaluates true, including resolution of every numeric obligation and complete handoff of the required evidence. Capture the supporting record-local accounting with that proof. Freeze the establishing pair and its proof evidence while the census remains proved; unrelated successful refreshes do not move it forward.

Invalidation clears `census_fenced` and retains the last successful pair without claiming current success. A transient fence failure leaves the census pending. Re-proof requires a strictly larger completed reconciliation ordinal; an ordinal once returned unproved must never later be returned proved. Do not manufacture an ordinal without completing reconciliation. In particular, a number resolved at a later cut cannot establish census completion at the earlier scan cut. Current checkpoint totals must not overwrite historical census totals.

**A.1. Producer rounds**

**P1 — complete header and physical records.** Land the complete declarations above and collision renames; implement authoritative tags, stable binding tokens, cookie reservation/burning, full inventory and installation reporting, numeric observations and resolution facts, immutable per-generation B storage, actual seed accumulator/on-CPU time, attempts-ended records, fresh-route address comparison, strict read-after-enable, record-local accounting and stable cursor semantics. Existing start/scan/attach/brackets/next paths populate these facts. Unknown or unsupported evidence remains explicitly unknown.

P1 includes linkable transitional definitions for the P2 APIs so I1 can compile. `refresh_until` performs no unbounded work and returns a conservative checkpoint: PENDING, zero completed cut/ordinal, unknown evidence counts, `handoff_complete=false`, `error=ENOSYS`; `attempts_stopped` reports only the actual gate. `checkpoint` returns the same conservative snapshot. `binding` returns false with zeroed output. These stubs cannot authorise production certification.

The P1 `stop_attempts` transitional definition must enforce the minimal irreversible entry gate across scan/attach/retry paths; it cannot be a no-op. It leaves watches active. Full finality reporting, retained inspection and shutdown behavior belong to P2. The P1 correction classifies fork-born and uninventoried backends before applying the stop gate: those routes retain `-ENOENT`; only a matching discovery attempt is refused by the gate. A pointer enable that never succeeded reports `pointer_continuous={false,false}`, preserves its positive errno and existing failure accounting, and does not by itself end attempts. A demonstrated break of an established attachment remains sticky.

P1 also closes the terminal pointer-observation tail before discarding its retained event. After the task/event is quiescent, read the final perf count before closing the FD. A readable, qualified nonwrapping count equal to the last validated snapshot’s absolute fires closes only that tail and clears no earlier fault. A greater or unreadable count, missing validated snapshot, or invalid count ordering leaves continuity unknown and latches one user-evidence increment plus a lifetime-scoped FAULT; it must not manufacture a demonstrated break. Repeated inspection of the same unresolved tail does not recount it. Qualify the final-count read after exit with one EL8 probe; if unavailable, state that limit and retain the unknown outcome.

P1 accounts for the first transition into a sticky producer fault once, without counting the same failure again merely because it also set the latch. Readable cumulative evidence totals remain known; the latch alone does not make them unreadable. The conservative P2 API stubs remain unchanged. P2 owns moving mandatory pointer inspection/comparison out from behind successful history reconciliation, bounded refresh, allocation-independent retained fault delivery, and complete shutdown ordering before merge finalisation and storage destruction. I1 tests use injected completed interfaces; production coordinator wiring waits for P2.

P1 checks include both headers in one translation unit; exercise every new record fact’s valid/unknown/contradictory states, reservation before failure, journal-only/no-slot inventory, token reuse prevention, immutable B, zero seed, fresh comparison and strict timestamp inequality. Mutants must build, reach their case and fail its intended assertion. Stub tests prove they cannot certify census, membership or shutdown completion.

**P2 — bounded lifecycle and recovery interfaces.** Replace all transitional definitions with refresh-until, current checkpoint, binding accessor and complete stop-attempts behavior; implement the permanent-reconciliation-failure latch, allocation-independent evidence totals/handoff state, and scoped fault delivery despite history or append failure. Preserve successful reconciliation pairs without treating them as current success. Stop attempts without stopping mandatory watches.

P2 checks cover transient versus permanent failure, deadline exhaustion across many entries, retained bindings/B after append loss, immutable old cuts, fault bursts/idempotent rereads, scoped contradictions after history failure, stopped retries and final helper teardown. Each failure must remain observable without successful append. P1 and P2 each compile and pass their own registered host tests and applicable mutants; no eBPF execution on macOS.

**B. Translation**

Every merge control is zero-initialised. Use current detection time `d` and a fresh complete 15-component snapshot `C`, including checked post-publication updates. Historical vectors qualify witnesses; they never replace current `C`. Copy identities from retained lifetime evidence, never the current PID map. Set `current_epoch_continuity=false` and include no discovery lifetime in REARM attachment proofs until the unit enabling production REARM supplies an independently qualified per-binding witness covering the original startup attachment through REARM and the current observation.

Three-valued conjunction: any known false operand yields known false; otherwise any unknown yields unknown; otherwise true. Set mask `known` only for known results and `truth` only for true. Optional failed candidates remain pending/rejected; they do not become false completed-proof claims.

[D3] **Variant (a):** qualified PostgreSQL 13 logger, statistics collector and archiver retain route-c bounds only through checked support publication, with per-process “present, not measured” support.
[D3] **Variant (b):** those three roles retain the same revocable route-c bound `b_i=e` and closure timing, with no role-support publication precondition and no per-process presentation; they are neither listed nor counted.

[D3] Variant (b) requires explicit owner acceptance naming these three roles and separately amended contracts/independent answers before implementation. It does not translate route c into the time-free NONMEMBER class, weaken proof/monitoring, eliminate checked gap publication or establish a cross-version population rule.

| Producer kind | Translation and ordering |
|---|---|
| CENSUS | `pgwt_capture_discovery_census(reconciliation_ns, proved, d, C)`. Require enumeration, lifecycle activation, completed reconciliation, unchanged qualified lifecycle accounting, complete handoff, registration of all obligations and no unresolved numbers. Negative updates first; eligible positive census last. Never use enumeration-start `census_cut_ns` as completion. |
| INVENTORY | `inventory(pid,cookie)` before dependent controls. Call `start_identity` only for qualified exact monotonic start. Retain binding/clock evidence independently of installation. For a previously unresolved number, permit ordinary INVENTORY only after the qualified pre-existing-R or eligible-J prerequisites below. |
| INSTALLED | `installed(pid,cookie,proof)` after INVENTORY, requiring IDENTITY_VISIBLE and EMISSION_IDENTITY. Implies no enable, admission or bound. Installation failure leaves inventory pending. |
| NUMERIC | `numeric(pid,0,failed_open)` with cookie zero; the second argument is explicitly unused. Deduplicate unresolved numbers without collapsing independently bound lifetimes. Deliver before corresponding J/R/G. |
| RESOLUTION: J | JOURNAL only with eligible exact retained terminal witness, complete reconciliation and the unique transport join. J takes precedence over R/G. Preserve earlier journal lifetimes across PID reuse. |
| RESOLUTION: pre-existing R | Ordinary INVENTORY, then separate INSTALLED if proved. Require conservative pre-F0 existence, pinned continuity, complete history/producers/journal, reliable accounting, qualified namespaces and exhausted J precedence. Never supply tick bounds to `start_identity`. |
| RESOLUTION: other R | REBIND only for FORK or NONMEMBER, with all four IDs known/nonzero, retained binding and nonzero handle, complete reconciliation/accounting, and positively established journal ineligibility. Unknown IDs or a post-F0 target lacking fork provenance remain pending. |
| RESOLUTION: G | GONE only after actual ESRCH strictly after failed-open completion and a subsequent completed reliable fence with no eligible J. Supplies no lifetime cookie or terminal discharge. |
| ATTEMPT | One WAIT_ATTEMPT per physical `(cookie,attempt)` word attempt. For watched NULL candidates, qualified non-NULL POINTER precedes it. At final attachment with attempts ended, send one numerical ADMIT with generation/e/mask; CONFIRM only a complete quiet witness. Otherwise retain for END. |
| NO_WORD [D3] | TRY_NO_WAIT_WORD with the qualified witness/deadline; never WAIT_ATTEMPT. Complete proof permits route-c ADMIT with pointer generation and `e=pointer_enabled_ns`. (a) Withhold CONFIRM_NO_WAIT_WORD until a real checked publisher exists; an available publisher’s failure faults. (b) Confirm without role-support publication after the authorised merge amendment. |
| POINTER | When `pointer_continuous.known=false`, emit no POINTER control; retain diagnostics and process applicable evidence increments or scoped faults independently. Otherwise set `pointer_continuous` to its known value, `pointer_read_ok=pointer_snapshot_valid`, and `pointer_value=(pointer_nonnull!=0)`; the latter is Boolean, never an address. Actual fire time unavailable means `pointer_time=0`. Retained contradictions precede claims/release; suppressing an unknown-continuity POINTER must not suppress independently established faults. |
| POINTER revocation [D3] | For an already committed route-c bound, coordinator records revocation before delivering the contradiction: (a) checked durable support revocation; (b) checked boot-wide gap publication. Retain delivery even if publication fails, block certification and record failure. Pre-commit contradiction creates no committed-bound revocation. |
| TERMINAL | Exact retained identity/start/history plus unique transport join produces TERMINAL with `terminal_binding_proved=true`, `names_exit=true`, actual source/position and DATA time. Unproved match remains pending until finalisation, then TERMINAL_UNRESOLVED names that EXIT and existing reserved inventory identity without claiming a match. |
| END | Permanently end attempts. Deliver independently valid final admission if not already accepted; otherwise ABANDON only installed, bound, never-admitted inventory while startup remains open. Uninstalled/ambiguous entries remain pending. END is not termination. |
| FAULT | Process sticky scoped pointer contradictions before release and explicitly fault otherwise uncovered evidence-total increments as unknown evidence. Follow the POINTER revocation rule [D3] when a committed route-c bound is affected. Never fabricate a loss-counter increment for evidence uncertainty. |

Quiet witness mapping: copy `t,e,X→wait_word,v0,v1,armed_addr→armed_address,resolved_addr→resolved_address,accumulator,cpu_base,on_cpu_ts,threads0,threads1,terminal0,terminal1,cpu_base_valid`; handles equal `binding_token` only when retained binding and endpoint observations qualify. Translate S0/S1 explicitly and copy the coherence-claim flag.

No-word mapping: enable/read/fence from pointer times; pointer zero only for a qualified successful NULL read; fires by checked nonwrapping subtraction from the original-enable baseline; handles from retained binding; role from `permanent_role`. Require binary-specific executable, role and thread-history qualification, plus separate visibility, continuity, read, completion, counter, ownership and binding evidence. Major/role alone is insufficient. Copy the completed-claim flag; no qualification manifest/hash is required.

Resolution mapping: `F0=f0_ns`, exact `start=lifetime_start_ns` only when qualified, `fence=reconciliation_ns`, named failed-open/gone times, retained handle, actual IDs and validated fork cookie. Booleans require their corresponding known-true facts; `claims_match` requires `terminal_match`. Keep raw map-cookie/tombstone fields zero. Journal `exit_time=terminal_ns` remains separate from joined DATA time so contradictions cannot be overwritten.

Authentication deadline is nonzero only when both authentication phase and deadline qualification hold. Issue WAIT_LIMIT once when `d` reaches it; process age supplies no deadline.

Mask provenance: E is carried evidence; K is a named guarded construction with retained falsifier tests; C is capture-owned state.

| Mask | Derivation |
|---|---|
| LIFECYCLE_ACCOUNTING | E: activation before F0, complete history/producers and readable unchanged lifecycle baseline. |
| PREEXISTING | E: qualified conservative upper bound strictly before the retained F0 clock bound, or exact qualified start `<F0`; fork provenance takes precedence. |
| NO_FORK_PROVENANCE | E: no-observed-fork plus complete reconciled lifetime history. |
| COOKIE_AUTHORITY | E: partitioned fork allocator, serial discovery allocator, partitioned/burned cookie and no wrap. |
| IDENTITY_VISIBLE | E: immutable identity and successful retained installation visibility. |
| INSTALL_BEFORE_ENABLE | E: actual installation ordering consistent with visibility and selected enable. |
| PINNED_BINDING | E: retained token, inventory/FD/memory/postmaster binding and lifetime continuity; route c includes pinned pointer/role observations. |
| ADDRESS_CONTINUOUS | E: qualified address and fresh comparison plus stable phase or completely observed relocation; route c requires qualified pointer coverage. |
| PRESERVED_STATE | E: producer ownership and generation fencing; K: isolated seed publication with no subsequent userspace replacement. |
| ENABLED | E: successful enable for the selected generation and nonzero completion time. |
| EMISSION_IDENTITY | K: qualified emission paths select current-lifetime fork authority first, otherwise immutable discovery identity, before publication. Cookie presence alone is insufficient. |
| FAILURE_ACCOUNTING | E: readable relevant vectors, complete accounting through the cut, append/fault integrity and no unconsumed applicable failure. Quiet rejection is not loss. |
| BRACKET_ORDER | K: complete ordered quiet path; E: ordered timestamps, successful reads/seed/enable, fresh comparison and endpoints. Fresh/partial paths cannot assert it. |
| THREAD_HISTORY | E: single-thread history, both endpoint thread counts one and complete intervening lifecycle history. |
| OWNERSHIP | E: qualified absence of foreign writers, producer ownership and complete thread/exec history. |
| CPU_ENROLLED | E: accounting active on the complete fixed CPU set before S0. |
| FRESH_COHERENT_SEED | E: valid actual seed, valid CPU base, `cpu_base==accumulator`, `on_cpu_ts==0`, matching wait/time/address; K: ordered installation. |
| RECONCILED | E: completed lifecycle/generation/silent-writer fences; C: staged/consumed records reconciled with retained attachment history before commitment. |
| COUNTER_NO_WRAP | E: qualified voluntary-count no-wrap/reset under retained binding; equality is additional evidence only. |
| REQUIRED_READS | E: valid required endpoint/status/address/seed observations and qualified S0/S1; K: all required operations precede completed-proof claims. |
| INVENTORIED / STARTUP_OPEN | C: accepted inventory / actual current startup-gap state. Producer diagnostics and elapsed time cannot set them. |

For decoded record `r`, generation membership requires matching lifetime cookie, CPU in that generation’s retained B, and `r.seq > B[cpu][dest]`; retain the merge’s `t>=e` check. This is sufficient, not equivalent to all possible membership proofs. Post-enable acknowledgements and dequeue time supply no provenance. Set `baselines_refreshed=false`; TEXT defaults to shared.

**C. Coordinator**

Implement portable `discovery_integration.{h,c}` with bounded registries keyed by capture plus lifetime `(pid,cookie)`, numeric observation, attachment generation and terminal transport identity. Track attempted delivery, accepted control and committed outcome separately.

Attachment states are BOUND_UNINSTALLED → INSTALLED_PENDING → POINTER_PENDING/ATTEMPTING → ADMITTED_NUMERICAL/ADMITTED_NO_WORD/ABANDONED. Accepted admission and abandonment are mutually exclusive capture-long dispositions. Uninstalled entries may remain pending.

Coverage is orthogonal: PENDING → BOUND_COMMITTED or TERMINAL_COMMITTED. A committed route-c contradiction gives REVOKED_PENDING, retains admission history and prohibits replacement attachment. Terminal disposition is independently UNDECIDED → MATCHED or UNRESOLVED; final outcomes cannot switch. Numeric observations become J_RESOLVED/R_RESOLVED/G_RESOLVED only through qualified resolution.

Initialise failure baselines at or before F0. Use F0 as discovery coverage `boot_start`, preserving unrelated daemon clocks. Persist the startup gap before certification. Setup receives full producer inventory and numeric observations, including entries without backend slots.

Checkpoint order:

1. Block release and attachment re-entry.
2. Perform bounded producer refresh while required helpers remain valid.
3. Reread cursor-zero CENSUS and the allocation-independent current checkpoint; maintain a separate append cursor.
4. Copy new records and retained sticky contradictions into bounded coordinator storage.
5. Apply negative census updates, faults and required revocations before release-capable inputs.
6. Register inventory, installations and numbers; process ordered controls and terminal joins.
7. Apply eligible positive census last, using its own completed cut/evidence and the fresh checkpoint’s acceptance gate.
8. Resume transport release with current counters and checked publication updates.

Apply the same barrier inside receipt callbacks when attachment generates records during a poll. Drain available handoff before classifying discovery DATA; stage records whose origin is not yet provable, preserving source order and capacity accounting. Track the producer’s census ordinal and whether it has been observed unproved separately from coordinator acceptance. Refuse an unproved-to-proved transition at the same ordinal and do not apply that positive census. Apply an eligible positive census at most once per ordinal, after dependent obligations, negative updates and faults. A checkpoint-gated deferral of an otherwise proved record is not itself a producer-unproved observation. Repeated proved records must retain the same establishing cut and proof evidence; re-proof after invalidation requires a larger completed ordinal.

Change only the in-memory receipt interface to expose decoded `pgwt_wire_record` and permit envelope-origin assignment before capture feed. Retain `(capture,cpu,dest,seq) → (source,position,pid,cookie,time)` for terminals. Require destination L, source `cpu_count+1`, unique matching and exact PID/time/cookie consistency; cookie-less EXIT binding needs retained lifetime proof. Wire sequence is never reader position. Receipt may precede controls while DATA feed follows them.

At a completed reconciliation cut covering a held inventoried EXIT, consume every valid retained witness, then finalise still-unmatched EXITs as UNRESOLVED. PERMANENT_FAILURE triggers this without another successful fence. Consume matched terminal outcomes through checked release; neither matching nor disappearance alone commits discharge.

Permanent failure means retained history loss/overflow, irrecoverable lifecycle-baseline change, unrecoverable helper/CPU qualification loss, or an explicit latch whose evidence cannot be reconstructed during this capture. A timeout or transient fence failure alone is insufficient.

On permanent failure, stop attempts first, consume independently valid final attachments, select qualified ADMIT/ABANDON dispositions and retain remaining identities pending. Finalise unresolved terminal obligations so ordinary backends are not held indefinitely by undecided EXITs. Abandonment removes only missing-ADMIT dependency; producer completion, history, accounting and strict completeness still gate discard/frontier advancement. Unfinished requirements yield suspended/incomplete outcome.

Poll both evidence totals during transport silence. Advance delivered high-water marks only after checked handling; repeated snapshots are idempotent and jumps preserve accounting. Inspect sticky scoped contradictions independently of history. A sticky fault contributes one evidence increment at its latch, never one per polling tick; later independent faults remain separately accountable. Known cumulative totals remain known unless their own reads or arithmetic fail. Mandatory count inspection and qualified perf-versus-observer comparisons continue despite the latch or failed history; unavailable prerequisites yield unknown evidence, never a clean comparison. Until P2 supplies that inspection path, a live route-c proof with reconciliation other than COMPLETE has unknown current continuity and cannot justify clean release or certification. Preserve demonstrated breaks independently of that unknown state. If scope is unrecoverable, conservatively invalidate all potentially affected route-c proofs before release; append failure cannot hide revocation.

Only an already committed route-c bound triggers census reset and boot-wide revocation. A contradiction before commitment remains sticky and blocks later confirmation without manufacturing a committed-bound revocation. Explicit REARM must not erase it.

[D3] Under (a), missing publisher means pending inventory with pointer monitoring and withheld CONFIRM_NO_WAIT_WORD; an installed publisher that rejects publication produces writer failure. Under (b), remove only role-support publication/presentation after the authorised amendment; preserve route-c timing, monitoring, checked gaps and revocation.

Both variants require complete applicable revocation history on replay and retention through rotation. Missing/damaged history cannot leave earlier windows certified merely because a later watermark moved its floor forward: uncertain retroactive revocation makes affected coverage unavailable from boot and invalidates caches. A lost tail containing the sole boot-wide revocation is a required independent case. Variant (b) removes per-lifetime role records, not this recovery obligation.

Shutdown uses one checked absolute deadline `now + PGWT_DISCOVERY_SHUTDOWN_BUDGET_NS`, shared across every entry, helper wait and transport drain. Measure and choose that constant on EL8 under connection churn; this specification assigns no fixed one-second value.

Shutdown order: stop attempts; final bounded refresh while helpers are valid; quiesce emitters; complete bounded helper/transport accounting; stop helpers; drain allocation-independent faults, sticky contradictions and retained handoff; finalise terminal dispositions; finish merge and checked persistence. Never initiate helper-dependent resolution after helper validity ends. Deadline exhaustion cannot be acknowledged as successful shutdown.

| Operational result | Recorded outcome |
|---|---|
| Optional inactivity/role/address rejection | Pending candidate or failed bracket; no invented loss. |
| Required read/open/enable/map failure | Existing applicable checked counter/fault and pending/final attachment result; open failure alone never proves G. |
| Clock/namespace/identity contradiction | Unknown-evidence fault; affected proof withheld. |
| Transient fence/readiness failure | Pending; retain last successful cut without upgrading it. |
| Permanent reconciliation loss | Sticky permanent failure, stopped attempts and terminal-finalisation procedure; suspended/incomplete when required completion remains unavailable. |
| Capacity/allocator exhaustion | Allocation fault, retained tracking-failure state and incomplete coverage; never certify the tracked subset. |
| Malformed wire/sequence/dependency failure | Existing transport fault and corresponding incomplete/suspended outcome. |
| DATA/gap/frontier/flush/fsync rejection | Checked writer fault; affected publication/coverage remains uncommitted. |
| Role-support publication [D3] | (a) unavailable publisher withholds confirmation; available-but-failing publisher faults. (b) no such operation; checked gap failures still fault. |
| Shared deadline exhaustion | Incomplete shutdown and remaining obligations recorded; no inferred permanent failure. |
| Otherwise unclassified negative operation | Checked unknown-evidence fault and incomplete outcome. |
| Fault publication also fails | Stop certification; retain unclean outcome and surviving diagnostics, without claiming durable recording. |

Map operational failures into explicit checked outcome state instead of fatal daemon-poll negatives or false success. Preserve internal-invariant guards. Pending totals come from merge commitment, separately for numbers and lifetimes; overflow makes totals unavailable. Backend recovery consults retained final-attachment state, so pointer-only attachment is not retried merely because `wp_fd==-1`.

**Coordinator-round dependencies**

- **I1 — pure coordinator/translation, new files:** needs P1’s complete header, record tags, identity/resolution facts, masks and generation-storage contract. Uses injected P2 checkpoint/binding/stop/refresh behavior to test ordering and state transitions; P1 stubs cannot enable production certification.
- **I2 — receipt interface, membership and terminal join:** needs P1’s immutable per-generation B, cookies, retained terminal identifiers and exact timestamps; needs P2 binding lookup to obtain B and sticky lifetime state in production. Test interleaved CPUs, sequence versus reader position, receipt-before-controls, DATA-first/journal-first and generation mismatches.
- **I3 — daemon/open_snapshot/backend wiring:** needs all P1 records and finality facts plus every completed P2 API, permanent-failure latch and history-independent fault path. Implements checkpoint barriers, liveness, shutdown and checked error mapping on a common base. Publisher wiring follows [D3].
- **I4 — rows, schedules and mutants:** needs integrated P1/P2 and real checked writer/read-back seams. Include J13 through INVENTORY, unknown-ID REBIND refusal, stale/substituted B, strict read timing, pre/post-commit contradictions, publisher gating [D3], permanent-loss terminal liveness, capacity and every shutdown stage. End host integration at merge outputs and persisted record-level read-back, not verdict/display. Under [D3](a), six route-c rows require actual support publication; under (b), separately freeze amended presentation/publication answers while preserving bound timelines. Retain damaged-tail, missing-history, rotation/reopen and the four separately qualified EL8/Ubuntu × PG13/PG18 live cells.