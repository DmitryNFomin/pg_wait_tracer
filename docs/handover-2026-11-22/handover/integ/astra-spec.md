**Discovery integration: frozen interface specification**

All unmarked clauses below are normative requirements, not claims that the implementation already provides them. `P` and `M` denote `scratchpad/wt-pc` and `scratchpad/wt-discm`; `scratchpad` is `SP`. `R` denotes `M/contracts/merge-v1/oracle-discovery/ROWS.md`.
Scope: U-P supplies physical evidence; U-M fixes the two reviewed merge nits; U-I coordinates and wires them. Persistence is a separate unit. Preserve contracts, expected answers, existing guards, raw transport bytes and BPF programs. Exclude A2 verdict/display, fork-born never-initialised membership, priority, server and UI work.

**A. U-P header delta, exact**

VERIFIED: the producer exposes `struct pgwt_discovery_attempt` and physical facts; the merge exposes the distinct typedef `pgwt_discovery_attempt` and proof masks (`P/src/discovery_admission.h:42–146`; `M/src/core/pgwt_discovery.h:12–149`). The structure tag and typedef may coexist; do not rename either.
Rename only `struct pgwt_discovery_fact` → `struct pgwt_producer_fact`, and producer enumerators `PGWT_DISC_RUNNING`, `PGWT_DISC_DENIED`, `PGWT_DISC_MALFORMED` → `PGWT_PRODUCER_RUNNING`, `PGWT_PRODUCER_DENIED`, `PGWT_PRODUCER_MALFORMED`. Retain their values and explicitly translate inactivity enumerators.
Notation: `F` below means `struct pgwt_producer_fact { bool known, value; }`; unknown is `{false,false}`. Times are `uint64_t` nanoseconds in CLOCK_MONOTONIC unless stated otherwise; zero means unavailable. Tokens, cookies, generations and counts have no clock domain. Zero is a valid counter or CPU accumulator, never a substitute for a validity flag.
Keep all existing handoff members except the renames. Add the following members to `struct pgwt_discovery_attempt`; unlisted new members do not exist.

| Members and exact C types | Validity, unknown representation and consumer |
|---|---|
| `enum pgwt_discovery_handoff_kind kind` | Required discriminator. Define `PGWT_DH_INVALID=0`, then `CENSUS=1, INVENTORY=2, INSTALLED=3, NUMERIC=4, RESOLUTION=5, ATTEMPT=6, NO_WORD=7, POINTER=8, TERMINAL=9, END=10, FAULT=11`, each with prefix `PGWT_DH_`. Zero is invalid. |
| `uint64_t binding_token` | Nonzero, capture-unique token allocated when a pinned binding is retained; never an FD number, PID, pointer or recycled slot. Zero for absent pinned binding, including journal-only inventory. Supplies quiet/no-word handles and `resolution.handle`. |
| `uint64_t listed_at, failed_open, gone_at, bound_at, identity_visible_at` | Actual listing, failed-open completion, liveness-probe completion, retained-binding establishment and immutable installation visibility times. Relevant respectively to NUMERIC, RESOLUTION, INVENTORY and INSTALLED; zero is unknown. Consume `numeric()`, G ordering, binding and installation proof. |
| `uint32_t tid, tgid, parent, postmaster` | Actual task identifiers in the qualified namespace, not inferred from the resolution label. Zero unknown; consume resolution classification. `pid` remains the enumerated number/control PID. |
| `enum pgwt_discovery_holder holder` | Define `PGWT_DHOLDER_UNKNOWN=0`, `DISCOVERY=1`, `FORK=2`, `NONMEMBER=3`, `MISSING_FORK=4`, all with that prefix. Valid on R; diagnostic cross-check against actual facts, never sufficient proof. |
| `uint64_t fork_cookie` | Validated current-lifetime fork provenance; zero means no observed cookie only when complete retained history proves absence. Consumes `resolution.fork_cookie`; a current PID-map lookup alone is insufficient. |
| `F start_mono_known, namespaces, retained_binding` | Qualify respectively `lifetime_start_ns` as an exact monotonic start, common PID namespaces, and continuity of the retained pinned binding. Consume resolution booleans and PINNED_BINDING. Unknown remains unknown; a tick-derived upper bound is not an exact start. |
| `F history_complete, producers_complete, journal_complete, journal_eligible, gone_esrch` | Scoped through this record’s resolving fence. Consume corresponding resolution booleans. `journal_eligible=false` must be positively established for R/G; `gone_esrch=true` requires the actual post-open probe result. |
| `uint64_t reconciliation_ns, reconciliation_count` | Latest successfully completed relevant reconciliation and its capture-monotone ordinal. Initial zero means none. Preserve the last successful pair after later failure; never advance on an attempted or partial reconciliation. Consume census/resolution fences and checkpoint scheduling. |
| `enum pgwt_discovery_reconciliation reconciliation` | Define `PGWT_DREC_PENDING=0`, `PGWT_DREC_COMPLETE=1`, `PGWT_DREC_PERMANENT_FAILURE=2`. COMPLETE qualifies only the named completed cut; PERMANENT_FAILURE means usable reconciliation cannot be restored within this capture. Consumes liveness, never supplies a fence. |
| `struct pgwt_discovery_ack pre_enable[MAX_CPUS]; uint64_t pre_enable_completed_ns; bool pre_enable_valid` | Separate immutable B vector for this generation, indexed by existing `cpu_ids[0..cpu_count)`. Snapshot after active-generation publication and before enable. Invalid unless every enrolled CPU acknowledged the same request and the exclusion premise holds. `attempts[dest]` is a sequence cut; `request/stamp` are monotonic ns. Consumes transport membership and completion proof. |
| `uint64_t accumulator, on_cpu_ts; bool seed_valid` | Actual installed seed: accumulator in CPU-consumption ns; `on_cpu_ts` monotonic ns, with zero meaning off-CPU. `seed_valid=false` means unavailable. Consume quiet witness and FRESH_COHERENT_SEED; preserve separate `cpu_base_valid`. |
| `F install_before_enable, failure_accounting, voluntary_no_wrap` | Actual installation ordering; complete readable failure accounting through the record’s completion cut; qualified absence of voluntary-count reset/wrap across the bracket. Consume identically named mask obligations. Equality of endpoints alone proves none of these. |
| `bool attempts_ended, claims_completed_coherence, claims_completed_no_word` | Explicit irreversible end of startup attempts; explicit claims carried with proposed proofs. False claims flags assert no completed proof. END and final ATTEMPT/NO_WORD carry `attempts_ended`; consume final disposition and confirmation claim fields. |
| `bool pointer_snapshot_valid` | Qualifies existing pointer counts from a completed observation. False means unknown, not zero fires. Consume POINTER/no-word witness; non-NULL counts encode occurrence, not a physical address. |
| `uint64_t kernel_evidence_count, user_evidence_count; bool evidence_counts_known, handoff_complete` | Capture-monotone cumulative evidence-fault totals, separate from the 15 counters; known false on unreadability/wrap. `handoff_complete` starts true and becomes permanently false on lost append. Consume fault delivery and certification gating; allocation-independent storage is mandatory. |

