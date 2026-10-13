# Diagnosis of the reported `test_probe_bounds.c:1018` failure

## Contract resolution on the CPU-filter branch

This branch incorporates diagnosis commit `12463d4` without duplicating its
reproductions. The report below is retained as historical evidence. Its Linux
matrix establishes that master passes and that the branch's refusal-contract
change caused the failure. Its claim that the changed contract was correct
is superseded by this source-based decision: retain **empty, not failed**.

Issue [#318](https://github.com/DmitryNFomin/pg_wait_tracer/issues/318) concerns
the same unsupported class/event filter on the Sessions summary path, not
merely an empty input stream. It requires honoring the filter or routing to
raw. The landed fix (`8705412`, #326) establishes raw routing and a no-rows
fallback; its `ts_summary_visitor` comment explicitly calls visibly empty
results the house rule and cites `aas_summary_visitor`. That precedent also
faces an unsupported summary filter: per-PID class totals are unavailable,
so a PID filter skips every record and produces zero-filled buckets without
a failure signal. The later #358 test pins the Sessions fallback with
`!rsf.failed && rsf.num_rows == 0` on a populated summary fixture.

An unsupported filter is inability to answer, not evidence of zero activity.
Nevertheless, this API deliberately represents that fallback as empty rows.
Changing it to an explicit failure would be a contract change covering the
exact case #318 already addressed, rather than a correction demanded by #345.
`handle_top_sessions` is the sole production caller and forces class, query,
and event filters to raw; after #345 it tests event-filter presence, including
CPU=0. Unfiltered summaries remain reachable, but their unsupported-filter
fallback cannot be triggered through the current viewer. Changing that
fallback buys no current viewer behavior and breaks a pinned invariant.

Commits `5c22cb1` and `d5aaef5` therefore remain: the former reverts the
`fd9c980` assertion change and the latter restores the runtime fallback.
`tests/test_probe_bounds.c` is byte-identical to base `b907ec0`. Unsupported
summary filters return `failed=0, num_rows=0, rows=NULL`; the CPU presence flag,
raw routing, 21ms-to-3ms fixture, and sole-CPU-execution assertions remain.

## Original diagnosis (before the contract decision)

The failure is on an **unmerged branch**, not in master. No product or test
change belongs on master for this incident.

Line 1018 at master's `b907ec023bd5a2a51525144ec6f15fbf0e0a8cac` checks
`!rsf.failed && rsf.num_rows == 0`. The fixture supplies one summary session
with 5 ms of LWLock time, then asks for class `Lock`. Per-session summaries
have no per-event breakdown, so this path cannot answer a class filter.
Master refuses by skipping the records and returning a successful empty
result. The assertion guards against returning unfiltered session numbers
under a filtered label (#318).

The actual failing log is
`wt-345/tests/results/box-check-ephemeral-el9-20261012-165051.log` in the
sibling worktree. Its header identifies commit
`d231d189da237d0b51e2e13301a9524e315b678a`, tree hash
`a35dbc94e588fba5266114d10422472470ac1bc1`, branch
`agent/cx-345-cpufilter`, and Rocky Linux VM 169914184 (49.13.209.14).
It reports 87 checks and exactly one failure at line 1018.

That branch changes the summary Sessions entry point to reject unsupported
class, event, and query filters before visiting any records. The new result
is `failed=1`, `num_rows=-1`, `rows=NULL`, even when no summary records exist.
This intentionally distinguishes an unsupported answer from a measured zero.
The old assertion therefore fails on correct new behavior.

The branch already contains repair commit
`fd9c980335fd2e29ed1ff7ba39e4a36b27f812ed`, which requires all three fields
of that explicit refusal. This strengthens the refusal check; it preserves
the six cap+1 fixtures, growth checks, exact totals, and allocation-failure
checks. Its newer Ubuntu box log already reports 87 checks and zero failures.
At diagnosis time the remote branch still pointed to `d231d18`; the repair
was local to the sibling worktree and its gates were still running.

## Reproduction

`linux-matrix.log` and each case's `result.log` are fresh reproductions on
the same Rocky VM as the original failure, using GCC 11.5.0. Each case was
staged in a separate directory under `/tmp/cx-probe-bounds-diagnosis`, without
changing the original checkout or using either CI runner. The test was run
under a 30-second timeout. The build and run commands in each directory were:

```sh
cc -g -O2 -Wall -Isrc -DPGWT_SERVER -o test-probe \
  tests/test_probe_bounds.c src/compute.c src/wait_event.c \
  src/idle_rule.c src/cJSON.c src/spawn.c -lm
timeout 30 ./test-probe
```

| Case | Commit | Exit | Checks | Failures |
| --- | --- | --- | --- | --- |
| Current master | `b907ec0` | 0 | 87 | 0 |
| #358 merge point | `2fbf8fb` | 0 | 87 | 0 |
| CPU-filter branch before repair | `d231d18` | 1 | 87 | 1 |
| CPU-filter branch after repair | `fd9c980` | 0 | 87 | 0 |

All four results also reproduced on macOS with Clang and the existing
`tests/results/probe-bounds/spawn_stub.c` link adapter. Current master passed
an additional AddressSanitizer/UndefinedBehaviorSanitizer run.

The four master merges after #358 are #338 (`638b77c`), #350 (`7571789`),
#354 (`91864db`), and #336 (`b907ec0`). `git diff 2fbf8fb b907ec0 -- src`
is empty. The test source, its linked implementation, and its dependency
headers are also unchanged. Both merge-point and current-master tests pass;
there is no failing master interval to bisect.

Classification: **test coupled to changed-but-correct behavior on the
unmerged CPU-filter branch**. The supplied claim that this blocks every
branch through merged code is contradicted by the source provenance and
fresh reproductions. Applying the new refusal assertion to master without
its associated product change would introduce the failure there.

## Gate infrastructure

The requested `make box-check EPHEMERAL=1 KEEP=1` was attempted. Creation
failed with `shared core limit exceeded` at fsn1, nbg1, and hel1. A subsequent
account inventory listed five cx33 servers: 166207930, 167553077, 169914184,
169933004, and 169933684. No VM was created or deleted by this attempt.
The last two VMs belong to other active branch runs; cleanup remains with
the owner. This resource failure is separate from the test diagnosis.
