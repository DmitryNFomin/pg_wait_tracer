# Attack on POC-PLAN.md (Opus, independent; read-only, origin/master @ 7815defb)

**Q1. Today's design, and what the kernel already holds.** Section 1 is incomplete in a way that matters: most of the proposal already exists or has existed.
- VERIFIED `src/bpf/pg_wait_tracer.bpf.c:577-716`. Per trap, today's handler does `state_map` lookup by pid, one `probe_read_user` of the cached word address, drops same-value writes (`:620`), computes the duration and the on-CPU delta from the `sched_switch` accumulator (`:509-555`), and then either `accum_add()` into a PERCPU_HASH keyed by event (`lightweight_mode`, `:635-637`, map `:208-218`) or `resolve_exact_attr` (3 more lookups) plus one 48-byte `bpf_ringbuf_output(..., BPF_RB_NO_WAKEUP)` (`:292-303`). So there is no wakeup per event. It then updates `last_event`/`last_ts`/`last_cpu_ns` in place. The per-backend "current event, since when" record and per-backend on-CPU accounting from `sched_switch` already exist (`state_map`, `:115-119`).
- VERIFIED `git show fe405a01` (2026-02-25). The product began with exactly this idea: PERCPU_HASH `wait_stats` keyed by (pid,event) and `query_wait_stats` keyed by (query,event), with log2 histograms, inserted with unchecked `BPF_ANY`. It was replaced by the stream, with the claim "reduces overhead (~11% → ~7%)". The method behind that claim is not recorded. VERIFIED `docs/DAEMON_ARCHITECTURE.md:6-18` gives the reason for the change: raw events as the source of truth, for queries over any time range.
- Today's arming: open the event disabled, read the word, seed `state_map` with `BPF_ANY`, then enable (VERIFIED `src/backend.c:384-414,322`). The code itself names a "seed→arm race" (VERIFIED `src/backend.c:213-216`).

**Q2a. Per-trap cost: the trap is the expensive part, not the stream.**
- VERIFIED `docs/handover-2026-11-10/HANDOVER-REVIEW-LEAD.md:100-107,145-176`. A bare watchpoint, with no BPF and no PostgreSQL, costs 4.7-6.7 µs per fire on all three boxes, and 14 µs at low fire rates. An armed watchpoint costs 3.4-4.7 µs per schedule-in even when it never fires. "Trap count x platform trap cost explains the whole delta" of +0.70 cores. The uprobe int3 path (about 6 per statement) costs more than the watchpoint.
- That is the only separation of trap cost from stream cost in the repository, and it leaves no room for a stream cost. "`--lightweight` removes about a third" (`:127`) is confounded: lightweight mode also drops the execution probes (VERIFIED `src/exact_probe.c:148-162`), so that third is uprobes, not the ring buffer.
- INFERRED from the above: the proposal removes the ring write (an estimated ~100 ns) and keeps the #DB exception, `hw_breakpoint` notify, `perf_bp_event`, the BPF run and the debug-register switching. Its cost will be about today's watchpoint cost.