Changed existing-field semantics: reserve/burn `cookie` at bound discovery inventory creation and journal-only identity creation, before installation; numeric-only observations retain zero. Export INVENTORY even without a backend slot or successful attachment, and INSTALLED only after successful immutable installation. Cookie exhaustion retains an unregistered obligation and records allocation failure.
On every fresh fallback, reset and recompute `address_compared`; never inherit it from a failed quiet bracket. Set `accumulator/on_cpu_ts/seed_valid` from the installed seed. Preserve B independently of `producers_after`. Fill numerical `counters_after` and `failure_accounting` as well as route-c counterparts.
`pointer_fires_before` is the baseline immediately before the original pointer enable, not a later retry snapshot; `pointer_fires_after` covers through the stated fence. Require nonwrapping accounting and strict `pointer_read_ns > pointer_enabled_ns`. `pointer_continuous` describes the required continuous-observation proof; retain demonstrated invalidation even after history failure.
Explicit tagged list: CENSUS, INVENTORY, INSTALLED, NUMERIC, RESOLUTION, ATTEMPT, NO_WORD, POINTER, TERMINAL, END, FAULT. Existing selector booleans remain compatibility mirrors; `kind` is authoritative. FAULT may additionally carry `no_wait_word` and scoped pointer facts; those are payload, not another kind. Reject contradictory tags; zero all irrelevant payload.
Add `struct pgwt_discovery_checkpoint` with exactly `uint64_t reconciliation_ns, reconciliation_count, kernel_evidence_count, user_evidence_count; enum pgwt_discovery_reconciliation reconciliation; bool evidence_counts_known, handoff_complete, attempts_stopped; int error;`. `error` is zero or positive errno, diagnostic only; unknown reconciliation is PENDING.
Add `struct pgwt_discovery_checkpoint pgwt_discovery_refresh_until(struct pgwt_daemon *, uint64_t deadline_ns);`: absolute monotonic deadline, checked before each entry and fence wait; refresh scoped faults even when history is unusable. Deadline exhaustion returns PENDING unless capture-long failure is independently established.
Add `void pgwt_discovery_checkpoint(struct pgwt_daemon *, struct pgwt_discovery_checkpoint *);`, allocation-free and nonblocking; `bool pgwt_discovery_binding(struct pgwt_daemon *, uint64_t binding_token, struct pgwt_discovery_attempt *);`, returning a retained INVENTORY snapshot including installation, attempt-finality and sticky pointer facts; and `void pgwt_discovery_stop_attempts(struct pgwt_daemon *);`, an immediate irreversible gate that leaves required watches active.
Keep `pgwt_discovery_refresh()` as a bounded compatibility wrapper. Keep other existing function signatures. `pgwt_discovery_next()` always reserves cursor zero for the mutable CENSUS snapshot once discovery starts; append consumption begins at one, so readiness cannot shift cursor interpretation. Binding snapshots must survive append failure.
VERIFIED: the original interface has unknown authentication qualification, mutable cursor-zero census, and history failure before scoped pointer inspection (`P/src/discovery_admission.h:81–85`; `P/src/discovery_producer.c:1183–1244`). The new API must preserve unknown deadlines and remove that fault-delivery dependency.

**B. Translation table, final**

All merge controls are zero-initialised; unused fields stay zero. Every delivery uses current detection time `d` and a fresh complete 15-component snapshot `C`, including post-publication updates. Historical vectors qualify witnesses; they never replace current `C`. Copy `(pid,cookie)` from the retained identity, not a current PID lookup.
Define three-valued conjunction: any known false operand → known false; otherwise any unknown operand → unknown; otherwise true. Set a mask’s `known` bit only for a known result, and `truth` only for true. Optional failed candidates produce pending/rejection, not invented false admission claims.

