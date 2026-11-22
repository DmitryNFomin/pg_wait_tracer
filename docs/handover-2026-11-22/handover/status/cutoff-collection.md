# Cutoff collection (collected 2026-11-22 ~00:11-01:20 UTC). Raw copies: SP/status/cutoff-raw/<machine>/ (u2b-el8, u2b-ubuntu, p1b-el8, p1b-ubuntu)
Trees: git rev-parse e15a832e^{tree} = 9b7e7fefda44c9bf842d82a05c6bcc36a56c9923; 39f159d2^{tree} = b9dd6267b2fb262f2fb412ec650aa9357e16f925.

## Job 1 u2b round 19 (e15a832e). NOT finished: EL8 orchestrator alive at 01:20 (pid 1067381 el8-runs.sh; process count 2 by ps)
- EL8 orchestrator (u2b-el8/el8-orchestrator.log): finished diagnostic-load-1..3, diagnostic-quiet-1..3, final-load-1..6; RUNNING final-quiet-1 (start 2026-11-22T01:09:31Z); never started final-quiet-2..4; no ALL_RUNS_COMPLETE, no runs-complete file; tier-el8.log does not exist (EL8 tier never started).
- Last lines: "RUN END final-load-6 exit=1 ...T01:09:31Z" / "RUN START final-quiet-1 2026-11-22T01:09:31Z".
- EL8 runs are on tree 9b7e7fef = e15a832e tree (u2b-el8/final-tree). Diagnostic runs are on the private copy /root/u2b-r19-diag (patched gate, u2b-el8/readiness-diagnostic.diff).
### Restart-readiness (u2b-el8/readiness-table.txt; polls verbatim last 3: u2b-el8/last3-polls.txt)
run | exit | first status s | first new boot s | watermark>start s | polls
load-1 | 1 | 0.956091 | 0.956091 | never in 120 s | 2370
load-2 | 1 | 0.966411 | 0.966411 | never in 120 s | 2370
load-3 | 1 | 1.058315 | 1.058315 | never in 120 s | 2371
quiet-1 | 94 | 0.909846 | 0.909846 | 3.897503 | 78
quiet-2 | 94 | 0.858307 | 0.858307 | 2.882626 | 58
quiet-3 | 94 | 0.957102 | 0.957102 | 2.930842 | 59
- Loaded runs fail with "restarted daemon not ready within diagnostic 120s: missing watermark_wall > boot_start_wall; last_exception=None" (diagnostic-load-N.log). Last poll of load-1 elapsed 119.97 s: watermark_wall == boot_start_wall (1795302827681386948).
- Quiet exit 94 = "COVERAGE-NUMERIC excused windows=17/8/10 exit=94" (quiet-1/2/3 logs).
### Reproductions with committed gate (u2b-el8/reproduction-table.txt)
final-load-1..6: exit 1 each (final-load-N.exit). Clause in 5 of 6 (1,3,4,5,6): "FAIL default Live window: restarted daemon not ready within 15s: missing watermark_wall > boot_start_wall; last_exception=None; coverage={... watermark_wall == boot_start_wall ...}" (final-load-N.log).
final-load-2 differs: DEFAULT-LIVE-FAILURE (snapshot clip=capture_start, verdict=incomplete, tab=matrix) then playwright TimeoutError Page.wait_for_function 60000ms at ui_live_smoke.py line 884 assert_default_live; no "FAIL default Live window" line; a MemoryError also appears in the log (final-load-2.log lines ~59, ~96).
Observer: violations=0 in all six (waits/records: 1:16/267, 2:14/254, 3:16/265, 4:16/266, 5:16/265, 6:16/265). Quiet: final-quiet-1 pending, observer_waits=32 records=373 violations=0 so far; quiet-2..4 not run.
### Tiers
EL8: not started (tier-el8.log absent; queued after ALL_RUNS_COMPLETE).
Ubuntu, PRELIMINARY tier on the EARLIER tree f62e80336543d2f6912cf46ac36f840a9170f3e7 (does NOT match e15a832e tree): u2b-ubuntu/tier-ubuntu-preliminary.log
 - "Executed: 243 (passed 241, failed 1, refused 0, known-failing 1 (1 xpass)), skipped 5, excluded 1, total 249" (line 20030); "Live UI smoke: overall=PASS" (line 20025)
 - RUN_ALL_SUMMARY {excluded 1, executed 243, failed 1, kernel 6.8, known_failing [test_init_hint_recovery issue 959 xpass], ok false, os ubuntu, passed 241, skipped 5, status FAIL, total 249, xpass 1}; U2B_R19_TIER_EXIT 1; wall 7221 s
 - known-failing/xpass: test_init_hint_recovery (#959, UNEXPECTED PASS). Failed test not named by the summary; the only unittest failure in the log is test_ui_live_smoke_failure.py ::test_default_live_replies_survive_large_output_without_masking_failure (TypeError, Path(None), lines 6185-6212): UNCONFIRMED that this is the counted failure.
 - U2B_R19_SOURCE_START f62e8033..., U2B_R19_SOURCE_END fcf5d9a494162ea00b91ced8a3d830ef95d8b6f5 (differ; source end hash not equal to start).
Ubuntu, FINAL tier on tree 9b7e7fef (= e15a832e tree, matches): u2b-ubuntu/tier-ubuntu.log header "box-check: commit=b15ec1d2 tree-hash=9b7e7fef... built-from=9b7e7fef..."; STILL RUNNING at 01:20 (box-tier process present), no Executed:/Live UI smoke/RUN_ALL_SUMMARY yet; tier-ubuntu/exit and mock-full.exit absent.

## Job 2 p1b round P1b (39f159d2). NOTHING RUNNING on either machine at 00:11 (no qualify/test process; load avg 0.00 EL8, 0.02 Ubuntu). Qualifiers died after 23:31.
- Final tree check: final-source.hash on both machines = b9dd6267b2fb262f2fb412ec650aa9357e16f925 = 39f159d2^{tree}: MATCH (p1b-el8/final-source.hash, p1b-ubuntu/final-source.hash). Intermediate tree 6837a6cb4687... ran 10 live reps earlier, tiers cancelled (state.json); kept apart, not used below.
- Matrices on final tree, both machines: PRODUCTION loads=48 rejected=0 (3 modes x 16 verdicts); FALSIFIER loads=48 rejected=0 (matrices.log; qualification logs). lifecycle-guard exit 0, build exit 0.
- Flag-off: "DEFAULT-OFF: 54/54 complete wire/raw records byte-identical to 5fd6a519" both machines (default-off.log).
- Live arms (final tree), tests/test_discovery_producer_live.py, each run PASS line "DISCOVERY-LIVE PASS":
  EL8 (phase probes 0): PG13 3/10 (1,2,3; run 3 log has PASS but no RESULT line in driver log, killed after), PG17 2/10, PG18 2/10, 0 failures; runs 4-10 never run (17-3 never started).
  Ubuntu (phase 1): PG13 4/10 (1-4; run 4 log has PASS, no RESULT line), PG17 3/10, PG18 3/10, 0 failures; rest never run.
  Role arms (live-roles.py 13 / 18 / 13 reuse): EL8 2/2/2 exit 0; Ubuntu 3/3/3 exit 0. (el8/ubuntu-final-qualification.log; p1b-*/roles-*.log)
- Full tier (box-tier.sh) on final tree: NEVER RUN on either machine (no full-tier.log; no Executed: or "Live UI smoke:" line). SOURCE-AFTER check and QUALIFICATION-COMPLETE never printed.
- Item 3 never run: live arms runs 4-10 (EL8) / 5-10 (Ubuntu), roles reps 3-10 (EL8)/4-10 (Ubuntu), full tier both machines, post-tier source check. Mac host suites/guards: local files only (host-checks.log), not re-verified here.
- Earlier evidence kept apart: EL8 original-guard reproductions failed at iterations 5 and 56 (classification.md); Ubuntu attribution 32 runs no failures (state.json).
