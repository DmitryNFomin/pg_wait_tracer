# r13 diagnosis: pure-CPU capture_loss (A) and Mode B missing row (B)
Tree b291215c (tree 9904545b = VM /root/u2b-r13, md5 of open_snapshot.c and test_capture_smoke.py match). Probes and raw output: `diag13/`.
Branch base = merge-base 4059fcde (#981). origin/master (346ccc95) is AHEAD of that base, so `git diff origin/master` shows master-only work as removals; branch-own changes below use `git diff 4059fcde HEAD`.

## Failure A
**1. What opens a capture_loss gap.** VERIFIED (read). The only GAP_OPEN writer is `cov_gap` (src/open_snapshot.c:352), called only from `cov_counters` (:376), which compares the tick's 15-component counter vector with its base:
- the vector is UNKNOWN (:382): any component unreadable (`pgwt_cov_cv_compare`, :309), or `consume_failed` (:639), or `init_state_read_fail_total != 0`, which marks the INIT component invalid (src/counter_vector.c:56). Reason COUNTERS_UNKNOWN.
- the vector becomes known again, or is known for the first time if the base was unreadable at start (:395). Reason COUNTERS_UNKNOWN.
- any component differs from the base (:412). Reason DROP. The gap opens and closes on the same tick.
- Components (src/counter_vector.h:22-38, sources counter_vector.c:40-63): lifecycle_fork_drop, lifecycle_init_drop (`lifecycle_init_lost_total`, src/backend.c:1318), lifecycle_exit_drop, wait_read_fail, query_text_drop, pg13_query_text_drop, exact_query/cmd/phase_update_fail, accum_update_fail, fork_state_update_fail, **event_ring_drop**, state_map_update_fail (+ userspace `state_map_full_total`, backend.c:132, sampler.c:773), seen_query_ids_update_fail, writer_push_fail.
- The coverage reader turns every such GAP into BAD_TAINT, which is reported as capture_loss (src/coverage.c:406-407, 490).
- `ringbuf_drops_total` is only event_ring_drop (src/control.c:846), and it counts only `emit_event` failures (src/bpf/pg_wait_tracer.bpf.c:292-302). Lifecycle and query-text ring failures go to `ringbuf_site_drops` alone (bpf.c:765, 803, 918, 1070, 1251). The other 14 components are not `ringbuf_drops_total`. The test and `coverage_loss.loss_signature` read only that one counter (tests/test_capture_smoke.py:2478-2488, tests/coverage_loss.py:23-28).

**2. Why the range is ~15 s.** VERIFIED (code + records).
- The taint runs from the floor_db of the last WATERMARK with W <= gap start to the paired GAP_CLOSE (src/coverage.c:316-353).
- The gap start is the last written W (open_snapshot.c:414). GAP_CLOSE is the detecting tick's `publish_from_ns` (:416).
- floor_db is the oldest `last_ts` of any non-idle backend (open_snapshot.c:287-306, gathered :682-712). The waitless hog never transitions, so floor_db stays at boot_start for the whole run. Every WATERMARK in a probe trace has b = boot_start (diag13/covdump_samples.txt, and r02 in out3: a-b grows 0 to 9 s and b never moves).
- So one bad tick taints [boot_start, P_detect]. The server clips it to the request: 343.83 is the test's `win_from`, and 358.83 is P of the one non-good tick.

**3. Which condition fired in r13.** The branch is determined, the component is not (only reproduced).
- VERIFIED from the log line `ticks=35 good=34 heartbeats=2 gaps=1`: exactly one tick was non-good, and gaps=1.
- INFERRED from the code: an UNKNOWN episode costs at least two non-good ticks (:382, then :395). A start-of-run unknown base would close near attach (~341.8), not at 358.8. So it was CHANGED/DROP (:412) on a component other than event_ring_drop, which the test read 0 -> 0 in 289 in-window samples.
- NOT DETERMINED for r13 itself. The test deleted the trace (test_capture_smoke.py:2547). Only 300 bytes of stderr survive. GAP_OPEN stores only reason=DROP, and no component is logged (open_snapshot.c:412-419).
- Reproduced on <ip> with the same binary:
  - Isolated: the phase's tracer argv, full metrics every 0.1 s (purecpu_probe.py; out2_summary.txt, out1). 21/21 were complete with gaps=0.
  - Suite order: the test's own `phase_cpu_straddle` then `phase_pure_cpu_straddle`, 20 rounds (phase_probe.py, analyze3.py; out3_*, out4_*). 10 pure-CPU runs had a DROP gap.
    - 9 were event-ring drops (`ringbuf_wait_transition_drops_total` 0->1 or more).
    - **out4 round 7 is the r13 signature**, VERIFIED (out4_log.txt, r07_failureA_repro.txt). Both checks FAIL, ringbuf 0->0, `ticks=35 good=34 heartbeats=2 gaps=1 promotions=33`, and the uncovered range runs from win_from to 840.88 s mid-run. The only vector component that moved is `lifecycle_init_lost_total` 0->1 (`no_opportunity` 0->1, `lifecycle_init_lost_untraced_ns_total` 0->2233189). It moved in one 0.1 s read [840.18,840.29], while `lifecycle_events_total` went 3->7.
    - out4 round 5 is a second shape that also fails the test, VERIFIED. A wait-transition ring drop was detected only by the SEAL tick (GAP_OPEN a = W_final). So uncovered = [win_from, to_ns]. The 0->1 first appears between reads ending 745.03 and starting 745.13, both later than `upper` = to_ns = W_final (743.90). So `window_loss` drops it (test_capture_smoke.py:2481-2483), yet the floor rule withholds the whole window.
- r13's component is most likely lifecycle_init_lost as in round 7 (INFERRED). Naming it would require the component at GAP_OPEN, or the test keeping every sample and the trace on failure.

**4. Real loss or over-withholding?**
- Measurement (round 7): one process's INIT was never handled before it exited, with an untraced lifetime of 2.23 ms. The hog's own counters show no loss. In r13 we know only that one non-ring component changed.
- Interpretation (INFERRED):
  - Something real was unknowable: a backend whose whole lifetime is untraced may have waited. The capture_loss label is therefore backed by a counted loss, and the test watches too narrow a set. It accepts only `ringbuf_drops_total`, and only reads inside the window, which round 5 shows also misses a loss caught at SEAL.
  - The withheld extent is not supported by any loss. 2.23 ms lost became 21 s (round 7) or 15 s (r13) uncovered, because the floor rule (Q2) anchors the taint at boot_start while the hog runs. Whether that is over-withholding or the intended conservative contract is a design question for advisers/owner. master already has an unused pid/lifetime-bounded `pgwt_writer_coverage_untraced_lifetime` (master event_writer.c:1684).
  - The verdict is capture-wide, so pid-filtered hog numbers are withheld for another process's loss.
- The first FAIL (`cpu_ms = 0`) is the same withholding: time_model is COV_WITHHOLD (src/server.c:5276-5280). It is not a separate defect (INFERRED).

**5. Independent of the branch?**
- The gap condition is identical on master. VERIFIED: the branch's own diff to open_snapshot.c (`git diff 4059fcde HEAD`) is 7 lines of bookkeeping (gap_start_ns/gap_reason) in `cov_gap`. counter_vector.c, capture_counters.c, backend.c and daemon.c are untouched. Master since the base changed only coverage_record.h, event_reader.c, event_writer.c/.h and server.c (comment only). The new writers `..._untraced_lifetime` and `..._a2_bucket` have no callers on master (git grep).
- The consequence is branch-new. VERIFIED: master's server has no coverage verdict (no src/coverage_trace.c, no capture_loss, master server.c:2888-2889 skips COVERAGE blocks as non-data). Master writes the same GAP but withholds nothing.

What would have to change (A), in both test and product. Test: accept any counter-vector component as the witness, including a SEAL-tick loss read after to_ns, and keep the trace on failure. Product: name the moved component in the GAP record or log. Whether a ms-scale INIT loss may taint [boot_start, P] is an adviser/owner design question, not a test fix.

## Failure B
**6. The missing row is the one dropped ring record.** VERIFIED (diag13/b_session_timeline.txt, b_trace_findings.txt).
- The missing row is query 2055386814451594937, "SELECT pg_sleep(2), 1;" (sleeper2, pid 430789). The CLI printed -8886085649470066503 (= 9560658424239485113 signed, the pg_sleep(3)) and 1557402608523615666 (pg_sleep(2) with the other shape).
- In the trace, pid 430789 has CPU ending at 602.768997 and then nothing until CPU starting at 604.771090525. It has no PgSleep record (12 records). sleeper3 (430790) has PgSleep s=602.768997 d=2002 ms, ending at 604.771090425, 100 ns earlier.
- A transition record carries the closing interval (old_event, duration; bpf.c:646-658), so the lost record is the WAIT-END (PgSleep -> running) record of 430789.
- The metrics show exactly one drop at that site (`ringbuf_wait_transition_drops_total=1`). GAP_OPEN reason=2 lies between W=37412.42 and the detecting tick at 37414.42 mono, which contains the drop at about 37413.97 mono.
- Mechanism: two backends woke within 100 ns and lost NMI-context reservation contention (bpf.c:288-291 comment). INFERRED; the comment names it, and this run did not observe it.

**7. Does the CLI tell the user?** VERIFIED (log 18326-18352 + grep).
- stdout has no loss notice. The query_event printer (src/output.c:1093-) prints only the header and rows.
- stderr's only related line is the shutdown summary `INFO: coverage: ... gaps=1` (open_snapshot.c:776). It names neither drops nor dropped events.
- Drop counts exist only on the control socket `metrics` (control.c:846; ringbuf_metrics.h:10).
- A grep of fprintf/printf for drop|lost|loss in output.c, daemon.c, pg_wait_tracer.c, event_stream.c and backend.c finds only the test-only hint, INIT-LOST (backend.c:1327) and a PG13 text-queue warning.
- Same on origin/master: the same grep on `git show origin/master:` returns nothing new, and master has the same summary line.

**8. Is it reachable on master unchanged?**
- VERIFIED: the branch's own diff touches none of daemon.c, backend.c, event_stream.c, src/bpf/ or the accumulator. In output.c it adds only `replay_idle_unverified` branches (set only in src/replay.c:188), and none of them is in `pgwt_print_query_event`. replay.c is replay-only.
- INFERRED: the failure is therefore reachable on master. The branch's test differs (it adds `--trace-dir` and a control-socket readiness wait). Whether writing the trace changes the drop rate is NOT DETERMINED. Comparing the drop rate of Mode B with and without `--trace-dir` on this VM would settle it.
- test_query_event has no exit-94 path in KNOWN_FAILING (tests/run_all.sh:184-192).

What would have to change (B): the product, to recover or report a lost wait-end record (#871 family), and to show the live CLI user that the interval lost events. Alternatively, an owner decision lets the test accept the #871 witness, which is a test change.
