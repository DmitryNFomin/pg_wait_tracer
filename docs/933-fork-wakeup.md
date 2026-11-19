# Exact FORK wakeup: measurement handoff

Base: `ce402968`, branch `agent/933-attach-latency`. One implementation round.
This unit reduces the measured notification delay before attachment. Linux
latency, loss-rate and verifier-load results for this tree are pending the lead's
run; the host results below establish plumbing and protocol behavior only.

The exact-perf `on_fork` submit selects `BPF_RB_FORCE_WAKEUP` by default.
`--test-fork-wakeup=adaptive` sets the test-only rodata boolean and restores flags
0 at that same submit. `--test-fork-wakeup=force` explicitly selects the default.
The knob has no effect on the raw, non-exact FORK route. Other submit policies
retain their existing producer assertions, including the watchpoint route.

## EL8 4.18 compatibility evidence

Read the source extracted from `kernel-4.18.0-553.157.1.el8_10.src.rpm` for the
previous mechanism measurement:

```console
$ shasum -a 256 /Users/dmitryfomin/work/git/pg_wait_tracer/tests/results/session-20261111/m0c/el8/ksrc/kernel/bpf/ringbuf.c
4dd84445898d1142b960039ea7cf1bc57ab192582694f2a606af65246e5f4cd5  /Users/dmitryfomin/work/git/pg_wait_tracer/tests/results/session-20261111/m0c/el8/ksrc/kernel/bpf/ringbuf.c
```

