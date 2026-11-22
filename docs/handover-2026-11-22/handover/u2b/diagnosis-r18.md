# diag18: the loaded default-Live UI failure, branch agent/947-coverage-server b15ec1d2 (tree ab8f457a)
Ran on EL8 <ip>, where the implementer's loaded runs ran (VERIFIED: /tmp/u2b-r18-ordered-repro.sh). /root/u2b-r18 was copied to /root/diag18. The build marker reads ab8f457a. The manifest matches `git ls-tree -rz b15ec1d2` (same md5). The only file that differs from the manifest is tests/results/overhead_trend.csv (VERIFIED).
Load was generated the same way: `pgbench -U postgres -d postgres -c 4 -T 3600` started 1 s before `bash tests/ui_live_smoke.sh --pg-version 18`, under the box flock, with the same environment (VERIFIED). Ubuntu was not used.
The harness diff records data and asserts nothing new: diag18-raw/diag18-harness.diff, plus patch_diag18.py and patch_diag18_v2.py.
- v1: per-clause tally of the combined guard on every rAF poll and every 100 ms; mount id/seq; pane-error presence; DOM snapshots at 10/30/59.8 s; console errors/warnings with location; timestamp of every websocket frame. v2 adds the same for the panel (numeric) wait plus every numeric_loss operand in its original short-circuit order; screenshot after the callback. Both return the original expression's value unchanged; side effect: one extra page.evaluate between waits (a few ms).

## Runs (fixed order chosen before any result; none repeated or dropped)
| run | load | exit | where it failed |
|---|---|---|---|
| A-L1 (v1) | yes | 1 | restart arm: "restarted daemon did not publish its first watermark within 15s". All 8 capture_start guards passed. Separate failure; cause NOT DETERMINED |
| A-Q1 (v1) | no | 94 | pass (known loss) |
| A-L2 (v1) | yes | 1 | Sessions: the guard passed in 0.6 s, then the 60 s panel wait timed out and numeric_loss returned false |
| A-Q2 (v1) | no | 1 | Overview, second call (6 s panel wait), same mechanism: the timeout came 0.02 s after the info reply and before the replies (+0.07 s) |
| A-L3 (v1) | yes | 1 | Sessions, same as A-L2 |
| B-L1 (v2) | yes | 1 | Queries: numeric_loss `notice_has_expected=false`, notice "21:50:01 – 22:05:01 UTC (15.0m)" |
| B-Q1 (v2) | no | 94 | pass |
| B-L2 (v2) | yes | 1 | Queries, same, notice "22:10:03 – 22:25:03 UTC (15.0m)" |
| B-Q2 (v2) | no | 94 | pass |
| B-L3 (v2) | yes | 1 | Queries, same, notice "22:30:05 – 22:45:05 UTC (15.0m)" |
Counts: loaded 6/6 exit 1 (5 on the notice clause, 1 on restart readiness); quiet 3 of 4 passed and 1 failed on the notice clause. All VERIFIED from the run logs and the JSON in diag18-raw/el8{,-v2}/.

## The clause
- **The combined Live/clip/mount guard never failed in any of the 10 runs.** All 5 clauses (mount id, live, 900, clip, notice prefix) reached ok=1 every time. Sessions passed in 0.3–0.9 s under load (VERIFIED: guard-*.json `ever`).
- The clause that fails is in `numeric_loss`, tests/ui_live_smoke.py:867–870: `expected_notice in notice`. `notice` is read once, at the instant the 60 s (or 6 s) panel wait expires.
- In all 3 v2 failures the other operands were true: loss_evidence, no_errors, clip_eq. `observe` was never reached because of short-circuit evaluation (VERIFIED).
- For A-L2, A-L3 and A-Q2 this is INFERRED from the failure snapshot (plain notice, resolvedLive=null) and frame timing.
- The implementer's two original failures share this signature, down to 72 replies for 74 requests, the same as A-L2 and A-L3. "First combined guard" was a misattribution: both waits raise the same "Timeout 60000ms exceeded" text (INFERRED).

