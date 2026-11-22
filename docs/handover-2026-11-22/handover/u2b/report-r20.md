# U2b round 20 report (INCOMPLETE: handback enforced at ~1h50m of the 420-minute budget)

Commits (git log --oneline e15a832e..HEAD), branch agent/947-coverage-server, worktree impl-u2b, not pushed:
- a823b498 Walk trace files by block header in the loss witness
- 9027ceec Restart step asserts S1-S5 instead of one watermark condition
- daec65b3 Name a missing replies side file instead of Path(None)
- d1842270 Name S3 when no reply to a post-readiness Live request arrives
Final tree (HEAD^{tree}) = 3e8524d1ce7f734b6829508c720414f29bde82e1.
git diff --stat e15a832e: coverage_loss.py 31, test_coverage_live.py 95, test_ui_live_smoke_failure.py 5,
test_ui_live_smoke_lib.py 162(+), test_ui_live_window.py 2, ui_live_smoke.py ~210; about 440 insertions and 80 deletions.

## Item 1: S1-S5 (tests/ui_live_smoke.py)
- S1/S2: run_default_live_gate.restart(), using restart_readiness(cov, old_boot, gap_logged) and new_boot_gap_logged(log, offset, boot).
  One 15 s deadline from the replacement's start. S2 holds when watermark_wall > boot_start_wall or a complete COVERAGE-GAP line
  written after the replacement started carries the new boot id. The failure names S1 or S2.
  restart() now also waits, within the existing 15 s stop deadline, for the old daemon to exit (gone or zombie). The old daemon
  prints its summary after unlinking its socket (daemon.c ordering), so S5 attributes the line by log offset.
  restart() returns {boot, boot_start_wall, s2 branch, timings} and prints RESTART-READY.
- S3: check_restart_first_reply(fresh, boot_start_wall, loss_reply): clip == restart, int(from_ns_exact) >= boot_start_wall, and
  rows or loss.observe. A timeout waiting for that first reply also names S3 (commit d1842270). The 6 s deadline is unchanged.
- S4: in default_live_window_gate, when the reply was accepted by the witness without rows, the gate waits for
  #table-container .coverage-unavailable and a notice containing "daemon restart". Otherwise it fails naming S4.
- S5: teardown() → check_replacement_summary(text): SIGTERM the replacement, then require exactly one "INFO: coverage:" line after
  the offset, with ticks > 0, write_failures == 0, and not (good >= 3 and promotions == 0). It prints RESTART-SUMMARY and runs
  before loss.exit_code, so an S5 failure turns the accepted loss result (94) into exit 1.
- Unit tests in tests/test_ui_live_smoke_lib.py: test_restart_readiness_sentences_s1_s2, test_restart_first_reply_sentence_s3,
  test_replacement_summary_sentence_s5, test_restart_loss_branch_sentences_s4_s5 (94 path; S4 red; S5 red after 94; S3 from<start;
  S3 no reply). Mac: "Ran 469, passed 469, failed 0".

## Item 2: controls. Both were run on tree 7d7301c6 (HEAD daec65b3). Re-running them on the final tree was started and then stopped (see below).
- Ubuntu, quiet, replacement with PGWT_TEST_COVERAGE_YIELD=1: exit 1 at S2 as expected:
  "restart sentence did not hold: S2: ...; coverage={... 'boot_start_wall': 1795342296075109102, 'watermark_wall': 1795342296075109102, 'paused': False ...}"
- EL8 under load with YIELD: exit 1. S2 held via a gap line (RESTART-READY s1 0.911 s, s2 "gap" 1.936 s). The run then failed on a bare
  "Timeout 6000ms exceeded while waiting for event framereceived", meaning no reply to a post-readiness time_model request within
  6 s. That failure now names S3 (d1842270). S5 was never reached.
  HOLE (as the adviser predicted): the replacement's summary line under load was "ticks=8 good=0 heartbeats=8 gaps=8 ... promotions=0".
  Under load every tick is a gap, so YIELD leaves good = 0 and S5 cannot see it. Had the first reply arrived in time, this control
  would most likely have ended as the accepted loss result (94) rather than red. That last point is INFERRED: not run to completion.
  Logs: EL8 /tmp/u2b-r20-evidence/tree-7d7301c6/control-load-yield.{log,key,exit}; daemon log /tmp/pgwt_ui_live_daemon_WdL1dk.log lines 621 and 705.

## Item 3: bounded memory
coverage_records streams block headers and reads payloads per block; raw_boots reads 36 header bytes and takes extents from the
same walk.
- Same answers: sha256 is identical old vs new on three saved EL8 traces (pgwt_ui_live_dTONZQ 2 boots/473 MB, e03SpC 3 boots,
  7cCFfg 1 boot). Peak RSS of the answer script: 745,340 kB old, 24,900 kB new.
  Files: EL8 /tmp/u2b-r20-evidence/answers-{old,new}.json.
