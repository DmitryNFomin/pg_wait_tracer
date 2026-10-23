# pg_wait_tracer: one feature and the bugs found in the sampler-vs-tracer experiments (2026-10-21/22)

**Evidence:**
- `tests/results/cpu-limit-and-locktail-20261021/` (README + raw: runs B0–B7, A0–A6)
- `tests/results/lock-tail-20261021/`
- `tests/results/case3-repro-20261021/`
- `tests/results/sampler-accuracy-20261021/` (E7, Q1–Q4)

All runs used the box build of 2026-10-14 on a Hetzner cx33 (AMD EPYC-Rome, KVM), PG 18.6.

## Feature F1: split "waiting for a CPU" out of every interval (run-delay attribution)

**Why.** Under a cgroup CPU limit (Kubernetes), time spent runnable but not running hides inside wait labels.
- After a wait ends, the label stays set until the backend actually gets a CPU (E7).
- A woken backend stays labelled `Client:ClientRead` (idle) until it runs, so that time leaves DB Time entirely.
- In run B1 (1-CPU limit, 72% average use, a parallel report every ~5 s), p99 went from 4.6 to 317 ms.
  - pg_wait_sampling showed Lock/WAL.
  - The kernel showed 0.93 sessions waiting for a CPU against 0.66 on a CPU.
- After the fix the profile suggests (`synchronous_commit=off`, run B5), the sampler saw 0.72 active sessions while the
  kernel saw 0.57 on a CPU and **1.57 waiting for one**.
- pg_wait_sampling cannot ever show this: it reads only the label, and its collector is throttled with the backends.
- pg_wait_tracer is the only PostgreSQL-aware observer outside the cgroup, so this is its strongest "only the tracer"
  case. Today the tracer misses it too:
  - The sampled tier shows the same labels (B4).
  - The exact tier only splits Off-CPU* out of CPU-labelled gaps.

**Exact tier.**
- At every transition the BPF program already computes `cpu_ns`. Also read `current->sched_info.run_delay` (BTF/CO-RE)
  and emit the per-interval delta as `runq_ns`. This needs a trace format version bump.
  - Both boundaries are sampled while the task is running, so the delta is exactly the runnable time inside the
    interval, including CFS-bandwidth throttled time.
- Reporting:
  - Show each wait as sleeping + runnable, e.g. "Lock:transactionid 120 ms, of which 70 ms waiting for CPU".
  - Add a time-model row "CPU wait*", the sum of runnable time across all classes.
  - Count the runnable part of `ClientRead` intervals as DB Time: the request has arrived but the backend can't run.
    Label it separately, e.g. "CPU wait* (request queued)".
- No `sched_waking` hook is needed for the totals.

**Sampled tier.**
- On every tick, read `/proc/<pid>/schedstat` (field 2 = run_delay ns) for each tracked backend. That is one small read
  per backend per tick, from outside the cgroup, at no cost to PostgreSQL.
- Report the delta as an independent per-backend, per-query and global "CPU wait*" measurement.
- Do **not** attribute the whole delta to the label seen at the tick, because several transitions may lie between
  ticks (Astra's caveat). Exact per-label attribution comes from the exact tier.

**Also.** Show cgroup context in the time-model header: `cpu.max`, plus `nr_throttled` / `throttled_usec` deltas from
`cpu.stat`. `effective_cores.c` already locates the cgroup.

**Acceptance.** Re-run the B series (`raw/runb.sh`, 3 repetitions each of B0, B1, B5, B7):
- The tracer's "CPU wait*" is within ±15% of the summed kernel schedstat run-delay of the client backends.
- At least 60% of it sits under non-CPU labels or ClientRead.
- In B1, CPU wait* is the top row.
- The sampled tier costs ≤ 3% of cgroup CPU.

## Bugs / issues

| # | Issue | Evidence | Notes / where |
|---|---|---|---|
| 1 | **Exact-tier overhead anomaly**: +0.65 cores at 1,400 TPC-B tx/s, i.e. +85% cgroup CPU at fixed throughput. That is ≈12 µs per watchpoint fire, against the documented 200–300 ns. Under a 1-CPU limit with 0.28 cores of headroom it capped throughput at ~880/s against 1,400 offered (99% of periods throttled). | `b-B0` vs `b-B3`, `b-B1` vs `b-B2`. Cgroup user CPU 0.47→0.65, system 0.28→0.76 cores. | **Triaged 2026-10-22, see `tests/results/overhead-triage-20261022/`:** a bare watchpoint fire costs ~4.7–5.2 µs on these AMD KVM guests (no BPF), and uprobe traps cost about the same. In a `--mode full` profile, uprobes take ~0.39 cores and watchpoints ~0.33; the BPF programs are small. (An earlier sentence here claimed the cost was not proportional to transitions. Normalised per transaction it is the same order, so that claim is withdrawn.) The README's "6–30%" is throughput loss on another platform; say so, and give CPU cost separately. All runs n=1. |
| 2 | **`Extension:Extension` counted as DB Time** (pg_wait_sampling collector latch, bgworkers) | B4 26% of DB Time, B3 32%, A runs | Should be excluded like Activity/idle, or at least not attributed to client DB Time |
| 3 | **CLI replay ignores measured `cpu_ns`**: accumulates the whole gap as CPU* when `old_event == 0`, so no Off-CPU* row. B2 live `system_event` CPU* 4.3% vs replay `time_model` CPU* 22.4%. | `b-B2/trace` (v3 trace, header has cpu_ns) | `src/event_reader.c:541` (Astra). pgwt-server uses `cpu_ns` correctly; make the CLI path match. |
| 4 | **Duplicate rows in pgwt-server `executions`**: 92,242 rows for 46,746 calls; one copy has an empty `execution_detail` | `lock-tail-20261021/raw/r-v2trace/trace` | Possibly addressed by #329 (one row per execution); the Oct-14 build still showed it. Re-check on that trace. |
| 5 | **CPU* > execution duration in `execution_detail`**: 63 of 97 executions, e.g. 7.55 ms CPU* inside a 2.40 ms execution. The update's `query_event` CPU* is 26.1 s vs its pg_stat_statements total exec time of 6.4 s. | same trace; `slow-execs.csv` | Intervals not clipped to execution boundaries, and/or CPU* attributed across statements. Clients summing `execution_detail` events currently get wrong totals. |
| 6 | **`lock_chains` "holder" is a heuristic**: picks the pid whose CPU interval overlaps the wait most; no lock tag, no ownership edge | `src/compute.c:3458` | Label it "likely holder" in UI/docs. Defeated by idle-in-transaction holders, holders waiting on WAL, and prepared transactions. |
| 7 | `system_event` view has no Off-CPU* row (only `time_model` does) | B2/B3 live output | Minor; inconsistent views |
| 8 | `WARN: could not write trace/wait_event_names.json` on every `-T` run | all traced runs | pgwt-server then can't classify Timeout pacing waits |
| 9 | `WARN: group 'dba' not found` → traces readable by root only | all traced runs | Minor |

## Priority for the talk

1. F1, which makes the CPU-limit case a genuine "only the tracer" case.
2. Bug 1 triage, so any overhead number shown is the design's, not this VM's.
3. Bugs 2, 3 and 5, which are visible in any demo.
4. Bug 4 and the label for bug 6.
