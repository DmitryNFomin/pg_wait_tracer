# measure-r13 (files in .../scratchpad/u2b/meas13/; paths below relative to it)
Tree b291215c at /root/u2b-r13 on both VMs (md5 test_capture_smoke.py 58cbb054..., open_snapshot.c 16305489..., test_query_event.py 07dd59fa... all equal the worktree's; Ubuntu tree verified by md5). Binaries unchanged, no rebuild.

## Measurement 1 (EL8 <ip>, PG18, kernel 4.18): test_query_event, whole test x 40 per arm
Alternated X01,Y01,X02,Y02... by /root/meas13_runq.sh (copy: runq.sh). One run ~65 s. Per-run logs: el8/X_NN.log, el8/Y_NN.log (each ends "EXIT=rc"); aggregation: agg.py -> el8_summary.txt.
- Arm X = branch test (with --trace-dir + readiness wait), run from /root/u2b-r13.
- Arm Y = merge-base 4059fcde tests/test_query_event.py (saved as test_query_event_base.py), placed in scratch tree /root/meas13/y: copy of the tree's tests/ dir (rsync, no results) with only test_query_event.py replaced by the base version, symlinks to the same pg_wait_tracer and pgwt-server. The base file imports only testutil and test_capture_smoke (branch versions, same as arm X); no helpers taken from 4059fcde.
| arm | runs | void | Mode B row-count FAIL ("parsed 2 entries") | other failures | drops where known |
| X | 40 | 0 | 4 (runs 05,19,22,26) | 0 | all 4 failing: ringbuf_drops_total = wait_transition drops = 2,1,3,1 |
| Y | 40 | 0 | 7 (runs 09,10,13,23,28,31,32) | 0 | NOT OBTAINABLE |
Every failure in both arms had rows=2 (needs >=3); all other checks passed in all 80 runs (exit code was 1 only for the Mode B check).
- Drops: arm X prints MODE-B-EVIDENCE only when it has <3 rows, so counts for the 36 passing X runs are unknown (not measured). Arm Y has no --trace-dir, hence no control socket, so no metrics; I did not add an observer/wrapper since that would change the test.
- X failures also report coverage verdict incomplete, capture_loss (X_05 shown in log).
## Measurement 2 (Ubuntu <ip>, kernel 6.8.0-139, PG18 postmaster 960): phase_probe.py 20 rounds
Run as: python3 -B phase_probe.py /root/u2b-r13 /root/meas13_u/out 20 960 (my guess at the invocation; the diagnosis does not record it). Files: ubuntu/log.txt, ubuntu/metrics.jsonl, ubuntu/analysis.txt (analyze3.py output), scripts phase_probe.py analyze3.py covdump.py.
| rounds run | rounds with a gap | pure-CPU runs w/ GAP record | loss-like counters moved | failed checks |
| 20 | 0 | 0 | none in any of 40 traced dirs (20 straddle + 20 pure-CPU) | 0 (20/20 failed_checks_delta=0; 20/20 "AAS window is fully confirmed") |
No gap component or uncovered range exists to report. Each trace has only the BOOT record (ubuntu/analysis.txt).
Kernel difference is not the only difference between the VMs (different CPU, distro, load); I did not isolate it.

## Measured vs inferred
- Measured: counts above. Y fails at least as often as X (7/40 vs 4/40); the difference is not tested for significance, but the failure is not caused by the branch's test changes, since Y fails without --trace-dir.
- Inferred only: that Y's failures are also single dropped ring events (Y has no witness; same signature rows=2 only).
- Not done: no VM created/deleted/rebooted; no files in repos changed. Scratch dirs on VMs: /root/meas13, /root/meas13_u, /root/meas13_runq.sh.