At lines 410–413, that vendor kernel's `bpf_ringbuf_commit` checks
`flags & BPF_RB_FORCE_WAKEUP` and queues `irq_work` unconditionally; adaptive
notification instead requires the consumer position to equal this record's
position. At lines 422–426 its submit helper prototype has
`.arg2_type = ARG_ANYTHING`. Thus the submit flags argument accepts a scalar
selected from rodata; it does not require a literal or constant-size argument.
This is source-level compatibility evidence, not a load of the changed object.
The existing producer already uses FORCE_WAKEUP for boundary submits; its
previous EL8 verifier results and rodata folding are documented in
[the A1 producer argument](871-a1-producer-argument.md#what-the-418-verifier-needs-measured-on-el8).

The [kernel ring-buffer documentation](https://docs.kernel.org/bpf/ringbuf.html)
explains adaptive notification and explicit FORCE_WAKEUP control. The vendor
source above is the evidence specific to EL8's backport. Run the existing Linux
verifier matrix and the paired daemon command below on the new build; retain
the daemon load log for each arm to demonstrate both rodata choices load.

## Round safety

Additional FORK wakes need no round-timing change. A new round is requested only
in `pgwt_transport_poll` (`src/percpu_transport.c:640–645`), reached by the normal
live path at most once per clock-anchored 10 ms service slot
(`src/percpu_transport.c:1536–1544`). The off-slot capacity-paused path reaches
recovery while suspended, before the request branch (:630–636).

Off-slot `fence_round` and `complete_round` progress an already requested round;
they cannot start a new one. Fencing requires that round's E/L boundaries, and
completion requires every CPU's higher-round acknowledgement and the frozen
targets. Retrying them on every pass preserves prompt completion after the last
acknowledgement without waiting for another service slot. Early wakes do not
reset the slot, and shutdown keeps its existing bounded completion loop.

The raw readers are drained before the slot check on every pass
(`src/percpu_transport.c:1531–1536`), as they were at `ce402968`. The drain uses
`consume_all` to consume hint/perf and E/L readers (:1138–1186). Thus an off-slot
pass does not leave E/perf unread merely because it is off-slot; the previous
comment's proposed spin until the next slot does not describe this code. The
existing drain budgets and backlog resumption remain in effect.

The `never_empty` host schedule checks completion on the pass that consumes the
last acknowledgement, plus off-slot fencing and empty-pass completion of the
next round. The built `slot_gated_completion` mutant must fail that assertion.
The existing 100,000-early-wake cadence case checks that new requests remain
slot-gated; the completeness, counter, cut, discard, budget and shutdown
mutations continue to run.

## Diagnostic populations

`ATTACH-TIMELINE-STATS` adds `outer_loop_wakes`,
`outer_loop_lifecycle_wakes`, and `outer_loop_elapsed_ns`. A wake is a successful
outer `epoll_wait` return, including a timeout or a zero-time backlog return.
The lifecycle count increments once when any L record is dequeued during that
pass, across all batches. Errors and inner shutdown waits are excluded. Elapsed
time spans the first through the last successful outer return. Rates are null
when that interval is unavailable; they include timer/control/backlog and idle
polling work as well as FORKs. Compare the rate with births, not just transactions.

The paired report uses every emitted completed timeline, including losses.
Each population reports its backend denominator, lost count, counts and losses
by outcome, and nearest-rank fork-to-receive p50/p90/p99 in ns with the valid
latency denominator. It reports the `batch_records_before=0` and `>0` strata,
plus an explicit unknown-stratum count. Pending lifetimes and diagnostic
omissions remain visible in the raw diagnostic stats; they are not successful
attachments or zero latencies. Per-run files keep the complete raw C summaries.

## Paired Linux command

On the same VM and build, with the PostgreSQL client tools/environment for the
chosen server:

```sh
sudo -E python3 -B tests/init_loss_repro.py \
  --binary "$PWD/pg_wait_tracer" --pg-version 18 \
  --runs 3 --seconds 10 --rate 200 --paired-wakeup \
  --output /tmp/i933-wakeup-el8-pg18
```

Repeat for PG13 and for Ubuntu, with a fresh output directory per cell. The
schedule is force/adaptive, adaptive/force, force/adaptive. Timeline collection
is enabled automatically. Both arms use the same binary path and stimulus;
there are no retries or altered connection holds. The existing uncertified
kernel and attachment premises remain explicit mechanism-test premises.
`summary.json` contains each run's report; `paired-summary.json` aggregates each
arm while summing observation periods, without bridging the time between runs.
`commands.json`, `daemon.stderr`, `before-stop.stderr` and per-run timeline files
retain the evidence. Exit 1 still means the expected-red counter-change symptom
occurred before stop; it does not mean the paired artifacts were discarded.

Lead's targets: force p50 <0.3 ms in every cell and in the >0 stratum, plus PG18
p90 <2 ms; adaptive >0-stratum p50 >3 ms. A green adaptive control invalidates
the latency comparison. PG13's long service tail remains a measurement question.

## Host verification

The following commands run with `CFLAGS=''` and again with
`CFLAGS='-fsanitize=address,undefined -fno-omit-frame-pointer'`.
All generated C files and executables are in a temporary directory outside the
repository. `PGWT_TRANSPORT_HOST_TEST` keeps the Linux loader out of host builds.

For each C row, compile with:

```sh
cc -std=gnu11 -Wall -Wextra -Werror -DPGWT_TRANSPORT_HOST_TEST -I src \
  $CFLAGS <sources below> -o <external temporary binary>
<external temporary binary>
```

| C suite | Sources after the compiler flags | Plain | ASan/UBSan |
|---|---|---|---|
| Backend INIT | `tests/test_backend_init.c src/backend_init.c` | PASS | PASS |
| Wire | `tests/test_transport_wire.c` | PASS | PASS |
| Producer | `tests/test_transport_producers.c` | PASS, 11/11 submit policies | PASS, 11/11 submit policies |
| Transport | `tests/test_percpu_transport.c src/percpu_transport.c src/merge_capture.c src/core/merge_adapter.c src/core/merge.c` | 43/43 rows, 50/50 guards | 43/43 rows, 50/50 guards |
| Diagnostics | `tests/test_transport_diagnostics.c src/percpu_transport.c src/merge_capture.c src/core/merge_adapter.c src/core/merge.c` | 10 pool fills, phase/replay/histogram/schedstat PASS | same PASS |

| Command | Plain result | ASan/UBSan result |
|---|---|---|
| `python3 -B tests/test_attach_timeline.py` | All field/summary/edge/production/off-path groups PASS; 10/10 mutants killed | same PASS, 10/10 |
| `python3 -B tests/test_init_loss_diagnostics.py` | 14/14 host groups PASS; 3/3 mutants killed | same PASS, 3/3 |
| `python3 -B tests/test_backend_init_order.py --mutants` | baseline PASS; 3/3 mutants killed | same PASS, 3/3 |
| `python3 -B tests/test_backend_attach_race.py --mutants` | baseline PASS; 1/1 mutant killed | same PASS, 1/1 |
| `python3 -B tests/percpu_transport_mutants.py` | 92/92 mutants killed, including slot-gated completion | same PASS, 92/92 |
| `python3 -B tests/transport_diagnostics_mutants.py` | baseline PASS; 5/5 mutants killed | same PASS, 5/5 |
| `python3 -B tests/test_core_merge_index.py` | index work PASS | same PASS |
| `python3 -B tests/core_merge_index_mutants.py` | 3/3 differential and 2/2 cost mutants killed | same PASS, 5/5 |
| `python3 -B tests/test_core_merge_queue.py` | queue work PASS | same PASS |
| `python3 -B tests/core_merge_queue_mutants.py` | 4/4 work, 1/1 tie-break, 1/1 held suffix, 1/1 payload pairing mutants killed | same PASS, 7/7 |
| `bash tests/core_merge_mutants.sh` | 36/36 mutants killed | same PASS, 36/36 |
| `bash tests/core_merge_adapter_mutants.sh` | 12/12 goldens, 31/31 guards, 78/78 end-to-end (linear/indexed); 27/27 mutants killed | same PASS, 27/27 |

The registered producer C suite also invokes
`python3 -B tests/transport_producers_host.py`: 29/29 producer mutants and 4/4
probe-loop mutants killed in each configuration. Its live-driver host checks,
4096-arrival churn simulation, diagnostic forwarding and portable golden pieces
pass in each configuration. The producer knob-ignored mutant builds, reaches
the adaptive control and fails `DATA fork L adaptive test control`.

The timeline suite compiles the production getopt row, switch arm and pre-load
rodata assignment. It checks default force, adaptive, explicit force, invalid
values and last-option-wins. The rodata-ignored mutant fails the same plumbing
assertion as the baseline adaptive arm would. Three accounting mutants reject
counting epoll errors, counting records as wakes, and failing to reset the
per-wake lifecycle marker. Hand-calculated paired-report cases cover ranks,
strata, loss denominators, outcomes, nulls and rates.

Registration and whitespace evidence:

```console
$ scripts/check-test-registration.sh
OK: test-registration guard (base d5060bfc0d2329bb7706947ee5f22c70cc734da7) — append-only invariant holds
$ scripts/check-runner-registration.sh
OK: runner-registration guard -- every tests/ci_smoke.sh invocation (5) is also invoked by tests/run_all.sh (223 invocations total, including tests/unit_tests.list entries); every candidate test file on disk (202 scanned: tests/test_*.{py,sh} plus tests/web_unit/*.test.mjs) is invoked by a runner or covered by a glob; or is a documented exemption (2 listed, 2 currently invoke no runner).
$ git diff --check
(exit 0; no output)
```

Dispatch validation:

```console
$ scripts/lead/check-brief.sh /private/tmp/claude-501/-Users-dmitryfomin-work-git-pg-wait-tracer/27cc85b4-040d-47d2-ba40-e0c59ac54ec1/scratchpad/m933l/brief-wake.md
CHECK BRIEF: covered #871, #933
```


Working-tree diff (the new handoff document is untracked until the wrapper commits):

```console
$ git diff --stat ce402968
 src/attach_timeline.h             |  25 ++++++-
 src/bpf/pg_wait_tracer.bpf.c      |   4 +-
 src/daemon.c                      |   5 ++
 src/daemon.h                      |   1 +
 src/percpu_transport.c            |  13 ++--
 src/percpu_transport.h            |   6 +-
 src/pg_wait_tracer.c              |  11 +++
 tests/init_loss_repro.py          |  71 ++++++++++++++++--
 tests/percpu_transport_mutants.py |   6 +-
 tests/test_attach_timeline.py     | 146 +++++++++++++++++++++++++++++++++++++-
 tests/test_percpu_transport.c     |  18 +++--
 tests/transport_bpf_harness.h     |   9 ++-
 tests/transport_producers_host.py |   9 ++-
 13 files changed, 291 insertions(+), 33 deletions(-)
```

New documentation: `docs/933-fork-wakeup.md`.

## Report audit limitation

The required report audit was attempted after the host checks. It could not
initialize its nested Codex client in this sandbox, so it issued no SEND verdict.
This is an audit execution failure, not a successful report audit.

```console
$ scripts/lead/audit-report.sh /private/tmp/i933-wake-work/final-report.txt
PREPASS: no unevidenced absence claim found
AUDIT INCONCLUSIVE (auditor exit 1) -- do not send. draft=aa0ad8150a8a7b7c9944adc5430c8f7a0c7b38500f1050e0c5c9db97f2c3044d
```

The auditor transcript contains:

```text
WARNING: proceeding, even though we could not create PATH aliases: Operation not permitted (os error 1)
Reading additional input from stdin...
Error: failed to initialize in-process app-server client: Operation not permitted (os error 1)
```

Implementation stops here under the brief's failed-check stop rule. Linux
verifier loads and paired latency acceptance are still assigned to the lead.
