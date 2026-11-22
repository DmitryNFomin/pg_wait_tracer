# Review: agent/947-coverage-server, 4059fcde..c99a35ce (2 commits: 4caaa54f, c99a35ce)

**Verdict: NOT-READY, 1 blocker (B1) and 2 should-fix items.**

## What changed
- Every pgwt-server view response now carries one coverage verdict, `fidelity` + `coverage{verdict, reasons[], covered_to, uncovered[], restart_cause, excludes_in_flight, idle}`. It comes from one trace-model builder (src/coverage_trace.c) that calls the merged pure verdict. The verdict code itself is unchanged.
- #947: a window that spans a daemon restart is no longer labelled `exact`. The 10 whole-window views return `{code:"coverage_unverified", fidelity:"incomplete", coverage}`. AAS and heatmap buckets that meet uncovered time are sent as null. Record-level views keep their rows and get the label.
- Default window: the latest boot, ending at its sealed W or floor. An empty clip reads unverified. Explicit endpoints are not moved. A live_edge window computes over [from, covered_to).
- The replay CLI and `--dump` print the same verdict and withhold totals. Daemon `status` gains `coverage{boot, boot_start_wall, watermark_wall, paused, pause_start_wall, reason}`.
- The pilot route is outside the label (Amendment A4). The fixture generators write v6 WATERMARK/SEAL and accept `--legacy-v4`. C7 tests flipped. A new box test SIGKILLs the daemon under pgbench.

## Evidence
- Check stamp: `.pgwt-check.stamp` is absent in the worktree. `scripts/tree-hash.sh` = 60d2e6777e7fc4a40b6cc0df8604fb2dd8216aea. The lead is running make check; I did not run it.
- Box-check: tests/results/box-check-ephemeral-ubuntu-20261120-092738.log, still running at 08:47Z. Header line 155: `source commit=c99a35ce… tree-hash=60d2e677…`, which matches the tip and the tree. No run_all summary yet.
- Premise reproduced on the base: the branch's own `test_coverage_consumers.py --case restart` against a 4059fcde server fails on the first route. `aas` over [50,150) of a two-boot directory returns `fidelity:"exact"`, and `time_model` returns `exact` with db_time_ms 2000. On c99a35ce all 13 cases pass, including restart, pilot (A4), gap, retention, missing, legacy, idle, default/empty clip, live, shape and replay.
- Mutants (`coverage_consumers_mutants.py`, run on c99a35ce in an exported tree): 8 built, 8 red on their named cases. None was vacuous; both pilot mutants were checked as reaching the policy line.
- test_coverage_status: compiled and passed on the Mac.
- UI gallery: not applicable, since web/ is unchanged.

## Findings

1. **Blocker: live_edge windows send null whole-window totals, and the merged web turns them into wrong numbers.** File: src/server.c:5401, :5428, plus the null cells built at :5410-5427 (via :5253).
   - What the code does: `add_coverage` nulls `max_aas`, and for heatmap `max_count` and `total_events`, when the verdict is live_edge. It also adds 16 `[i, l, null]` cells for each bucket past covered_to.
   - What the contract says: Amendment A3 item 1 defines a live_edge window's whole-window numbers as those of [from, covered_to). The consumer contract, tests/mock_server.py:842/:865, nulls these totals only when the verdict is incomplete, and drops cells rather than injecting null cells. web/static/lib/builders/fidelity.js:168 gives a live_edge window no `uncovered` list.
   - Reproduced on the Mac. Input: a live boot fixture made with the test's own kernel-id seam: 4 backends, AAS about 4, W one second behind `to`, request [now−60 s, now) with 60 buckets. The real c99a35ce server output was fed to the merged web builders.
     - AAS: `aasScaleMax` = 0, `aasPeakText` = "peak 0.0 AAS (up to HH:MM:SS)", y-axis max 1. Expected: about 4.0.
     - Heatmap: `heatmapScaleMax` = 1 while the largest real cell is 4, so every cell saturates the colour scale. 16 injected cells are drawn as unshaded zero-count cells (`[59,0,0,null]`, uncoveredCount 0). Hovering one throws `Cannot read properties of null (reading 'toLocaleString')` (histogram.js:201).
   - Why this is reachable: in Live mode `to` = server now and W lags about 1-2 s, so live_edge is the normal live state. This is the Live Overview peak, the main demo path.
   - Fix: keep `max_aas`, `max_count` and `total_events` for live_edge, computed over the buckets inside [from, covered_to). Drop live-edge cells as the mock does, or let the web understand them; the server must match the merged mock.
   - The `shape` test compares only the `coverage` keys, so nothing catches this. A test over aas and heatmap response shape on a live_edge fixture is needed.