| Producer kind | Translation, fields, prerequisites and ordering |
|---|---|
| CENSUS | `pgwt_capture_discovery_census(reconciliation_ns,proved,d,C)`. Require enumeration, lifecycle activation, completed reconciliation, qualified unchanged lifecycle counters, complete handoff, all obligations registered and no unresolved numbers. Apply invalidation first and positive certification last. Do not use enumeration-start `census_cut_ns` as completion. |
| INVENTORY | `inventory(pid,cookie)` before dependent input; `start_identity(pid,cookie,lifetime_start_ns)` only with `start_mono_known=true`. Retain token, clock evidence and physical state. A number undergoing R/J remains unresolved until its qualified resolution is accepted; do not prematurely register its replacement through an independent shortcut. |
| INSTALLED | `installed(pid,cookie,proof)` after INVENTORY; require IDENTITY_VISIBLE and EMISSION_IDENTITY. No admission, enablement or bound is implied. Retain an uninstalled identity on failure. |
| NUMERIC | `numeric(pid,listed_at,failed_open)`; cookie zero. Deduplicate unresolved observations by number, preserving all independently bound lifetimes. Deliver before any corresponding J/R/G. |
| RESOLUTION | Explicitly translate J→JOURNAL, R→REBIND, G→GONE using resolution mapping below. J precedes R/G when eligible. R’s holder enum is cross-checked, never cast. J additionally names the joined EXIT; G supplies no lifetime cookie or discharge. |
| ATTEMPT | One WAIT_ATTEMPT per `(cookie,attempt)` representing a physical word attempt. For a watched NULL candidate, qualified non-NULL POINTER precedes it. Once `attempts_ended && final_attachment`, issue one numerical ADMIT with generation, `e`, admission mask; then CONFIRM only for a complete quiet witness. Otherwise retain the failed attempt for END. |
| NO_WORD | TRY_NO_WAIT_WORD with witness and qualified deadline, if any; no WAIT_ATTEMPT. For a complete qualified permanent-role witness, issue route-c ADMIT, then CONFIRM_NO_WAIT_WORD when checked support persistence is available. Both use the pointer generation and `e=pointer_enabled_ns`. Failed qualification leaves pending. |
| POINTER | `pointer_read_ok=pointer_snapshot_valid`, `pointer_value=(pointer_nonnull!=0)` as a documented Boolean non-NULL indicator, `pointer_time=0` when actual fire time is unavailable. `pointer_continuous` is emitted only from known continuity evidence; do not convert unknown into a demonstrated break. Deliver retained contradictions before subsequent claims or release. |
| TERMINAL | Exact retained identity/start/history plus transport join → TERMINAL with `terminal_binding_proved=true`, `names_exit=true`, joined source/position and DATA semantic time. An unproved match remains an obligation until checkpoint finalisation; then TERMINAL_UNRESOLVED names the same EXIT and an existing reserved inventory identity without asserting a match. |
| END | Set attempts permanently ended. Independently valid final evidence → ADMIT if not already accepted; otherwise ABANDON only for installed, bound, never-admitted inventory while startup remains open. Uninstalled/ambiguous entries remain pending. END is not a terminal event. |
| FAULT | Deliver scoped POINTER invalidation to revoke affected route-c support, and explicit `FAULT(unknown_evidence,d,C)` for otherwise uncovered evidence-fault increments. Continue ordinary counter checking; never increment a loss component merely to represent evidence uncertainty. |

Quiet mapping: `w.{t,e,wait_word,v0,v1,armed_address,resolved_address,accumulator,cpu_base,on_cpu_ts,threads0,threads1,terminal0,terminal1,cpu_base_valid}` ← corresponding producer members, with `X→wait_word`; handles equal `binding_token` only when retained binding and both endpoint observations qualify. Translate S0/S1 explicitly; producer UNKNOWN stays unknown. Copy the explicit coherence-claim flag.
No-word mapping: `enable/read_at/fence` ← pointer times; `pointer=0` only for a successful qualified NULL read; `fires=after−before` with checked nonwrapping subtraction and original-enable coverage; handles from retained binding; `role=permanent_role`; role qualification combines producer evidence with the applicable binary/major qualification. Visibility, continuity, read success, completion, counters, ownership and binding each require their separate evidence; copy the explicit completed-claim flag.
Resolution mapping: `F0=f0_ns`, `start=lifetime_start_ns`, `fence=reconciliation_ns`, `failed_open/gone_at/handle` from their named members; task identifiers and fork cookie copied directly; `exit_time=terminal_ns`. `start_known/clocks_qualified` require exact qualified monotonic start; namespaces/history/producers/journal/eligibility/ESRCH/binding/counter booleans are true only from their qualified facts. `claims_match` requires `terminal_match`. Raw map-cookie/tombstone fields stay zero: this interface supplies no such evidence.
Joined terminal fields always use DATA’s actual timestamp and reader position; compare the journal timestamp separately. Do not overwrite a contradiction with the journal value. Authentication deadline is nonzero only when both `authentication_phase` and `auth_deadline_known` qualify it; WAIT_LIMIT is issued once when current `d` reaches it.
Mask provenance: **E** = evidence carried; **K** = true by construction of the named qualified code path, acceptable only while its guards and falsifier tests remain intact; **C** = capture-owned. K is never “the operation usually succeeds.”

