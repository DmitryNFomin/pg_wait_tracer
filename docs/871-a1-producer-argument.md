# A1 producer protocol and kernel argument

Scope: experimental `exact_perf_mode != 0`, sequence mode N. This is producer
implementation and a falsifiable completion argument, **not merge certification**.
The default producer paths keep raw payloads, timestamp placement, accounting
and userspace map retirement. The frozen wire and v4 storage format are unchanged.
Attachment qualification is A2; production drain/merge admission is B.

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
| `on_bootstrap` | state only | **no envelope, reservation or attempt** | preserves fork-seeded cookie |
| `on_exit` lifecycle EXIT | L ring | LIFECYCLE / 24 / 56 | exiting state cookie |
| `on_report_query_id` query text | L ring | TEXT / 272 / 304 | producing state cookie |
| `on_executor_start` PG13 text | L ring | TEXT / 272 / 304 | producing state cookie |
| `on_transport_helper`, leader E/L request | E and L | BOUNDARY / 16 / 48 | 0 (no backend) |
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
admission recheck is discarded and reconciled in `transport_failures` only;
it does not increment the logical ring aggregate or site counter. The command
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

Text reservations precede reading/copying the string. Unreadable or empty text
is discarded and counts a transport failure, including an empty but valid
string, because it consumed a transport attempt. Existing logical failure
semantics are preserved (only read error increments the text site). Wait DATA
uses `bpf_perf_event_output(..., BPF_F_CURRENT_CPU, ...)`; nonzero return counts
the existing wait site and `event_ring_drop` aggregate, plus a CPU transport
failure. Helper and E/L boundary failures count the destination transport slot.

B must read **both** `transport_attempts` and `transport_failures`
(PERCPU_ARRAY[3], u64, CPU/E/L keys 0/1/2). They supplement, not replace, the
fifteen logical counters. A failed boundary cannot disappear. INIT has no wire
attempt under the mode; the logical `lifecycle_init_drop` component still counts
bootstrap observations that were never attached, including no-opportunity loss
for merge certification. A helper's
return zero is not delivery evidence on old perf implementations. Reconcile
attempts against consumed records after completion, including the terminal gap.

### Complete reserve-site audit (round 2)

Command used: `rg -n 'bpf_ringbuf_(reserve|submit|discard|output)' src/bpf/pg_wait_tracer.bpf.c`.
These are all eleven explicit reserve expressions in that file. NULL never
owns a reservation. The portable shim rejects double-submit/discard and checks
that every successful reservation has been released before resetting a case.

| Reserve expression | Every successful-reserve path |
| --- | --- |
| `transport_reserve_trace`, E | Returns ownership to `transport_exit`, `emit_marker`, or `on_report_activity`; their paths are below. NULL counts aggregate/site/transport failure. |
| `transport_reserve_lifecycle`, L | Returns ownership to `on_fork` or `transport_exit`; their paths are below. NULL counts site/transport failure. Bootstrap no longer calls it. |
| `transport_boundary`, E branch | Fills boundary, submits once; no subsequent branch/return before submit. |
| `transport_boundary`, L branch | Same as E; the destination branches are exclusive. |
| `on_bootstrap`, raw L | Mode zero only: fills raw INIT and submits once. NULL counts hint drop; stored facts remain. |
| `on_fork`, raw L | Mode zero only: fills FORK and submits once. NULL counts fork-site drop. |
| `on_exit`, raw L | Mode zero only: fills EXIT and submits once. NULL counts exit-site drop. |
| `on_report_query_id`, enveloped L | On unreadable/empty text, discards once and returns; otherwise submits once before dedup update. |
| `on_report_query_id`, raw L | Same exclusive text branches, using the raw reservation pointer. |
| `on_executor_start`, enveloped L | On unreadable/empty text, discards once and returns; otherwise submits once before dedup update. |
| `on_executor_start`, raw L | Same exclusive text branches, using the raw reservation pointer. |

Wrapper ownership paths:

- `on_fork`: successful enveloped L always fills/submits once. The separate
  test-only forced-failure branch reserves nothing.
- `transport_exit`: when all required reservations succeed, submits E if
  required and then L, each once. If either required destination fails,
  discards each successful partner once and reconciles its loss; no publication
  survives a partial pair. With no live watchpoint only L is required.
- `emit_marker`: after E success, a missing state or closed admission discards
  once and returns, accounting only the transport attempt. Otherwise submits
  once. Reserve failure still updates attribution and never submits NULL.
