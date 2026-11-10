# Issue 757 round 2: shared integer CPU accounting

Base HEAD and origin/master were both `42727667d2ea075b4ad8c8f5c5f3dea16d0756b3`
(`git rev-parse HEAD origin/master`). Production code was unchanged for the
initial red runs. No fetch, add, commit, push, PR, Chromium, tree-hash script,
make check, ui-gallery, box-check, or snapshot generation was performed.

The integer contract rounds each clipped CPU contribution once, clamps it to
its gap wall, and accumulates integer ns. Subtracting the integer CPU total
from the integer gap wall equals summing the per-record remainders because
no contribution exceeds its wall. Overview uses that gap total directly,
avoiding cancellation against unrelated waits. Events and Sessions convert
response totals to ms once. Sessions now exposes an additive `cpu_ms` total
so callers can compare CPU totals without inverting a floating-point percentage.

Events emits `offcpu_aas` using the full requested window, with null for an
unavailable measurement and numeric zero for a measured zero. Normal and
compare tables render the payload value; compare has separate A AAS/B AAS
columns. Class/event-filtered Overview and Events requests use raw records,
matching Sessions: the summary time-model subset cannot supply that split.
These filtered raw scans remain subject to the existing load-limit refusal.

## Red before production edits

The original two inputs have duration 10485760 ns, clipped wall 1048576 ns,
and CPU 10433332 / 10433330 ns. Test expectations use `(cpu + 5) / 10`,
independently of the helper under test, with exact `==` assertions.
Replay additionally tests CPU 10433336: the original pair both round down,
so neither alone can expose truncation.

Initial Mac build attempts:
- `make pgwt-server`: FAIL, missing Darwin `st_mtim` and `SOCK_CLOEXEC` APIs.
- `make -C tests test_window_clip test_replay_window`: FAIL, no `build/wait_event.o`.
- A first new C-test compile caught a missing `wall_ms` test argument; corrected.
- `demos/build-server-local.sh`: PASS, `==> pgwt-server ready ...`; uses the
  repository's existing Darwin compatibility header.
- The first replay build raced the server object's creation and reported
  missing `server_wait_event.o`; the subsequent build after server completion passed.

Commands that produced the behavioral red evidence:

```sh
clang -O2 -DPGWT_SERVER -Isrc -include demos/replay-compat.h \
  -o /tmp/757-window tests/test_window_clip.c src/compute.c src/wait_event.c \
  src/idle_rule.c src/spawn.c src/cJSON.c -lm
/tmp/757-window
make -C tests test_replay_window gen_test_traces CFLAGS='-O2 -Wall -Werror=implicit-function-declaration -I../src -I../include -I/opt/homebrew/include -include ../demos/replay-compat.h -L/opt/homebrew/lib'
tests/test_replay_window
python3 tests/test_data_time_model.py
node --test tests/web_unit/events-offcpu-compare.test.mjs
```

C window red (exit 1):
```
  FAIL: 757 time_model exact integer off-CPU (tests/test_window_clip.c:2876)
  FAIL: 757 exact off-CPU parity across all three handlers (tests/test_window_clip.c:2878)
  FAIL: 757 exact integer CPU totals (tests/test_window_clip.c:2885)
338 checks, 3 failures
```

Replay red (exit 1):
```
  FAIL: 757 replay exact rounded CPU
  FAIL: 757 replay exact integer off-CPU
  FAIL: 757 replay event CPU equals time model CPU
235 checks, 3 failures
```

Server red (exit 1):
```
  FAIL: 757 summary emitted Events offcpu_aas is available
  FAIL: 757 summary emitted Events AAS == Overview waiting AAS exactly
  FAIL: 757 raw emitted Events offcpu_aas is available
  FAIL: 757 raw emitted Events AAS == Overview waiting AAS exactly
  FAIL: 757 raw straddle emitted AAS exact equality
  FAIL: 757 unmeasured Events AAS emits null, not zero
13/19 tests passed
```
Web red (exit 1):
```
null !== 0.23
compare columns: actual undefined, expected '0.23'
null !== 0
3 failures
```

