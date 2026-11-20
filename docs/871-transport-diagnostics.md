# Transport saturation diagnostics

Refs #871. The daemon pauses exact capture when a transient queue fills and
resumes it automatically when recovery can qualify. Data in a pause is lost
and counted. It is never an observed zero. Every window overlapping that gap
is incomplete. This is daemon-side reporting; the consumer limitations below
still apply.

Automatic recovery is restricted to `allocation_fail` in `transport_queue`,
`capture_arrival`, or `core_staging`. Lifetime, binding, retained-adapter,
identity, frontier and dependency exhaustion, and all other VOID reasons,
keep their existing suspension behaviour. Recovery does not repair kernel
loss or unknown lifetime evidence, and never changes capacities or priorities.

After VOID, the daemon counts and discards the held payload backlog, discards
held boundaries, and returns transient storage to its free lists. It retires
the failed request and its possible acknowledgement round. Source positions,
attempt/sequence accounting, core clock promises and earlier frontiers survive.
A late pre-proof DATA record that contradicts a retained core promise is still
sent to the core for clock/seal validation and earlier-epoch invalidation.

Each recovery attempt reads a new full counter vector. An unreadable recovery
baseline is reported as `counter_unknown`, exactly as before automatic recovery
existed, and ends automatic recovery for the rest of the capture (the core
refuses REARM after it); a readable changed vector goes through CHECK
and stops automatic recovery as a non-capacity fault. Attachment facts are
acquired again through the same BEGIN/COMPLETE mechanism used at startup.
Old proofs are cleared, and the daemon issues the core's explicit REARM only
after acquisition succeeds. Fresh completed-round E/L boundaries and every
CPU's higher-round acknowledgement are still necessary for certification.
Old request tokens cannot qualify the new epoch; payload older than the fresh
proof is counted-dropped, not released.

Retries start after 10 ms and double after failure, capped at **5 seconds**.
The existing 10 ms readable-counter checks continue during that backoff;
backoff limits proof acquisition, not integrity monitoring.
They have no attempt limit. The event loop continues draining and counting
records while paused; recovery never sleeps in a callback or exits because
capacity is exhausted. A stopped run can end with recovery still pending.

**Present qualification limit:** production attachment proof (A2) does not
exist. The enabled startup test premise is explicitly acquired again for
recovery; that proves the mechanism only. Without that premise, recovery
attempts keep waiting for a production proof mechanism and capture remains
unavailable. These host results make no Linux or production recovery claim.

## Pause and wire accounting

The daemon writes `capacity_pause`, `recovery_attempt`, `REARM` and
`capacity_resume` lines as those operations occur. The final `coverage` line
reports `capacity_pauses`, `recovery_attempts`, `rearms`, `pause_seconds`, total
`unavailable_seconds`, the last pause's monotonic `last_start_ns` and
`last_end_ns`, `reason`, `pool`, `dropped`, and whether that pause is still
`open`. `capacity_pauses` counts capacity-VOID incidents. If another incident
occurs before qualified coverage resumes, its unavailability overlaps the open
pause; elapsed time is counted once, and the last start remains the last
certified frontier. End 0 with `open=1` means recovery is not yet certified. With no pauses,
the last reason/pool are `none` and the timestamps/counts are 0.

A pause starts at the last certified frontier, not at its detection timestamp.
Its end is the fresh proof's acquisition time, confirmed only when a new
qualified FRONTIER reaches that time. Until that confirmation its duration
continues growing. Recovered coverage starts at that proof, never before it.
`covered_ns` counts intervals confirmed by frontiers; `unavailable_ns` includes
pauses and the unconfirmed startup/terminal suffix. Within the monotonic capture
window from the initial baseline to the final snapshot, with the two terms
counted independently (covered from frontiers, unavailable from the pause
intervals and the uncertified tail):

```text
covered_ns + unavailable_ns = window_ns
```

When the core reports an invalidated epoch, `epoch_invalid=1` withdraws the
whole run's coverage in this conservative report: all its time is unavailable.
Consumers requiring finer valid prefixes must use the core's per-epoch history.

The final `wire_identity` line prints this exact partition, with `ok=1` only
when it balances:

```text
wire = released + counted_dropped + uncovered + pending + boundary_control + rejected_fault
```

- `released`: successful DATA and TEXT callbacks, including lifecycle DATA.
  A callback's uncertified suffix remains subject to the contract's VOID rule.
- `counted_dropped`: discarded payload records and held boundary controls,
  counted once even when several stages retained the same payload token.
- `uncovered`: cookie-less backend records outside the covered population.
- `pending`: payload arrivals still retained and neither released nor dropped.
- `boundary_control`: consumed boundaries, including stale or unrequested
  controls; held boundaries discarded during recovery move to counted-dropped.
- `rejected_fault`: wire records rejected before capture admission, including
  malformed envelopes and sequence faults. Perf LOST is a separate notification,
  not an additional delivered wire record.

`wire` counts records delivered to the wire reader, not attempts the kernel
never delivered. The existing per-destination reconciliation continues to print
`attempted = delivered + failures + discards`; those are distinct quantities.

## Consumer review for the next unit

This is a source review of the current routes, not a browser experiment.
SEARCHED: `src/` C/headers and `web/` JS for `merge_sink`, `merge-qualification`,
`FRONTIER`, `coverage`, `fidelity`, and summary admission/production.
NOT SEARCHED: external integrations, deployed binaries, and live browser runs.

| Surface | Current reader and consequence for a capacity pause |
| --- | --- |
| Qualification sink | `src/merge_capture.c:1100` (`daemon_sink`) and `src/merge_capture.c:979` (`pgwt_capture_dump`) consumes DATA/FRONTIER/VOID and writes `merge-qualification.bin`. The daemon's coverage report carries the pause lower bound; the unchanged qualification binary does not persist REARM/acquisition-time records. It must not be used as a standalone complete product capture. |
| Raw trace and live daemon view | `src/merge_capture.c:1085` (`daemon_trace`) calls `pgwt_process_trace_payload`; `src/event_stream.c:94` writes the ordinary trace and updates legacy accumulators. These consumers receive released records, not pause/frontier metadata. A resumed transition can carry an interval crossing a pause; these views cannot label or clip that gap from this route alone. |
| Server coverage and web response fidelity | `src/server.c:2984` treats a full-mode file's first-to-last timestamp as whole-file exact coverage; `src/server.c:3389` classifies window fidelity from those spans, and `src/server.c:4858` derives raw response fidelity from transition/sample presence. Thus a window spanning a pause may still be labelled exact/complete. Neither rule reads the merge pause/frontier report. |
| AAS and web plots | `src/compute.c:929` starts AAS buckets at zero and adds received evidence. `web/static/lib/builders/aas.js:120` renders absent class values as 0; `web/static/lib/builders/fidelity.js:45` defaults absent fidelity to exact, and `web/static/lib/panels.js:48` hides the pane badge for exact windows. A gap without events can therefore look like zero activity under an exact label. The plotting consumer also includes `web/static/lib/uplot-aas.js:494` and `web/static/views/active.js:1173`. |
| Tables and CLI summary | Events, sessions and queries consume the same response fidelity (`web/static/views/events.js:96`, `web/static/views/sessions.js:36`, `web/static/views/queries.js:50`). `src/server.c:8936` / `:8942` print dump-summary fidelity from the raw load. They can report partial totals as exact; they do not receive capacity-gap completeness. |
| Persisted summaries | Production sets `summary_writer = NULL` at `src/daemon.c:1554`; `should_use_summaries` returns 0 at `src/server.c:4849`. These disabled routes do not newly render a pause as zero or complete. Their historical structures do not carry the new capacity-pause record either. |

The next consumer unit needs persisted coverage intervals and the fresh-proof
lower bound, and must refuse or mark incomplete every overlapping window.
Server/web/summary edits are outside this round.

## Stderr

```sh
rg '^exact-perf: (suspended: allocation_fail|allocation_last|pool_stats|phase |schedstat )' tracer.stderr
```

An exhaustion line retains the usage **before** VOID discards staged evidence:

```text
exact-perf: suspended: allocation_fail at 123; automatic REARM pending fresh proofs pool=capture_arrival occupancy=2 capacity=2 high=2
exact-perf: allocation_last pool=capture_arrival occupancy=2 capacity=2 high=2
exact-perf: pool_stats pool=capture_arrival occupancy=0 capacity=2 high=2
```

`allocation_last` repeats the last failure snapshot. With no allocation failure
its pool is `none`. `pool_stats` reports current usage and the capture-wide high
water for every runtime pool, including pools that did not fail. Core staging
and arrival occupancy can be zero after VOID while their high water and the
failure snapshot remain full. Identity/lifetime histories survive suspension.

Pool names are `transport_queue`, `capture_arrival`, `capture_lifetime`,
`capture_binding`, `capture_retained`, `core_staging`, `core_identity`,
`core_dependency`, `core_frontier`, and `core_payload`. Units are slots except
`core_payload`: the maximum payload bytes in an occupied staging slot, against
the per-record payload limit. `core_setup` identifies a failed allocation of
the coordinator or its source bookkeeping during creation; it cannot have a
runtime high water. A caller injecting an allocation FAULT without identifying
its storage has `pool=none`; the production capacity branches supply a pool.
The qualification binary's record format does not change; diagnostic metadata
is in memory and stderr only.

Phase lines have this form (nanoseconds for durations):

```text
exact-perf: phase name=request_to_el unit=ns count=1 mean=100 max=100 hist=1,0,0,0,0,0,0,0
```

| Name | Sample |
| --- | --- |
| `request_to_el` | Round request to consumption of both requested E/L boundaries. |
| `fence_to_ack` | Fence request to consumption of the last distinct CPU's higher-round acknowledgement. |
| `ack_to_release_end` | Last acknowledgement to the end of queue replay, including waits/yields between replay slices and any target/cut hold. |
| `release_replay` | Sum of time inside replay calls in a completed round, including its CHECK and envelope/adapter/core/sink work; excludes time between calls. |
| `records_per_round` | Queued wire envelopes replayed in a completed round, including boundaries and lifecycle/TEXT as well as DATA. |

