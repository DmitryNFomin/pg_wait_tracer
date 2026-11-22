Process-slot population rule: contract delta against origin/master 7815defb. The rule text is an INFERRED normative proposal. Code facts were read at producer tip 39f159d2 (local branch agent/988-discovery-producer; the EXIT flags and the transport stack exist only there and on the other 988 branches, not on master). "pgsrc" = scratchpad/a2go/disc/pgsrc-fable (scratch copies of PostgreSQL source, not the shipped binaries). M = contracts/merge-v1/CONTRACT.md, A2 = contracts/coverage-a2-v1/CONTRACT.md.

WHERE THE SHAPE CANNOT BE WRITTEN AS STATED

(a) The proof can be written, but only for PostgreSQL 13 in exact_perf, and the post-arm read alone is not enough. The bootstrap watch is armed after the fork. The post-arm read follows SET_BPF (VERIFIED 39f159d2 src/backend.c:1161,1212-1221; src/perf_event.c:60-110, opened enabled and SET_BPF is called afterwards). MyProc is NULL both before InitProcess and after ProcKill (VERIFIED pgsrc pg13-proc.c:67,339,561,899,974). On 17/18 my_wait_event_info points at private storage both before init and after the reset in ProcKill (VERIFIED pgsrc pg18-wait_event.c:40,351,363; pg18-proc.c:540,710,1034,1100). So a NULL or private read cannot tell "not yet" from "already done", and that also holds when EXIT has a fire. Take a backend that took its slot before the arm. Its ProcKill NULL write can fire after the arm, and the read can then return NULL. EXIT is then (1,0), so (1,0) plus a NULL read describes a slot holder too.

The proof therefore needs one more ordered fact the tracer already has: a lifetime-bound state read, made after the pointer read, showing bootstrap_fired=0 and init_stored=0. The state carries both flags (VERIFIED 39f159d2 bpf.c:1334-1337,1350-1362; EXIT copies them at 1548-1551). Every write after that state read is observed, and MyProc was NULL when the read began. With INIT_STORED=0 no fire stored a non-NULL value, so the only NULL writer left is the refusal at pg13-proc.c:339.

On 17/18 a refusal writes nothing (VERIFIED pgsrc pg18-proc.c:436-438). No never-slot proof at EXIT exists there. Flags (0,0) are unknown on every version.

Today's post-arm read goes by PID through /proc/<pid>/mem (VERIFIED 39f159d2 src/discovery.c:795-812). The pinned /proc handle is closed before that read (VERIFIED backend.c:1188). The read must be bound to the lifetime, or a reused PID could supply the NULL. The read is currently kept only for a diagnostic line at EXIT (VERIFIED backend.c:1478-1499).

(b) This cannot be written for connections from existing evidence. The same ABA means a live NULL or private read proves nothing. In exact_perf the INIT is stored in the state and not published to the merge before EXIT (VERIFIED bpf.c:1350-1363). A released child's later slot entry is therefore not a merge input, and the merge cannot hold a frontier for it. (b) can be written for qualified permanent no-slot roles. The role read through the same binding removes the ABA, because such a role never runs InitProcess or ProcKill. The discovery producer already reads this role (VERIFIED 39f159d2 src/discovery_producer.c:960-962). Written that way, (b) also stops a re-forked PostgreSQL 13 archiver, statistics collector or logger, or a PostgreSQL 17/18 logger, from holding FORK and the frontier until it exits (M:187 today, VERIFIED).

(c) Written as stated, with nothing changed.

(d) Under the narrowed (b) no disposition is needed. A slot write by a qualified permanent role contradicts its qualification and is a FAULT. That uses no gap record and no new kind. The first reading's hard gap [init_ts, exit_ns) would need two things. First, slot-entry accounting before every frontier, for the reason given under (b). Second, a gap declared below an already published frontier, which existing gaps support only as revocation. Both stay open.

(e) Written as stated. One addition is needed: DECISION 39's new prohibition leaves the PostgreSQL 17/18 logger with no route. Decision 41 would then never close the startup gap while a logger runs. So 37(c) must also admit the 17/18 logger (the 37(c), three DECISION 39 and one DECISION 43 edits below). "N backends not traced" is a contract string (A2:96-97, with A2:138 making the words literal; VERIFIED). It is not yet implemented in src/ or web/ on origin/master, the three 988 branches or agent/933-a2-codec (SEARCHED: git grep 'backends not traced' on those five refs, src and web; NOT SEARCHED: other branches and tests/). Unknowns are always inside N, so the change is unconditional.

DELTA SLOT

**contracts/merge-v1/CONTRACT.md, DECISION 30. APPEND AFTER the exact sentence:**

> Q-mode discovery remains unqualified.

**TEXT:**

