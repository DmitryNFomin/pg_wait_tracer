# Brief: U2b round 6 (tests only): make the two new live gates actually run and wait correctly

## Authorised by

Owner, 2026-11-14: "Please take lead here, I think we move really slow" (core rework block). The review of 7a474fac (scratchpad/u2b/review-7a474fac.md) found the product code in src/ and web/ correct and the live-window defect fixed, with two blockers in test code only. scratchpad = SP

Issue #947; live comments (refreshed at dispatch):

```console
$ gh api repos/DmitryNFomin/pg_wait_tracer/issues/947/comments --paginate --jq '.[] | {id: .id, body: .body}'
{"body":"**Owner decision, Q1 (captures recorded before coverage tracking), 2026-11-18:** \"a\"\n\nWhat (a) means, as put to the owner:\n- Every capture recorded before the coverage record ships shows \"coverage not recorded — capture predates coverage tracking\". This includes the 26 demo-rehearsal captures.\n- Their whole-window numbers are withheld, the same as for any window whose coverage cannot be confirmed.\n- No legacy exclusion from #947.\n- The demo captures are re-recorded before the tag.\n\nFollow-up: re-record the demo-rehearsal captures before the tag, as a scheduled item once the coverage label (U1/U2b) is in master.\n\nRefs #947\n","id":6980525535}
{"body":"**Coverage spec r2, Amendment A4 (2026-11-20): the evidence-v1 pilot route is outside the coverage label**\n\nThe U2b implementer stopped at spec §3 \"pilot_wait_metrics ANDs its evidence verdict\". Both design advisers answered (Astra, Fable): the rule is wrong as written and is amended, not implemented.\n\n- The pilot route reads only v5 evidence-v1 files; v2/v3/v4/v6 containers are filtered out before its codec. v5 files carry no COVERAGE records, and evidence-v1 carries its own life/coverage/loss verdict per (capture, pid).\n- The two certify different things on different clocks: pilot windows are monotonic ns, the coverage verdict is wall-mapped. ANDing would withhold every answer from a v5-only directory, and in a mixed directory would judge a monotonic window against wall boots.\n- **Rule:** the pilot response keeps its own shape (`pilot:true`, `contract:\"evidence-v1\"`, per-metric completeness and reasons) and carries no `fidelity` and no `coverage` key, on success and on `pilot_unsupported`. It bypasses the coverage emitter entirely.\n- **Tests:** a dispatch-level case with a real v5 fixture inside a restart directory (the response must equal the v5-alone response and carry neither key), plus two mutants: pilot added to the withhold list, and the coverage emitter re-enabled for the pilot.\n- **Stated limit:** with the pilot enabled, the Events page can show a pilot value beside coverage-withheld panels for the same window. It is a different quantity (one backend, one capture, its own certificate), not a second view of one number.\n- A stale web comment and two web tests that assume the server ANDs are left for a later web change.\n\nRefs #947\n","id":7033879421}
{"body":"**Coverage spec r2, Amendment A5 (2026-11-20): the Live window on a young capture, and when \"retention\" applies**\n\nFound by the U2b round-4 review on a real daemon: the web's Live mode always asks for [now − 15 min, now). On a capture younger than 15 minutes, and for 15 minutes after every daemon restart, that window starts before the boot's start S, so all ten whole-window views are withheld. The label also read \"(older than the retained data)\" on a fresh capture where nothing was removed. Both design advisers answered (Astra, Fable); they agree on the verdict and the direction, and differ only on where the clip lives. Resolved by the lead as below.\n\n1. **The verdict is unchanged.** An explicit window that starts before S stays incomplete (rows 23 and 24c stand). The request was wrong, not the verdict.\n2. **Live is a span resolved by the server.** A live request carries a span and no `from`; the server answers over [max(now − span, S of the live boot), live edge] and echoes the resolved bounds plus `requested_from_ns` and a clip cause (`capture_start` or `restart`). An explicit `from` is never moved. Reason for the server rather than the web: the web holds nanoseconds as JS doubles, and Fable's simulation showed that rounding its copy of S up to the next microsecond still lands below S in about 12% of cases; the server holds S exactly.\n3. **After a restart, Live starts at the current boot.** It shows the new boot's numbers only, never summed across boots, with the notice naming the restart and its time. An explicit window across the restart stays withheld (restart).\n4. **UI text:** \"Last 15 min — showing since capture start HH:MM:SS\", or \"… since daemon restart HH:MM:SS\" / \"… since PostgreSQL restart HH:MM:SS\". Rates, averages and AAS denominators use the resolved bounds, not the requested span.\n5. **\"Retention\" needs evidence of removal.** [boot_start, S) is `retention` only when the earliest retained file of the boot has file_no \u003e 0. When file 0 is present, that short startup stretch (daemon start to the first file's open) is `not_captured`. New verdict row: earliest file has file_no 0 and BOOT observation 5, boot_start 0: window [0,50) is incomplete [not_captured] u[0,5); [5,50) is complete. Row 13a keeps `retention`.\n6. **Gate:** the real-daemon live UI smoke opens the untouched default view on a young capture and asserts numbers on every whole-window tab plus the clip notice, and a second arm restarts the daemon and asserts the same within one refresh. Removing the server's clip must turn it red.\n\nRefs #947\n","id":7037950207}
```

CONTROLLING SCOPE DECISION: #947 comment 7037950207 "Removing the server's clip must turn it red."

## Starting point

Run scripts/lead/codex-task.sh on the EXISTING branch agent/947-coverage-server at 7a474fac. First run `git log --oneline -1`; it must print 7a474fac. Otherwise print WRONG_HEAD <sha> and stop.

Commit with messages ending "Refs #947". Do not push. Do not run `make check`. Change tests/ only. Do not touch src/ or web/.

## Task

1. **tests/test_ui_live_window.py** (registered at tests/unit_tests.list:268):
   - Around :28, a local `http = http.server...` shadows the `http` module imported at the top, so the test raises UnboundLocalError wherever Playwright is installed. Rename the local.
   - Around :87, Playwright is imported unconditionally, so CI's unit job (which installs only pillow and numpy) cannot import the file. Follow the repo's import-guard rule (tests/test_unit_import_guard.py): import Playwright inside the function that drives the browser, keep the pure logic importable, and make the browser part run where check.sh runs browser tests. If the browser is unavailable where the test is required, it must fail loudly, not pass.
   - Show that the clip-removal control is red: with the server's live-span clip disabled (the existing mutant or an equivalent), the test fails.
2. **tests/ui_live_smoke.py around :809-813:** the "first refresh after restart" marker is taken before `restart()`, so live ticks queued during the restart count as the first fresh answer; during the restart gap the server correctly withholds, and the gate fails for any gap of about 5 s or more. Take the marker after the restarted daemon is serving (its first BOOT/WATERMARK is visible to the server), then require numbers within one refresh. Do not add retries or longer tolerances; a gap the gate cannot handle must still fail.
3. Run both tests on the Mac with Chromium (the lead's environment has it: PYENV_VERSION=3.11.7), report the output, and report the mock reproduction of the restart arm with a 6 s gap and a 2 s gap (both must now pass, and the clip-removal control must still fail).

## Excluded

src/, web/, everything outside these two test files and their registration, KNOWN_FAILING, retries, tolerances.

## Done

Report: commit sha, `git diff --stat 7a474fac`, the two tests' output, the control's red result, both registration guards, `git diff --check`.

## Budget

Rounds: 1
Wall-clock minutes: 45

## Report by

At the end of the budget, even if incomplete: at most 8 lines.