The first two count a phase as soon as its endpoint is consumed, once per
round; the last three count completed rounds. A phase without an endpoint
has no sample; zero count means no completed observation of that phase.
Incomplete rounds are not silently counted as zero-duration completed rounds.
`mean` is integer total/count. Histogram bins are lower-inclusive and
upper-exclusive: `[0,100 us)`, `[100 us,1 ms)`, `[1,10 ms)`, `[10,100 ms)`,
`[100 ms,1 s)`, `[1,3 s)`, `[3,10 s)`, `[10 s,infinity)`.
For records the edges are 0, 1, 16, 256, 4096, 65536, 262144, 1048576, infinity.
These are reader-observed phase endpoints, not producer timestamps or
completeness evidence.

On Linux there is one scheduler line for the reader and one per pinned helper:

```text
exact-perf: schedstat role=helper tid=7 cpu=3 start_valid=1 end_valid=1 start_ns=222 end_ns=555 delta_ns=333
```

The value is the second field (run delay in nanoseconds) of
`/proc/self/task/<tid>/schedstat`. Start reads occur after buffer/helper setup,
before producer attachment. End reads occur after the final capture drain,
before final dump/stat reporting and helper teardown. All end samples precede
formatting. Missing/malformed reads or a regressing counter produce
`delta_ns=unknown`, with validity fields; they do not affect capture.
The reader has `cpu=4294967295` because it is not one of the pinned helpers.
These are observed scheduler counters, not an assertion that kernel schedstats
collection was enabled. The host suite uses explicit schedstat fixtures.

High-water tracking adds constant comparisons at existing pool allocations;
phase tracking adds work at boundaries and replay-call/round endpoints.
Scheduler file reads occur only at capture start/end. There are no added
per-record syscalls or locks. Linux runtime cost still needs the lead's box
measurement.

## Host verification

### Capacity recovery round, 2026-11-17

Starting HEAD: `274db404` (change set A). One implementation round, Mac host
checks, 0 box hours; token cost is available only in the dispatch log. The
exact dispatch brief passed:

```console
$ bash scripts/lead/check-brief.sh /private/tmp/pgwt-codex/agent%2F871-combined-diag.lock/brief.OTJyIH
CHECK BRIEF: covered #871
```

The authorised contract-only clarification is:

> DECISION 29 (owner-authorised saturation round, 2026-11-17): after a transient-capacity VOID, the daemon may issue the explicit REARM; nothing else in this contract changes.

All rows below ran both plain and with
`CFLAGS='-fsanitize=address,undefined -fno-omit-frame-pointer'`. Mutants were
compiled, reached their named case, and failed their intended assertion;
compiler failures and sanitizer failures were not counted as kills.

| Command / suite | Plain result | Sanitized result |
| --- | --- | --- |
| Compile/run `tests/test_core_merge_contract.c` + `src/core/merge.c`, passing `contracts/merge-v1/examples.json` | 99/99 (86 N/Q rows + 13 guards), API safety/recovery PASS | same |
| `bash tests/core_merge_mutants.sh` | 36/36 killed | same |
| `python3 -B tests/test_core_merge_index.py` | 112,188 inputs, 262 scenarios, 55,639 outputs identical to pinned base; cost/instrumentation checks PASS | same |
| `python3 -B tests/core_merge_index_mutants.py` | 3/3 differential + 2/2 deterministic cost killed | same |
| `python3 -B tests/test_core_merge_queue.py` | 277,517 inputs, 54 scenarios, 156,592 outputs identical; work bounds PASS | same |
| `python3 -B tests/core_merge_queue_mutants.py` | 4 work + 1 tie-break + 1 held suffix + 1 payload pairing = 7/7 killed | same |
| `bash tests/core_merge_adapter_mutants.sh` | goldens 12/12, guards 31/31, end-to-end 78/78, both linear and indexed; 27/27 adapter mutants killed | same |
| `tests/percpu_transport_mutants.py` (also invoked by the preceding command) | 91/91 killed: 48 transport + 24 tick/drain/cadence/yield + 18 raw-budget + 1 natural-duration mutant; 9/9 raw guards and 6/6 virtual deadline cases PASS | same |
| Compile/run `tests/test_transport_wire.c` | 4 header goldens, boundary/control bytes and MA bits PASS | same |
| Compile/run `tests/test_percpu_transport.c` + transport/capture/adapter/core sources | 43/43 production N rows; 49/49 completion/refusal guards PASS | same |
| Compile/run `tests/test_transport_diagnostics.c` + same sources | 10 isolated pool/payload fills; phase/histogram/schedstat checks PASS | same |
| `python3 -B tests/transport_diagnostics_mutants.py` | 5/5 killed | same |
| `python3 -B tests/test_capture_bookkeeping.py` | 4/4 guards, 6/6 killed; no-VOID qualification bytes and all 22 existing diagnostics lines identical | same |
| `python3 -B tests/test_capture_recovery.py` | 14/14 schedules; 14/14 killed | same |

The C compile/run commands use `cc -std=c11 -Wall -Wextra -Werror
-DPGWT_TRANSPORT_HOST_TEST` and the indicated CFLAGS; capture/transport binaries
link `src/percpu_transport.c src/merge_capture.c src/core/merge_adapter.c
src/core/merge.c`. Output binaries and all mutant trees were outside the repo.
The recovery Python entry compiles `tests/capture_recovery_host.c`, which
includes the real capture binding and links the unchanged core/adapter.