- `on_report_activity`: only a saved true boundary condition reserves. Thus
  the unchanged-command early return cannot own a reservation. The same saved
  condition enters the emission block despite concurrent `wp_live` replacement;
  open admission submits once, closed admission discards once. A saved false
  condition reserves/publishes nothing even if userspace later sets `wp_live`.

`emit_event` uses the atomic `bpf_ringbuf_output` helper in mode zero, which
does not transfer a reservation reference to the BPF program. Perf DATA and
CPU boundaries use `bpf_perf_event_output`, also without explicit ring ownership.

## On-demand helper protocol for B

Set `helper_tgid` before load. Register `helper_cpus[TID] = CPU`, with one pinned
SCHED_OTHER helper per online CPU. The registered process leader is also pinned
and issues E/L requests. An unregistered TID, foreign TGID or wrong current CPU
publishes nothing. Perf output always selects `BPF_F_CURRENT_CPU`, never a CPU
provided by userspace. The helper program is a task-context `getppid` syscall
tracepoint, copied from the bench mechanism at `fcda6b39`; user helpers park on
untimed condition variables and call the syscall only after a request.

`transport_ctl.round=0` is inactive. For an E/L request, update the single control
entry with a nonzero round and a new nonzero `el_request` token, then wake the
registered leader. It reads `w` before reserving each E/L boundary and copies
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
production translation unit excludes the knob, test map, spin function and
FORK injection block at preprocessing time; it contains no falsifier back-edge
even when the runtime mode is enabled. A nonzero test value forces one L FORK
reservation failure per producing CPU (the reserve is deliberately bypassed),
consumes its attempt, reads a would-be timestamp into `test_spin.started`,
loops for the requested interval, then counts the failure and publishes
`test_spin.completed`. No wire record is invented for the failed publication.
The loop has a hard 65,536-iteration bound; the test requests 20 ms and measures
the actual duration. The live driver builds this object and its own skeleton
and loader in an external temporary directory using the project's resolved
compiler/libbpf/bpftool settings. Clean arms load the normal production object.

The probe records helper boundaries and per-CPU requests with their round.
Request timestamps are taken after signalling the pinned helper. For every
CPU with a spin, it requires a request issued **during** that spin and a consumed
boundary on that CPU with that exact round and `w > ended`. Missing requests,
missing boundaries and unrelated CPU/round evidence refuse. During this test
requests are issued every millisecond as stimulus, not as a production timer. A CPU
boundary with `started <= w <= ended` proves the helper passed the unfinished
spin and fails the test, even if the consumer only reads it after completion.
This avoids falsely failing a safe acknowledgement between the actual increment
and the later completion instrumentation store. It also checks completion and
transport conservation after stop. An observed overtake fails even if another
CPU lacks evidence. A measured spin under 1 ms, missing stimulus,
or a verifier rejection is exit 78 / REFUSED, never PASS. **The EL8 loop has not
been verified on this Mac.** If that kernel rejects the bounded loop, the
smallest alternative is a separately built test object with a straight-line,
unrolled chain of clock reads, measuring the delay and refusing it unless >=1 ms;
its instruction limits must themselves be checked on the real EL8 verifier.
No sleepable helper or grace period may substitute for this section.

Build on the box with the normal project/test recipes, then run:

```sh
make build-tests
sudo python3 tests/test_transport_producers_live.py --pid POSTMASTER_PID --seconds 20
# Same test is in tests/run_all.sh's live tier.
```

The Python driver obtains offsets from the running server's installed headers
and resolves ELF symbol/file offsets independently. It runs PG13 when that is
the selected server (and supports provisioned PG17/18). It creates/drops a
private pgbench database, runs `-c16 -j4` and `-C -c16 -j4`, each for 20 seconds,
then runs the INIT restoration mutant and delayed-failure arm (five seconds
each). PG14-16 are excluded by `run_all.sh` using its existing `exclude_test`
matrix convention; unsupported PG is not a new exit status.
The standalone loader owns both rings and
the perf reader; it prints a per-CPU/destination attempted/delivered matrix.
The libbpf RAW callback includes alignment padding: 80-byte DATA arrives as
84 raw bytes, 48-byte boundaries as 52; the portable test pins both lengths.