> Population (owner decision, issue 988, 2026-11-21). On every supported version and on both provenance routes, a process is a backend only while it holds a PostgreSQL process slot. A process that takes a slot through InitProcess or InitAuxiliaryProcess is a backend, auxiliary processes included. A wait word, a write to it, an attachment, authentication or appearance in a shared-memory view does not by itself make a process a backend. A lifetime whose slot history the tracer cannot establish counts as holding a slot for its whole lifetime. It is never dropped, never classified as not a backend and never VOIDed on that ground alone; on the fork route §4 gives it the ordinary UNTRACED [fork_ns,exit_ns). Only §4's slot-history proofs and Decision 37(c) establish that a lifetime holds no slot. This rule adds no capture-file record kind and changes no interval endpoint.

**contracts/merge-v1/CONTRACT.md, DECISION 37(c). REPLACE:**

> (c) **No-wait-word route.** This route applies only to a bound discovery lifetime whose qualified permanent role cannot acquire or use a wait word. A NULL pointer alone does not establish that role.

**WITH:**

> (c) **No-wait-word route.** This route applies only to a bound discovery lifetime whose qualified permanent role cannot acquire a PostgreSQL process slot. On PostgreSQL 13 such a role can neither acquire nor use a wait word. On PostgreSQL 17 and 18 its process-private wait word is outside the population and is never observed for numbers. A NULL pointer or a process-private pointer target alone does not establish that role.

**contracts/merge-v1/CONTRACT.md, DECISION 39. APPEND AFTER the exact sentence:**

> Decision 37(c) instead qualifies the complete pointer variable and its permanent-role proof; it does not qualify a NULL target as a wait-word address.

**TEXT:**

> On PostgreSQL 17 and 18 the binary-specific qualification selects the watched pointer variable. It is either MyProc, whose required post-enable observation is NULL, or my_wait_event_info, whose required post-enable observation is its process-private target. Decision 37(c)'s NULL observation means that value. With my_wait_event_info, every subsequent fire contradicts the permanent-role proof and follows Decision 43.

**contracts/merge-v1/CONTRACT.md, DECISION 39. REPLACE:**

> A permanent auxiliary dummy may qualify.

**WITH:**

> A process-private wait word, including a permanent dummy, never qualifies. A private wait word alone never qualifies a process that holds no slot (the logger on PostgreSQL 17 and 18); such a process can resolve only through Decision 37(c) or 40.

**contracts/merge-v1/CONTRACT.md, DECISION 39. REPLACE:**

> The qualification establishes that the identified role cannot acquire or use a wait word during the remaining lifetime.

**WITH:**

> The qualification establishes that the identified role cannot acquire a PostgreSQL process slot during the remaining lifetime; on PostgreSQL 13 this includes that it cannot acquire or use a wait word.

**contracts/merge-v1/CONTRACT.md, DECISION 43. APPEND AFTER the exact sentence:**

> They are not backends: they are not listed and not counted.

**TEXT:**

> On PostgreSQL 17 and 18 the logger never holds a PostgreSQL process slot either; the same treatment applies to it.

**contracts/merge-v1/CONTRACT.md, §4 A2 refusals. REPLACE:**

> In particular, fired-but-not-stored, absent disposition, successful-enable/clear-EVER_LIVE races and observation-only death receive no exception.

**WITH:**

> In particular, absent disposition, successful-enable/clear-EVER_LIVE races and observation-only death receive no exception. Fired-but-not-stored receives none either, except the proved PostgreSQL 13 refusal under "Slot history of fork-born lifetimes" below; an attempted acquisition or a max_connections label alone supplies no exception.

**contracts/merge-v1/CONTRACT.md, §4 A2 hold. REPLACE:**

> For a new lifetime without another valid creation/attachment proof, hold FORK and its release batch and frontier until the qualifying EXIT is eligible for release.

**WITH:**

> For a new lifetime without another valid creation/attachment proof or a recorded live no-slot proof (below), hold FORK and its release batch and frontier until the qualifying EXIT is eligible for release.

**contracts/merge-v1/CONTRACT.md, §4. APPEND AFTER the exact sentence:**

> A FORK+INIT-created lifetime that meets the A2 proof is an A2 lifetime and receives UNTRACED; the DECISION 25 creation route does not displace it.

**TEXT:**

