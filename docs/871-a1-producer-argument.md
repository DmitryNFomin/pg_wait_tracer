# A1 producer protocol and kernel argument

Scope: experimental `exact_perf_mode != 0`, sequence mode N. This is producer
implementation and a falsifiable completion argument, **not merge certification**.
The default producer paths keep raw payloads, timestamp placement, accounting
and userspace map retirement. The frozen wire and v4 storage format are unchanged.
Attachment qualification is A2; production drain/merge admission is B.

The overhead profile supplied for the 2026-11-16 screen measured the adaptive
event-ring submit wakeup chain at about 21–22 µs per transaction on Ubuntu and
12–13 µs on EL8, inside PostgreSQL. Under `exact_perf_mode`, ordinary E-ring
TRACE records (markers, command boundaries and terminal transitions) and
L-ring EXIT/TEXT records submit with `BPF_RB_NO_WAKEUP`; both readers
are polled. L-ring FORK retains flags 0 (adaptive wakeup) to avoid delaying
attachment: it fires once per connection, not per transaction. E/L BOUNDARY
cuts use `BPF_RB_FORCE_WAKEUP`, at most one wake per ring per round. Adaptive
wakeup only wakes when the consumer position equals the submitted record's
position, which preceding NO_WAKEUP DATA can prevent. CPU perf
boundaries/acknowledgements and the INIT perf wake-up hint
are unchanged. Each main-loop pass polls the transport and attempts the common
E drain (`src/daemon.c`, `pgwt_daemon_run`), with a 10 ms maximum outer epoll
timeout (0 after a budget yield); the inner completion wait is at most 1 ms
(`src/percpu_transport.c`, `linux_wait`). These bound sleeping, not total
record residence under load: the existing 50 ms pass and 10 ms drain budgets
may defer work and mark backlog for immediate resumption. The completion fence
still requires consumed E/L boundaries, every higher-round CPU ack and frozen
attempt targets; a short E/L target holds until the mmap consumer position
reaches the fixed producer-position cut (`complete_round`, `linux_positions`),
independently of wakeups. Flag-off submits are unchanged. The re-screen on
EL8/Ubuntu with PG13/PG18 is pending; no overhead result is claimed here.

## Wire and producer inventory

Each row below emits the frozen 32-byte v1 header followed by the named payload.
Flags and q are zero. CPU is the emitting CPU. `seq` starts at 1, independently
for each `(CPU,destination)`, and is allocated before reservation/output.
An unsuccessful attempt consumes its sequence. Cookies are copied into bytes
before publication; a reader must never replace them using today's PID entry.

| Producer | Destination | Kind / payload / complete wire bytes | Cookie |
| --- | --- | --- | --- |
| `on_watchpoint` wait transition | CPU perf | TRACE / 48 / 80 | producing state cookie |
| `emit_marker` via execute/plan USDT start/done | E ring | TRACE / 48 / 80 | producing state cookie |
| `on_report_activity` command flip | E ring | TRACE / 48 / 80 | producing state cookie |
| `on_exit` terminal transition | E ring | TRACE / 48 / 80 | exiting state cookie |
| `on_fork` | L ring | LIFECYCLE / 24 / 56 | **new child's** cookie, not postmaster's |
| `on_bootstrap` | state only, plus a wake-up hint | **no envelope, reservation or attempt** | preserves fork-seeded cookie |
| `on_exit` lifecycle EXIT | L ring | LIFECYCLE / 24 / 56 | exiting state cookie |
| `on_report_query_id` query text | L ring | TEXT / 272 / 304 | producing state cookie |
| `on_executor_start` PG13 text | L ring | TEXT / 272 / 304 | producing state cookie |
| `on_transport_helper`, the E/L issuer thread | E and L | BOUNDARY / 16 / 48 | 0 (no backend) |
| `on_transport_helper`, each registered helper | its CPU perf | BOUNDARY / 16 / 48 | 0 (no backend) |

`on_sched_switch` and the query/phase attribution helpers do not publish records.
Lightweight accumulation does not publish DATA. Full exact mode is the intended
consumer of this experiment; no sampled/tiered certification is asserted.

The BPF `lifetime_allocator` ARRAY[1] is the only allocator. Only the selected
postmaster's synchronous `on_fork` path increments it. The selected postmaster
is a single task, so these increments are serialized, even if it migrates.
The counter starts at zero and saturates at UINT64_MAX; exhaustion counts
`fork_state_update_fail` and refuses allocation rather than reissuing a value.
Userspace must not reset/write this map during a capture. A failed state insert
is counted independently of FORK publication. Every fork replaces stale state.
State created by any other path has cookie zero and has no fork proof.

Flagged exit reserves its required outputs and copies the cookie while the
exiting task still owns its PID, then deletes state/attribution/seed entries in
BPF. Delayed userspace EXIT handling skips those PID-keyed deletes. The only
preseed change is copying `prev.cookie` under `d->exact_perf`; mode zero writes
zero. This **does not** prove task-bound attachment, concurrent preseed safety,
idempotent directly-attached INIT, or lifetime-bound userspace fd closure. Those
remain A2. Recovered stored INITs are not turned into wire INIT records.
Under the mode every new backend uses the 25b fork-seed route; FORK+INIT
admission is unreachable and its first interval remains uncertified. An early
marker without the completed proof VOIDs. This follows the accepted INIT
decision in issue 871 comment 6874906593, not an A1 certification claim.

## Reservation and failure paths

Markers reserve E before taking `now`. A failed reserve counts both the site
and event-ring aggregate plus the transport failure, then still calls the
phase-edge update with the fresh `now`. Command flips likewise reserve first
when a boundary is needed, then update command/query attribution at `now` even
if reservation failed. No old attribution edge is silently kept because its
publication failed. A successful reservation that becomes unusable at the
admission recheck is withdrawn: it is counted in `transport_discards`, then
released. It is not a failure and does not increment `transport_failures`,
the logical ring aggregate or the site counter (see "Failures and discards"
below). The command
producer snapshots its boundary condition once so a userspace state replacement
cannot strand a reservation or create an unreserved boundary.

EXIT reserves E (when an exact terminal transition is required) and L before
one shared timestamp. If either fails, both outputs are suppressed: the failed
reservation is counted, and its successful partner is discarded and counted
at its own destination/site. It always retires task state afterwards. FORK
already reserved before timestamp and now reserves the enveloped size.
`on_bootstrap` stamps/stores INIT facts in the existing fork-seeded entry once,
publishing `init_stored` after those facts. It does not touch L or consume a
transport attempt, even when L cannot reserve. Missing state still increments
`INIT_NO_STATE`. Mode zero retains its existing raw hint publication and failure
accounting. Stored INITs never become synthetic wire records.

Text reservations precede reading/copying the string. An unreadable text is
discarded and counts a transport failure plus the text site (it is a lost
text). An empty but valid string is discarded and counts a transport discard:
it consumed an attempt and nothing was lost. Wait DATA
uses `bpf_perf_event_output(..., BPF_F_CURRENT_CPU, ...)`; nonzero return counts
the existing wait site and `event_ring_drop` aggregate, plus a CPU transport
failure. Helper and E/L boundary failures count the destination transport slot.

B must read `transport_attempts`, `transport_failures` **and**
`transport_discards` (PERCPU_ARRAY[3], u64, CPU/E/L keys 0/1/2). They
supplement, not replace, the fifteen logical counters. A failed boundary
cannot disappear. INIT has no wire
attempt under the mode; the logical `lifecycle_init_drop` component still counts
bootstrap observations that were never attached, including no-opportunity loss
for merge certification. A helper's
return zero is not delivery evidence on old perf implementations. Reconcile
attempts against consumed records after completion, including the terminal gap.

### Failures and discards (changed by the B integration round)

Until the integration round every attempt that delivered no record was counted
in `transport_failures`, including a reservation withdrawn by design. The
daemon cannot tell those apart from a loss, and a sequence number that no
record carries then reads as a gap. They are now two counters, per
(CPU,destination):

    attempts = delivered + failures + discards

- **Failure** (`transport_failures`): the record did not arrive. A refused
  ring reservation, a refused perf output (DATA or a boundary), an unreadable
  query text, and the granted half of a failed EXIT pair. Every site except a
  failed boundary also increments one of the fifteen logical counters.
- **Discard** (`transport_discards`): the producer withdrew a granted
  reservation and nothing was lost: a marker or command boundary whose state
  vanished or whose admission closed between the reservation and the recheck
  (the default path publishes nothing there either), and an empty query text.
  A discard increments no logical counter.

