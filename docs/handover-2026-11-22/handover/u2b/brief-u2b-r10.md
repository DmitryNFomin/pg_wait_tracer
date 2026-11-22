# Brief: U2b round 10 (tests only): the known-loss exit is decided last

## Authorised by

Owner, 2026-11-14: "Please take lead here, I think we move really slow" (core rework block). A fresh Opus review of rounds 7-8 (scratchpad/u2b/review-137aa637.md; scratchpad = SP) found the product code correct and the scoped known-failing exit (94, the proven EL8 event-loss case) applied too early in three tests, so checks that do not depend on withheld numbers no longer run on EL8.

Issue #947; live comments (refreshed at dispatch):

```console
$ gh api repos/DmitryNFomin/pg_wait_tracer/issues/947/comments --paginate --jq '.[] | {id: .id, body: .body}'
{"body":"**Owner decision, Q1 (captures recorded before coverage tracking), 2026-11-18:** \"a\"\n\nWhat (a) means, as put to the owner:\n- Every capture recorded before the coverage record ships shows \"coverage not recorded — capture predates coverage tracking\". This includes the 26 demo-rehearsal captures.\n- Their whole-window numbers are withheld, the same as for any window whose coverage cannot be confirmed.\n- No legacy exclusion from #947.\n- The demo captures are re-recorded before the tag.\n\nFollow-up: re-record the demo-rehearsal captures before the tag, as a scheduled item once the coverage label (U1/U2b) is in master.\n\nRefs #947\n","id":6980525535}
{"body":"**Coverage spec r2, Amendment A4 (2026-11-20): the evidence-v1 pilot route is outside the coverage label**\n\nThe U2b implementer stopped at spec §3 \"pilot_wait_metrics ANDs its evidence verdict\". Both design advisers answered (Astra, Fable): the rule is wrong as written and is amended, not implemented.\n\n- The pilot route reads only v5 evidence-v1 files; v2/v3/v4/v6 containers are filtered out before its codec. v5 files carry no COVERAGE records, and evidence-v1 carries its own life/coverage/loss verdict per (capture, pid).\n- The two certify different things on different clocks: pilot windows are monotonic ns, the coverage verdict is wall-mapped. ANDing would withhold every answer from a v5-only directory, and in a mixed directory would judge a monotonic window against wall boots.\n- **Rule:** the pilot response keeps its own shape (`pilot:true`, `contract:\"evidence-v1\"`, per-metric completeness and reasons) and carries no `fidelity` and no `coverage` key, on success and on `pilot_unsupported`. It bypasses the coverage emitter entirely.\n- **Tests:** a dispatch-level case with a real v5 fixture inside a restart directory (the response must equal the v5-alone response and carry neither key), plus two mutants: pilot added to the withhold list, and the coverage emitter re-enabled for the pilot.\n- **Stated limit:** with the pilot enabled, the Events page can show a pilot value beside coverage-withheld panels for the same window. It is a different quantity (one backend, one capture, its own certificate), not a second view of one number.\n- A stale web comment and two web tests that assume the server ANDs are left for a later web change.\n\nRefs #947\n","id":7033879421}
{"body":"**Coverage spec r2, Amendment A5 (2026-11-20): the Live window on a young capture, and when \"retention\" applies**\n\nFound by the U2b round-4 review on a real daemon: the web's Live mode always asks for [now − 15 min, now). On a capture younger than 15 minutes, and for 15 minutes after every daemon restart, that window starts before the boot's start S, so all ten whole-window views are withheld. The label also read \"(older than the retained data)\" on a fresh capture where nothing was removed. Both design advisers answered (Astra, Fable); they agree on the verdict and the direction, and differ only on where the clip lives. Resolved by the lead as below.\n\n1. **The verdict is unchanged.** An explicit window that starts before S stays incomplete (rows 23 and 24c stand). The request was wrong, not the verdict.\n2. **Live is a span resolved by the server.** A live request carries a span and no `from`; the server answers over [max(now − span, S of the live boot), live edge] and echoes the resolved bounds plus `requested_from_ns` and a clip cause (`capture_start` or `restart`). An explicit `from` is never moved. Reason for the server rather than the web: the web holds nanoseconds as JS doubles, and Fable's simulation showed that rounding its copy of S up to the next microsecond still lands below S in about 12% of cases; the server holds S exactly.\n3. **After a restart, Live starts at the current boot.** It shows the new boot's numbers only, never summed across boots, with the notice naming the restart and its time. An explicit window across the restart stays withheld (restart).\n4. **UI text:** \"Last 15 min — showing since capture start HH:MM:SS\", or \"… since daemon restart HH:MM:SS\" / \"… since PostgreSQL restart HH:MM:SS\". Rates, averages and AAS denominators use the resolved bounds, not the requested span.\n5. **\"Retention\" needs evidence of removal.** [boot_start, S) is `retention` only when the earliest retained file of the boot has file_no \u003e 0. When file 0 is present, that short startup stretch (daemon start to the first file's open) is `not_captured`. New verdict row: earliest file has file_no 0 and BOOT observation 5, boot_start 0: window [0,50) is incomplete [not_captured] u[0,5); [5,50) is complete. Row 13a keeps `retention`.\n6. **Gate:** the real-daemon live UI smoke opens the untouched default view on a young capture and asserts numbers on every whole-window tab plus the clip notice, and a second arm restarts the daemon and asserts the same within one refresh. Removing the server's clip must turn it red.\n\nRefs #947\n","id":7037950207}
```

CONTROLLING SCOPE DECISION: #947 comment 6980525535 "Their whole-window numbers are withheld, the same as for any window whose coverage cannot be confirmed."

## Starting point

Run scripts/lead/codex-task.sh on the EXISTING branch agent/947-coverage-server at 3b53b034 (round 9). First run `git log --oneline -1`; it must print 3b53b034. Otherwise print WRONG_HEAD <sha> and stop.

Commit with messages ending "Refs #947". Do not push. Do not run `make check` on the Mac. Change tests/ only. Your Linux machines (NEW addresses; the earlier two VMs are gone, and the old EL8 address now belongs to the Ubuntu VM): EL8 root@<ip>, Ubuntu root@<ip>. Both are freshly provisioned: rsync and build from scratch. Round 9 (3b53b034) could not collect its full-tier runs because the earlier VMs were deleted mid-round; your full-tier runs here are the evidence for round 9 as well.

## Task

Rule for every test that can exit 94: run every check that does not depend on the withheld whole-window numbers, in full, and fail normally (non-94) if any of them fails; decide 94 only at the end, and only when the sole unmet checks are the ones whose window is incomplete with capture_loss while this run's own drop counters are nonzero.

1. **tests/ui_live_smoke.sh around :446-449 and tests/ui_live_smoke.py:** today a 94 from the default-Live gate exits before the stability walk and the blink self-test, so on EL8 blinks, page errors, leaks and missing tabs are never checked (master checks them there). Run the walk and the self-test regardless; they must pass; only the numeric default-Live assertions may be the 94 case. Limit `loss_signature` to the EL8 4.18 case (review nit, ui_live_smoke.py around :859). The run_all summary line must say the live UI smoke ran.
2. **tests/test_query_event.py around :426:** the 94 decision is taken before the Mode B checks, which grade the CLI's own output (not withheld). Run Mode B always; it must pass or fail on its own. The review notes that in the final EL8 run Mode B had 3 rows (a pass) and still exited 94. Also settle the original "Mode B parsed 2 entries" cause (round 7's EL8 failure): reproduce it or show with evidence that the startup-race fix removed it; say which.
3. **tests/test_capture_smoke.py around :864 and :1651-1660:** do not skip the live-view checks on a 94; and check the controlled window first: when the controlled window itself is covered (no drop inside it, as in the run the review cites: drops 14 -> 14 across the controlled interval), the exact per-PID checks must run and pass even if the wider default window has loss.
4. Extend the forced-different-failure controls so that each of these tests is forced to fail in a check that runs AFTER the old 94 decision point (a blink in the walk, a wrong Mode B row count, a wrong per-PID value in the controlled window) while loss is present: each must fail the gate with a non-94 exit.
5. Run the full box tier on both VMs and paste both `Executed:` lines and the "Live UI smoke:" lines.

## Excluded

src/, web/, contracts/, new KNOWN_FAILING entries, retries, tolerances, creating or deleting VMs.

## Done

Report: commit sha, `git diff --stat 3b53b034`, both `Executed:` lines and live-UI lines, the new controls' results, the Mode B root-cause statement, both registration guards, `git diff --check`.

## Budget

Rounds: 1
Wall-clock minutes: 180

## Report by

At the end of the budget, even if incomplete: at most 10 lines.