> **Slot history of fork-born lifetimes (owner decision, issue 988, 2026-11-21).** These rules apply in exact_perf only. They use only the bootstrap pointer watch, the fork-seeded state and the EXIT flags already kept, and add no kernel probe, per-connection trap or capture-file record kind. The slot pointer is MyProc on PostgreSQL 13 and my_wait_event_info on 17 and 18. Its no-slot value is NULL on 13 and the process-private target on 17 and 18.
>
> A *post-arm read* is a successful read of the complete slot pointer, through a binding to the same lifetime (Decision 33), made after the bootstrap watch is enabled with its handler attached. A failed or PID-only read is not a no-slot value.
>
> A *post-arm state read* is a successful lifetime-bound read of the fork-seeded state showing BOOTSTRAP_FIRED=0 and INIT_STORED=0. It counts only if made after a post-arm read that returned the no-slot value; a state read made before that pointer read proves nothing.
>
> (i) **Refusal proved at EXIT, PostgreSQL 13 only.** A lifetime is proved never to have held a slot when all of the following hold:
>
> - it satisfies A2 conditions (a), (b), (d) and (e);
> - it has a post-arm read returning NULL, followed by a post-arm state read;
> - its EXIT carries (BOOTSTRAP_FIRED,INIT_STORED)=(1,0) with init_ts=0;
> - no counter changed.
>
> Release FORK under the hold rule above, then EXIT, with no UNTRACED, INIT or INIT-loss increment. The lifetime is not an A2 omission and counts once in its fork cohort's N_k. On PostgreSQL 17 and 18 a refusal writes no slot pointer, so this proof does not exist there.
>
> (ii) **Live no-slot proof, qualified permanent roles only.** This proof applies to a fork-born lifetime that is still unattached. It consists of a post-arm read returning the no-slot value, then a post-arm state read, then a read of the role discriminator through the same binding showing a member of Decision 39's qualified permanent-role set for this binary, then a completed producer/lifecycle fence at e with unchanged counters. Deliver the witness as control `CONFIRM_NO_SLOT(pid,g,e,witness,d,C)`. It consumes no transport position and supplies no source boundary.
>
> Once the proof is recorded, release FORK under ordinary validation as soon as it is eligible. The lifetime holds neither later frontiers nor its fork cohort. Keep the bootstrap watch armed until EXIT. At the matching EXIT with BOOTSTRAP_FIRED=0 and INIT_STORED=0, release EXIT with no UNTRACED.
>
> Any later evidence of a slot pointer write refutes the proof: BOOTSTRAP_FIRED or INIT_STORED in the fork-seeded state or in EXIT. Report `FAULT(unknown_evidence,d,C)` at that observation and release nothing further for the lifetime. Take the write time as the stored init_ts, or as e when no init_ts is retained, and mark and report invalid every epoch whose published frontier is at or above it.
>
> A connection or any other process without a qualified permanent role has no live no-slot proof.
>
> (iii) **Unknown slot history.** Every other never-attached lifetime keeps its existing treatment. A lifetime meeting the A2 proof receives UNTRACED [fork_ns,exit_ns) in its fork cohort, including flags (0,0) after a post-arm no-slot read: its slot may have been taken and released before the watch was armed. Any other history retains its existing fault. Missing proof never drops a lifetime or classifies it as not a backend.
>
> Add input `bool never_slot_proved` to `pgwt_merge_record`. It is meaningful only on EXIT, defaults to false, and is set only for (i).

**contracts/coverage-a2-v1/CONTRACT.md, Scope and policy. APPEND AFTER the exact sentence:**

> Count every admitted never-attached lifetime, irrespective of an older diagnostic's "lost" classification.

**TEXT:**

> A lifetime that merge-v1 §4 proves never held a PostgreSQL process slot, either by a refusal proved at EXIT or by a recorded live no-slot proof, is not an omission. It contributes no pair and nothing to U_k, and it still counts as an observed birth in N_k, including against N_qual. A lifetime whose slot history is unknown remains an omission when merge-v1 admits it under A2.

**contracts/coverage-a2-v1/CONTRACT.md, finality. REPLACE:**

> A cohort finalizes only when qualified source boundaries establish complete birth accounting through J_k.end and every birth has either proved attachment or a reconciled terminal disposition.

**WITH:**

> A cohort finalizes only when qualified source boundaries establish complete birth accounting through J_k.end and every birth has proved attachment, a reconciled terminal disposition or a recorded live no-slot proof under merge-v1 §4. A live no-slot lifetime does not keep its cohort unfinished. A later refutation of its proof is a contradiction under the immutability rule below.

**contracts/coverage-a2-v1/CONTRACT.md, UI text. REPLACE:**

> “N backends not traced; at most X backend-time omitted.”

**WITH:**

> “up to N backends not traced; at most X backend-time omitted.”

**contracts/coverage-a2-v1/CONTRACT.md, status. APPEND AFTER the exact sentence:**

> This is not a relative DB Time, wait-attribution, execution-count or latency-distribution error bound.

**TEXT:**

> "up to" is required because an omitted lifetime whose slot history is unknown may never have held a PostgreSQL process slot; N counts omitted lifetimes, not proved backends.

ORACLE ROWS

