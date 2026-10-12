# CPU filter (#345) and first request after shrink (#342)

Base: origin/master, 91864db22b330acc8a564bf5098016985b2b1095.
PR #365 was OPEN at the initial fetch and again before the gate phase.

## Red before green

`red-before.log` records failed assertions before changing runtime logic;
`green-focused.log` records the same fixtures after the fixes. The JSON
response printed by one existing truncation assertion is excerpted to 500
characters; assertion outcomes and totals are retained.

- #345, `python3 tests/test_data_filters.py`: 10/16 passed. Explicit CPU
  returned 21ms instead of 3ms; CPU+PID returned 9ms instead of 2ms;
  CPU+query returned 11ms instead of 1ms. Wide Sessions and Queries both
  returned 21ms instead of 3ms. After: 16/16 passed.
- #345, `test_data_executions.test_lifecycle_and_detail(TestRunner(...))`:
  33/37 passed. Table and scatter both returned all three executions instead
  of the sole CPU execution. After: 37/37 passed.
- #345, `test_agg_raw_crosscheck`: five failed assertions. Both raw and
  summary CPU totals were 11362.296ms instead of the independent fixture's
  4053ms; Sessions summary refusals lacked poisoned counts and failure flags.
  After: 183 checks, zero failures (includes an additional no-record refusal).
- #345, `test_block_agg`: explicit CPU was accepted as aggregate-compatible.
  `test_interval_index`: CPU returned rc=0/have_result=1 instead of refusing
  a filtered request. After: both pass; interval suite 122 checks, zero failures.
- #342, `section_watermark_shrink(TestRunner(...))` and
  `bypass_truncated_file_under_meta(TestRunner(...))` from
  `test_data_current_trace_cache`: 15/21 passed. The first request after a
  watermark-only shrink returned zero rows with caching ON and OFF. The
  block-boundary truncation returned zero instead of a readable smaller
  prefix (7425 events before truncation). After: 21/21 passed. The complete
  cache suite also passes 112/112.

The C red probes used the new presence field in the header and test callers,
but the original runtime implementation ignored it. Python red probes used
master's original server implementation. These focused probes ran locally
on macOS using Homebrew LZ4; build-command portability adapters were
`-Dst_mtim=st_mtimespec`, `-DSOCK_CLOEXEC=0`, and, for spawn.o only,
`-Dpipe2(a,b)=pipe(a)`. No portability change was made to source. The required
full `make box-check` uses the normal Linux build and existing runner lock.

## Presence flag threading

`pgwt_filter.has_event_filter` is independent of `event_id`: a zeroed filter
remains absent; parsing any numeric event ID, including CPU=0, sets presence.
All former presence checks are replaced:

- server.c: parse_filters; handle_top_sessions summary routing;
  handle_top_queries summary routing; execution_matches_request.
- compute.c: pgwt_filter_matches; summary_event_matches_filter;
  pgwt_compute_aas_from_summaries context; tm_summary_visitor's unfiltered
  decision and filtered class accumulation; the Top Sessions summary refusal
  moved from ts_summary_visitor to pgwt_compute_top_sessions_from_summaries;
  tq_summary_visitor's filtered branch, per-event match and class-only fallback.
- block_agg.c: pgwt_block_agg_filter_supported.
- interval_index.c: is_unfiltered (both storing and querying use it).

Execution event filters copy the whole struct, so the flag survives leader
and worker processing. Existing C fixtures assigning nonzero event IDs now
set presence too. Event ID checks for CPU rendering and hash-table occupancy
are event identity checks and retain their original behavior.

Unsupported Sessions summary filters return failed=1, num_rows=-1 and
rows=NULL even when no summaries exist. On supported paths the old arithmetic
and ordering remain unchanged. No retry, sleep, tolerance, epsilon, or
KNOWN_FAILING entry was added. The extra `info` request formerly bypassing
#342 in the truncation fixture is removed.

## PR #369 CI failure repair

Run 38264692891, job 114863778264 built both the tracer/server and C tests
successfully. `gh run view --job --log` refused while the overall run was
still in progress; the completed job's `/actions/jobs/114863778264/logs` API
returned the log. `ci-failure-excerpt.log` preserves the actual failure:
`test_probe_bounds.c:1018` expected an unsupported class-filtered Sessions
summary request to return an empty success (`!failed`, `num_rows=0`).

The test now requires the full explicit refusal contract above, including
`rows=NULL`. This strengthens the refusal assertion and preserves every
test and fixture; no runtime implementation or build registration changed.
The original test reproduced locally with 87 checks and one failure; the
updated test passes all 87. The existing CPU-filter fixture still passes
16/16, including exactly 21ms unfiltered and 3ms with explicit CPU=0.
The execution lifecycle fixture passes 37/37: both table and scatter retain
only the sole CPU execution from the three-execution fixture. Results are
preserved in `ci-repair-focused.log` (C probe built with the same macOS
objects and portability adapters described above).

#342 restores present=1 immediately after the inline cov_reset in
cov_scan_file. Deleted-file resets still clear presence. The first rebuilt
response must equal a fresh uncached prefix bit for bit, excluding request ID.