## Cause (file:line, VERIFIED by reading)
Each Live tick calls `anchorLive` (web/static/app.js:1472) then `updateTimeRange` (app.js:1474). `anchorLive` sets `resolvedLive = null` (web/static/lib/state.js:164), so `liveWindowLabel` returns null (state.js:208) and the header falls back to the plain range (app.js:1191). The clipped label returns only when the refresh's first reply passes through `onWindowResolved`: the AAS reply (app.js:692) or the tab's reply (web/static/lib/view-manager.js:126). The panel wait starts just after a reply (when the guard passes), so its timeout lands at a phase set by the tick period. B-L1/L2/L3: numeric_loss read the notice 0.032 / 0.550 / 0.141 s after the info reply, before the Queries reply arrived (next reply 1.08 s later in B-L2). A-L2: timeout 0.77 s after the info reply. A-L3: 0.65 s after.

Measured under load (v2 panel waits, VERIFIED): tick period 5.20 → 5.99 s across tabs; tab reply latency median 0.19 → 0.96 s, max 1.45 s; plain-notice share of polls 3.6% → 15.9%; reply-to-mount ≈ 0 (−1 to −7 ms vs Python's frame timestamp). Quiet: tick about 5.05 s, latency 0.04–0.12 s, plain share 0.3–0.9%. Timeouts: 60 s (capture_start sweep), 6 s (later calls).

## What the user sees on the failing view (VERIFIED: DOM snapshots and screenshot, B-L1 Queries)
- At 10, 30 and 59.8 s the pane is correct and current. It shows the coverage-withheld card "Coverage unverified — whole-window metrics unavailable (capture loss) (not yet confirmed — live)" with loss intervals that advance every tick.
- Mount seq advanced 11 times in 60 s; no pane-error card or spinner; 0 console errors/warnings in all 10 runs. The header flickers once per tick (30 s snapshot: "21:49:31 – 22:04:31 UTC (15.0m)").
- The screenshot at timeout (diag18-raw/el8-v2/ui-load-1/guard-08-panel-queries-capture_start-at-timeout.png) shows the header "21:50:01 – 22:05:01 UTC (15.0m)" next to the badge "4.1m window" and an AAS chart marked "Incomplete capture" since 22:00:48.
- For 0.6–1.3 s of every ~6 s under load (about 0.05–0.1 s when quiet), the header claims a full 15-min window that contradicts the capture-start clip shown the rest of the time. It recovers by itself on every reply.

## Classification
- **PRODUCT DEFECT (transient, self-recovering), exposed by a phase-dependent harness read.**
- The product part: two views of one quantity disagree for up to about 1.3 s per tick (header range vs the "since capture start" clip and the 4.1m badge). This is RULE 2 / tag-criterion (b) territory: a contradictory label.
- It is introduced by this branch: origin/master 47370c9f has no `liveWindowLabel` and no `resolvedLive` in web/static. A `git grep` over web/ and tests/ for those names, `showing since` and `assert_default_live` returned nothing (VERIFIED).
- The harness part: numeric_loss samples the notice once, at ui_live_smoke.py:867. Whether it fails depends on where the timeout lands in the tick, so the gate is a phase lottery: about 16% per 60 s wait at the Queries load level, more as the tab sweep proceeds (INFERRED from the plain-notice share).
- If the product kept the resolved clip until the next reply, this gate would pass (INFERRED). Whether the flicker counts as a defect, rather than acceptable in-flight state, is a design call for the advisers.

## Master
The guard and the Live label code do not exist on master (git grep above, VERIFIED). No master build is on EL8: no /root/* built-from marker is origin/master's tree 380cd6f8 (VERIFIED); nothing was built.

## Not determined
A-L1's cause (restarted daemon, no watermark within 15 s under load; log saved at diag18-raw/el8/ui-load-1.daemon.log, not analysed); whether the original runs failed for exactly this reason (INFERRED only, their replies have no timestamps); the exact failure probability per wait.

Cleanup: /root/diag18 and this run's /tmp/pgwt_ui_live* and pgwt-default-live* files were removed on EL8. No pgbench, daemon or Chromium process remains and the lock is free (VERIFIED). Ubuntu was never touched.