The new recovery mutant list is: `backlog_kept`, `stale_baseline`,
`stale_attachment_proofs`, `omit_pause_time`, `omit_unavailable_time`,
`rearm_lifetime`, `rearm_noncapacity`, `trust_preproof_data`, `uncapped_backoff`,
`retained_backlog_kept`, `hide_retained_seal`, `proof_start_as_lower_bound`, `rearm_after_proof_fault`, `skip_paused_checks`. The schedules force three
VOID/REARM cycles separately in each transient pool, require resumed DATA,
empty transient storage, increasing frontiers, exact wire/time identities,
wait on an unreadable baseline or unavailable proof premise, stop on a changed
counter, and resume after twenty failed proof attempts. They also exercise
retained payload/boundary disposal, late pre-proof invalidation and every wire
accounting class except TEXT (TEXT conservation is in bookkeeping's replay),
and time spent acquiring proofs: that time remains unavailable, with the fresh
lower bound stamped only at acquisition completion. A non-capacity fault
reported during acquisition cancels recovery before REARM.

No-VOID compatibility result, independently comparing with `d926bf22`:

```text
merge-qualification.bin: 48,896 identical bytes
sha256: 1bbd23f7d4ae5f27c6be34062f8e6f71e1862c19d45a46f7ad9fc5e087c6c96c
22 pre-existing diagnostic lines: byte-identical
```

The two new accounting lines are additive and separately required by the
bookkeeping test. Its original byte comparisons, guards and six mutants are
preserved; no frozen expected answer or tolerance was changed.

```console
$ python3 contracts/merge-v1/oracle-rows32-43/check_rows32_43.py
row 32: sol: N=MATCH Q=MATCH; sonnet: N=MATCH Q=MATCH
... rows 33-42: both authors, both modes MATCH ...
row 43: sol: N=MATCH Q=MATCH; sonnet: N=MATCH Q=MATCH
```

That is 48/48 oracle comparisons. A byte comparison using `git show
274db404:<path>` found every other merge-v1 file (35/35, including examples,
all historical oracle answers and their support files) unchanged. Historical
oracle answer sets were not regenerated. `examples.json` SHA256 remains
`27ec607741569d62ea91e707467338a568f33a163a6d4f697f151ffaf119ee25`.

The final registration/whitespace evidence and diff stat are recorded below.

```console
$ bash scripts/check-test-registration.sh
OK: test-registration guard (base d5060bfc0d2329bb7706947ee5f22c70cc734da7) — append-only invariant holds
$ bash scripts/check-runner-registration.sh
OK: runner-registration guard -- every tests/ci_smoke.sh invocation (5) is also invoked by tests/run_all.sh (221 invocations total, including tests/unit_tests.list entries); every candidate test file on disk (200 scanned: tests/test_*.{py,sh} plus tests/web_unit/*.test.mjs) is invoked by a runner or covered by a glob; or is a documented exemption (2 listed, 2 currently invoke no runner).
$ git diff --check
(exit 0; no output)
$ git diff --exit-code 274db404 -- src/core src/bpf web src/server.c tests/run_all.sh
(exit 0; no output)
```

SEARCHED: the exact excluded paths in that git diff; unchanged frozen files
were compared individually with `git show 274db404:<path>`.
NOT SEARCHED: other working trees and external deployments.

```console
$ git diff --stat 274db404
 contracts/merge-v1/CONTRACT.md     |   1 +
 docs/871-transport-diagnostics.md  | 259 ++++++++++++++++++++++++++++++++++++-
 src/merge_capture.c                | 248 ++++++++++++++++++++++++++++++++---
 src/merge_capture.h                |  27 +++-
 src/percpu_transport.c             | 101 ++++++++++++++-
 src/percpu_transport.h             |   9 ++
 src/transport_diagnostics.h        |  12 +-
 tests/percpu_tick_harness.h        |   4 +
 tests/test_capture_bookkeeping.py  |  10 +-
 tests/test_transport_diagnostics.c |   4 +-
 tests/unit_tests.list              |   3 +
 11 files changed, 642 insertions(+), 36 deletions(-)
```

New files are untracked until the dispatch wrapper commits (the sandbox
forbids staging): `tests/capture_recovery_host.c` and
`tests/test_capture_recovery.py`. The wrapper also has the prepared
`.codex-commit-msg`; its final commit SHA is not available before exit.

The reporting gate was attempted with
`bash scripts/lead/audit-report.sh /private/tmp/pgwt-recovery-progress.md`.
Its result was **AUDIT INCONCLUSIVE (auditor exit 1)**, not SEND. The auditor
could not initialize its in-process app-server client:
`Operation not permitted (os error 1)`. This is a reporting-tool limitation;
the test results above come directly from the completed commands. A lead with
a working auditor still needs to audit the report. No permission override was
attempted.

`make check`, Linux/BPF loaders, box workloads, server and web tests are outside
this brief; portable host code is the evidence here. Production recovery still
requires A2, and the consumer integration described above is the next unit.

### Original diagnostic unit

The diagnostic binary is registered in `tests/unit_tests.list`, `tests/Makefile`, and
the portable Mac tier of `scripts/check.sh`. It fills ten runtime pools or
payload limits individually, verifies the failure and retained high water,
checks two rounds with duplicate boundaries and a replay yield, and tests
histogram edges and valid/unknown scheduler output. The wrong-pool,
lost-high-water, and wrong-ack-origin mutants must build, reach their named
case, and fail the intended assertion.

```sh
cc -std=c11 -Wall -Wextra -Werror -fsanitize=address,undefined \
  -DPGWT_TRANSPORT_HOST_TEST -fno-omit-frame-pointer \
  tests/test_transport_diagnostics.c src/percpu_transport.c src/merge_capture.c \
  src/core/merge_adapter.c src/core/merge.c -o /tmp/pgwt-transport-diagnostics
/tmp/pgwt-transport-diagnostics
python3 -B tests/transport_diagnostics_mutants.py
scripts/check-test-registration.sh
scripts/check-runner-registration.sh
```

The brief excludes `make check`, BPF changes/execution on the Mac, pool resizing,
scheduling changes, and retries or tolerance changes in tests. Linux `/proc` reads are not
live-qualified by this host run.

Original transport-only host results, 2026-11-16 (Mac, before this port):

```console
$ cc -std=c11 -Wall -Wextra -Werror -fsanitize=address,undefined -DPGWT_TRANSPORT_HOST_TEST -fno-omit-frame-pointer tests/test_transport_diagnostics.c src/percpu_transport.c src/merge_capture.c src/core/merge_adapter.c src/core/merge.c -o /private/tmp/871-diagnostics-work/diagnostics
$ /private/tmp/871-diagnostics-work/diagnostics
transport diagnostics: 10 pool fills, phase times/replay yields, histograms and schedstat PASS
$ python3 -B tests/transport_diagnostics_mutants.py
KILLED wrong_pool: built, reached arrival, failed a.pool==pool
KILLED lost_high_water: built, reached arrival, failed s.pools[pool].occupancy==2
KILLED wrong_ack_origin: built, reached phases, failed d.count==count && d.total==total && d.max==max
transport diagnostic mutants: 3/3 killed
$ cc -std=c11 -Wall -Wextra -Werror -fsanitize=address,undefined -DPGWT_TRANSPORT_HOST_TEST -fno-omit-frame-pointer tests/test_percpu_transport.c src/percpu_transport.c src/merge_capture.c src/core/merge_adapter.c src/core/merge.c -o /private/tmp/871-diagnostics-work/percpu_transport
$ /private/tmp/871-diagnostics-work/percpu_transport
production wire rows 1-43: 43/43 N PASS; completion/refusal guards: 49/49 PASS
$ cc -std=c11 -Wall -Wextra -Werror -fsanitize=address,undefined -fno-omit-frame-pointer tests/test_core_merge_contract.c src/core/merge.c -o /private/tmp/871-diagnostics-work/merge-contract
$ /private/tmp/871-diagnostics-work/merge-contract contracts/merge-v1/examples.json
core merge contract: 99/99 passed
rows 1-31 prior_interval_certified=true assertions: 41
$ bash tests/core_merge_mutants.sh
core merge mutants: 36/36 killed
$ python3 -B tests/core_merge_index_mutants.py
core merge index mutants: 3/3 differential, 2/2 deterministic cost killed
$ bash tests/core_merge_adapter_mutants.sh
transport + tick/publication/drain/cadence/yield mutants: 72/72 killed at intended assertions
raw budget guards: 9/9 PASS
raw budget mutants: 18/18 killed at intended assertions
transport host mutants total: 91/91 killed at intended assertions
adapter baseline: golden 12/12, guard sweep 31/31, end-to-end 78/78 PASS, both linear and indexed
adapter mutants: 27/27 killed; guard sweep 31/31 PASS
$ scripts/check-test-registration.sh
OK: test-registration guard (base d5060bfc0d2329bb7706947ee5f22c70cc734da7) — append-only invariant holds
$ scripts/check-runner-registration.sh
OK: runner-registration guard -- every tests/ci_smoke.sh invocation (5) is also invoked by tests/run_all.sh (217 invocations total, including tests/unit_tests.list entries); every candidate test file on disk (197 scanned: tests/test_*.{py,sh} plus tests/web_unit/*.test.mjs) is invoked by a runner or covered by a glob; or is a documented exemption (2 listed, 2 currently invoke no runner).
```

Outputs above are final summary excerpts; fixture/guard/mutant progress lines
are omitted. The existing mutation harness snapshots now copy the diagnostic
header and the isolated tick harness declares the new diagnostic fields; its
cadence/yield assertions are unchanged.

Implementation cost: one implementation round; Mac host verification; 0 box
hours. Token use is not exposed to this implementer and is left for the dispatch
log rather than estimated.


## Combined transport and core-queue port

The core keeps its AVL index, linked live-slot order, free list, dependency
indices and held-prefix cache. Staging occupancy is the live record count;
payload occupancy walks the live-slot links, because released slots leave holes
and retain stale record metadata. Capacity and capture-wide high water keep the
same definitions as the original diagnostics. Queue-node/waiting-index creation
failures name `core_staging`; dependency-index failures name `core_dependency`.

Conflict resolution:
- `src/core/merge.c`: retained the core queue and added the diagnostic updates at
  its existing insertion points, with live-slot traversal for payload occupancy.
- `tests/unit_tests.list`: retained both core-queue entries and appended the
  diagnostic entry.

A sparse-slot case releases the first slot, checks the larger live payload in
another slot, reuses the freed slot, then checks usage and high water after
release and VOID. Two additional mutants must fail their intended assertions:
misnaming core staging, and using the former dense-prefix payload scan. The
original three mutants remain intact.

Combined-tree Mac evidence, 2026-11-16 (command/output summary):

```console
$ scripts/lead/check-brief.sh /private/tmp/pgwt-codex/agent%2F871-combined-diag.lock/brief.59Y8ii
CHECK BRIEF: covered #871
$ cc -std=c11 -Wall -Wextra -Werror -fsanitize=address,undefined -DPGWT_TRANSPORT_HOST_TEST -fno-omit-frame-pointer tests/test_transport_diagnostics.c src/percpu_transport.c src/merge_capture.c src/core/merge_adapter.c src/core/merge.c -o /private/tmp/871-combined-diag-work/diagnostics
$ /private/tmp/871-combined-diag-work/diagnostics
transport diagnostics: 10 pool fills, phase times/replay yields, histograms and schedstat PASS
$ python3 -B tests/transport_diagnostics_mutants.py
KILLED wrong_pool: built, reached arrival, failed a.pool==pool
KILLED wrong_core_pool: built, reached core_staging, failed a.pool==pool
KILLED dense_payload_scan: built, reached sparse_slots, failed stats.pools[PGWT_POOL_CORE_PAYLOAD].occupancy==12
KILLED lost_high_water: built, reached arrival, failed s.pools[pool].occupancy==2
KILLED wrong_ack_origin: built, reached phases, failed d.count==count && d.total==total && d.max==max
transport diagnostic mutants: 5/5 killed
$ cc -std=c11 -Wall -Wextra -Werror -fsanitize=address,undefined -DPGWT_TRANSPORT_HOST_TEST -fno-omit-frame-pointer tests/test_percpu_transport.c src/percpu_transport.c src/merge_capture.c src/core/merge_adapter.c src/core/merge.c -o /private/tmp/871-combined-diag-work/percpu_transport
$ /private/tmp/871-combined-diag-work/percpu_transport
production wire rows 1-43: 43/43 N PASS; completion/refusal guards: 49/49 PASS
$ cc -std=c11 -Wall -Wextra -Werror -fsanitize=address,undefined -fno-omit-frame-pointer tests/test_core_merge_contract.c src/core/merge.c -o /private/tmp/871-combined-diag-work/merge-contract
$ /private/tmp/871-combined-diag-work/merge-contract contracts/merge-v1/examples.json
core merge contract: 99/99 passed
rows 1-31 prior_interval_certified=true assertions: 41
$ bash tests/core_merge_mutants.sh
core merge mutants: 36/36 killed
$ python3 -B tests/core_merge_index_mutants.py
core merge index mutants: 3/3 differential, 2/2 deterministic cost killed
$ python3 -B tests/test_core_merge_queue.py
queue work PASS
$ python3 -B tests/core_merge_queue_mutants.py
core merge queue mutants: 4/4 work assertions, 1/1 tie-break, 1/1 held suffix, 1/1 payload pairing killed
$ bash tests/core_merge_adapter_mutants.sh
transport + tick/publication/drain/cadence/yield mutants: 72/72 killed at intended assertions
raw budget guards: 9/9 PASS
raw budget mutants: 18/18 killed at intended assertions
transport host mutants total: 91/91 killed at intended assertions
adapter baseline: golden 12/12, guard sweep 31/31, end-to-end 78/78 PASS, both linear and indexed
adapter mutants: 27/27 killed; guard sweep 31/31 PASS
$ python3 -B tests/test_core_merge_index.py
index work PASS
$ for suite in transport_wire transport_producers; do cc -std=c11 -Wall -Wextra -Werror -fsanitize=address,undefined -DPGWT_TRANSPORT_HOST_TEST -fno-omit-frame-pointer "tests/test_${suite}.c" -o "/private/tmp/871-combined-diag-work/$suite" || exit; "/private/tmp/871-combined-diag-work/$suite" || exit; done
wire golden boundary payload/control and MA bit order: PASS
producer mutants: 26/26 killed at intended assertions
$ scripts/check-test-registration.sh
OK: test-registration guard (base d5060bfc0d2329bb7706947ee5f22c70cc734da7) — append-only invariant holds
$ scripts/check-runner-registration.sh
OK: runner-registration guard -- every tests/ci_smoke.sh invocation (5) is also invoked by tests/run_all.sh (219 invocations total, including tests/unit_tests.list entries); every candidate test file on disk (198 scanned: tests/test_*.{py,sh} plus tests/web_unit/*.test.mjs) is invoked by a runner or covered by a glob; or is a documented exemption (2 listed, 2 currently invoke no runner).
$ git diff --check
(exit 0)
```

This port used one implementation round, Mac host checks and 0 box hours;
token use remains available to the lead through the dispatch log. Runtime
scheduler sampling and overhead remain for Linux measurement.


Report-audit attempt (separate from the passing implementation checks):

```console
$ scripts/lead/audit-report.sh /private/tmp/871-combined-diag-work/report.md
PREPASS: no unevidenced absence claim found
AUDIT INCONCLUSIVE (auditor exit 1) -- do not send.
```

The nested auditor's transcript reports:
`Error: failed to initialize in-process app-server client: Operation not permitted (os error 1)`.
A SEND verdict was not obtained. The dispatch wrapper must create the commit;
its SHA is unavailable before this implementer exits.

## INIT-loss receipt evidence (Refs #933, 2026-11-17)

This diagnostic preserves the INIT-loss decision, its counters, and the
`counter_change` VOID. It adds one `INIT-LOST-EVIDENCE:` line immediately after
`INIT-LOST:`. It measures receipt, not successful merge admission or release.

The supplied Ubuntu `transport-recovery-b/ubuntu/voids.txt:221-225` and EL8
`transport-cost-a/el8/outc/el8-pg18/unthrottled-c3-r{1,2}/tracer.stderr`
were read, not rerun. All three have `init_ts=0`, an observation-timed EXIT,
then permanent `counter_change` suspension. Source inspection confirms that
`pgwt_exit_init_lost` accepts a missing state entry with an unattached bootstrap
backend, and `transport_exit` deletes that entry in BPF. This establishes a
possible ordering explanation, not whether those particular backends lost
records. The new Linux measurement remains necessary.

Fields on `INIT-LOST-EVIDENCE:`:

| Field | Meaning |
| --- | --- |
| `pid`, `cookie`, `identity` | Receipt identity. Exact-perf uses the wire lifetime cookie; `cookie=0 identity=pid_only` explicitly identifies the default wire's weaker identity. The diagnostic cookie is never attachment proof. |
| `history` | `tracked` means this receipt history and its count are available. `incomplete` means the bounded table could not retain complete history, or no entry matched the lost backend's identity. |
| `branch` | `state_present`: both state reads succeeded; the unchanged stored/failed/retained INIT predicate classified the loss. `state_missing_bootstrap`: first read returned ENOENT and the registry retained its bootstrap handle. `state_missing_init_ts`: ENOENT, no bootstrap handle, but a retained nonzero INIT timestamp. The bootstrap branch takes precedence if both predicates hold. |
| `fork`, `init_record`, `exit_record` | `delivered` or `not_received` **at the instant of loss**. `unknown` is used when history is incomplete. A stored INIT or an attachment attempt is not a delivered INIT record. `not_received` does not predict later delivery. |
| `fork_ts`, `init_record_ts`, `exit_record_ts` | The delivered records' BPF CLOCK_MONOTONIC timestamps; 0 if unavailable. |
| `fork_received_ns`, `init_received_ns`, `exit_received_ns` | Exact-perf's existing transport receipt observation, also CLOCK_MONOTONIC. The default route prints 0 (receipt time unknown), avoiding a clock read per lifecycle record. |
| `transitions_received` | Non-marker TRACE records received for this PID/cookie before the loss, including the final exit transition if received. Counted before capture discard, queuing or release. Counting stops after successful attachment; an attached backend cannot satisfy the INIT-loss predicate. `unknown` replaces the count if history is incomplete. |
| `bpf_exit_ts`, `bpf_exit_source` | BPF exit time, from a delivered lifecycle EXIT (`lifecycle`) or exit transition (`transition`). `unknown` means neither was received; conflicting times produce `conflict` and an unknown delay. |
| `state_read_after_ns` | A failure-only clock read immediately after the classification's final state lookup, before either stderr line. An upper bound on state-read completion. |
| `exit_to_read_ns` | `state_read_after_ns - bpf_exit_ts`, an upper bound on the BPF-exit-to-state-read delay, only when a delivered lifecycle EXIT or exit transition and a non-regressing clock make it known; otherwise `unknown`. Observation-timed retirement cannot invent a BPF exit timestamp. |
| `history_overflows` | Sticky count of attempts that found the diagnostic table full. After an overflow, counts are conservatively unknown for the rest of this daemon run. This does not alter any integrity or merge counter. |

| `exit_bootstrap_fired`, `exit_init_stored`, `exit_init_ts`, `exit_wp_ever_live` | Transport v2 EXIT-carried lifetime disposition, described below. `pending` until the matching EXIT arrives; `unknown` on a received default-ring EXIT without a disposition. |

An unreadable state still follows the existing error path, and an already
attached backend still is not an INIT loss. These paths print no new loss line.
In exact-perf mode the producer deliberately publishes no INIT record, so
`init_record=not_received` alone is not a transport failure.

The original receipt-only host cases (before the EXIT-disposition extension) produced these examples:

```text
INIT-LOST-EVIDENCE: clock=CLOCK_MONOTONIC pid=42 cookie=7 identity=cookie history=tracked branch=state_present fork=delivered fork_ts=100 fork_received_ns=110 init_record=not_received init_record_ts=0 init_received_ns=0 exit_record=delivered exit_record_ts=300 exit_received_ns=310 transitions_received=2 bpf_exit_ts=300 bpf_exit_source=lifecycle state_read_after_ns=400 exit_to_read_ns=100 history_overflows=0
INIT-LOST-EVIDENCE: clock=CLOCK_MONOTONIC pid=42 cookie=7 identity=cookie history=tracked branch=state_missing_bootstrap fork=delivered fork_ts=100 fork_received_ns=110 init_record=not_received init_record_ts=0 init_received_ns=0 exit_record=delivered exit_record_ts=300 exit_received_ns=310 transitions_received=1 bpf_exit_ts=300 bpf_exit_source=lifecycle state_read_after_ns=400 exit_to_read_ns=100 history_overflows=0
INIT-LOST-EVIDENCE: clock=CLOCK_MONOTONIC pid=42 cookie=7 identity=cookie history=tracked branch=state_missing_init_ts fork=delivered fork_ts=100 fork_received_ns=110 init_record=delivered init_record_ts=123 init_received_ns=133 exit_record=not_received exit_record_ts=0 exit_received_ns=0 transitions_received=1 bpf_exit_ts=0 bpf_exit_source=unknown state_read_after_ns=400 exit_to_read_ns=unknown history_overflows=0
```

Cost: formatting and one additional clock read occur only on a counted loss.
The receipt-count exception adds one bounded hash lookup per non-marker TRACE,
with an increment only for entries not marked attached. Exact-perf reads the
existing receipt observation rather than adding a clock call. Lifecycle
receipts update the same table; EXIT handling removes its entry and repairs
the hash probe chain. There are no added per-tick scans, BPF map operations,
syscalls or per-record allocations. The table has 4,096 slots (360,464 bytes including the EXIT-disposition extension on
the host), plus one diagnostic cookie per backend entry (8 bytes each).
Lookups/deletion are bounded by 4,096 slots; after a full-table overflow, TRACE
tracking returns immediately and the count is unknown. Late records after
retirement can occupy entries until EXIT/daemon teardown, so capacity is an
explicit diagnostic limit. The default route starts new receipt history on a
new FORK but cannot disambiguate delayed records from a reused PID without a
cookie. CPU overhead has not been measured on Linux.

### EXIT-carried INIT disposition (measurement round 2)

Exact-perf now uses **transport wire version 2**. The lifecycle payload stays
**24 bytes**, and its 32-byte header plus payload stays **56 bytes**. The old
32-bit type word is split into a 16-bit type at offset 0 and 16-bit
`exit_flags` at offset 2; pid stays at 4, `addr` at 8, and EXIT time at 16.
Only EXIT uses the flags and repurposes the unused `addr` as the full 64-bit
stored INIT timestamp. No timestamp bits are truncated or borrowed for flags.
The flags are: disposition present (1), bootstrap fired (2), INIT stored (4),
and watchpoint ever marked live (8). The version changes because the EXIT
payload's interpretation changes, although its size does not. The header
goldens and a new independent EXIT payload golden pin this layout. Version 1
transport input is rejected; rebuild producer and daemon together. The
default lifecycle ring keeps its old bytes (the upper type half stays zero),
and the v4 trace format is unaffected.

Before deleting a lifetime's BPF state, both the paired E/L EXIT branch and
the L-only EXIT branch copy these facts. `bootstrap_fired` is stamped on
entry to `on_bootstrap`, including a zero/failed pointer read. `init_stored`
is the existing publication flag; `exit_init_ts` is its timestamp, or zero
when no INIT was stored. `exit_wp_ever_live` is sticky evidence that this
lifetime's interval state was marked `wp_live`, including across detach and
reseed. This reports the existing BPF state convention (preseed marks it
before enabling the hardware); it is **not** a successful-enable or A2
attachment certificate. A missing state/EXIT cannot supply these facts.

| Carried facts | How to read them |
| --- | --- |
| `exit_bootstrap_fired=1 exit_init_stored=1 exit_init_ts=T` | Bootstrap ran and stored INIT. With `branch=state_missing_bootstrap`, BPF retirement erased the stored facts before the daemon's loss-time lookup/sweep could read them. `T` preserves the missing timestamp for the next classification decision. |
| `exit_bootstrap_fired=1 exit_init_stored=0 exit_init_ts=0` | Bootstrap ran but did not publish stored INIT, for example because the pointer was zero. This differs from never firing; a failed read retains its existing failure accounting. |
| `exit_bootstrap_fired=0 exit_init_stored=0 exit_init_ts=0` | Bootstrap never ran for that fork-seeded lifetime. This alone does not prove that the backend did no work or had no attach opportunity. |

The loss line prints all four fields as `pending` until the EXIT is received.
If that happens later, exactly one `INIT-LOST-EVIDENCE-EXIT:` follow-up prints
the disposition, EXIT timestamp and receipt observation. Join it to the
original `INIT-LOST-EVIDENCE:` by **both pid and lifetime cookie**; an EXIT
for a reused pid with another cookie cannot complete the join, and duplicate
EXITs cannot print it twice. The fixed diagnostic table retains pending
joins until EXIT; table overflow can prevent a join and is still reported by
`history_overflows`. Default-ring cookie-less histories cannot safely join a
late EXIT and remain explicitly weaker.

The receipt callback records disposition before its pre-record sweep and
before capture can discard or queue the record. The capture passes a copy
with only the diagnostic type half cleared to the frozen adapter; the core
still sees the original FORK/INIT/EXIT semantic tags. Original carried bytes
remain available to diagnostics and the output payload. There is **no** new
per-wait BPF work. State carries two additional 32-bit diagnostic fields;
bootstrap stamps one field, attach/reseed preserves them, and EXIT copies
them. Daemon formatting occurs only on loss or its late-EXIT follow-up.

**Accounting is deliberately unchanged.** In particular, a nonzero
`exit_init_ts` does not replace the loss predicate's `init_ts=0`, change the
with/no-opportunity counters, add untraced time, or cancel a counter-change
VOID. The carried timestamp is for the next measurement/decision round.

These complete examples come from `python3 -B tests/test_init_loss_diagnostics.py`;
the before-loss EXIT cases and after-loss EXIT cases use the same facts:

```text
INIT-LOST-EVIDENCE: clock=CLOCK_MONOTONIC pid=42 cookie=7 identity=cookie history=tracked branch=state_missing_bootstrap fork=delivered fork_ts=100 fork_received_ns=110 init_record=not_received init_record_ts=0 init_received_ns=0 exit_record=delivered exit_record_ts=300 exit_received_ns=310 transitions_received=0 bpf_exit_ts=300 bpf_exit_source=lifecycle state_read_after_ns=400 exit_to_read_ns=100 history_overflows=0 exit_bootstrap_fired=1 exit_init_stored=1 exit_init_ts=123 exit_wp_ever_live=0
INIT-LOST-EVIDENCE-EXIT: clock=CLOCK_MONOTONIC pid=42 cookie=7 exit_record_ts=300 exit_received_ns=310 exit_bootstrap_fired=1 exit_init_stored=1 exit_init_ts=123 exit_wp_ever_live=0
INIT-LOST-EVIDENCE: clock=CLOCK_MONOTONIC pid=42 cookie=7 identity=cookie history=tracked branch=state_missing_bootstrap fork=delivered fork_ts=100 fork_received_ns=110 init_record=not_received init_record_ts=0 init_received_ns=0 exit_record=delivered exit_record_ts=300 exit_received_ns=310 transitions_received=0 bpf_exit_ts=300 bpf_exit_source=lifecycle state_read_after_ns=400 exit_to_read_ns=100 history_overflows=0 exit_bootstrap_fired=1 exit_init_stored=0 exit_init_ts=0 exit_wp_ever_live=0
INIT-LOST-EVIDENCE-EXIT: clock=CLOCK_MONOTONIC pid=42 cookie=7 exit_record_ts=300 exit_received_ns=310 exit_bootstrap_fired=1 exit_init_stored=0 exit_init_ts=0 exit_wp_ever_live=0
INIT-LOST-EVIDENCE: clock=CLOCK_MONOTONIC pid=42 cookie=7 identity=cookie history=tracked branch=state_missing_bootstrap fork=delivered fork_ts=100 fork_received_ns=110 init_record=not_received init_record_ts=0 init_received_ns=0 exit_record=delivered exit_record_ts=300 exit_received_ns=310 transitions_received=0 bpf_exit_ts=300 bpf_exit_source=lifecycle state_read_after_ns=400 exit_to_read_ns=100 history_overflows=0 exit_bootstrap_fired=0 exit_init_stored=0 exit_init_ts=0 exit_wp_ever_live=0
INIT-LOST-EVIDENCE-EXIT: clock=CLOCK_MONOTONIC pid=42 cookie=7 exit_record_ts=300 exit_received_ns=310 exit_bootstrap_fired=0 exit_init_stored=0 exit_init_ts=0 exit_wp_ever_live=0
```

Each follow-up above belongs to a separate late-EXIT case, whose first loss
line has `exit_record=not_received` and all four disposition fields `pending`.
The host suite also covers no prior receipt, duplicate EXIT, a replacement
cookie, paired/unpaired BPF EXITs, and disposition preservation through
capture. Both producer EXIT branches have a built mutant that zeros the
carried timestamp and fails its intended assertion. Separate daemon mutants
drop the carried timestamp and suppress the late join. The unchanged
production BPF functions are compiled by `tests/transport_producers_host.py`
with `tests/transport_bpf_harness.h`; this is a C host harness with fake
helpers, **not** a BPF verifier/load test. Linux qualification remains future
work; no BPF program runs on the Mac.

### Plain-churn reproduction on Linux

The live issue body was read with
`gh api repos/DmitryNFomin/pg_wait_tracer/issues/933 --jq '{title,body}'`.
It cites 5/5 Ubuntu 6.8 PG18 runs at 200 connections/s. The cited
`tests/results/session-20261114/b-integration/ubuntu/salvaged-vm/final-plainchurn-pg18-{1..5}/run.txt`
files name `load=churnr200 secs=10`; their stderr contains INIT loss and
`counter_change`. This harness uses that plain select-only `-C` load, with no
connection hold, against the current branch's binary:

```sh
# Linux root, a built branch binary, running PG13/17/18, PG client tools in PATH.
# PGPORT selects the same cluster as --pg-version (or pass its --pid explicitly).
sudo env PATH="$PATH" PGPORT=5418 python3 -B tests/init_loss_repro.py \
  --pg-version 18 --runs 5 --seconds 10 --rate 200 \
  --output /tmp/pgwt-933-evidence
```

Use a fresh output directory outside the repository. The harness creates and
drops its own scale-1 database and starts the tracer before each load. It
prints both INIT diagnostics, suspension lines, and each run's
`capture_ended_counter_change` boolean. `before-stop.stderr`, metrics,
`stop-request-ns.txt`, full stderr, pgbench output, commands and `summary.json`
remain in that output directory. Only permanent `counter_change` suspension
present before the stop request counts as red; shutdown-only VOID does not.
Exit 1 means at least one red run, 0 means the symptom was not reproduced,
2 means a harness error, and 78 means refused. It runs every requested arm
without retries. This is an opt-in measurement harness, intentionally capable
of returning red; it is not a new live qualification gate. Existing live gate
assertions are unchanged. It uses the existing attachment test premise and,
on Ubuntu, the uncertified-kernel override: certification remains false.

### Host verification of the previous receipt-only round

One implementation round, starting HEAD `63426d28`; 0 box hours. Linux
reproduction was not run on the Mac. Token cost belongs to the dispatch log.
Both registration guards and the exact brief gate passed. The backend INIT
order and portable producer/probe compilers now honor CFLAGS; the bookkeeping replay copies the new
header into its current-tree sandbox while keeping the pinned old tree intact.

Each command below passed plain and with
`CFLAGS='-fsanitize=address,undefined -fno-omit-frame-pointer'`.
C builds use `cc -std=c11 -Wall -Wextra -Werror -Isrc
-DPGWT_TRANSPORT_HOST_TEST`, plus those CFLAGS in the sanitized run. C output
binaries are outside the repository. `MERGE` below abbreviates
`src/core/merge_adapter.c src/core/merge.c`; `TRANSPORT` abbreviates
`src/percpu_transport.c src/merge_capture.c` plus MERGE. The contract/adapter/
transport C executables receive `contracts/merge-v1/examples.json` as argv[1].

| Command / source list | Plain | ASan + UBSan |
| --- | --- | --- |
| Compile/run `tests/test_core_merge_contract.c src/core/merge.c` | 99/99 | 99/99 |
| `bash tests/core_merge_mutants.sh` | 36/36 killed | 36/36 killed |
| `python3 -B tests/test_core_merge_index.py` | 112,188 inputs; 262 scenarios; 55,639 outputs identical; work bounds PASS | same |
| `python3 -B tests/core_merge_index_mutants.py` | 3/3 differential + 2/2 cost killed | same |
| `python3 -B tests/test_core_merge_queue.py` | 277,517 inputs; 54 scenarios; 156,592 outputs identical; work bounds PASS | same |
| `python3 -B tests/core_merge_queue_mutants.py` | 7/7 killed | same |
| Compile/run `tests/test_core_merge_adapter.c` + MERGE | 12/12 goldens; 31/31 guards; 78/78 N/Q cases | same |
| `bash tests/core_merge_adapter_mutants.sh` (includes `percpu_transport_mutants.py`) | 27/27 adapter + 91/91 transport killed; 9/9 raw guards; 6/6 virtual deadline cases | same |
| Compile/run `tests/test_transport_wire.c` | 4/4 header goldens; boundary/control/MA bits PASS | same |
| Compile/run `tests/test_transport_producers.c` (portable host route) | pure pieces/host checks PASS; 10/10 flags; 4/4 probe + 26/26 producer mutants killed | same |
| Compile/run `tests/test_percpu_transport.c` + TRANSPORT | 43/43 N rows; 49/49 completion/refusal guards | same |
| Compile/run `tests/test_transport_diagnostics.c` + TRANSPORT | 10/10 pool fills; phase/histogram/schedstat checks PASS | same |
| `python3 -B tests/transport_diagnostics_mutants.py` | 5/5 killed | 5/5 killed |
| `python3 -B tests/test_capture_bookkeeping.py` | 4/4 guards; 6/6 killed; 48,896 qualification bytes + 22 diagnostics lines identical | same |
| `python3 -B tests/test_capture_recovery.py` | 16/16 schedules; 19/19 killed | same |
| Compile/run `tests/test_backend_init.c src/backend_init.c` | sweep/duplicate/EXIT/missing-state accounting suite PASS | same |
| `python3 -B tests/test_backend_init_order.py --mutants` | batch order + loss split/EXIT timing PASS; 3/3 killed | same |
| `python3 -B tests/test_backend_attach_race.py --mutants` | open/bootstrap/enable/identity/errno cases PASS; 1/1 killed | same |
| `python3 -B tests/test_init_loss_diagnostics.py` | 7/7 host groups; 4/4 reproduction-parser cases; 1/1 wrong-branch mutant killed | same |

The new groups cover every loss classification, an EXIT received before a
pre-record sweep, an exit transition arriving before lifecycle EXIT, cookie isolation, counting before capture discard, stopped
counting after attachment, unchanged non-loss decisions, default PID reuse,
collision-chain deletion and table overflow. The wrong-branch mutant builds,
executes the loss cases, then fails the `branch` field assertion; build or
sanitizer failures cannot count as kills. Receipt-only cases also assert that
tracking added neither map reads nor clock calls.

Command evidence for this round:

```console
$ bash scripts/lead/check-brief.sh /private/tmp/claude-501/-Users-dmitryfomin-work-git-pg-wait-tracer/27cc85b4-040d-47d2-ba40-e0c59ac54ec1/scratchpad/m871/brief-933-diag.md
CHECK BRIEF: covered #871, #933
$ bash scripts/check-test-registration.sh
OK: test-registration guard (base d5060bfc0d2329bb7706947ee5f22c70cc734da7) — append-only invariant holds
$ bash scripts/check-runner-registration.sh
OK: runner-registration guard -- every tests/ci_smoke.sh invocation (5) is also invoked by tests/run_all.sh (222 invocations total, including tests/unit_tests.list entries); every candidate test file on disk (201 scanned: tests/test_*.{py,sh} plus tests/web_unit/*.test.mjs) is invoked by a runner or covered by a glob; or is a documented exemption (2 listed, 2 currently invoke no runner).
$ git diff --check
(exit 0; no output)
```

The following tracked-file stat excludes the three new files, which remain
untracked until the dispatch wrapper commits: `src/init_loss_diag.h`,
`tests/init_loss_repro.py`, and `tests/test_init_loss_diagnostics.py`.

```console
$ git diff --stat 63426d28
 docs/871-transport-diagnostics.md | 173 ++++++++++++++++++++++++++++++++++++++
 src/backend.c                     |  14 ++-
 src/backend.h                     |   1 +
 src/backend_init.c                |  19 ++++-
 src/backend_init.h                |   5 ++
 src/daemon.c                      |   6 ++
 src/daemon.h                      |   3 +
 src/event_stream.c                |   2 +
 src/percpu_transport.c            |  21 ++++-
 tests/test_backend_init_order.py  |   2 +-
 tests/test_capture_bookkeeping.py |   2 +
 tests/transport_producers_host.py |   4 +-
 tests/unit_tests.list             |   3 +
 13 files changed, 248 insertions(+), 7 deletions(-)
```


### Host verification of EXIT disposition (round 2)

One implementation round from `627d5ccc`; host tests only, zero box hours.
The commands below were run both plain and with exactly
`CFLAGS='-fsanitize=address,undefined -fno-omit-frame-pointer'`. All C
executables were built outside the repository with
`cc -std=c11 -Wall -Wextra -Werror -DPGWT_TRANSPORT_HOST_TEST -Isrc`, plus
CFLAGS. `TRANSPORT` abbreviates `src/percpu_transport.c src/merge_capture.c
src/core/merge_adapter.c src/core/merge.c`; `MERGE` abbreviates the last two.
The contract, adapter and transport executables take
`contracts/merge-v1/examples.json` as their corpus (the transport can use its
default path). This is unchanged contract evidence, not Linux qualification.

| Command / source list | Plain | ASan + UBSan |
| --- | --- | --- |
| Compile/run `tests/test_transport_wire.c` | 4 header goldens + v2 EXIT golden + boundary/control/MA PASS | same |
| Compile/run `tests/test_transport_producers.c` | pure/host checks and 10 submit-flag branches PASS; 4 probe + 28 producer mutants killed | same |
| Compile/run `tests/test_percpu_transport.c` + TRANSPORT | 43/43 N rows; 50/50 completion/refusal guards | same |
| `python3 -B tests/percpu_transport_mutants.py` | 92/92 host mutants killed; 9/9 raw guards; 6/6 virtual deadline cases | same |
| `python3 -B tests/transport_diagnostics_mutants.py` | 10/10 pool fills; phase/histogram/schedstat PASS; 5/5 mutants killed | same |
| `python3 -B tests/test_capture_bookkeeping.py` | 4/4 guards; 6/6 mutants killed; 48,896 qualification bytes and 22 diagnostic lines identical | same |
| `python3 -B tests/test_capture_recovery.py` | 16/16 schedules; 19/19 mutants killed | same |
| Compile/run `tests/test_backend_init.c src/backend_init.c` | sweep, timestamp, duplicate, EXIT races and missing-state accounting PASS | same |
| `python3 -B tests/test_backend_init_order.py --mutants` | batch order, opportunity partition and EXIT timing PASS; 3/3 killed | same |
| `python3 -B tests/test_backend_attach_race.py --mutants` | open/bootstrap/enable/identity/errno cases PASS; 1/1 killed | same |
| `python3 -B tests/test_init_loss_diagnostics.py` | 14/14 host groups; 4/4 parser cases; 3/3 mutants killed | same |
| Compile/run `tests/test_core_merge_contract.c src/core/merge.c` | 99/99 | same |
| `bash tests/core_merge_mutants.sh` | 36/36 killed | same |
| Compile/run `tests/test_core_merge_adapter.c` + MERGE | 12/12 goldens; 31/31 guards; 78/78 N/Q cases | same |
| `bash tests/core_merge_adapter_mutants.sh` | 27/27 adapter mutants killed (linear and indexed); transport list also passes | same |
| `python3 -B tests/test_core_merge_index.py` | 112,188 inputs; 262 scenarios; 55,639 identical outputs; work bounds PASS | same |
| `python3 -B tests/core_merge_index_mutants.py` | 3/3 differential + 2/2 cost killed | same |
| `python3 -B tests/test_core_merge_queue.py` | 277,517 inputs; 54 scenarios; 156,592 identical outputs; work bounds PASS | same |
| `python3 -B tests/core_merge_queue_mutants.py` | 7/7 killed | same |

The v2 decoder negative controls retain the existing assertions: their
invalid-version input is now zero, because the former invalid value 2 is
this round's valid transport version. The historical bookkeeping sources,
expected contract answers, tolerances and live runner assertions are unchanged.
The new capture mutant omits EXIT diagnostic normalization and fails the
three-disposition release assertion. Both BPF timestamp-dropping mutants
build and fail their corresponding paired/L-only timestamp assertion;
the daemon timestamp and late-join mutants fail their named output fields.
Sanitizer errors do not count as mutant kills.

```console
$ git log --oneline -1
627d5ccc docs: an unreadable recovery baseline ends automatic recovery; the time identity's terms are independent
$ bash scripts/lead/check-brief.sh /private/tmp/claude-501/-Users-dmitryfomin-work-git-pg-wait-tracer/27cc85b4-040d-47d2-ba40-e0c59ac54ec1/scratchpad/m871/brief-933-disposition.md
CHECK BRIEF: covered #871, #933
$ bash scripts/check-test-registration.sh
OK: test-registration guard (base d5060bfc0d2329bb7706947ee5f22c70cc734da7) — append-only invariant holds
$ bash scripts/check-runner-registration.sh
OK: runner-registration guard -- every tests/ci_smoke.sh invocation (5) is also invoked by tests/run_all.sh (222 invocations total, including tests/unit_tests.list entries); every candidate test file on disk (201 scanned: tests/test_*.{py,sh} plus tests/web_unit/*.test.mjs) is invoked by a runner or covered by a glob; or is a documented exemption (2 listed, 2 currently invoke no runner).
$ git diff --check
(exit 0; no output)
```

<!-- EXIT-DISPOSITION-DIFFSTAT -->
```console
$ git diff --stat 627d5ccc
 docs/871-transport-diagnostics.md   | 172 +++++++++++++++++++++++++++++++++++-
 src/backend.c                       |   2 +
 src/bpf/pg_wait_tracer.bpf.c        |  25 +++++-
 src/escalation.c                    |   2 +
 src/init_loss_diag.h                |  44 ++++++++-
 src/merge_capture.c                 |  21 ++++-
 src/percpu_transport.c              |   7 +-
 src/pg_wait_tracer.h                |  14 ++-
 src/pgwt_transport_wire.h           |   7 +-
 tests/Makefile                      |   2 +-
 tests/capture_recovery_host.c       |   2 +-
 tests/percpu_transport_mutants.py   |   2 +
 tests/test_init_loss_diagnostics.py |  98 +++++++++++++++++---
 tests/test_percpu_transport.c       |  43 ++++++++-
 tests/test_transport_producers.c    |   2 +-
 tests/test_transport_wire.c         |  33 +++++--
 tests/transport_bpf_harness.h       |  36 +++++++-
 tests/transport_producers_host.py   |  10 +++
 18 files changed, 483 insertions(+), 39 deletions(-)
```
<!-- /EXIT-DISPOSITION-DIFFSTAT -->

### Opt-in attachment timeline (Refs #933, Refs #871)

`--attach-timeline` enables userspace timing only. It does not change attach
order, producers, wakeup flags, counters, integrity or VOID decisions. The
existing debug-block switch is independent. The previously supplied
`session-20261117/i933-disposition/analysis.txt` was read, not re-run; this
unit adds the measurement needed to locate the observed fork-to-attach delay.
It makes no claim yet about which stage accounts for that live delay.

Each dequeued lifecycle EXIT emits `ATTACH-TIMELINE: ` followed by one JSON
object. All backends are logged, including losses; there is no sampling.
Receipt timing happens at the start of the raw ring callback, before
decoding/capture disposition and before the per-record INIT sweep. In exact
mode the cookie identifies the lifetime even when loss was observed before
the EXIT arrived. The default wire is explicitly `identity=pid_only` and
cannot disambiguate a delayed old record after PID reuse. A newly observed
FORK resets that PID's timeline. Remembered duplicate EXITs print once.

Every timestamp is CLOCK_MONOTONIC nanoseconds. BPF `ktime_get_ns()` and the
userspace monotonic clock use the same time domain. Missing values are JSON
`null`, including failed resource-usage reads. Zero context-switch counts,
zero preceding records and zero measured durations remain real zeros.

| Field | Meaning |
| --- | --- |
| `clock` | Always `CLOCK_MONOTONIC`. |
| `pid`, `cookie`, `identity` | Receipt identity; cookie 0 is absent and labelled `pid_only`. This is not an attachment certificate. |
| `fork_ns` | Timestamp carried by the BPF FORK record. Null without a received FORK, including pre-existing backends. |
| `rb_receive_ns` | Clock read at entry to the raw callback that dequeued this FORK. It is not a release/replay time or a cached batch clock. |
| `epoll_return_ns` | Most recent successful epoll return preceding that dequeue, including timeout/poll returns. Both the outer daemon epoll and the transport's inner epoll update it. Null before the first return, or after an interrupted/failed wait. |
| `batch_records_before` | Number of raw L callbacks before this FORK in the same actual `ring_buffer__consume` call, counting query-text and boundary records too. Reset for every consume in both transport entry points and the default lifecycle reader. Null without a FORK receipt. |
| `handler_entry_ns`, `handler_exit_ns` | Immediately before/after the call to `pgwt_handle_fork`, including its early returns and direct-INIT path. Entry is after the pre-record sweep; receive-to-handler includes that sweep and intervening capture/dispatch work. |
| `bootstrap_open_start_ns` | Immediately before the bootstrap watchpoint open. This separates work before the open from the open itself. |
| `bootstrap_open_ns`, `bootstrap_open_ok` | End of the bootstrap open (which includes enabling it), and whether it succeeded. The timestamp alone never asserts it armed. |
| `bootstrap_fire_ns` | Existing stored INIT timestamp, obtained from INIT handling/receipt or the EXIT disposition. This timestamps stored INIT, not necessarily the first handler invocation. A fired-but-not-stored bootstrap has no carried fire timestamp and stays null. No BPF timestamp is added. |
| `wait_open_ns`, `wait_open_ok` | End/result of the disabled real-wait-watchpoint open. |
| `wait_enable_ns`, `wait_enable_ok` | End/result of enabling the real wait watchpoint. |
| `exit_ns` | Actual BPF timestamp of the received lifecycle EXIT. A userspace liveness observation never substitutes for this value. |
| `handler_entry_nvcsw`, `handler_entry_nivcsw` | Daemon thread's cumulative voluntary/involuntary switch counts sampled with `getrusage(RUSAGE_THREAD)` at FORK-handler entry. These are counts, not nanoseconds. |
| `handler_exit_nvcsw`, `handler_exit_nivcsw` | Same thread counts at FORK-handler return. Their deltas identify switches during that call; they do not measure how long the thread waited before receipt. |
| `outcome` | `attached` after a recorded successful wait enable; otherwise `never_fired`, `fired+stored` or `fired+not_stored` from the existing EXIT disposition. `unknown` when neither successful enable nor disposition is available, including default-ring losses without disposition. `wp_ever_live` is deliberately not treated as successful-enable evidence. |
| `lost` | True only when the existing INIT-loss predicate actually counted this lifetime. Diagnostic disposition never cancels that loss or changes its opportunity class. |

Open/enable fields retain the first successful return, or the first failed
return if no attempt succeeded. The extra bootstrap-start field retains the
first attempt's start. Existing recovery can already attempt attachment
again; the diagnostic introduces no attempts. The normal plain-churn
measurement uses full mode, so later tier changes do not define these times.
Instrumented clock/resource/formatting helpers preserve `errno` at the
attachment and receipt seams.

At shutdown `ATTACH-TIMELINE-SUMMARY: ` emits JSON for `population=all` and
`population=lost`, each with `outcome=all` and each of the five outcome
values. `backends` is the number of completed timelines in that group.
Each distribution contains its own valid-sample `n` plus `p50`, `p90`, `p99`
and `max`, in nanoseconds:

| Distribution | Subtraction / inclusion |
| --- | --- |
| `fork_to_receive_ns` | `rb_receive_ns - fork_ns`. |
| `receive_to_handler_ns` | `handler_entry_ns - rb_receive_ns`. |
| `handler_to_bootstrap_armed_ns` | `bootstrap_open_ns - handler_entry_ns`, successful bootstrap opens only. |
| `fork_to_wait_enabled_ns` | `wait_enable_ns - fork_ns`, successful wait enables only. |

Missing or reversed endpoints do not contribute; n=0 has null percentiles
and max. Quantiles use nearest rank `ceil(n * percentile / 100)`, with no
interpolation. Thus [1,2,3,5,6,7,8,9,10,20] has p50=6, p90=10, p99=max=20.
The population is **received EXITs**, not every birth: still-live backends
and lifetimes without a delivered EXIT cannot contribute a completed row.

`ATTACH-TIMELINE-STATS: ` reports `completed`, `pending`, `table_overflows`,
`summary_dropped`, `replaced_pending`, and `summary_allocation_ok`. These
belong only to the diagnostic, not the daemon's integrity counter vector.
The active table has 4,096 slots; completed slots can be reused. Overflow
means some histories/lines are unavailable. `replaced_pending` counts a new
default-wire FORK replacing an uncompleted PID history. If summary storage
cannot grow, lines still print and `summary_dropped` names the omitted
samples. A shutdown allocation failure prints stats with
`summary_allocation_ok=false` instead of fabricated distributions. Exact
summary samples retain O(completed lifetimes) memory; sorting is deferred
until shutdown. A pending lifetime gets no invented EXIT or completion.

Cost when disabled: the daemon keeps a null timeline pointer and checks it
at the hooks. It performs no added diagnostic clock reads, resource-usage
syscalls, map reads, table work, allocation, formatting, or per-backend
storage initialization. There are additional conditional pointer checks;
this is a code-inspection statement, not a measured zero CPU overhead claim.
The host production-open matrix asserts zero timeline clock calls while
off, and the existing raw-reader budget harness has a fail-if-called clock
stub. When enabled, raw L callbacks read the clock, FORK handlers read
thread usage twice, and every EXIT assembles a line on the stack and writes it with one stdio call:
these costs can perturb
the latency being measured. No Linux overhead result is claimed here.

The reproduction command is unchanged except for the opt-in flag:

```sh
sudo env PATH="$PATH" PGPORT=5418 python3 -B tests/init_loss_repro.py \
  --pg-version 18 --runs 5 --seconds 10 --rate 200 --attach-timeline \
  --output /tmp/pgwt-933-attach-timeline
```

Each run writes `attach-timeline.jsonl` (parsed EXIT objects) and
`attach-timeline-summary.json` (shutdown summary objects and stats), as well
as the existing full `daemon.stderr`, `before-stop.stderr`, metrics,
pgbench output and commands. Summary extraction happens after shutdown.
The harness's expected-red return codes, stimulus and assertions are
unchanged. This command is for Linux; it was not executed on macOS.


### Attachment-timeline host evidence

One authorised implementation round from `27c2c2d6`; host-only, 0 box hours.
The dispatch started at 2026-11-17 07:44:38 UTC; token accounting belongs to
its dispatch log. Two compilation errors and one fixture setup error in the new test were
corrected before these successful runs. The existing raw-budget harness
needed an explicit fail-if-called diagnostic-clock stub to link the newly
instrumented reader bodies; its assertions and mutant list are unchanged.
The bookkeeping sandbox copies the new transitive header only into its
current-tree arm; frozen comparison sources are unchanged.

Synthetic output from `python3 -B tests/test_attach_timeline.py` (also
identical under the sanitizer flags below):

```text
ATTACH-TIMELINE: {"clock":"CLOCK_MONOTONIC","pid":42,"cookie":7,"identity":"cookie","outcome":"attached","lost":false,"fork_ns":1000,"rb_receive_ns":1003,"epoll_return_ns":1001,"batch_records_before":7,"handler_entry_ns":1033,"handler_exit_ns":1336,"bootstrap_open_start_ns":1034,"bootstrap_open_ns":1333,"bootstrap_fire_ns":1002,"wait_open_ns":2500,"wait_enable_ns":4000,"exit_ns":4001,"handler_entry_nvcsw":10,"handler_entry_nivcsw":11,"handler_exit_nvcsw":20,"handler_exit_nivcsw":21,"bootstrap_open_ok":true,"wait_open_ok":true,"wait_enable_ok":true}
ATTACH-TIMELINE-SUMMARY: {"population":"all","outcome":"all","backends":10,"fork_to_receive_ns":{"n":10,"p50":6,"p90":10,"p99":20,"max":20},"receive_to_handler_ns":{"n":10,"p50":60,"p90":100,"p99":200,"max":200},"handler_to_bootstrap_armed_ns":{"n":9,"p50":700,"p90":2000,"p99":2000,"max":2000},"fork_to_wait_enabled_ns":{"n":4,"p50":2000,"p90":10000,"p99":10000,"max":10000}}
```

Both configurations ran the commands in the table. Plain uses empty
`CFLAGS`; sanitized uses exactly
`CFLAGS='-fsanitize=address,undefined -fno-omit-frame-pointer'`.
The host runner set `ASAN_OPTIONS=halt_on_error=1:exitcode=86`,
`UBSAN_OPTIONS=halt_on_error=1:exitcode=87:print_stacktrace=1`, and
`PYTHONDONTWRITEBYTECODE=1`. Sanitizer failures cannot count as mutant kills.

For C rows the exact common compile command is
`cc -std=c11 -Wall -Wextra -Werror -DPGWT_TRANSPORT_HOST_TEST -Isrc $CFLAGS
 tests/NAME.c SOURCES -o /private/tmp/pgwt-933-timeline-validation/MODE/NAME`,
then that executable is run from the repository root. The contract and
adapter executables receive `contracts/merge-v1/examples.json` as argv[1].
`TRANSPORT` means `src/percpu_transport.c src/merge_capture.c
src/core/merge_adapter.c src/core/merge.c`; `MERGE` means the last two.
All binaries and generated mutation trees were outside the repository.

| Command / C test and extra sources | Plain | ASan + UBSan |
| --- | --- | --- |
| `test_transport_wire` | 5 goldens; boundary/control/MA bits PASS | same |
| `test_transport_producers` | portable checks; 10/10 flags; 4/4 probe and 28/28 producer mutants killed | same |
| `test_percpu_transport` + TRANSPORT | 43/43 N rows; 50/50 completion/refusal guards | same |
| `test_transport_diagnostics` + TRANSPORT | 10/10 pool fills; phase/histogram/schedstat suite PASS | same |
| `test_backend_init` + `src/backend_init.c` | sweep/timestamp/duplicate/EXIT/missing-state suite PASS | same |
| `test_core_merge_contract` + `src/core/merge.c` | 99/99 | same |
| `test_core_merge_adapter` + MERGE | 12/12 goldens; 31/31 guards; 78/78 N/Q cases | same |
| `python3 -B tests/test_attach_timeline.py` | 10/10 synthetic EXITs; 12/12 summary groups; edge/production/off-path guards PASS; 3/3 mutants killed | same |
| `python3 -B tests/test_backend_init_order.py --mutants` | batch/order/opportunity/EXIT suite PASS; 3/3 killed | same |
| `python3 -B tests/test_backend_attach_race.py --mutants` | open/bootstrap/enable/identity/errno suite PASS; 1/1 killed | same |
| `python3 -B tests/test_init_loss_diagnostics.py` | 14/14 host groups; 4/4 parser cases; 3/3 killed | same |
| `python3 -B tests/transport_diagnostics_mutants.py` | 10/10 pool fills; 5/5 killed | same |
| `python3 -B tests/test_capture_bookkeeping.py` | 4/4 guards; 6/6 killed; 48,896 qualification bytes and 22 diagnostics lines identical | same |
| `python3 -B tests/test_capture_recovery.py` | 16/16 schedules; 19/19 killed | same |
| `bash tests/core_merge_mutants.sh` | 36/36 killed | same |
| `bash tests/core_merge_adapter_mutants.sh` | 27/27 adapter and 92/92 transport killed; 9/9 raw-budget guards; 6/6 virtual deadline cases | same |
| `python3 -B tests/test_core_merge_index.py` | 112,188 inputs; 262 scenarios; 55,639 identical outputs; work bounds PASS | same |
| `python3 -B tests/core_merge_index_mutants.py` | 3/3 differential and 2/2 work mutants killed | same |
| `python3 -B tests/test_core_merge_queue.py` | 277,517 inputs; 54 scenarios; 156,592 identical outputs; work bounds PASS | same |
| `python3 -B tests/core_merge_queue_mutants.py` | 7/7 killed | same |
| `python3 -B tests/test_counter_failures.py` | natural-error/silent-path/success/once-only suite PASS | same |
| `python3 -B tests/test_counter_vector_portable.py` | 15-component vector, ring, writer, daemon reads and mocked phase routing PASS | same |
| `python3 -B tests/counter_vector_mutants.py` | 32/32 killed | same |

The new field-swap mutant builds and completes all synthetic timelines,
then fails `FAIL field fork_ns:`. The wrong-median and wrong-lost-population
mutants likewise build and reach the summary arithmetic assertions. Edge
cases cover real zero durations, reversed endpoints, nulls, failed thread
usage, lost-cookie isolation, late/duplicate EXIT, default PID reuse,
pending lifetimes, table overflow and summary allocation failure. Production
bootstrap/wait-open/enable and receipt/EXIT bodies run with injected I/O;
the actual epoll, raw-callback and consume-reset hook placements are also
pinned in source. Existing loss and non-loss decisions remain asserted.

The Linux-only `test_backend_lifecycle` and `test_tick_drain` binaries, full
Linux daemon build and live reproduction are not run on macOS. The table
records portable production-body and daemon-loop/raw-reader coverage,
not Linux load/verifier or live latency evidence. No live gate assertion,
known-failing rule, contract, expected answer or tolerance was changed.

Final guard and working-tree evidence:

<!-- ATTACH-TIMELINE-GUARDS -->
```console
$ git log --oneline -1
27c2c2d6 Carry bootstrap and stored INIT disposition on exact-perf EXIT
$ bash scripts/lead/check-brief.sh /private/tmp/pgwt-codex/agent%2F933-attach-latency.lock/checked.wevW0g
CHECK BRIEF: covered #871, #933
$ bash scripts/check-test-registration.sh
OK: test-registration guard (base d5060bfc0d2329bb7706947ee5f22c70cc734da7) — append-only invariant holds
$ bash scripts/check-runner-registration.sh
OK: runner-registration guard -- every tests/ci_smoke.sh invocation (5) is also invoked by tests/run_all.sh (223 invocations total, including tests/unit_tests.list entries); every candidate test file on disk (202 scanned: tests/test_*.{py,sh} plus tests/web_unit/*.test.mjs) is invoked by a runner or covered by a glob; or is a documented exemption (2 listed, 2 currently invoke no runner).
$ git diff --check
(exit 0; no output)
$ python3 -B /private/tmp/pgwt-933-timeline-validation/run.py
VALIDATION: 46/46 command groups passed
VALIDATION: {'plain': 23, 'sanitized': 23} failed= 0
```
<!-- /ATTACH-TIMELINE-GUARDS -->


The required report-audit command was attempted after host validation. It
returned INCONCLUSIVE before an auditor could run; this is a reporting-tool
initialization failure, not an independent readiness verdict:

```console
$ bash scripts/lead/audit-report.sh /private/tmp/pgwt-933-timeline-validation/final-report.md
PREPASS: no unevidenced absence claim found
AUDIT INCONCLUSIVE (auditor exit 1) -- do not send. draft=0581f6e85eae7d437cf52324e68d0857912d67990945062c474a9172b0ca9e8e
$ tail -25 /private/var/folders/tf/4mlc41nx3zsbx0v4x8wy4hl00000gn/T/pgwt-report-audits-501/7b521caa56420570fc0229a8a5043699d8e150876d20995eaf67477aae8848f1/0581f6e85eae7d437cf52324e68d0857912d67990945062c474a9172b0ca9e8e/transcript.txt
WARNING: proceeding, even though we could not create PATH aliases: Operation not permitted (os error 1)
Reading additional input from stdin...
Error: failed to initialize in-process app-server client: Operation not permitted (os error 1)
```

`git diff --stat` excludes the new untracked `src/attach_timeline.h` and
`tests/test_attach_timeline.py` until the dispatch wrapper commits them.
Their line counts are included below; no staging or commit is performed
inside this sandbox.

<!-- ATTACH-TIMELINE-DIFFSTAT -->
```console
$ git diff --stat 27c2c2d6
 docs/871-transport-diagnostics.md | 248 ++++++++++++++++++++++++++++++++++++++
 src/backend.c                     |  17 +++
 src/daemon.c                      |  33 +++++
 src/daemon.h                      |   3 +
 src/percpu_transport.c            |  17 +++
 src/pg_wait_tracer.c              |   4 +
 tests/init_loss_repro.py          |  23 ++++
 tests/percpu_budget_harness.h     |   7 ++
 tests/test_capture_bookkeeping.py |   1 +
 tests/unit_tests.list             |   3 +
 10 files changed, 356 insertions(+)
$ wc -l src/attach_timeline.h tests/test_attach_timeline.py
     308 src/attach_timeline.h
     357 tests/test_attach_timeline.py
     665 total
```
<!-- /ATTACH-TIMELINE-DIFFSTAT -->

## Exact transport startup refusals

Exact-perf requires transport helpers running with `SCHED_OTHER`. Starting
the tracer with real-time scheduling (for example, `chrt -f 1`) is not supported
yet: startup prints the cause on stderr and exits non-zero. This diagnostic
does not change the scheduling policy or the existing refusal.

An unreadable initial loss-counter vector also refuses startup. Its stderr
line names the first unreadable component and its lookup errno when available;
`errno=unknown` means that the failing observation supplied no errno. Later
successful lookups cannot replace that component's error.

The cut/discard harness regenerates a BPF skeleton whose map member order can
differ from the product skeleton. It rebuilds userspace objects against that
header and refuses the final link if compiler dependencies or object/header
fingerprints disagree. Portable diagnostic and link-guard tests run with
`python3 -B tests/test_transport_startup.py`; Linux kernel execution remains
part of the live transport tests.
