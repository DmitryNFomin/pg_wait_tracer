# Issue #356: warm executions, exact PID lookup

Base: `6bfe7d3`, branch `agent/cx-356-executions`. Measurement host:
`pgwt-gate-2` (AMD EPYC-Rome), trace `/tmp/pgwt_cx_rehearsal_tqWPfm`.
Every comparison uses this host, trace, compiler, optimization flags, and
900-second window. No timing ratio to gate-1 or the historical 4217 ms is
claimed. The historical number predates the already-landed #365 changes.

## Verification of the issue's analysis

- **Row cap / serialization:** held. `handle_executions` returns 100 of
  501,398 matching rows (`truncated=true`), 25,628 wire bytes. Baseline warm
  serialization is 0.301–0.377 ms; it is not the dominant phase.
- **Interior `n_events` is window-independent:** held under the stated strict
  interior bounds and fixed filters. `pgwt_compute_executions` increments only
  the attributable stack top while `interval_overlaps` and
  `pgwt_filter_matches` accept the event. The inclusive raw loader's endpoint
  selection makes clipped edge rows different; changing that rule is unsafe.
  The change here preserves every event and all attribution decisions.
- **No executions phase split:** held. The shipping base has no executions
  phase instrumentation. `instrument.py` adds measurement-only timers to
  `load_execution_rows`, `handle_executions`, and `pgwt_compute_executions`.
- **Linear PID lookup:** held. `exec_pid_state_get` scans all historical PID
  states per event. The baseline sees 147 states and 6,726,385 combined records,
  including 1,654,324 retained prefix markers. The approximate 1 s compute
  prediction does not hold on this workload/build: warm compute is 168–194 ms.
- **Cold-marker confound:** the first call is excluded, not interpreted as a
  prefix cost. `load_execution_rows` flips `retain_exec_marks` before loading,
  and `cov_decode_markers` fills the retained marker index lazily. Warm prefix
  copying is 18–22 ms here, not the issue's historical ~1 ms; neither supports
  assigning 1.8 s to a warm prefix walk. The cold call also loads the raw cache,
  so its excess is not assigned solely to marker decoding.
- **Raw-load / PID-only floor and mandatory counter design:** stale after
  #365 on this measured build. Baseline warm loading is 211–226 ms, not 2.6 s.
  A build-time counter was not introduced. Its changing-worker-link seam
  remains unproven; `backends.jsonl` remains request-time authoritative.
- **#375 / #357:** `bagg_window_eligible` still returns zero unconditionally.
  No aggregate refusal was removed. The valid warm 900 s Transitions canary
  reproduces approximately 379 ms on this box. Nothing here claims #357 fixed.

## Measurements and reproducibility

`measure.py` makes seven executions calls in ONE server process for each arm;
call zero is excluded. It then runs four 900 s Transitions calls, excluding
its first call. It enables `PGWT_CURRENT_TRACE_CACHE_STATS=1`, retains stderr,
and asserts exact response bytes against the baseline on every after call.
Request IDs and all request parameters agree between arms. The runtime
comparison covers all seven complete responses; their SHA-256 digests are
retained in the JSONL records, and the first warm response pair is retained
as a byte-identical wire example.

Baseline warm endpoint median: **591.463 ms**, n=6, range 563.653–597.122 ms.
After warm endpoint median: **444.320 ms**, n=6, range 435.160–493.847 ms.
Warm compute falls from 168–194 ms to 58–66 ms.
Transitions medians: **379.236 → 364.826 ms**, n=3 per arm.
Cache counters: `decoded=1239` and `resets=0` throughout all six warm executions;
`served` increases from 1239 to 7434. Timings are not used to infer cache hits.

