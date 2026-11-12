# Issue #871 — merge-stage contract v2

Proposed prototype contract, 2026-11-11. No implementation or experiments were run for this answer.
**Read** means inspected evidence; **Inference** means a conclusion; **Decision** defines proposed behavior.

Source aliases: `A` = supplied `astra-a1.md`; `C` = supplied `fable-a1.md`; `S` = supplied storage `evidence-v1.md`; `R` = `/Users/dmitryfomin/work/git/pg_wait_tracer/tests/results/session-20261111`; `K` = `R/m0c/el8/ksrc`. Repository paths refer to this master checkout.

## 0. Disposition of the challenge

- **Accept R1:** timestamps precede `emit_event()` reservation, and master shares its failure counter (`src/bpf/pg_wait_tracer.bpf.c:245`).
- **Accept R2, restricted to master:** INIT reserves before stamping and has no failure counter there (`src/bpf/pg_wait_tracer.bpf.c:665`).
- **Reject R3 as a diagnostic-branch claim:** revision `1464365d` counts INIT failure at `src/bpf/pg_wait_tracer.bpf.c:708`; daemon exposes it at `src/daemon.c:46`.
- **Accept R4:** map operations remain inside watchpoint execution; changing transport does not establish their reliability (`src/bpf/pg_wait_tracer.bpf.c:571,642`).
- **Accept R5:** EL8 invokes the debug notifier after NMI entry and before IRQ enable (`K/arch/x86/kernel/traps.c:896,933,944`).
- **Accept R6:** EL8 uses the NMI trylock and tracks BUSY reservations; that alone is not a complete boundary proof (`K/kernel/bpf/ringbuf.c:322,333`).
- **Accept R7:** marker/transition tie order affects attribution (`src/event_sort.h:57`).
- **Accept R8:** storage specifies global VOID, inclusive frontiers and no stored sequence column (`S:13,44,79,141`).
- **Accept R9:** the cited event totals and TPS agree with M0b; the TPS difference is approximately 2.49% relative to the lower result (`R/m0b/el8/r1/pgbench.out:14`, `r4/pgbench.out:14`).
- **Accept §1’s source split:** ring reservations and serialized per-CPU perf output require different boundary arguments.
- **Accept §1’s ring seal conditionally:** all relevant timestamps must follow reservation; the exit lifecycle path also needs attention, not only `emit_event()` (`src/bpf/pg_wait_tracer.bpf.c:722,812,817`).
- **Accept §1’s perf FIFO conditionally:** prove exclusive writers, same-CPU serialization, clock behavior and failure visibility; FIFO alone is insufficient.
- **Resolve §1’s upstream 6.8 question:** the notifier and synchronous perf/BPF path precede IRQ enable; the installed Ubuntu build still requires qualification.
- **Accept §1’s BPF clock and VOID recommendation:** read boundary time in BPF; regressions and seal violations must not terminate the daemon merely because the kernel permits them.
- **Accept §2’s measurement-first sequence decision:** count exact same-PID marker/transition ties; reject its unconditional timestamp-monotonicity premise.
- **Reject §3’s “INIT loss unmeasured” blanket statement:** diagnostic EL8 runs reported zero; accept that INIT under connection churn remains unmeasured (`R/m0b/el8/r1/daemon.stderr:14`, `r4/daemon.stderr:14`).
- **Accept §3’s map-operation concern:** inspect EL8 behavior and read failure counters; do not infer safety from zero transport loss.
- **Accept §4:** retain global VOID, require zero normal-load losses, and keep per-site diagnostics separate from the storage vector.
- **Accept §5’s five-repeat EL8 c16 gate:** that is the principal reproduction cell.
- **Accept §5’s sizing objection:** compare stall tolerance using actual transport bytes and hottest-CPU rates, not equal nominal buffer sizes.
- **Accept §5’s calibration recommendation; correct its characterization:** 3% is about the observed 2.49% spread, not inside it.
- **Accept §5’s EL8 no-tracer comparison:** report proposed/master and proposed/no-tracer on both hosts.
- **Accept §5’s wakeup-policy requirement:** freeze it before timing.
- **Accept §5’s freshness correction:** measure 10 ms, 100 ms and 1 s boundary intervals; freshness is not a gate.
- **Accept §5’s scope reduction:** prototype the transport, merge, real callbacks and diagnostics; defer the listed production lifecycle work.
- **Accept §6’s refusal correction:** kernel-permitted behavior causes VOID and recovery; exit is reserved for internal invariant breaches.
- **Accept §6’s per-CPU ringbuf experiment; reject unconditional “never fails”:** CPU-exclusive use removes the identified cross-CPU lock contention, but capacity, eligibility and other failures remain.
- **Accept §6’s Ubuntu clarification:** the perf prototype moves the wait-transition transport on both kernels.
- **Accept §6’s overclaim warning with the audit correction:** INIT movement is deferred; M0b identifies the failing emit site only for its measured workloads.