2. **Should-fix: an empty default clip withholds `info` itself, which drops the handshake fields.** File: src/server.c:5353.
   - What the code does: `empty_default` withholds every route, including `info`. tests/test_coverage_consumers.py:381 asserts this.
   - Reproduced: a boot with no watermark yet returns info keys `[code, coverage, fidelity, id]` only. `now_ns`, `from_ns`/`to_ns`, `protocol`, `server_version` and `num_cpus` are all missing.
   - What the spec says: §3 lists info under "Other" (label plus boot/gap/hole lists), not under Withhold. The brief requires only that the empty clip read as unverified.
   - When it happens: during a daemon's first 1-2 s before its first watermark. It also persists for any directory whose latest boot ended before writing a watermark beyond its start, for example a start that exits in its first tick.
   - Consequence, read from the web source and not run in a browser: `TimeRange.initDefault` uses `nowNs || toNs` = 0, so the initial window is [−15 min, 0). The version handshake and the CPU count are lost.
   - Fix: label info, do not withhold it.

3. **Should-fix: every coverage_refresh rereads the whole trace directory.** File: src/server.c:3246, src/coverage_trace.c:130-180, :219.
   - What the code does: `pgwt_cov_trace_read` reopens every file and reads every block header and every COVERAGE payload. It also makes one control-socket round trip. It does this on every refresh, and a request triggers 2-3 refreshes (dispatch, handler, load). The base scan was incremental.
   - Measured on the Mac (-O1 build, 20 mixed requests: info/aas/time_model/top_events/heatmap over one 15-min window):

     | hourly files, 3,600 WATERMARKs each | base 4059fcde | c99a35ce |
     |---|---:|---:|
     | 1 | 1 ms | 8 ms |
     | 8 | 4 ms | 40 ms |
     | 24 (the default 24 h retention) | 11 ms | 119 ms |

     The cost grows linearly with retained watermarks.
   - Contract: Amendment A2 item 2 says U2b inherits the 50 ms bound for a whole-day window over 86,400 watermarks.
   - Fix: cache the model per file identity, as `cov_scan_file` does, and reread only the growing current.trace.

Checked and found correct (no finding):
- Pilot bypass and the leak control.
- The 10 withhold routes on the raw route and on a forced summary route. Summaries are disabled in the product.
- Generation/offset mapping moved into the builder and shared with replay; replay now uses canonical offsets.
- Idle nulling of the parent, its children, `idle_time_ms` and top_events rows.
- Gap buckets null on both AAS paths.
- Explicit endpoints untouched; invalid_window only for an explicit empty window.
- Status serializer.
- Live box test: it asserts what it claims and is registered in LIVE_TESTS (tests/run_all.sh:627).
- C7 flips (mixed_v3, sample_weights_v3) are real withheld-form assertions, not loosened checks.
- Changed C fixtures now declare coverage through the tested window.
- server_harness drops the ±30 s padding: a padded window would now be correctly withheld.
- No web/, contracts/ or src/coverage.{c,h} changes.

## Design questions for the owner
None.
