# Question: the live view on a young capture, the false "retention" label, and an idle exact_perf watermark

Read-only. Answer in at most 40 lines; mark every claim VERIFIED (file:line read) or INFERRED. scratchpad = SP

## Part 1: the default Live window (U2b blocker)
Branch agent/947-coverage-server at fb43c3c6, worktree scratchpad/wt-u2b4. Review: scratchpad/u2b/review-fb43c3c6.md (B1). Spec: scratchpad/m871/spec-coverage-label-r2.md (§2 states; rows 23, 24; Amendments A1-A3).
- The web's Live mode always requests [now − 900 s, now) (web/static/lib/state.js:101, :137, per the review). On a capture younger than 15 minutes, and for 15 minutes after every daemon restart, that window starts before the boot's S, so the verdict is incomplete and all 10 whole-window routes are withheld; AAS/heatmap keep only covered buckets.
- The test that caught this on Linux (tests/ui_live_smoke.py) was changed in round 4 to request only covered time, so the gate no longer sees what a user sees.
Questions:
1. Is the verdict right and the request wrong (fix in the web: anchor Live at max(now − span, S) using info's from_ns, rounded up to the next microsecond because the web holds ns as doubles), or should the server resolve a live span itself (a spec amendment)? Which keeps "one verdict in one place" (#871 comment 6935035590) and what must the UI say about the clipped part ("since capture start HH:MM:SS")?
2. After a daemon restart, should Live stop at the current boot (so it shows the new boot's numbers), or keep spanning the restart (withheld, restart)? Give the user-facing consequence of each.
3. Which gate must go red on today's behaviour, so ui_live_smoke cannot hide it again?

## Part 2: the false "retention" reason
On a fresh capture where nothing was removed, the label reads "(older than the retained data)". The review attributes it to the gap between the daemon's start time (boot_start) and its first BOOT record's observation (src/open_snapshot.c:605; spec §2: S = max(boot_start, BOOT observation of the earliest retained file); A1 item 5). Is that a writer defect (boot_start should equal the first BOOT observation, or the BOOT record should be written at capture start), a reader defect (retention only when an earlier file of the boot is actually missing), or a spec gap? Name the smallest correct fix and its test.

## Part 3: an idle exact_perf capture (transport branch)
Branch agent/933-tickfix at ecd3f611 (clone /private/tmp/pgwt-codex/agent%2F933-tickfix, `git show HEAD:<path>`). On EL8 its live test's pre-existing arm (tests/test_exact_perf_coverage_live.py:240, an idle capture with one idle psql and no load) failed "startup frontier advances even while uncovered": W never moved past boot_start. Log: scratchpad/a2go/tcov/g1/el8-ecd3f611.log around line 17972-18000. The SIGKILL arm under pgbench passed with W advancing.
4. From the code: does the merge frontier (coverage_cursor) advance at all on an idle box under exact_perf? The owner chose a demand-driven helper ("nothing is woken on an idle box with nothing waiting", #871 comment 6848477572). If W cannot advance when idle, every quiet period reads not_yet_covered/unclean_end. What is the smallest correct way to certify idle time (for example a boundary round on each 1 s coverage tick), and does it conflict with the owner's idle-cost decision (≤ 1% of a core per helper, provisional)?