| Merge mask bit | Three-valued derivation and provenance |
|---|---|
| LIFECYCLE_ACCOUNTING | **E** lifecycle activation before F0 ∧ complete retained history/producers ∧ readable unchanged lifecycle-class baseline. |
| PREEXISTING | **E** qualified conservative start upper bound strictly before the retained F0 clock bound, or qualified exact kernel start `<F0`; fork provenance takes precedence. |
| NO_FORK_PROVENANCE | **E** `no_observed_fork` ∧ complete reconciled history for this lifetime. |
| COOKIE_AUTHORITY | **E** partitioned fork allocator ∧ serial discovery allocator ∧ partitioned/burned cookie ∧ no wrap. |
| IDENTITY_VISIBLE | **E** immutable identity ∧ successful retained installation visibility. |
| INSTALL_BEFORE_ENABLE | **E** `install_before_enable` ∧ visibility time/order consistent with the selected numerical or pointer enable. |
| PINNED_BINDING | **E** retained token ∧ inventory/FD/memory/postmaster binding ∧ lifecycle continuity; route-c includes the same pinned pointer/role observations. |
| ADDRESS_CONTINUOUS | **E** numerical address qualification ∧ fresh comparison ∧ qualified stable phase or complete observed relocation; route-c uses qualified complete-pointer coverage instead. |
| PRESERVED_STATE | **E** `producer_owned` ∧ generation fencing; **K** isolated seed publication in `disc_seed`, with no post-publication userspace replacement. |
| ENABLED | **E** successful `enable_confirmed` for the selected generation and nonzero completion time. |
| EMISSION_IDENTITY | **K** qualified emission paths resolve current-lifetime fork authority first, otherwise immutable discovery identity, and snapshot it before publication; requires the build/platform qualification, not cookie presence alone. |
| FAILURE_ACCOUNTING | **E** `failure_accounting`, valid relevant before/after vectors, complete append/fault accounting and no unconsumed applicable failure. Quiet failure alone is not loss. |
| BRACKET_ORDER | **K** the complete ordered `pgwt_discovery_brackets` quiet path; **E** ordered timestamps, fresh comparison, successful reads/seed/enable and endpoint observations. A partial/fresh path cannot assert it. |
| THREAD_HISTORY | **E** single-thread history ∧ both thread counts one ∧ complete intervening lifecycle history. |
| OWNERSHIP | **E** qualified absence of foreign writers ∧ producer ownership ∧ complete thread/exec history for the required interval. |
| CPU_ENROLLED | **E** CPU accounting active and enrolled on the complete fixed CPU set before S0. |
| FRESH_COHERENT_SEED | **E** actual valid seed, `cpu_base_valid`, `cpu_base==accumulator`, `on_cpu_ts==0`, matching wait/time/address; **K** installation at the ordered seed step. |
| RECONCILED | **E** completed lifecycle/generation/silent-writer fences; **C** reconciliation of staged and consumed records with retained attachment history before commitment. |
| COUNTER_NO_WRAP | **E** `voluntary_no_wrap` under the retained binding; endpoint equality is an additional check, not the proof. |
| REQUIRED_READS | **E** valid endpoint/status/address/seed observations and qualified successful S0/S1; **K** the complete quiet path performs each required operation before claiming completion. |
| INVENTORIED | **C** accepted capture inventory; producer diagnostics cannot set it. |
| STARTUP_OPEN | **C** current capture gap state; neither elapsed time nor producer census counts set it. |

VERIFIED: quiet validation checks handles, CPU state and masks; fresh membership additionally checks matching generation and `t>=e`; no-word validation requires strict read-after-enable (`M/src/core/merge.c:487–525`).
For decoded record `r`, prove final-generation membership only when its lifetime cookie matches, its CPU belongs to retained B, and `r.seq>B[cpu][dest]`; retain the merge’s `t>=e` check. This is sufficient, not an equivalence. Never substitute post-enable acknowledgements. Set `baselines_refreshed=false`; default TEXT to shared.
VERIFIED: wire sequence and reader position are distinct, markers do not refresh the numerical baseline, and TEXT dependencies are query-keyed (`P/src/pgwt_transport_wire.h:29–53`; `M/src/percpu_transport.c:254–267`; `P/src/bpf/pg_wait_tracer.bpf.c:1748–1788,1930–1982`; `M/src/core/merge_adapter.c:153–185,312–325`).

**C. Coordinator state machine**