The discard is counted **before** `bpf_ringbuf_discard` releases the
reservation. A reader that has consumed past that reservation, or a later
record of the same (CPU,destination), therefore finds it counted, without
relying on the helper fence. The standalone probe and its live driver accept
`attempted == delivered + discarded` in the clean arms and print the discards.

Measured (the daemon's clean runs on EL8 4.18 and Ubuntu 6.8, PG13 and PG18,
`-c16` and `-C`; see "Daemon integration" below): 0 failures and 0 discards in
every run. The discard path is therefore exercised only by the host harness.

### What the 4.18 verifier needs (measured on EL8)

Measured on Rocky 8.10, kernel `4.18.0-553.157.1.el8_10`, clang 21.1.8, with
the two earlier rounds of this unit (ab9886d7). With `exact_perf_mode` on the
object did not load; libbpf stopped at the first rejected program:

```text
libbpf: prog 'on_exit': BPF program load failed: Invalid argument
196: (85) call bpf_ringbuf_reserve#131          ; transport_reserve_lifecycle, L
197: (bf) r8 = r0
198: (55) if r8 != 0x0 goto pc+22               ; the first NULL test of `life`
from 198 to 221: ... R8=mem(id=0,ref_obj_id=11,off=0,imm=0) ... refs=11
...
313: (15) if r8 == 0x0 goto pc+152              ; `if (life)` again, in the failure arm
from 313 to 466: ... R8=mem(id=0,ref_obj_id=11,off=0,imm=0) ... refs=11
468-479: three bpf_map_delete_elem calls
636: (95) exit
Unreleased reference id=11 alloc_insn=196
processed 220 insns (limit 1000000) ...
```

At 198 the verifier has already refined `r8` from "pointer or NULL" to a
non-NULL `mem` that owns reference 11. At 313 the compiled code tests the same
register against zero a second time. This verifier does not decide that test:
it also walks the `r8 == 0` side, with `r8` still typed `mem` and reference 11
still held, and that side skips the submit and the discard and reaches `exit`.
The source had `if ((!trace || out) && life) {...} else { if (out) ...; if
(life) ... }`: a pointer kept in a variable across a merge of paths and tested
again. The first test is what releases the reference on the NULL side; a second
test on a pointer already known non-NULL releases nothing on either side.

