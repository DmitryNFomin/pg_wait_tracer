# Review: agent/947-coverage-server, rounds 7+8 (1d9a90ff..137aa637)

**Verdict: NOT-READY. There are 2 blockers, both in the #871 known-failing test scoping. The product changes in d3a86d42 are correct.**

## What changed
- Server coverage cache: it is now also keyed on the `.meta` committed-block count. A publication that changes only the contents of a same-size `.meta` within one timestamp tick is no longer served from the stale model.
- `executions`, `exec_scatter` and `span_detail` now turn on marker retention before the frozen coverage snapshot. Their first request uses the prefix index instead of the legacy path.
- A default (empty) window opened before the first watermark is clamped to [S,S]. It no longer runs backwards (to < from).
- Seven EL8-4.18-only KNOWN_FAILING entries (`name|871|el8@4.18|exit=94`). Exit 94 requires that the server withholds the window for `capture_loss` AND that this daemon run's own `ringbuf_drops_total` is > 0 (tests/coverage_loss.py).
- The terminal-flush CPU check (capture smoke) now reads the trace through a raw reader (tests/capture_trace_probe.c). It also asserts that the server withholds the uncertified tail after W_final.

## Evidence
- **Check stamp:** there is no `.pgwt-check.stamp` in wt-u2b8. `scripts/tree-hash.sh` = f692589189ee8426e7512d7299d0a7c632d03bbb. Mac `make check` was not run (the brief excluded it; this review is read-only).
- **Box-check:** there is no `tests/results/box-check-*.log`. The evidence is the implementer's manual run_all on two VMs: /private/tmp/u2b-r8-evidence/{el8,ubuntu}/box-final.log.
  - I verified each file is byte-identical to remote-logs/u2b-r8-box-frozen.log, the final tier.
  - I recomputed the source manifest hash on 137aa637 myself: 19fa4963… It equals the SOURCE_START and SOURCE_END printed on both VMs.
  - EL8: `Executed: 239 (passed 228, failed 1, refused 4, known-failing 6 (1 xpass))`. "Live UI smoke: NOT RUN in this invocation". Status FAIL.
  - Ubuntu: `Executed: 240 (passed 238, failed 1, refused 0, known-failing 1 (1 xpass))`. "Live UI smoke: overall=PASS". Status FAIL.
  - The one failure on both tiers is test_capture_smoke_tiered: "CPU straddle/tiered: every requested CPU bucket is numeric", coverage incomplete[not_captured] over about 70 ms. That item is being fixed in another round, so it is not graded here. **The branch has no green box tier.**
- **Product regressions** (both logs): test_coverage_consumers.py passes every case: restart, pilot, live, default_clip, empty_clip (new startup row), live_span, the cache sequence, refresh_once and timing.
  - Timing: max 21.78 ms (< 50 ms).
  - coverage_consumers_mutants: "20 built, 20 red on named cases". This includes the 2 new mutants, publication_cache_uses_stat_only and empty_startup_bounds_reversed.

## Answers to the four questions
1. **Product logic (d3a86d42): correct.**
   - Cache: coverage_trace.c:283-310. It reads `committed` before the reuse decision. When scan_growth or scan_file reads a newer count, the next refresh sees a mismatch and re-enters scan_growth, which is a no-op when blocks_scanned == committed. So a race costs at most one extra growth pass and never serves a stale model.
   - Retention: server.c:9562-9566. It matches the handlers' own env gate (:7219, :7683).
   - Clamp: coverage_trace.c:432-439 touches only the default `to`.
   - The live totals, info-on-empty-clip, 50 ms, pilot-bypass and live_span cases all still pass, with their mutants red.
2. **Scoped exit 94.**
   - Exit 94 cannot be reached through a dead daemon, an empty trace or a crash:
     - every call site checks rc == 0 before classifying;
     - capture_metrics returns {} on failure, and {} never qualifies;
     - not_captured, restart and missing_file never qualify;
     - a crash or uncaught exception exits 1;
     - exit_code(failed) returns 1 whenever any check failed.
   - Off EL8 4.18, el8_418() is False, so these tests must pass. test_run_all_known_failing.py proves that Ubuntu exit 94 is red and that EL8 exit 1 is red, for all 7 names.
   - The forced controls are real, but they only force failures that run before the classification. None of them covers the assertions that exit 94 skips, which is where blockers 1 and 2 live.
   - "Server withholds everything with capture_loss while drops > 0" is swallowed only on EL8. The C unit tier and Ubuntu still catch it.
