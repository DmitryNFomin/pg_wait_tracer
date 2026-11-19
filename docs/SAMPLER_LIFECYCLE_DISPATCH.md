# Lifecycle enrollment before sampling (Refs #866)

The sample-timer fd branch consumes the lifecycle ring immediately before
`handle_sample_timer`. This admits a queued FORK before target construction
when the timer precedes the lifecycle fd in `events[]`, including when the
record arrived after `epoll_wait` returned. The existing lifecycle consumer,
identity checks, transport and processing policy are reused unchanged.

## Host regression and cost evidence

Run on macOS or Linux, without PostgreSQL or BPF:

```sh
make -C tests test_sampler_dispatch
tests/test_sampler_dispatch
tests/test_sampler_dispatch --benchmark
```

The test compiles the production fd dispatcher, lifecycle consumer, sample
timer handler, pending-INIT sweep, FORK/enrollment functions and sampler
target loop. Kernel/process I/O uses the existing initialized shared-row
fixture. It checks PG13, PG17 and PG18 with both fd orders, a queued FORK
whose ring fd is absent from the returned events, and the following tick.
It requires both a target with PID 1000 and an emitted sample in that poll.
The registered C target also runs in the Mac deterministic tier.

Red control against the supplied starting revision (temporary files outside
the repository):

```sh
git show ff5511a0:src/daemon.c > /tmp/p866-daemon-ff5511a0.c
python3 -B tests/build_sampler_dispatch.py \
  --daemon-source /tmp/p866-daemon-ff5511a0.c \
  --output /tmp/p866-dispatch-ff5511a0
/tmp/p866-dispatch-ff5511a0
```

Observed exit 1, after a successful C build:

```text
FAIL: PG13 queued FORK pid=1000 must be a sampler target in the same poll (targets=0 samples=0 ring_first=0 ring_returned=1)
```

That revision has inline dispatch. The builder wraps its unchanged branch
body in the same callable signature, replacing only `events[i].data.fd` with
the fd argument. With the change, the host test reports:

```text
sampler dispatch: PASS (PG13/17/18, both fd orders, post-epoll FORK, next tick)
sample tick delay: PASS (unset, cached, opt-in after capture, invalid values)
```

Cost on the dispatch host, one million empty consumes per population:

```text
empty pre-tick consume: population=0 ticks=1000000 ring_calls/tick=1 clock_calls/tick=2 ns/tick=62.60 (host ring shim; production consume+sweep)
empty pre-tick consume: population=500 ticks=1000000 ring_calls/tick=1 clock_calls/tick=2 ns/tick=60.63 (host ring shim; production consume+sweep)
```

This measures the production consume wrapper and sweep with real host clock
reads and a fake empty ring. It excludes libbpf's empty-ring inspection and
is not a Linux daemon CPU measurement. The added steady-tick work is one
consume, one pending-INIT sweep and two monotonic clock reads; the sweep
visits only pending INITs, so an empty pending list does not walk the backend
registry. Queued enrollment work moves before sampling. No wakeup or
per-idle-backend read is added.

Additional focused evidence:

```sh
CFLAGS='-fsanitize=address,undefined -fno-omit-frame-pointer' \
  python3 -B tests/build_sampler_dispatch.py --output /tmp/p866-dispatch-sanitized
/tmp/p866-dispatch-sanitized
python3 -B tests/test_backend_init_order.py --mutants
python3 -B tests/test_sampler_batch_summary.py
python3 -B tests/test_sampled_attr_io.py
scripts/check-test-registration.sh
scripts/check-runner-registration.sh
git diff --check
```

The dispatch sanitizer run, INIT-order mutants, summary tests and both
registration guards passed. The complete attribution I/O invocation exited
zero with:

```text
sampled_attr_io: 10260850/10260850 checks passed
sampled_attr_io mutants: 44/44 built, reached and rejected by named assertions
```

An additional, unrelated
`python3 -B tests/test_sample_weight_controls.py` invocation stopped before
its tests because its prerequisite `pgwt-server` had not been built. No
weight-control result is claimed. `make check` was excluded by the brief.

## Opt-in tick-overrun control