Use a portable coordinator in `discovery_integration.{h,c}`. Its bounded registries are keyed by capture plus `(pid,cookie)`, numeric observation, attachment generation, and terminal transport identity. Track delivery separately from merge acceptance and commitment.
Lifetime attachment states: BOUND_UNINSTALLED → INSTALLED_PENDING → POINTER_PENDING or ATTEMPTING → ADMITTED_NUMERICAL / ADMITTED_NO_WORD / ABANDONED. Accepted admission and abandonment are mutually exclusive capture-long dispositions. An uninstalled entry may remain pending indefinitely.
Coverage state is orthogonal: PENDING → BOUND_COMMITTED or TERMINAL_COMMITTED; route-c contradiction moves BOUND_COMMITTED → REVOKED_PENDING, retaining admission history and prohibiting replacement attachment. Terminal obligations independently move UNDECIDED → MATCHED or UNRESOLVED, never between final outcomes.
Numeric states: OBSERVED → J_RESOLVED / R_RESOLVED / G_RESOLVED, otherwise PENDING. Preserve earlier journal lifetimes across reuse. Match or discharge nothing merely because the number is gone.
Initialise failure baselines at or before F0; use F0 as discovery coverage `boot_start`, while preserving unrelated daemon timing fields. Persist the startup gap before certification. Pass the full producer inventory/numeric observations, not merely backend slots, into capture setup.
VERIFIED: coverage setup currently uses `d->start_ts` and a backend-derived PID list; census rejects a fence earlier than coverage start (`M/src/open_snapshot.c:693–712`; `M/src/merge_capture.c:802–806`). U-I must align these clocks and startup inputs.
Checkpoint order: (1) block release and attachment re-entry; (2) bounded producer refresh; (3) reread cursor zero and allocation-independent fault checkpoint; (4) copy new append records and retained scoped contradictions into bounded coordinator storage; (5) apply negative census updates, faults and revocations before any release-capable input; (6) register inventory/installations/numbers, then process ordered controls and terminal joins; (7) apply eligible positive census last; (8) resume transport release using current counters. Retain a separate append cursor.
The same barrier applies inside receipt callbacks when backend attachment creates handoff records during a poll pass. Drain available handoff before classifying discovery DATA. Stage records whose final origin is not yet provable; preserve source order and capacity accounting. No record receives provenance from dequeue time.
Change only the in-memory transport receipt interface to expose the decoded `pgwt_wire_record` and permit setting its envelope origin before capture feed. Retain `(capture,cpu,dest,seq)→(source,position,pid,cookie,time)` for terminals. Require `dest=L`, source `cpu_count+1`, exact PID/time/cookie consistency and unique matching; never use `terminal_seq` as position. Receipt may precede control delivery while DATA feed follows it, allowing disposition-before-DATA schedules.
At a completed reconciliation cut covering a held EXIT, consume all valid retained witnesses, then finalise each still-unmatched inventoried EXIT as UNRESOLVED. If reconciliation becomes PERMANENT_FAILURE, do this immediately without requiring another completion fence. A transient incomplete fence alone does not trigger capture-long failure.
Permanent-failure triggers are retained history loss/overflow, an irrecoverable lifecycle-baseline change, unrecoverable helper/CPU qualification loss, or an explicit producer failure latch whose required evidence cannot be reconstructed during this capture. A deadline alone is not proof of any trigger.
On permanent failure, stop attempts first, consume independently valid retained final attachments, select qualified ADMIT/ABANDON outcomes, and retain other identities pending. Accepted abandonment removes only the missing-ADMIT dependency: producer completion, history, failure accounting and ordinary strict completeness still gate discard/frontier advancement. If these cannot finish, report suspended/incomplete.
Shutdown uses one absolute deadline, initially `now+1,000,000,000 ns` with checked addition, shared by all discovery/transport drain work. Stop new attempts; perform final refresh while helpers remain valid; quiesce emitters; complete bounded helper/transport accounting; stop helpers; perform a final allocation-independent fault, scoped-contradiction and retained-handoff drain; finalise EXIT dispositions; finish merge and checked persistence. Never start helper-dependent resolution after helper validity is lost.
Operational error mapping: optional inactivity/role/address rejection → pending or failed bracket; required read/open/enable/map failure → its existing checked counter/fault plus pending/final attachment outcome; clock/namespace/identity contradiction → unknown-evidence fault; transient fence/readiness failure → pending; permanent reconciliation loss → the liveness procedure; capacity/allocator exhaustion → allocation fault plus incomplete tracking; malformed wire/sequence/dependency failure → existing transport fault; support/DATA/gap/frontier/flush/fsync failure → checked writer fault and uncommitted coverage; deadline exhaustion → incomplete shutdown; any otherwise unclassified negative operation result → checked unknown-evidence fault and incomplete outcome.
If fault publication also fails, stop certification, preserve an unclean outcome and surviving diagnostics; do not claim durable recording. Never pass these operational failures as fatal negatives to daemon polling, and never acknowledge them as successful completion. Preserve internal-invariant guards and bounded scheduling.
VERIFIED: negative poll results currently take fatal daemon paths, and capture operations can return allocation/argument/publication errors (`P/src/daemon.c:581–584,2118–2121`; `M/src/merge_capture.c:725–806,1015–1031`). U-I must use explicit checked outcome state.
Poll both evidence totals even during transport silence; retain delivered high-water marks only after checked handling. Repeated snapshots are idempotent; jumps preserve fault accounting. Read scoped sticky contradictions independently of history. If scope cannot be recovered, conservatively invalidate all potentially affected route-c proofs before release; append failure never hides a committed-bound revocation.
Pending totals come from merge commitment, with separate numeric/lifetime counts; overflow makes the total unavailable rather than reporting the tracked subset as complete. Backend recovery consults retained lifetime/final-attachment state, so a pointer-only final attachment is not retried merely because `wp_fd==-1`.

**D. Support persistence for the no-wait-word route**