**Q2b. "Nothing can be lost" is false as stated.**
- Same-value writes: harmless, already skipped (`:620`).
- Two writes in one trap: impossible. The x86 data breakpoint traps after each store, synchronously in the writing thread (INFERRED from x86 semantics).
- Execution context: on 4.18, `do_debug` runs `nmi_enter()` (VERIFIED in the #871 comment of 2026-11-11 14:38, EL8 source `traps.c:867-896`). PERCPU maps and in-place updates are safe there. A hash insert spins on the bucket lock, but cannot deadlock against itself, because a user-mode #DB never interrupts kernel code (INFERRED). `bpf_spin_lock` is unavailable to tracing programs, and 4.18 has no mmap'able maps and only XADD atomics (INFERRED, kernel knowledge).
- **The reader tears records.** The syscall lookup copies the value without a lock while the handler rewrites `{event, since, totals}`. One read can then pair event A with B's start time, so the open-interval addition is wrong. This needs a hand-made seqcount or a double buffer, with no barrier instruction available in BPF.
- Hash insert failure: the map is preallocated with fixed capacity (`MAX_BACKENDS`). Exited backends must keep a tombstone until the reader folds their totals, otherwise the last second of every exited backend is lost. Churn plus tombstones then fills the map, and new backends go untracked. The first version silently ignored this failure (`fe405a01` `BPF_ANY` unchecked).
- Exit totals go out through `lifecycle_rb`, a shared ring, so they are a stream again (VERIFIED `:910-916`).
- CPU migration: no issue. The pid's entry is touched only on the CPU where it runs (`:521-524`).
- Counter overflow: none with u64 nanoseconds.
- Watchpoint disabled or unschedulable: four debug registers are shared with gdb/ptrace and with `perf -e mem:` users. A pinned per-task event that fails to schedule moves to the ERROR state and stays silent. Some hypervisors do not virtualise debug registers. Perf throttling is unresolved: #390 is "conditional, not clean", and its detector could not be made red (VERIFIED `HANDOVER-REVIEW-LEAD.md:167-172`). INFERRED otherwise.
- Word address: on PG14+, `my_wait_event_info` points at local storage before `InitProcess` and again after `ProcKill` resets it. Waits during authentication and exit are invisible and count as CPU (INFERRED from PostgreSQL source knowledge; the bootstrap watchpoint `:721-767` covers only the start).
- PID reuse: `on_fork` must overwrite stale state, which the advisers noted on #871 (2026-11-13). Exec is not applicable.

**Q2c. Arming race.**
- The current order (read, then enable) loses any transition between the read and the enable. A backend that enters a long lock wait in that window is labelled CPU for hours.
- Simplest correct rule: open the event disabled, attach the program, enable; then read the word; then insert with `BPF_NOEXIST`. If any trap happened first, the handler already created the entry from its own read and the seed loses. The origin is the read time, not the arming time.
- Processes that sleep for hours (idle connection, checkpointer) produce no traps. They need this seed and an open interval added by the reader. Hours of sleep are not a problem in themselves (INFERRED).

**Q2d. Per-query attribution.**
- PG14-18: `PgBackendStatus.st_query_id`, readable in the handler (`read_query_id_cached`, `:309-323`). It is top-level only, and 0 unless `compute_query_id` is on. "`--skip-query-id` changed nothing", so the read cost is negligible (VERIFIED `HANDOVER:125`).
- PG13: the only source is `PlannedStmt->queryId` reached from the executor USDT (`:85-92`), which means uprobes.
- **Fatal flaw:** the query id changes without any write to the wait-event word. CPU and early waits after `ClientRead` ends are charged to the previous query or to 0, and a query with no waits is invisible. This is why the product traps `pgstat_report_query_id` with a uprobe (`:1019-1044`). Correct attribution brings back the uprobe, or a second watchpoint, and therefore the trap cost.
- Memory: the key (pid,query,event) cannot be preallocated (for example 1024 x 5000 x 50 entries), and LRU eviction is silent loss. The key (query,event) without the pid is bounded but can still fill. Per-trap cost of the lookups: hundreds of ns against a 5 µs trap.

**Q2e. What totals cannot give.** The server serves views that need individual intervals or executions (VERIFIED `src/server.c`):
- p50/p95/p99 per event, which need histograms (`:5484-5510`);
- `session_timeline` (`:6416`), `executions`, `execution_detail`, `span_detail` and `exec_scatter` (`:6829-7450`);
- `transitions` (`:8089`), `lock_chains` (`:8362`), `interference` and `concurrency` (`:8410-8464`).

If those views come from a best-effort stream and the totals from the maps, the two views disagree whenever the stream drops, which CLAUDE.md RULE 2 forbids. The only fix is a lossless stream, which is #871 again. Note that #871 already measured both per-CPU transports delivering every record on EL8 and Ubuntu (VERIFIED, #871 comment of 2026-11-12 10:16). The loss problem was solved; what consumed the month was merge, generation and boundaries, and this PoC excludes exactly that part (section 5).

**Q3. Can each criterion fail?**
- H1 cannot fail. `state_map`, `accum_map` and `tp_btf/sched_switch` already load on 4.18 (VERIFIED, #871 M0b: "loaded and ran on 4.18 without errors").
- H2 fails for the wrong reason. `pg_sleep` and `ProcSleep` loop on `WaitLatch` (the `deadlock_timeout` wakeup, signals), so one scripted wait produces several intervals and "known counts" are not known (INFERRED, PostgreSQL source). The risk is that the criterion gets edited after the run. Use `pg_locks.waitstart` (PG14+) as the truth for lock waits.
- H3 is tautological. Intervals are cut from one clock, so a missed or mislabelled transition moves time between states and leaves the sum unchanged. The missing check is cross-source: time not in a wait event but asleep according to `sched_switch` should be about 0.
- H4 compares unlike quantities. The perf hit count includes same-value writes, and neither count sees a trap that never fired. The reader stall cannot fail, because no ring remains. Missing: zero map-insert and tombstone failures with a forced-red control, a torn-read check under a fast reader, and churn.
- H5 will very likely fail. The daemon's own `cpu_ns_total`/schedstat is already measured at 0.825 on EL8 and 0.861 on Ubuntu (VERIFIED, #675 body). A per-backend criterion is missing: a total over backends hides per-backend errors.
- H6 passes for the wrong reason. The product's exact mode carries about 6 uprobes per statement and the PoC arm carries none. Add a bare-watchpoint arm (empty program) and a "product without uprobes" arm. The throttled +10% will very likely fail on trap cost alone: at 1,400 tx/s, the watchpoint share of about 0.33 cores is roughly 236 µs per transaction (INFERRED from `HANDOVER:117-118`).
- H7: the seed race is a window of microseconds and will not be hit by chance. Inject a delay between the read and the enable (the `PGWT_TEST_STALE_SEED` hook already exists, `src/backend.c:213-231`).
- H8: `pg_stat_statements` has no wait counts, so "counts" is undefined. Use per-query (on-CPU + waits) against `total_exec_time` (+ plan time) to expose the boundary flaw.

**Prior art:** in-kernel aggregation was built, then removed in `fe405a01`; lightweight mode survives as `--lightweight`. `gh issue list --search "in-kernel aggregation OR accumulator OR aggregate in kernel"` returned no issue that proposes or rejects it (#808, #439, #381 and others are about totals disagreeing).

**Three most likely ways this ends the same way:**
1. The overhead is unchanged, because the trap (5 µs per fire plus the cost per schedule-in) stays, and per-query attribution needs uprobes or another watchpoint.
2. Map totals and best-effort per-event views disagree, so the RULE 2 tag-blockers come back, and the stream must become lossless again: #871 plus the merge, generation and boundary work.
3. The correctness claim rests on criteria that pass for the wrong reasons (H1, H3, H4, H6). Meanwhile the real new failure modes go untested: torn reads, map capacity and tombstones, query-boundary misattribution, the seed race, and a watchpoint in ERROR state.

**Cheapest kill experiment:** on both gate boxes, run throttled pgbench with three arms, n=3 each: no tracer; a hardware watchpoint on every backend's word with an empty BPF program (`return 0`, the trap floor); the current product full mode. If the trap-floor arm alone adds more than 10% CPU per transaction, H6 dies whatever the handler does, and the idea fails before any accounting code is written.