`PGWT_DEBUG_SAMPLE_TICK_DELAY_US=20000` requests one 20 ms sleep after capture
on every sample tick. At the arm's 100 Hz this extends the body beyond the
10 ms nominal period while the next timer is already armed. A FORK arriving
in that tail must be consumed before the next target list. The setting is
parsed once at daemon initialization, announced on stderr, accepts integers
1 through 1000000, and defaults to zero when unset or invalid. A signal may
shorten the single sleep. The host test replaces the sleep and checks its
position, duration, cached setting and disabled path.

The live arm already propagates `PGWT_DEBUG_*` environment variables; its
assertions, classes, tolerances and transport are unchanged. These Linux
commands were run for commit 2cf5cbf6 on 2026-11-16 (Ubuntu 6.8 and EL8 4.18,
PG 13.23): 8 fixed-build arms passed with 0 late, the pre-fix tracer went RED
(200/200 missing) and the EL8 control below went RED (97 one-tick lates) while
its fixed arm passed; evidence in `tests/results/session-20261117/p866-pg13-live8/`
on the lead's Mac (git-ignored). Do not run them on macOS. Use provisioned PG13
boxes and retain all evidence outside the repository. A passing natural arm must have empty `late_attribution_ticks`
and `missing_attribution_pids`, and no timing or other errors.

On the Ubuntu 6.8 box, from the fixed checkout:

```sh
set -eu
uname -r
git rev-parse HEAD
make -j4
for k in 1 2 3; do
  sudo env -u PGWT_DEBUG_SAMPLE_TICK_DELAY_US python3 -B \
    tests/test_sampler_batch_live.py --pg13-far-rows \
    --evidence-dir "/tmp/p866-dispatch-ubuntu-fixed-$k-$(date +%s)"
done
```

On the EL8 4.18 box, from the fixed checkout:

```sh
set -eu
uname -r
git rev-parse HEAD
make -j4
for k in 1 2 3; do
  sudo env -u PGWT_DEBUG_SAMPLE_TICK_DELAY_US python3 -B \
    tests/test_sampler_batch_live.py --pg13-far-rows \
    --evidence-dir "/tmp/p866-dispatch-el8-fixed-$k-$(date +%s)"
done
```

For the EL8 gate-fires control, after the wrapper has committed this change,
build a temporary source archive of that commit. Remove **only** the
pre-sample consume. This restores the starting revision's dispatch order
while retaining exactly the same debug delay and other code. This is an
instrumented unfixed control, not an unmodified ff5511a0 binary (that binary
does not understand the new knob).

```sh
set -eu
fixed_tree=$(pwd -P)
fixed_sha=$(git rev-parse HEAD)
control_tree=$(mktemp -d /tmp/pgwt866-unfixed-control.XXXXXX)
git archive "$fixed_sha" | tar -x -C "$control_tree"
python3 - "$control_tree/src/daemon.c" <<'PY'
from pathlib import Path
import sys
path = Path(sys.argv[1])
source = path.read_text()
site = '        pgwt_consume_lifecycle_ring(d);\n        handle_sample_timer(d);'
assert source.count(site) == 1
path.write_text(source.replace(site, '        handle_sample_timer(d);'))
PY
make -C "$control_tree" -j4 PGWT_BUILD_VERSION="$fixed_sha-unfixed-control"
make -C "$fixed_tree" -j4
sudo env PGWT_DEBUG_SAMPLE_TICK_DELAY_US=20000 python3 -B \
  tests/test_sampler_batch_live.py --pg13-far-rows \
  --master-tracer "$control_tree/pg_wait_tracer" --require-negative-control \
  --evidence-dir "/tmp/p866-dispatch-el8-overrun-pair-$(date +%s)"
```

This invocation first requires the fixed arm to pass with zero extra ticks,
then requires the control to go RED on missing/late first attribution. A
setup/perf failure cannot satisfy that RED. Check by hand that both daemon
stderr files show `PGWT_DEBUG_SAMPLE_TICK_DELAY_US=20000` (the harness does not
assert it); both binary hashes and the
per-PID timing verdicts are saved in their respective `summary.json` files.
The natural runs' pending-negative-control notice is resolved by this
separate paired control. If the control stays green or the fixed arm is
late, qualification remains incomplete; do not change the gate.
