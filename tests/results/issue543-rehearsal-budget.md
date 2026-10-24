# Issue #543 implementation evidence

Base: `origin/master` / `04797231d678d8b50ddd91c779da200bceed45e7`.
Branch: `agent/543-rehearsal-budget`. No live rehearsal was run here.

The workload timers started before daemon/bridge startup, whereas the walk's
clock started afterwards. Its pass budgets are estimates, not upper bounds;
three selector timeouts can overrun the fixed workload timers. The liveness
guard correctly refuses then, losing the verdict and replacing real findings
with a void.

The shell now owns workload lifetime through its existing EXIT/INT/TERM
cleanup. The lock loop has an explicit `--until-stopped` mode (fixed-duration
smoke callers retain their mode). Pgbench uses `-T 86400`: one day exceeds
the launcher's walk/wait/collect budget for the documented 30-45 minute runs
and bounds orphaned load if the harness is killed. Cleanup sends SIGINT to the
original PID so pgbench prints its statistics. There are no restarts, retries
or modified UI thresholds. Both workloads remain live through end-of-capture
queries too.

An early preflight rejects nonfinite, nonpositive, malformed and too-short
plans before PostgreSQL discovery, load or sysctl changes. A reduced baseline
cannot pass and is refused; the explicitly documented DURATION_MIN=3 mechanics
self-test remains allowed and announces its expected reduced coverage. Direct
Python invocations use the same plan validation before resetting result files.

Rejected alternatives: shrinking selector waits changes the measurements without
diagnosing the hangs;
a remaining-time walk cutoff needs a new incomplete-verdict/abort path and
still sacrifices useful findings. Keeping load alive preserves the existing
walker, timeouts, failed-tab aggregation and liveness guard. This does not
diagnose the separate selector-readiness defect (#543 item 2, possibly #537),
nor impose a new overall browser deadline. The existing outer launcher's
wait/collect protocol is unchanged.

## Demonstrated RED, before implementation

Command (new test against unchanged base production code):

```text
python3 tests/test_rehearsal_workload_lifetime.py WorkloadLifetime.test_three_selector_timeouts_past_old_deadline_produce_red_not_void
AssertionError: delayed walk voided (exit=1):
...
FATAL: pgbench (pid 101) is no longer running (pass 'late' tab 'scatter') -- refusing to grade the remaining tabs against an idle system
Ran 1 test in 0.009s
FAILED (failures=1)
```

Original full output: `/tmp/issue543-red.log` (outside repository).

## GREEN, after implementation

Same delayed walk, unchanged delay and guard:

```text
test_three_selector_timeouts_past_old_deadline_produce_red_not_void ... ok
delayed walk: 2190s > old 2100s; 33 tabs; RED verdict, no liveness void
```

The test executes the actual shell controlled-load block with argv-recording
commands, then uses those arguments to model workload completion. It runs the
production `_run_passes` and `_assert_workload_alive` with a virtual clock and
recorded tab measurements. The first three late-pass tabs each consume an
extra 60s and fail. Assertions require crossing the old deadline, walking all
33 tabs, and retaining exactly those three failures in the verdict. It is
deterministic simulation, not evidence that selectors or live PostgreSQL work.
Separate tests cover either workload dying, the real lock loop continuing
until SIGTERM and cleaning up, fixed-duration compatibility, and preflight
refusal/acceptance. Registered in `scripts/check.sh` and `tests/unit_tests.list`.

## Completed validation of cbc4b2d

The implementation was committed as `cbc4b2d`. The owner's subsequent full
`make check` passed with stamp `fc247be77eaf7ca7d02743b846f61cf8ccd98c04`
(also present in `.pgwt-check.stamp`). Gate-2 box-check returned:

```text
Executed: 149 (passed 149, failed 0)
Live UI smoke: overall=PASS
```

These results apply to that committed implementation, before the second-round
review fixes. Full check is left to the owner for the revised tree; no rehearsal
was started. The owner will validate the real pgbench argv on gate-2 with a
10-second smoke of `pgbench -U postgres -d postgres -c 4 -T 86400 --rate=25`.

## Second-round regression controls

The delayed-walk test now uses `rehearsal_plan(2100)` and repeats the same walk
with `pg_end=1000`, requiring `SystemExit`. Temporarily removing both workload
liveness calls from `_run_passes` (leaving the guard implementation intact)
produced:

```text
(pg_end=1000) ... FAIL
AssertionError: SystemExit not raised
```

The shell preflight test now includes a valid `DURATION_MIN=35` control. Every
case passes `--pg-version 999`, so discovery cannot start a real workload.
Temporarily inserting `sys.exit(1)` on the heredoc's success path produced:

```text
(duration='35') ... FAIL
AssertionError: 'launch plan target=2100s' not found in ''
```

Both mutations were restored. Full RED logs are
`/tmp/issue543-round2-F3-red.log` and `/tmp/issue543-round2-F4-red.log`.
The revised workload-lifetime suite passes all six tests, including tolerance
controls inside and outside the mechanics self-test's `1e-6` second allowance.
The existing rehearsal library suite passes all 287 assertions; shell syntax
and `git diff --check` also pass. No full check or box-check was rerun for this
round. Changes remain uncommitted for the owner; the sandbox protects `.git`.