`browser.py` measures complete Waterfall mount/paint, including its execution
list and selected execution detail request. It navigates seven times in ONE
upstream server process per arm, excludes navigation zero, and asserts the
900 s window, rendered canvas, no browser errors, and no upstream restart.
It never sends `time_model`; it does not derive paint or a client cost by
subtracting endpoint timing. Both arms use the same Go bridge/browser build. Each final browser process
sends exactly seven `executions` and seven `execution_detail` requests.
Complete warm Waterfall paints: **767 → 671 ms**, n=6 per arm;
ranges **746–815 → 629–697 ms**. These are directly measured complete paints,
already below 3000 ms on the base and improved further by this change.
The measured workload therefore does **not** require #357 to meet its budget.

Two preliminary harness runs yielded no accepted measurement series: one
failed the Python integer-versus-JavaScript-number window assertion; another
timed out before the initial Queries mount and before any raw load reached
the server. The final harness checks input equality and the exact 900-second
width in JavaScript, and captures startup/browser errors without weakening
the mount assertion or its timeout. No retry was added to the harness.
The reported paints use only the final complete single-process series;
the startup timeout's cause remains unestablished.

Run `instrument.py` on disposable copies named `before` and `after`, never
on the production checkout. `run.sh` is the focused Linux build/unit/data/
measurement sequence, under `/tmp/pgwt-box-check.lock`; it does not invoke
`make check` or `make box-check`. The before copy is archived from the base;
the after measurement copy is the final source with the same instrumentation.
The separate `final` copy has no instrumentation and builds/runs the C units.

## Correctness and provenance

`test_execution_pid_index` compares every byte of every execution row with
`executions_linear_oracle.h`, a frozen extraction from `6bfe7d3` with only
identifiers renamed and terminal whitespace trimmed. 451 comparisons cover 700 interleaved leader PIDs,
worker PIDs, legal PID zero, hash collisions, state reallocations, plans,
nested/orphan executions, the unconditional attribution clear on pop,
worker links, samples, zero-duration events, clipped/equal endpoint bounds,
empty input and PID/query/event/class filters. Its nonempty/count/worker/
inferred-end/plan/clipped ledger must be exercised.

The real helper's PID map counters are asserted: 700 inserted states,
700 index entries, growth beyond the initial capacity, and no extra inserts
on reverse-order reuse. Allocation failure at first insertion and at later
growth must refuse without publishing a partial state. Injecting map failure
through `pgwt_compute_executions` must return `failed=1` and no rows: that
assertion proves the full computation actually uses the indexed helper.
The unrelated injection control must still return rows.

`mutations.py` recompiles a control and four deliberately broken product
lookups; all four must fail the test. `asan.txt` records the same differential passing with AddressSanitizer on macOS.
`mutations.txt` records a passing control
and caught linear fallback, truncated PID, wrong slot and ignored allocation
failure. No epsilon, tolerance, retry or sleep was added to any test.

Production functions changed: **`exec_pid_state_get`,
`pgwt_compute_executions`**. `src/server.c` is unchanged. New test functions:
`add`, `compare`, `provenance_and_refusal`, `main`, and the unused-summary
link stub `pgwt_visit_summaries`. The oracle's renamed copies are
`oracle_exec_pid_state_get`, `oracle_exec_pid_push_row`,
`oracle_exec_pid_pop_row`, `oracle_exec_pid_peek_row`,
`oracle_exec_pid_attributable_row`, `oracle_execution_append`,
`oracle_worker_link_index`, `oracle_interval_overlaps`, and
`linear_compute_executions`. The measurement-only C helper is `exec_measure_now`; Python harness helpers
are `ask`, `sent`, and `socket`.

## Completed validation

Linux GCC 13.3 builds succeeded for the instrumented before/after servers,
the shared Go client, and the uninstrumented final daemon/server and all unit
targets. All **47 C unit binaries and 15 listed script checks** passed.
`test_data_executions.py`: **131/131**; `test_data_exec_prefix.py`: **48/48**.
The final source SHA-256 hashes match this worktree; `sources.sha256`,
`binaries.sha256`, `trace.sha256`, and build/result logs retain provenance (trailing whitespace normalized).
`summary.json` records the independently parsed warm medians, exact wire
comparisons, cache counters and browser request counts. Test registration and
`git diff --check` also passed locally. No prohibited check, push or PR ran.