The clean arms assert load success, framing, zero q/flags, lifetime FORK-cookie
bindings, strict attempt sequences, zero LOST/failures, exact attempted/delivered
conservation after stop, no later E/L timestamp below w, and a higher-round CPU
ack from every online CPU after consuming E/L. Each clean PG arm also asserts
`init_records == 0` on L, `init_stored > 0` in the cohort, `attached > 0` and
`trace > 0`. Every CPU/destination has zero failures/LOST and attempted equals
delivered, including the `-C` arm. The self-reuse arm deliberately
holds both lifetimes' records until the second child has overwritten/retired its
PID state. It moves `ns_last_pid` on the disposable box, requires the same
initial-namespace PID to be reused, and checks both different FORK cookies.
A nested PID namespace or inability to move that cursor is a refusal, not a
vacuous reuse pass. This changes the allocation cursor only, not pid_max or an
existing task. Default-path real-PG regression evidence still comes from the
ordinary flag-off box suite; the host harness additionally pins raw mode-zero
record sizes and retained-state behavior.

The probe's simplified watchpoint attachment is stimulus only, not an A2 proof.
An already-exited or too-short task can be uninstrumented; its FORK/EXIT transport
still participates in reconciliation. INIT storage is sampled from the matching
backend/cookie state, including after attachment, and counted once only when
the backend's `/proc/PID/cmdline` title identifies the driver's unique database
cohort. Unrelated postmaster children cannot satisfy this count. This makes
the no-publication assertion nonvacuous. No assertion certifies attachment or
merge coverage. The restoration mutant must build/load, produce INIT records,
and fail exactly `zero INIT records on L under exact_perf_mode`; every other
clean transport and stimulus assertion must pass or it is not a killed mutant.

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
| Marker timestamp moved ahead of reserve | `timestamp after reserve` |
| FORK cookie selected from the current emitting PID entry rather than the child lifetime | `queued cookie survives pid reuse` |
| Reused PID's DATA keeps cookie 1 | `DATA cookie across PID reuse` |
| E attempt increment only after reserve success | `marker fail attribution coherent` (includes consumed attempt) |
| Helper ignores expected CPU | `helper wrong CPU refused` |
| Command re-reads `wp_live` after a userspace map-entry replacement | `command saved admission releases reservation` |
| Bootstrap restores the L reservation/publication | `zero INIT records on L under exact_perf_mode` (host and live) |
| Command admission discard increments logical ring losses | `command admission discard is not a ring drop` |

The retained-record box test is the read-time/current-PID-resolution control:
it defers drain across real initial-namespace reuse and requires records to lie
in their own cookie's FORK/EXIT interval (L metadata is also checked by FIFO
lifetime position). Resolving the old records from the current or deleted PID
entry cannot satisfy those checks. This box control is supplied, not run here.

## Round-2 validation and handoff (Mac, 2026-11-14)

Required head and exact brief gate:

```text
$ git log --oneline -1
8927d916 Add flagged BPF transport producers and lifetime cookies
$ scripts/lead/check-brief.sh /private/tmp/claude-501/-Users-dmitryfomin-work-git-pg-wait-tracer/27cc85b4-040d-47d2-ba40-e0c59ac54ec1/scratchpad/m871/brief-a1-r2.md
CHECK BRIEF: covered #871
```

Portable validation (no BPF compilation or loading):

```text
$ cc -std=c11 -Wall -Wextra -Werror -fsanitize=address,undefined -DPGWT_TRANSPORT_HOST_TEST tests/test_transport_producers.c -o /private/tmp/pgwt-a1-r2.nb42H6/host
$ /private/tmp/pgwt-a1-r2.nb42H6/host
falsifier evidence: PASS (request during spin, matching CPU/round, later boundary; missing evidence refuses)
transport pure pieces: PASS (golden envelope, attempts, allocation, cookie, CPU binding)
live driver host checks: PASS (active preemption, missing evidence, INIT assertions, PG13-18 exclusion matrix)
production producer bodies: PASS (identity, reservation order, accounting, helper, mode zero)
mutant timestamp-before-reserve: KILLED (timestamp after reserve)
mutant seq-only-on-success: KILLED (marker fail attribution coherent)
mutant unpinned-helper: KILLED (helper wrong CPU refused)
mutant current-pid-cookie: KILLED (queued cookie survives pid reuse)
mutant stale-cookie-on-reused-pid: KILLED (DATA cookie across PID reuse)
mutant command-double-read: KILLED (command saved admission releases reservation)
mutant INIT-publication-restored: KILLED (zero INIT records on L under exact_perf_mode)
mutant admission-discard-counts-drop: KILLED (command admission discard is not a ring drop)
$ python3 -B tests/test_counter_failures.py
BPF failure paths (host C): PASS (natural errors, silent paths, success, once-only knobs)
$ scripts/check-test-registration.sh
OK: test-registration guard (base 8cafdcb2b5a3893c7d24dde9b9b934cdc701af83) — append-only invariant holds
$ scripts/check-runner-registration.sh
OK: runner-registration guard -- every tests/ci_smoke.sh invocation (5) is also invoked by tests/run_all.sh (209 invocations total, including tests/unit_tests.list entries); every candidate test file on disk (192 scanned: tests/test_*.{py,sh} plus tests/web_unit/*.test.mjs) is invoked by a runner or covered by a glob; or is a documented exemption (2 listed, 2 currently invoke no runner).
$ bash -n tests/run_all.sh
(exit 0)
$ python3 -B -c 'import ast; from pathlib import Path; [ast.parse(Path(p).read_text(), filename=p) for p in ("tests/transport_producers_host.py", "tests/test_transport_producers_live.py")]; print("Python syntax: PASS")'
Python syntax: PASS
$ git diff --check
(exit 0)
```

