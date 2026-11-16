# Exact-perf service budget audit (Refs #871, Refs #931)

Base: `d2f6719d`; the budget implementation audited is `8bf60d7d..d2f6719d`.
The supplied brief passed `scripts/lead/check-brief.sh` with
`CHECK BRIEF: covered #871, #931, #932`. Contracts and core merge implementation
are inputs to this unit. The wrapper creates the resulting commit.

Audit command:

```sh
git diff 8bf60d7d d2f6719d -- src/daemon.c src/merge_capture.c src/percpu_transport.c src/percpu_transport.h src/pg_wait_tracer.c src/transport_budget.h | rg -n '^\+.*(if|for|return|break|limit|timeout|budget|until|deadline|EAGAIN|EINTR)'
```

The table also includes the existing cut cap, service cadence and helper wait
that these budgets interact with. Capacities that reject input with an explicit
allocation fault are covered by the existing refusal guards; they are not time
or pass limits.

| Site | Deferred work and resumption | Check ordering and accounting | Named guard(s) |
|---|---|---|---|
| `pgwt_budget_begin`, daemon iteration / capture deadline | Clamp the shared pass to 50 ms and the capture deadline. Main-loop deadline/signal stops normal service and enters finish. | The clock grants no completeness. End-of-pass E yield sets backlog, making the next epoll timeout zero. Complete stops need no unfinished count; incomplete stops use the finish counters below. | `service_deadline`, `tick_duties`; six virtual live-deadline cases |
| `bounded_drain` expired entry | Leave raw input unread; next pass retries the drain. | No vector was read or check consumed here. Completion work still runs; `drain_budget_yields`. | `drain_entry_edge` |
| `bounded_drain` 10 ms slice | Reader callbacks stop draining; restore the original shared service deadline for completion/merge work. | No checkpoint is taken by the slice. Resumption still needs the full completion-fence vector; `drain_slice_yields`. | `drain_slice_edge` |
| `release_queue` 50 ms yield / `complete_round` resume | Retain `queue_head`, `release_left` and the frozen prefix. Resume that prefix before starting another round. | Deliver the fence CHECK before setting `releasing`; it is never resent with a substituted vector. Periodic checks during a yield also reach the core. `release_budget_yields`. | `rv_fence_edge`, `release_edge`, `service_deadline` |
| `pgwt_capture_envelope` old MA_CHECK budget return | Removed. Every control input is mapped immediately. Only the later retained sweep can yield. | Every supplied fresh control vector reaches the adapter/core, including the periodic full read. No equality-based shortcut. | `rv_fence_edge`, `periodic_edge`, `rv_final_check` |
| `flush_source` expired admission | Keep the head and all following envelopes. A subsequent envelope or control sweep retries the source FIFO. | If this invocation has not delivered its observation, map a CHECK with that exact vector before deferring. A control sweep passes an explicit already-delivered flag; mapping a record also delivers the observation. `source_budget_yields`. | `source_edge`, `merge_deadline` |
| Control sweep expiry / `source_count` traversal bound | Keep source heads; `next_source` rotates across later checks. Each visited source drains until hold or expiry. | The control CHECK is mapped before traversal. Traversing each source once cannot drop a check. `sweep_budget_yields`; remaining retained/staged items are counted at finish. | `sweep_edge`, `periodic_edge`, `merge_deadline` |
| `receive_perf` time yield | libbpf consumes the current sample and leaves its unread suffix for the next raw pass. | The current record is decoded and accounted first. No vector read here; resumption still takes the fence check. `perf_budget_yields`. | `perf_edge` |
| `receive_hint` time yield | Consume the current wake hint; later hints wait for the next pass. | Hints carry no counter vector or merge input. `hint_budget_yields`. | `hint_edge` |
| `receive_ring` time yield | Return negative EINTR after consuming/accounting the current record. Resume from libbpf's unread position. | No counter read here; preserved callback errors still win. `ring_budget_yields`. | `ring_edge` |
| `pgwt_transport_ring_record` / ring reader record cap | Negative EAGAIN after the cap's last record. The 4096-record slice (65536 for an unbounded consume request) leaves the suffix unread. | Count and decode the yielding record; never treat a positive callback result as a stop. `ring_record_cap_yields`. | `ring_cap_edge`, `bounded_ring`, `tick_duties` |
| `consume_all` pre-reader expiry / reader-count traversal bound | Keep `next_reader`; an expired pass reports nonempty. Next pass cycles all perf, hint, E and L readers fairly. | No fresh counter read here. Partial visits are not an empty/completed pass. `reader_budget_yields`. | `reader_edge`, `drain_slice_edge` |
| `linux_consume_io` expired entry and yielded-result propagation | Keep raw ring contents. E returns EAGAIN to the daemon backlog flag; L is serviced on the next ready/periodic pass. | E backlog prevents a poll-timeout sleep; real callback errors remain errors. `consume_budget_yields`, with callback/cap counters above. | `consume_edge`, `tick_duties` |
| `pgwt_percpu_transport_poll` old pre/post-drain budget returns | Both removed. `bounded_drain` can defer raw input while completion work still runs. | A budget cannot suppress a completed fence's fresh read/check. Release remains separately budgeted. | `rv_fence_edge`, all raw edge guards |
| Finish loop deadline before a pass | Stop admission, do the mandatory fresh stop CHECK, then VOID if still active. | Never substitute unknown-evidence for an unchecked counter change. `finish_deadline_reached`; queued capture/transport items and unread attempt deficits have separate counters. | `finish_entry_check` |
| Finish loop deadline after a pass | Same absolute 100 ms deadline; no fresh 100 ms allowance on resume. | Same mandatory stop CHECK, plus final capture CHECK. `finish_deadline_reached`; unfinished counters as above. | `finish_time_check`, `finish_deadline` |
| Finish 256-pass cap | Stop even if the virtual clock does not advance. | Deliver the fresh stop vector before the cap fault. `finish_pass_limit_reached` stays distinct from the time limit. | `finish_cap_check`, `shutdown_raw` |
| `linux_wait` finishing helper wait | Wait at most 1 ms, rounded down to fit the pass/absolute finish deadline. Below 1 ms use a zero-timeout poll, then continue finish. | No counter read/check is skipped by the wait. `wait_budget_yields` counts waits deferred at the edge. | `wait_edge` |
| `finish_capture` final CHECK | Deliver directly even if admission is expired; retained/staged suffixes are counted and VOID. | Final vector is never routed through a budget skip or an unbounded retained drain. `capture_final_unfinished_records`. | `rv_final_check`, `rv_uncovered_finish`, `shutdown_raw` |
| Completion cut pass cap | Freeze the cut; stop after its stated backlog/stall allowance. | Fresh CHECK before cap fault; no release over the missing prefix. `cut_pass_limit_reached`, `cut_pass_limit_records`. | `cut_cap_check`, `terminal_discard` |
| Periodic service-slot gate | Decide the slot using the clock before `daemon_observe` reads any counter component. Early calls defer the entire observation/check pair. | After a read, always deliver CHECK, including at an expired budget. Existing cadence remains once per slot; full-vector silence reads remain once per 10 ms. | `periodic_edge`, `tick_duties` (`cadence_reads_before_gate` mutant) |