- Loaded run "before" (e15a832e harness, EL8, 4 pgbench clients): gate process VmHWM 761,596 kB, and MemoryError again at
  coverage_loss.py:267 raw_boots, exit 1 (before-load.{log,mem.json}).
- Loaded run "after": NOT MEASURED (the final-tree loaded runs were stopped).

## Item 4
The preliminary Ubuntu tier's one counted failure was test_ui_live_smoke_failure.py (FAILED (errors=1), TypeError Path(None)); the
other FAIL lines in that log are intended controls. It came from an intermediate tree, f62e8033, that never became a commit. On
e15a832e and on my tree the test passes (r19 final tiers on 9b7e7fef: both "Ran 12 ... OK"). The test was wrong: it turned a
recorder failure into a TypeError. It now asserts that the side-file path is not None. A mutant that removes the page-unavailable
guard in _record_default_live_failure now fails with "unexpectedly None : replies side file not written without a page"
(/private/tmp/u2b-r20-evidence/item4/mutant.log).

## Items 5 and 6: NOT DONE on the final tree
The final-tree orchestrators (EL8 el8-r20b.sh: build, control, 6 loaded, 4 quiet, tier; Ubuntu ubuntu-r20.sh: build, control,
mock tier, box tier) had just started when handback was enforced. I stopped them (process groups killed, ps shows nothing
left). To resume: sync with /private/tmp/u2b-r20-evidence/scripts/sync.sh <host> <worktree>, then
`nohup setsid bash /tmp/u2b-r20-evidence/{el8-r20b,ubuntu-r20}.sh` on each machine.
Mac on tree 7d7301c6 (only the ui_live_smoke.py message and one lib test changed after it):
- node --test tests/web_unit/*.test.mjs: tests 1002, pass 1002, fail 0.
- test_block_type_sites PASS; test-registration and runner-registration guards OK; git diff --check clean.
Mock tier: tests/test_ui_live_window.py --browser. "PASS clipped Live tick" holds on the Mac, but the script exits 1 at its built
C mutant case (observer SmokeFailure instead of PWTimeoutError). The same failure occurs on base e15a832e on the Mac. On Ubuntu
the same script exits 0 (tree 7d7301c6, mock-live-window.exit; r19 e15a832e also 0).
Earlier results kept apart (tree 9b7e7fef = e15a832e, from r19, collected this round):
- Ubuntu tier: Executed 243 (passed 242, failed 0), Live UI smoke PASS.
- EL8 tier: Executed 243 (passed 232, failed 1, refused 4, known-failing 6 (2 xpass)), Live UI smoke PASS. The failed test is
  test_capture_smoke_full (Mode C qattr Lock:relation 1378 ms < 4000, ringbuf_drops_total=1, issue #128 assertion; expected
  exit 94, got 1).
- EL8 final-quiet 1-4: exit 94.
Raw copies: /private/tmp/u2b-r20-evidence/r19-late/.

## Item 7 (report only)
SEARCHED: gh issue list --search "status coverage loss" / "last loss" / "status reply gap" / "legend" / "overlapping labels" /
"chart label overlap"; issue 473.
NOT SEARCHED: issue comments.
No issue covers status-reply loss fields.
- Status reply fields needed:
  - per boot: ticks, good_ticks, gaps (DROP and COUNTERS_UNKNOWN separately), promotions;
  - last_loss_wall (detect time of the latest closed gap) and last_loss_components (the deltas);
  - watermark_age_ns (now minus W).
  Readers: the restart readiness gate (S2 without the test-only stderr), the web header (to state "losing events since HH:MM"
  instead of a silent frozen W), and the loss witness (independent totals). This is a contract change, excluded here.
- Display finding 1: in r18 screenshot guard-08-panel-queries-capture_start-at-timeout.png the chart legend shows
  22:00:48–22:04:54 (capture loss) while the pane card shows 22:00:48–22:04:55. Their not_yet_covered ranges differ too
  (54.938–55.449 vs 55.936–56.780). These are two replies about a second apart for one window. No issue was found.
- Display finding 2: the top-right labels "Incomplete capture" / "Full" / "4 CPUs" overlap in the same screenshot. This recurs from
  closed issue 473 item 1 (closed 2026-10-26).
- Also visible there: the header pill says "4.1m window" while the range label says "21:50:01 – 22:05:01 (15.0m)". UNVERIFIED on
  the current tree.

## Scope questions
1. Under EL8 load, S5 cannot catch a YIELD-style frozen watermark: good = 0 because every tick is a gap. Is a stronger sentence wanted
   (e.g. promotions > 0 required when any tick is good, or a status-reply field)? The latter is a contract change.
2. The existing 6 s first-reply deadline was missed under EL8 load in the control. If the loaded final runs hit it too, they will exit 1
   at S3. Deadlines are excluded from my scope.
3. The Mac-only failure of the C-mutant case in test_ui_live_window.py --browser is pre-existing on e15a832e. Should it be filed?