Additional filtered-route red after arithmetic fix, before route fix:
`python3 tests/test_data_time_model.py` exited 1 and printed each of these
for both the CPU class and event 0:
```
  FAIL: 757 CPU class/event filter exact CPU parity across handlers
  FAIL: 757 CPU class/event filter exact off-CPU parity across handlers
```

## Negative controls (all detected, then restored)

Each mutation was temporary, rebuilt before execution, and restored in a
`finally` block. `/tmp/757-negative.py` executed the following controls:

1. Shared CPU helper: replace `(uint64_t)(measured + 0.5)` with
   `(uint64_t)measured`. Rebuild and run `test_window_clip` and
   `(cd tests && ./test_replay_fidelity)`: both exit 1.
```
  FAIL: 757 raw/summary exact CPU aggregation across sessions (test_window_clip.c:2940)
  FAIL: 757 raw/summary exact off-CPU aggregation across sessions (test_window_clip.c:2943)
  FAIL: 757 replay exact rounded CPU
  FAIL: 757 replay exact integer off-CPU
```
2. Shared residual: replace `gap_ns - cpu_ns` with `gap_ns - cpu_ns + 1`.
   Rebuild and run `tests/test_window_clip`: exit 1 (22 failures).
```
  FAIL: 757 time_model exact integer off-CPU (test_window_clip.c:2876)
  FAIL: 757 exact session off-CPU total (test_window_clip.c:2889)
  FAIL: 757 time_model exact integer off-CPU (test_window_clip.c:2876)
  FAIL: 757 exact session off-CPU total (test_window_clip.c:2889)
  FAIL: 757 raw/summary exact off-CPU aggregation across sessions (test_window_clip.c:2943)
  FAIL: 757 raw/summary exact CPU aggregation across sessions (test_window_clip.c:2940)
  FAIL: 757 raw/summary exact off-CPU aggregation across sessions (test_window_clip.c:2943)
```
3. Wire denominator: replace Events' `res.offcpu_ms / wall_ms` with
   `res.offcpu_ms / res.db_time_ms`. Run `demos/build-server-local.sh`, then
   `python3 tests/test_data_time_model.py`: build passes, test exits 1.
```
  FAIL: 757 summary emitted Events AAS == Overview waiting AAS exactly
  FAIL: 757 raw emitted Events AAS == Overview waiting AAS exactly
```
4. UI payload: set synthetic `aas` back to null. Run
   `node --test tests/web_unit/events-offcpu-compare.test.mjs`: exit 1.
   Failing assertion: `null !== 0.23` (normal/fallback rendering), and the
   compare-column and measured-zero assertions also fail.
5. UI compare: render `_a.aas` in B's AAS column. Same Node command: exit 1.
   Failing assertion: `'0.23' !== '0.13'` in compare payload AAS rendering.

The filter-route red above separately detects omission of the raw-route
fallback. Final positive runs below used restored production code.

## Final commands and results

Mac object setup (server objects were compiled by the local build wrapper):
```
cp build/server_wait_event.o build/wait_event.o
cp build/server_idle_rule.o build/idle_rule.o
cp build/server_spawn.o build/spawn.o
cp build/server_cJSON.o build/cJSON.o
```

Final server build: `demos/build-server-local.sh`: PASS, last lines
`DONE pgwt-server` / `==> pgwt-server ready ...`.

Final C build command (PASS):
```sh
make -C tests test_window_clip test_time_model_absence test_cpu_split_parity \
  test_agg_raw_crosscheck test_straddler_accounting test_open_cpu_accounting \
  test_idle_accounting test_idle_summary test_replay_fidelity test_replay_window \
  test_io_worker_parity test_io_worker_summary test_summary_qattr_background \
  CFLAGS='-O2 -Wall -Werror=implicit-function-declaration -I../src -I../include -I/opt/homebrew/include -include ../demos/replay-compat.h -L/opt/homebrew/lib'
```
Each command below ran with `tests/` as its working directory and exited 0:

