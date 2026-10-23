# Console outputs

E5, E7, Q2 and Q3 kept their per-session rows in unlogged tables (e5_result, e7_truth, e7_psa, e7_pgws) that were
dropped when gate-2 was restored. Q4's script overwrote q4-*.txt on each repetition, so raw/ holds only the last rep.
The files here are the summary outputs exactly as printed during the session; they are the only record of those runs.

| File | Run |
|---|---|
| e1_ticklat.txt | E1 summary (raw per-tick lateness is in raw/sampler-e1/lat-*.txt) |
| e2_collector.txt | E2 summary (raw round timestamps in raw/sampler-e2/rounds-*.txt) |
| e5_same_total.txt | E5, both runs |
| e6_detection.txt | E6 summary (raw per-wait rows in raw/sampler-e2/e6-*.csv) |
| e7_known_truth.txt | E7 report and schedstat summary (schedstat raw in raw/sampler-e2/sched-*.txt) |
| q1_quota_cpu.txt | Q1 summary (raw history rows in raw/sampler-e2/q-hist-*.csv) |
| q2_q3_quota_mixed.txt | Q2 and Q3 reports |
| q4_tracer_vs_pgws.txt | Q4, all three repetitions |
