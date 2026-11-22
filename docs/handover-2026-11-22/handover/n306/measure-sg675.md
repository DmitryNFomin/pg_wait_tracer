# sg675: #675 / #671 S/G and E/G re-run on master 7815defb (2026-11-22)
**Method.** I used #671's `scratch671/probe.sh` and `derive.py`, with derive.py unchanged, taken from origin/preserve/671-tier-truth. Settings were as in 671: PG 18 on port 5418, tiered mode, 10 Hz, 8 clients, `-j 2`, `-T 320`, an 8 s warm-up, then a 300 s escalation (budget 900). S = `RAW_CPU_SAMPLED_NS` and E = `RAW_CPU_EXACT_NS` from `cross_validate --tolerance 8.8 --raw-ns --show-idle`. G = Σ Δ `/proc/<pid>/schedstat` field 1 over the client backends present at both window edges.
Validity checks kept from 671: main-thread policy must read back as SCHED_OTHER, comparator fields present, G>0, pid churn and steal recorded. Order was K,Z,K,Z,K,Z on each machine, with the two machines running in parallel.
**Deviation 1, launch shape.** The tracer ran as a plain transient system service (`systemd-run --collect` with RuntimeMaxSec, TimeoutStopSec, StandardOutput/StandardError and ExecStopPost; no CPU, Nice or Slice property), not as a shell child. pgbench ran in a `--scope --slice=user.slice` unit with no property, started before the tracer and released by a go file. On EL8 (v1) pgbench went into a scratch cpu cgroup `/sg675-load-<run>`, created with mkdir and removed with rmdir, as in the cross-validation test. On Ubuntu (v2) the test's `CPUWeight=100` on the scope was left out: cpu was already enabled at the root and in user.slice, so the scope got its own group without it. Groups were read with `tests/cpu_group_readback` at window midpoint. The arm's env was set inside the unit (Z: `PGWT_SAMPLER_NICE=0`).
**Deviation 2, extra records.** pgbench ran with `-P 5 --progress-timestamp`. tps = mean of the 5 s progress intervals in the window, because the scope stop removes pgbench's summary. Nice was read per task at window midpoint from `/proc/<tid>/stat` field 19.
**Deviation 3, hosts (limits comparability with 0.504 / 0.788).** Both VMs are **Intel Xeon Skylake**; 671 used AMD EPYC-Rome on both hosts. EL8 runs PG 18.6 with pg_stat_statements only; 671's EL8 ran PG 18.4 with pg_wait_sampling preloaded. PG 13, 16 and 17 clusters were running idle on both VMs. Kernels match 671's (4.18.0-553.157.1; 6.8.0-139). The 671 binary was 6b1fb8b2; this run used 7815defb, built with box-build.sh's make targets.
## EL8 (Rocky 8.10, 4.18, cgroup v1), <ip>
| run | start UTC | arm | S s | E s | G s | S/G | E/G | tps | nice | groups ok |
|---|---|---|---|---|---|---|---|---|---|---|
| el8-K-1 | 04:22:23 | K | 616.3 | 690.8 | 823.5 | 0.748 | 0.839 | 1982.7 | -10 | yes |
| el8-Z-1 | 04:27:38 | Z | 473.1 | 689.0 | 825.8 | 0.573 | 0.834 | 2001.1 | 0 | yes |
| el8-K-2 | 04:32:54 | K | 605.8 | 690.0 | 822.0 | 0.737 | 0.840 | 1970.5 | -10 | yes |
| el8-Z-2 | 04:38:10 | Z | 493.2 | 701.8 | 829.3 | 0.595 | 0.846 | 1995.8 | 0 | yes |
| el8-K-3 | 04:43:26 | K | 609.3 | 690.1 | 823.9 | 0.740 | 0.838 | 2030.7 | -10 | yes |
| el8-Z-3 | 04:48:42 | Z | 481.6 | 699.8 | 827.2 | 0.582 | 0.846 | 2005.4 | 0 | yes |
Groups in every run: daemon `v1 /`, backend `v1 /`, pgbench `v1 /sg675-load-<run>`.
## Ubuntu 24.04 (6.8, cgroup v2), <ip>
| run | start UTC | arm | S s | E s | G s | S/G | E/G | tps | nice | groups ok |
|---|---|---|---|---|---|---|---|---|---|---|
| ubuntu-K-1 | 04:27:59 | K | 773.4 | 857.5 | 855.8 | 0.904 | 1.002 | 1709.1 | -10 | yes |
| ubuntu-Z-1 | 04:33:17 | Z | 705.9 | 864.2 | 859.1 | 0.822 | 1.006 | 1745.5 | 0 | yes |
| ubuntu-K-2 | 04:38:34 | K | 768.9 | 868.4 | 856.9 | 0.897 | 1.013 | 1728.2 | -10 | yes |
| ubuntu-Z-2 | 04:43:52 | Z | 706.5 | 836.1 | 872.5 | 0.810 | 0.958 | 1820.3 | 0 | yes |
| ubuntu-K-3 | 04:49:09 | K | 748.9 | 848.0 | 856.6 | 0.874 | 0.990 | 1755.0 | -10 | yes |
| ubuntu-Z-3 | 04:54:27 | Z | 709.3 | 861.3 | 857.8 | 0.827 | 1.004 | 1763.6 | 0 | yes |
Groups in every run: daemon `v2 /system.slice`, backend `v2 /system.slice`, pgbench `v2 /user.slice/sg675-load-<run>.scope`.
**All 12 runs:** steal 0.000 %, pid churn 0 (8 pids at both edges), SCHED_OTHER read back, daemon `exited:0`; no run void, replaced or dropped; no 671 rule broken. Nice is the main thread's (tid = MainPID); the other two tasks read 0 in both arms. The daemon logged `sampler CPU group: shared` and `applied nice -10` (K) or `0` (Z). xval at 8.8 pp: EL8 K PASS, EL8 Z FAIL, Ubuntu PASS in both arms.
## min / median / max (n=3 per arm) beside 671 (SCHED_OTHER medians, n=3)
| machine | arm | S/G min / med / max | E/G min / med / max | 671 S/G | 671 E/G |
|---|---|---|---|---|---|
| EL8 4.18 | K (nice -10) | 0.737 / 0.740 / 0.748 | 0.838 / 0.839 / 0.840 | 0.504 | 0.830 |
| EL8 4.18 | Z (nice 0) | 0.573 / 0.582 / 0.595 | 0.834 / 0.846 / 0.846 | 0.504 | 0.830 |
| Ubuntu 6.8 | K (nice -10) | 0.874 / 0.897 / 0.904 | 0.990 / 1.002 / 1.013 | 0.788 | 0.991 |
| Ubuntu 6.8 | Z (nice 0) | 0.810 / 0.822 / 0.827 | 0.958 / 1.004 / 1.006 | 0.788 | 0.991 |
671's daemon ran at nice 0, so arm Z is the closer analogue of 671's numbers, subject to deviations 1 and 3.
Raw: `SP/n306/raw-sg675/{el8,ubuntu}/` (per-run dirs, all.jsonl, drive.log, build.log); scripts in `.../raw-sg675/scripts/`.
**Cleanup:** /root/sg675 is removed on both VMs. Afterwards both showed 0 sg675 units, 0 sg675 cgroup dirs, and 0 tracer or pgbench processes. Other trees under /root were untouched. The worktree `wt-sg675` was removed. No repository files were edited or committed.