| Command | Last result | Verdict |
| --- | --- | --- |
| `./test_window_clip` | 342 checks, 0 failures | PASS |
| `./test_time_model_absence` | PASS: test_time_model_absence (0 failures) | PASS |
| `./test_cpu_split_parity` | 142 checks, 0 failed | PASS |
| `./test_agg_raw_crosscheck` | 270 checks, 0 failed | PASS |
| `./test_straddler_accounting` | === test_straddler_accounting: PASS === | PASS |
| `./test_open_cpu_accounting` | === test_open_cpu_accounting: PASS === | PASS |
| `./test_idle_accounting` | 170 checks, 0 failed | PASS |
| `./test_idle_summary` | 425 checks, 0 failed | PASS |
| `./test_replay_fidelity` | ALL PASSED | PASS |
| `./test_replay_window` | ALL PASSED | PASS |
| `./test_io_worker_parity` | 58 checks, 0 failed | PASS |
| `./test_io_worker_summary` | 63 checks, 0 failed | PASS |
| `./test_summary_qattr_background` | PASS: 0 failure(s) | PASS |

The first broad replay-fidelity invocation from the repo root failed its
existing relative-header lookup assertions; invoking it from `tests/`, as
its runner does, passed. This was a working-directory correction, not a
retry for runner noise. An intermediate web run failed the ratio-column
position assertion after AAS columns were added; the assertion now selects
by column key. An added B-only *unavailable-baseline* fixture was invalid
(the primary A request must exist); it was removed. Successful B-only compare
is covered, as are A-only, both, unavailable baseline, measured zero and null.

Server commands (all exit 0):

| Command | Last result | Verdict |
| --- | --- | --- |
| `python3 tests/test_data_time_model.py` | 29/29 tests passed | PASS |
| `python3 tests/test_data_events.py` | 85/85 tests passed | PASS |
| `python3 tests/test_data_sessions.py` | 4/4 tests passed | PASS |
| `python3 tests/test_data_offcpu_identity.py` | 6/6 tests passed | PASS |
| `python3 tests/test_data_filters.py` | 16/16 tests passed | PASS |
| `python3 tests/test_data_window_bound.py` | 192/192 tests passed | PASS |
| `python3 tests/test_data_summary_honesty.py` | 87/87 tests passed | PASS |

`node --test 'tests/web_unit/*.test.mjs'`: PASS, 900 tests, 900 pass,
0 fail, 0 skipped.

`scripts/check-test-registration.sh`: PASS, append-only invariant holds.
`scripts/check-runner-registration.sh`: PASS, every candidate test registered.
No new C test executable was added: all C regressions extend existing
registered executables, so unit_tests.list and Makefile's TESTS are unchanged.
`git diff --check`: PASS (no output).

#410 expectations only:
```sh
PYTHONPATH=tests python3 -c 'import test_summary_raw_parity as t; t.RESULT_DIR="/tmp/757-parity-final"; t.main()'
```
PASS, last line `=== test_summary_raw_parity: PASS ===`.
`tests/summary_raw_parity_expected.json` is unchanged. Its declared #410 entries
(peak AAS and io_worker availability across the existing windows) did not
change. This tolerant parity gate is **not evidence for the sub-ns fix**;
the exact C, replay, JSON and UI assertions above are that evidence.

## Gate-agent work / unverified

Run full `make check`, `make ui-gallery` (review the measured-zero synthetic
row and added A/B AAS compare columns), and `make box-check` / Linux
`tests/run_all.sh --require-live`, plus the usual independent review.
These were not run here, per the task's sandbox contract. No PNG changed.
No known arithmetic counterexample remains in the exercised contract; live
capture, Chromium rendering, and the full Linux suite are UNVERIFIED here.

Repository report-audit attempt:
`scripts/lead/audit-report.sh /tmp/757-final-report.md` exited 2.
Prepass passed; the independent auditor did not initialize:
```
Error: failed to initialize in-process app-server client: Operation not permitted (os error 1)
AUDIT INCONCLUSIVE (auditor exit 1) -- do not send.
```
No independent report-audit verdict is claimed, and no retry was made.
The task's required final report records this limitation.
