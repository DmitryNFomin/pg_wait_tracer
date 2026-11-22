# Brief: U2b round 11 (tests only, closing): the stability walk on coverage-withheld panes, then the final full tiers

## Authorised by

Owner, 2026-11-14: "Please take lead here, I think we move really slow" (core rework block). Round 10 (08b54616) moved the known-loss exit 94 to the end of three tests and reported PARTIAL; its own evidence is /private/tmp/u2b-r10-evidence/case-summary.txt (read it first: it names what is left). The review that asked for this is scratchpad/u2b/review-137aa637.md (scratchpad = SP).

Issue #947; live comments (refreshed at dispatch):

```console
$ gh api repos/DmitryNFomin/pg_wait_tracer/issues/947/comments --paginate --jq '.[] | {id: .id, body: .body}'
{"body":"**Owner decision, Q1 (captures recorded before coverage tracking), 2026-11-18:** \"a\"\n\nWhat (a) means, as put to the owner:\n- Every capture recorded before the coverage record ships shows \"coverage not recorded — capture predates coverage tracking\". This includes the 26 demo-rehearsal captures.\n- Their whole-window numbers are withheld, the same as for any window whose coverage cannot be confirmed.\n- No legacy exclusion from #947.\n- The demo captures are re-recorded before the tag.\n\nFollow-up: re-record the demo-rehearsal captures before the tag, as a scheduled item once the coverage label (U1/U2b) is in master.\n\nRefs #947\n","id":6980525535}
{"body":"**Coverage spec r2, Amendment A4 (2026-11-20): the evidence-v1 pilot route is outside the coverage label**\n\nThe U2b implementer stopped at spec §3 \"pilot_wait_metrics ANDs its evidence verdict\". Both design advisers answered (Astra, Fable): the rule is wrong as written and is amended, not implemented.\n\n- The pilot route reads only v5 evidence-v1 files; v2/v3/v4/v6 containers are filtered out before its codec. v5 files carry no COVERAGE records, and evidence-v1 carries its own life/coverage/loss verdict per (capture, pid).\n- The two certify different things on different clocks: pilot windows are monotonic ns, the coverage verdict is wall-mapped. ANDing would withhold every answer from a v5-only directory, and in a mixed directory would judge a monotonic window against wall boots.\n- **Rule:** the pilot response keeps its own shape (`pilot:true`, `contract:\"evidence-v1\"`, per-metric completeness and reasons) and carries no `fidelity` and no `coverage` key, on success and on `pilot_unsupported`. It bypasses the coverage emitter entirely.\n- **Tests:** a dispatch-level case with a real v5 fixture inside a restart directory (the response must equal the v5-alone response and carry neither key), plus two mutants: pilot added to the withhold list, and the coverage emitter re-enabled for the pilot.\n- **Stated limit:** with the pilot enabled, the Events page can show a pilot value beside coverage-withheld panels for the same window. It is a different quantity (one backend, one capture, its own certificate), not a second view of one number.\n- A stale web comment and two web tests that assume the server ANDs are left for a later web change.\n\nRefs #947\n","id":7033879421}
{"body":"**Coverage spec r2, Amendment A5 (2026-11-20): the Live window on a young capture, and when \"retention\" applies**\n\nFound by the U2b round-4 review on a real daemon: the web's Live mode always asks for [now − 15 min, now). On a capture younger than 15 minutes, and for 15 minutes after every daemon restart, that window starts before the boot's start S, so all ten whole-window views are withheld. The label also read \"(older than the retained data)\" on a fresh capture where nothing was removed. Both design advisers answered (Astra, Fable); they agree on the verdict and the direction, and differ only on where the clip lives. Resolved by the lead as below.\n\n1. **The verdict is unchanged.** An explicit window that starts before S stays incomplete (rows 23 and 24c stand). The request was wrong, not the verdict.\n2. **Live is a span resolved by the server.** A live request carries a span and no `from`; the server answers over [max(now − span, S of the live boot), live edge] and echoes the resolved bounds plus `requested_from_ns` and a clip cause (`capture_start` or `restart`). An explicit `from` is never moved. Reason for the server rather than the web: the web holds nanoseconds as JS doubles, and Fable's simulation showed that rounding its copy of S up to the next microsecond still lands below S in about 12% of cases; the server holds S exactly.\n3. **After a restart, Live starts at the current boot.** It shows the new boot's numbers only, never summed across boots, with the notice naming the restart and its time. An explicit window across the restart stays withheld (restart).\n4. **UI text:** \"Last 15 min — showing since capture start HH:MM:SS\", or \"… since daemon restart HH:MM:SS\" / \"… since PostgreSQL restart HH:MM:SS\". Rates, averages and AAS denominators use the resolved bounds, not the requested span.\n5. **\"Retention\" needs evidence of removal.** [boot_start, S) is `retention` only when the earliest retained file of the boot has file_no \u003e 0. When file 0 is present, that short startup stretch (daemon start to the first file's open) is `not_captured`. New verdict row: earliest file has file_no 0 and BOOT observation 5, boot_start 0: window [0,50) is incomplete [not_captured] u[0,5); [5,50) is complete. Row 13a keeps `retention`.\n6. **Gate:** the real-daemon live UI smoke opens the untouched default view on a young capture and asserts numbers on every whole-window tab plus the clip notice, and a second arm restarts the daemon and asserts the same within one refresh. Removing the server's clip must turn it red.\n\nRefs #947\n","id":7037950207}
```

CONTROLLING SCOPE DECISION: #947 comment 6980525535 "Their whole-window numbers are withheld, the same as for any window whose coverage cannot be confirmed."

## Starting point

Run scripts/lead/codex-task.sh on the EXISTING branch agent/947-coverage-server at 08b54616 (round 10). First run `git log --oneline -1`; it must print 08b54616. Otherwise print WRONG_HEAD <sha> and stop.

Commit with messages ending "Refs #947". Do not push. Do not run `make check` on the Mac. Change tests/ only. Your Linux machines, as root, already provisioned and used by round 10: EL8 <ip>, Ubuntu <ip>. rsync your tree and rebuild. Run ONE thing at a time on each machine: round 10's timing failure may be its own concurrent load.

## Task

1. **The stability walk must work when every pane is coverage-withheld.** On EL8 4.18 all eleven panes are withheld (the proven event-loss case), and the walker waits for numeric rows before its sweep, so its independent checks (page errors, leaks, missing tabs, blink) never ran and the live UI smoke reported FAIL on all eleven tabs. A pane that shows the coverage-withheld state is a settled pane: the walker treats "numbers present" or "withheld state present" as ready (by the state the UI actually renders, not by a timeout), then runs every independent check on it. A pane that shows neither still fails. The numeric assertions stay the only thing the exit 94 may cover, and only under the existing conditions (window incomplete with capture_loss AND this run's own drop counters nonzero, EL8 4.18 only).
2. **A real blink control under loss on EL8:** with loss present and the panes withheld, inject the existing blink stimulus; the walk must reach it, measure it and fail the gate with a non-94 exit (round 10: "no measured blink / no stimulus", inconclusive). And the positive arm: no stimulus, the walk passes and the run exits 94 only for the numeric default-Live checks.
3. **Ubuntu, `tests/test_coverage_consumers.py` timing (24-file AAS 143 ms against a 50 ms bound in round 10's full tier):** run it alone on the idle Ubuntu machine five times and report the five numbers. If it passes alone, say what else was running when it failed, with evidence. If it fails alone, report the numbers and stop on this item: do not change the bound, add a retry or a tolerance.
4. **Round 10's tier wrappers exited 2 on a generated `tests/results/overhead_trend.csv`.** Say whether `scripts/box-tier.sh` on this branch does that by itself (then it is a defect: find which change on this branch causes it, since master's CI box tiers pass) or whether it came from your own collection wrapper.
5. **Final full tiers** of the final tree on both machines with `scripts/box-tier.sh` exactly as CI runs it, one at a time per machine; paste both `Executed:` lines, both "Live UI smoke:" lines and the list of every failed or known-failing test by name. Expected: Ubuntu 0 failed with Live UI smoke PASS; EL8 0 failed, with the live UI smoke reporting that it ran and the scoped known-failing entries only.

## Excluded

src/, web/, contracts/, new KNOWN_FAILING entries, retries, tolerances, changed bounds, creating or deleting VMs.

## Done

Report: commit sha, `git diff --stat 08b54616`, items 2-4 with their numbers, both `Executed:` lines and live-UI lines of the FINAL tree (say the tree hash each tier ran), both registration guards, `git diff --check`.

## Budget

Rounds: 1
Wall-clock minutes: 240

## Report by

At the end of the budget, even if incomplete: at most 10 lines.