The merge rows SL1-SL7 and the coverage rows SL8-SL11 go into contracts/coverage-a2-v1/oracle-a2/, which holds the M and C rows and their notation. The discovery rows SL12-SL13 go into contracts/merge-v1/oracle-discovery/, which holds the N rows and the NW_D notation. Inputs go in ROWS.md. Expected outputs go in a new file contracts/coverage-a2-v1/oracle-a2/EXPECTED-slot.md (oracle-a2 has no EXPECTED file today; VERIFIED by git ls-tree) and in oracle-discovery/EXPECTED.md.

**contracts/coverage-a2-v1/oracle-a2/ROWS.md. APPEND AFTER the exact line:**

> M63e: B,X; lifetime-bound perf_event_open(disabled) failed ESRCH, but this kernel's operation/error mapping is unqualified.

**TEXT:**

```text

SLOT ROWS (process-slot population; merge fixture above, with these additions)

Capture mode is exact_perf; rows apply in N and Q. The PostgreSQL major is 13 unless a row states 18.
ARM is the bootstrap pointer watch on D's task, opened enabled with its handler attached, succeeding before RD. It is not a wait-watchpoint enable or an attachment attempt; attachment history otherwise contains no attempts.
RD(v) is a successful read of the complete slot pointer through a binding to D's lifetime, after ARM. v=NULL means MyProc is NULL (PostgreSQL 13); v=PRIV means my_wait_event_info targets process-private storage (PostgreSQL 18).
ST0 is a successful lifetime-bound read of D's fork-seeded state, at the stated position, showing BOOTSTRAP_FIRED=0 and INIT_STORED=0. ST(f,s) shows BOOTSTRAP_FIRED=f and INIT_STORED=s instead.
ROLE(r) is a lifetime-bound read of D's role discriminator after ST0. The fixture supplies qualified permanent no-slot role entries for the PostgreSQL 13 logger (13) and the PostgreSQL 18 logger (17); these are synthetic qualification inputs.
FENCE(t) is a completed producer/lifecycle fence at semantic instant t with unchanged counters. When RD, ST0, ROLE and FENCE together form a complete live no-slot witness, the adapter delivers CONFIRM_NO_SLOT(30,1,t,witness,20,C0) at FENCE.
SLOT(t) is a sweep read of D's fork-seeded state showing BOOTSTRAP_FIRED=1, INIT_STORED=1 and init_ts=t.
ARM, RD, ST, ROLE, FENCE, SLOT and CONFIRM_NO_SLOT are adapter observations or controls; they consume no transport source position.
X70 abbreviates EXIT_D70@L with the default flags. Report, for each variant, the output sequence, the final epoch state, any epoch invalidation, and D's contribution to its fork cohort's N and U.

SL1: B,ARM,RD(NULL),ST0,X(flags=(1,1,0,0),i=0).
SL2a: B,ARM,ST0,RD(NULL),X(flags=(1,1,0,0),i=0); the state read precedes the pointer read.
SL2b: B,ARM,RD(NULL),ST(1,0),X(flags=(1,1,0,0),i=0).
SL2c: B,ARM,then the slot-pointer read fails ENOENT and returns no value,ST0,X(flags=(1,1,0,0),i=0).
SL2d: SL1, except the slot-pointer read uses the PID only, without a lifetime binding.
SL2e: PostgreSQL 18. B,ARM,RD(PRIV),ST0,X(flags=(1,1,0,0),i=0).
SL3a: B,ARM,RD(NULL),ST0,X.
SL3b: PostgreSQL 18. B,ARM,RD(PRIV),ST0,X.
SL4: B,ARM,RD(NULL),ST0,X(flags=(1,1,1,0),i=11); the producer protocol requires no separate INIT publication.
SL5a: B,ARM,RD(NULL),ST0,ROLE(13),FENCE(14),S(15),S(60),X70,S(71); this is the complete input sequence.
SL5b: PostgreSQL 18. SL5a with RD(PRIV) and ROLE(17).
SL6a: SL5a with ROLE(client backend); no permanent-role proof exists.
SL6b: SL5a without FENCE(14).
SL6c: SL5a with ROLE(archiver, numeric value 11, no qualified permanent-role entry).
SL7a: SL5a with SLOT(40) inserted between S(15) and S(60).
SL7b: SL5a with SLOT(40) inserted between S(60) and X70.
SL7c: SL5a with X70 replaced by EXIT_D70@L(flags=(1,1,1,0),i=40); no SLOT observation.
```

**contracts/coverage-a2-v1/oracle-a2/ROWS.md. APPEND AFTER the exact line:**

> C24: J_0 certificate says N=2,U=1; Q(100,106); complete retained FORK evidence establishes three distinct births, with the other two attached; W=[0,1000).

**TEXT:**