Choose one capture-bound, atomically replaced sidecar: `discovery-role-<32 lowercase hex boot_nonce>.v1` beside trace files. It contains a complete retained table, including revoked entries; retain it while any file from that capture remains.
Bytes are little-endian, with no native structure copies. Header is 64 bytes: offset 0 magic ASCII `PGWTDROL` (8); 8 schema `u16=1`; 10 row size `u16=64`; 12 row count `u32`; 16 boot nonce (16); 32 kernel boot ID (16); 48 monotonic `boot_start u64`; 56 snapshot observation time `u64`. Then N sorted 64-byte rows, followed by CRC32C over header and rows (`u32`) and zero padding (`u32`): total `72+64N`.
Each row: offset 0 `pid u32`; 4 qualified role code `u32`; 8 discovery cookie `u64`; 16 bound `e u64`; 24 revocation detection time `u64` (zero means not revoked); 32 qualification identity SHA-256 (32 bytes). The qualification identity names an immutable manifest binding PostgreSQL major, executable identity, role-code meaning and permanence qualification.
Reserve BOOT mode bit `0x400` as “discovery role sidecar required”; write an empty valid sidecar before advertising any discovery coverage under that bit. This is an existing BOOT-field capability extension, not a new numerical carrier or wire change. Unknown readers must refuse unsupported coverage semantics.
Publication callback writes the complete snapshot through checked temporary-file write, fsync, rename and directory fsync before returning success; the merge’s subsequent release checkpoints still decide bound commitment. Repeated identical assertions are idempotent; conflicting identities, roles, qualifications or `e` fail. Revocation is irreversible.
On revocation, durably reopen coverage from boot and publish the revoked snapshot before further certification; invalidate caches keyed by capture and sidecar content. If one publication fails, retain incomplete coverage; a durable boot-wide revocation invalidates stale active role assertions conservatively.
Absent, damaged, mismatched or unsupported required sidecar → unavailable coverage/role support, never inferred role or zero measurements. Revoked assertion → no active route-c support and unavailable affected coverage from boot. Valid active assertion → “present, not measured”, qualified role, unavailable individual quantities and no numerical/population contribution.
Separate persistence unit owns new `src/discovery_role.{h,c}`, writer/reader API changes in `src/event_writer.{h,c}` and `src/event_reader.{h,c}`, capability definition in `src/coverage_record.h`, and support consumption in `src/coverage.{h,c}`, plus tests/registration. U-I owns the callback wiring. Specify the new format in a separately authorised `contracts/discovery-role-v1/`; do not change `contracts/coverage-a2-v1` or GAP_CLOSE reserved fields.
VERIFIED: route-c commitment depends on successful support publication; existing BOOT carries capture identity, and GAP_OPEN has checked fsync (`M/src/merge_capture.c:395–412`; `M/src/coverage_record.h:9–25,65–70`; `M/src/event_writer.c:1647–1669`).
Interim: if U-I lands first, a PG13 lifetime needing route c remains pending with unavailable individual values and an open startup gap live and on reopen unless valid terminal discharge resolves it; durable qualified-role presentation is not yet provided.

**E. Row table**

INFERRED classification: E = coordinator end-to-end with injected physical operations; M = merge/consumer-only; I = invalid-claim injection; P = requires an additional platform/provenance qualification. E is host reachability, not a claim of live qualification. Preserve every canonical input and answer; physical operations may produce equivalent control delivery without pretending the coordinator emits invalid claims.
Counts: **85 E, 8 M, 12 I, 3 P = 108**. The M class comprises the settled seven impossible disposition schedules plus D13b, a direct coverage-reader population fixture. The twelve settled invalid-claim fixtures remain separate.

