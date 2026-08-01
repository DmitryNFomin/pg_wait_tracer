# Changelog

All notable changes to pg_wait_tracer. Entries are reconstructed from the git
tag history for released versions; the Unreleased section tracks work merged to
`master` since the last tag. The client (Go) and server/daemon (C) share one
version — a build embeds `git describe` at compile time and the client warns on
client/server skew (see `RELEASING.md`).

The format loosely follows [Keep a Changelog](https://keepachangelog.com/).

## [Unreleased]

- **fix(cpu): seed→arm race — stale-state sweep for attached backends.** The
  residual pure-CPU straddle live-view flake (three CI hits in one day,
  PG13/17/18): the preseed reads `wait_event_info` microseconds before
  `PERF_EVENT_IOC_ENABLE` arms the watchpoint; if the seeded wait ends inside
  that window, its ending write never fires and the state_map entry stays
  frozen at a wait that ended long ago (`on_cpu_ts = 0`), so a waitless
  pure-CPU straddler read live `CPU* = 0` for the whole capture while the
  terminal flush and the `/proc` magnitude cross-check stayed correct (PR #56's
  fire-time `on_cpu_ts` open never runs — there IS no fire). Fixed by
  `pgwt_sweep_stale_state` (the attached-backend sibling of PR #52's
  `pgwt_recover_unattached_backends`, same per-tick level-triggered slot): a
  STABLE actual-vs-state wei mismatch (two `/proc` reads 2ms apart) on a FROZEN
  entry (no fire ≥1s), re-checked against a racing real fire, is repaired by
  the exact attach reseed — the never-bounded stale interval is dropped, never
  emitted. One INFO line per repair + new `state_reseeds_total` metric. New
  deterministic regression `phase_stale_seed_sweep` + `PGWT_TEST_STALE_SEED`
  hook (attach seed poisoned with a fake stale wait — the missed fire on every
  run), with a negative under `PGWT_TEST_NO_STALE_SWEEP` proving the sweep is
  the repairing agent. Both runs hold sched_switch inert via
  `PGWT_TEST_NO_SCHED_ONCPU` — on a busy host a single preemption of the hog
  otherwise opens the on-CPU stretch without the sweep (observed on the EL8
  box) — with the sweep's repair reseed exempt from that hook's seed force as
  the one sanctioned non-fire opener.

- **ui(U1+U2a): fixture gallery, chart identity, camera-lite instrument**
  (Track U, PRs #60/#62). Gallery: 49 deterministic builder states +
  7-word visual checklist + tight-threshold cell snapshots — and it caught
  a flagship bug on day one (stacked areas on a value x-axis only ever
  painted series[0]). Identity: deterministic per-event colors, stable
  stack order, name-keyed legend surviving live ticks, honesty overlays
  moved to a silent annotation series (no longer vanish on hover). U2a:
  camera + strip-cache modules; wheel-zoom under cursor and shift+drag pan
  on the AAS chart with no server round-trip in the gesture (150ms
  settle-refine); type:'time'+useUTC axis fixes the stacked-area bug with
  TZ-stable ticks. The pre-registered gesture gate measured RED
  (p95 61-68ms vs 33ms line, 3 runs) — U2b (uPlot AAS substrate) is now
  scheduled per the written rule.

- **ui(U0): error visibility, live-pause, misplaced marks, test infra** (Track U
  Phase U0, PR #59). Failures now paint a per-pane error card (view, command,
  server code/hint, Retry) instead of being pixel-identical to an idle DB — the
  `window_too_large` refusal is finally presented; `[pgwt]`-prefixed console
  error surface; formatter null-guards. Drill/breadcrumb/filter-clear pause
  live refresh (Live button resumes, one span source of truth). Burst markers
  use containing-bucket arithmetic (newest burst no longer draws far-left),
  session-timeline bars clip to the window (no more painting over PID labels),
  the escalation markLine renders for the first time (was placed past the
  axis), `animation:false` in every builder. Tests: five copy-pasted FakeWS
  monkey-patches replaced by a loopback-gated `?ws=` override; console guard
  re-scoped; playwright pinned + baseline VERSION provenance; run_all.sh runs
  the node unit layer; three tests that pinned the confirmed bugs flipped.

- **fix(cpu): pure-CPU straddler live CPU\*.** The measured-CPU live view
  intermittently reported `CPU* = 0` for a waitless pure-CPU command that
  straddled capture (~2% of runs, PG13 on 2-vCPU CI, never on a real box; the
  terminal flush and the `/proc` magnitude cross-check were correct — only the
  periodic live read was 0). A backend momentarily in a wait at the attach
  instant seeds `on_cpu_ts = 0`; after it leaves that wait the `on_watchpoint`
  transition to on-CPU advanced `last_cpu_ns` but never opened `on_cpu_ts`, so a
  backend that then ran uninterrupted (no `sched_switch` to open it — common
  when a CPU-bound backend owns a core on a quiet host) kept its on-CPU
  accumulator flat and read `cpu_open == 0` for the whole stretch. Fixed by
  opening `on_cpu_ts` at every watchpoint fire (the backend is provably on-CPU
  there) — a no-op in normal operation. New deterministic regression
  `phase_straddle_livecpu_deterministic` + a `PGWT_TEST_NO_SCHED_ONCPU` hook
  (sched_switch inert) reproduces the miss on every run (live `CPU*` 9027ms with
  the fix vs a 66ms background floor without).

## [0.13] — 2026-07-21

The **Trust Milestone** (Track T, `docs/ROADMAP_AND_STATUS.md`) — a
correctness-and-honesty hardening pass after a five-perspective adversarial
review found that every one of the tool's honesty guarantees was violated by at
least one code path, and that CI never executed the capture slice where all four
field escapes lived. Merged so far:

- **T0 — real-PG CI safety net.** A capture-smoke CI job installs PostgreSQL
  (PGDG, matrix PG 13/16/17/18), runs the real BPF capture path against it, and
  goes red if capture silently records nothing. `/proc/maps` + PIE/non-PIE ELF
  fixtures unit-test load-base and symbol resolution (the #24 class). `run_all.sh`
  gained `--require-live` (an all-skip run can no longer pass) and a single
  shared unit-test list; the overhead gate runs `--quick` with a tracked CSV
  trend.
- **T1 — fidelity merge & summary honesty.** Escalation markers are now the
  exact-wins coverage authority (no more double-counting long waits across an
  escalation boundary); the summary fast path is coverage-aware and never
  hardcodes a `"fidelity":"exact"` label over sampled data; markers are filtered
  from every aggregation; latency columns are gated under sampled fidelity;
  `--replay` is fidelity-aware; the merge runs in one clock domain.
- **T2 — AAS semantics.** One decomposed active-session model across all paths:
  on-CPU samples are first-class (a pure CPU storm now raises AAS and can trigger
  escalation), category decomposition (plan/exec/command/maintenance/background),
  io_workers excluded from AAS and surfaced as a utilization metric. Fixes the
  PIE uprobe attach-offset defect (silently dead uprobes) and an exit-record
  phantom-CPU artifact. Decisions in `docs/AAS_SEMANTICS_DECISION.md` and
  `docs/T2_IOWORKER_STUDY.md`.
- **T4 — capture & sampler hardening.** The BPF `state_map` is sized to the
  registry with checked inserts and a `state_map_full` counter (no more silent
  under-count above 512 backends); strict offset validation refuses to attach on
  a mismatched layout instead of tracing garbage; load-base resolution matches
  the exact binary path (not a substring); `execve` replaces `popen`; a total
  sampler read failure is loud in logs and control-socket status.
- **T5 — durability & retention.** The writer never truncates — it recovers and
  renames aside an existing `current.trace`/`current.summary` on startup;
  archive renames are collision-safe; retention gained a size cap and orphan
  cleanup; `query_texts.jsonl` is append-only with dedup-on-load; the on-disk
  format and its durability policy are specified in `docs/TRACE_FORMAT.md`.
- **T6 — client transport trust.** The transport rejects error envelopes as
  failures (a dead SSH pipe now shows a degraded-transport state, not "No data"
  under a green pill); request ids are namespaced per WebSocket connection (two
  tabs never exchange data); the bridge respawns SSH with backoff + keepalives;
  init is idempotent across reconnects; tooltip SQL is escaped; times are labeled
  UTC; the WS is gated by a per-session token.
- **T7 — release engineering & matrix.** A nightly containerized matrix
  (rockylinux:8 / rockylinux:9 / ubuntu:24.04) builds, unit-tests, and runs the
  capture smoke on each distro — EL8 exercising the static libbpf/bpftool
  bundling path that gating CI never compiled. A committed golden trace fixture
  pins on-disk format compatibility (decode + checksum in CI). A client/server
  **version handshake** makes deployment skew visible (server reports its
  version + protocol in `info`; the client warns loudly, never refuses). Added
  `RELEASING.md` and this changelog; removed stale build artifacts from git.

- **T3 — escalation budget & trigger quality** (ESC-1..12).
  Committed-remainder-aware extension charging with a mid-window budget clamp;
  de-escalation flushes open intervals as exact end-of-window records;
  live-accumulator dedup during escalation; a minimum-activity guard on the
  lock rule.
- **T8 — exact measured CPU.** Per-backend on-CPU time is now measured exactly
  from a `tp_btf/sched_switch` accumulator (replacing tick-quantized
  `sum_exec_runtime` and wait-gap inference), gated on kernel BTF with a loud
  gap-inference fallback when it is absent. The time model decomposes into
  `CPU*` (query on-CPU), `Off-CPU*` (the measured runqueue/unaccounted
  residual — the 10046 "unaccounted-for time" analogue), and Σ waits, which
  conserve to DB Time exactly (asserted by `test_data_offcpu_identity`).
  On-CPU spin *during* a wait stays labeled under its wait class (LWLock etc.)
  rather than folded into an anonymous CPU bucket, so lock-contention burn
  stays diagnosable. Measured CPU is cross-checked against `/proc` on-CPU
  within ±20%; the sched_switch program's overhead is within measurement noise
  (110-126k ctx-switches/s). Two live-view capture holes found in OS/PG matrix
  validation are fixed: a fork-caught compute backend's *ongoing* on-CPU
  interval was suppressed by a stale double-count guard (pinned query read ~0
  CPU, `%DB` nonsense), and a pre-existing backend the one-shot startup scan
  missed was never recovered (`pgwt_recover_unattached_backends`) — both
  waitless pure-CPU commands that read `CPU* = 0`. Specs in
  `docs/S3_SCHED_SWITCH_CPU.md`, `docs/ROADMAP_AND_STATUS.md`,
  `docs/AAS_SEMANTICS_DECISION.md`.

Validated live on the EL9 / EL8 / Ubuntu 24.04 matrix (PG 13/16/17/18):
`run_all.sh --require-live` 64/0/3 on each, plus the containerized nightly.

## [0.12] — 2026-06-21

PostgreSQL 13 support and tiered-capture correctness fixes.

- PG13 wait-event capture via `MyProc` resolution + a runtime offset-validation
  guard that refuses to attach on a wrong offset, plus PG13 wait-event name
  tables (PR #27).
- PG13 query attribution through `pg_stat_statements` — the `standard_ExecutorStart`
  uprobe and query-text capture, with pgss gating (PRs #28, #31).
- Sampled mode feeds the live accumulator, so `--view` shows captured waits in
  the default tiered mode (PR #30, a field-reported gap).
- README/INSTALL reframed around tiered capture, escalation, the control socket,
  and the corrected OS + PostgreSQL support matrix (PR #29).

## [0.11] — 2026-06-19

Rocky 8 / RHEL 8 support and idle-event honesty.

- Rocky 8 / RHEL 8 build and runtime support: on EL8 (libbpf 0.5.0, pre-USDT)
  the Makefile builds a pinned libbpf + bpftool from source and links libbpf
  statically; handles the non-PIE PGDG postgres binary (PR #24 — the load-base
  resolution the later #24-class fixtures guard).
- Idle-but-visible events (e.g. `Client:ClientRead`) render `—` for %DB instead
  of a bogus bar; non-idle %DB sums to ~100% (PR #25).
- Live-suite cleanup: `bc`-free overhead math, de-flaked `test_cross_validate`
  and `test_client_wait` (PR #26).

## [0.10] — 2026-06-18

Tiered capture becomes the default.

- Tiered mode (always-on low-overhead sampler with on-demand full-fidelity
  escalation) is now the default capture mode, justified by a
  cross-validation test (PR #23).
- Anomaly-triggered escalation: an AAS-vs-baseline + lock-fraction rules engine
  opens bounded full-fidelity windows automatically (PR #19, A5).
- Fidelity-aware UI: sampled shading, the escalate control, unavailable panels,
  and a daemon self-metrics panel, wired through the control socket (PR #21, B5).
- A6 cooperative-provider interface stub (PR #20); B3 pure-builder view
  migration completed and legacy adapters removed (PRs #17, #18).
- B4 visual-regression snapshot suite with CI-generated baselines (PR #22).

## [0.1] – [0.9]

Pre-history: the initial BPF wait-event tracer, the on-disk trace format v2,
the pgwt-server replay engine, the web investigation UI, and the REWORK_PLAN
architecture (per-task watchpoints, tiered capture, read-time exact-wins merge,
pure-builder UI). See the git tag history for detail.