## 1. Evidence and prototype boundary

**Read:** EL8’s shared ring lock is tried once in NMI context (`K/kernel/bpf/ringbuf.c:322`). M0c recorded free-space `-EAGAIN` failures, including failures surviving four retries. [Issue #871 measurements](https://github.com/DmitryNFomin/pg_wait_tracer/issues/871).

**Read:** upstream 6.8 `exc_debug_user()` calls `notify_debug()` at line 990 before `local_irq_enable()` at line 993. The breakpoint notifier calls `perf_bp_event()` synchronously; perf routes the overflow to the attached BPF program. [traps.c:990](https://github.com/torvalds/linux/blob/v6.8/arch/x86/kernel/traps.c#L990), [hw_breakpoint.c:521](https://github.com/torvalds/linux/blob/v6.8/arch/x86/kernel/hw_breakpoint.c#L521), [core.c:9136](https://github.com/torvalds/linux/blob/v6.8/kernel/events/core.c#L9136), [core.c:9819](https://github.com/torvalds/linux/blob/v6.8/kernel/events/core.c#L9819).

**Inference:** for that upstream path, the watchpoint BPF program executes before interrupts are re-enabled. This resolves the upstream question, not vendor-build equivalence.

**Read:** upstream 4.18’s BPF clock uses the fast monotonic accessor, whose documented behavior permits backward observations. [helpers.c:101](https://github.com/torvalds/linux/blob/v4.18/kernel/bpf/helpers.c#L101), [timekeeping.c:376](https://github.com/torvalds/linux/blob/v4.18/kernel/time/timekeeping.c#L376).

**Decision:** prototype full mode, one boot, fixed CPU membership. Move wait-transition output to per-CPU perf buffers on Ubuntu and EL8; keep INIT, other lifecycle events and query text on `L`, and markers/terminal transitions on `E`.
The prototype exercises actual writer and live attribution callbacks through the merger. Its FRONTIER/VOID outputs may be an oracle/diagnostic stream; they do not make existing consumers evidence-aware.
A boundary is qualified only after the actual kernel, clock, producer-completion and memory-order arguments are documented. A zero-violation benchmark is not that proof.

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
**Read:** the local m0/m0b/m0c file inventory contains logs/metrics, not the trace payloads. M0b recorded `/var/tmp/m0b-20261111/r1/trace` on the gate host (`R/m0b/el8/m0.log:14`). **No tie count is claimed here.**

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
| 1 | `B12@P1, A10@P0, S(13)` | `A10 → B12 → F12`. |
| 2 | `B10@P0, A10@P1, S(11)` | `A10 → B10 → F10`; PID breaks independent ties. |
| 3 | `A10@P0, S(10), S(11)` | First `F9`, A held; then `A10 → F10`. |
| 4 | `A10@P0, A12@P0`; other sources boundary 20 | Newest perf record supplies P0 boundary 12: `A10 → F11`; A12 held. |
| 5 | `A10@P0, S0(100)`; P1/E/L remain at 0 | Hold; no frontier. Silence on P1 changes nothing. |
| 6 | Same setup, then P1/E/L boundary 100 | `A10 → F99`; no fabricated idle event. |
| 7 | E delivers `B12, A11`; then `S(13)` | `A11 → B12 → F12`; shared-ring inversion is accepted. |
| 8 | L delivers untimed `TEXTx`, then `A10@P0, S(11)` needing x | `TEXTx → A10 → F10`; no timestamp assigned to x. |
| 9 | Same PID/time: EXIT, terminal TRANS, INIT, FORK arrive reversed; `S(11)` | `FORK10 → INIT10 → TRANS10 → EXIT10 → F10`. |
| 10 | Same-source A enters X at 10, then leaves X at 10; `S(11)` | Preserve the two transitions; X is a distinct zero-duration wait; `F10`. |
| 11 | A’s `TRANS10@P0(q=4)` arrives before `MARKER10@E(q=3)`; `S(11)` | N: VOID, neither tied record released. Q: `MARKER → TRANS → F10`. |
| 12 | A’s second transition `10@P0(q=2)` arrives before first `10@P1(q=1)`; `S(11)` | N: VOID. Q: q1 then q2, preserving the zero-duration intermediate state, then `F10`. |
| 13 | A enters X at 4; `S(6)`; B also attached; loss detected at 9 | `A4 → F5 → VOID9`; both attachments freeze at 5; X is not closed at 9. |
| 14 | After row 13, delayed `LOST=1` arrives at 12 | Another `VOID12`; no recovery and no addition of LOST to the helper-failure total. |
| 15 | F5 exists; helper failure increments a counter; that CPU emits nothing further | Next counter check emits VOID; no later frontier despite no LOST callback. |
| 16 | P0 receives A12 then A11 while other source cutoffs remain 0 | Clock-regression VOID; neither DATA record released; daemon stays alive. |
| 17 | `S(11)`, then late P1 DATA at 9 | `F10`, then seal-violation VOID; reject late DATA and mark the disproved epoch invalid; daemon stays alive. |
| 18 | `S(11)`, then P1 DATA at 11, then `S(12)` | Equality with strict boundary is legal: `F10 → DATA11 → F11`. |
| 19 | F5 exists; eligible A7; writer rejects its publication | VOID/paused capture; no F7 and no trusted A7 suffix. |
| 20 | Same source position delivered twice | Internal invariant breach: VOID if possible, reject duplicate, exit nonzero. |

## 6. Numbered prototype run list

All times below estimate **gate-box hours summed across both hosts**, excluding implementation time.

1. **Inventory, measurement and qualification — approximately 1 hour.** Retrieve original trace payloads and perform §2’s count; freeze N/Q mode and the independent expected answers. Record exact kernel builds/configuration, clocksource, topology, PostgreSQL/libbpf versions and attachment masks. Inspect actual vendor clock implementations, 6.8 callback equivalence, EL8 map-helper behavior and both boundary/failure-visibility proofs. An unproved source remains unqualified.

2. **Pilot sizing and wakeups — approximately 0.5 hour.** Use master c16/c64/churn pilots to measure peak aggregate and per-CPU production rates. Let `H=64 MiB/Rmaster`, where `Rmaster` is peak event-ring bytes/s including framing. For source i, allocate at least `H×Ri` bytes, rounded up to supported page/power-of-two capacity, including boundary traffic; use actual perf/ring framing and any q bytes.
   Example arithmetic: with equal four-CPU rates, 56-byte ring records and measured 64-byte perf records, perf needs `64 MiB/4×64/56 = 18.29 MiB` per CPU before rounding, hence 32 MiB. Measure framing rather than assuming this example. Size ancillary bottlenecks consistently in both tracer arms or report their smaller limiting H.
   Record all rounding, total resident/mapped memory, hottest-CPU occupancy and maximum drain gap; freeze sizes before gates. Bound merger staging explicitly and treat exhaustion as loss.
   Perf policy: byte watermark `min(64 KiB, capacity/8)`, plus unconditional drain polling every 10 ms. Keep boundary frequency independent. Record master’s existing wakeup policy; use the same 10 ms polling interval for the per-CPU-ring experiment.

3. **Master/master noise calibration — approximately 3.33 hours.** On each host, run five pairs for each throughput cell: c1, c16, c64, and c16 connection churn. Each run has 30 s warmup plus 120 s measurement; randomize pair labels and interleave cells.
   For each host/cell define `dT=max|ln(TPS₂/TPS₁)|` and `dL=max|ln(latency₂/latency₁)|` across the five pairs. Freeze proposed/master median-ratio gates at `TPS≥exp(−2dT)` and `latency≤exp(2dL)`. The multiplier two is a declared engineering allowance, not a statistical confidence claim. If calibration is visibly unstable, repeat it before comparing the prototype.

4. **Main three-arm matrix — approximately 6 hours.** On Ubuntu 6.8 and EL8 4.18 compare no tracer, instrumented master transport, and perf+merge with real writer/live callbacks. Match instrumentation where applicable and keep capture settings/output destination fixed.
   Use PG 18.6, scale 10, simple-protocol TPC-B, c1/c16/c64 with `j=min(c,4)`, plus c16 `pgbench -C` reconnect-per-transaction churn; five repeats per arm/cell, 30 s warmup and 120 s measured. Randomize arm order within each repeat.
   Add five 120 s idle runs per arm/host with 16 connected backends. Confirm churn creates new backends while bootstrap probes are active; record connection, INIT attempt/delivery and attachment counts. Zero INIT drops with zero INIT attempts is not a completed churn test. Use 100 ms boundaries for perf+merge here.

5. **Freshness sweep — approximately 0.83 hour.** Reuse the 100 ms c16 results; add five perf+merge c16 repeats at 10 ms and 1 s on each host. Report boundary request/completion latency, event-to-release latency, frontier age p50/p95/p99/max, staging high-water mark and daemon CPU. **No freshness pass/fail threshold.**

6. **Fault and ordering checks — approximately 0.75 hour.** Run the frozen examples, migration, idle-CPU boundaries, delayed BUSY reservations and tiny-buffer overflow. Force failure followed by silence, delayed LOST, counter unreadability/reset, clock regression, seal violation and writer failure. Verify VOID/hold/exit distinctions and lack of a new frontier after failure. These checks exercise the prototype contract, not production shutdown behavior.

7. **EL8 per-CPU ringbuf arm — approximately 1 hour.** First test array-of-maps creation and verifier acceptance of CPU-selected inner ringbuf reserve/submit. Preserve logs/errno if unsupported. If supported, run five c16 repetitions using the same workload, stall-tolerance arithmetic and diagnostics. Report failures by site/errno/occupancy, delivery reconciliation, TPS, daemon CPU and memory beside perf+merge. This is the owner’s measured alternative, not an automatic transport switch.

8. **Read counters and apply gates — approximately 0.5 hour.** Save initial, per-second and final-after-producer-completion values: all 15 slots; per-site output attempts/successes/failures; perf LOST; boundary failures; clock regressions; seal violations; ambiguous ties; sequence gaps/reuse where enabled; staging/callback/writer failures; INIT and attachment counts. Reconcile successful emissions with delivered/staged records and report unexplained differences.
   **Correctness pass:** frozen expected outputs match; boundary proofs are qualified; every normal perf+merge repeat has zero loss/fault counters and no unexplained discrepancy, including all five EL8 c16 and churn repeats. Injected faults must produce the prescribed refusal, not zero counters.
   **Performance pass:** each throughput cell meets its pre-frozen proposed/master TPS and latency thresholds on both hosts. Report proposed/no-tracer ratios on both hosts, plus PostgreSQL CPU, daemon CPU, memory and output volume; no additional unapproved absolute overhead budget is invented.
   **Ringbuf result:** unsupported / supported-with-loss / supported-zero-loss, with measurements. Unsupported ringbuf does not fail the approved perf path.
   **Incomplete is not pass:** absent traces, absent INIT exercise or unqualified boundaries remain explicit outstanding checks.

**Estimated total:** approximately **14 gate-box hours**, budget **14–16 hours** for setup variation and limited reruns; the per-CPU-ring experiment accounts for about one hour.

## 7. Deferred to step 3

Production stop/final drain; rotation; de-escalation and END semantics; CPU online/offline; MAX_CPUS removal; INIT transport movement if warranted; durable evidence encoding and evidence-aware consumers; complete reattachment/identity recovery and certification invalidation behavior.
