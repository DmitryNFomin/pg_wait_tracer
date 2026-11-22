# Brief: U2b round 7 (rewritten; fresh implementer): make every tier green, on real Linux and in a real browser

## Authorised by

Owner, 2026-11-14: "Please take lead here, I think we move really slow" (core rework block). The coverage label's server half is on branch agent/947-coverage-server at 1d9a90ff. Its product design is settled (spec r2 with Amendments A1-A5, #947 comments 7033879421 and 7037950207) and its review found the src/ and web/ logic correct. What fails is the evidence the earlier rounds could not run: the Mac browser suite, and the Linux box tier. scratchpad = SP

Issue #947; live comments (refreshed at dispatch):

```console
$ gh api repos/DmitryNFomin/pg_wait_tracer/issues/947/comments --paginate --jq '.[] | {id: .id, body: .body}'
{"body":"**Owner decision, Q1 (captures recorded before coverage tracking), 2026-11-18:** \"a\"\n\nWhat (a) means, as put to the owner:\n- Every capture recorded before the coverage record ships shows \"coverage not recorded — capture predates coverage tracking\". This includes the 26 demo-rehearsal captures.\n- Their whole-window numbers are withheld, the same as for any window whose coverage cannot be confirmed.\n- No legacy exclusion from #947.\n- The demo captures are re-recorded before the tag.\n\nFollow-up: re-record the demo-rehearsal captures before the tag, as a scheduled item once the coverage label (U1/U2b) is in master.\n\nRefs #947\n","id":6980525535}
{"body":"**Coverage spec r2, Amendment A4 (2026-11-20): the evidence-v1 pilot route is outside the coverage label**\n\nThe U2b implementer stopped at spec §3 \"pilot_wait_metrics ANDs its evidence verdict\". Both design advisers answered (Astra, Fable): the rule is wrong as written and is amended, not implemented.\n\n- The pilot route reads only v5 evidence-v1 files; v2/v3/v4/v6 containers are filtered out before its codec. v5 files carry no COVERAGE records, and evidence-v1 carries its own life/coverage/loss verdict per (capture, pid).\n- The two certify different things on different clocks: pilot windows are monotonic ns, the coverage verdict is wall-mapped. ANDing would withhold every answer from a v5-only directory, and in a mixed directory would judge a monotonic window against wall boots.\n- **Rule:** the pilot response keeps its own shape (`pilot:true`, `contract:\"evidence-v1\"`, per-metric completeness and reasons) and carries no `fidelity` and no `coverage` key, on success and on `pilot_unsupported`. It bypasses the coverage emitter entirely.\n- **Tests:** a dispatch-level case with a real v5 fixture inside a restart directory (the response must equal the v5-alone response and carry neither key), plus two mutants: pilot added to the withhold list, and the coverage emitter re-enabled for the pilot.\n- **Stated limit:** with the pilot enabled, the Events page can show a pilot value beside coverage-withheld panels for the same window. It is a different quantity (one backend, one capture, its own certificate), not a second view of one number.\n- A stale web comment and two web tests that assume the server ANDs are left for a later web change.\n\nRefs #947\n","id":7033879421}
{"body":"**Coverage spec r2, Amendment A5 (2026-11-20): the Live window on a young capture, and when \"retention\" applies**\n\nFound by the U2b round-4 review on a real daemon: the web's Live mode always asks for [now − 15 min, now). On a capture younger than 15 minutes, and for 15 minutes after every daemon restart, that window starts before the boot's start S, so all ten whole-window views are withheld. The label also read \"(older than the retained data)\" on a fresh capture where nothing was removed. Both design advisers answered (Astra, Fable); they agree on the verdict and the direction, and differ only on where the clip lives. Resolved by the lead as below.\n\n1. **The verdict is unchanged.** An explicit window that starts before S stays incomplete (rows 23 and 24c stand). The request was wrong, not the verdict.\n2. **Live is a span resolved by the server.** A live request carries a span and no `from`; the server answers over [max(now − span, S of the live boot), live edge] and echoes the resolved bounds plus `requested_from_ns` and a clip cause (`capture_start` or `restart`). An explicit `from` is never moved. Reason for the server rather than the web: the web holds nanoseconds as JS doubles, and Fable's simulation showed that rounding its copy of S up to the next microsecond still lands below S in about 12% of cases; the server holds S exactly.\n3. **After a restart, Live starts at the current boot.** It shows the new boot's numbers only, never summed across boots, with the notice naming the restart and its time. An explicit window across the restart stays withheld (restart).\n4. **UI text:** \"Last 15 min — showing since capture start HH:MM:SS\", or \"… since daemon restart HH:MM:SS\" / \"… since PostgreSQL restart HH:MM:SS\". Rates, averages and AAS denominators use the resolved bounds, not the requested span.\n5. **\"Retention\" needs evidence of removal.** [boot_start, S) is `retention` only when the earliest retained file of the boot has file_no \u003e 0. When file 0 is present, that short startup stretch (daemon start to the first file's open) is `not_captured`. New verdict row: earliest file has file_no 0 and BOOT observation 5, boot_start 0: window [0,50) is incomplete [not_captured] u[0,5); [5,50) is complete. Row 13a keeps `retention`.\n6. **Gate:** the real-daemon live UI smoke opens the untouched default view on a young capture and asserts numbers on every whole-window tab plus the clip notice, and a second arm restarts the daemon and asserts the same within one refresh. Removing the server's clip must turn it red.\n\nRefs #947\n","id":7037950207}
```

CONTROLLING SCOPE DECISION: #947 comment 7037950207 "Rates, averages and AAS denominators use the resolved bounds, not the requested span."

## Starting point

Run scripts/lead/codex-task.sh on the EXISTING branch agent/947-coverage-server at 1d9a90ff. First run `git log --oneline -1`; it must print 1d9a90ff. Otherwise print WRONG_HEAD <sha> and stop.

Commit with messages ending "Refs #947". Do not push. Do not run `make check` on the Mac. Never commit PNGs.

## Your Linux machines (new this round)

Two throwaway VMs are yours for this round, reachable with `ssh root@<ip>` (your network access is enabled):
- EL8 (Rocky 8.10, kernel 4.18): root@<ip>
- Ubuntu (24.04, kernel 6.8, Chromium + Playwright installed): root@<ip>
Both are fully provisioned box-tier machines (PG 13/16/17/18, the repo's build deps). Copy your working tree with rsync (exclude .git, build outputs and tests/results), build there, and run the failing tests directly as root, the way tests/run_all.sh runs them. Run the Mac browser suites on the Ubuntu VM (your Mac sandbox cannot launch Chromium). Before you report, run the full box tier on both: `cd <dir> && tests/run_all.sh --require-live` (or the command scripts/box-tier.sh uses), and paste each `Executed:` summary. Do not create or delete VMs; the lead deletes these.

## Task

Fix every failure below, following the Rules section.

### Failures to fix (all on 1d9a90ff)

Mac `make check` (log: scratchpad/u2b/g6/check-1d9a90ff.log):
- tests/test_web_ui.py: "FAIL: AAS = 3.54 (got '0.01')": a wrong AAS on the main chart, most likely the resolved-bounds denominator (A5 item 4) or the mock's resolved bounds. Find which, fix the product if the product is wrong.
- tests/test_ui_live_smoke_nav.py: AttributeError `ui_live_smoke.COVERED_LIVE_SPAN_JS` (round 6 removed it; the nav test still uses it).

EL8 box tier (log: scratchpad/u2b/g6/el8-1d9a90ff.log, `Executed: 239 (passed 225, failed 9, ...)`; "Live UI smoke: NOT RUN in this invocation"):
- test_daemon_server: "Trace covers time range 1795179581013427280 -> 1795179581008206268" (end before start) and "Server DB Time = 0ms".
- test_capture_smoke_tiered: TypeError int + NoneType.
- test_capture_smoke_full: "pure-CPU straddle trace carries CPU after terminal flush: cpu_ms = 0" and the straddle window incomplete [capture_loss].
- test_data_exec_prefix: "index run really used the index (got ['legacy', 'index', ...])" and two related lines.
- test_data_current_trace_cache: the stub-reason case.
- test_backend_attach_race: ('wp_attach_failures_total', 1) (WP-ATTACH-FAIL errno=3 with liveness errno 13): determine whether this branch causes it; it also appeared on another branch, so if it is not this branch's, say so with evidence and do not change it.
- the remaining failing tests in that log, and why the live UI smoke did not run.
Ubuntu box tier: scratchpad/u2b/g6/ubu-1d9a90ff.log, still running at dispatch; read it when it has an `Executed:` line and add its failures.

## Rules

For each failure: root cause first (product defect vs outdated test), then the fix. A product defect gets a test that fails before your fix. A test change must not weaken what the test checks; a real-daemon test must still see numbers for a window inside covered time. No KNOWN_FAILING, retries or tolerances. Keep every earlier mutant red.

## Excluded

The trace format and writer, src/coverage.{c,h} rules beyond A5, the transport, A2, contracts/, KNOWN_FAILING, retries, tolerances, tests/web_snapshots/*.png, creating or deleting VMs.

## Done

Report: a per-failure table (test, cause, fix), the commit sha, `git diff --stat 1d9a90ff`, the browser suites' output from the Ubuntu VM, the two full box-tier `Executed:` lines (EL8 and Ubuntu), both registration guards, `git diff --check`.

## Budget

Rounds: 1
Wall-clock minutes: 300

## Report by

At the end of the budget, even if incomplete: at most 15 lines plus the table.
