# Review: agent/947-coverage-server, round 5 (fb43c3c6..7a474fac), whole unit 4059fcde..7a474fac

**Verdict: NOT-READY. There are 2 blockers, and both are in test code. The product code (src/ and web/) for A5 is correct on every point checked. The B1 scenario is fixed.**

## What changed
- In Live mode the UI now sends a span (`live_span`) and no start time. The server resolves the window to [max(now − span, S of the current boot), live edge] and echoes the resolved bounds, `requested_from_ns`, `clip` and the exact ns strings.
- On a young capture, or after a daemon or PostgreSQL restart, the default "Last 15 min" view shows numbers on every whole-window tab. The time label reads "Last 15 min — showing since capture start / daemon restart / PostgreSQL restart HH:MM:SS".
- An explicit window that starts before S is still withheld (rows 23 and 24c). An explicit window across a restart is still withheld.
- [boot_start, S) is labelled `retention` only when the boot's earliest retained file has file_no > 0. Otherwise it is `not_captured` (new row 31a/b).
- If a rotation happens between the meta read and the reader open, that one request's coverage is withheld. It is no longer reported as a false `trace_unreadable`.

## Evidence
- Check stamp: there is no `.pgwt-check.stamp` in wt-u2b5. `scripts/tree-hash.sh` = 3cf5db4bddf330a638d20dc14d8f413323b80eb1. I did not run make check (read-only brief).
- Box-check: there is **no box-check log for 7a474fac**. The round-4 logs (u2b/g3/el8-fb43c3c6.log and ubu-fb43c3c6.log) were still growing at 11:49Z and have no `Executed:` line yet.
- UI gallery (the lead's run, u2b/g5/gallery-7a474fac.log): "0 changed, 3 added, 0 removed, 90 unchanged". The added cells are fidelity-live-capture-start, fidelity-live-daemon-restart and fidelity-live-postgres-restart. I opened the postgres cell; it shows the A5 text.
- Host runs on a `git archive 7a474fac` export (scratchpad/rv-u2b5):
  - test_coverage_consumers.py: PASS on all cases, including live_span and rotation_snapshot. unbound_rotation_witness is RED. Timing: max 18.1 ms.
  - coverage_consumers_mutants.py: "18 built, 18 red on named cases".
  - coverage_verdict_mutants.py: "27 killed, 4 drift controls: PASS". ignore-file-no is killed on 31a and never-retention on 13a.
  - web_unit: 968/968 pass.
  - The live-smoke footage, failure and lib tests and the import guard pass.
  - **test_ui_live_window.py fails** (finding 1).
  - On a copy with only finding 1's variable rename (scratchpad/rv-u2b5p), it passes: all 7 tabs render numbers with the capture-start label, then with the daemon-restart label, and the gate prints "RED ignore_S_live_UI" (the clip-removal mutant goes red).

## The six verification points
1. **B1 scenario: fixed.** The real-clock C server was given `live_span: 900` on a capture only milliseconds old. All 10 withheld routes, plus aas, heatmap and info, return complete or live_edge. from_ns equals the fixture's S exactly, and clip is capture_start. After a restart with the same postmaster the cause is daemon; with a different postmaster it is postgres. An explicit [S−1 s, to] is still withheld (not_captured or restart), and so is an explicit window across the restart.
2. **One S.** `pgwt_cov_boot_start` (coverage.c:48) is called by prepare_boot (:382), by pgwt_cov_trace_window (coverage_trace.c:419) and by server.c:5321. All three read the same `boots` array (coverage_trace.c:382). The clip is an exact int64 assignment, so the resolved from can never land below S. The test asserts `from_ns == S` exactly.
3. **Denominators.** On the server, handlers read req->from_ns/to_ns, which are overwritten with the resolved bounds at server.c:5352-5353; the mutant live_denominator_uses_requested_span is RED. On the web:
   - buildSummary, timeline and waterfall use the bounds in the response.
   - active.js builderOpts uses the primary response's bounds.
   - Everything else reads ctx.timeRange, which applyResolved sets before build (view-manager.js:126, app.js:692).
   - Compare B is A's exact resolved window, shifted.
4. **Retention.** The rule is at coverage.c:551-555. BOOT rec.b is the writer's file_no (event_writer.c:913). Row 31a/b matches the A5 text, which is the independent oracle. Both mutants are real and red.
5. **Live UI gate.** The main arm opens the untouched default view: no hash, a 900 s span, and it waits for `resolvedLive.clip`. With the clip removed it goes red (the C-server mutant control, once finding 1 is fixed). The restart arm does SIGTERM the real daemon and start a replacement, but it is racy (finding 2).
6. **Regressions:** none. Live totals, info on an empty clip, the cache, the pilot bypass, withholding, the replay refusal and integer endpoints all still pass, and their mutants are red.

## Findings
1. **Blocker: tests/test_ui_live_window.py:28 (and :87) fails on every runner. It is registered at tests/unit_tests.list:268.**
   - Input: run_all's unit tier on any box-check, or `make -C tests check` in ci.yml:409-415 and nightly.yml.
   - Expected: PASS.
   - Actual, reproduced: with Playwright installed, `UnboundLocalError: cannot access local variable 'http'` at :26. The cause: `http = http.server.ThreadingHTTPServer(...)` makes `http` a local name, but the module is imported at module scope. test_pilot_route.py avoids this only because it imports http.server inside the function.
   - CI's unit job installs only pillow and numpy (ci.yml:409). There, the unconditional `from playwright.sync_api import ...` at :87 raises ModuleNotFoundError. This part is read from source, not reproduced. test_pilot_route.py:485 shows the established NOT-RUN gate for that case.
   - So the browser default/restart gate and its clip-removal control never ran.
2. **Blocker: tests/ui_live_smoke.py:809-813. The restart arm's "first refresh" check counts replies from before the restart. Read from source; reproduced with the mock.**
   - `first = len(replies)` is taken before `restart()`. During restart()'s blocking sleep, Playwright's sync API buffers websocket events and dispatches them on the next call. Any 5 s Live tick that lands between SIGTERM and the replacement's first watermark therefore becomes `fresh[0]`. During that gap the server correctly answers coverage_unverified, or still answers clip=capture_start for the old boot. The gate then raises "first refresh withheld numbers".
   - Input: the mock gate with a simulated restart (state set to unclean_end for 6 s, then live_daemon_restart; scratchpad/rv5-scripts/race.py).
   - Expected: PASS, because the product is correct.
   - Actual: "GATE FAILED … SmokeFailure restart Live: first refresh withheld numbers: [{'code': 'coverage_unverified', … 'unclean_end'…".
   - With a 2 s gap it passed 3 of 3 runs.
   - The real restart gap is SIGTERM shutdown, plus replacement startup, plus the 1 s first-watermark timer. It is not measured. Any gap of 5 s or more fails every run; shorter gaps fail intermittently. That is noise by construction, and a later fix would be tempted to mask it with a retry.
   - The check must count only replies to requests issued after the new boot's first watermark: for example, take the marker after restart() and after draining queued events, or key replies on resolved from_ns ≥ the new boot_start_wall.

No product-code finding.

## Design questions for the owner
None.