| Row | Class | Required distinction |
|---|---|---|
| D01 | E | Fresh anchor and persisted endpoint arithmetic. |
| D02a | E | Quiet bound; strict completeness beyond seed. |
| D02b | E | Userspace reread supplies no bound. |
| D02c | E | Delayed transitions retain earlier provenance. |
| D02d | I | Explicitly false coherent-seed claim. |
| D02e | E | Exhaustion, valid fresh attachment, no fresh DATA. |
| D03 | E | DATA receipt before final admission delivery. |
| D04a-install | I | False installation-before-enable claim. |
| D04a-state | I | False preservation claim. |
| D04a-binding | I | Claimed binding without evidence. |
| D04a-address | I | Claimed continuity despite unobserved relocation. |
| D04a-cookie | I | Claimed allocator authority outside partition. |
| D04b | E | Start upper bound equality refuses admission. |
| D04c | I | False FD/reused-PID lifetime match. |
| D04d | E | Burned cookie and pre-install exit tombstone. |
| D04e | E | Reuse, tombstone persistence and fork allocation failure. |
| D04f | E | Partition endpoints and exhaustion. |
| D04g | E | Mutable-state preservation and real insertion failure. |
| D07a | E | Multiple committed quiet bounds. |
| D07b | E | Census fence dominates closure. |
| D08a | E | Terminal discharge without attachment/state. |
| D08b | E | PID-only ESRCH does not discharge. |
| D09 | E | Reuse, delayed DATA and cleanup isolation. |
| D10 | E | Late unlisted lifetime revokes from boot. |
| D13a | E | Counter fault preserves suspension. |
| D13b | M | Direct coverage-only pre-boot UNTRACED rejection. |
| D13c | I | Discovery claim substitutes for missing fork proof. |
| D13d | E | Fresh-anchor writer rejection. |
| D13e | I | Unqualified mode-Q admission claim. |
| Q01 | E | Coherent quiet wait/CPU interval. |
| Q02 | E | Voluntary-count change; producer writes preserved. |
| Q03 | E | Running endpoint rejects quiet proof. |
| Q04 | E | State letters/pcount cannot qualify inactivity. |
| Q04b | E | Schedstat cannot qualify inactivity. |
| Q05 | E | Optional denied inactivity read. |
| Q06 | E | Thread history rejects quiet; independent fresh route. |
| Q07 | E | Valid zero CPU seed and measured execution. |
| Q08 | E | Address mismatch rejects bracket. |
| Q09 | E | Earlier generation cannot replace final baseline. |
| D14a | E | Attached/discovery completion populations remain distinct. |
| D14b | E | Census endpoint clips available interval. |
| D14c | E | Equal timestamp does not establish generation membership. |
| A01 | E | Installed failed identity abandoned; validated discard. |
| A02 | E | Future abandoned DATA remains discarded. |
| A03 | E | Abandonment followed by genuine terminal discharge. |
| A04 | M | Abandonment after accepted admission. |
| A05 | M | Admission after abandonment. |
| A06 | M | Uninventoried abandonment. |
| A07 | E | Abandoned lifetime still blocks closure. |
| A08 | E | Abandonment does not forgive counter loss. |
| A09 | E | Idempotent abandonment retransmission. |
| A10 | M | Confirmation after abandonment. |
| A11 | E | Ordinary late-record validation still applies. |
| A12 | E | Explicit external REARM preserves abandonment. |
| A13 | E | Missing E completeness prevents advancement. |
| A14 | E | Unknown failure accounting prevents release. |
| A15 | E | Rejected EXIT does not discharge. |
| A16 | E | Ambiguous DATA identity cannot be discarded successfully. |
| A17 | E | Fork-route replacement remains independent. |
| A18 | E | Accepted disposition does not cancel prior fault. |
| A19 | P | Exclusive TEXT needs independent attribution proof; default shared path differs. |
| A20 | M | First abandonment after terminal closure. |
| T1 | E | Final unresolved disposition releases held EXIT only. |
| T2 | M | Deliberately omitted final disposition at finish. |
| T3 | M | Matched disposition contradicts prior unresolved. |
| T4 | E | Receipt/join and disposition before DATA feed. |
| V1 | E | Post-EXIT checkpoint failure prevents discharge. |
| V2 | E | Earlier TERM does not change transaction failure. |
| V3 | E | Later DATA publication failure prevents discharge. |
| V4 | E | FRONTIER publication failure prevents discharge. |
| N1 | E | Injected qualified permanent-role support; no CPU seed. |
| N2 | E | Client NULL remains pending. |
| N3 | E | Pre-enable NULL read is insufficient. |
| N4 | E | Pointer fire invalidates zero-fire candidate. |
| N5 | E | Committed route-c contradiction revokes from boot. |
| N6 | E | NULL termination preserves legitimate prior bound. |
| N7 | E | Non-NULL observation precedes numerical attempt. |
| N8 | E | Installed NULL candidate exits without word attempt. |
| N9 | E | Completion-checkpoint lifecycle failure. |
| N10 | E | Injected independently qualified stats-collector role. |
| N11 | E | Unqualified role code cannot pass a whitelist assumption. |
| N12 | I | False continuous-observation admission claim. |
| N13 | E | Role support and numerical lifetime close jointly. |
| N14 | E | Supporting-evidence publication rejection. |
| N15 | E | Required pointer-read failure retains real counter. |
| N16 | P | Qualified native authentication deadline is required. |
| J1 | E | Journal-only reserved identity and cookie-less EXIT. |
| J2 | E | Post-open ESRCH plus completed fence. |
| J3 | E | ENOENT followed by live probe remains pending. |
| J4 | E | EACCES/EPERM never proves gone. |
| J5 | E | Lifecycle loss prevents G. |
| J6 | E | Entered but incomplete producer prevents matching. |
| J7 | E | Injected kernel filter accounts missing fork. |
| J8 | E | Non-leader rejected despite stale discovery identity. |
| J9 | E | Non-child rejected despite tombstone. |
| J10 | E | Non-child rejected despite live stale identity. |
| J11 | E | Unknown kernel start produces evidence fault. |
| J12 | E | R resolves to observed fork-route replacement. |
| J13 | P | Live pre-existing R requires qualified exact start for current merge interface. |
| J14 | E | R resolves proved non-member. |
| J15 | E | Journal lifetime survives fork-route reuse. |
| J16 | I | False match against different retained kernel start. |
| J17 | I | False journal/DATA timestamp agreement. |
| J18 | E | Journal EXIT publication failure prevents discharge. |
| J19 | E | Post-publication lifecycle fault prevents discharge. |
| J20 | E | Later unlisted target revokes after G resolution. |
| J21 | E | ESRCH before failed open cannot establish G. |
| J22 | E | Observed fork provenance overrides pre-F0 start. |

**F. Tests per unit**

