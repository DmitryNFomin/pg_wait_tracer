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
smoke callers retain their mode). Pgbench has no unlimited-time switch, so it
uses `-t 2147483647` per client instead of `-T`: four clients at 25 total tx/s
would need approximately 10.9 years to finish naturally. Cleanup stops the
original PID. There are no restarts, retries or modified UI thresholds. Both
workloads remain live through end-of-capture queries too.

An early preflight rejects nonfinite, nonpositive, malformed and too-short
plans before PostgreSQL discovery, load or sysctl changes. A reduced baseline
cannot pass and is refused; the explicitly documented DURATION_MIN=3 mechanics
self-test remains allowed and announces its expected reduced coverage. Direct
Python invocations use the same plan validation before resetting result files.

Rejected alternatives: fixed headroom merely moves the arbitrary cutoff;
shrinking selector waits changes the measurements without diagnosing the hangs;
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

Full local gate command: `make check > /tmp/issue543-make-check.log 2>&1`.
The final report provides its tree stamp; `.pgwt-check.stamp` must equal
`scripts/tree-hash.sh` and `git rev-parse HEAD^{tree}` after commit.

`make box-check` was deliberately not run per the owner's sandbox instruction.
There is no box-check summary tail yet: owner must run it on gate-2 against
the committed branch. In particular that gate must verify actual workload
teardown and the live harness; simulation cannot establish those.

## Sandbox commit blocker

Normal `git add ... && git commit ...` was attempted in the supplied clone.
It failed before staging, with exit 128:

```text
fatal: Unable to create '.../scratchpad/fix543/.git/index.lock': Operation not permitted
```

No alternate Git directory/index or worktree was used. Changes remain
uncommitted in this working tree; gate-2 cannot check out the implementation
until the sandbox write restriction is resolved and the changes are committed.
Process inspection with `ps` was also denied. The check reported that it could
not activate `core.hooksPath` for this clone.

## Full check result in this sandbox

`make check` completed with exit 2 / `CHECK FAILED`; **no check stamp exists**.
No retry or sandbox workaround was attempted. Its failures were:

- `test_gitignore_build_products.sh`: tree-hash produced nothing (the guard
  suppresses the underlying Git stderr; see the direct Git denial above).
- Go vet/test: `open /Users/dmitryfomin/Library/Caches/go-build/...: operation not permitted`.
- `test_served_asset_guard.sh`: process classification failed because `ps`
  exited 126, including the forgotten-server scan/teardown assertions.
- All five browser suites (`test_web_ui.py`, `test_web_ui_chaos.py`,
  `test_chip_label_alignment.py`, `verify_waterfall_fold.py`,
  `test_dfg_geometry.py`) failed to launch Chromium: `bootstrap_check_in
  org.chromium.Chromium.MachPortRendezvousServer...: Permission denied (1100)`.

The new six-test suite passed within that full invocation, as did the existing
287-assertion rehearsal library suite. Standalone GREEN output is in
`/tmp/issue543-green.log`; the complete full-check log is in
`/tmp/issue543-make-check.log`.

Manual reviewer guards: `scripts/check-test-registration.sh` passed.
`scripts/check-runner-registration.sh` reported exactly the baseline gaps
documented in CLAUDE.md/#433 (CI-only capture/uprobe smoke tests and four
existing orphans); the new test was not among its findings.
