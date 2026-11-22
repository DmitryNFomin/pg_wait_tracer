# EL8 clean-boot CPU-group layout (<ip>, Rocky 8.10, 4.18.0-553.157.1, cgroup v1), commit 8cc67d0f
Tree: copy of /root/n306/tree, git-tree hash 1c1784b2... == `8cc67d0f^{tree}`; prebuilt binary + layout-start.sh reused (paths re-pointed to /root/n306m). Drivers: raw-el8-clean/{step1.sh,run.sh}.
## Step 1 (reboot 2026-11-21 20:10:41Z, probes 20:10:56Z, uptime 22 s at launch)
- postgresql-18: active; postmaster pid 1063 cpu line `4:cpu,cpuacct:/` (memory/pids/systemd: /system.slice/postgresql-18.service)
- non-default CPU units: only `-.slice CPUAccounting=yes` (0 units with CPUShares/CPUWeight/Quota set; 157 units CPUAccounting=no)
- plain probe unit (run-u26): cpu line `4:cpu,cpuacct:/` (other hierarchies /system.slice/run-u26.service)
## Step 2: 20 startup-only checks, 20:11:03Z-20:14:13Z, alternated login/service pairs
| context | exits | tuple (daemon, backend, pgbench, sampler nice) x count |
| login (10) | 0 x10 | (/, /, /pgwt-xval-load-<pid>-10, -10) x10 |
| service (10) | 0 x10 | (/, /, /pgwt-xval-load-<pid>-10, -10) x10 |
(pgbench group is a per-run scope-specific cpu cgroup; the pid suffix is the only difference between rows.)
## Step 3: 10 runs, `--rates "10 100"`, 20:14:13Z-20:39Z. deviation = max disagreement pp. tps = pgbench tps (5 runs/context)
| context | exits | rate | deviation min/med/max | tps min/med/max |
| login | 0 x5 | 10 | 2.26/2.86/3.81 | 2047.48/2063.63/2072.09 |
| login | 0 x5 | 100 | 2.74/3.61/3.67 | 1999.50/2008.45/2047.69 |
| service | 0 x5 | 10 | 2.45/3.55/3.86 | 2046.92/2058.96/2087.14 |
| service | 0 x5 | 100 | 2.91/3.18/3.63 | 2017.44/2025.56/2032.65 |
Readbacks in all 10 runs, both rates: daemon `/`, backend `/`, pgbench `/pgwt-xval-load-<pid>-<rate>`, sampler nice -10. All 20 rate verdicts "within tolerance" (tolerance 8.8 pp).
## Step 4 (postmaster cpu / probe cpu / check exits); baseline before step 4: `/` / `/`
- a) CPUShares=512 unit started, active in /system.slice/n306m-pol.service, stopped (read 2 s later) -> `/system.slice/postgresql-18.service` / `/system.slice` / no check
- b) check login+service (20:37:07Z) -> `/` / `/` / exits 0,0 (daemon `/`, backend `/`, nice -10 both)
- c) restart postgresql-18 -> `/` / `/` / no check
- d) checks -> `/` / `/` / exits 0,0 (same readbacks)
- e) daemon-reload -> `/` / `/` / no check
- f) checks -> `/` / `/` / exits 0,0 (same readbacks)
(Row a was read once, 2 s after stop; the very next reading, after (b), was `/` for both. Not re-measured.)
## Step 5 (read-only, under `flock -n /tmp/pgwt-box-check.lock`; probe unit had no property, collected)
- pgwt-gate-el8 (20:17:26Z, uptime 122273 s): postmasters 13/16/17/18 all `cpu,cpuacct:/`; probe unit `cpu,cpuacct:/`; non-default: only `-.slice CPUAccounting=yes`.
- pgwt-gate <ip>: lock busy for 20 min of retries (60 s apart, last check 20:39:27Z still held): GAVE UP, nothing measured.
- pgwt-gate-2 (20:37:54Z, uptime 4842509 s): system.slice cgroup.subtree_control = `memory pids`; postmaster 18 `0::/system.slice/system-postgresql.slice/postgresql@18-main.service` (13/16/17 likewise); runner service process 23122 `0::/system.slice/actions.runner.DmitryNFomin-pg_wait_tracer.pgwt-gate-2.service`; probe `0::/system.slice/run-u1259934.service`; non-default CPU units: 107, all `CPUAccounting=yes` only (no CPUShares/CPUWeight/Quota).
## Layout sameness
On the clean boot the layout was the same in every start (all 26 startup-only/qualification starts plus 6 in step 4: daemon `/`, backend `/`, nice -10, exit 0).
## Cleanup
/root/n306m removed on the VM; no n306m/pgwt-xval unit, scope or cpu cgroup, no tracer/pgbench process left there; only the system's `run-user-0.mount` remains on all three probed machines (not mine). No scratch worktree was created. Raw: raw-el8-clean/ (step1.txt, step4.txt, start-*, qual-*, el8.txt, gate.txt, gate2.txt).
