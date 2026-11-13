# Kernel argument for the perf experiment — Refs #871

**This is not a qualification or permission to enable product perf.** The frozen
[merge-v1 contract](../../contracts/merge-v1/CONTRACT.md), section 3, requires a
kernel argument as well as measurements. Status labels below apply to individual
claims: **CITED** means the linked source establishes the stated source fact;
**INFERENCE** gives a conditional argument from those facts; **UNPROVEN** needs
actual-kernel evidence or additional proof. Neither upstream tag represents all
vendor backports. Linux builds and box measurements are left to the lead.

## Permitted producers and no overlap

**CITED — upstream x86 #DB handler ordering.** In v4.18,
[`do_debug`, traps.c:721–728, 782–793](https://github.com/torvalds/linux/blob/v4.18/arch/x86/kernel/traps.c#L721-L793)
enters through `ist_enter`, invokes the DIE_DEBUG notifier, and only afterward
conditionally enables IRQs. The notifier dispatches to the breakpoint handler at
[hw_breakpoint.c:534–540](https://github.com/torvalds/linux/blob/v4.18/arch/x86/kernel/hw_breakpoint.c#L534-L540),
which calls `perf_bp_event` at
[505](https://github.com/torvalds/linux/blob/v4.18/arch/x86/kernel/hw_breakpoint.c#L505).
In v6.8, [`exc_debug_user`, traps.c:948–1013](https://github.com/torvalds/linux/blob/v6.8/arch/x86/kernel/traps.c#L948-L1013)
uses user entry handling and calls `notify_debug` before `local_irq_enable`;
[`notify_debug`:864–877](https://github.com/torvalds/linux/blob/v6.8/arch/x86/kernel/traps.c#L864-L877)
calls DIE_DEBUG, and
[hw_breakpoint.c:555, 580–586](https://github.com/torvalds/linux/blob/v6.8/arch/x86/kernel/hw_breakpoint.c#L550-L586)
connects it to perf. The v4.18 BPF overflow wrapper invokes the attached program
synchronously at
[core.c:8494–8518](https://github.com/torvalds/linux/blob/v4.18/kernel/events/core.c#L8494-L8518).

**CITED — CPU-clock boundary uses an IRQ timer, not a hardware NMI event.**
v4.18 CPU-clock setup selects the hrtimer and declares NO_NMI at
[core.c:9232–9256](https://github.com/torvalds/linux/blob/v4.18/kernel/events/core.c#L9232-L9256).
[9099–9170](https://github.com/torvalds/linux/blob/v4.18/kernel/events/core.c#L9099-L9170)
shows the overflow callback, the `exclude_idle` test, pinned period restart, and
`CLOCK_MONOTONIC` initialization without SOFT mode.
[hrtimer.c:1268–1289](https://github.com/torvalds/linux/blob/v4.18/kernel/time/hrtimer.c#L1268-L1289)
selects the hard timer base for that mode;
[1484–1518](https://github.com/torvalds/linux/blob/v4.18/kernel/time/hrtimer.c#L1484-L1518)
runs hard timers from the IRQ-disabled timer interrupt, with the callback call at
[1396–1399](https://github.com/torvalds/linux/blob/v4.18/kernel/time/hrtimer.c#L1396-L1399).
v6.8 makes the context explicit: HARD initialization/restart and CPU-clock NO_NMI
are at
[core.c:11046–11117, 11179–11204](https://github.com/torvalds/linux/blob/v6.8/kernel/events/core.c#L11046-L11204);
[hrtimer.h:29–59](https://github.com/torvalds/linux/blob/v6.8/include/linux/hrtimer.h#L29-L59)
defines HARD as hardirq execution, including RT. No perf hardware-cycle sampling
or task-clock event is used for this boundary.

**INFERENCE — allowed timestamp-to-output sections do not overlap on one CPU.**
The DATA source is a write performed by a userspace instruction, not a kernel
write, execution breakpoint, uprobe, tracepoint or arbitrary NMI program. The
bench attaches one per-TID 8-byte write watchpoint with `exclude_kernel=1`; each
CPU has one pinned writer, or there is one migrating writer. The CPU-clock hardirq
cannot run while the user #DB handler executes BPF before IRQ enabling. Conversely,
userspace cannot execute a watched store while that CPU is executing the hardirq
boundary. Two such hardirq callbacks cannot overlap each other. Migration happens
between stores; the producing handler remains on its current CPU. Thus one allowed
producer completes its timestamp, helper call and failure accounting before the
other allowed producer can start on that CPU. This is the required exclusion
argument; calling a generic interrupting heartbeat a boundary would not provide it.

**UNPROVEN — complete actual-kernel entry/IRQ and producer inventory.** Before
acceptance, reviewers must match the interrupt-gate entry, notifier chain,
preemption behavior, timer mode and config in the running vendor kernel to that
argument. PREEMPT_RT/vendor changes are not qualified by upstream citations alone.
The helper scratch slot discussed below can also be used by other BPF programs
on the same CPU even when they write to different perf maps; their absence or
non-overlap must be established on the actual box. Only DATA and BOUNDARY are
attached to this bench's output buffers, but that fact alone is insufficient to
prove exclusive use of the kernel's helper scratch storage.

## Syscall boundary producer (`syscall_boundary`)

**CITED — syscall and tracepoint choice.** `getppid` has no arguments, user
buffer, or blocking I/O: its body reads the parent's TGID under RCU in
[v4.18 sys.c:863–872](https://github.com/torvalds/linux/blob/v4.18/kernel/sys.c#L863-L872)
and [v6.8 sys.c:929–938](https://github.com/torvalds/linux/blob/v6.8/kernel/sys.c#L929-L938).
The syscall metadata macros generate `sys_enter_getppid` in both
[v4.18 syscalls.h:125–198](https://github.com/torvalds/linux/blob/v4.18/include/linux/syscalls.h#L125-L198)
and [v6.8:128–208](https://github.com/torvalds/linux/blob/v6.8/include/linux/syscalls.h#L128-L208),
conditional on `CONFIG_FTRACE_SYSCALLS`. This makes it a small, named,
argument-independent trigger across the two upstream versions. The bench calls
`syscall(SYS_getppid)` explicitly, so no libc caching or vDSO substitution can
replace the syscall. Tracepoint presence, permissions and seccomp policy on an
actual box remain **UNPROVEN** until attachment and producer checks succeed.

**CITED — synchronous task-context path.** The syscall entry path invokes
`trace_sys_enter` after entering the kernel, with IRQs enabled:
[v4.18 common.c:116–118, 250–271](https://github.com/torvalds/linux/blob/v4.18/arch/x86/entry/common.c#L116-L271)
and [v6.8 common.c:26–69](https://github.com/torvalds/linux/blob/v6.8/kernel/entry/common.c#L26-L69).
The syscall tracepoint invokes BPF synchronously before trace-event submission:
[v4.18 trace_syscalls.c:527–589](https://github.com/torvalds/linux/blob/v4.18/kernel/trace/trace_syscalls.c#L527-L589)
and [v6.8:517–582](https://github.com/torvalds/linux/blob/v6.8/kernel/trace/trace_syscalls.c#L517-L582).
It is a syscall execution, not a CPU-clock sampling interrupt. The output helper
still writes a perf buffer; attaching a tracepoint through libbpf's perf-event
API does not make the trigger depend on CPU_CLOCK sample timers or their rate.

**INFERENCE — no overlap with the permitted DATA producer.** The bench's DATA
watchpoint is per writer TID, write-only, with `exclude_kernel=1` (see
`open_breakpoint` in [nmi_transport.c](nmi_transport.c)). Its only watched stores
are userspace instructions. **CITED:** `perf_bp_event` applies the privilege
filter before invoking perf: [v4.18 core.c:8161–8169](https://github.com/torvalds/linux/blob/v4.18/kernel/events/core.c#L8161-L8169)
and [v6.8:9987–9995](https://github.com/torvalds/linux/blob/v6.8/kernel/events/core.c#L9987-L9995);
`exclude_kernel` rejects kernel-mode registers in
[v4.18:7423–7437](https://github.com/torvalds/linux/blob/v4.18/kernel/events/core.c#L7423-L7437)
and [v6.8:9165–9179](https://github.com/torvalds/linux/blob/v6.8/kernel/events/core.c#L9165-L9179).
**INFERENCE:** the synchronous #DB BPF handler runs before IRQs
are enabled: [v4.18 traps.c:734–745](https://github.com/torvalds/linux/blob/v4.18/arch/x86/kernel/traps.c#L734-L745)
and [v6.8:991–994](https://github.com/torvalds/linux/blob/v6.8/arch/x86/kernel/traps.c#L991-L994),
with the EL8-specific ordering recorded below. A syscall helper cannot execute
on that CPU until the unfinished user #DB handler returns. Conversely, a
userspace watched instruction cannot execute inside the kernel tracepoint.
Helpers are pinned to singleton CPU masks using `sched_setaffinity`; BPF filters
by helper TGID and registered TID, checks the actual CPU against the registered
CPU, and refuses output on a mismatch. One helper runs per participating CPU;
only this syscall BOUNDARY and user-watchpoint DATA use its output buffer in this
arm. The CPU-clock boundary program is not loaded in this arm. Thus their
timestamp-to-output sections cannot overlap on a CPU. In v4.18 the synchronous
[BPF call disables preemption](https://github.com/torvalds/linux/blob/v4.18/kernel/trace/bpf_trace.c#L38-L78);
the fixed affinity also prevents a helper's migration to another CPU.
Other IRQ/NMI BPF programs, vendor changes, RT and actual helper scratch users
remain **UNPROVEN** under the inventory requirements above.

**INFERENCE — completion and cost.** Each helper waits against absolute
`CLOCK_MONOTONIC` deadlines, skipping missed periods rather than bursting.
Stopping broadcasts to the waiting helpers and joins them before detaching the
tracepoint, draining, and reading counters. A joined helper has returned from
its synchronous syscall/BPF invocation, including failed outputs. The BPF clock
read supplies `w`; the helper's userspace clock is only a wakeup scheduler.
Each period per CPU costs a syscall, a wakeup and scheduling/context switches
(out to sleep and back in); the tracepoint filter also runs on unrelated
`getppid` calls. Exact cost is **UNPROVEN** until measured. Per-helper
`CLOCK_THREAD_CPUTIME_ID` deltas include synchronous task-context kernel work but
exclude some wakeup IRQ and other-task scheduler work. They are reported per
CPU in every case and remain measurable without IRQ time accounting. They do
not certify total overhead: the frozen idle cost gate still uses the paired
`/proc/stat` delta and stays **UNMEASURED** without IRQ time accounting. A helper
alone exceeding 1% of a CPU fails even if total cost is unmeasured.

**UNPROVEN — progress and platform limits.** A wakeup is not a scheduling latency
guarantee: saturation, virtualization or long IRQ-disabled sections may delay
the helper. Every CPU must pass the same 100 ms consumed-boundary and stamp-gap
gates in idle, one-active, all-active and migrating cases, with fires equal
outputs, zero wrong-CPU events, zero loss, seals and timestamp regressions.
The protocol retains the perf boundary and adversarial arms, and runs a separate
+1 ms syscall-boundary seal control. Cpusets can reject a singleton affinity;
`isolcpus` and `nohz_full` deployments may restrict or prohibit placing waking
helpers on those CPUs. The caller's allowed CPUs are recorded, pinning errors
refuse the run, and successful pinning does not qualify those deployment modes.
CPU hotplug is excluded: the bench fixes the participating set and provides no
online/offline reconfiguration. Actual-kernel clock, publication and producer
inventory claims below still require proof; this alternative is not product
enablement or completed box qualification.

## EL8 vendor difference

**CITED — inspected RHEL-derived 8.10 extract, not inferred from upstream v4.18.**
The lead's saved extract from
`kernel-4.18.0-553.157.1.el8_10.src.rpm` has `arch/x86/kernel/traps.c` SHA256
`21d90bccbbca54f62d4f6f9167d290c2686e9aa5664b288dc33a9af3f36c53f4`.
Its `do_debug` is at 867; it calls `nmi_enter()` at 896 even on the user path,
DIE_DEBUG at 933–935 before IRQ enabling at 943–944, and `nmi_exit()` at 976.
This differs from upstream v4.18's `ist_enter` path and v6.8's split user/kernel
entry paths. The extract was read at the lead's
`tests/results/session-20261111/m0c/el8/ksrc/arch/x86/kernel/traps.c`; it is not a
new artifact in this change. Package/provenance and the contention finding are in
[the controlling issue comment](https://github.com/DmitryNFomin/pg_wait_tracer/issues/871#issuecomment-6798737580).

**INFERENCE:** the NMI accounting label does not let a user instruction execute
inside an unfinished kernel timer callback. The proposed exclusion depends on
where the instruction originated and on IRQ ordering, not just `in_nmi()`.
**UNPROVEN:** this extract does not establish all EL8 perf/helper/hrtimer backports,
or their equality to the package the lead will run. The protocol records that
running package and config; reviewers still need its matching source.

## Older helper scratch storage and loss accounting

**CITED — v4.18 has one sample-data slot per CPU.**
[bpf_trace.c:369–419](https://github.com/torvalds/linux/blob/v4.18/kernel/trace/bpf_trace.c#L369-L419)
defines one `bpf_trace_sd`, initializes it, points it at the current raw payload,
and uses it synchronously. Nested uses could overwrite the outer invocation's
sample state. The later multi-slot implementation is present in
[v5.3:448–489](https://github.com/torvalds/linux/blob/v5.3/kernel/trace/bpf_trace.c#L448-L489)
and [v6.8:650–697](https://github.com/torvalds/linux/blob/v6.8/kernel/trace/bpf_trace.c#L650-L697),
with three slots, nesting indexing and an overflow refusal.
**INFERENCE:** the no-overlap argument above protects the older slot for these
permitted producers; a perf ring's own nested-publication support does not by
itself protect this separate helper scratch state. **UNPROVEN:** whether any
particular pre-5.3 vendor kernel backported the multi-slot implementation.

**CITED — helper success need not mean delivery.** In
[v4.18 bpf_trace.c:398–399](https://github.com/torvalds/linux/blob/v4.18/kernel/trace/bpf_trace.c#L398-L399),
perf output is followed by return zero. The helper can therefore hide perf's
capacity drop. In
[v6.8:648](https://github.com/torvalds/linux/blob/v6.8/kernel/trace/bpf_trace.c#L648)
the return is propagated. The bench's per-CPU/site `produced` counts attempts,
not just helper successes. After stop and drain, attempts minus delivered is
compared with sequence gaps, including initial and terminal gaps. Helper failures
and PERF_RECORD_LOST remain separate diagnostics. The one-page paused-drain
control must show missing attempts even if the old helper returns zero and no
LOST callback follows the final drop.

## Publication, consumer ordering and completion

**CITED — perf publication barrier.** Both
[v4.18 ring_buffer.c:26–100](https://github.com/torvalds/linux/blob/v4.18/kernel/events/ring_buffer.c#L26-L100)
and [v6.8:25–134](https://github.com/torvalds/linux/blob/v6.8/kernel/events/ring_buffer.c#L25-L134)
withhold the published head until the outermost output completes. A write barrier
precedes head publication; the documented pairs require a consumer read barrier
before reading data, and ordering of reads before publishing the consumed tail.
**CITED — example consumer:**
[v6.8 tools ring_buffer.h:51–72](https://github.com/torvalds/linux/blob/v6.8/tools/include/linux/ring_buffer.h#L51-L72)
uses acquire head reads and release tail stores;
[libbpf.c:12847–12891](https://github.com/torvalds/linux/blob/v6.8/tools/lib/bpf/libbpf.c#L12847-L12891)
processes the head snapshot in position order before releasing the tail. This
bench uses libbpf's raw consumer rather than implementing mmap barriers itself.
**UNPROVEN:** the actual linked libbpf must supply the corresponding barriers;
upstream v6.8 userspace source is an example, not identification of that library.
The protocol records linkage and build inputs for that review.

**CITED — disable path completion facts:**
[v4.18 core.c:131–142, 249–270, 2164–2175](https://github.com/torvalds/linux/blob/v4.18/kernel/events/core.c#L131-L270)
uses a synchronous target-CPU function call for CPU-wide events; the
[disable dispatch](https://github.com/torvalds/linux/blob/v4.18/kernel/events/core.c#L2164-L2175)
uses that machinery. CPU-clock stop
[9207–9210](https://github.com/torvalds/linux/blob/v4.18/kernel/events/core.c#L9207-L9210)
cancels its timer via
[9150–9158](https://github.com/torvalds/linux/blob/v4.18/kernel/events/core.c#L9150-L9158).
**INFERENCE:** joined DATA threads have returned from their synchronous watchpoint
handlers; disabling and closing each boundary event before the final drain and
counter reads completes the permitted producers. This covers failed helper paths
that do not leave a transport record. **UNPROVEN:** matching completion and counter
visibility arguments for every actual vendor build, including v6.8-derived boxes.
Live product capture would need an ongoing completion mechanism, not just the
bench's terminal stop. Stable counter reads during live production cannot prove
that an interrupted failure increment has completed.

## Clock ordering and remaining scope

**CITED — clock identity and caveat.** Both helpers call `ktime_get_mono_fast_ns`:
[v4.18 helpers.c:107–110](https://github.com/torvalds/linux/blob/v4.18/kernel/bpf/helpers.c#L107-L110),
[v6.8:174–177](https://github.com/torvalds/linux/blob/v6.8/kernel/bpf/helpers.c#L174-L177).
[Timekeeping v4.18:407–436](https://github.com/torvalds/linux/blob/v4.18/kernel/time/timekeeping.c#L407-L436)
and [v6.8:459–488](https://github.com/torvalds/linux/blob/v6.8/kernel/time/timekeeping.c#L459-L488)
explicitly describe possible regression across updates, including a CPU-local NMI
interrupting an update. **INFERENCE:** user-origin watchpoints cannot interrupt a
kernel timekeeping update, despite EL8's NMI accounting label; hardirq boundaries
need the update's IRQ exclusion as well. **UNPROVEN:** the complete ordered
same-CPU clock-observation argument for the selected vendor kernel, clocksource,
virtualization and timekeeping updates. Recording a clocksource name or measuring
zero regressions does not establish that argument. It must be completed before
these diagnostics can supply certified merge frontiers.

**UNPROVEN / OPEN (excluded):** CPU hotplug and `nohz_full`, including their effects
on participating-source identity, idle progress and teardown; other producer
classes; product perf enablement; any product layout/core
change. The benchmark keeps a fixed CPU population. Its default gates include the
100 us adversarial arm and controls; none resolves an unproven source claim.

## Product failure-site map and full-provider metrics

The BPF object exports `ringbuf_site_drops` beside the existing `ringbuf_drops` map.
It is a PERCPU_ARRAY of u64, keyed by the following stable enum slots in
`src/pg_wait_tracer.h`. The full provider reads and sums every possible CPU, without resetting
while producers are attached. Both new failures and the retained event aggregate
use nesting-safe atomic XADD. The control socket exposes the totals under the
stable names below, wherever the full provider already supplies the aggregate.

| Slot | Control metric | Failure site |
|---|---|---|
| 0 | ringbuf_wait_transition_drops_total | watchpoint trace output |
| 1 | ringbuf_exit_transition_drops_total | terminal backend trace output |
| 2 | ringbuf_marker_drops_total | execution/plan marker output |
| 3 | ringbuf_command_boundary_drops_total | command start/end output |
| 4 | ringbuf_lifecycle_init_drops_total | bootstrap lifecycle reserve |
| 5 | ringbuf_lifecycle_fork_drops_total | fork lifecycle reserve |
| 6 | ringbuf_lifecycle_exit_drops_total | exit lifecycle reserve |
| 7 | ringbuf_query_text_drops_total | PG17+ query-text reserve |
| 8 | ringbuf_pg13_query_text_drops_total | PG13 query-text reserve |

The existing aggregate remains the sum of failed trace-event output attempts;
lifecycle reservations gain separate accounting. Successful reservations later
discarded after a query-text read failure are not reserve failures. Counting read
failures belongs to subsequent work. The aggregate's meaning and provider selection
are unchanged; site totals also include lifecycle/query-text rings, so their sum
is not the legacy event-ring aggregate. Live snapshots can race with increments.
