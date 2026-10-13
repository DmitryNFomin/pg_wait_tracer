PgSleep: PID 1171002, start 1790897311539822175 (2026-10-01 23:28:31.539822175 UTC), duration 3004037354 ns, cpu_ns=923834; SELECT pg_sleep(3).
Neighbourhood: ClientRead → CPU 183333 ns → PgSleep → CPU 279544 ns → ClientRead; EXEC_START 11.742 µs before sleep, EXEC_END 21.792 µs after it, no internal marker or transition.
Verdict: evidence supports benign CPU inside the recorded wait boundary; no stale-label or unclosed-clock defect is proven, so no src fix is proposed.
Immediate identical sleeps carry 35.908/31.589 µs; versus the next sleep this event adds 963.841 µs wall and 892.245 µs CPU, leaving only 71.596 µs additional non-CPU time.
Measured combined entry/exit/wakeup allowance: 0.923834 ms observed (0.030753%); documented in criteria, not converted to a higher acceptance threshold.
Lock:transactionid: real 100% ratios on 12.974 µs and 4.909 µs pgbench UPDATE waits, not millisecond anomalies; largest CPU is separately 112.622 µs in a 1.00014595 s hot-row wait.
Lock:extend: statistically empty, n=2; 20.118/22.122 µs CPU in 62.917/75.692 µs waits, each followed by CPU and DataFileExtend inside the same INSERT; no defect proven.
Verification: six selected raw events match unchanged Measurement execution details; threshold remains 824430 ns, outlier remains counted, repeated window remains REFUSED.
Unknown: exact wakeup/signal/probe/interrupt cause and CPU split; no scheduler edges/stacks to independently exclude a short accounting error; global execution coverage gaps persist.
Evidence and reproduction: NOTES.md, investigate.py, comparison.json, per-target raw/protocol files; src unchanged, no box-check/rehearsal/PR/push.
