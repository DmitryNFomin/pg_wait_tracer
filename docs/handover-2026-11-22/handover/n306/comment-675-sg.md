**The referee experiment re-run on master 7815defb (2026-11-22): the default nice -10 moves S/G from 0.58 to 0.74 on EL8 and from 0.82 to 0.90 on Ubuntu; it does not close the gap**

Method: issue 671's `probe.sh` and `derive.py` (derive.py unchanged, from `origin/preserve/671-tier-truth`), PostgreSQL 18, tiered mode, 10 Hz, 8 clients, a 300 s window. Two arms alternated K, Z, K, Z, K, Z on each machine: K = master's default (sampler at nice -10), Z = `PGWT_SAMPLER_NICE=0`. S = sampled on-CPU seconds, E = exact-tier on-CPU seconds, G = the kernel's schedstat total over the same eight backends and the same window.

| Kernel | Arm | S/G min / median / max | E/G min / median / max | Issue's figures (S/G, E/G) |
|---|---|---|---|---|
| EL8 4.18 | K, nice -10 | 0.737 / 0.740 / 0.748 | 0.838 / 0.839 / 0.840 | 0.504, 0.830 |
| EL8 4.18 | Z, nice 0 | 0.573 / 0.582 / 0.595 | 0.834 / 0.846 / 0.846 | |
| Ubuntu 6.8 | K, nice -10 | 0.874 / 0.897 / 0.904 | 0.990 / 1.002 / 1.013 | 0.788, 0.991 |
| Ubuntu 6.8 | Z, nice 0 | 0.810 / 0.822 / 0.827 | 0.958 / 1.004 / 1.006 | |

All 12 runs valid by issue 671's rules: steal 0.000 %, pid churn 0, SCHED_OTHER read back, the daemon and the backends in one CPU group and pgbench outside it in every run, the nice value read back as intended; no run void, replaced or dropped.

Where this is not comparable with the issue's 0.504 / 0.788:
- Both machines here are Intel Skylake VMs; issue 671 used AMD EPYC-Rome on both hosts.
- The tracer ran as a plain transient system service with pgbench in its own scope (the shape the cross-validation test now uses), not as a shell child.
- EL8 here runs PostgreSQL 18.6 with pg_stat_statements only; issue 671's EL8 had pg_wait_sampling preloaded.
- Three runs per arm per machine; one workload.

What stands after it:
- On 4.18 the sampled tier still reports about three quarters of the on-CPU time the kernel records with the shipped default, and the exact tier about 84 % of it. Neither is explained here. This issue stays open and stays a tag-blocker.
- The scoped known-failing entry (`test_cross_validate_tiered|675|el8@4.18|exit=79`) is unchanged. docs/DEV_LOOP_PLAN.md ties its removal to this issue being fixed, while the signature it classifies (the test exceeding 8.8 pp on EL8) has not occurred since the default changed: 2.26 to 4.44 pp in 13 EL8 runs, including the EL8 gate box in CI. Whether the entry should now go, so that the EL8 gate is held to 8.8 pp instead of 15, is a question for the design advisers; it is not decided here.

Raw: lead scratchpad `n306/measure-sg675.md` and `n306/raw-sg675/`.

Refs #675