What was read and what was not: the verdict and the register states above are
the kernel's own log (`tests/results/session-20261114/a1-fresh/el8/
r0-baseline-modeon-verifier.log`, reproduced before any change). The reason
this verifier walks that branch was **not** read in its source:
`kernel/bpf/verifier.c` of this vendor kernel is not among the extracted files.
Upstream decides such a test only for the pointer types its `is_branch_taken`
knows to be non-NULL, and a ring-buffer `mem` pointer joined that set late;
that is an inference about the vendor kernel, not an observation of it. Ubuntu
6.8 was not shown the old shape, so nothing is claimed about whether it would
have rejected it.

**The rule this file now follows.** A reservation pointer is tested against
NULL exactly once, immediately, by the function that reserved it. Everything
that fills, submits or discards the reservation is lexically inside the
non-NULL arm of that one test, and every path of that arm ends in exactly one
submit or one discard. The pointer is never stored in a variable that another
path sets to NULL, and it is never tested again. `transport_reserve_trace` and
`transport_reserve_lifecycle` therefore take the attempt number and reserve
but do not test; their callers do. Code that both outcomes need (the marker's
attribution edge, the command gate's edges) is an inline function called from
each arm, so no path merges while a reservation is held. Because there is no
second test, there is no branch for the verifier to walk with the reference
held; this does not depend on the compiler removing a redundant test.

The same cause was present in three more places that the first rejection
hid: `emit_marker` (`wire` tested again at each of three exits),
`on_report_activity` (`if (wire)` inside the emission block) and both query
text producers (`evt` derived from `wire` and tested after the merge with the
mode-zero reservation). All were rewritten to the rule, not only `on_exit`.
The mode-zero code is 2f2c211d's text, unchanged: against that commit the
only lines removed from this file are one comment and `on_watchpoint`'s single
`emit_event` call, which is now the `else` of the mode branch.

A second verifier limit was found in the test-only object. The falsifier's
delay loop was `for (i < 65536) { ended = ktime(); if (ended - started >=
spin) break; }`. The 4.18 verifier accepts a bounded loop, but rejected this
one:

```text
libbpf: prog 'on_fork': BPF program load failed
The sequence of 8193 jumps is too complex.
processed 81950 insns (limit 1000000) max_states_per_insn 4 total_states 1240 ...
```

An exit that depends on data leaves one pending verifier state per iteration,
and the limit is 8192. The loop now reads the clock in fixed blocks (64 blocks
of 500 rounds of eight reads, trip counts the verifier knows) and compares
once per block, so at most 64 states are pending. It loads on 4.18.

Because the verifier folds the read-only configuration constants and never
walks code they switch off, a load says nothing about code behind a gate that
was off. `tests/test_transport_producers --probe --verifier-matrix` therefore
loads every program of the object alone, with the mode off and on, with every
gate turned on (command boundary, both text routes, PG13 edges, CPU
accounting), and prints one verdict per program. The live test runs it for
the production object and requires 26 loads and no rejection; it runs it for
the falsifier object too and refuses if that object is rejected.

### Complete reserve-site audit

Command: `grep -n 'bpf_ringbuf_\(reserve\|submit\|discard\|output\)' src/bpf/pg_wait_tracer.bpf.c`.
There are ten reserve expressions. The portable shim rejects a double
submit/discard and checks that every successful reservation was released
before it resets a case.

| Reserve expression | Owner, its one NULL test, and every path after a successful reserve |
| --- | --- |
| `transport_reserve_trace`, E | Does not test. Owners: `transport_marker`, `transport_activity`, `transport_exit` (below). |
| `transport_reserve_lifecycle`, L | Does not test. Owners: `on_fork`, `transport_exit` at three sites (below). |
| `transport_boundary`, E branch | Tested once (`if (!cut) { count; return; }`); then fills and submits once. |
| `transport_boundary`, L branch | Same; the destination branches are exclusive. |
| `transport_text`, enveloped L | Tested once; unreadable or empty text discards once, otherwise submits once. Used by both text producers under the mode. |
| `on_bootstrap`, raw L | Mode zero only, unchanged: fills raw INIT and submits once; NULL counts the hint drop. |
| `on_fork`, raw L | Mode zero only, unchanged: fills FORK and submits once. |
| `on_exit`, raw L | Mode zero only, unchanged: fills EXIT and submits once. |
| `on_report_query_id`, raw L | Mode zero only, unchanged: discard on unreadable/empty text, otherwise submit. |
| `on_executor_start`, raw L | Mode zero only, unchanged: same two exclusive branches. |

Owners under the mode:

- `on_fork`: L tested once; NULL counts the site and the transport failure
  and returns; otherwise header, payload, one submit. The test-only
  forced-failure branch reserves nothing.
- `transport_exit`, four outcomes, each ending at `retire` with nothing held:
  1. terminal transition required, E refused: E failure counted; L is still
     attempted, tested once, discarded if it was granted, and counted;
  2. E granted, L refused: L failure counted, E discarded once and counted;
  3. both granted: one timestamp after both reservations, E submitted once,
     then L submitted once;
  4. no terminal transition: L tested once; NULL counts, otherwise one submit.
- `transport_marker`: E tested once. NULL counts the site, the aggregate and
  the transport failure, then runs the attribution edge with a fresh
  timestamp and returns. Non-NULL: the same edge decides between one submit
  and one discard (state gone or admission closed; the discard reconciles the
  attempt only and is not a ring drop).
- `transport_activity`: reserves only when the caller's saved admission
  condition is true. NULL counts and still runs the command and query edges.
  Non-NULL: open admission submits once, closed admission discards once. The
  entry's `wp_live` is not read again after the reservation.
- `on_bootstrap` under the mode reserves nothing.

`emit_event` uses `bpf_ringbuf_output` in mode zero, which hands no
reservation to the program. Perf DATA and CPU boundaries use
`bpf_perf_event_output`, also without ring ownership.

## On-demand helper protocol for B

Set `helper_tgid` before load. Register `helper_cpus[TID] = CPU`, with one pinned
SCHED_OTHER helper per online CPU. One registered, pinned thread issues the
E/L boundaries: the thread named by the rodata `helper_el_tid`, or the process
leader when that is 0. The standalone probe pins its leader and leaves it 0.
The daemon's leader is its event loop and is not pinned, so the daemon names
its first helper thread. An unregistered TID, foreign TGID or wrong current CPU
publishes nothing. Perf output always selects `BPF_F_CURRENT_CPU`, never a CPU
provided by userspace. The helper program is a task-context `getppid` syscall
tracepoint, copied from the bench mechanism at `fcda6b39`; user helpers park on
untimed condition variables and call the syscall only after a request.

`transport_ctl.round=0` is inactive. For an E/L request, update the single control
entry with a nonzero round and a new nonzero `el_request` token, then wake the
issuer thread. It reads `w` before reserving each E/L boundary and copies
round into that boundary. It attempts each E/L pair once per token. A failed
boundary invalidates that request; a new token is needed for another attempt.
Normal CPU helpers publish one CPU boundary per requested syscall. There is no
separate acknowledgement record.

Consume E and L of round r completely, **then** advance round, wake every
participating CPU helper, and consume each CPU's own boundary with round > r.
Only after all those acknowledgements may B reread the counters and consider a
frontier. No equality test on two snapshots, elapsed timeout or idle period is a
completion substitute. A missing/failed acknowledgement means no frontier.
The probe implements this ordering at its terminal checkpoint. Its periodic
requests during the workload are test stimulus, not a proposed daemon timer.

## Kernel argument: premises, evidence and open points

Actual EL8 source inspected: the lead's extracted files under
`tests/results/session-20261111/m0c/el8/ksrc/`, kernel
`4.18.0-553.157.1.el8_10`. This directory contains the debug entry, ring buffer,
preemption header and x86 entry assembly; it does not supply every BPF dispatcher,
map, perf, timekeeping or JIT source. Upstream v4.18/v6.8 references below are
**comparisons**, not proof that a vendor build has identical dispatch code.

1. **Non-preemption and non-migration from reservation through accounting.**
   The actual EL8 `arch/x86/kernel/traps.c:867-896` enters NMI accounting in
   `do_debug`, including user-origin watchpoints; `include/linux/preempt.h:41-53,
   103,174-178` defines the NMI/preemption mask and preemption disable. This
   supports excluding task scheduling during watchpoint DATA and bootstrap.
   Actual `kernel/bpf/ringbuf.c:322-326` confirms the try-once NMI lock that the
   perf route removes from DATA. Bootstrap stores facts without taking a ring
   lock under the mode; this removes its NMI ring reservation too.

   The remaining permitted producers are USDT/uprobe markers, activity and query
   uprobes, sched FORK/EXIT, and the helper syscall tracepoint. Upstream
   [v4.18 kernel/trace/bpf_trace.c:40-81](https://github.com/torvalds/linux/blob/v4.18/kernel/trace/bpf_trace.c#L40-L81)
   disables preemption around the complete program invocation;
   [trace_uprobe.c:1093-1105](https://github.com/torvalds/linux/blob/v4.18/kernel/trace/trace_uprobe.c#L1093-L1105)
   calls it for uprobes;
   [kernel/events/core.c:8257-8272](https://github.com/torvalds/linux/blob/v4.18/kernel/events/core.c#L8257-L8272)
   calls it for perf tracepoints, including sched and syscall events.
   [core.c:8494-8514](https://github.com/torvalds/linux/blob/v4.18/kernel/events/core.c#L8494-L8514)
   also brackets perf-event BPF with preemption disable.
   **Open:** compare those complete dispatchers and the running preemption
   configuration with this EL8 vendor kernel before claiming the premise for
   every producer. The partial vendor source alone does not establish that.

2. **Why the helper cannot pass an unfinished producer.** Conditional on (1),
   a task-context helper pinned to CPU i cannot run there between another
   producer's reservation/timestamp and its output/failure increment. A
   user-mode write watchpoint cannot interrupt a kernel-resident producer:
   permitted DATA comes only from the selected backend's user instruction.
   Thus the same destination's producer invocations do not nest on one CPU;
   E's non-NMI producers cannot be interrupted by user DATA, and bootstrap's
   user-origin debug exception cannot interrupt a kernel L producer. This also
   justifies XADD-then-load sequence selection on kernels lacking fetch-XADD.
   It does not qualify kernel watchpoints, hrtimer helpers, other producers or
   arbitrary interrupting BPF programs using the same perf map.

   On upstream 6.8, ordinary uprobes use a migration-only program runner
   ([trace_uprobe.c:1347-1354](https://github.com/torvalds/linux/blob/v6.8/kernel/trace/trace_uprobe.c#L1347-L1354),
   [include/linux/bpf.h:2009-2045](https://github.com/torvalds/linux/blob/v6.8/include/linux/bpf.h#L2009-L2045));
   preemption disable below that call does not protect the BPF invocation.
   Task-context helper exclusion must **not** be asserted under `preempt=full`.
   The driver records the dynamic mode, command line and available boot config.
   [v6.8 kernel/sched/debug.c:232-250](https://github.com/torvalds/linux/blob/v6.8/kernel/sched/debug.c#L232-L250)
   prints the active dynamic mode in parentheses: `none voluntary (full)`.
   The driver reads that selection, including runtime changes. If debugfs is
   missing/unreadable it uses boot config only for a static kernel; a dynamic
   kernel without a readable selection is **unknown**, regardless of cmdline.
   It refuses the completion claim for full or unknown. Ubuntu runs remain mechanism
   and mode-zero controls; they are not an Ubuntu certification claim.

3. **Publication and counter visibility.** Actual EL8 ringbuf
   `kernel/bpf/ringbuf.c:358-362` publishes BUSY reservation positions with a
   release store; `:388-402` clears BUSY with `xchg`. A consumer must pass every
   preceding reservation and must not skip BUSY. Counter increments are atomic
   XADDs (also on failure paths). Once each CPU has completed such a section
   before its requested acknowledgement, B's later counter map reads must see
   those increments through coherent memory and the perf/ring publication
   barriers. This is a conditional ordering argument, not evidence that a
   logical counter lookup can never fail. B must retain validity errors.
   **Open:** actual vendor perf output, per-CPU map copying/JIT barriers and the
   linked libbpf acquire/release implementation must be identified/reviewed.
   The bench explicitly leaves that matching-vendor visibility step open.

4. **Complete CPU population.** Requests cover every online CPU, including
   CPUs with no DATA. The probe compares the initial allowed affinity population
   with the online count, refuses restricted coverage, registers each helper,
   and checks every CPU's higher-round ack. B must retain a fixed online set;
   CPU hotplug and `nohz_full` remain outside this unit. CPU affinity and a
   received ack do not prove that an omitted CPU had no older invocation.

5. **Clock order.** The completion fence is causal through userspace and does
   not compare cross-CPU clocks to order the requests. It does **not** supply
   the separate clock premise needed by E/L: a later reservation must not
   obtain a semantic timestamp smaller than a preceding boundary's w.
   [v4.18 kernel/bpf/helpers.c:107-110](https://github.com/torvalds/linux/blob/v4.18/kernel/bpf/helpers.c#L107-L110)
   selects the fast monotonic accessor. Its timekeeping comments
   ([v4.18:407-436](https://github.com/torvalds/linux/blob/v4.18/kernel/time/timekeeping.c#L407-L436),
   [v6.8:459-488](https://github.com/torvalds/linux/blob/v6.8/kernel/time/timekeeping.c#L459-L488))
   describe regression during slope updates, including cross-CPU observations.
   **Unestablished:** the complete actual-kernel, clocksource and hypervisor
   cross-CPU ordering argument. Measuring zero regressions does not prove it.
   This remains a U2 blocker even if the delayed-failure falsifier passes.

## Falsifier and live probe

`test_fail_spin_ns` exists only in the separately compiled
`tests/transport_falsifier.bpf.c` object (`PGWT_TRANSPORT_FALSIFIER`). The
production translation unit excludes the knob, the test map, the spin function
and the FORK injection block at preprocessing time, so it contains no test
loop. A nonzero value forces one L FORK reservation failure per producing CPU
(the reserve is deliberately bypassed), consumes its attempt, reads a would-be
timestamp into `test_spin.started`, spins, then counts the failure and
publishes `test_spin.completed`. No wire record is invented for the failed
publication. The spin reads the clock in fixed blocks and stops at the first
block boundary at or past the requested time, or after the last block (see
"What the 4.18 verifier needs" for why it is not compared on every read). The
test requests 20 ms; the loader measures what it got and refuses less than
1 ms. The live driver builds this object with its own skeleton and loader in
a temporary directory, using the project's resolved compiler, libbpf and
bpftool. Clean arms load the normal production object.

**What counts as evidence.** For each CPU that spun, the loader requires:

1. no boundary of that CPU's helper with `started <= w <= ended` (one such
   boundary means the helper ran inside an unfinished producer: the fence is
   falsified and the test fails, whatever the other CPUs show);
2. a wake of that CPU's helper that began and returned inside the spin (the
   clock is read before and after the wake call), so the helper was runnable
   on that CPU while the producer was unfinished; and
3. a boundary of that CPU, carrying that request's round or a later one,
   with `w > ended`, so the helper did run, its records do arrive, and it ran
   only afterwards.

Without (2) and (3) the result is a refusal (exit 78), never a pass. A
boundary from an older round than the request is stale and does not count.
The identical round is not required: rounds only grow and the round is read
by the helper's program when it runs, so a later round is still that helper's
later run. Requiring equality refused correct runs whenever the round
advanced between the wake and the helper's first run, which is routine when
a requester shares the helper's CPU.

**Who issues the wakes.** The loader's main thread is pinned to one CPU and
cannot run while a spin occupies that CPU, so it cannot be the source of the
wakes. Two requester threads are pinned to two different CPUs. Each wakes the
helpers of all the *other* CPUs about every 100 microseconds and stamps each
wake. Forks come from one postmaster, so one CPU spins at a time, and
whichever CPU that is (the loader's own included) at least one requester is
free to challenge it. With fewer than two online CPUs the arm refuses. A
helper makes one `getppid` call per wake in every arm. These wakes are test
stimulus, not a proposed daemon timer.

It also checks completion and transport conservation after stop. A measured
spin under 1 ms, missing stimulus, or a verifier rejection of the test object
is exit 78 / REFUSED, never PASS. No sleepable helper or grace period may
substitute for this section.

With the usual 1 MiB perf buffer per CPU this arm passed on EL8 4.18 and
failed on Ubuntu 6.8 with
`PERF_RECORD_LOST` on every CPU: 148,731 wakes in 5 s there against 36,903 to
50,745 in eight EL8 runs, about 17,300 to 17,800 records delivered per CPU
(one full buffer) and 60,626 lost. On 6.8 this loader drains slowly under connection
churn; it attached 264 of 8,837 backends in the clean `-C` arm. The likely
reason, not measured, is that its main thread stays in the lifecycle-ring
consume loop while forks keep arriving. The falsifier arm therefore uses a
16 MiB buffer per CPU (262,144 boundary records; the busiest CPU produced
41,781 in that failed arm), which the arm cannot fill even if the main thread
never drains. The cadence is unchanged, so on EL8 the only difference from
the measured runs is the larger buffer.

Build on the box with the normal project and test recipes, then run:

```sh
make build-tests
sudo python3 tests/test_transport_producers_live.py --pid POSTMASTER_PID --seconds 20
# Same test is in tests/run_all.sh's live tier.
```

The driver needs no PostgreSQL server headers. PG17+ publishes a pointer
straight to `wait_event_info`; for PG13 it uses the daemon's own validated
x86_64 offsets (`src/discovery.c`, `pgwt_detect_pgproc_wait_offset` and
`pgwt_detect_pg13_query_offsets`), and the portable test fails if the copy in
the driver stops matching that file. ELF symbol and file offsets are resolved
from the running binary. It creates and drops a private pgbench database and
runs, in order:

1. the verifier matrix of the production object (26 loads, none rejected);
2. `pgbench -c16 -j4`, 20 s, clean;
3. `pgbench -C -c16 -j4`, 20 s, clean;
4. real initial-namespace PID reuse with both lifetimes' records retained;
5. the INIT-publication mutant under `pgbench -C`, 5 s;
6. the verifier matrix of the falsifier object, then the delayed-failure arm
   under `pgbench -C`, 5 s;
7. a control: the same arm with the stamped challenges switched off
   (`--no-challenge`), which must end in a refusal.

PG14-16 are excluded by `run_all.sh` using its existing `exclude_test` matrix
convention; unsupported PG is not a new exit status. The standalone loader
owns both rings and the perf reader and prints a per-CPU/destination
attempted/delivered matrix. The libbpf RAW callback includes alignment
padding: 80-byte DATA arrives as 84 raw bytes, 48-byte boundaries as 52; the
portable test pins both lengths.

The clean arms assert load success, framing, zero q/flags, lifetime
FORK-cookie bindings, strict attempt sequences, zero LOST and zero failures on
every destination, attempted equal to delivered per CPU and destination after
stop, no later E/L timestamp below w, and a higher-round CPU ack from every
online CPU after consuming E/L. Each clean PG arm also asserts
`init_records == 0` on L, `init_stored > 0` in the cohort, `attached > 0` and
`trace > 0`. The self-reuse arm deliberately holds both lifetimes' records
until the second child has overwritten and retired its PID state. It moves
`ns_last_pid` on the disposable box, requires the same initial-namespace PID
to be reused, and checks both different FORK cookies. A nested PID namespace
or inability to move that cursor is a refusal, not a vacuous reuse pass.
Default-path real-PG regression evidence still comes from the ordinary
flag-off box suite; the host harness additionally pins raw mode-zero record
sizes and retained-state behaviour.

**Non-vacuity of "zero INIT records on L".** That assertion says something
only if `on_bootstrap` fired. The loader arms a bootstrap watchpoint when it
consumes a backend's FORK, and a backend normally publishes its wait pointer
sooner than that: in one EL8 run the loader saw a stored INIT for 1 of 18
backends in the 16-client arm and for 5 of 10,351 under `-C`, and in the next
run for none of 17, which failed the arm. The driver therefore creates four
*held* backends per PG arm: it connects and withholds the startup packet for
300 ms. The postmaster forks the backend at connect, and the backend reads the
startup packet before `InitProcess()` publishes the pointer, so the watchpoint
is armed first by construction. The loader prints each backend whose stored
INIT it observed, and the arm requires every held backend among them. The
falsifier arm holds none, because it turns the first fork on each CPU into a
forced failure and that backend has no FORK record.

**The loader's attach predicate.** The loader attaches a wait watchpoint only
when the backend's pointer targets a shared mapping, which is the daemon's own
rule (`pgwt_addr_is_shared`): on PG17+ the pointer is non-zero from the first
instruction and targets a process-local placeholder until `InitProcess()`.
Attaching on non-zero alone put the watchpoint on the placeholder and closed
the bootstrap watchpoint early. The attachment is still stimulus only, not an
A2 proof: an already-exited or too-short task can be uninstrumented, and its
FORK/EXIT transport still participates in reconciliation. INIT storage is
counted once per backend, only when its `/proc/PID/cmdline` title names the
driver's own database, so unrelated postmaster children cannot satisfy it.

**The INIT-publication mutant.** It puts the L publication back into
`on_bootstrap`, in the production reservation shape so that it loads on 4.18.
That reservation is taken in NMI context, where 4.18 refuses it when the ring
lock is contended (11 to 33 per 120 s under `-C` in the issue's measurement),
and each refusal is an attempt with no delivery. So that every run meets
refusals, on every kernel, the mutant also gives up the reservation for odd
PIDs and counts it exactly as a refused one. The driver holds backends until
it has an odd and an even PID. The loader's `--init-mutant` option accepts on
L exactly the failures counted at the INIT site, per CPU, and still fails on
any other failure on any destination. The arm must then show a published INIT
and a refused one, every other clean assertion green, and fail exactly
`zero INIT records on L under exact_perf_mode`. A load failure or any other
assertion is not a killed mutant.

## Portable checks and deliberately broken variants

The registered `test_transport_producers` unit runs the pure header/allocator
checks and invokes `transport_producers_host.py`. That driver extracts unchanged
production function bodies into kernel-helper shims, compiles them as host C,
and runs both genuine successes and reserve/read/output failures. It does not
compile BPF bytecode. It checks every enveloped producer, paired EXIT discard,
attribution updates on failure, fork replacement and retained DATA/FORK cookies,
helper admission, allocator exhaustion, and raw flag-off migration cases.

Each mutant is compiled successfully and must reach its named failing assertion:

| Mutant | Distinguishing assertion |
| --- | --- |
| Marker timestamp taken ahead of its reservation | `timestamp after reserve` |
| FORK cookie selected from the current emitting PID entry rather than the child lifetime | `queued cookie survives pid reuse` |
| Reused PID's DATA keeps cookie 1 | `DATA cookie across PID reuse` |
| E attempt number taken only after the reservation succeeded | `marker fail attribution coherent` (includes consumed attempt) |
| Helper ignores expected CPU | `helper wrong CPU refused` |
| Command publication reads `wp_live` again after the reservation | `command saved admission releases reservation` |
| Bootstrap restores the L reservation/publication | `zero INIT records on L under exact_perf_mode` (host and live) |
| Command admission discard increments logical ring losses | `command admission discard is not a ring drop` |
| EXIT with E refused leaves its unused L reservation held | `partial exit discarded and counted` |

The retained-record box test is the read-time/current-PID-resolution control:
it defers drain across real initial-namespace reuse and requires records to lie
in their own cookie's FORK/EXIT interval (L metadata is also checked by FIFO
lifetime position). Resolving the old records from the current or deleted PID
entry cannot satisfy those checks.

## Box validation (2026-11-14)

Evidence folder (on the lead's Mac, git-ignored):
`tests/results/session-20261114/a1-fresh/{el8,ubuntu,mac}/`. Each live log
starts with the kernel, the PostgreSQL major, the postmaster pid and port, and
the tree it was built from.

What was measured while this shape was being built, on a throwaway Rocky 8.10
VM (kernel `4.18.0-553.157.1.el8_10`, 4 vCPU, the gate snapshot image as it
is, no package added):

| Step | Tree | Result |
| --- | --- | --- |
| Baseline, mode on | ab9886d7 (the two earlier rounds) | rejected at `on_exit`, `Unreleased reference id=11 alloc_insn=196`, as in the smoke run |
| Producers rewritten to the reservation rule | work in progress | whole object loads with the mode on |
| Verifier matrix, every gate on | work in progress | 13 programs x 2 modes = 26 loads, 0 rejected |
| Falsifier object, per-read exit | work in progress | `on_fork` rejected, `The sequence of 8193 jumps is too complex` |
| Falsifier object, block loop | work in progress | 26 loads, 0 rejected; measured spins 11.7 to 17.3 ms for 20 ms requested (the loop ends after its last block) |
| Image without server headers | gate image 451329433 | `ls /usr/pgsql-*/include/server/postgres.h`: no such file; the driver no longer needs one |

**Mode off, compared with 2f2c211d on the kernel.** The daemon of each tree
was run in `--mode full` (the mode is off by default) against PG18 and PG13,
and every loaded program was dumped as the kernel holds it after verification
(`bpftool prog dump xlated`; dumps and `SUMMARY.txt` are in
`el8/modeoff-xlated/`). For all twelve programs of 2f2c211d, on both
PostgreSQL versions, the multiset of kernel helper calls is identical in the
two trees. The instruction counts are not:

| Program | 2f2c211d | this unit | What the difference is |
| --- | ---: | ---: | --- |
| `on_sched_switch` | 56 | 56 | none |
| `on_bootstrap`, `on_exit`, `on_exec_start`, `on_exec_done` | 84, 220, 236, 237 | +2 each | the load of the `exact_perf_mode` constant; the branch itself is removed |
| `on_plan_start`, `on_plan_done` | 224, 225 | +3 each | the same load, and one more move |
| `on_executor_start` (PG13) | 266 | 268 | the same load |
| `on_report_query_id` | 234 | 238 | the same load, and register moves |
| `on_fork` | 68 | 73 | two such loads and one store zeroing the new `cookie` field |
| `on_watchpoint` | 415 (PG18), 407 (PG13) | +7 | the load, the context pointer kept in a register, renumbered stack slots |
| `on_report_activity` | 294 (PG18), 416 (PG13) | 331, 454 | the compiler no longer inlines `pgwt_exact_merge_attr` there and calls it as a BPF subprogram, as 2f2c211d's `on_watchpoint` already does |

`on_transport_helper` is new; with the mode off it is five instructions and
the daemon never attaches it. So the mode-off programs are not
instruction-identical to 2f2c211d: they make the same helper calls, and the
one change in code generation beyond a constant load is the out-of-line
function in `on_report_activity`. This was measured on EL8 only. The legacy
source text itself is unchanged (see "What the 4.18 verifier needs").

Things the box showed that the Mac could not:

- `clang` 21.1.8 compiles `__sync_fetch_and_add` in this file to
  `atomic64_fetch_add`, and this kernel's verifier accepts it. The comment in
  `transport_producer.h` about kernels lacking the fetching form describes
  older kernels, not this one.
- The registered portable test (`test_transport_producers`, no arguments)
  did not compile on Linux: glibc's `<stdlib.h>` declares its own
  `on_exit()`, and the harness compiles the production program of that name
  as host C (`conflicting types for 'on_exit'`, gcc 8.5). It had only ever
  been run on the Mac. The harness now compiles it under another name, and
  the test passes on the box with every mutant killed.
- The 16-client arm's non-vacuity assertion depended on a race (see
  "Non-vacuity" above), and the loader attached to PG17+'s placeholder
  address. Both are fixed in the loader and driver, not in the producers.
- On this image the PG13 service starts through the `postmaster` symlink, so
  the process is named `postmaster` and `find_postmaster` (shell and Python,
  both `pgrep -x postgres`) does not find it. `run_all.sh --pid` works. This
  unit does not change those resolvers.
- `tests/Makefile` rebuilds `test_transport_producers` only when the `.c`
  file, the wire header or the skeleton changes, not when
  `tests/transport_probe*.h` or `src/bpf/transport_producer.h` does. A box
  check always cleans first, so it is unaffected; an incremental build after
  editing only those headers runs a stale loader. `tests/Makefile` is outside
  this unit.

Not exercised on a box by this unit: the phase markers (`emit_marker` through
the USDT programs). The loader attaches no USDT probe, so the marker path is
verified to load (matrix) and checked for ordering and accounting only by the
portable harness. The cross-CPU clock premise of point 5 and the vendor
dispatch and visibility points of 1 and 3 above remain open for U2.

The A2 boundary is unchanged: the earlier rounds copy `prev.cookie` during
flagged preseed and skip the delayed userspace map deletion under the flag;
this round changes no backend attachment code in `src/`.

## Daemon integration (B, 2026-11-14)

The daemon and these producers had never run together. On EL8 4.18 the daemon
at 86a06d59 never completed a round: `helper_cpus` stayed empty, no CPU's
`helper_progress` advanced, `transport_ctl` stayed at round 1, and the run
ended in `VOID(allocation_fail)` with a 5.7 s held age
(`tests/results/session-20261114/b-integration/el8/r0-base-pg18-c16/`).
What each side now does for one round, in order:

1. **Before load.** The daemon starts one pinned helper thread per online CPU
   and reads each thread id. It sets rodata `helper_tgid` (its pid) and
   `helper_el_tid` (its first helper). After load it writes
   `helper_cpus[tid] = cpu` for every helper, before any wake.
2. **Request** (at most once per 10 ms slot, only with queued or staged
   records). The daemon writes `{round r, el_request = a new nonzero token}`
   and wakes every helper. Each helper calls `getppid`. The issuer thread
   publishes the E and the L boundary of round r, once for that token; every
   helper also publishes a CPU boundary of round r, which is not an
   acknowledgement and is not passed on.
3. **Fence** (every loop pass). When the round's E and L boundaries have been
   consumed, the daemon reads `transport_attempts` once into the frozen
   targets, writes `{round r+1, el_request 0}` and wakes every helper again.
4. **Acknowledgements.** Each helper publishes its CPU boundary of round r+1.
5. **Completion** (every loop pass), in `complete_round`
   (`src/percpu_transport.c`): see the rule below.
6. **Release.** The daemon rereads the fifteen counters, then replays the
   queued envelopes to the adapter and the coordinator, which releases DATA
   strictly below `W = min(w[s])`.

### The completion rule

A round completes when (a) its E and L boundaries were consumed, (b) every
CPU's higher-round acknowledgement was consumed, and (c) consumption reached
the frozen target on every (CPU,destination), every skipped sequence number
being a counted discard and no failure being counted. **An empty pass is not
required.** It was a proxy: emptiness is not evidence (CONTRACT.md:128), and
on a busy box no pass is ever empty. Measured: every completed round in every
loaded run of this unit completed on a pass that consumed something. Measured
against the old rule on the same build and load (EL8, PG18, `-c16` at 2,000
tps, 10 s, three runs each): the old rule completed 7, 12 and 7 rounds, held
records for 1.2 s or more at the median, and two of the three runs ended in
VOID; this rule completed 1,000 to 1,004 rounds with a held age of 12.2 to
12.5 ms at the 99th percentile and no VOID.

Why nothing more is released than under the old rule (complete only after an
empty pass). What is released is decided by the coordinator, not by the
transport: DATA with `t < W`, where `W` is the minimum of the cutoffs carried
by this round's boundaries and by consumed perf DATA (CONTRACT.md:103-106).
Completing earlier presents the coordinator with a subset of what a later
completion would present, so `W` is no larger. Every record below that `W`
is already present, by what each boundary means (CONTRACT.md:83):
- E and L: a producer reserves before it takes its timestamp, and the
  boundary's `w` is read before the boundary's own reservation, so a record
  with `t < w` was reserved earlier. The ring is consumed in reservation
  order and an unfinished reservation stops the reader (CONTRACT.md:88), so
  such a record was consumed before the boundary.
- CPU i: its perf buffer is ordered, so every DATA record older than the
  acknowledgement was consumed before it (CONTRACT.md:90-91).
The fence, the targets and the counter reread are for the other half of a
boundary's meaning: a failed publication of that window must be visible in
out-of-band accounting before a frontier is published (CONTRACT.md:84, :98).
That is unchanged: the reread still follows every acknowledgement.

**A target that is not reached is a hold, not a fault**, while its records
can still arrive. It is decided in a bounded number of passes even when no
pass is ever empty:
- A **failed** attempt is diagnosed without any pass. The failure counters
  are read after every acknowledgement, and on every 10 ms slot while a round
  is open (a failed boundary or acknowledgement never arrives, so the round
  could otherwise wait for it forever). Any failure is `VOID(sequence_gap)`.
- Otherwise the daemon reads the E and L producer positions **once**, after
  every acknowledgement (the cut). Every attempt up to a target had finished
  before its CPU's helper could acknowledge, so a record it delivered lies
  below the cut. The reader reaches the cut in at most
  `backlog / (4096 records) + 1` passes, not counting passes that stop at an
  unfinished reservation; after 4096 of those the round is
  `VOID(unknown_evidence)`. Later traffic cannot move the cut or the targets.
- Past the cut, an attempt that is still missing was never delivered. If it
  is a counted discard the round completes; otherwise `VOID(sequence_gap)`.
A sequence number is never skipped or recorded as delivered, and nothing is
released on the way to either verdict.

Read in source for the cut: the kernel publishes `producer_pos` at
reservation, after marking the record BUSY (EL8
`kernel/bpf/ringbuf.c:358-362`), and commit or discard clears BUSY
(`:388-402`). The box's libbpf (v1.4.7, `src/ringbuf.c:234-275`) stops at a
BUSY record, and advances `consumer_pos` past a discarded one without calling
the reader.

### Also changed, and why

- **INIT wake-up hint.** Under the mode INIT is stored in state and published
  nowhere (issue 871 comment 6874906593), so nothing told the daemon to sweep
  for it. An isolated short backend then exited unattached: measured, 2 of 4
  short `-c16` runs ended in `VOID(counter_change, lifecycle_init_drop)`,
  caused by pgbench's own setup connection. `on_bootstrap` now puts one
  sample on a per-CPU hint buffer (`init_wake`) after storing the facts. It
  is a wake-up only: no envelope, no sequence number, no attempt, no
  accounting, and it takes no ring lock. With it, 0 of 6 repeats lost an INIT.
- **Uncovered records.** A backend that existed before the capture has no
  lifetime cookie. Its TRACE and lifecycle records are consumed and accounted
  but are not merge input; the daemon counts them (`uncovered`). Before this,
  the first such record (PostgreSQL's auxiliary processes produce them within
  a second) was `VOID(unknown_evidence)`. A FRONTIER says nothing about an
  uncovered backend.

- **The standalone probe under `pgbench -C`.** A box-check of 85102df2 on
  Ubuntu 6.8 failed the producers' live test in its `-C` arm: the probe had
  exited. Read in its log (`boxcheck-a1g-ubuntu.log`): 16 `attempt sequence
  gap/reorder` and 35 `L record outside its own FORK/EXIT positions`, among
  10,309 libbpf "failed to create uprobe ... No such process" lines against
  10 backends with a stored INIT. The uprobe errors were not what ended it:
  the probe ignored every attach error, of any kind. It ended because any
  `PROBE-FAIL` ends its loop, and the failures were lost records on L. Read
  in its source: it opened a watchpoint and attached two per-pid uprobes for
  every FORK **inside the lifecycle ring's consume callback**, so under churn
  the reader stayed away from the 64 KiB ring for milliseconds per
  connection and reservations were refused. Changed: FORK consumption only
  registers the backend; the main loop instruments it between drains. Every
  attach error is classified by errno and liveness: ESRCH, or a task that is
  gone, a zombie or exiting, is an exit race, counted once per backend and
  printed (`attach_exit_races`); any other error on a live backend is
  printed with its errno and fails the probe (`attach_failures`), where
  before it was silently ignored. The cause above is read from the log and
  the source; the box runs that follow test it.

  **Follow-up failure on `567bf82a` (dispatch evidence, not a run performed
  in this round).** Ubuntu 6.8, PG18, the producers' `pgbench -C` ten-run
  loop stopped at run 1 with exit 1, after the earlier callback-stall fix.
  The log reported `PROBE-FAIL: attempt sequence gap/reorder` and
  `PROBE-FAIL: L record outside its own FORK/EXIT positions`. L failures
  totalled 45 (8/9/9/19 per CPU), with 0 discards; CPU and E had 0 failures.
  There was no `duplicate/nonincreasing` line. Evidence supplied by the lead:
  `tests/results/session-20261114/b-integration/ubuntu/salvaged-vm/`
  `final-producers-churn10-pg18.log` and `final-producers-live-pg18.log`.
  The daemon, reading the same 64 KiB lifecycle ring on that VM/kernel, had
  0 L failures in nine churn runs. The inference that the probe's remaining
  attach and `/proc` work starved its reader is still to be measured.

  **B1 round 1 (`8bf60d7d`):** the probe drains after each backend's bootstrap and two
  uprobe attaches (at most **K=3** attach attempts between drains), then
  separately after its wait-attach and `/proc` inspection. Already
  instrumented backends also get that latter drain. Even the INIT-restoration
  mutant defers attach work from its INIT callback to the main loop. The
  drain can consume EXIT/FORK and reuse a registry slot, so the next step rechecks its current
  pid and pending state. The registered host driver executes the actual
  service/instrument/drain functions against a finite queue model; moving
  the drains after all attaches must fail
  `FAIL lifecycle ring drained between attach batches`. Every existing L
  failure assertion remains in force. Ring sizes, retries, load limits and
  timeouts are unchanged. A fresh Linux run is still required.

  At every normal probe exit, including failures and setup refusals, stderr
  now has these seven machine-readable lines (the live driver also forwards
  them on success):

  ```text
  PROBE-STAT: ringbuf_site_drops site=LIFECYCLE_FORK value=<sum over CPUs|null>
  PROBE-STAT: ringbuf_site_drops site=LIFECYCLE_EXIT value=<sum over CPUs|null>
  PROBE-STAT: ringbuf_site_drops site=QUERY_TEXT value=<sum over CPUs|null>
  PROBE-STAT: ringbuf_site_drops site=PG13_QUERY_TEXT value=<sum over CPUs|null>
  PROBE-STAT: lifecycle_rb max_producer_minus_consumer_bytes=<bytes> ring_size_bytes=<bytes> samples=<count>
  PROBE-STAT: attaches max_between_drains=<count> total=<count>
  PROBE-STAT: proc_reads max_between_drains=<count> total=<count>
  ```

  Occupancy is sampled before every drain, including the top of each main
  and completion loop. The size comes from the loaded lifecycle map; zero
  samples/size means no usable observation, and an unavailable site read is
  `null`, never a measured zero. Attach counts include attempted per-backend
  watchpoint opens and uprobe attaches, successful or failed. `/proc` counts
  cover explicit `read`, `pread` and `fgets` attempts (each maps line/EOF
  attempt counts); they exclude libbpf's internal I/O. Maxima include the
  final partial interval as well as completed intervals.

  On the box, FORK/EXIT refusals with the peak close to `ring_size_bytes`
  support probe lag. Refusals with a consistently low sampled occupancy
  warrant producer investigation; a peak sampled between operations cannot
  prove occupancy at the instant of failure. TEXT site counters include
  both reservation refusals and negative user-string reads, so nonzero TEXT
  with low occupancy may be a text-read fault rather than reader lag. These
  aggregate counters cannot distinguish those two TEXT causes on their own;
  do not call them all full-ring drops. No result for this revised probe is
  claimed here.

  **Ubuntu 6.8, PG18 result on `8bf60d7d` (supplied by the lead):** the
  steady arm had 0 FORK/EXIT/TEXT site drops and a sampled lifecycle-ring
  peak of 2,496 / 65,536 bytes. Unthrottled `pgbench -C` still failed:
  LIFECYCLE_FORK drops 9, LIFECYCLE_EXIT drops 13, TEXT 0; sampled peak
  3,008 / 65,536 bytes; at most 3 attaches between drains. The failures were
  attempt sequence gaps and L records outside their FORK/EXIT positions.
  That occupancy was sampled before consume, so it cannot show a ring
  filling during consume. EXIT still closed descriptors and destroyed
  uprobe links inside the callback. The daemon comparison used rate 200
  with a 100 ms hold, rather than this unthrottled load; it does not settle
  the cause.

  **B1 round 2 (`f38ebdd0`, consumer changes only):** every single
  `ring_buffer__consume` call is timed, and the L callback samples
  producer-minus-consumer bytes on every record, including TEXT and
  boundaries. EXIT only marks its matching PID/cookie slot for close,
  keeping the identity checks and `exited_before_attach` count. FORK also
  defers cleanup of an older slot with the same PID. A closing slot remains
  occupied and cannot be attached or reused until teardown completes.
  After consume returns, the main thread tears down at most **K=1 backend
  per drain** (up to two descriptor closes and two link destroys), including
  during shutdown, with a drain between batches. No PID, cookie or fd is
  saved across a drain. The existing K=3 attach bound remains.

  Three additional lines are always printed at exit, alongside the seven
  above, and forwarded by the live driver on success:

  ```text
  PROBE-STAT: consume max_us=<longest single ring_buffer__consume call> total_calls=<count>
  PROBE-STAT: lifecycle_rb in_callback_max_producer_minus_consumer_bytes=<bytes>
  PROBE-STAT: teardown max_us=<longest single deferred backend teardown> total=<count>
  ```

  Durations use monotonic nanoseconds and print whole microseconds. Consume
  timing excludes perf consumption and deferred teardown. Teardown includes
  all descriptor closes and link destroys for one backend. A zero call or
  teardown count means there was no such timed operation. Callback occupancy
  is still a sample, not occupancy at the instant of a failed reservation.
  The host harness executes the actual L callback, registration, teardown
  and service functions. Restoring teardown in the callback must fail
  `FAIL backend teardown runs outside lifecycle callback`; freeing a slot
  before its teardown must fail
  `FAIL closing slot reserved until deferred teardown`. The existing
  drain-after-all-attaches mutant remains required.

  **Decision rule for the lead's next box run:** drops going to 0 with only
  the consumer changed means probe lag. If drops persist with short consume
  calls, the next step is a failure-time snapshot of positions and length
  in the BPF reservation path, kept outside the ring (A1). Every L failure
  still fails the run; this round makes no new Linux measurement claim.

  **B1 round 2 box result on `f38ebdd0` (supplied by the lead, Ubuntu
  6.8, `pgwt-gate-2`):** PG18 churn passed 10 of 10 runs, with every
  ringbuf site drop counter 0 (previously FORK 9 and EXIT 13). Changing only
  the consumer settled those earlier drops as probe lag. Deferred teardown
  still took about 71,000–83,000 us per backend, with 11,998–15,505
  teardowns per run. PG13 churn stopped at the first run: twelve
  `PROBE-FAIL: probe backend registry full` lines, drops 0, lifecycle-ring
  peak 10,112 / 65,536 bytes, teardown total 11,101 and attaches total 938.
  The PG13 and PG18 full tests passed; the deliberate spin arm had four
  FORK drops. Evidence: `tests/results/session-20261116/b1-r2-f38ebdd0/`
  on the lead's machine. These are dispatch measurements, not runs made
  during round 3.

  **B1 round 3 (probe only):** activity and query uprobes now attach once,
  with pid -1, like `src/exact_probe.c`. PG13 selects
  `on_executor_start`; PG17+ selects `on_report_query_id`. The links live
  until probe shutdown, including when their original backends exit. The
  self-reuse arm attaches no PostgreSQL uprobes. A global attach error
  fails setup, and partial setup releases its successfully attached link.
  Backend teardown closes only its bootstrap/wait watchpoint descriptors
  (normally only the remaining wait descriptor), then clears the slot.
  After each drain all closing slots are reclaimed outside the callback;
  the former K=1 queue of costly link destructions is gone. Consume and
  per-backend teardown timing keep their existing meanings; global link
  destruction at shutdown is not backend teardown. Backend service now
  attempts at most one watchpoint attach before draining; the two startup
  global attaches also contribute to the attach statistics.

  PID/cookie and FORK/EXIT-position checks still read the wire records and
  the retained per-cookie identity table, including TEXT records that can
  arrive before watchpoint attachment. A closing slot remains reserved
  until its descriptors are closed. Exhausting the fixed backend slots
  still fails, now with `used`, `size`, `closing`, `pending`, `active`,
  `boot_fds` and `wait_fds` counts. Pending excludes closing; active is the
  remaining occupied slots, not a claim that those tasks are still live.
  The added exit diagnostic, forwarded on successful live runs too, is:

  ```text
  PROBE-STAT: registry max_used=<peak occupied backend slots> size=<MAX_BACKENDS>
  ```

  The host simulation runs the actual registration, instrumentation, record
  callback and teardown functions with 4,096 arrivals at 1,000 backends/s,
  after 16 instrumented backends exit together. Descriptor closes advance
  a simulated clock by 1 us, and each link destruction by 37.5 ms (75 ms
  per backend for the restored two-link path). The global-link version
  retires every lifetime without registry exhaustion. A built mutant
  restores per-backend attaches and destroys and must fill the registry,
  failing `FAIL churn registry accepts backends without exhaustion`.
  The callback, reserved-slot and drain-between-attaches mutants remain
  required. This is a synthetic regression, not a Linux throughput claim.

  For the next box run, read **all eleven PROBE-STAT lines**: the four
  site-drop counters, lifecycle-ring peak/size/samples, attaches, proc_reads,
  consume max_us/calls, in-callback occupancy, teardown max_us/total, and
  registry max_used/size. Compare teardown cost and registry occupancy on
  PG13 churn against the dispatch measurement above. All L failure gates
  remain strict. This revision has no new Linux measurement result.

### What is still unproven, and what was found

- The kernel premises 1, 3 and 5 above are as open as before. The cut adds no
  premise beyond 1 (the helper cannot pass an unfinished producer on its
  CPU). On Ubuntu 6.8 nothing is claimed: the daemon runs there only with
  `PGWT_TEST_EXACT_UNCERTIFIED_KERNEL=1` and says so.
- **No attachment proof exists (A2).** Without it every fork-seeded
  generation is `VOID(unknown_evidence)` at its first TRANS (contract row 33):
  measured. The box runs set `PGWT_TEST_EXACT_ATTACH_PREMISE=1`, which
  assumes the proof at FORK consumption through the report path A2 will use.
  Every number below is a transport measurement under that premise and
  certifies nothing.
- **The coordinator limits the rate.** `pgwt_merge_feed` (`src/core/merge.c`,
  not changed here) re-sorts and re-validates its staged queue on every input
  and moves the whole queue for every released record. Measured on EL8, 4
  vCPU, PG18: clean at 8,000 select-only tps (32,000 released records/s);
  at 12,000 tps and unthrottled (about 17,000 tps) a round's release takes
  from 0.5 s to 98 s. The 12,000-tps run ended in `VOID(unknown_evidence)`;
  the unthrottled run filled the queue and ended in `VOID(allocation_fail)`
  and `VOID(unknown_evidence)`. 89.6% of daemon CPU was in `pgwt_merge_feed`.
  This is a measured limit of the experimental mode, tracked by issue #932.
  In this **EL8 rate sweep**, the transport completed every round it requested
  (342/342 at 12,000 tps; 7/7 unthrottled). That is not a claim about Ubuntu,
  other runs, or bounded service: before the issue #931 service budgets the EL8
  daemon ran 50 s (PG18) and 97 s (PG13) against `--duration 18`, an overrun of
  32–79 s. The registered deadline arm now checks natural
  exit within duration + 15 s and SIGTERM to exit within 15 s under load;
  the historical overruns are not measurements of those budgets.
- **Short-lived backends (issue #933).** A backend whose INIT is lost VOIDs
  the capture. On PG13 the daemon needs milliseconds of
  `/proc/<pid>/mem` reads to attach a backend. A `pgbench -S -C` backend
  lives a few milliseconds and some exit unattached. The default transport
  loses them too (15 and 20 `lifecycle_init_lost_total` in two 8 s runs at 200
  connections/s, on 21227d96 and on this tree) and counts them as
  no-opportunity. Under the mode the state is already retired by BPF when the
  daemon handles the EXIT, so the loss cannot be classified, and each one is
  `VOID(counter_change, lifecycle_init_drop)`: 39 in 8 s. On PG18, where
  attaching is cheap, a plain `-C` run lost a backend in 1 of 8 runs at 200
  connections/s and in 4 of 7 unthrottled runs (about 430 connections/s).
  Connections that hold for 100 ms are attached and run clean on both, so
  the registered live test's `-C` arm holds each connection for 100 ms.
- A backend that exits before it ever reaches `InitProcess` is also counted
  as an INIT loss under the mode, for the same reason (state already
  retired). How often that happens was not measured.
- **0 transport failures in any clean run** is conditional: overload runs
  are not clean, fail the clean-run assertions, and are excluded from that
  statement. The Ubuntu PG18 unthrottled `-c16` overload runs recorded these
  E failures and sequence-gap faults (not a certification result):

  | Run | E failures | sequence-gap faults |
  |---|---:|---:|
  | 1 | 124,163 | 3 |
  | 2 | 282,106 | 3 |
  | 3 | 278,850 | 2 |

  Evidence: `tests/results/session-20261114/b-integration/ubuntu/salvaged-vm/`,
  `final-unthrottled-pg18-c16-{1,2,3}/daemon.stderr`. In the EL8 rate sweep, E failures and
  sequence-gap faults were both 0 at 12,000 tps and unthrottled; those runs
  still VOIDed for the reasons above. Zero transport failures does not imply
  a clean capture.
- **Cut and terminal discard: exercised live by the `cutdiscard` arm.**
  This describes the arm's coverage; **the lead has not run the new arm yet,
  and no live PASS is claimed here**. The historical clean runs had 0 discards;
  their evidence does not establish this path. The arm and its uncounted
  mutant are described below. The existing host tests remain the executed
  evidence until the lead records a box result.
- The two live mutants named in the brief as examples (a stale-round
  acknowledgement, completion without the sequence check) cannot be made to
  fail on a clean live run: a stale boundary arriving while fencing and a
  sequence hole both need a stimulus the historical clean loads did not
  produce. The new `cutdiscard` arm supplies a terminal-hole stimulus. Host mutants
  cover them (`accept_stale_round`, `ignore_terminal_hole`,
  `uncounted_skip_accepted`). The three original live mutants are the defects
  that kept rounds from completing. Completion without acknowledgements was
  tried as a fourth: the sequence check stops it at or before its first
  round, at one of two assertions depending on timing, so it stays a host
  mutant (`skip_cpu_ack`).

### Live cut and terminal-discard arm (execution pending)

Run on a built Linux gate box, as root, with the postmaster PID:

```sh
python3 tests/test_transport_daemon_live.py --pid "$POSTMASTER" --only cutdiscard --no-mutants
python3 tests/test_transport_daemon_live.py --pid "$POSTMASTER" --only cutdiscard
```

The second command runs the counted control and then the mutant. The arm also
runs in the default sequence; `--no-mutants` skips only the mutant. On the Mac,
`python3 tests/test_transport_daemon_live.py --dry-run` checks source anchors,
the live scheduler through virtual I/O, and verdict controls, without
compiling/loading BPF or claiming the kernel schedule.

`tests/transport_cutdiscard.py` builds a temporary daemon and producer object;
the product source and build recipes do not acquire a test hook. Only the test
build includes `tests/transport_cutdiscard.h`. It temporarily admits the main
thread to the existing helper map for a pinned `getppid` syscall and removes it
afterwards. The test producer uses the existing reserve/give-up shape from the
odd-PID stimulus, counts a discard **before** releasing the reservation, and
does not invent a delivered record or modify an accounting-map value in userspace.
Map, rodata and skeleton member layouts remain unchanged.

The schedule is causal, without added sleeps or assertion retries:

1. Once pgbench is launched, real watchpoint DATA is queued, and both E/L
   candidate boundaries are consumed,
   before the real target read, make one final E attempt on the second online
   CPU, which is not the E/L issuer. Reserve and discard it in the kernel.
2. Defer **both** E/L reader paths while CPU perf readers continue. Consume
   every CPU's higher-round acknowledgement. Completion reads real mmap
   producer/consumer positions; require an unconsumed E prefix and a fixed cut,
   a HOLD, and unchanged DATA, TEXT, FRONTIER and completed-round counts.
3. Publish more real E/L boundary traffic on the issuer CPU after the cut,
   requiring both producer positions to advance beyond it, then resume E/L
   consumption. The cut cannot move with that traffic. The counted arm must
   complete with `holds > 0`, `cuts > 0`, `terminal_discards > 0`, no VOID,
   and `attempted = delivered + failures + discards` for every destination
   (the daemon additionally reconciles every CPU/destination key).
4. The mutant removes only the stimulus's discard-accounting call. It must
   reach the same HOLD/cut/traffic schedule, then produce `VOID(sequence_gap)`
   with no additional released DATA, TEXT, FRONTIER or completed round after
   the stimulus baseline. Build/load failure,
   another VOID reason or missing stimulus fails the test, never kills it.

PostgreSQL processes belonging to the selected postmaster are temporarily
pinned away from the second CPU so its E discard stays terminal; existing
affinities are restored in cleanup. Other CPUs continue pgbench and helper
traffic. This restricted schedule is a mechanism test, not an overhead screen,
throughput qualification, attachment proof, or an Ubuntu certification claim.

### Measurements (not gates)

Under the attachment test premise, 4 vCPU (cx33), 10 s per run, three runs
per cell, ranges over the runs. `-c16` is `pgbench -S -c 16 -j 4 -R 2000`;
`-C` is `pgbench -C -c 16 -j 4 -R 200` with each connection holding 100 ms
(about 143 connections/s). Every run: 0 VOID, 0 transport failures, 0
discards, 0 perf LOST, every round completed on a non-empty pass, every
acknowledgement produced after its fence request.

| Kernel, PG, load | rounds/s | held age p99 / max, ms | event to release p99 / max, ms | released records/s | daemon CPU, % of a core | queue high |
|---|---:|---:|---:|---:|---:|---:|
| EL8 4.18, PG18, `-c16` | 99.7 to 100.1 | 12.2 to 12.7 / 32.6 to 35.1 | 12.4 to 13.0 / 32.7 to 114.8 | 7,883 to 7,994 | 15.7 to 16.5 | 156 to 181 |
| EL8 4.18, PG18, `-C` | 79.9 to 85.1 | 24.8 to 27.0 / 31.0 to 46.2 | 25.9 to 27.8 / 33.9 to 47.0 | 2,850 to 2,871 | 24.7 to 25.9 | 122 to 152 |
| EL8 4.18, PG13, `-c16` | 99.7 to 100.0 | 12.6 to 13.5 / 23.9 to 32.8 | 12.8 to 13.6 / 24.3 to 35.8 | 7,916 to 7,987 | 34.2 to 39.3 | 156 to 187 |
| EL8 4.18, PG13, `-C` | 81.0 to 83.1 | 26.2 to 28.1 / 36.5 to 41.7 | 26.8 to 29.7 / 42.6 to 371.1 | 2,878 to 2,904 | 34.8 to 36.0 | 117 to 127 |

Held age is from wire consumption to publication; event to release is from
the record's own timestamp, so it includes kernel-buffer time. The two
event-to-release maxima above 100 ms (114.8 and 371.1 ms) are FORK lifecycle
records, not wait transitions (the daemon names the record with the
largest age); in the other ten runs the largest is 47.0 ms. Rounds are requested at most once per 10 ms slot, which is
the 100 rounds/s ceiling.

Evidence: `tests/results/session-20261114/b-integration/` (git-ignored).
