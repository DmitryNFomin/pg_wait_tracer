You are a Linux scheduler + PostgreSQL internals expert. Be concrete and skeptical. Do not modify files.

Goal: the speaker (talk "Beyond Sampling: True Wait Event Tracing with pg_wait_tracer") needs to find conditions where
wait-event SAMPLERS (pg_wait_sampling 1.1.11 at 100 Hz default, pg_stat_activity pollers like pgsentinel / scripts /
monitoring agents at 1-10 Hz, AWS Performance Insights at 1 Hz) give MATERIALLY WRONG numbers, especially on LOADED
systems, measured against a known truth. "Materially" = would change the diagnosis or the fix.

What we already measured on a 4-vCPU Hetzner VM (Ubuntu 24.04, kernel 6.8, PG 18):
- A bare 100 Hz SCHED_OTHER timer: wake lateness p50 0.08 ms, p99 0.2-0.4 ms, max 4.6 ms even at load average 14.
- pg_wait_sampling collector: achieved 100.0 Hz, inter-round gap p99 <= 10.63 ms, max 14.6 ms, under pgbench -S c16 and TPC-B c16.
- Detection of injected advisory-lock waits by pg_wait_sampling history matches min(1, d/10ms) within ~3 pp, idle and loaded.
- Same total wait time: 598 waits of 50 ms vs 27,114 waits of ~1.1 ms -> both ~3,000 profile samples.
- Earlier kernel-traced experiment (bpftrace sched tracepoints) with a 49 Hz pg_stat_activity poller, 8 pgbench backends
  on 4 vCPU: fraction of backends on-CPU-or-runnable at the sampler's switch-in instants 63.3% vs 66.7% by time
  (-3.4 pp, all 8 backends -2.6..-5.1). Unsaturated (2 backends): -0.4 pp. pg_stat_activity rows "state != active and
  wait_event IS NULL" were 23% of rows saturated, 11% unsaturated. Sampler wakeup->on-cpu delay mostly 8-16 us, tail 1-4 ms.
- In that saturated run, kernel schedstat over the 8 backends: on-CPU 177 s, run-queue delay 152 s, blocked 164 s
  (wall 494 backend-s); exact-trace "CPU*" (wait_event_info == 0) wall = 258 s.

Questions:
1. List mechanisms (ranked by expected magnitude on a loaded box) by which a sampler's numbers are wrong vs the truth
   of what backends were doing. For each: mechanism, which tools it affects, expected magnitude, whether an exact trace
   of wait_event_info is ALSO wrong (be honest), and a concrete reproducible experiment with known ground truth.
   Consider at least: run-queue time after a wait ends still labelled with the old wait_event (e.g. ClientRead/IO/Lock
   label persisting while runnable); ClientRead absorbing queueing so AAS/DB time is under-reported under saturation;
   cgroup CPU throttling (cpu.max, Kubernetes limits) shown as CPU; state='active' filters in pg_stat_activity samplers;
   torn pg_stat_activity rows (state vs wait_event vs query read at different times); query_id / query attribution
   races for short statements; aliasing with periodic activity (vacuum_cost_delay, bgwriter/walwriter delays, app
   pollers, commit_delay); the sampler perturbing the system (ProcArrayLock, pg_stat_activity cost under load);
   Performance Insights 1 Hz + "sampled load" scaling; short incidents shorter than the drill-down window.
2. For the top 3, write the exact experiment: workload, ground-truth measurement (e.g. /proc/<pid>/schedstat,
   cgroup cpu.stat throttled_usec, client-side timestamps, a victim session that logs its own phase timings), what to
   compare, and the predicted result if the mechanism is real. We have root on the box, no bpftrace installed (can
   apt install), pg_wait_sampling installed, pg_wait_tracer available.
3. Which of these is the strongest "sampler is wrong, and it changes the fix" story for a 10-minute talk, and which
   real-world reports (with links you actually opened) describe it.