Yield counters count scheduling decisions and can count a retained suffix more
than once. Terminal item counters count the remaining items at the stop once:
`unfinished_transport_records`, `unfinished_capture_records`,
`unfinished_unread_records`, and their sum `unfinished_records`.
`capture_final_unfinished_records` records the final capture suffix before CHECK
can clear staging; it is a separate observation, not another addend to the sum.
Uncovered records are consumed/accounted by the existing received path and do
not enter transport queues, retained queues, held-age tracking or unfinished
record counts. The reviewer guard exercises an uncovered record arriving during
finish; the normal busy finish guard drains covered input to DATA/FRONTIER/OK.

Every raw edge guard starts with a changed counter vector and the final CPU ack
pending, drives the actual stop, resumes the suffix and feeds the production
adapter/core. It requires exactly VOID(counter_change), no DATA, no FRONTIER and
no OK status. Each raw site has a scheduling mutant and a separate mutant that
skips its completion checkpoint after that yield. Every mutation must compile,
reach its registered guard and fail its named assertion; crashes and sanitizer
failures are rejected as kills.

The retained live arm is `tests/test_transport_daemon_live.py --only deadline`:
unthrottled load, natural exit after the requested lower bound and by duration
+ 15 s, and SIGTERM-to-exit within 15 s. Its docstring cites both deadline and
core-queue issues. Only its orchestration runs on the host; Linux/BPF execution
is outside this brief. The service budgets are cooperative between synchronous
core/OS calls, as documented in `percpu_transport.h`; this unit does not establish
a bound on one indivisible slow call (the core-queue unit is Refs #932).

Implementation cost: one authorised round, host execution only. Token total is
not measured inside this implementation session.

Validation evidence (2026-11-16, macOS host; ASan and UBSan enabled):

```console
$ CFLAGS='-fsanitize=address,undefined -g' PYTHONUNBUFFERED=1 tests/core_merge_adapter_mutants.sh
adapter baseline: golden 12/12, guard sweep 31/31, end-to-end 78/78 PASS, both linear and indexed
R1: KILLED at G1 (linear and indexed)
R2: KILLED at G2 (linear and indexed)
R3: KILLED at G3 (linear and indexed)
R4: KILLED at G4 (linear and indexed)
R5: KILLED at G5 (linear and indexed)
R6: KILLED at G6 (linear and indexed)
R7: KILLED at G7 (linear and indexed)
R8: KILLED at G8 (linear and indexed)
R9: KILLED at G9 (linear and indexed)
R10: KILLED at G10 (linear and indexed)
R11: KILLED at G11 (linear and indexed)
R12: KILLED at G12 (linear and indexed)
FAILED_begin: KILLED at failed_terminal (linear and indexed)
identity_unbound: KILLED at identity_unbound (linear and indexed)
ESCALATE_END: KILLED at escalation_markers (linear and indexed)
P_nonfork_stale: KILLED at proof_nonfork (linear and indexed)
P_late_cookie: KILLED at proof_late_cookie (linear and indexed)
P_pid_only: KILLED at proof_pid_only (linear and indexed)
P_reused: KILLED at proof_reused (linear and indexed)
P_cleanup: KILLED at proof_unsafe_cleanup (linear and indexed)
P_cookie_overwrite: KILLED at proof_overwrite_cookie (linear and indexed)
P_q_overwrite: KILLED at proof_overwrite_q (linear and indexed)
P_stale_marker: KILLED at proof_stale_marker (linear and indexed)
P_emission_cookie: KILLED at proof_other_facts (linear and indexed)
P_failure_accounting: KILLED at proof_other_facts (linear and indexed)
P_continuous: KILLED at proof_other_facts (linear and indexed)
P_idempotent_init: KILLED at proof_other_facts (linear and indexed)
adapter mutants: 27/27 killed; guard sweep 31/31 PASS
[transport portion also exited 0; latest transport invocation follows]
$ CFLAGS='-fsanitize=address,undefined -g' PYTHONUNBUFFERED=1 python3 tests/percpu_transport_mutants.py
transport baseline: production wire rows 1-43: 43/43 N PASS; completion/refusal guards: 49/49 PASS
REACHED rv_fence_edge
RV-C fence edge: fresh vector delivered before yielded release; VOID counter_change PASS
REACHED rv_uncovered_finish
RV-A rc=0 phase=10 count=0 out0kind=-1 reason=-1 status=0 uncovered=1
REACHED rv_final_check
RV-B rc=0 count=1 out0kind=3 reason=0 status=1
finish accounting: failures and discards excluded from unfinished; unread attempts counted across CPU/E/L PASS
finish deadline: 100 ms absolute bound, unfinished evidence VOID and counted once PASS
transport all_acks PASS
transport missing_ack PASS
transport stale_ack PASS
transport counter_timing PASS
transport post_snapshot PASS
shutdown: unread perf DATA drained and released with FRONTIER + OK; nonempty/BUSY suffix VOID; pass cap distinct from deadline; final counters checked
terminal attempts: empty CPU/E/L with last publication missing => VOID(sequence_gap) PASS
perf cadence: byte watermark min(buffer/8,65536), poll 10 ms; 100000 early wakes, 1 topology service PASS
poll budget: empty drains and prompt helpers only: below-watermark DATA + two rounds, release by 60 ms; <=70 ms with poll lateness (not a sustained-load bound) PASS
completion: never-empty passes hold without every ack (300 passes) and release on the pass that consumes the last ack PASS
completion: an unconsumed snapshot-era record holds without a fault; the cut and the target stay frozen under later traffic PASS
completion: a counted failure on any key, a failed boundary and a failed ack VOID(sequence_gap) with no pass; unreadable accounting VOIDs PASS
completion control: a record below a published frontier arriving later => VOID(seal_violation), epoch invalid PASS
discards: a counted discard explains a skipped number; an uncounted skip, a failure and a repeat VOID(sequence_gap) PASS
terminal hole: a counted discard completes at the cut; an uncounted one holds until the cut then VOID(sequence_gap); an unreachable cut VOIDs after 4098 passes PASS
uncovered: cookie-less TRACE and EXIT are consumed, accounted and counted, never merge input PASS
binary sink: TRACE DATA 88 bytes; 41 DATA + FRONTIER, 1 write/batch
counter cost: 41 released DATA, 13 map lookups at 1 completion checkpoint; 0 map lookups/DATA
indexed lookups at 1000 lifetimes: L 1.002, Q 1.000 buckets/lookup; TRACE (L+2Q) 3.002 buckets/record
indexed lookups at 100000 lifetimes: L 1.118, Q 1.117 buckets/lookup; TRACE (L+2Q) 3.352 buckets/record
lifetime history: 100000 fork/exit cycles; PID reuse isolated; 262144 + 1 => VOID(allocation_fail)
blanket_finish_void: KILLED at rv_uncovered_finish (pgwt_capture_report(d.capture).status==PGWT_MERGE_OK)
skip_final_budget_edge_check: KILLED at rv_final_check (d.outputs[0].reason==PGWT_MERGE_COUNTER_CHANGE)
failures_as_unfinished: KILLED at finish_failures (stats.unfinished_records==unread)
pass_cap_as_deadline: KILLED at shutdown_raw (pgwt_transport_stats(d.transport).finish_pass_limit_reached==1)
service_without_clock: KILLED at service_deadline (d.now-started<=PGWT_TRANSPORT_PASS_NS)
merge_without_clock: KILLED at merge_deadline (d.now-started<=PGWT_TRANSPORT_PASS_NS)
finish_ignores_deadline: KILLED at finish_deadline (d.now-started<=PGWT_TRANSPORT_FINISH_NS)
finish_counter_dropped: KILLED at finish_deadline (pgwt_transport_stats(d.transport).finish_deadline_reached==1)
synthetic_daemon: KILLED at admission
skip_cpu_ack: KILLED at missing_ack
early_counter_read: KILLED at counter_timing
accept_stale_round: KILLED at stale_ack
ignore_seq_gap: KILLED at gaps
per_record_write: KILLED at dump
positive_ring_limit: KILLED at bounded_ring
skip_shutdown_drain: KILLED at shutdown_raw
forget_historical_proof: KILLED at lifetime_history
per_record_wakeup: KILLED at cadence
per_pass_topology: KILLED at cadence
slot_gated_completion: KILLED at never_empty
restore_empty_gate: KILLED at never_empty
constant_el_token: KILLED at never_empty
moving_target: KILLED at snapshot_hold
moving_cut: KILLED at snapshot_hold
diagnose_before_cut: KILLED at snapshot_hold
ignore_failure_counters: KILLED at terminal_failure
no_failure_poll_while_open: KILLED at terminal_failure
unreadable_accounting_is_zero: KILLED at terminal_failure
uncounted_skip_accepted: KILLED at discards
repeat_accepted: KILLED at discards
ignore_terminal_hole: KILLED at terminal_discard
unbounded_cut_wait: KILLED at terminal_discard
terminal_discard_is_delivered: KILLED at terminal_discard
uncovered_into_merge: KILLED at uncovered
skip_terminal_attempts: KILLED at terminal_attempts
skip_tick_entry: KILLED at pending
omit_core_backlog: KILLED at core_held_age
omit_adapter_backlog: KILLED at pending
skip_fence_edge_check: KILLED at rv_fence_edge (d->outputs[0].reason==PGWT_MERGE_COUNTER_CHANGE)
skip_periodic_edge_check: KILLED at periodic_edge (d->outputs[0].reason==PGWT_MERGE_COUNTER_CHANGE)
skip_source_edge_observation: KILLED at source_edge (d->outputs[0].reason==PGWT_MERGE_COUNTER_CHANGE)
skip_sweep_edge_check: KILLED at sweep_edge (d->outputs[0].reason==PGWT_MERGE_COUNTER_CHANGE)
skip_release_resume_check: KILLED at release_edge (d->outputs[0].reason==PGWT_MERGE_COUNTER_CHANGE)
skip_finish_entry_check: KILLED at finish_entry_check (d->outputs[0].reason==PGWT_MERGE_COUNTER_CHANGE)
skip_finish_time_check: KILLED at finish_time_check (d->outputs[0].reason==PGWT_MERGE_COUNTER_CHANGE)
skip_finish_cap_check: KILLED at finish_cap_check (d->outputs[0].reason==PGWT_MERGE_COUNTER_CHANGE)
skip_cut_cap_check: KILLED at cut_cap_check (d->outputs[0].reason==PGWT_MERGE_COUNTER_CHANGE)
transport mutants: 47/47 killed at intended assertions
default tick: fe519c9e lines 443-677 byte-identical after substituting default drain; order/logs PASS
REACHED tick_duties
actual daemon publication callback: 1000 refreshes, writer component only, 0 map lookups/DATA PASS
actual capture poll: 100000 early calls, 1 core CHECK; silent full vector once/10 ms PASS
actual Linux drain/callbacks: perf DONE and ring negative yield within 50 ms; suffix retained; 4096 record guard PASS
actual Linux consume + daemon backlog: expired pass, timed yield and record cap keep next wait awake; errors preserved PASS
shared tick: full duty order, 6 views, rates/count, GC, rotations/retention, lightweight/test/death guards PASS
skip_tick_count: KILLED at tick_duties
per_record_map_reads: KILLED at tick_duties
ignore_perf_receipts: KILLED at tick_duties
perf_without_clock: KILLED at tick_duties
ring_without_clock: KILLED at tick_duties
continue_failed_tick_drain: KILLED at tick_duties
omit_tick_backlog: KILLED at tick_duties
per_pass_core_checks: KILLED at tick_duties
cadence_reads_before_gate: KILLED at tick_duties (observation_reads==1)
repeat_backlog_warning: KILLED at tick_duties
expired_consume_sleeps: KILLED at tick_duties (consume_event_ring(d,"test",true) && e.records==15)
yielded_consume_sleeps: KILLED at tick_duties (consume_event_ring(d,"test",true) && e.records==10)
daemon_forgets_yield: KILLED at tick_duties (consume_event_ring(d,"test",true) && e.records==15)
transport + tick/publication/drain/cadence/yield mutants: 60/60 killed at intended assertions
REACHED drain_entry_edge
transport drain_entry_edge PASS (expiry + pending counter => VOID counter_change)
REACHED drain_slice_edge
transport drain_slice_edge PASS (expiry + pending counter => VOID counter_change)
REACHED reader_edge
transport reader_edge PASS (expiry + pending counter => VOID counter_change)
REACHED perf_edge
transport perf_edge PASS (expiry + pending counter => VOID counter_change)
REACHED hint_edge
transport hint_edge PASS (expiry + pending counter => VOID counter_change)
REACHED ring_edge
transport ring_edge PASS (expiry + pending counter => VOID counter_change)
REACHED consume_edge
transport consume_edge PASS (expiry + pending counter => VOID counter_change)
REACHED ring_cap_edge
transport ring_cap_edge PASS (expiry + pending counter => VOID counter_change)
REACHED wait_edge
transport wait_edge PASS (expiry + pending counter => VOID counter_change)
raw budget guards: 9/9 PASS
drain_entry_false_empty: KILLED at drain_entry_edge (bounded_drain(d.transport)==0)
drain_slice_unbounded: KILLED at drain_slice_edge (bounded_drain(d.transport)==0 && p0.left==1)
reader_false_empty: KILLED at reader_edge (consume_all(&l,&empty)==0 && !empty)
perf_unbounded: KILLED at perf_edge (consume_all(&l,&empty)==0 && !empty && p0.left==2)
hint_unbounded: KILLED at hint_edge (consume_all(&l,&empty)==0 && !empty && h0.left==2)
ring_unbounded: KILLED at ring_edge (consume_all(&l,&empty)==0 && !empty && e.left==1)
consume_false_empty: KILLED at consume_edge (linux_consume_io(&l,PGWT_WIRE_DEST_E,true)==-EAGAIN && e.left==2)
wait_rounds_up_deadline: KILLED at wait_edge (linux_wait(&l)==0 && raw_wait_timeout==0)
ring_cap_positive: KILLED at ring_cap_edge (linux_consume_io(&l,PGWT_WIRE_DEST_E,true)==-EAGAIN && e.left==1)
skip_drain_entry_edge_checkpoint: KILLED at drain_entry_edge (d->outputs[0].reason==PGWT_MERGE_COUNTER_CHANGE)
skip_drain_slice_edge_checkpoint: KILLED at drain_slice_edge (d->outputs[0].reason==PGWT_MERGE_COUNTER_CHANGE)
skip_reader_edge_checkpoint: KILLED at reader_edge (d->outputs[0].reason==PGWT_MERGE_COUNTER_CHANGE)
skip_perf_edge_checkpoint: KILLED at perf_edge (d->outputs[0].reason==PGWT_MERGE_COUNTER_CHANGE)
skip_hint_edge_checkpoint: KILLED at hint_edge (d->outputs[0].reason==PGWT_MERGE_COUNTER_CHANGE)
skip_ring_edge_checkpoint: KILLED at ring_edge (d->outputs[0].reason==PGWT_MERGE_COUNTER_CHANGE)
skip_consume_edge_checkpoint: KILLED at consume_edge (d->outputs[0].reason==PGWT_MERGE_COUNTER_CHANGE)
skip_ring_cap_edge_checkpoint: KILLED at ring_cap_edge (d->outputs[0].reason==PGWT_MERGE_COUNTER_CHANGE)
skip_wait_edge_checkpoint: KILLED at wait_edge (d->outputs[0].reason==PGWT_MERGE_COUNTER_CHANGE)
raw budget mutants: 18/18 killed at intended assertions
live deadline orchestration: 6/6 virtual process cases PASS (early zero exit rejected)
natural_exit_without_lower_bound: KILLED at early_zero_exit
transport host mutants total: 79/79 killed at intended assertions
$ scripts/check-test-registration.sh
OK: test-registration guard (base 23cad3a2752adc4e76eb2a125b145476693d4948) — append-only invariant holds
$ scripts/check-runner-registration.sh
OK: runner-registration guard -- every tests/ci_smoke.sh invocation (5) is also invoked by tests/run_all.sh (214 invocations total, including tests/unit_tests.list entries); every candidate test file on disk (196 scanned: tests/test_*.{py,sh} plus tests/web_unit/*.test.mjs) is invoked by a runner or covered by a glob; or is a documented exemption (2 listed, 2 currently invoke no runner).
$ git diff --check
[exit 0]
```

Registered transport guard manifest (49): `rv_fence_edge`, `periodic_edge`, `source_edge`, `sweep_edge`, `release_edge`, `cut_cap_check`, `finish_entry_check`, `finish_time_check`, `finish_cap_check`, `rv_uncovered_finish`, `rv_final_check`, `finish_failures`, `service_deadline`, `merge_deadline`, `finish_deadline`, `admission`, `all_acks`, `missing_ack`, `stale_ack`, `counter_timing`, `post_snapshot`, `core_held_age`, `attempt_window`, `late_fork`, `unknown_init`, `unknown`, `gaps`, `writer`, `decode_lost`, `shutdown_topology`, `shutdown_raw`, `terminal_attempts`, `cadence`, `pending`, `poll_budget`, `never_empty`, `snapshot_hold`, `terminal_failure`, `seal_control`, `discards`, `terminal_discard`, `uncovered`, `flag_off`, `injected_kernel`, `dump`, `dump_failure`, `bounded_ring`, `counter_cost`, `lifetime_history`.
Raw budget guard manifest (9): `drain_entry_edge`, `drain_slice_edge`, `reader_edge`, `perf_edge`, `hint_edge`, `ring_edge`, `consume_edge`, `ring_cap_edge`, `wait_edge`.
The compiled shared-duty/cadence/publication harness is `tick_duties`.
The mutation transcript above names all 27 adapter and 79 transport mutants,
including blanket finish VOID, every budget site's checkpoint skip, and all
three yield/sleep propagation mutants.

Changed files (9); production delta: four files, +129/-27 lines:

- `docs/931-service-budget-audit.md`
- `src/merge_capture.c`
- `src/merge_capture.h`
- `src/percpu_transport.c`
- `src/percpu_transport.h`
- `tests/percpu_budget_harness.h`
- `tests/percpu_tick_harness.h`
- `tests/percpu_transport_mutants.py`
- `tests/test_percpu_transport.c`

```console
$ git diff --numstat -- src
41	10	src/merge_capture.c
1	0	src/merge_capture.h
78	16	src/percpu_transport.c
9	1	src/percpu_transport.h
```

The required owner-report audit could not initialize its auditor. This is an
incomplete reporting gate, not a SEND result:

```console
$ scripts/lead/audit-report.sh /private/tmp/pgwt-931-fresh/report.md
PREPASS: no unevidenced absence claim found
AUDIT INCONCLUSIVE (auditor exit 1) -- do not send. draft=89d74b726b5bc1696829d81bcbd0aad8772327d95607f7da67565f376164e989
$ tail -n 35 [auditor transcript]
WARNING: proceeding, even though we could not create PATH aliases: Operation not permitted (os error 1)
Reading additional input from stdin...
Error: failed to initialize in-process app-server client: Operation not permitted (os error 1)
```

The implementation is prepared for the wrapper. The report audit remains
incomplete; the Linux deadline run remains outside this host-only brief.
