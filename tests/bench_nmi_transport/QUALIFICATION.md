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
In v6.8, [`exc_debug_user`, traps.c:1012–1062](https://github.com/torvalds/linux/blob/v6.8/arch/x86/kernel/traps.c#L1012-L1062)
uses user entry handling and calls `notify_debug` before `local_irq_enable`;
[`notify_debug`:923–935](https://github.com/torvalds/linux/blob/v6.8/arch/x86/kernel/traps.c#L923-L935)
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
classes; product perf enablement and daemon readers; any product layout/core
change. The benchmark keeps a fixed CPU population. Its default gates include the
100 us adversarial arm and controls; none resolves an unproven source claim.

## Product failure-site map prepared for a later reader

The BPF object exports `ringbuf_site_drops` beside the existing `ringbuf_drops` map.
It is a PERCPU_ARRAY of u64, keyed by the following stable enum slots in
`src/bpf/pg_wait_tracer.bpf.c`. Read and sum every possible CPU, without resetting
while producers are attached. Both new failures and the retained event aggregate
use nesting-safe atomic XADD. No daemon/server metric or transport is changed here.

| Slot | Name suffix (`PGWT_RING_FAIL_`) | Failure site |
|---|---|---|
| 0 | WAIT_TRANSITION | watchpoint trace output |
| 1 | EXIT_TRANSITION | terminal backend trace output |
| 2 | MARKER | execution/plan marker output |
| 3 | COMMAND_BOUNDARY | command start/end output |
| 4 | LIFECYCLE_INIT | bootstrap lifecycle reserve |
| 5 | LIFECYCLE_FORK | fork lifecycle reserve |
| 6 | LIFECYCLE_EXIT | exit lifecycle reserve |
| 7 | QUERY_TEXT | PG17+ query-text reserve |
| 8 | PG13_QUERY_TEXT | PG13 query-text reserve |

The existing aggregate remains the sum of failed trace-event output attempts;
lifecycle reservations gain separate accounting. Successful reservations later
discarded after a query-text read failure are not reserve failures. Counting read
failures and exposing these maps as daemon metrics belong to subsequent work.