The initial mode-zero counter harness compilation found three obsolete
falsifier shims unused after the compile-time exclusion. Those declarations
were removed; the counter harness passes above. No check was loosened.
`make check` is reserved to the lead by this round's brief and was not invoked.

UNVERIFIED here: Linux C/BPF builds, production and test-object verifier
acceptance, live INIT restoration mutant, real-PG cohort storage/attachment,
PID reuse, preemption detection against a running kernel, measured spin and
positive helper challenge on EL8/Ubuntu. The live command and its strict
assertions are above; the lead runs those checks. The kernel argument still
leaves vendor dispatch/visibility and cross-CPU clock ordering open for U2.

Round 2 changes no backend attachment code. The existing round-1 changes copy
`prev.cookie` during flagged preseed and skip delayed userspace map deletion
under the flag; A2 remains separate. Cost: one authorised correction round,
no gate-box use; token totals come from the dispatch wrapper.

Requested diff snapshot (before adding this evidence block; the new test object
source is listed separately because `git diff --stat` omits untracked files):

```text
$ git diff --stat 8927d916
 docs/871-a1-producer-argument.md       | 225 ++++++++++++++++++++-------------
 src/bpf/pg_wait_tracer.bpf.c           |  40 +++---
 tests/counter_bpf_harness.c            |   4 +-
 tests/run_all.sh                       |   7 +
 tests/test_transport_producers.c       |  14 ++
 tests/test_transport_producers_live.py | 138 +++++++++++++++++---
 tests/transport_bpf_harness.h          |  81 +++++++++++-
 tests/transport_probe.h                |  91 ++++++++++---
 tests/transport_probe_decode.h         |  23 ++++
 tests/transport_producers_host.py      |  73 +++++++++++
 10 files changed, 545 insertions(+), 151 deletions(-)
$ git ls-files --others --exclude-standard
tests/transport_falsifier.bpf.c
$ git diff --exit-code 8927d916 -- 'src/daemon.*' src/event_stream.c src/pg_wait_tracer.c 'src/percpu_transport.*' 'src/merge_capture.*' src/core/ Makefile tests/Makefile tests/unit_tests.list scripts/check.sh src/backend.c src/backend.h contracts/ web/
(exit 0)
```

SEARCHED: tracked changes against 8927d916 in every excluded path plus backend.c/.h.
NOT SEARCHED: other branches and ignored artifacts produced by other worktrees.

Required report audit reached its prepass, then failed to initialize its
independent auditor subprocess. Under the brief's failed-check stop rule this
ends the round; it is not a SEND verdict or a readiness claim.

```text
$ scripts/lead/audit-report.sh /private/tmp/pgwt-a1-r2.nb42H6/report.txt
PREPASS: no unevidenced absence claim found
AUDIT INCONCLUSIVE (auditor exit 1) -- do not send. draft=2d995b316dbc4692293eb72da38da3afd3c75f90d7287c0d7cf18cc42e4b11d3
(exit 2)
$ cat /private/var/folders/tf/4mlc41nx3zsbx0v4x8wy4hl00000gn/T/pgwt-report-audits-501/a71dbed920564a59822d6393dee80e777d04574261842b66646328dc3e9d3665/2d995b316dbc4692293eb72da38da3afd3c75f90d7287c0d7cf18cc42e4b11d3/transcript.txt
WARNING: proceeding, even though we could not create PATH aliases: Operation not permitted (os error 1)
Reading additional input from stdin...
Error: failed to initialize in-process app-server client: Operation not permitted (os error 1)
```