U-P host cases must exercise every added member’s known/unknown/contradictory state: reservation before any attachment failure; journal-only allocation; no-backend-slot inventory; token stability/reuse; installation reporting; numeric observation and each holder class; actual G ordering; exact-start qualification; completed reconciliation ordinal versus transient/permanent failure; immutable pre-enable B; actual CPU seed including zero; fresh address comparison; attempts-ended; strict NULL read timing; scoped faults after history failure and append failure.
Pair each added fact with a product mutant that removes, falsifies, swaps, reuses or prematurely asserts it. Require successful mutant build, reached named case and failure of the intended assertion. Include missing pre-enable-vector retention, post-enable substitution, stale binding token, cookie reservation delayed until attach, unknown→true, read equality acceptance, reconciliation success on timeout, dropped scoped revocation and false loss on role exit/PID reuse.
U-I host route is injected producer operations → real handoff → coordinator → actual transport decoder/capture merge → real checked writer/sidecar → reopened coverage reader. E rows must assert controls, authoritative pending counts, committed bounds/discharges, persisted GAP_CLOSE endpoint/reserved bytes and numerical reader results. P rows receive synthetic host proofs but remain unqualified for production until their missing premise is supplied; I/M rows retain their explicit injection boundary.
Target schedules: cursor-zero invalidation after initial consumption; empty unknown census; registration before positive census; attachment inside receipt; interleaved CPUs where seq differs from L position; journal-first and DATA-first; same PID/different starts; same timestamp/different generations; pre-enable marker with `seq<=B`; migrating post-enable emissions straddling later acknowledgements; shared TEXT dependency; non-NULL/NULL ABA; fault bursts, unchanged rereads, failed append, lost history and shutdown faults.
Liveness tests must prove ordinary backends cease being held by still-undisposed inventoried cookie-less EXITs after permanent reconciliation loss, while incomplete abandonment fences still prevent trusted advancement. Test every shutdown stage, invalid helpers, total deadline across many numbers, every bounded store full, and failure of both ordinary publication and fault publication.
Persistence tests cover empty snapshot, multiple lifetimes, PID reuse, qualification identity, exact bytes/CRC/size, required-sidecar absence, damage, wrong capture, truncation, atomic-publication failures, irreversible revocation, rotation/retention, cache invalidation and reopen. N14 must reject the actual supporting publication; an always-successful callback is not an acceptable seam.
U-M retains both reviewed falsifiers: TRY_NW → ADMIT(c) → contradictory POINTER → explicit REARM → CONFIRM_NO_WAIT_WORD must never commit; removing the confirmation pointer guard must fail. Sweep pre-commit POINTER placements and require identical census state with no committed-bound revocation; post-commit contradiction must revoke from boot. Keep `src/core/merge.c` outside U-I.
Live cells are separately recorded for vendor EL8 4.18/PG13, EL8 4.18/PG18, Ubuntu/PG13 and Ubuntu/PG18. Freeze kernel, executable, configuration and qualification identities. Assert quiet indefinitely idle clients/auxiliaries, zero CPU validity, busy fresh admission, startup exit/J, real PID reuse, fork-route disconnect during capture, required-read failure, capacity refusal and orderly interruption.
PG13 cells additionally exercise each qualified permanent-NULL role, durable role support and role exit/reuse; PG18 cells exercise qualified pointer relocation/termination and relevant thread history where supported. Normal cases assert unchanged 15-counter vectors **and** unchanged evidence-fault totals. Unsupported premises remain explicitly unqualified.
A cell is vacuous if it never reaches the intended route/race, stops before the test backend exits, silently omits the role, checks only loss counters, or leaves every positive case pending. Host injection does not qualify kernel behavior; Linux qualification does not replace deterministic falsifiers.
Implementation checks: existing producer and discovery host/mutant suites, new integration/persistence suites, registration guards, flag-off byte comparisons, then authorised `make check` and Linux box gates. Register C tests in `tests/unit_tests.list`. Run no eBPF work on macOS; these are required checks, not results claimed by this specification.

**G. Open points**

**Exact start for pre-existing R — advisers:** VERIFIED: merge classification needs a known exact monotonic start and REBIND stores it as lifetime identity (`M/src/core/merge_adapter.c:229–238`; `M/src/merge_capture.c:858–867`); producer rebinding currently obtains `/proc` tick observations (`P/src/discovery_producer.c:1045–1076`). Recommended answer: retain pending until exact retained evidence exists; separately approve a bounded-start classification interface if ordinary live R must complete without it. Never pass a tick upper bound as exact start.
**Persistence authorisation — owner:** approve the separate sidecar unit, BOOT capability allocation and new format contract before implementation. Recommended answer: use section D unchanged; do not hide persistence changes inside U-I or expand into server/UI work.
**Qualification authority — advisers, then owner for machines/release:** freeze binary-specific role/ownership/inactivity qualification manifests and the four live-cell acceptance records. Recommended answer: unknown qualification refuses the affected proof; role names, release prefixes and current code whitelists are insufficient.
**Equality of enable and terminal timestamps — advisers:** recommended answer: permit equality only with retained operation ordering proving successful enable before termination; equality alone remains insufficient.
**Exclusive TEXT and authentication deadlines — advisers:** recommended answer: retain shared TEXT and unknown production deadlines until separately scoped qualification supplies the missing evidence. Keep A19/N16 out of production-qualified totals meanwhile.
**Shutdown budget — owner:** recommended default is the one-second total deadline specified above; any different deployment budget must remain one absolute bound, never multiplied by unresolved population.