3. **Test changes, legitimate or weakening:**
   - Legitimate:
     - test_capture_smoke terminal CPU: the raw reader keeps the >500 ms floor (test_capture_smoke.py:2630 and the negative arm). The tail check at :2564-2571 is real: it requires coverage_unverified with no rows and no cpu_ms over [W_final, tail end].
     - The CPU-straddle and sampled-AAS readiness changes. They are also stricter, because they now require numeric buckets and a complete verdict.
     - test_daemon_server.py:138-189: the pgbench ordering is fixed, and the rc and CLI DB Time checks are kept ahead of the exit-94 classification.
     - test_execution_cardinality.py:435-470: the site, mask and rc checks now run before the classification, and the queries are scoped to the cohort window.
     - test_coverage_restart_live.py:70-90: the restart, nonce and boot2 clean-stop checks are kept. It was xpass in the final run.
     - test_query_event readiness: the root cause is a startup race. The blind sleep(2) reproduced 2 rows with 0 drops; post-attach readiness gives 3 rows. The exit-94 classification added to it is a weakening (blocker 2).
   - Weakening: test_query_event.py:426 and test_capture_smoke.py:864 (blocker 2), and ui_live_smoke.sh:447 (blocker 1).
4. **test_web_ui AAS 0.01 is right.** The live request resolves to [now−900 s, now], because the mock's boot started 3600 s earlier (mock_server.py:1147-1151). wall_ms = 900000 (:1250), and AAS = 12760/900000 = 0.01418, which fmtAas shows as "0.01".
   - The independent oracle is AAS = DB Time / wall.
   - The old "3.54" did not follow from its own fixture: 12760 ms / 3600000 ms is 0.0035, not 3.54.
   - Wall = "900.0s" is asserted exactly, and the denominator mutant (live_denominator_uses_requested_span) stays red in the C server. No denominator bug is hidden.

## Findings
1. **Blocker: tests/ui_live_smoke.sh:446-449 together with tests/ui_live_smoke.py:815-831 and :921-928. On EL8, exit 94 from the new default-Live gate skips the whole stability walk and the injected-blink self-test.**
   - Input: an EL8 4.18 box tier where the default view hits capture_loss and drops > 0. That happens on every run: the final run had drops=51.
   - Expected: a blink, page error, leak, missing tab or frame failure in the walk still exits 1 (brief r8 item 1; testing-and-push.md:50).
   - Actual: `exit "$DEFAULT_LIVE_RC"` (94) returns before the walk starts. run_all counts this as known-failing #871. The EL8 log shows "Live UI smoke: NOT RUN in this invocation". Page errors are only checked up to the first numeric timeout.
   - The walk runs on EL8 in master (origin/master has no default-live gate), so this branch removes it there.
   - A fix must still run the walk and the self-test after a 94. Only the whole-window numeric "rendered" assertions may be classified, under the same signature.
2. **Blocker: tests/test_query_event.py:426 (entry at tests/run_all.sh:193), and the same pattern at tests/test_capture_smoke.py:864. Exit 94 is decided before assertions whose inputs are not withheld.**
   - Mode B grades the CLI's own output, not server totals.
   - Input: the final EL8 run had parsed_rows=3 and drops=1. remote-logs/u2b-r8-query-loaded-proof.log had drops=12090 and "Mode B parsed 3 entries".
   - Expected: the Mode B checks run and pass (exit 0).
   - Actual: the test exits 94 and is counted as known-failing.
   - So any EL8 regression to fewer than 3 (or 0) Mode B rows with at least one drop in the window is reported as #871. The implementer marked the cause of the historical post-ready 2-row failure as UNVERIFIED, so brief r8 item 3 (treat it as #871 only if it is proven to be #871) is not met.
   - :864 skips assert_wait_events on the CLI live view in the same way.
   - A fix must remove the Mode B entry and its classification. In the live phase, classify only an assertion that actually failed.
3. **Should-fix: tests/test_capture_smoke.py:1651-1660, trace phase. The fallback is inverted.**
   - The default-window `resp` is classified first. Only when it lacks capture_loss is the controlled window [required_from, required_to] queried.
   - Input: final EL8, trace/full. The test itself logged "drops=14->14" across the controlled interval.
   - Expected: the per-PID exact sleeper and waiter checks (:1668-1717) run. The comment at :1670 says global drops must not excuse them.
   - Actual: they are skipped with exit 94. Read from source and log; not reproduced whether the controlled window alone was complete.
4. **Nit: tests/ui_live_smoke.py:859.** It uses `loss_signature`, which is not gated to EL8. So on Ubuntu, a withheld first restart refresh with real loss is accepted instead of failing at once. Later ticks still fail, so this does not change any outcome.

## Design questions for the owner
None.
