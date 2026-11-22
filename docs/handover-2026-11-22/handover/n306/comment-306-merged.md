**306-a is on master (PR 1004, squash 7815defb, merged 2026-11-22 04:06 UTC)**

What landed: the sampler at nice -10 by default; a startup warning when the tracer and PostgreSQL are in different CPU groups; `test_cross_validate_tiered` run in the deployment layout (tracer as a plain system unit, pgbench outside, the layout and the nice value read back; a mismatch is exit 80).

The PR's own CI was the first run of the new test on the real gate boxes, started by the Actions runner (run 44972248327):

| Box tier | Runner | 10 Hz | 50 Hz | 100 Hz | 200 Hz | Tier |
|---|---|---|---|---|---|---|
| Ubuntu | pgwt-gate-2 | 2.34 | 3.51 | 2.94 | 3.01 | 238 executed, 0 failed, live UI PASS |
| EL8 | pgwt-gate-el8 | 2.63 | 3.66 | 2.78 | 2.75 | 238 executed, 0 failed, live UI REFUSED (waterfall, scatter) as before |

For comparison, master before this change read 7.6 to 9.3 pp at 10 Hz and 8.2 to 8.7 pp at 100 Hz in the same runner shape on the Ubuntu gate box, and the required Ubuntu tier failed on this test in 6 of 42 jobs.

Not done by this change:
- The residual under-read at the best placement (about 2 to 4 pp), this issue's original question, is open.
- The 8.8 pp bound is unchanged; re-deriving it from the shipped configuration (pre-registered before its confirmation runs) is the next step here.
- On EL8 the test passed while its scoped known-failing entry (issue 675, exit 79, cap 15 pp) is still in `tests/run_all.sh`. Issue 675 is stated against a kernel referee (sampled on-CPU time about half of what the kernel records), which this test does not measure; that ratio is being re-measured on master before the entry is touched.
- The startup warning is on the daemon's output and in the documentation, not yet in the status interface.

Refs #306