```text

SL8: J_0:N=201, of which the birth at 300 has a refusal proved at EXIT under merge-v1 §4 (i) and 198 others have proved attachment; Q(100,106),Q(103,109); N_qual remains 200; W=[0,1000).
SL9: J_0:N=200, of which the birth at 300 has a refusal proved at EXIT and 197 others have proved attachment; Q(100,106),Q(103,109); W=[0,1000).
SL10: J_0 has complete birth accounting with one birth, at 100, which has a recorded live no-slot proof under merge-v1 §4 (ii) and is still alive with no EXIT. There are no other births, no omissions and no other unfinished reconciliation; no J_0 certificate is stipulated. Derive whether J_0 finalizes, its certificate, and the result for W=[0,1000).
SL11: C1's exact evidence and W, requested through an enabled UI route; give the complete UI text.
```

**contracts/merge-v1/oracle-discovery/ROWS.md. APPEND AFTER the exact line:**

> | N16 | `TRY_NW_D(4;role=authenticating client, permanent-role proof absent, qualified remaining authentication deadline=14), AOP(deadline 14 expires; no initialization or matching terminal evidence), S(16)`. |

**TEXT:**

```text
| SL12a | PostgreSQL 18. `NW_D6{qualified PG18 logger role 17; the qualification selects MyProc as the watched pointer; pinned NULL read of MyProc after 6; D's process-private wait word is non-NULL and is written during qualification}, S(6), S(8)`. The fixture supplies an independently qualified permanent-role entry for PG18 logger 17. Inspect backend listing, population contributions and any numerical admission. |
| SL12b | SL12a, except the qualification selects my_wait_event_info as the watched pointer and the pinned read after 6 observes its process-private target. |
| SL13 | PostgreSQL 18. D is the qualified PG18 logger role 17. `B_D5{e=6; the armed and resolved address is D's process-private wait word}, AD_D6{route=b; asserts a qualified address}, S(8)`. All other bracket and admission facts hold. |
```

**contracts/coverage-a2-v1/oracle-a2/EXPECTED-slot.md. CREATE with TEXT:**

```text
Expected outputs for the SLOT rows (INFERRED design answers; not frozen oracle answers). U(f,e,r)=UNTRACED(30,1,f,e,r). Outputs are identical in N and Q.

| ID | Expected output and state |
|---|---|
| SL1 | Hold until S(13)'s L boundary, then `FORK10 → EXIT12 → F12`. No UNTRACED, INIT or INIT-loss increment; C0; epoch active. D adds 1 to J_0's N and 0 to U; it is not listed. |
| SL2a | `VOID20[C0,unknown_evidence]`; no batch output (M49's result). The state read before the pointer read proves nothing. |
| SL2b | `VOID20[C0,unknown_evidence]`; no batch output. |
| SL2c | `VOID20[C0,unknown_evidence]`; no batch output; a failed read is not NULL. |
| SL2d | `VOID20[C0,unknown_evidence]`; no batch output; a PID-only read is no proof. |
| SL2e | `VOID20[C0,unknown_evidence]`; no batch output; the refusal proof is PostgreSQL 13 only. |
| SL3a | `FORK10 → U(10,12,10) → EXIT12 → F12`; unknown slot history; D adds 1 to N and 1 to U. |
| SL3b | `FORK10 → U(10,12,10) → EXIT12 → F12`; as SL3a. |
| SL4 | `FORK10 → U(10,12,10) → EXIT12 → F12`; no synthetic INIT; slot holder never attached. |
| SL5a | No output before S(15)'s L boundary, which gives `FORK10 → F14`; S(60) gives `F59`; X70 is staged; S(71) gives `EXIT70 → F70`. Total `FORK10 → F14 → F59 → EXIT70 → F70`. No UNTRACED, INIT or INIT-loss increment; epoch active; D adds 1 to N and 0 to U; it is not listed. |
| SL5b | As SL5a. |
| SL6a | No output through S(60) (FORK10 holds its batch and frontier); at S(71)'s L boundary `FORK10 → U(10,70,10) → EXIT70 → F70`. D adds 1 to N and 1 to U. |
| SL6b | As SL6a. |
| SL6c | As SL6a. |
| SL7a | `FORK10 → F14 → VOID20[C0,unknown_evidence]` at SLOT(40); epoch suspended; no epoch invalidation (40 > 14); later inputs give no output; no UNTRACED, EXIT output or attachment. |
| SL7b | `FORK10 → F14 → F59 → VOID20[C0,unknown_evidence]` at SLOT(40); mark and report the epoch invalid (F59 ≥ 40); later inputs give no output. |
| SL7c | `FORK10 → F14 → F59 → VOID20[C0,unknown_evidence]` at the arrival of EXIT_D70; mark and report the epoch invalid (F59 ≥ 40); EXIT70 is not released; S(71) gives no output. |
| SL8 | Unavailable(untraced_unqualified); note 2/12 backend-ms. |
| SL9 | Numbers retained; note 2/12 backend-ms; J_0 certificate N=200, U=2. |
| SL10 | J_0 finalizes with certificate N=1, U=0; numbers retained; zero affected lifetimes, no note. |
| SL11 | Numbers retained; note 2/12 backend-ms; UI text exactly “up to 2 backends not traced; at most 12 ms backend-time omitted.” |

Mutants (design, not built):

| Mutant | Fails |
|---|---|
| Keep fired-but-not-stored VOID even with the ordered proof | SL1 |
| Emit UNTRACED for a proved refusal, or drop its birth from N_k | SL1, SL8 |
| Accept a state read made before the pointer read, a fired state, a failed read or a PID-only read as the proof | SL2a, SL2b, SL2c, SL2d |
| Apply the refusal proof on PostgreSQL 17/18 | SL2e |
| Treat a post-arm no-slot read with flags (0,0) as never-slot | SL3a, SL3b |
| Let the post-arm read evidence remove a slot holder's UNTRACED | SL4 |
| Hold a live permanent-role FORK until EXIT | SL5a, SL5b |
| Release a live FORK without a permanent-role proof, without the fence, or on an unqualified role value | SL6a, SL6b, SL6c |
| Treat a later slot write as an ordinary late attachment or a gap, or skip invalidation of a frontier at or above it | SL7a, SL7b, SL7c |
| Exclude proved never-slot births from N_k (N_qual check) | SL8 |
| Count a proved never-slot lifetime as an omission | SL9 |
| Keep a cohort unfinished while its live no-slot lifetime runs | SL10 |
| Keep the old literal without "up to" | SL11 |
```

**contracts/merge-v1/oracle-discovery/EXPECTED.md. APPEND AFTER the exact line:**

> | N16 | `F15`; no ADMIT or bound; `G(open), P=1+0, census=known`; attempts 0. Report exceeded waiting limit; deadline expiration discharges nothing. |

**TEXT:**

```text
| SL12a | `F5 → F7`; at `S(6)`, no bound, `G(open), P=1+0`; at `S(8)`, `b_D=6, C_close=6, G(6), P=0+0, census=known`. Attempts 0. No backend listing, private-word measurement or role-support publication; no contribution to `N_k`, `U_k` or numerical numerators. |
| SL12b | Same as SL12a. |
| SL13 | `VOID20[C0,unknown_evidence]`; no accepted admission, bound or frontier; `G(open), P=1+0, census=known`; D is not listed. |
```

**contracts/merge-v1/oracle-discovery/EXPECTED.md. APPEND AFTER the exact line:**

> | Omit the endpoint increment for D20's uncertified fresh-record anchor at C_close | H13 |

**TEXT:**

```text
| Admit the PG17/18 logger numerically through its private wait word | SL13 |
| Require a NULL my_wait_event_info observation on 17/18, leaving the logger without a route | SL12b |
| List or count the PG17/18 logger | SL12a, SL12b |
```

EXISTING ROWS: no expected output changes under this delta.

| Row | Second reading's claim | Here | Reason |
|---|---|---|---|
| M44 (0,0) | needs slot status | unchanged `U(10,12,10)` | (0,0) is unknown slot history; there is no new kernel bit (lead's rule); §4 (iii). |
| M48 (0,0), EXIT at 10 | needs slot status | unchanged `U(10,10,10)` | Same as M44. |
| M57a, M63a, M63b (0,0) with admissible failures | needs slot status | unchanged | Same as M44; ESRCH/ENOENT before the arm give no post-arm evidence at all. |
| M49 (1,0) | refusal becomes ordinary | unchanged VOID | M49 supplies no post-arm read or post-arm state read. Without them (1,0) also describes a ProcKill write by a slot holder armed late. The new disposition needs SL1's evidence. |
| M45, M46, M56, M62a/b, M63d | none named | unchanged | Slot holders or (0,0); §4 (iii). |
| first reading's list (M45...M63d) | changed only under the draft | unchanged | Those changes came from the draft's entry-time interval; this delta keeps [fork_ns,exit_ns). |
| C1-C24 | none | unchanged | Q pairs remain omissions. C18e's recorded answer does not quote the UI words; its rendered text becomes "up to 3 backends not traced; at most 18 ms backend-time omitted." (SL11 pins the literal). |
| N1-N16, R, H, J, U, V, A rows | none | unchanged | The PG13 semantics of 37(c) and 39 are kept by the "on PostgreSQL 13 this includes" wording. No existing discovery row uses PG18 or a private wait word (SEARCHED: grep -i 'dummy\|PG18\|private\|logger' in oracle-discovery/ROWS.md at origin/master; hits only lines 35-36, PG13 logger defaults; NOT SEARCHED: EXPECTED.md text for implicit PG18 assumptions). |

TASK

TASK for independent answerers: process-slot population delta

Read only ../CONTRACT.md (merge-v1) and coverage-a2-v1/CONTRACT.md with DELTA SLOT applied, the ROWS.md files of oracle-a2 and oracle-discovery with the SLOT rows appended, and this task. Do not read EXPECTED.md, EXPECTED-slot.md, earlier answer sets, comparisons, product code, product output, tests or review logs. Derive every answer from the contract text alone. Do not implement or change the contract. If the contract does not determine an answer, write `UNDECIDABLE | <the missing rule, in one sentence>` instead of guessing.

Each row and variant is independent. Expand S(w) into its four boundaries and evaluate release after every primitive input, including adapter observations and controls.

For every merge row SL1-SL7 (every variant) give: the exact output sequence (FORK, INIT, UNTRACED, EXIT, FRONTIER, VOID with time, vector and reason) in publication order; the input at which each hold, release, fault and invalidation occurs; the final epoch state; any epoch invalidation with the frontier it disproves; and D's contribution to its fork cohort's N and U. For each, state which of §4's slot-history cases (i), (ii) or (iii) applies and which of its facts are present or missing.

For SL1 and SL2a-SL2e, state exactly which ordering or binding fact makes the refusal proof hold or fail. For SL5-SL7, state when the live no-slot proof is recorded, when FORK becomes releasable, and what the refutation does to frontiers already published.

For every coverage row SL8-SL11 give: status (numbers, or Unavailable with reason); the note values, or "no note", or "values withheld"; the J_0 certificate where finality is asked; and for SL11 the complete UI text.

For SL12a, SL12b and SL13 give the merge stream, the committed bound, the startup-gap state, P, census, attempts, backend listing and population contributions after S(6) where present and at the end, in the existing N-row format.

Also re-derive M44, M48, M49 and M57a as controls, and state whether DECISION SLOT changes any of them.

Supply at least one targeted mutant per SL row: the input and the exact assertion that tells the mutant apart from the contract result. This is a design answer; do not claim any mutant was built or run.

End with a section AMBIGUITIES listing every place where you had to choose between two readings, or state that there was none. Agreement does not establish implementation or platform qualification.

OPEN FOR THE AUTHOR

1. A live no-slot proof for connections (silent clients, health checks before authentication). It cannot be written from existing evidence (ABA, and INIT is not a merge input before EXIT; VERIFIED above). Written meanwhile: no such proof; FORK holds until EXIT as today (M:187). The stall is bounded by PostgreSQL's authentication_timeout for a client that sends nothing (INFERRED, not read in pgsrc).
2. The (d) disposition. Written meanwhile: not needed, because (ii) is restricted to permanent roles and a slot write is a FAULT. The first reading's [init_ts, exit_ns) gap waits for slot-entry accounting before frontiers and a rule for gaps below a published frontier.
3. (1,0) with unchanged counters but without the ordered proof (M49). The second reading would make it ordinary. Written meanwhile: VOID as today, because it is not unknown history alone (it also covers a lost INIT store).
4. The pointer watched by route (c) on PostgreSQL 17/18. Written meanwhile: either MyProc (NULL) or my_wait_event_info (private target), chosen by binary qualification. Both are rows (SL12a/b).
5. The refutation time when no init_ts is retained (a NULL-only fire on 13). Written meanwhile: e. This is conservative: every frontier at or after the proof's fence counts as disproved.
6. The reader and UI listing rule. Written meanwhile: a FORK/EXIT pair without UNTRACED and without TRANS is not listed (INFERRED; the reader's listing rule was NOT SEARCHED). The implementation unit must verify this or the owner's "not listed" fails.
7. Fable's open points. Whether one slot episode per lifetime holds is not needed: any slot write after (ii) refutes. init_ts as a cohort key is not used: there is no entry-time cohort. The cheap K3 rate check, counting never-attached lifetimes with init_ts − fork_ns > 1 s in the a2-loss50 attach-timeline records, is unmeasured.
8. Owner wording. Option "b" said health checks, cancel requests and silent clients are "not a backend". Under this delta, the unproved ones are counted as "up to N backends not traced". The README must say so (Fable item 31). This is not a new decision, but the owner should read it before the tag.

IMPLEMENTATION UNITS (not a brief)

- Producer (src/backend.c, src/discovery.c, src/discovery_producer.c; no BPF change):
  - Bind the post-arm read to the lifetime. Today it is PID-only and the pinned target is closed first (VERIFIED backend.c:1188,1212-1221; discovery.c:795-812).
  - Add the ordered post-arm state read through read_init_state (existing; VERIFIED backend.c:1236-1248).
  - For a still-unattached fork-born child, read the role through the same binding using the existing backend_type_addr (VERIFIED discovery_producer.c:960). Keep the bootstrap watch armed.
  - After CONFIRM_NO_SLOT, a stored INIT must report refutation instead of attaching through pgwt_handle_init (INFERRED call path; VERIFIED attach_stored_init → pgwt_handle_init at backend.c:1250-1252).
  - Logger: remove the PG17/18 B_LOGGER private-word acceptance (VERIFIED discovery_producer.c:957-963 and 1130) and give it route (c) with the 17/18 pointer rule.
  - Collides with agent/988-discovery-producer (39f159d2), which changes backend.c, bpf.c and discovery_producer.c (VERIFIED git diff --name-only). Fold the logger part into its next round, as in the first reading. The rest waits for it.
- Merge (src/core/merge.c, merge_adapter.c, pgwt_merge.h):
  - EXIT input never_slot_proved, the (i) disposition, and the fired-not-stored exception.
  - Control CONFIRM_NO_SLOT, the hold exception, and refutation with epoch invalidation.
  - Collides with agent/988-discovery-merge (c4708558), which changes the same files and adds core/pgwt_a2_population.h (VERIFIED). It goes after that branch lands.
- Coordinator (src/merge_capture.c, discovery_integration.c, the A2 ledger):
  - Carry the post-arm facts to EXIT and deliver CONFIRM_NO_SLOT.
  - Count proved never-slot births in N_k and not U_k.
  - Finalize cohorts with a live no-slot lifetime and treat refutation as a certificate contradiction.
  - Collides with agent/988-discovery-coordinator (018573f0), which changes merge_capture.c and discovery_integration.c (VERIFIED).
- Daemon, docs and UI:
  - Change the UI literal to "up to N backends not traced". Not yet implemented on the refs searched, so it collides with nothing.
  - Run an el8 box-check, because exact_perf is EL8-only (Decision 44, VERIFIED M:510).
- README text (from the first reading's item 4, corrected for this delta): "A process is counted as a backend from the moment it takes a PostgreSQL process slot until it exits; auxiliary processes such as the checkpointer, background writer and WAL writer take one and are counted. The logger never takes a slot on any version, and on PostgreSQL 13 neither do the statistics collector and the archiver: they are not backends and are not listed or counted. A connection the tracer can prove never took a slot (on PostgreSQL 13, a connection refused at max_connections after the tracer had armed its watch) is not a backend and is not listed. A process that exits before the tracer can tell whether it took a slot, which includes most connect-and-close probes, cancel requests and early refusals, is counted as not traced; the coverage note then reads "up to N backends not traced". A health check that reaches authentication, such as pg_isready, takes a slot and is counted." The pg_isready clause is INFERRED (Fable; the measurement file was not found under scratchpad/a2go) and must be measured before it ships.

ANCHOR CHECKS (each: `git show origin/master:<file> | grep -F -o -- '<anchor>' | wc -l`, all = 1)

- M "Q-mode discovery remains unqualified." → 1 (line 302)
- M "(c) **No-wait-word route.** This route applies only to … does not establish that role." → 1 (396)
- M "Decision 37(c) instead qualifies the complete pointer variable … as a wait-word address." → 1 (428)
- M "A permanent auxiliary dummy may qualify." → 1 (432)
- M "The qualification establishes that the identified role cannot acquire or use a wait word during the remaining lifetime." → 1 (436)
- M "They are not backends: they are not listed and not counted." → 1 (500)
- M "In particular, fired-but-not-stored, … receive no exception." → 1 (177)
- M "For a new lifetime without another valid creation/attachment proof, hold FORK … eligible for release." → 1 (187)
- M "A FORK+INIT-created lifetime that meets the A2 proof … does not displace it." → 1 (192)
- A2 "Count every admitted never-attached lifetime, irrespective of an older diagnostic's "lost" classification." → 1 (9)
- A2 "A cohort finalizes only when … or a reconciled terminal disposition." → 1 (37)
- A2 "“N backends not traced; at most X backend-time omitted.”" → 1 (97)
- A2 "This is not a relative DB Time, wait-attribution, execution-count or latency-distribution error bound." → 1 (99)
- oracle-a2/ROWS.md "M63e: B,X; lifetime-bound perf_event_open(disabled) failed ESRCH, …" → 1
- oracle-a2/ROWS.md "C24: J_0 certificate says N=2,U=1; …" → 1
- oracle-discovery/ROWS.md "| N16 | `TRY_NW_D(4;role=authenticating client, …), S(16)`. |" → 1
- oracle-discovery/EXPECTED.md "| N16 | `F15`; no ADMIT or bound; …" → 1
- oracle-discovery/EXPECTED.md "| Omit the endpoint increment for D20's uncertified fresh-record anchor at C_close | H13 |" → 1
- oracle-a2/EXPECTED-slot.md: absent at origin/master (git ls-tree … | wc -l = 0), so CREATE.
