# Brief: U2b round 9: the last box-tier failure (tiered capture-smoke readiness)

## Authorised by

Owner, 2026-11-14: "Please take lead here, I think we move really slow" (core rework block). Round 8 (137aa637) left exactly one failing test on each system: EL8 `Executed: 239 (passed 228, failed 1, refused 4, known-failing 6 (1 xpass))`, Ubuntu `Executed: 240 (passed 238, failed 1, ...)` (/private/tmp/u2b-r8-evidence/{el8,ubuntu}/box-final.log). Its own report: "both tiers still fail sampled CPU-straddle readiness (69.7/83.9 ms `not_captured`). Waiting for a durable watermark remains."

Issue #947; live comments (refreshed at dispatch):

```console
$ gh api repos/DmitryNFomin/pg_wait_tracer/issues/947/comments --paginate --jq '.[] | {id: .id, body: .body}'
{"body":"**Owner decision, Q1 (captures recorded before coverage tracking), 2026-11-18:** \"a\"\n\nWhat (a) means, as put to the owner:\n- Every capture recorded before the coverage record ships shows \"coverage not recorded — capture predates coverage tracking\". This includes the 26 demo-rehearsal captures.\n- Their whole-window numbers are withheld, the same as for any window whose coverage cannot be confirmed.\n- No legacy exclusion from #947.\n- The demo captures are re-recorded before the tag.\n\nFollow-up: re-record the demo-rehearsal captures before the tag, as a scheduled item once the coverage label (U1/U2b) is in master.\n\nRefs #947\n","id":6980525535}
{"body":"**Coverage spec r2, Amendment A4 (2026-11-20): the evidence-v1 pilot route is outside the coverage label**\n\nThe U2b implementer stopped at spec §3 \"pilot_wait_metrics ANDs its evidence verdict\". Both design advisers answered (Astra, Fable): the rule is wrong as written and is amended, not implemented.\n\n- The pilot route reads only v5 evidence-v1 files; v2/v3/v4/v6 containers are filtered out before its codec. v5 files carry no COVERAGE records, and evidence-v1 carries its own life/coverage/loss verdict per (capture, pid).\n- The two certify different things on different clocks: pilot windows are monotonic ns, the coverage verdict is wall-mapped. ANDing would withhold every answer from a v5-only directory, and in a mixed directory would judge a monotonic window against wall boots.\n- **Rule:** the pilot response keeps its own shape (`pilot:true`, `contract:\"evidence-v1\"`, per-metric completeness and reasons) and carries no `fidelity` and no `coverage` key, on success and on `pilot_unsupported`. It bypasses the coverage emitter entirely.\n- **Tests:** a dispatch-level case with a real v5 fixture inside a restart directory (the response must equal the v5-alone response and carry neither key), plus two mutants: pilot added to the withhold list, and the coverage emitter re-enabled for the pilot.\n- **Stated limit:** with the pilot enabled, the Events page can show a pilot value beside coverage-withheld panels for the same window. It is a different quantity (one backend, one capture, its own certificate), not a second view of one number.\n- A stale web comment and two web tests that assume the server ANDs are left for a later web change.\n\nRefs #947\n","id":7033879421}
{"body":"**Coverage spec r2, Amendment A5 (2026-11-20): the Live window on a young capture, and when \"retention\" applies**\n\nFound by the U2b round-4 review on a real daemon: the web's Live mode always asks for [now − 15 min, now). On a capture younger than 15 minutes, and for 15 minutes after every daemon restart, that window starts before the boot's start S, so all ten whole-window views are withheld. The label also read \"(older than the retained data)\" on a fresh capture where nothing was removed. Both design advisers answered (Astra, Fable); they agree on the verdict and the direction, and differ only on where the clip lives. Resolved by the lead as below.\n\n1. **The verdict is unchanged.** An explicit window that starts before S stays incomplete (rows 23 and 24c stand). The request was wrong, not the verdict.\n2. **Live is a span resolved by the server.** A live request carries a span and no `from`; the server answers over [max(now − span, S of the live boot), live edge] and echoes the resolved bounds plus `requested_from_ns` and a clip cause (`capture_start` or `restart`). An explicit `from` is never moved. Reason for the server rather than the web: the web holds nanoseconds as JS doubles, and Fable's simulation showed that rounding its copy of S up to the next microsecond still lands below S in about 12% of cases; the server holds S exactly.\n3. **After a restart, Live starts at the current boot.** It shows the new boot's numbers only, never summed across boots, with the notice naming the restart and its time. An explicit window across the restart stays withheld (restart).\n4. **UI text:** \"Last 15 min — showing since capture start HH:MM:SS\", or \"… since daemon restart HH:MM:SS\" / \"… since PostgreSQL restart HH:MM:SS\". Rates, averages and AAS denominators use the resolved bounds, not the requested span.\n5. **\"Retention\" needs evidence of removal.** [boot_start, S) is `retention` only when the earliest retained file of the boot has file_no \u003e 0. When file 0 is present, that short startup stretch (daemon start to the first file's open) is `not_captured`. New verdict row: earliest file has file_no 0 and BOOT observation 5, boot_start 0: window [0,50) is incomplete [not_captured] u[0,5); [5,50) is complete. Row 13a keeps `retention`.\n6. **Gate:** the real-daemon live UI smoke opens the untouched default view on a young capture and asserts numbers on every whole-window tab plus the clip notice, and a second arm restarts the daemon and asserts the same within one refresh. Removing the server's clip must turn it red.\n\nRefs #947\n","id":7037950207}
```

CONTROLLING SCOPE DECISION: #947 comment 7037950207 "When file 0 is present, that short startup stretch (daemon start to the first file's open) is `not_captured`."

## Starting point

Run scripts/lead/codex-task.sh on the EXISTING branch agent/947-coverage-server at 137aa637. First run `git log --oneline -1`; it must print 137aa637. Otherwise print WRONG_HEAD <sha> and stop.

Commit with messages ending "Refs #947". Do not push. Do not run `make check` on the Mac. Your Linux machines (same as before): EL8 root@<ip>, Ubuntu root@<ip>.

## Task

1. tests/test_capture_smoke.py, tiered mode, the sampled CPU-straddle phase: it starts its measured window before the capture has a durable watermark, so the first 70-84 ms read `not_captured` and the phase fails on both systems. Make the phase wait until the server reports coverage that reaches the start of the window it is about to measure (the boot's S and a watermark past it), then measure; keep every assertion about waits, attribution and CPU, and keep the phase's duration. No retry loop around the assertion and no tolerance for an uncovered sliver: readiness first, then one strict measurement.
2. If, after that, anything else fails on either system, treat it as round 8 did (root cause first; the exact-signature exit 94 only for the proven EL8 event-loss case round 8 defined).
3. Run the full box tier on both VMs and paste both `Executed:` lines. The target is failed 0 on both.

## Excluded

src/, web/, contracts/, new KNOWN_FAILING entries other than that proven EL8 event-loss case, retries, tolerances, creating or deleting VMs.

## Done

Report: commit sha, `git diff --stat 137aa637`, both `Executed:` lines, both registration guards, `git diff --check`.

## Budget

Rounds: 1
Wall-clock minutes: 150

## Report by

At the end of the budget, even if incomplete: at most 8 lines.
