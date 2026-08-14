# pg_wait_tracer — Roadmap &amp; Status (consolidated)

> **Single source of truth for what is built and what remains.**
>
> This file consolidates and **replaces** the previously-scattered planning
> documents — `REWORK_PLAN.md`, `TRUST_MILESTONE_PLAN.md`, `ROADMAP.md`,
> `DEVELOPMENT_PLAN.md`, `REVIEW_AND_PLAN.md`, `TRACING_ANALYSIS_PLAN.md`,
> `CLI_REDESIGN.md`, `QT_PM_ANALYSIS.md`, `T8_MEASURED_CPU_PLAN.md`,
> `T8_MEASURED_CPU_REVISION.md`, and `FUTURE_WORK.md`. Nothing from those was
> dropped: every planned item, design decision, and status is preserved below,
> including completed ("done") work.
>
> Still-authoritative **reference/spec** docs are kept separate and cross-linked
> rather than folded in: `S3_SCHED_SWITCH_CPU.md` (the shipped measured-CPU
> spec), `AAS_SEMANTICS_DECISION.md`, `T2_IOWORKER_STUDY.md`, `TRACE_FORMAT.md`,
> `DAEMON_ARCHITECTURE.md`, plus `CHANGELOG.md` / `RELEASING.md` / `README.md`
> / `INSTALL.md`.
>
> _Last updated: 2026-07-21 (v0.13 shipped)._

## Release map

| Release | Date | Contents | Detail |
|---|---|---|---|
| v0.1–v0.9 | Feb–Jun 2026 | Foundation: core BPF tracer, trace files, daemon, web client, analysis endpoints (Dev-Plan sprints 1–14; ROADMAP phases 1–8/A–H; code review + arch + test plan). | Part 3 |
| **v0.10** | 2026-06-18 | Tiered-fidelity **rework** (Tracks A/B/E): sampled-always-on daemon + on-demand full-fidelity escalation; ECharts-only UI restructure. | Part 1 |
| **v0.11** | 2026-06 | **EL8 / RHEL 8** support (rework Track C goal; static libbpf/bpftool bundling). | Part 1 |
| **v0.12** | 2026-06-21 | **PostgreSQL 13** (rework Track D: MyProc resolution + pgss query attribution). | Part 1 |
| **v0.13** | 2026-07-21 | **Trust Milestone** (T0–T7) + **exact measured CPU** (T8 / sched_switch). | Part 2 |

## How to read this document

- **Part 1** — the tiered-fidelity rework (Tracks A–E), shipped v0.10–v0.12.
- **Part 2** — Trust Milestone (T0–T7) + measured CPU (T8), shipped v0.13.
- **Part 3** — foundation &amp; early plans (v0.1–v0.9): sprints, roadmap phases, code review.
- **Part 4** — analysis features, the CLI redesign, and the long-range research vision.
- Every item carries a **status badge**: **[DONE vX.Y]**, **[PARKED]**,
  **[LEFT]** (planned, not built), **[SUPERSEDED]**, **[DROPPED]**.

---

## Executive summary — Done vs. Left

**Four bodies of work; three fully shipped, the fourth is the parked/future backlog.**

### ✅ Done (shipped through v0.13)

- **Tiered-fidelity capture** — sampled always-on (~0% PG overhead), on-demand
  full-fidelity escalation, control socket, trace format v2→v3,
  anomaly-triggered escalation, escalation-budget accounting (rework A0–A6; T3).
- **Web UI** — ECharts-only, pure builders + builder/visual-snapshot tests,
  fidelity-aware panels + escalate button, transport-trust hardening (B1–B5; T6).
- **The whole Trust Milestone (T0–T7)** — real-PG CI safety net; fidelity/summary
  honesty; decomposed AAS (on-CPU first-class); capture/sampler + durability
  hardening; client-transport trust; release engineering + nightly OS matrix.
- **Exact measured CPU (T8)** — per-backend on-CPU time from a `sched_switch`
  accumulator; `CPU*` / `Off-CPU*` / Σwaits conserve to DB Time; on-CPU spin
  stays under its wait label. Plus the two live-view capture bugs fixed at
  release: #51 fork-caught ongoing CPU (`has_closed_data`) and #52 straddle
  scan-race (`pgwt_recover_unattached_backends`).
- **Platforms** — EL8 (kernel 4.18, measured-CPU validated), EL9, Ubuntu 24.04;
  **PostgreSQL 13**; query-text capture; ASH-style query views; wait-transition
  / variant / fingerprint / concurrency / lock-chain / interference **endpoints**.

### 🔲 Left (not built)

> **⚠️ CRITICAL — sampled-tier overhead is REAL (~20–30%); root-caused; staged fix underway (investigation done 2026-08-13; fix stage 1 merged #83).**
> The documented "sampled tier ≈0% / TPS within noise" is **FALSE** for the
> always-on sampled tier. Measured on the box (pgbench, PG13/17/18): the sampled
> tier costs **−18% to −30% TPS** plus a per-statement latency tax that crosses
> **+10% by ~500 TPS read-write** — present even at 1 client, so it is a FIXED
> per-transaction cost, not a benchmark/high-throughput artifact.
> **Root cause:** the always-on sampled path attaches two per-query uprobes
> (`pgstat_report_query_id`, `pgstat_report_activity`) that fire **5/tx (RO) to
> 33/tx (RW) at ~7.5 µs/trap** in the PostgreSQL BACKEND — the `int3`/single-step
> trap machinery itself, not the eBPF body (~0.17 µs). Present **since v0.10**,
> MASKED during the original measurement by a PIE file-offset bug that silently
> stopped the probe from firing; the "daemon CPU 0.6%" figure was correct but
> measured only the tracer PROCESS and missed the kernel trap time charged to the
> PostgreSQL backends (the overhead test forces `--mode full` and never A/B-tested
> sampled). **Fix (fail-safe, staged):** read `st_query_id`/`st_state` at each 10 Hz
> tick from `PgBackendStatus` (external reads, no traps) and scope the uprobes to
> full/escalation windows only. Measured achievable: **<1%** (near the original
> <0.5% goal). PG13 (no in-core query_id, gate measured ~15–23%) uses
> `st_activity_raw` + a synthetic normalized-text grouping key; sampled query TEXT
> becomes pgss-normalized (accepted). **Progress:** stage 1 (offset discovery +
> fail-safe validation) **merged (#83)**; stages 2–5 (at-tick reader/gate swap →
> tiered attach lifecycle → PG13 text path → permanent sampled-overhead A/B gate)
> in progress. The "≈0%" claims elsewhere in this doc are **stale** pending those
> stages — see this note.

**Real feature gaps (defined, never built):**
- **PostgreSQL 14 / 15 / 16** — only PG13 shipped; README says "14-16 not yet".
- **Query trace-context / OpenTelemetry export** (from PR #33, in review) — W3C
  `traceparent` extraction linking a client app-request to its DB execution, a
  native OTel/Jaeger JSON exporter, and a span model over the shipped U3
  per-execution waterfall. DB-safe (daemon-side only, no PG overhead). Review
  (2026-08-13) found fixable correctness bugs — exporter wait-class decode,
  in-progress-export crash, `traceparent` bounds/W3C validation,
  mock/`protocol-drift` sync; merge after the author's fixes.
- **Plan-node algebra / plan-attributed tracing** (from PR #34; subsumes the old
  `st_plan_id` plan-id-capture item) — waits attributed to plan operators
  (`active_node_id`), live plan operator-tree capture, a hierarchical plan-node
  waterfall + Jaeger flamegraph. DECLINED as-is (2026-08-13) pending a REDESIGN:
  the current impl is inert (plan capture unwired), NodeTag tables are wrong on 5
  of 6 majors, per-`query_id` plan identity is invalid, the foreign-memory walker
  is unsafe (unbounded recursion on cyclic pointers), and it adds an O(n²)
  `top_queries` server regression; the intended per-tuple `Exec*` uprobe approach
  measured/estimated **150–322× per-query overhead**. Redesign constraints:
  out-of-band per-EXECUTION capture (not per-tuple probes, not per-`query_id`
  identity), offsets derived/verified against live structs per major, a
  bounded/cycle-safe walk, and NEVER active in the always-on sampled tier.
- **B6 analysis-view UIs** — per-execution waterfall, latency scatter, transition
  matrix; plus deferred UI for concurrency/burst markers and lock-chain /
  interference (the compute **endpoints** exist; only the UI is missing). Now
  sequenced inside **Track U — UI: OEM loop + instrument** (Part 4, adopted
  2026-07-31): P1/P2 fixes → gallery → chassis/drill/URL → camera-lite →
  gated uPlot instrument pane → B6-on-chassis.
- **Prometheus exporter** — metric names are Prometheus-ready; no exporter.
- **CLI `--format json` / `csv`** — dropped; JSON ships via the web protocol.

**Measured-CPU follow-ons (the canonical FUTURE_WORK list, post-v0.13):**
- Multi-window `%DB` drift (cosmetic — visible parents sum 106–110% under load).
- Live view: account the *open* interval of a repeated **wait** (`has_closed_data`
  guard, now CPU-only, still suppresses ongoing waits).
- Within-wait **on-CPU-spin vs off-CPU-blocked** split (keeps the label).
- **Off-CPU analysis** (voluntary/involuntary switches, runqueue latency).
- **Sampler feed** from the exact CPU accumulator (tighten sampled-tier CPU).
- **Parallel-worker CPU roll-up** to the leader's query.

**Parked by design:**
- **Cooperative provider implementation** — interface frozen (A6), body a stub
  (extension track).
- **Track C perfbuf/BTF fallbacks** (C2/C3) — for pre-EL8.10 kernels without
  backported BTF; parked indefinitely.

**Research / long-range (aspirational, uncommitted):**
- The entire **Queueing-Theory / Process-Mining** program (Little's Law / W(N),
  conformance checking, MVA prediction, the "Investigation Canvas" rule engine).
- **HMM anomaly detection** (never built; shipped `anomaly.c` is a simpler trigger).
- **PG-core-patch extension track** (`pg_wait_event_timing`).

_Full detail with evidence and status for every item is in Parts 1–4._

---
## Part 1 — The tiered-fidelity rework (Tracks A/B/C/D/E)

The rework turned the tracer from a single-mode, full-fidelity watchpoint sampler into an always-on tiered monitor (cheap sampled tier + on-demand/anomaly-triggered escalation to exact watchpoints) with a testable, extensible web UI, plus tracks for older OSes (Rocky 8) and older PostgreSQL (13–16).
Its **core (Tracks A0–A6, B1–B5, E1–E3) shipped in v0.10** (master c027d3c, 2026-06-18, PRs #6–#23); Track C (EL8) landed in v0.11; Track D (PG13) in v0.12; several rework decisions were later hardened by the v0.13 Trust Milestone.

**Source status header (verbatim intent):** the plan itself was marked `✅ COMPLETE (all phases merged — master c027d3c, 2026-06-18)`, dated 2026-06-11 (completed 2026-06-18). Two tracks were independent until Phase A5/B4, where the UI grows fidelity-aware features atop the restructured codebase.

**Completion summary facts (must not be lost):**
- Core rework complete = tiered capture (A0–A6) + UI restructure/testability/fidelity UI (B1–B5) + Track E live-suite cleanup, all merged and verified end-to-end. **CI green** (build-and-unit, web-ui incl. chaos+soak, snapshots, protocol-drift) **and a full live `run_all.sh`** on the test box = **46 passed / 0 failed / 1 skipped** (the skip = web-UI snapshot job, which the CI snapshots job covers authoritatively).
- **Still open in the plan (NOT done in the rework):** B6 (new analysis views), Track C (Rocky 8 / RHEL 8), Track D (older PostgreSQL 14–16). Marked future work; their sections remained the design for them. (Track C later shipped v0.11 by a different mechanism than C1–C3 describe; Track D shipped PG13 in v0.12.)
- **Default capture mode became `tiered`** — always-on sampled tier + bounded/budgeted on-demand + anomaly-triggered escalation to full watchpoints. **Proven results:** sampled tier ≈ **0% impact on PostgreSQL** (daemon CPU 0.6% @10 Hz; pgbench TPS within noise) **[⚠️ STALE — this is WRONG; see the CRITICAL sampled-tier-overhead note in the Executive Summary: the sampled tier actually costs ~20–30% TPS from two per-query uprobes; the "daemon CPU 0.6%" figure measured only the tracer process and missed the backend trap time, and the original ≈0% was masked by a PIE bug. Staged fix underway (stage 1 merged #83).]** vs the full watchpoint tier's **29–30%** (documented range). **Cross-validation: 0.9pp** worst-case event-share disagreement between sampled and exact over the same window @10 Hz, 5/5 top-5 overlap.
- **Merged PRs (authoritative phase→PR map):** #6 B1 · #7 A0 · #8 Lock-naming fix · #9 Track E · #10 ClientRead idle-but-visible · #11 B2 · #12 A1 · #13 A2 · #14 B3p1 · #15 A3 · #16 B3p2 · #17 A4 · #18 B3p3 · #19 A5 · #20 A6 · #21 B5 · #22 B4 · #23 default→tiered.
- **Bugs found & fixed along the way:** Lock-class subtype mislabel on PG17+ (relation→advisory, real product bug — #8); the ClientRead idle/visible semantics decision (#10); pre-existing flaky live-test thresholds (test_overhead 15%-gate vs documented 6–30%; test_client_wait load-dependent DB-Time threshold) corrected during #23 validation.
- **Deferred (not rework-blocking):** cooperative provider **IMPLEMENTATION** (extension track — A6 only froze the interface); historical-escalation annotations + mixed-fidelity sub-range shading in the UI (small server additions); idle-ClientRead %DB cosmetic (later fixed v0.11 / PR #25).

**Goals.** **(A)** Run 24/7 with <0.5% overhead (sampled tier), escalate to full watchpoint tracing for bounded windows — manually or on anomaly — so "the data from 3am is already on disk." **(B)** No more manual UI testing after every change: regressions caught by CI; most UI logic testable without a browser; new views additive.

**Non-goals.** No userspace rewrite (C daemon/server, Go client stay). No new charting framework — **ECharts becomes the only chart library**; ApexCharts (then rendering the main AAS chart) is dropped in B3 and its drag-select-window UX replaced by a custom selection overlay (~100 lines, testable). No client-side compute relocation (server keeps computing views). Cooperative tier (PG patch/extension hooks) is interface-only here; implementation belongs to the extension track. Prometheus exporter for AAS/sampled metrics is planned for later, out of scope — but A3's view semantics and the A0 control socket must not preclude it (stable metric names, queryable aggregates).

**Workflow / process facts:** branch + PR per phase, user merges; two phases land at a time (one per track, in parallel); subagents work in isolated worktrees (a parallel pair without worktree isolation collided once, recovered with no lost work — isolation mandatory thereafter). **Testing reality:** CI (GitHub Actions) runs unit + synthetic + web-UI-vs-mock + protocol-drift; it **cannot** run live capture tests (no PG, no hardware watchpoints on runners). Live tests run on **dmitry-micro-test** (Hetzner cx23, Rocky 9.7, kernel 5.14, PG 18.4). **Definition of done for any capture phase = a green live run on the box, not just green CI.** Trace-format and Prometheus decisions locked in during execution: trace format is **v2-only** (no deployed installs → A1 drops v1 reader compat; version field stays for future v3); Prometheus exporter is a planned future consumer only (A0 metric names already snake_case + unit-suffixed).

---

### Track A — Tiered-Fidelity Capture  **[DONE v0.10]** (A0–A6 all merged)

#### A.0 Target architecture
A single daemon holds: a **backend registry** (pid→PGPROC addr, query) fed by fork/exit lifecycle; **capture providers** — `sampled` (24/7), `full` (windows), `cooperative*` (stub); a **control socket + self-metrics** plane; an **escalation engine**; and an **event_writer v2** (typed blocks). `pgwt-server` connects to the control socket to escalate / read status. ASCII data-flow: PG backends → backend registry → capture providers → event_writer v2; pgwt-server ↔ control socket; escalation engine drives provider tiering.

**Provider contract** — one interface, three implementations, one schema:
```c
struct pgwt_capture_provider {
    const char *name;                  /* "sampled" | "full" | "coop" */
    enum pgwt_fidelity fidelity;       /* SAMPLED | EXACT */
    int  (*start)(struct pgwt_daemon *);
    int  (*stop)(struct pgwt_daemon *);
    int  (*poll)(struct pgwt_daemon *);   /* drain into writer */
    void (*self_metrics)(struct pgwt_metrics *);
};
```
The existing watchpoint path becomes the `full` provider with **no behavioral change**. `sampled` is new. `coop` is a stub whose contract is fixed now so the extension track can fill it later. (Delivered as `src/provider.h`, `src/provider_full.c`, `src/sampler.c/h`, `src/provider_coop.c/h`.)

#### A.1 Design decisions (rationale preserved)

**D1 — Sampled tier is pure userspace, no BPF in the hot path.** The daemon already resolves/tracks each backend's `PGPROC->wait_event_info` address (`src/backend.c:74-189`; bootstrap→init at `src/backend.c:191-265`). The sampler reuses that registry:
- One `process_vm_readv()` call per tick with up to **1024 remote iovecs** (4 bytes each, one per backend) reads every backend's `wait_event_info` in a **single syscall**. PG's main shm segment is inherited anonymous mmap, so addresses are identical across PG processes — read via any live backend pid (fall back to **per-pid `pread`** on the rare `EFAULT` from a concurrently-exiting backend).
- Zero cost on PG itself (no traps, signals, or backend-executed instructions). Daemon cost at 1000 backends × 100 Hz = one syscall + ~4 KB copy per tick — well under 0.5% of one core.
- `query_id` per sample joined from the registry; in daemon mode the existing `on_report_query_id` uprobe (`src/bpf/pg_wait_tracer.bpf.c:387`) maintains pid→query_id; the sampler reads the registry, not PG memory. (A later no-BPF fallback can read `st_query_id` from BackendStatusArray — not in first cut.)
- Backend lifecycle: daemon mode keeps fork/exit BPF tracepoints. A degraded no-BPF mode (periodic `/proc` rescan) is a follow-up, not this rework.
- **Default rate 10 Hz, configurable 1–1000 Hz (`--sample-rate`);** the A4 cross-validation test decides whether the default moves.

**D2 — Trace format v2 (typed blocks, not sentinel hacks).** Version field exists (`PGWT_TRACE_VERSION`, `src/event_writer.h:15-16`); bump to **v2**: block header gains `block_type` (`TRANSITIONS` | `SAMPLES`) and `sample_period_ns` (0 for transition blocks); sample blocks use the columnar layout minus non-applicable columns (no `old_event`, no `duration_ns`) = timestamp (delta varint), pid, event, query_id. **v2-only** (decision 2026-06-15: no deployed installs → no v1 burden; reader/writer speak v2 exclusively; version field stays so v3 is graceful; old local traces regenerated, not migrated). **Explicitly rejected:** encoding samples via `PGWT_MARKER_*`-style sentinel values inside v1 (builds a lie into the schema every future reader must know).

**D3 — Compute layer: fidelity-aware views, exact-wins merge.** `compute.c` assumed exact intervals (`duration_ns` consumed directly, AAS bucketing at `src/compute.c:221-243`). Each view declares `required_fidelity`:
- **SAMPLED is enough:** time_model, system_event, session_event, query_event, AAS, active. Estimator: each sample of a non-idle session contributes `sample_period_ns` to its (event, session, query) cells — standard ASH math.
- **EXACT required:** histogram, transitions, lock chains, interference. Over a window with no transition blocks these return `{"unavailable": "requires full-fidelity data"}` — never a silent empty result.
- **Merge rule for mixed windows:** time ranges covered by transition blocks are authoritative; sample records inside those ranges are dropped at read time (block time-range comparison via header `first_ts`/`last_ts`), preventing double counting while the sampler keeps running through escalation windows (it does — stopping would create gaps if escalation crashes).
- Every view response carries a `fidelity` field per time range so the client renders the distinction (B5).

**D4 — Control socket (a control plane).** Today's only runtime interface is SIGTERM (`src/daemon.c:145-151,384-389`). Add a unix domain socket (`{trace_dir}/pgwt.sock`, mode 0600), JSON-line protocol: `{"cmd":"status"}` → mode, uptime, backends tracked, current tier; `{"cmd":"escalate","duration_s":60,"reason":"manual"}` → ack/deny; `{"cmd":"deescalate"}`; `{"cmd":"metrics"}` → events/s, samples/s, ringbuf drops, attach failures, estimated overhead, escalation budget remaining. `pgwt-server` proxies these to the client (new `control` command in its JSON protocol) so the browser gets an "Escalate 60s" button over the same SSH channel — no new network path.

**D5 — Escalation engine (bounded, budgeted, fast).**
- **Attach speed:** skeleton + ringbuf loaded at daemon start (idle maps cost nothing); escalation only runs the watchpoint attach loop — ~3 syscalls/backend (`src/perf_event.c:19-58`), a few ms for 1000 backends. PGPROC addresses from the registry; `state_map` pre-seeded exactly as `pgwt_scan_existing_backends()` (`src/backend.c:144-176`) so initial wait states aren't lost.
- **Bounded windows:** every escalation has a hard duration; expiry detaches watchpoints even if the requester vanished.
- **Budget:** `--escalation-budget` (default **300s of full fidelity per hour**). Manual requests over budget are denied with reason; anomaly triggers respect it silently. This bounds worst-case production overhead by configuration, not hope.
- **Anomaly triggers (A5):** rules on the sampled stream — `AAS > k × rolling_baseline for N ticks`, `lock-class fraction > threshold`. Hysteresis + cooldown so a flapping metric can't burst-burn the budget; every trigger writes a structured "escalation event" into the trace so the UI can show *why* full data exists.

**D6 — Self-observability.** `metrics` (D4) is production-critical once 24/7. Estimated overhead = trap count × measured per-trap cost (calibrated at startup with a synthetic write loop) + sampler CPU from `/proc/self`. Exposed via control socket and recorded into the trace as periodic marker records.

#### A.2 Phases

**Phase A0 — Control socket + self-metrics (groundwork)  [DONE v0.10]** (2026-06-13, merge b1ef3c4, PR #7; rebased onto B1). *Evidence:* `src/control.c/h`, `src/server.c`, `src/daemon.c`, `tests/test_control.sh`. Scope: D4 socket with `status`/`metrics` only; daemon metrics counters; pgwt-server `control` proxy; `--dump` prints daemon status if socket present. **Live-validated `test_control.sh` 16/16** (socket, status/metrics JSON, counters rising under load, error handling, concurrent clients + disconnect survival, pgwt-server proxy, `--dump` block, SIGTERM clean unlink). Metrics shipped: `events_total`, `events_per_sec`, `lifecycle_events_total`, `wp_attach_failures_total`, `trace_events_written_total`, `trace_bytes_written_total`, `backends_tracked`. **Ringbuf drops intentionally NOT reported** (no BPF-side drop map until A2; `control.h` documents this; A2 must add + surface it). Wiring `test_control.sh` into `run_all.sh` deferred at merge → done in Track E (E3).

**Phase A1 — Trace format v2  [DONE v0.10]** (PR #12). *Evidence:* `src/event_writer.c/h`, `src/event_reader.c/h`, `src/summary_*.c`, `tests/gen_test_traces`. Scope (as planned): D2 typed blocks, v1-compat reader, writer emits v2, `gen_test_traces` emits both, pgwt-server + replay read both; round-trip unit tests; old traces replay identically. **Delivered variant (SUPERSEDES the planned v1-compat reader):** trace format is **v2-only** (decision 2026-06-15) — no deployed installs, so A1 drops v1 reader compatibility; version field retained for a future v3. *(Forward note: T8/v0.13 later added trace format v3 for measured-CPU — see Trust Milestone chapter.)*

**Phase A2 — Sampled provider  [DONE v0.10]** (PR #13). *Evidence:* `src/sampler.c/h`, `src/provider.h`, `src/provider_full.c`, `src/daemon.c`, `src/backend.c`. Scope: D1 sampler behind the provider interface; extract the existing watchpoint path into the `full` provider (mechanical, no behavior change); `--mode sampled|full|tiered` flag (default `full` = then-current behavior). Tests: sampler unit test vs a fake registry; live `--mode sampled` vs pgbench (sane sample rates); daemon CPU at 100 Hz × N backends below budget. Acceptance: `--mode sampled` runs without watchpoints, writes sample blocks; `--mode full` byte-identical to prior behavior.

**Phase A3 — Fidelity-aware compute  [DONE v0.10]** (PR #15). *Evidence:* `src/compute.c`, `src/server.c`, `src/event_stream.c`. Scope: D3 sample estimators for the six sampled-capable views; `required_fidelity` declarations; exact-wins merge; `fidelity` field in every response; `--dump` annotates fidelity. Tests: synthetic tests with known sample streams → exact expected estimates (`test_data_*.py`); mixed-window merge tests (no double counting); EXACT-required view over sampled-only window → explicit unavailable. Acceptance: a sampled-only trace yields correct time_model/system_event/AAS; histograms/transitions report unavailable, not empty. *(Forward note: T1/v0.13 hardened this merge — escalation markers become the exact-wins coverage authority, summary fast path made coverage-aware and no longer hardcodes `"fidelity":"exact"` over sampled data, markers filtered from aggregation, latency columns gated under sampled fidelity, one clock domain.)*

**Phase A4 — Tiered orchestration + cross-validation  [DONE v0.10]** (PR #17; default flip PR #23). *Evidence:* `src/escalation.c/h`, `src/escalation_budget.c/h`, `src/daemon.c`, `src/control.c`. Scope: D5 minus anomaly rules. `--mode tiered`: sampler always on, escalation via control socket, bounded windows, budget enforcement, state_map pre-seeding on escalate. Tests: escalate/expire/deny-over-budget integration tests; **the cross-validation test** — tiered under pgbench, escalate 60s, compare sampled estimators vs exact over the same window (top-5 events match, shares within ±10% at 10 Hz), which both validates estimators and empirically sets the default sample rate. Acceptance: tiered survives PG restarts (existing re-attach), escalation windows produce mixed traces all views handle, default mode can flip to `tiered` (it did — PR #23). *(Forward note: T3/v0.13 added committed-remainder-aware extension charging with a mid-window budget clamp, de-escalation flushes open intervals as exact end-of-window records, live-accumulator dedup, minimum-activity guard on the lock rule.)*

**Phase A5 — Anomaly-triggered escalation  [DONE v0.10]** (PR #19). *Evidence:* `src/anomaly.c/h`, `tests/test_anomaly.c`, `tests/test_anomaly_live.sh`, `tests/dump_markers.c`. Delivered: AAS-vs-baseline + lock-class-fraction rules on the live sampled stream, evaluated each sampler tick. **Rolling EWMA baseline** (NOT updated while AAS is anomalous, so a sustained incident can't raise the bar and silence the rule). **Hysteresis** (sustained-N-ticks) + **cooldown** so a flapping metric can't burst-burn the budget; an anomaly while escalated **EXTENDS** the window via A4's `pgwt_escalate` extend path. Over-budget anomaly triggers are dropped **SILENTLY** (log only) — unlike manual escalate which denies. Every near-trigger is logged for data-driven tuning. Anomaly windows carry `PGWT_ESC_REASON_ANOMALY` in trace markers (distinct from manual). **Config flags** (tiered-only, conservative defaults): `--anomaly-aas-factor` (3.0), `--anomaly-aas-ticks` (3), `--anomaly-lock-fraction` (0.30), `--anomaly-cooldown-s` (120), `--anomaly-window-s` (60). **Counters via control-socket metrics:** `anomaly_fires_total`, `anomaly_near_total`, `anomaly_dropped_budget_total`, `anomaly_dropped_cooldown_total`, `anomaly_baseline_aas`. Tests: `test_anomaly.c` **74/74** pure-rule checks (sustain, cooldown, baseline-protection, budget boundary, metrics derivation, combined fire), wired into run_all.sh + CI; live `test_anomaly_live.sh` **11/11** — a 12-backend `LOCK TABLE` storm auto-escalated (tier=escalated, START reason=anomaly), captured transitions, auto-de-escalated (END reason=expired), cooldown blocked immediate re-fire; `dump_markers.c` decodes reason-tagged markers. **Implementation note vs plan:** rule state + rolling baseline live in the new pure, unit-testable `src/anomaly.c/h` (baseline derived from the sampler's per-tick batch in `sampler.c`), NOT in `escalation.c`/`compute.c` — so no `compute.c` change was needed. Note for B5: anomaly-reason escalations are already in the trace (marker reason byte = 1) and metrics; the UI just renders them.

**A5 follow-up — AAS-1 CPU-storm escalation ✅ [RESOLVED 2026-08-12 — PRs #76/#77/#78/#80/#82]**. The A5 CPU-storm escalation gate (`phase_cpu_storm_escalation`) was intermittently failing on 2-vCPU CI (~18/25 under contention): a real ~4-session CPU storm did not escalate because the EWMA baseline had drifted up (non-idle warmup), so the multiplicative `aas > 3×baseline` rule (threshold ≈6) never crossed the storm's ≈4. Two misdiagnoses were refuted with evidence first — a stale local `discovery.o` build artifact (intermittency-within-one-build ruled it out), and a sampler/phase-locking hypothesis (a captured per-tick trace showed AAS was measured correctly). Every baseline-relative fix then failed on the *noisy integer* AAS signal: an additive secondary rule over-fired and drained the 300s/hr budget (adversarially proven), and a variance-aware (MAD) rule with a strict consecutive-tick sustain missed a jittery storm entirely. **Reframe (decided via a realistic-noise design study):** AAS-1 is a CPU-**saturation** guard — sustained CPU demand near effective capacity — not an "AAS vs baseline" detector; this is baseline-independent and dissolves the 2×-at-the-integer-noise-floor ambiguity. **Built in three adversarially-reviewed, separately-gated stages:** (#76) per-target sampler read validity so a failed read can no longer be misread as `we==0`/CPU; (#77) `src/effective_cores.c` capacity resolver (postmaster affinity ∩ min hierarchical cgroup quota, fractional-aware, `--anomaly-cpu-capacity` override, UNKNOWN sentinel — never guesses); (#78) a CPU-demand CUSUM `S=max(0, S+dt·(min(cpu_aas/C,1.25)−0.80))`, fire at `S≥1.5`, alongside the unchanged AAS/lock rules. An Opus adversarial pass caught two HIGH regressions on the CUSUM (a chronically-busy box re-firing and monopolizing the whole budget; zero margin against a capacity under-estimate) — fixed with **fire-once-per-episode** (drain-before-rearm: re-arm only on a trusted `u<k` recovery), an absolute `cpu_aas` floor + an affinity-only capacity margin, a coverage-return reset (no stale fire), and a `PGWT_NEAR_CPU_SUSTAIN` log. New escalation reason `PGWT_ESC_REASON_CPU_SATURATION` + metrics (`anomaly_cpu_saturation_fires_total`, `effective_cpu_capacity_cores`/`_source`); flags `--anomaly-cpu-cusum-{k,h,cap,disable}`, `--anomaly-cpu-min-aas`, `--anomaly-cpu-margin`. **Confirmed:** `test_anomaly` 170/170 on noisy-integer streams (storm fires C=4≈8s/C=2≈4s, never on C=16, five one-hour normal workloads 0 grants/0 drops), and the rewritten deterministic capture-smoke storm test asserts `escalation_reason=cpu_saturation` — green on PG 13/16/17/18.
  **REOPENED then FULLY RESOLVED 2026-08-12 (#80, #82) — two further root causes surfaced only on busy PG17/PG18 CI (the PG13 box could not reproduce either).** First, #80 hardened the CUSUM's coverage-gap logic: brief low-coverage flaps no longer wipe accumulated `S` (a sustained blind interval still resets it, bounded-hold ≈2s derived from the actual sample rate; fresh-UNKNOWN capacity self-contains the gap). Then two distinct misses remained, both fixed in **#82**: (1) *the test workload never sustained saturation* — `CpuStorm(3).fire(reps=800)` (800 discrete short `generate_series` statements) was diluted by temp-file I/O (`IO:BufFileWrite`, ~11–15% of ticks on the box) and by query-boundary/idle gaps on CI, so demand oscillated around capacity and the CUSUM (which drains on below-reference ticks) never reached `h`. Replaced with a bounded, pure-CPU PL/pgSQL `fire_continuous` burn that asserts all three PID-tagged backends stay `active`+waitless on every ground-truth poll, so the test *proves* a sustained incident. (2) *the coverage gate was over-broad* — `cpu_coverage_ok = (read_invalid_last==0)` is a GLOBAL flag, so a single failed read of any target (a background/transient backend churning on a busy runner) froze the CUSUM even though the storm backends read fine and `cpu_aas` was a sustained 3 (per-tick trace on PG18: `coverage_ok=0 read_invalid_last=2/14 … cpu_aas=3 … cpu_cusum=0 gap_ticks=120`, with `capacity_changed=0 guard_blocked=0` excluding the other gates). Fixed with **asymmetric, saturation-monotone coverage**: because #76 excludes failed reads, missing targets only bias `cpu_aas` DOWNWARD, so an incomplete tick whose observed `u ≥ reference` is a trusted saturation *lower bound* — it accumulates (never faster than a complete read of the same instant) and resets the blind-gap counter; an incomplete tick *below* reference holds `S` (never drains — an undercount cannot erase evidence) and advances the #80 gap counter; complete-under-reference still drains; the fire predicate now requires observed saturation on the firing tick (anti-stale). New unit regression `test_cpu_cusum_incomplete_saturation_monotone` covers all four directions (fires on incomplete-but-saturated — and *fails against the old logic*; anti-false-drain; #80 sustained-blind wipe; anti-stale). An **Opus adversarial gate proved the over-fire concern impossible** (the fix can never fire earlier or on weaker evidence than complete coverage; the old freeze was a false-*negative*). **Meta-fix:** PG17.10 (port 5417) + PG18.4 (port 5418) were installed on the Hetzner box alongside PG13 — the PG13-only box is *why* these version-specific misses reached CI — and the escalation now fires **8/8 on PG13/17/18** on the box, with master green on PG13/16/17/18 in CI.

**Phase A6 — Cooperative provider stub (interface freeze only)  [DONE v0.10]** (PR #20). *Evidence:* `src/provider_coop.c/h` (confirmed stub), `tests/test_coop.c`. Delivered: a `coop` provider implementing the `struct pgwt_capture_provider` vtable, advertising `PGWT_FIDELITY_EXACT`, whose `start()` cleanly reports "cooperative provider not available in this build" and returns -1 so the daemon aborts startup with a clear message (no crash). `enum pgwt_mode` gains `PGWT_MODE_COOP`; `--mode coop` parses to it (`src/pg_wait_tracer.c`); `pgwt_daemon_init()` selects the stub (`src/daemon.c`); coop arms neither watchpoints nor sampler (`pgwt_mode_uses_watchpoints`/`_uses_sampler` in `daemon.h`). Default and full/sampled/tiered selection unchanged. **FROZEN CONTRACT** (in `provider_coop.h`): the extension's cooperative provider must (1) emit the SAME v2 trace records — a wait transition is a TRANSITIONS record byte-identical to `full`'s; (2) advertise `PGWT_FIDELITY_EXACT` so EXACT-required views need no change; (3) hand every event to the writer through the SAME path `full` uses (poll() drains delivery into `src/event_stream.c` `handle_event` → `src/event_writer.c`), taking backend identity from the SHARED registry (`src/backend.c`); (4) implement all four vtable entry points (start/stop/poll/self_metrics); (5) NOT own backend discovery (mode-independent registry/lifecycle stays in the daemon). The extension track replaces the stub bodies in `provider_coop.c`; nothing else in the daemon changes. Tests: `test_coop.c` — BPF-free unit test (`-DPGWT_SERVER`, like test_sampler/test_anomaly) asserting the provider registers, advertises EXACT, exposes the full vtable, and `start()` returns the clean not-available status (NULL-daemon tolerance, no-op stop/poll/metrics while unarmed); wired into run_all.sh C-unit + CI build-and-unit. **The cooperative provider IMPLEMENTATION itself is [PARKED]** (extension track — see cross-cutting).

---

### Track B — UI Rework  **[DONE v0.10]** (B1–B5 merged; B6 not built)

#### B.0 Target architecture
Native ES modules, no build step, still embedded via `go:embed`. The rule that makes it testable: **every view is a pure function from server JSON to a render model; only a thin mount layer touches the DOM and ECharts.** Layout:
- `web/static/app.js` → shrinks to bootstrap + router (~100 lines).
- `vendor/echarts.min.js` → exact version, vendored (no CDN, no floating tags — deterministic builds, airgapped-safe, stable B4 snapshots).
- `lib/`: `transport.js` (WebSocket, request ids, single-flight, abort), `view-manager.js` (enter/leave lifecycle, response chokepoint), `state.js` (filters/breadcrumbs/time-range, explicit), `table.js` (one shared table/drill-down component), `selection.js` (drag-select window overlay: pointer events + styled band div + `convertFromPixel` → time range, replacing ApexCharts' selection UX).
- `views/`: `overview.js  events.js  sessions.js  queries.js  histogram.js  transitions.js  active.js` — each exports `{ id, requests(state), build(data, state) [PURE → model], mount(el, model, callbacks) [thin DOM/ECharts], enter(), leave() [owns its chart] }`.
- **view-manager** = single chokepoint: guarantees `leave()` completes before the next `enter()`; drops any response addressed to a no-longer-active view (replaces per-path generation counters `app.js:176-186` by construction). **transport** owns request ids + single-flight per view (a new refresh supersedes the pending one — no stale-response-overwrites-fresh-data). **Chart lifecycle:** each view creates its ECharts instance in `enter()`, disposes in `leave()`, nowhere else (the five module-level chart globals + ad-hoc disposes at `app.js:429-445,1443,1462` disappear). **Pure builders** return ECharts option objects / table row models as plain data; they run and are tested in Node, no browser.

#### B.1 Phases

**Phase B1 — Safety net (before touching anything)  [DONE v0.10]** (2026-06-13, merge 861c313, PR #6). *Evidence:* `.github/workflows/ci.yml`, `tests/test_protocol_drift.py`. Scope: Playwright fixture fails any test on console error / pageerror / unhandled rejection (allowlist only on the reconnection test, which must stay last); GitHub Actions workflow builds pgwt + runs `test_web_ui.py` against `mock_server.py` on every PR/push (missing Playwright is a **failure** in CI with env `CI` set, still a skip locally); protocol-drift `tests/test_protocol_drift.py` generates the fixture in CI (no binary committed — deviation from the original "commit a trace file" text) and diffs real pgwt-server vs mock response schemas. Acceptance met — canary JS exception failed the web-ui job, then reverted. **Bonus:** the guard caught **two real app.js bugs** — the Live button (ReferenceError) and the Concurrency tab never loading — both fixed. CI builds bpftool from source (GH runners lack it); several stale `exit 0` tests now propagate failure. (master history carries the canary+revert pair 05628b1/91815c7, net-neutral.)

**Phase B2 — Adversarial mock  [DONE v0.10]** (PR #11). *Evidence:* chaos mode in `tests/mock_server.py`. Scope: configurable latency jitter (50–300 ms), out-of-order delivery for concurrent requests, late responses after navigation. New tests replicating manual testing: rapid tab switching, clicking before data loads, toggling live mode mid-refresh, drag-zoom during refresh — all under chaos mode, all asserting zero console errors + correct final state. Acceptance: chaos suite fails against then-current `app.js` (it should — these are the real bugs), and the failures form the acceptance list for B3.

**Phase B3 — The restructure  [DONE v0.10]** (PRs #14 / #16 / #18 = B3p1/p2/p3). Scope: B.0 architecture; mechanical migration one view at a time, simplest→hardest: **active → overview → events → sessions → queries → histogram → transitions**. Live mode moves into view-manager + transport (one refresh loop, superseding, honoring "last 15 min means NOW"). Shared table component extracted once, used by all table views. **Chart consolidation (during overview migration):** AAS chart moves ApexCharts→ECharts, **ApexCharts deleted entirely** (was `app.js:586-761`); **custom selection overlay** `lib/selection.js` replaces the library drag-select (pointer events on the chart container, a styled band element, `convertFromPixel` pixels→time; reusable on the concurrency chart; testable — unit-test pixel→time math in Node, drive drag in Playwright, snapshot the band in B4); **vendor exact-version ECharts** into `web/static/vendor/` and drop the CDN `<script>` tags (`index.html:7-8` floated `echarts@5`); builders stay library-agnostic (emit view models; only mount speaks ECharts — keeps a future per-chart lib experiment, e.g. uPlot for AAS, a contained adapter swap). Per-view DoD: pure builder extracted with Node unit tests; chart owned by enter/leave; existing Playwright tests pass; chaos tests for that view pass. Acceptance: full chaos suite green; `app.js` bootstrap-only; zero module-level chart variables; ApexCharts gone from `index.html`; no CDN script tags; drag-select on the AAS chart works via the overlay (Playwright-driven drag selects the expected range); soak test (50 random tab/filter transitions) → no console errors, stable listener counts.

**Phase B4 — Builder test suite + deepened assertions  [DONE v0.10]** (PR #22). Scope: Node test runner (`node --test`, no framework) for all pure builders — snapshot tests for option objects, filter-correctness tests (filtered input → filtered rows), edge cases (empty window, single session, self-loop transitions); Playwright assertions upgraded to read data via `page.evaluate(() => chart.getOption())` instead of canvas-exists, plus an end-to-end filter test (set filter → table *rows* change); **visual regression snapshots** (Playwright `toHaveScreenshot`) of each view vs the fixed mock dataset — AAS chart, histogram heatmap, Sankey transitions, drill-down tables — with a small pixel-diff tolerance for antialiasing; pinned browser version + viewport + device scale, animations disabled; baselines committed, intentional changes reviewed as baseline diffs. Acceptance: a deliberate off-by-one in a builder is caught by a Node test in ms without Playwright; removing a series color mapping (no JS error, data present) is caught by a snapshot diff in CI.

**Phase B5 — Fidelity-aware UI (joins Track A)  [DONE v0.10]** (PR #21). Depends on A3 (fidelity field), A4 (escalation via control proxy), B3 (view registry). *Evidence:* `lib/builders/fidelity.js`, `lib/panels.js`, `lib/control.js`, `tests/web_unit/fidelity.test.mjs`. Delivered (pure-builder + thin-mount + view-manager, no B3 regression):
- **Sampled shading:** `lib/builders/fidelity.js` turns a response's top-level `fidelity` ("exact"|"sampled"|"mixed") + optional `fidelity_ranges` into an ECharts markArea band; the AAS builder attaches it behind the stacked areas. Sampled → amber band over the whole window; mixed → bands only over sampled sub-ranges if present, else window marked mixed. Legend chip `#aas-fidelity-chip` explains it.
- **Unavailable panels:** EXACT-required views (histogram, transitions, concurrency) detect the `{"unavailable":"requires full-fidelity data"}` shape and render an explicit "No full-fidelity data in this window — escalate to capture" panel (with escalate affordance) instead of an empty chart — `buildUnavailablePanel` + `lib/panels.js`.
- **Escalate control:** header "Escalate 60s" button + budget readout + a Stop affordance while active, via `lib/control.js` → pgwt-server's `control` proxy (escalate/deescalate). Shows granted window / budget remaining / denial reason. Hidden when no daemon (static replay).
- **Escalation annotations:** the active escalation window rendered on the AAS timeline (markArea + labeled markLine), distinguishing reason=manual (cyan) vs reason=anomaly (red). Drove a small server addition: `build_status` now emits `escalation_reason` for the open window.
- **Daemon self-metrics panel** (`#metrics-btn`): events/s, samples/s, ringbuf drops, estimated overhead, anomaly counters, budget remaining — from the control-socket `metrics`.
- **Mock:** `tests/mock_server.py` gained `PGWT_MOCK_FIDELITY` (exact|sampled|mixed + `sample_period_ns`), the `{"unavailable":...}` shape for EXACT views over a sampled window, and the `control` command (status/metrics/escalate/deescalate state machine mirroring `src/control.c`, incl. over-budget denial). Defaults keep every existing test + protocol-drift green (exact, no daemon).
- **Tests:** `tests/web_unit/fidelity.test.mjs` — 29 Node builder checks (markArea model, mixed sub-ranges, unavailable panel, manual-vs-anomaly annotation, metrics + escalate-control models, overhead); Playwright: 4 new B5 tests (sampled shading, unavailable panels, escalate flow, metrics panel) vs a second sampled+daemon mock. Gates: `test_web_ui` **171/171** + chaos **25/25** green, zero console errors, no chart-global regressions. Acceptance met — a sampled/mixed window is visually unambiguous and escalation is operable from the browser.

**Phase B6 — New analysis views  [LEFT / not built]** (deferred future work — three views defined, none implemented). Depends on B3 (view registry), A3 (fidelity declarations — all three require EXACT data and must report "needs full-fidelity data" over sampled-only windows). Each view is additive: a server view + a registry entry (requests / pure builder / mount) — also the proof the restructure made views cheap. The three views:
1. **Per-execution waterfall (the 10046 view).** Drill: query fingerprint → executions list (sorted by duration) → one execution's lifetime as a span timeline — each wait a colored bar, gaps = on-CPU, phase boundaries from existing `PGWT_MARKER_PLAN_START`/`EXEC_START` markers. Server: new `executions` view (per-fingerprint execution list from markers) + `execution_detail` (events for one pid/time range — existing filtering covers it). Mount: ECharts custom series (same technique as the session timeline).
2. **Latency scatter.** Every wait as a dot on (time, log duration), colored by class — exposes modes/outlier bands aggregates hide (two `DataFileRead` bands = cache vs disk; a stripe at a round number = a timeout). Server: new `scatter` view with **server-side density downsampling** (cap points per screen-pixel bucket, never ship raw millions); response includes how many points were dropped (no silent truncation). Click/lasso → drill to underlying events.
3. **Transition matrix (the Sankey's scalable sibling, same data).** from-event rows × to-event columns, color = count or total time, ordered/grouped by wait class; readable where the Sankey saturates (>~20 nodes); every cell drills to matching transitions. Server: reuse the existing transitions computation with a matrix-shaped response. Offered as an alternate rendering on the transitions tab (toggle Sankey ⇄ matrix).
Tests (planned): pure-builder Node tests with synthetic fixtures (waterfall span layout math, scatter bucket downsampling vs the mock, matrix ordering/grouping); mock gains the three shapes; B4 visual snapshots for each; Playwright drill-path test (fingerprint → execution → waterfall). Acceptance (planned): each view lands as registry entry + server view with no view-manager/transport changes; all three correctly report unavailability over sampled-only windows.

---

### Track C — Rocky 8 / RHEL 8 support (kernel 4.18 + backports)

**Goal (run on RHEL 8-family kernels, EOL 2029): [DONE v0.11]** (PR #24) — but achieved by a **different mechanism than C1–C3 describe.** *Evidence:* `Makefile` — when the system libbpf lacks USDT (Rocky 8 ships libbpf 0.5.0, pre-USDT; probed by `scripts/detect_libbpf_usdt.sh`), it builds a **pinned libbpf + bpftool from source and links libbpf statically** (`build/libbpf`), `-I`-ing the bundled newer headers first; Rocky 9 / Ubuntu / CI use the system libbpf unchanged. Plus non-PIE PGDG postgres load-base handling (the #24 fix later guarded by the #24-class fixtures). Because the bundled modern libbpf on EL8.2+ kernels provides ringbuf / CO-RE / modern helpers, the perfbuf/BTF/helper **fallbacks C2/C3 were never needed** and were not built.

Plan principle (preserved): **runtime feature probing, never kernel-version gating** — RHEL backports make version numbers meaningless; the daemon probes at startup and degrades gracefully, always reporting which tier is available and why. What already works on 4.18: hardware-breakpoint perf events (2.6.33), `PERF_EVENT_IOC_SET_BPF` (4.1), perf-event BPF programs (4.9), uprobes, tracepoints, `PERCPU_HASH`. The **sampled tier needs none of the modern BPF features** — after A2 it runs on Rocky 8 unmodified. Gaps identified for the full tier: **BTF/CO-RE** (RHEL 8.2+ / 4.18.0-193+; for older minors support `--btf <path>` with BTFHub BTF, else require 8.2+); **ringbuf** (upstream 5.8, backported only in later 8.x minors — probe via `libbpf_probe_bpf_map_type()`, fall back to `BPF_MAP_TYPE_PERF_EVENT_ARRAY` for both `event_ringbuf` and `lifecycle_rb`); **`bpf_probe_read_user[_str]`** (upstream 5.5, backport varies — probe at load, fall back to legacy `bpf_probe_read`, valid for user memory on x86_64).

**Phase C1 — Feature-probe module + graceful degradation  [SUPERSEDED by v0.11 static-libbpf bundling]** (not built as written). *Planned scope:* startup probes (BTF, ringbuf, helpers); a structured capability report in `status`/`metrics` (control socket, A0) and startup log; tiered mode auto-restricts to sampled-only with an explicit reason when the full tier is unavailable. Planned acceptance: on a kernel lacking ringbuf, daemon starts, runs sampled, and `status` says precisely what is missing. *Reality:* no Track-C capability-probe/graceful-degradation module exists (the only "capability" probe in the code is T8/v0.13's measured-CPU sched_switch/BTF probe); EL8 support is delivered by bundling a modern static libbpf so the full tier runs directly. Depends (per plan) on A0 + A2.

**Phase C2 — Perf-buffer fallback path  [LEFT / not built]** (superseded — modern static libbpf provides ringbuf on EL8, so no perfbuf path was written). *Planned scope:* dual map/program flavors in one skeleton (`bpf_program__set_autoload` / `bpf_map__set_autocreate` after probing); perfbuf consumer in `event_stream.c`; a **reorder buffer** before the trace writer (per-CPU perf buffers deliver out of timestamp order; v2 blocks delta-encode timestamps and require sorted input — sort within the flush interval); durations unaffected (precomputed in BPF per backend). Planned tests: synthetic out-of-order delivery test for the reorder buffer; full live suite on a Rocky 8 VM; cross-check perfbuf vs ringbuf equivalence on a kernel with both. Planned acceptance: full tier produces correct traces via perfbuf on Rocky 8. *Reality:* code uses `ring_buffer__new` / `BPF_MAP_TYPE_RINGBUF` unconditionally; no `perf_buffer__new`, no reorder buffer, no map-flavor autocreate switch.

**Phase C3 — Helper fallbacks + external BTF  [LEFT / not built]** (superseded — bundled modern libbpf + EL8.2+ BTF suffice). *Planned scope:* CO-RE-guarded `bpf_probe_read` fallback; `--btf <path>` external BTF loading; INSTALL.md matrix for RHEL 8 minors. Planned acceptance: full tier works on Rocky 8.2+ stock kernel; documented path for older minors. *Reality:* no `--btf` flag / external-BTF loader exists; the non-PIE handling shipped in #24 is a related but distinct fix. C2/C3 were planned independent of Tracks A3+/B; testing would require a Rocky 8 VM in the matrix (deps on the remote box only). *(Forward note: v0.13/T7 later added a nightly containerized rockylinux:8 cell exercising exactly this static libbpf/bpftool bundling path — and project memory records EL8/kernel-4.18 validation incl. measured-CPU sched_switch working, with provisioning gotchas: PG module disable, py3.9, building pg_wait_sampling; USDT warning benign.)*

---

### Track D — Older PostgreSQL support (13–16; PG13 feasibility VERIFIED)

Current state before the track: PG 17/18 full; on 14–16 the tracer starts but does not capture correctly. **Root cause = address discovery, not the watchpoint mechanism:** discovery resolves the `my_wait_event_info` global (`src/discovery.c:650`) and the bootstrap watchpoint watches that pointer being set, but PG 14–16 backends write `MyProc->wait_event_info` **directly**. The watched *field* exists in PGPROC since PG 9.6, and a watchpoint on its address catches writes regardless of which pointer the code wrote through — so support is a resolution problem, solvable with existing machinery (ELF symbol resolution + `/proc/<pid>/mem` reads). Design principle (like Track C): **PG version knowledge lives in data tables with runtime validation, not scattered version checks.** One version-adapter module owns: symbol names to resolve, `offsetof(PGPROC, wait_event_info)` per major, wait-event name tables, feature flags (query_id available? pg_wait_events view?). Every resolved address is validated before trust (read the value, check the class byte against known wait-event classes; refuse to attach on mismatch — custom builds shift offsets, so **fail loudly, never trace garbage**).

**Phase D1 — MyProc-based address resolution (unlocks PG 14–16)  [DONE v0.12 — mechanism; PG14/15/16 validation LEFT]** (PR #27). *Evidence:* `src/discovery.c` — PG<17 MyProc-based `wait_event_info` resolution (`pgwt_confirm_wait_offset` / `pgwt_validate_wait_addr` runtime guard), per-version `offsetof(PGPROC, wait_event_info)` (case 13 → 684). Scope: fallback discovery — resolve `MyProc` symbol, read the pointer per backend via `/proc/<pid>/mem`, add per-version offset, runtime-validate; bootstrap flow for new backends watches the `MyProc` variable write instead of `my_wait_event_info`; the adapter chooses the path by PG major. Benefits both tiers (the A2 sampler consumes the same registry addresses). Tests: live suite vs stock PGDG 14/15/16 on the VM; validation-rejection unit test (wrong offset → refuse, clear error). Planned acceptance: full + sampled capture correct on PG 14, 15, 16 under pgbench (cross-check vs pg_stat_activity sampling). **Reality:** the MyProc-resolution mechanism + offset-validation guard shipped in v0.12 in service of **PG13** (D3). The plan's explicit **PG 14/15/16 validation** was not the v0.12 deliverable and is **not separately validated** — PG16 later entered the v0.13/T0 capture-smoke CI matrix (PG 13/16/17/18); **PG14/15 remain not-explicitly-validated.**

**Phase D2 — Per-version wait-event name tables (14–16)  [DONE v0.12 for PG13; 14/15/16 LEFT]** (PR #27). *Evidence:* `src/wait_event_pg13.inc` exists; no pg14/15/16 `.inc` files. Scope: name tables for 14/15/16 enum orderings (PG 17 re-ordered enums alphabetically; ≤16 used hand-ordered enums — current tables in `src/wait_event.c` covered only 17/18). Generate tables by script from PG source headers per major (data, not hand-maintained code); commit generated tables + generator; `pg_wait_events` runtime discovery stays preferred on 17+. Planned acceptance: no hex-code event names on 14–16 traces in the live suite. **Reality:** only the PG13 name table was generated/committed (extended per D3(b)); the 14/15/16 tables were not built.

**Phase D3 — PG 13 (FEASIBILITY VERIFIED — GO, 2026-06-19)  [DONE v0.12]** (PRs #27, #28, #31; plus #30). *Evidence:* `src/wait_event_pg13.inc`; `src/discovery.c` (offset 684, QueryDesc/PlannedStmt offsets); `src/bpf/pg_wait_tracer.bpf.c` Program 10 uprobe on `standard_ExecutorStart` (`pg13_query_attr` gate). Validated on a real **PostgreSQL 13.23** (Rocky 8.10 box, port 5433) by direct probe. PG13 gets full wait-event analysis (both tiers) **plus query attribution** via pg_stat_statements. Three pieces:
- **(a) Wait capture — the prerequisite (= Phase D1).** PG13 has no `my_wait_event_info` global and writes `MyProc->wait_event_info` directly, so it needs the D1 MyProc-resolution path. Proven live: `offsetof(PGPROC, wait_event_info)=684` on 13.23, reading `*(MyProc)+684` from `/proc/<pid>/mem` matched `pg_stat_activity` (Lock=0x03000000, Timeout:PgSleep=0x09000001). PG13 postgres is **non-PIE** on EL8, so the #24 non-PIE load-base fix applies. `MyProc` resolves from `.dynsym`.
- **(b) PG13 wait-event name tables (= Phase D2, extended to 13).** `pg_wait_events` is PG17+ only; PG13's table is generated from its headers.
- **(c) Query attribution — Route B1 (pg_stat_statements-based).** PG13 has no in-core query_id, BUT when pgss is loaded its `post_parse_analyze` hook populates the core `PlannedStmt.queryId` field (exists in PG13). Proven populated and **matching `pg_stat_statements.queryid`** (via an `ActivePortal` outside-read and via gdb at `ExecutorStart` entry — queryId already set when ExecutorStart fires). **Design:** uprobe **`standard_ExecutorStart`** (chosen over the public `ExecutorStart`: with pgss loaded `ExecutorStart_hook` is set, so `ExecutorStart` is a trampoline tail-jumping into the hook chain and an entry uprobe on it does not fire — `standard_ExecutorStart` is the real body), arg0 = `QueryDesc*` → `+8` (plannedstmt) → `+8` (queryId, uint64) → store in `state_map` (the EXISTING attribution pipeline). One uprobe per query (NOT per event); serves BOTH tiers (A2 sampler reads the cached id at 10 Hz; full-tier watchpoint reads it per event). Offsets on 13.23: `QueryDesc.plannedstmt=8`, `PlannedStmt.queryId=8`, `PortalData.queryDesc=136` (and `QueryDesc.sourceText=16`). The BPF uprobe validates the walked queryId against pgss at runtime. **pgss is required for PG13 query attribution** — if not loaded, query views report "unavailable (requires pg_stat_statements)" (the A3 unavailable pattern). Decision: the text-fingerprint alternative ("Route A") is NOT pursued. **Coverage gap:** utility statements (CHECKPOINT/VACUUM/DDL) go through `ProcessUtility`, not `ExecutorStart`, so the ExecutorStart uprobe won't attribute them — same gap as other PG versions; an optional `ProcessUtility` uprobe can close it later.
- **Offset-resolution constraint:** **no PG13 debuginfo exists anywhere** (purged after PG13's Nov-2025 EOL; the binary is stripped). So PG13 (and any EOL version) offsets MUST be derived from `postgresql*-devel` **headers** at build time (compile an `offsetof()` probe) or hardcoded from header-derived values — never from a debuginfo package. Symbols (`MyProc`, `ExecutorStart`, `ActivePortal`) come from `.dynsym`, which survives stripping. Symbol VAs are build-specific → resolve at runtime from the target binary, don't hardcode.
- Note: PG13 is EOL (2025-11); the D1 prerequisite also unlocks PG14–16, which get **native** query_id (no pgss) via the existing `st_query_id` path — so PG14–16 are higher-value for the same core work. *(Also in v0.12: PR #30 — sampled mode feeds the live accumulator so `--view` shows captured waits in default tiered mode, a field-reported gap; PR #29 — README/INSTALL reframed around tiered capture, escalation, control socket, corrected OS+PG matrix.)*

**PG 14–16 (as a distinct shipped deliverable)  [LEFT / not built].** The D1 mechanism unlocks them and they'd get native query_id (higher value than PG13); PG16 later joined the v0.13/T0 capture-smoke CI matrix, but 14–16 were not the v0.12 target and 14/15 remain unvalidated; their name tables (D2) are unbuilt.

**Not supported: PG ≤ 12  [design decision — cut line at 13].** Technically possible (wait_event_info since 9.6) but PG instruments far fewer wait points before 13 (IO/IPC classes and coverage grew release by release), so captured data would be misleadingly sparse; combined with deep EOL (PG12 EOL 2024-11) and test-matrix cost, the cut line is 13.

Dependencies (plan): D1/D2 independent of Tracks A–C, can land anytime; doing D1 before/with A2 is ideal (sampler shares the registry); D3 needs pgss for query attribution. Test matrix: PGDG 13 (archive repo — dropped from the live EL8 mirror at EOL) + 14–16 alongside 17/18 on the test box (PG13.23 already installed, port 5433). Validation: full + sampled correct + (with pgss) query attribution matching pg_stat_statements, cross-checked vs pg_stat_activity.

---

### Track E — Live-suite cleanup (pre-existing debt)  **[DONE v0.10]** (across #8/#9/#23; further de-flake v0.11/#26)

Surfaced 2026-06-13 when the full `run_all.sh` first ran on a real box since CI was added. These fail/flake on **master baseline** (NOT caused by B1/A0) and were invisible because CI can't run live capture tests. Small, independent, done early so "green live run" is a meaningful gate for later capture phases.

**Phase E1 — De-flake and fix stale live tests  [DONE v0.10]** (#9; additional de-flake v0.11/#26).
- `test_cross_validate`: flaky — sampling side came back empty on a freshly-started PG (passed on the second run). Add warmup / longer sampling window / retry for determinism. *(De-flaked in v0.11/PR #26.)*
- `test_deterministic` "Lock:relation" assertion: gets `Lock:advisory` on PG 18 (the lock wait IS captured; lock-hold/wait detection passes) — fix the expected event for the PG version, or make the workload take the intended relation lock. *(Related to the #8 Lock-naming fix.)*
- `test_data_idle` / `test_client_wait`: stale ClientRead idle semantics — **already fixed by B1**; confirm green after B1 on the box. *(test_client_wait further de-flaked v0.11/PR #26.)*
Acceptance: `run_all.sh` functional section green on dmitry-micro-test, twice in a row (no flakes).

**Phase E2 — Skip-not-fail for extension-dependent tests  [DONE v0.10]** (#9). Tests that require the **patched PG** (`pg_wait_event_timing` events, extension in time_model) previously FAILED on stock PG 18. They must detect the absent extension and **SKIP** (counted skipped, not failed), so a stock-PG box can go fully green. Acceptance: on stock PG, extension tests skip; on patched PG, they run.

**Phase E3 — Wire test_control.sh into run_all.sh  [DONE v0.10]** (#23). Deferred from A0 (avoided colliding with B1's run_all.sh edits). Add it to the live suite. Acceptance: `run_all.sh` runs test_control **16/16**. (E1+E2+E3 = one small PR's worth, independent of all other tracks.)

---

### Cross-cutting items

**Prometheus exporter for AAS/sampled metrics  [LEFT / not built]** — explicitly out of scope for these tracks; a **planned future consumer**. Constraint honored: A0 metric names are already snake_case + unit-suffixed and the control socket exposes queryable aggregates so the exporter isn't precluded. *Evidence:* only comment references in `src/sampler.c` and `src/control.h`; no `/metrics` HTTP exporter exists.

**Cooperative provider IMPLEMENTATION  [PARKED]** — A6 (v0.10) froze the interface and shipped a refusing stub (`src/provider_coop.c/h`); the actual implementation (emitting EXACT trace records from a PG patch/extension) belongs to the **extension track** and is not built. The frozen 5-point contract (see A6) is the hand-off.

**PG18 async I/O — io_worker semantics + I/O data correctness (risk #6, OPEN DECISION in the plan)  [SUPERSEDED / RESOLVED by v0.13 (Trust Milestone T2)].** The plan flagged that under `io_method=worker` (PG18 default) a single logical read splits across processes: the requesting backend shows generic `IO:AioIoCompletion` (or no wait if completed in the background) while the actual `IO:DataFileRead/Write` runs on an **io_worker** with no requesting-query context. Consequences the plan said we must decide + verify: **(1) how to treat io_workers** — counting their busy I/O time as active load double-represents the SAME logical I/O in AAS (backend on AioIoCompletion + io_worker doing the read) → AAS reads higher under AIO-worker than the pre-AIO model, cross-version AAS not apples-to-apples; options were: count as today / exclude io_workers from AAS / surface as a distinct "I/O on behalf of sessions" class (a semantic call like the Client:ClientRead idle decision); **(2) query attribution to I/O degrades** — io_worker reads can't tie to the originating query (no query_id on the worker), the backend's AioIoCompletion has query_id but not read detail → per-query I/O is partial under `worker`, document it; **(3) validate data correctness better** — the existing `pg_stat_io` cross-check (~0.80 ratio, accepted as "async overlap") is too loose to *prove* correctness under AIO; add a focused validation summing backend `AioIoCompletion` + io_worker `DataFileRead/Write` reconciled against `pg_stat_io`, confirming no double-counting in DB Time/AAS; `io_uring` mode (rare on RHEL8) loses even the io_worker detail — a separate lower-priority case. **Plan status when written:** flagged from analysis (AioIoCompletion + 0.80 ratio observed); the AAS double-representation and backend/worker split NOT yet measured head-on — needs an empirical PG18 run before any io_worker semantic change. **Resolution:** v0.13/T2 decided it — **io_workers excluded from AAS and surfaced as a separate utilization metric** — with the empirical study in `docs/T2_IOWORKER_STUDY.md` and the decision recorded in `docs/AAS_SEMANTICS_DECISION.md`.

**Deferred UI polish (from the completion summary's "Deferred, not rework-blocking"):**
- **idle-ClientRead %DB cosmetic  [DONE v0.11]** (PR #25) — idle-but-visible events (e.g. `Client:ClientRead`) render `—` for %DB instead of a bogus bar; non-idle %DB sums to ~100%.
- **historical-escalation annotations + mixed-fidelity sub-range shading  [PARTIAL / LEFT]** — B5 delivered *current-window* escalation annotations (markArea + markLine, manual cyan / anomaly red) and sampled/mixed bands (mixed only over sub-ranges "if present"); the fuller *historical* escalation annotations and mixed sub-range shading were noted as small server additions and left for later.

---

### Sequencing (plan's dependency graph, with realized status)

```
Track A:  A0✅ ─▶ A1✅ ─▶ A2✅ ─▶ A3✅ ─▶ A4✅ ─▶ A5✅    A6✅          (all v0.10)
Track B:  B1✅ ─▶ B2✅ ─▶ B3✅ ─▶ B4✅ ─────────▶ B5✅    B6 ⬜ not built  (B1–B5 v0.10)
Track C:  C1(superseded)  C2 ⬜  C3 ⬜   EL8 goal shipped v0.11 via static libbpf bundling
Track D:  D1(v0.12, PG13)  D2(v0.12, PG13 only)  D3✅ v0.12    PG14–16 ⬜
Track E:  E1✅ E2✅ E3✅  (v0.10; extra de-flake v0.11)
```
Plan rules: ✅ = merged to master; ⬜ = not started/future. Core rework (A0–A6, B1–B5, E) complete 2026-06-18 @ master c027d3c. B1/B2 are small and land **first** regardless of Track A (they protect everything else). Tracks parallelize fully until B5. Default daemon mode flips to `tiered` only after A4's cross-validation passes its tolerances. A capture phase is "done" only with a **green live run on the box**, not just green CI.

### Risks and open questions (preserved verbatim in intent)
1. **process_vm_readv vs PG shm layout (A2):** assumes PGPROC addresses identical across PG processes (inherited mmap) — true for PG's anonymous shm on Linux, but verify vs `huge_pages=on` and EXEC_BACKEND-style edge cases early in A2. Mitigation: the registry stores per-pid addresses; per-pid `pread` fallback is a small constant-factor cost.
2. **Sampled estimator error at low rates (A3/A4):** 10 Hz may under-represent short-lived events (sub-100ms waits flicker below the sampling floor). The cross-validation test quantifies it; the UI's fidelity shading keeps it honest. Do not paper over it with extrapolation heuristics.
3. **Escalation attach race (A4):** backends forked mid-escalation must get watchpoints via the existing fork→bootstrap→init flow; verify the lifecycle path is active in tiered mode even while de-escalated (it must be, for the registry).
4. **B3 regression risk:** mitigated by ordering (B1/B2 first) and per-view migration with the full suite green between views. No big-bang rewrite of `app.js`.
5. **Anomaly rule tuning (A5):** baselines differ wildly across workloads. Ship conservative defaults, make every threshold configurable, and log every near-trigger to the trace so tuning is data-driven.
6. **PG18 async I/O — io_worker semantics + I/O correctness (OPEN DECISION + validation gap):** see the cross-cutting entry above — resolved by v0.13/T2.

---

### Rework status table

| Item | Description | Status | Evidence |
|---|---|---|---|
| A0 | Control socket + self-metrics | **DONE v0.10** | PR #7, b1ef3c4; `src/control.c/h`, test_control 16/16 |
| A1 | Trace format v2 (typed blocks) | **DONE v0.10** | PR #12; v2-only (superseded planned v1-compat reader); v3 later in T8 |
| A2 | Sampled provider (process_vm_readv) | **DONE v0.10** | PR #13; `src/sampler.c/h`, `provider.h`, `provider_full.c` |
| A3 | Fidelity-aware compute (exact-wins merge) | **DONE v0.10** | PR #15; `src/compute.c` (merge later hardened by T1) |
| A4 | Tiered orchestration + cross-validation | **DONE v0.10** | PR #17 + #23 flip; `src/escalation.c/h` (budget hardened by T3) |
| A5 | Anomaly-triggered escalation | **DONE v0.10** | PR #19; `src/anomaly.c/h`, test_anomaly 74/74, live 11/11 |
| A6 | Cooperative provider stub (interface freeze) | **DONE v0.10** | PR #20; `src/provider_coop.c/h` stub, test_coop |
| A6-impl | Cooperative provider IMPLEMENTATION | **PARKED** | extension track; interface frozen only |
| B1 | CI safety net (Playwright guard + workflow) | **DONE v0.10** | PR #6, 861c313; `.github/workflows/ci.yml`, test_protocol_drift.py |
| B2 | Adversarial mock (chaos mode) | **DONE v0.10** | PR #11; `tests/mock_server.py` chaos |
| B3 | The restructure (ES modules, ECharts-only) | **DONE v0.10** | PRs #14/#16/#18; ApexCharts deleted, `lib/selection.js` |
| B4 | Builder test suite + visual regression | **DONE v0.10** | PR #22; `node --test` builders, `toHaveScreenshot` baselines |
| B5 | Fidelity-aware UI (shading, escalate, panels) | **DONE v0.10** | PR #21; `lib/builders/fidelity.js`, fidelity.test.mjs 29, web_ui 171/171 |
| B6 | New analysis views (waterfall/scatter/matrix) | **LEFT / not built** | designed only; 3 views, none implemented |
| C-goal | Run on Rocky 8 / RHEL 8 (EL8) | **DONE v0.11** | PR #24; Makefile static libbpf+bpftool bundling + non-PIE fix |
| C1 | Feature-probe module + graceful degradation | **SUPERSEDED by v0.11 bundling** | no Track-C capability-probe module in code |
| C2 | Perf-buffer fallback + reorder buffer | **LEFT / not built** | code uses `ring_buffer__new`/RINGBUF only |
| C3 | Helper fallbacks + external `--btf` BTF | **LEFT / not built** | no `--btf` flag / external-BTF loader |
| D1 | MyProc-based address resolution (→14–16) | **DONE v0.12 (mechanism, PG13)** | PR #27; `discovery.c` offset 684 + validate guard; 14/15/16 unvalidated |
| D2 | Per-version wait-event name tables (14–16) | **DONE v0.12 (PG13 only)** | `src/wait_event_pg13.inc`; no 14/15/16 `.inc` |
| D3 | PG 13 (capture + pgss query attribution) | **DONE v0.12** | PRs #27/#28/#31/#30; `standard_ExecutorStart` uprobe, pg13 tables |
| D-1416 | PG 14–16 as a shipped/validated deliverable | **LEFT / not built** | PG16 only later in v0.13/T0 CI matrix; 14/15 unvalidated |
| D-≤12 | PG ≤ 12 support | **Not supported (design decision)** | sparse instrumentation + deep EOL; cut line = 13 |
| E1 | De-flake stale live tests | **DONE v0.10** | #9 (extra de-flake v0.11/#26) |
| E2 | Skip-not-fail for extension-dependent tests | **DONE v0.10** | #9 |
| E3 | Wire test_control.sh into run_all.sh | **DONE v0.10** | #23; test_control 16/16 in suite |
| X-Prom | Prometheus exporter (AAS/sampled metrics) | **LEFT / not built** | out of scope; metric names kept exporter-safe |
| X-io | PG18 io_worker AAS semantics (risk #6) | **SUPERSEDED / RESOLVED v0.13 (T2)** | io_workers excluded from AAS; T2_IOWORKER_STUDY.md, AAS_SEMANTICS_DECISION.md |
| X-ui1 | idle-ClientRead %DB cosmetic | **DONE v0.11** | PR #25 (`—` for %DB) |
| X-ui2 | historical-escalation annotations + mixed sub-range shading | **PARTIAL / LEFT** | B5 did current-window annotations + sampled bands only |

---

## Part 2 — Trust Milestone (T0–T7) & exact measured CPU (T8)

> **Status:** the entire Trust Milestone (Track T, phases T0–T7) **plus** the
> T8 measured-CPU work **shipped in v0.13 (tag 2026-07-21)**. This chapter is the
> consolidated, lossless record of that milestone — every finding, every fix,
> every rejected design, and the parked follow-ons — and it *replaces* the
> source docs (`TRUST_MILESTONE_PLAN.md`, `T8_MEASURED_CPU_PLAN.md`,
> `T8_MEASURED_CPU_REVISION.md`, `FUTURE_WORK.md`). `docs/S3_SCHED_SWITCH_CPU.md`
> is *kept* (it is the shipped measured-CPU spec); it is referenced here, not
> reproduced in full.
>
> Severity legend used throughout: 🔴 critical (wrong/lost data or a broken core
> guarantee), 🟠 major, 🟡 minor, 🔵 design question/decision.

### Why this milestone existed

The v0.12 rework (REWORK_PLAN A0–A6/B1–B5/E) delivered the *right architecture* —
per-task watchpoints, tiered capture with read-time exact-wins merge, pure-builder
UI. A **five-perspective adversarial code review** of master `@ fd21630` (capture
core, tiered orchestration, data path/format, client/UI, tests/CI) confirmed the
design but found that **every one of the system's honesty guarantees was violated
by at least one code path**:

- *"exact-wins merge prevents double counting"* — long waits inside escalation
  windows double-count (**FID-1**); the live view double-counts during every
  escalation (**ESC-3**).
- *"never a silent empty result"* — the summary fast-path silently drops all
  sampled data for windows ≥ 120 s and labels the result `"fidelity":"exact"`
  (**FID-2**), in the *default tiered mode*, for the most common query ("last 15
  minutes").
- *"worst-case overhead is bounded by configuration, not by hope"* — the
  escalation budget can be overspent ~2× via extensions (**ESC-1**), ledger
  overflow gives unbilled windows (**ESC-5**), and `--escalation-budget 0`
  silently means unlimited (**ESC-6**).
- *"fail loudly, never trace garbage"* — a wrong offset can be blessed by a
  validator that accepts `0` as proof (**CAP-2**); >512 backends silently record
  nothing (**CAP-1**); a total sampler read failure renders as an idle database
  forever (**SMP-1**).
- *"the tool is for incidents"* — a pure CPU storm produces AAS ≈ 0 and never
  triggers escalation (**AAS-1**); when the SSH transport dies the UI shows "No
  data" under a green connected pill (**UI-1**).

**Meta-finding:** the test net was stretched over the wrong half of the cliff.
All four field escapes (#8, #24, #30, #31) lived in the capture/discovery slice
that CI never executed (~27% of LOC, 100% of the escape record); the claimed
support matrix was ~24 cells and automated coverage was 1; `run_all.sh` passed
when everything skipped; the last three field fixes shipped with zero regression
tests (**TST-1..9**).

**Governing rule of the milestone:** *no feature work (B6, Track C, Track D)
until Track T is done* — every new feature built on the current
merge/budget/anomaly semantics would inherit their bugs.

**Principles (standing):** (1) *Verify before fixing* — each phase reproduces its
findings with a failing test first; a finding that doesn't reproduce is
documented and closed, no speculative fixes. (2) *Root cause, never workaround.*
(3) *A fix without a regression test is not done* (the #24/#30/#31 lesson).
(4) *T0 lands first* (safety net before surgery); every later phase keeps the new
real-PG CI job green. (5) *Branch + PR per phase*, reviewed and merged before the
next pair; parallel phases never share a working tree.

The plan was authored in **PR #35** (`trust-milestone-plan`).

---

### Phase T0 — Safety net: real-PG CI + gate hardening ✅ [DONE v0.13 — PR #36]

**Owns:** TST-1..8 (TST-5 partially); groundwork for everything else. **CHANGELOG
[0.13]:** "real-PG CI safety net" bullet.

**Findings**

- **TST-1 🔴** — CI never executed the capture path; 4/4 field escapes
  (#8/#24/#30/#31) were in the CI-unexecuted slice (~27% of LOC, 100% of the
  escape record).
- **TST-2 🟠** — GitHub runners can run BPF + real PostgreSQL today (the build
  job already asserts `/sys/kernel/btf/vmlinux`); no smoke job existed. Highest-ROI
  gap in the project.
- **TST-3 🟠** — CI and `run_all.sh` maintained two drifted unit lists
  (event_writer/reader missing from run_all; `test_concurrent_rw` built but run
  nowhere). Needed one shared list.
- **TST-4 🟠** (`tests/run_all.sh:105-185,224-231`) — all-skip was a *green* run;
  no `--require-live`, no minimum-executed floor.
- **TST-5 🟠** (T0/T7) — overhead gate only failed >50%, took ~70 min inside
  run_all (guaranteeing it was skipped), measured only `--mode full` — the
  shipped default's overhead was never gated, and no trend was stored. Fix: use
  `--quick` in run_all + CSV trend file; gate tiered/sampled too.
- **TST-6/13 🟠** — cross-validation accepted any catalog-valid name (the PR#8 bug
  class passes); pg_wait_sampling tolerance was 0.1×–10× (vacuous). Expand the
  deterministic-workload exact-event-assertion pattern; tighten tolerances.
- **TST-7 🟠** — `test_cross_validate_tiered.sh` (the test that justified flipping
  the default to tiered) and `test_anomaly_live.sh` were wired into nothing.
- **TST-8 🔴** — `pgwt_find_load_base` / `pgwt_find_symbol_offset`: pure,
  twice-broken (#24 class), zero tests.

**Fix / design shipped**

- **CI capture-smoke job**: installs PGDG PostgreSQL on the runner, starts it,
  runs `pg_wait_tracer` in `--mode full` AND `--mode tiered`; deterministic
  workload (`pg_sleep`, blocked `LOCK TABLE`, short pgbench); asserts *specific*
  events with bounded durations in live view and trace file; asserts query_id
  attribution non-empty. Matrix `pg-version: [13, 16, 17, 18]` via PGDG apt.
- **discovery.c unit tests**: committed `/proc/maps` fixtures (EL8 non-PIE PGDG,
  EL9 PIE, Ubuntu) + `-pie`/`-no-pie` ELF fixtures compiled in CI; assert
  `pgwt_find_load_base` / `pgwt_find_symbol_offset` results.
- **run_all.sh**: `--require-live` mode (root+PG-section skips become failures);
  single shared unit-test list consumed by both CI and run_all (adds missing
  `test_event_writer/reader`, `test_concurrent_rw`); wires
  `test_cross_validate_tiered.sh`, `test_anomaly_live.sh`, `test_control.sh`;
  overhead via `--quick` with CSV trend appended to a tracked results file.
- **Regression tests for the three shipped-untested fixes:** sampled `--view`
  shows data (#30 — covered by smoke job); query-attr uprobe fires (#31 — smoke
  assertion); load-base (#24 — the fixtures above).

**Acceptance (met):** CI red if capture silently records nothing on any matrix
cell; a deliberate load-base regression fails a unit test in milliseconds;
`run_all.sh --require-live` cannot pass vacuously.

---

### Phase T1 — Fidelity merge & summary honesty ✅ [DONE v0.13 — PR #39]

**Owns:** FID-1..5, FID-7. **CHANGELOG [0.13]:** "fidelity merge & summary honesty"
bullet.

**Findings**

- **FID-1 🔴** (`src/server.c:554-567,713-722`) — exact-wins coverage was derived
  from the TRANSITIONS block `[first_ts,last_ts]`, but transition timestamps are
  wait-**END** times: a 60 s lock wait emits nothing until it ends, so its
  interior samples survive the merge *and* the exact 60 s interval lands → up to
  **2× inflation** of long waits inside escalation windows. The authoritative
  `PGWT_MARKER_ESCALATE_START/END` markers were already in the trace
  (`pg_wait_tracer.h:98-99`) but the merge ignored them. **Fix: coverage =
  escalation-marker windows (+ whole-file for `--mode full`), not per-block
  timestamp ranges.** Also fixes **FID-6** boundary slop.
- **FID-2 🔴** (`src/server.c:928-937,957-962`) — `should_use_summaries()`
  selected the summary path on range length alone (≥120 s) and stamped
  `"fidelity":"exact"`, but summaries are fed only by the exact ringbuf path (the
  sampler never writes them). In default tiered mode "last 15 min" time_model/AAS
  returned only escalation slivers (or nothing), labeled exact — violating "last
  15 min means NOW" and the A3 contract. **Fix: summary path checks coverage
  against the window and falls back to / blends with raw sampled data; never
  hardcodes the fidelity label.**
- **FID-3 🟠** (`src/server.c:581-584` + `src/compute.c:558-641`) — samples
  normalized to `duration_ns = sample_period` fed the count/avg/p50/p95/p99
  pipeline: over sampled windows the events view reported fabricated percentiles
  (p95 ≈ sample period) in the same columns as real data. **Fix: suppress/gate
  latency columns under sampled fidelity** (the heatmap already did this).
- **FID-4 🟠** (`src/event_stream.c:30-44` + `src/summary_writer.c:155-224`) —
  markers were pushed to the summary writer *before* the `PGWT_IS_MARKER` check:
  exec/plan markers inflated per-query counts (skewing avg_us low) and escalation
  markers inserted **bogus query_ids** (`PGWT_ESC_PACK` values) into summaries; in
  raw compute only transitions/variants filtered markers — top_events, top_queries,
  heatmap did not. **Fix: filter markers at the accumulation chokepoints;** add a
  marker-bearing-trace regression test.
- **FID-5 🟠** (`src/replay.c`) — `--replay` consumed `old_event/duration_ns`
  directly: decoded samples contributed nothing and markers created junk entries;
  replaying a tiered trace showed near-zero activity with no warning. **Fix:
  fidelity-aware replay** (or explicit "sampled data present, use `--view`" notice).
- **FID-7 🟡** (`src/server.c:534-539`) — per-file mono→wall offset fixed at file
  open; NTP steps skew cross-file comparisons. **Fix: single clock domain for the
  merge; re-anchor wall mapping.**

**Acceptance (met):** the two headline rework claims ("no double count", "never a
silent empty result") hold under adversarial synthetic traces — a long wait
spanning an escalation window totals to ground truth; a 15-min tiered-default
query over sampled-only data returns correct data labeled `"fidelity":"sampled"`;
a marker-bearing trace yields no phantom query_ids.

---

### Phase T2 — AAS semantics: one definition, all paths ✅ [DONE v0.13 — decision PR #40, impl PR #42]

**Owns:** AAS-1, SMP-5; folds in the io_worker open question (REWORK risk #6).
**GATE — PASSED 2026-07-12.** **CHANGELOG [0.13]:** "AAS semantics" bullet.

**Finding**

- **AAS-1 🔴** (`src/sampler.c:103-121` + `src/anomaly.c:38-49`) — on-CPU sessions
  (`wait_event_info==0`) were skipped with a comment "A3 derives CPU from
  coverage", but **no coverage-derived CPU existed anywhere** in
  server.c/compute.c (found independently by two reviewers). Consequences: sampled
  AAS = "waiting sessions" (not ASH semantics); DB Time/AAS stepped discontinuously
  at every tier switch (FID class); **a pure CPU storm produced AAS ≈ 0 and never
  escalated** — the anomaly engine was blind to CPU incidents. The single most
  important semantic gap.
- **SMP-5 🔵** — fixed-phase sampling with no jitter (aliasing with periodic
  workloads); decision folded here (add ±10-20% jitter).

**Decision (authoritative: `docs/AAS_SEMANTICS_DECISION.md`; study:
`docs/T2_IOWORKER_STUDY.md`)** — the **full decomposed active-session model**:

- `active` = non-idle wait + on-CPU. **Command-open gate** for client backends
  (only count them as active when a command is open), **ungated** for background
  processes whose idle is instrumented.
- Categories: foreground **plan / exec / command-overhead**, plus **maintenance**,
  plus **background**.
- **io_workers** (PG18 `io_method=worker`, the default): **excluded from AAS**,
  surfaced instead as a **busy% utilization** metric ("I/O on behalf of sessions"
  reconciled empirically against `pg_stat_io`).

The io_worker study also registered **three tracer defects** for the
implementation to absorb: **(1)** the PIE uprobe attach-offset bug (the
`va − 0x400000` heuristic → silently dead uprobes on PIE builds; prerequisite fix
for the gate probe), **(2)** exit-record phantom CPU in traces, **(3)** an
in-trace samples/exact overlap observed on a pre-T1 build (re-verified on current
master — already fixed by T1). Defects (1) and (2) are called out as fixed in the
CHANGELOG T2 bullet.

> *Original decision framing (superseded by the above, kept for the record):* the
> recommended baseline was the ASH standard — active = on-CPU + non-idle wait; the
> sampler records `we==0` in an active transaction as first-class `CPU` samples so
> the anomaly engine sees them and exact windows count CPU implicitly. The
> io_worker choice (count-as-today / exclude-from-AAS / distinct "I/O on behalf of
> sessions" class) was left open pending one empirical PG18 run — which is what the
> study delivered.

**Scope shipped:** sampler batch includes on-CPU actives; anomaly metrics include
CPU (a CPU storm trips the AAS rule); server-side sampled CPU estimator so DB
Time/AAS are continuous across tier switches; optional sampling-phase jitter
(SMP-5).

**Acceptance (met, DoD #3):** a `SELECT`-storm CPU incident raises sampled AAS and
can trigger escalation; AAS shows no step artifact at tier switches; sampled AAS
vs exact AAS under a CPU-bound `pgbench -S` run agree within tolerance.

---

### Phase T3 — Escalation budget & trigger quality ✅ [DONE v0.13 — PR #43; status-flip docs PR #45]

**Owns:** ESC-1..12 (depends on T2 — the anomaly metric shape settles there).
Branch `t3-escalation-trust`. **CHANGELOG [0.13]:** "escalation budget & trigger
quality (ESC-1..12)" bullet.

**Findings → resolutions** (all fixed / documented; pure cores unit-tested in
`tests/test_escalation_budget.c` = budget/ledger, extended `tests/test_anomaly.c`
= lock floor + slow-release; integration in `test_escalation.sh` /
`test_control.sh`; live in the capture-smoke escalation phase):

- **ESC-1 🟠** (`escalation.c:253-267`) — extension check charged
  `new_deadline − old_deadline` but never the already-committed remainder
  `(old_deadline − now)`; repeated extensions ran a window to ~2× the hourly
  budget, with no mid-window clamp. **→ Fixed:** budget math moved to a pure,
  unit-tested core (`src/escalation_budget.c`); an over-ask is **clamped** to the
  remaining budget and the deadline is armed at `now + grant`, so
  extend-every-second caps total full-fidelity time at *exactly* the budget;
  `pgwt_escalation_check_budget()` (daemon timer) is the mid-window backstop.
- **ESC-2 🟠** (`escalation.c:141-170`) — de-escalation `detach_all()` reset the
  state_map, discarding every open wait interval (no final TRANSITIONS record),
  while the END marker extended the block's exact-wins range so the covering
  samples were dropped too — a systematic undercount hole at the end of every
  window, hitting exactly the long waits the window exists to capture. **→ Fixed:**
  `flush_open_intervals()` writes each open wait as a final transition (ts =
  de-escalation instant) *before* detach + END marker.
- **ESC-3 🟠** (`sampler.c:239-300` + `event_stream.c:19-98`) — in tiered mode both
  the sampler and the exact ringbuf fed `d->event_accum` during escalation: the
  live `--view` inflated up to ~2× exactly while an operator watched an incident.
  **→ Fixed:** `pgwt_sampler_accumulate()` skips pids with a live watchpoint while
  escalated (live-path analogue of the FID-1 merge fix); smoke-test live-view
  tolerance tightened from 7 s back to 6 s.
- **ESC-4 🟠** (`anomaly.c:109-114`) — lock-fraction rule fired on `active>=1`: a
  single backend's 300 ms row-lock wait triggered a 60 s full-fidelity window; on
  routine OLTP the engine duty-cycled the whole budget away on noise. **→ Fixed:**
  the lock rule now needs BOTH `lock_fraction > F` AND lock-class
  `AAS ≥ --anomaly-lock-min-aas` (default **2.0**).
- **ESC-5 🟡** (`escalation.c:289-293`) — ledger overflow (256 segments/hr) opened
  windows that were never billed. **→ Fixed:** overflow coalesces the two oldest
  segments (bills the gap, never under-bills); a window is never opened unbilled.
- **ESC-6 🟡** (`escalation.c:253`) — `--escalation-budget 0` disabled budgeting
  entirely while metrics reported 0 remaining. **→ Fixed:** `0` = **deny-all**
  (honest `0` remaining); `unlimited` (or `-1`) = no cap
  (`escalation_budget_unlimited: true`, remaining = `-1` sentinel = ∞ in the UI).
- **ESC-7 🟡** (`anomaly.c:122-129`) — a frozen baseline meant a legitimate load
  regime change re-fired every cooldown forever, and a daemon started mid-incident
  baked the incident into the baseline. **→ Fixed:** baseline stays frozen while
  over threshold; **slow learn-through** engages only after **15 min**
  (`PGWT_ANOMALY_DEF_LEARN_THROUGH_MIN`) continuously-over, then learns at **1/10**
  the normal EWMA rate (`PGWT_ANOMALY_DEF_SLOW_RELEASE_DIV`). Short incidents never
  move the bar; a real regime change is eventually adopted. Both pinned in tests.
- **ESC-8 🟡** — near-trigger log up to 10 lines/s. **→ Fixed:** rate-limited to
  reason-mask changes + a 60 s summary (with suppressed count); `anomaly_near_total`
  still counts every one.
- **ESC-9 🟡** (`control.c:157`) — `metrics` reported `"tier":"sampled"` in
  `--mode full`. **→ Fixed:** `metrics.tier` and `status.tier` share
  `daemon_tier_str()` (a fixed EXACT provider reports `escalated`, never a bogus
  `sampled`).
- **ESC-10 🟡** (`control.c:416`) — control-socket `unlink()` without a liveness
  probe let a second daemon steal a live daemon's socket. **→ Fixed:** a liveness
  probe makes a second daemon on the trace dir **refuse startup** (fatal, rc −2)
  instead of stealing the socket.
- **ESC-11/12 🔵** — synchronous `attach_all` stalls the sampler at high backend
  counts (ties into SMP-3: those ticks are *lost*); cooldown(120 s) vs window(60 s)
  = 50% duty cycle on sustained incidents. **→ Documented** in README: the ~50%
  duty cycle (cooldown from fire start) and the weight-compensated `attach_all`
  stall bound (SMP-3 keeps accounting correct; only resolution dips).

**Acceptance (met):** worst-case full-fidelity seconds per hour ≤ configured
budget under adversarial requests — provable by test; the live view during
escalation matches the post-hoc trace view within tolerance.

---

### Phase T4 — Capture & sampler hardening (silent-wrong-data class) ✅ [DONE v0.13 — PR #38]

**Owns:** CAP-1..12, SMP-1..4. **CHANGELOG [0.13]:** "capture & sampler hardening"
bullet.

**CAP findings (capture core: watchpoints, discovery, BPF)**

- **CAP-1 🔴** (`bpf/pg_wait_tracer.bpf.c:57`) — `state_map` max_entries=512 <
  MAX_BACKENDS(1024) < common `max_connections`. Above 512 live backends, inserts
  failed with an unchecked return (`bpf.c:256`, `backend.c` preseed too); those
  backends **never recorded a single event, no error** — silent under-count exactly
  under high-concurrency incidents. **→ Fixed:** sized to registry capacity,
  checked inserts, `state_map_full` counter surfaced in metrics.
- **CAP-2 🟠** (`discovery.c:315-339` + `backend.c:221`) — `pgwt_validate_wait_addr`
  accepted `wei==0` as valid (the most likely read from a *wrong* offset), checked
  one backend once, then set `wait_offset_validated` forever. A wrong PG13 offset on
  a custom build traced garbage labeled real. **→ Fixed:** require a non-zero,
  class-valid reading before trusting; validate across several backends; re-validate
  periodically; refuse-to-attach loudly on mismatch.
- **CAP-3 🟠** (`discovery.c:229-280`) — PG13 PGPROC/QueryDesc offsets hardcoded
  from one build (13.23), gated only by arch; PGPROC layout is not ABI-stable across
  configure flags/forks. **→ Fixed** (rides on CAP-2 strict runtime validation:
  refuse to attach on mismatch, loud error).
- **CAP-4 🟠** (`discovery.c:171-191`) — load-base resolution did
  `strstr(line, "postgres")` over the whole maps line and took the lowest match;
  extension paths like `.../pg_stat_statements.so` matched too. Under PIE a
  lower-mapped `.so` → wrong load base → zero events, no error (the #24 class, one
  directory layout away from recurring). **→ Fixed:** exact pathname-field
  comparison against the resolved binary path.
- **CAP-5 🟠** (`discovery.c:817-834`) — the PG17+ path had zero runtime sanity
  check on the resolved `my_wait_event_info` address (validation only ran for
  `use_myproc`). **→ Fixed:** the CAP-2 class-byte check extended to PG17+.
- **CAP-7 🟡** (`wait_event.c:773-776`, `discovery.c:385,463`) — `popen()` command
  lines with unquoted/weakly-quoted `pg_bindir`/`pg_user` in a root daemon.
  **→ Fixed:** fork/execve with argv (no shell).
- **CAP-8 🟡** (`backend.c:143-152`) — watchpoint ENABLE happened before state_map
  preseed; a transition in the window mis-timed the opening interval (re-exercised
  on every escalation). **→ Fixed:** open disabled → preseed → enable.
- **CAP-6/9/10/11/12 🟡/🔵** — `seen_query_ids` (4096) never ages; PID-reuse window
  on the raw first-event path; PID-namespace assumption unchecked; postmaster
  restart exits the daemon (ops contract — documented + supervisor guidance); no
  RLIMIT_NOFILE bump and EMFILE attach failures are quiet. **→ Folded into T4 where
  cheap** (RLIMIT_NOFILE bump + EMFILE-specific warning; PID-ns and
  postmaster-restart contracts documented), otherwise documented.

**SMP findings (sampled tier robustness)**

- **SMP-1 🟠** (`sampler.c:28-96`) — a total read failure (EPERM on
  `process_vm_readv` + fallback) was completely silent and indistinguishable from
  an idle DB; only a counter nobody watched moved; the anomaly baseline learned
  AAS=0. **→ Fixed:** loud log on first/persistent failure + health flag in the
  control-socket `status`.
- **SMP-2 🟠** (`sampler.c:53-91`) — the batched sweep read all targets through one
  pid; the safety argument ("foreign local address faults") failed for
  `.data`/`.bss` addresses mapped in every forked child — the read *succeeds* and
  returns the reader-pid's value, misattributed. **→ Fixed:** only batch targets
  known to be in shared memory; per-pid reads otherwise.
- **SMP-3 🟡** (`daemon.c:167-174`) — missed timer ticks (`expirations` ignored)
  dropped sample weight → AAS/DB-Time deflated under load, exactly when it matters.
  **→ Fixed:** weight samples by actual inter-tick elapsed time.
- **SMP-4 🟡** — per-backend BPF map lookup syscall per tick (batched); dead
  preallocated iovec fields removed. **SMP-5 🔵** — jitter decided in T2 (above).

**Acceptance (met):** no known path where the daemon records wrong-or-nothing
without a loud signal (log + control-socket health + metric).

---

### Phase T5 — Durability & retention ✅ [DONE v0.13 — PR #41]

**Owns:** DUR-1..10. **CHANGELOG [0.13]:** "durability & retention" bullet.

- **DUR-1 🔴** (`event_writer.c:155`, `summary_writer.c:412`) — `fopen("wb")` on
  fixed-name `current.trace`/`current.summary`: a daemon crash + restart at :59
  **erased up to an hour** of otherwise-recoverable data. **→ Fixed:** on startup,
  recover and rename-aside an existing current file (collision-safe name), never
  truncate.
- **DUR-2 🟠** (`event_writer.c:439-448`) — rotated filename derived from
  start-hour; a restart mid-hour (or a DST fold via `localtime_r`) renamed onto the
  same archive name, clobbering it. **→ Fixed:** collision-safe rename (suffix on
  existing target).
- **DUR-3 🟠** — retention was hours-only, no size cap; full-tier on a busy server
  could fill the disk inside the retention window; orphaned `current.trace`,
  `query_texts.jsonl`, `backends.jsonl` were never cleaned. **→ Fixed:**
  `--retention-gb` size cap + orphan cleanup.
- **DUR-4 🟠** (`query_text.c:142`) — `query_texts.jsonl` truncated on every
  restart, orphaning text for all retained query_ids (server assumes append).
  **→ Fixed:** append-open + dedup-on-load.
- **DUR-9 🟠** (`server.c:753-815`) — pid-filtered long-window queries forced the
  raw path and loaded every event in range into one unbounded doubling array (the
  immutable-file cache has a budget; this working array didn't). **→ Fixed:** filter
  during load; added a bound.
- **DUR-5/6/7/8 🟡** — no fsync policy (documented durability level); no written
  format spec (**wrote `docs/TRACE_FORMAT.md`** — endianness, packing, fsync
  policy); meta-path block scan lacked the sanity caps the fallback scan has
  (`event_reader.c:85-99` vs `:117`); one SAMPLES block per tick → ~36k tiny
  blocks/hour at 10 Hz and the footer's 100k-block cap broke at high sample rates
  (**→ batch ~1 s of ticks per block**).
- **DUR-10 🟡** (`query_text.c`) — raw query text stored with no redaction option,
  silent 4096-id cap, first-seen TOCTOU can capture the wrong statement, looser file
  perms than traces. **→ Fixed (minimum):** log the cap, match trace-file perms,
  document PII.

**Acceptance (met, DoD #5):** a daemon crash loses at most the unflushed tail,
never a committed hour; disk usage provably bounded.

---

### Phase T6 — Client transport trust ✅ [DONE v0.13 — PR #37]

**Owns:** UI-1..13 (client-only; paired with T0). **CHANGELOG [0.13]:** "client
transport trust" bullet.

- **UI-1 🔴** (`web/static/lib/transport.js:57` + `web/static/app.js:135-185`) —
  bridge error envelopes (`{"id":N,"error":...}`) were **resolved as data**; nothing
  outside control.js read `.error`. When SSH died: tables showed "No data", the AAS
  chart kept stale paint, Live kept ticking on a frozen window — under a green
  "connected" pill (the worst possible failure mode for an incident tool).
  **→ Fixed:** transport rejects on `.error` + a visible degraded-transport state
  distinct from "no data".
- **UI-2 🟠** (`web/bridge.go:137-143`) — request-ids were client-generated per tab
  but `pending` was shared per-bridge: two tabs collided and one received the
  *other tab's data* as a valid reply. **→ Fixed:** namespace ids per WS connection
  in the bridge.
- **UI-3 🟠** (`web/main.go:43-47` + `web/bridge.go:91-117`) — SSH session death was
  terminal (no respawn; the browser WS happily reconnected to the dead-bridged local
  server → permanent UI-1 state). **→ Fixed:** bridge respawns SSH with backoff +
  `ServerAliveInterval`/`BatchMode`.
- **UI-4 🟠** (`web/static/app.js:109-113`) — `init()` ran on every WS reconnect and
  was not idempotent: duplicate `echarts.init` without dispose, a new ViewManager,
  duplicate tab/resize listeners (N+1 handlers after N reconnects). **→ Fixed:**
  idempotent init / init-once + explicit resync path.
- **UI-5 🟠** (`web/static/views/active.js:61`) — an empty AAS response early-returned,
  leaving the previous window's paint on screen while the tables said "No data".
  **→ Fixed:** clear/empty-state the chart on `!hasData`.
- **UI-6 🟠** (`web/static/lib/builders/timeline.js:41`) — query text concatenated
  unescaped into tooltip HTML: any DB user's SQL could inject markup into the DBA's
  browser. **→ Fixed:** `esc()` it (table path already did); Node test for the
  formatter.
- **UI-7 🟠** — nothing tested `web/bridge.go`/`web/main.go` (the mock replaced
  both), and `transport.js`/`view-manager.js` had no Node tests. **→ Fixed:** a
  **bridge integration test** (spawns real `pgwt-server` locally over a pipe, no
  SSH) + `transport.test.mjs`/`view-manager.test.mjs` (error envelopes,
  single-flight superseding, epoch cancellation); Playwright kill-the-mock →
  degraded transport, not "No data".
- **UI-8..13 🟡** — `extractID` substring scan (id-first invariant parsed/documented
  + tested); daemon control plane re-probed after failure; escalation annotation
  now shades the actual window only; **all times labeled UTC** and custom-range
  `datetime-local` inputs made local-time-correct (a UTC+3 user no longer gets a
  3 h-off window); a per-session **WS token** now gates the socket that can escalate
  against production; `vendor/README` provenance note for vendored ECharts.

**Acceptance (met):** SSH/server death is visibly distinguishable from an idle
database within one refresh interval, and recovers without a page reload; two tabs
never exchange data.

---

### Phase T7 — Release engineering & matrix ✅ [DONE v0.13 — PR #44]

**Owns:** TST-9..12, repo hygiene. **CHANGELOG [0.13]:** "release engineering &
matrix" bullet.

- **TST-9 🔴** — the claimed matrix ≈24 cells (PG13/17/18 × EL8/EL9/Ubuntu ×
  PIE/non-PIE); automated coverage was 1 cell (highest-version PG on one box);
  EL8/non-PIE had been validated exactly once ever. **→ Fixed:** a **nightly
  containerized matrix** (rockylinux:8 / rockylinux:9 / ubuntu:24.04 — host kernel
  shared, so capture works from the privileged container) that builds, unit-tests,
  and runs the capture smoke on each distro; the rockylinux:8 cell also exercises
  the **static libbpf/bpftool bundling path** (TST-12 🟡) that gating CI never
  compiled; plus the PGDG PG version matrix in the CI smoke job.
- **TST-10 🟠** — format tests were same-code round-trips. **→ Fixed:** a committed
  **golden trace fixture** per released format revision, decode + checksum in CI.
- **TST-11 🟠** — no release process behind v0.1–v0.12 (no CHANGELOG/RELEASING.md/tag
  workflow); stale prebuilt binaries at the repo root; **no client/server version
  handshake** although a skewed Mac-client/Linux-server pair is the normal
  deployment state. **→ Fixed:** `RELEASING.md` (run matrix, check skip count, tag,
  record versions) + `CHANGELOG.md`; a **client/server version handshake** (server
  reports version + protocol in `info`; the client warns loudly on mismatch, never
  refuses); stale binaries removed from git; `.gitignore` for local build products.

**Acceptance (met):** a release is a checklist, not a memory; the EL8 build path is
exercised nightly; version skew warns instead of misbehaving.

---

### Sequencing (as executed)

Pairs (one PR per phase; the two phases of a pair are file-disjoint by construction
and were developed in parallel):

```
Pair 1:  T0 ✅ (CI/tests)        ∥  T6 ✅ (client/bridge)
Pair 2:  T1 ✅ (merge/summaries) ∥  T4 ✅ (capture/sampler hardening)
Gate  :  T2 ✅ decision (AAS semantics + io_worker — PG18 run done)
Pair 3:  T2 ✅ (AAS impl)        ∥  T5 ✅ (durability)
Pair 4:  T3 ✅ (escalation)      ∥  T7 ✅ (release eng)
```

- T0 first, non-negotiable — the net every other phase is verified against (B1
  lesson). T3 after T2 because the anomaly metric shape changes in T2. Every
  capture-behavior phase (T1–T5) required a green live run on a real box in addition
  to CI, now with `--require-live` so the gate cannot pass vacuously.

### Definition of Done (milestone) — item-by-item honest status

1. **✅** CI includes a real-PG capture smoke job, matrix PG 13/16/17/18, green
   (T0). The nightly containerized matrix (T7) additionally exercises
   EL8/EL9/Ubuntu cells.
2. **✅** (was 🔄 at plan time) Adversarial synthetic-trace tests prove: no double
   counting across escalation boundaries (trace AND live view), correct fidelity
   labels on every response path (T1), budget mathematically bounded (T3).
3. **✅** A CPU-storm incident is detectable (sampled AAS includes on-CPU; anomaly
   engine fires on it) (T2).
4. **✅** (was 🔄) No known silent-wrong-data path: every degraded state is visible
   in logs + control-socket status + UI. Capture/sampler (T4), transport (T6),
   escalation-billing honesty (T3) all done.
5. **✅** Daemon crash/restart loses at most the unflushed tail (T5).
6. **⚠️ LITERALLY UNMET — honest flag.** DoD #6 requires
   `run_all.sh --require-live` green **twice consecutively on EL9 and EL8 boxes with
   ZERO live-section skips** (the EL8 cell re-validated for the first time since
   #24; procedure in `RELEASING.md` step 4). The v0.13 release actually ran
   **64 pass / 0 fail / 3 skip** on each of EL9 / EL8 / Ubuntu 24.04 (PG 13/16/17/18)
   — see CHANGELOG [0.13] closing line. The **3 skips are the known
   web-ui/container skips** (browser/Playwright + container-only cells that cannot
   run headless in that harness), *not* live-capture skips — so the spirit of the
   gate (no silent live-section skip) holds, but the letter ("ZERO live-section
   skips") is not met. Recorded here rather than papered over.
7. **✅** `RELEASING.md` exists (T7) and v0.13 shipped through it (release **PR #53**,
   `release-v0.13`; `CHANGELOG [0.13] — 2026-07-21`).

### Explicitly parked / resumed after Track T

- **B6 new analysis views** — resume after T1/T3: all three are EXACT-required, so
  their value depends on escalation windows containing *correct* exact data.
- **Track D (PG14–16)** — resume after T4: D1's version-adapter design ("data tables
  + runtime validation, fail loudly") is the same machinery T4 hardens.
- **Track C2/C3 (pre-8.2 RHEL perfbuf/BTF fallbacks)** — parked indefinitely
  (shrinking population, not needed for current targets).
- **Prometheus exporter** — unchanged (future consumer; metric names already stable).

---

### Measured CPU for the exact tier (T8) — design evolution

> The EL9 close-out validation of the Trust Milestone surfaced one more gap that
> **gated v0.13**: the exact (full) tier could not *see* CPU. The fix went through
> **three documented design iterations** — the plan (T8_PLAN), the compute revision
> (T8_REVISION), and the shipped source change (S3). All history is preserved here
> because each stage rejected the previous stage's mechanism for a concrete,
> measured reason. **The whole thing shipped in v0.13** (measured-CPU PR **#50**
> folding the T8 plan/base/straddle/revision/S3 work, plus follow-on fixes **#51**,
> **#52**). **CHANGELOG [0.13]:** "exact measured CPU" bullet.

### The problem (constant across all three stages) ✅ [context]

The exact tier's only sensor is a hardware watchpoint on `PGPROC->wait_event_info`
writes. PostgreSQL writes that field on wait **ENTRY** and wait **EXIT** only. CPU
time was therefore never observed — it was **inferred as the gap between two wait
events**, and materialized (emitted as a trace record) only when the *next* wait
began. Three consequences, all confirmed live on EL9 (Rocky 9.7, PG 18.4):

1. **In-progress CPU is invisible.** A backend computing for 60 s emitted nothing;
   its CPU appeared only retroactively when it next waited. Repro:
   `DO $$ ... FOR i IN 1..200000000 LOOP ... $$` running at capture time →
   `--mode full` time_model showed `CPU* 0.0%`; the trace file was **empty (341
   bytes, zero events)**.
2. **Capture end loses the open CPU stretch entirely.** Window close,
   de-escalation, daemon exit: T3's `flush_open_intervals()` flushed open WAITS but
   not the on-CPU state — the straddling CPU was never written.
3. **The live view lags structurally:** an interval shows 0% CPU during a storm, a
   later interval shows a giant spike that belongs to the past.

This is **not synthetic-only**: a runaway nested-loop over **cached** data (the
classic bad-plan incident) sets no wait events (shared-buffer hits don't wait), so
it is seconds of pure CPU with zero writes to the watched field. It also reached
the **default (tiered) mode**: inside an escalation window T1's exact-wins merge
(correctly) drops sampled records within marker-covered ranges — but the exact
stream it defers to was blind to in-progress CPU. Ironically, post-T2 a CPU storm
*triggers* escalation, and escalation then suppressed the sampled CPU data that
triggered it. (The anomaly **engine** was unaffected — it reads sampler batches
pre-merge; only the recorded/viewed window data degraded.) The **wait side was
untouched** by all of this — waits are event-bounded and exact. The defect was
exclusively the CPU quantity.

### Stage 1 — T8_PLAN: read `sum_exec_runtime` at watchpoint boundaries ✅ [SUPERSEDED by S3; plan doc PR #47]

**Principle:** *stop inferring CPU, measure it.* The kernel already accounts every
task's on-CPU time exactly in `task_struct->se.sum_exec_runtime` (ns precision,
advances only while the task runs). Two properties made it fit: the watchpoint BPF
handler already runs **in the target backend's task context** on every wait
boundary (one kernel-memory read per fire), and it is a **monotonic accumulator**
so "CPU since the last boundary" is answerable at any read cadence with zero
information loss (reading an exact accumulator is *not* statistical sampling —
nothing aliases, magnitudes are exact). **Bonus arithmetic:**
`gap_wall_time − measured_cpu = off-CPU-unaccounted time` (runqueue waits, cgroup
throttling, page-fault stalls, uninstrumented kernel blocking) — the Oracle-10046
"unaccounted-for time" equivalent, per gap, per query. The old gap-inference
actively *mis-counted* this as CPU (a preempted/throttled backend is "not waiting"
but not running; on a saturated host the inference lies most). **Built-in
self-validation:** across a gap labeled WAIT the accumulator delta must be ≈0.

**Rejected designs (do not re-litigate):**

- **Option A — periodic userspace flush of the inferred gap:** emits partial CPU
  intervals on a poll clock. Rejected — it doesn't measure CPU, it re-infers with
  poll quantization (sampling in disguise), inherits the runqueue-as-CPU error, and
  needs fiddly seam-dedup against concurrent BPF emissions.
- **Keep sampled CPU inside escalation windows (merge change):** rejected — when
  the straddling command finally waits, the exact tier emits one giant retroactive
  CPU interval overlapping the retained samples → double count unless the merge
  tracks CPU coverage at interval granularity (re-creates the FID-1 class); does
  nothing for `--mode full`; CPU stays sampled-fidelity inside exact windows.
- **`bpf_timer` periodic BPF emission:** not available on EL8 (4.18), a first-class
  target — dead on arrival.
- **`sched_switch`/`finish_task_switch` accounting ("S3"):** exact and more
  powerful (per-switch off-CPU breakdown) but the hook fires for *every* context
  switch system-wide and is a much larger BPF surface — **deferred** at plan time as
  the future off-CPU track. (This is the design that ultimately shipped — see Stage
  3.)

**Prerequisite — finish PR #46 straddle command-gate + PIE fix.** PR #46
(`t2-fix-el9-cpu-gate`, the base branch) fixed the *command-open gate* half of the
straddle problem (edge-triggered `on_report_activity` misses commands already in
flight at attach) in both tiers: sampler per-tick `debug_query_string` level-check
(`pgwt_read_cmd_gate` in `src/sampler.c`); exact-tier preseed seeds `cmd_open` from
`debug_query_string` (`preseed_state_map` in `src/backend.c`). **Known defect in
#46**, caught by its own new CI regression phase `phase_cpu_straddle`
(`tests/test_capture_smoke.py`) on the PIE matrix cells (PG13/PG18 Ubuntu, run
29431156369): both new reads used `d->debug_query_string_addr` — the raw symbol
vaddr from `pgwt_find_symbol_offset()`, correct on non-PIE (`readelf -h` →
`Type: EXEC`, the EL9 box) but **WRONG on PIE** (per-process runtime VA =
`pgwt_find_load_base(pid,binary) + vaddr`, the #24 lesson). **Fix (shipped as PR
#48, `straddle-cpu-gate-fixes`):** resolve `debug_query_string` per backend like
other globals; for PIE the per-pid VA differs only by load base, which is identical
for all backends of one postmaster (fork inheritance), so resolve **once per
daemon** and store the runtime VA in `d->debug_query_string_addr`, keeping the raw
vaddr separately for the BPF rodata — **and** the BPF-side `debug_query_string_addr`
rodata had the *same* PIE bug on the query-TEXT capture path, fixed with the same
load-base-adjusted VA. Acceptance: `phase_cpu_straddle` green on the PIE matrix
(PG13/17/18 Ubuntu) AND non-PIE EL9.

**Chosen mechanism (S1 + fallback), as specced in T8_PLAN §5** (the *source* was
later swapped by S3, but the format, probe, flush, and compute contract below all
shipped):

- **BPF side:** `struct pgwt_pid_state` gains `__u64 last_cpu_ns`; the watchpoint
  handler reads `se.sum_exec_runtime` once per fire (`bpf_core_field_exists` →
  `BPF_CORE_READ`), sets `evt.cpu_ns = cpu_now − st->last_cpu_ns` and updates
  `last_cpu_ns` on every fire — **except** the redundant-write suppression path
  must **not** advance it (or CPU between suppressed fires is lost), and the
  `!st->wp_live` seed path snapshots `last_cpu_ns` without emitting for that
  pseudo-gap. `struct pgwt_trace_event` gains `__u64 cpu_ns`; everything gated
  behind a `volatile const bool cpu_accounting` rodata flag the daemon sets after
  its startup probe (compiles out cleanly on old kernels/BTF-less builds).
- **Userspace seed/flush:** `preseed_state_map()` reads the backend's accumulator
  (`/proc/<pid>/schedstat` field 1) into `init_state.last_cpu_ns`. **Terminal
  flush (fixes symptom #2):** extend T3's `flush_open_intervals()` to also emit the
  open CPU stretch for backends with `last_event==0 && cmd_open` — read schedstat
  now, emit a final record `old_event=0, new_event=0(flush),
  duration=now−last_ts, cpu_ns=sched_now−last_cpu_ns` — **and call the same helper
  on daemon shutdown for `--mode full`** (previously only de-escalation flushed;
  that is *why the DO-loop trace was empty*, not merely CPU-less). **Live view
  (fixes symptom #3):** the open-interval display path computes
  `cpu = schedstat_now − entry.last_cpu_ns`,
  `unaccounted = (now − entry.last_ts) − cpu` at **display time only** — no interim
  mid-gap trace records (this is what makes the design seam-free: records exist
  only at real boundaries + terminal flush, nothing to dedup).
- **Trace format v3** (`event_writer.c`/`event_reader.c`/`docs/TRACE_FORMAT.md`):
  bump `PGWT_TRACE_VERSION` to 3; TRANSITIONS block gains **one varint column
  `cpu_ns`**; SAMPLES unchanged. Reader is v3-native; **v2 files stay readable**,
  surfacing `cpu_ns = PGWT_CPU_NS_UNKNOWN` (`UINT64_MAX` sentinel) → compute falls
  back to legacy gap-inference; **v1 stays rejected**. Golden fixtures: add
  `tests/fixtures/golden/rev3/` + checksum; the rev2 decode test **keeps running**.
- **Capability probe + fallback chain** (`daemon.c`, Track-C style, at startup, in
  order): **(S1, preferred)** BTF present AND
  `bpf_core_field_exists(se.sum_exec_runtime)` AND `/proc/self/schedstat` exists
  (CONFIG_SCHED_INFO) → full measured-CPU. **(S1b)** BPF read works but schedstat
  missing → per-backend `perf_event_open(PERF_COUNT_SW_TASK_CLOCK, pid)` counting fd
  (counting-only, no ringbuf) for the residual/seed reads (mind RLIMIT_NOFILE — T4
  bumps it; double the per-backend budget). **(Legacy)** neither →
  `cpu_accounting=false`, UNKNOWN sentinel, gap-inference, reported **LOUDLY** in
  the startup log + control-socket `status` (never silent).
- **Compute + views (semantics locked here):** **DB Time total is UNCHANGED**
  (still wall-clock active time = waits + gaps). Within a gap: `CPU* = measured
  cpu_ns`, **new row/class `Off-CPU* = gap − cpu_ns`** (floor at 0; clamp `cpu_ns`
  to gap on clock skew, count clamps). Off-CPU* is part of DB Time (real query
  elapsed time), rendered as a sibling of CPU* in time_model and as its own AAS
  category key `offcpu`. **Fidelity:** Off-CPU* exists only where measured `cpu_ns`
  exists (exact tier, v3); sampled/v2 windows show CPU* as before and **no Off-CPU
  row (absent, not zero)**. Wait-labeled gaps: measured `cpu_ns` during waits
  (should be ≈0) is folded into the wait's row (no per-wait CPU splits) but
  accumulated into a **`wait_gap_cpu_ns_total`** self-check counter. query_event /
  session views switch CPU columns to measured values automatically. Sampled tier
  UNCHANGED. Mirror every response-shape change in `tests/mock_server.py`.
- **T8_PLAN §5.7 also-in-this-phase:** the **`test_multi_window` %DB summation
  fix** (a confirmed *test* bug from EL9: it summed parent classes AND their
  children — `Timeout` + `Timeout:PgSleep`, … → 120-130%; the fix sums top-level
  rows only — those without `:` in the name, plus CPU* and Off-CPU*). Do **not**
  touch the cross-validate Hz-matrix flakiness in code — it is box-load noise on
  the 2-vCPU validation box; the close-out runs use a resized box.

**Why Stage 1 was superseded:** see Stage 2 §1 — `sum_exec_runtime` is
tick-quantized (only current at a scheduler tick or context switch), so sub-ms CPU
bursts before a wait are read *stale* and leak into the following interval.

### Stage 2 — T8_REVISION: conserved CPU* + residual Off-CPU* ✅ [SUPERSEDED source, KEPT compute; PR #49→folded into #50]

Replaces T8_PLAN's **per-interval** Off-CPU* math and its self-check; **keeps** the
measured `sum_exec_runtime` capture, format v3, capability probe, and terminal
flush.

**What EL9 validation established.** The measured mechanism (read
`task->se.sum_exec_runtime` at each watchpoint boundary) is correct **in sum** but
imprecise **per interval**, because `sum_exec_runtime` is only brought current at a
scheduler tick (CONFIG_HZ=1000 → 1 ms) or a context switch. A backend that wakes,
runs a **sub-millisecond CPU burst**, then enters a wait is read at the wait-entry
*write* — **before** the deschedule updates the accumulator — so the burst's CPU is
missing there and surfaces in the *next* read, attributed to the **wait** interval
instead of the CPU interval that burned it. Measured:

- Pure `pg_sleep` (real sleeps, no CPU between): `wait_gap_cpu ≈ 88 ms` — the
  boundary/tick leak is negligible when there's no CPU to leak.
- pgbench (sub-ms bursts between frequent waits): `wait_gap_cpu ≈ 24.3 s ≈ CPU*
  itself` — roughly **half of all CPU is mis-timestamped** into the following wait
  intervals.

**Two things reliable, two not.** RELIABLE: (a) every interval's **wall duration**
(watchpoint timestamps — exact); (b) **Σ of all `cpu_ns`** = final − initial
`sum_exec_runtime` = the backend's **true total CPU** (a telescoping sum,
leak-immune). NOT reliable per-interval: which specific interval a sub-ms burst is
credited to — hence any per-interval CPU/off-CPU/spin split for wait-interleaved
workloads.

**Why the obvious per-interval fixes fail (do not attempt).** For the sequence
`CPU1(5,5) → LWLock1(20,0) → CPU2(0.4,0.4) → LWLock2(15,0) → IO(10,0) → CPU3(3,3)`
(dur, trueCPU in ms; true DB=53.4, CPU=8.4, LWLock=35, IO=10):

- **Model A** — count wait `cpu_ns` into CPU* AND keep full wait durations,
  `Off-CPU* = Σ(we==0)(dur−cpu_ns)`: **double-counts** the leaked CPU → DB Time
  overshoots by `Σ wait cpu_ns` (0.6 ms here; ~24 s on pgbench).
- **Model B** — wait class `= dur − cpu_ns`: **conserves DB Time** but deflates
  wait time and inflates Off-CPU* by the leak (~24 s on pgbench). Both unacceptable.

**The fix — residual Off-CPU*, from exact totals only.** Never use per-interval
`cpu_ns` for the CPU/off-CPU *split*; use it only in the **sum**. Per accounting
window (and per AAS bucket):

```
CPU*     = Σ cpu_ns over ALL intervals (we==0 AND wait intervals)   [conserved = true CPU]
wait[c]  = Σ duration of intervals of wait-class c                  [exact from timestamps]
DB Time  = Σ duration over ALL non-idle intervals                   [exact from timestamps]
Off-CPU* = max(0, DB Time − CPU* − Σ_c wait[c])                     [residual → runqueue/unaccounted]
```

Identity holds by construction: `CPU* + Off-CPU* + Σ wait[c] = DB Time`. It is
**leak-immune** — the leak only moves `cpu_ns` between the null-interval and the
following wait-interval, but CPU* is a *sum* (unchanged) and DB Time / wait
durations come from *timestamps* (unaffected). Verify on the §1 sequence:
`Off-CPU* = 53.4 − 8.4 − 45 = 0` (free core → correct; Models A/B gave 0.6).
Oversubscribed pure-CPU: `Off-CPU* = DB − CPU* = 34%` (the runqueue — the Finding #1
headline, now correct). Oversubscribed wait-heavy: `Off-CPU* = DB − CPU* − waits`
= real runqueue during null gaps.

**Honest scope statement (for docs/UI copy).** ACCURATE: per-backend / per-query /
per-window **CPU\*** (fixes the under-report), all **wait durations**, **DB Time**,
and **Off-CPU\*** as the residual (runqueue + genuinely-unaccounted). NOT
AVAILABLE: trustworthy **per-interval** CPU placement, and any **per-wait "spin vs
blocked"** split — the wait interval's `cpu_ns` is boundary-leaked *pre-wait* CPU,
not in-wait spin, so a spin/blocked decomposition would *lie* (it would only become
available with an exact per-interval source — see Stage 3 / future work).

**Concrete changes (T8_REVISION §3):**

- **compute.c** (`compute_time_model`, `compute_aas`): CPU* total = Σ `cpu_ns` over
  ALL intervals whose `cpu_ns != UNKNOWN` (both `old_event==0` and wait) — the wait
  `cpu_ns` that previously fed only the self-check counter is now *added in*. Remove
  the per-interval Off-CPU* split (`dur − cpu_ns` per on-CPU gap;
  `offcpu_aas += ov*(1−cpu_frac)`) → replace with the **residual**. **Remove the
  stale-0 → full-gap-as-CPU fallback** for measured-capable data (it caused replay
  `CPU* > physical cores`, Finding #1): a stale-0 `cpu_ns` on a `we==0` gap means
  "the burst leaked to an adjacent interval," and since CPU* is now a sum the leaked
  burst is already counted in its landing interval. **Keep the fallback ONLY for
  `cpu_ns == UNKNOWN`** (legacy/v2/sampled): show CPU* by old gap-inference and **no
  Off-CPU\***. Off-CPU* fidelity-gated to windows with measured `cpu_ns`; clamp at 0
  and count clamps in `offcpu_clamped_total`.
- **BPF / terminal flush (Finding #1 — replay > physical):** the terminal flush and
  the live open-interval path must emit **schedstat-measured** `cpu_ns` for the
  still-open interval (`/proc/<pid>/schedstat` field 1 delta since seed), **not
  wall-as-CPU** — this bounds a pure-CPU straddler's CPU* to actual on-CPU ns
  (≤ wall × 1 core/backend), so replay can never report CPU* > physical. (T8 added
  the flush; the bug was that it recorded the gap-inference *wall* value.)
- **Self-check (Finding #2 reframe):** the premise "a wait burns ~0 CPU" is **FALSE**
  (sub-ms bursts leak into waits). Replace the `test_cross_validate_tiered`
  wait-gap-≤1% check with the **conservation invariant**: over a pgbench window,
  tracer **CPU\*** must equal Σ traced backends' `/proc/<pid>/stat` utime+stime
  deltas within ±10%. **KEEP `wait_gap_cpu_ns_total`** as an *observability* metric
  (it quantifies how much CPU is boundary-attributed), **not** a gate.
  *(This "KEEP" is the winning side of the doc-conflict noted under Stage 3.)*
- **Views/mock/metrics:** Off-CPU* row/category shape unchanged (`offcpu`), now fed
  by the residual; mirror field changes in `tests/mock_server.py`;
  `docs/TRACE_FORMAT.md` needs no format change (the v3 `cpu_ns` column stays) —
  only the compute/semantics prose updates.

**Deferred alternative flagged in the revision (§5):** S3 (`sched_switch` tracing)
would make the residual model *exact per-interval* — see Stage 3, which is what
shipped.

### Stage 3 — S3: `tp_btf/sched_switch` accumulator (the source that SHIPPED) ✅ [DONE v0.13 — PR #50; spec kept in docs/S3_SCHED_SWITCH_CPU.md]

S3 **replaces the *source* of `cpu_ns`, not the compute.** The residual
CPU*/Off-CPU*/DB-Time contract from Stage 2 stays byte-for-byte; S3 simply feeds it
*exact* per-interval `cpu_ns` instead of leaky `sum_exec_runtime` reads, closing the
EL9 findings the accumulator source could not. Full spec is retained in
`docs/S3_SCHED_SWITCH_CPU.md`; the essentials:

- **The insight:** track, per tracked backend, the instant it came on-CPU.
  `exact_cpu_now(pid) = cpu_ns_total[pid] + (on_cpu_ts[pid] ? now − on_cpu_ts[pid] : 0)`.
  `cpu_ns_total` = Σ completed on-CPU stretches (updated when the task goes
  **off**-CPU); `on_cpu_ts` = start of the current stretch (set when it comes
  **on**-CPU). At a wait-boundary the backend is still on-CPU, so `now − on_cpu_ts`
  is the current stretch — **no stale accumulator, no tick floor**. This fixes the
  pure-CPU DO-loop (23k wait-event flips/s): each sub-tick `we==0` interval reads
  its exact on-CPU ns.
- **BPF:** `struct pgwt_pid_state` gains `__u64 cpu_ns_total` and `__u64 on_cpu_ts`
  (the T8 `last_cpu_ns` field is repurposed). A `SEC("tp_btf/sched_switch")` program
  (with a `raw_tracepoint/sched_switch` fallback for no-BTF) accounts prev→off and
  next→on, keyed off the **same `state_map`** the tool already maintains. Cost is
  one state_map lookup per switch; misses (untracked pids) return in ~tens of ns.
  Per-pid state, task on ≤1 CPU at a time → no cross-CPU race, plain u64, no atomics.
  `read_task_cpu_ns()` becomes exact via `read_task_cpu_ns_for(st, now)`; the
  `sum_exec_runtime` CO-RE read is **deleted entirely** (strictly worse — leaky, and
  its stale-0 landmine), so the fallback chain is **S3 → gap-inference**.
- **Userspace:** `preseed_state_map()` sets `on_cpu_ts = now` if the backend is
  currently on-CPU (`/proc/<pid>/stat` field 3 == 'R') else 0, `cpu_ns_total = 0`
  (base 0 at attach — only deltas matter); the schedstat seed read is dropped. The
  terminal flush computes the open interval from `read_task_cpu_ns_for` (now exact);
  the `/proc/schedstat` read is dropped. Sampler UNCHANGED. The daemon attaches
  `on_sched_switch` in the load path (always attached when the full tier is armed,
  detached with the tier), **before the first backend runs**.
- **Capability probe / status field:** try load+attach `on_sched_switch` (tp_btf if
  BTF, else raw_tracepoint) → `cpu_accounting = MEASURED (sched_switch)`; on attach
  failure → `legacy` (cpu_ns UNKNOWN → gap-inference, no Off-CPU*). Reported in the
  startup log and the control-socket `status`/`metrics` `cpu_accounting` field
  (`"sched_switch"` | `"legacy"`).
- **Compute / format / views: NO CHANGE** — S3 only changes where `cpu_ns` comes
  from, so PR #49's red windowed-straddle cell turns green (exact `cpu_ns` → no
  stale-0 → no misattribution).
- **Overhead (measured, EL9, 2026-07-19):** a pessimistic proxy (bpftrace
  `sched_switch` accumulating for *all* pids) on pgbench/4 vCPU: 110k–126k
  ctx-switches/s; TPS baseline 5520–5619 vs probe 5469–5706 (−c8), indistinguishable;
  latency at the noise floor. **≤ low-single-digit %** → cheap enough to run
  always-on with the full tier; no escalation-window gating needed.
- **S3 live wins (EL9):** windowed straddle (the PR #49 red cell) → cpu AAS ≈ 1 (not
  0.04); conservation → tracer CPU* within ±5% of Σ `/proc` utime+stime (the
  `sum_exec_runtime` source had missed by ~4×); oversubscription → Off-CPU* ≈
  runqueue fraction and replay CPU* ≤ physical; free core → Off-CPU* ≈ 0.

**⚠️ Documentation conflict (recorded honestly): `wait_gap_cpu_ns_total`.**
S3 §8 (its metrics list) says to **DROP `wait_gap_cpu_ns_total`** — arguing it is a
`sum_exec_runtime` artifact, since with S3 an on-CPU spin during a wait_event is
real and correctly lands in CPU*. **T8_REVISION §3.3 says KEEP it** as an
observability metric (it quantifies boundary-attributed CPU). **The shipped code
KEPT it.** Evidence: `FUTURE_WORK`'s "on-CPU-spin vs off-CPU-blocked" item states a
wait interval's `cpu_ns` is "already summed into the `wait_gap_cpu` observability
metric," and the multi-window future item continues to reference it. So the
REVISION §3.3 decision won over S3 §8; the metric remains as diagnostics, not a
gate.

**Shipping note (S3 §10 workflow):** T8 (plan) + revision + S3 were **folded into
one measured-CPU PR to master** — which is why branches `t2-fix-el9-cpu-gate`
(PR #46, the base) and the `t8-measured-cpu` implementation (PR #49) do not appear
as standalone merges; they were superseded by/folded into **PR #50**
(`s3-sched-switch-cpu`). The measured-CPU work is validated across the
EL9 / EL8 / Ubuntu 24.04 matrix (PG 13/16/17/18).

### Two live-view CPU bugs fixed this session (removed from FUTURE_WORK) ✅ [DONE v0.13]

Both were found in OS/PG matrix validation, are **waitless pure-CPU** commands that
read `CPU* = 0`, and were previously parked but are now fixed and removed from the
future list:

- **#51 — `has_closed_data` guard suppressed a fork-caught backend's *ongoing*
  CPU** (`s3-cpu-semantics-fix`). `pgwt_read_state_map` (`src/map_reader.c`) used to
  skip a backend's current OPEN interval whenever `d->accum` already held a CLOSED
  segment for the same `(pid, wait_event)`. A **fork-caught compute backend** passes
  through a startup `we==0` segment, so the guard suppressed its **entire ongoing
  on-CPU loop** (a pinned query read ~0 CPU live, `%DB` nonsense). **Fix (2026-07-19):**
  the guard was made **CPU-only** — on-CPU (`we==0`) always accounts its open
  stretch (which the measured-CPU S3 feature requires). *(The analogous WAIT case
  remains parked — see the future list below.)*
- **#52 — pre-existing backend the one-shot startup scan missed was never recovered**
  (`fix-straddle-scan-race`). A backend already running at attach that the one-shot
  startup scan raced past was never picked up → a waitless pure-CPU command read
  `CPU* = 0`. **Fix:** `pgwt_recover_unattached_backends` recovers pre-existing
  backends the startup scan misses. (`test_cpu_time` was also adjusted to fire
  compute *after* attach — the reliable fork path, not the straddle scan-race.)

Both are recorded in the CHANGELOG [0.13] T8 bullet.

---

### Trust Milestone & measured-CPU status table

| Phase / stage | Owns | Ships in | PR(s) | Status |
|---|---|---|---|---|
| **T0** Safety net: real-PG CI + gate hardening | TST-1..8 | v0.13 | #36 | ✅ DONE |
| **T1** Fidelity merge & summary honesty | FID-1..5, FID-7 | v0.13 | #39 | ✅ DONE |
| **T2** AAS semantics (decision + impl) | AAS-1, SMP-5; io_worker | v0.13 | #40 (decision), #42 (impl) | ✅ DONE (gate passed 2026-07-12) |
| **T3** Escalation budget & trigger quality | ESC-1..12 | v0.13 | #43 (+#45 status docs) | ✅ DONE |
| **T4** Capture & sampler hardening | CAP-1..12, SMP-1..4 | v0.13 | #38 | ✅ DONE |
| **T5** Durability & retention | DUR-1..10 | v0.13 | #41 | ✅ DONE |
| **T6** Client transport trust | UI-1..13 | v0.13 | #37 | ✅ DONE |
| **T7** Release engineering & matrix | TST-9..12, hygiene | v0.13 | #44 | ✅ DONE |
| **T8 · Stage 1** T8_PLAN — read `sum_exec_runtime` at boundaries | in-progress CPU invisible; flush; live lag | v0.13 (superseded source) | #47 (plan), #46 (base), #48 (straddle+PIE) | ✅ DONE (source superseded by S3) |
| **T8 · Stage 2** T8_REVISION — conserved CPU* + residual Off-CPU* | per-interval leak; residual model | v0.13 (compute kept) | #49 → folded #50 | ✅ DONE (compute shipped) |
| **T8 · Stage 3** S3 — `tp_btf/sched_switch` accumulator | exact per-interval `cpu_ns` source | v0.13 (shipped source) | #50 | ✅ DONE — spec kept in `docs/S3_SCHED_SWITCH_CPU.md` |
| Live-view fix: `has_closed_data` (CPU-only guard) | fork-caught ongoing CPU | v0.13 | #51 | ✅ DONE |
| Live-view fix: straddle scan-race recovery | `pgwt_recover_unattached_backends` | v0.13 | #52 | ✅ DONE |
| Release v0.13 | tag / CHANGELOG | v0.13 | #53 | ✅ DONE (2026-07-21) |

**Definition-of-Done ledger:** #1 ✅, #2 ✅, #3 ✅, #4 ✅, #5 ✅, **#6 ⚠️ literally
UNMET** (release ran **64 / 0 / 3**, not zero live-section skips — the 3 are known
web-ui/container skips, not live-capture skips), #7 ✅ (shipped via `RELEASING.md`,
PR #53). Doc-conflict on `wait_gap_cpu_ns_total`: S3 §8 said DROP, REVISION §3.3
said KEEP — **code KEPT it**.

### Deferred / future (from FUTURE_WORK) — all [LEFT / post-v0.13]

Parked with rationale so nothing is lost; each ships only when built (then it moves
to a CHANGELOG entry).

- **Residual straddle live-CPU flake: the seed→arm race (observed 2026-07-31,
  PG17 CI, first sighting since PR #56; then 3 hits in one day across
  PG13/17/18).** ✅ **FIXED — `pgwt_sweep_stale_state` (src/backend.c), the
  ATTACHED-backend sibling of the #52 recovery.** PR #56 opens `on_cpu_ts` at
  every watchpoint fire, which closed the missed-switch-in mode. One rarer
  window remained: the backend's transient wait ends in the microseconds
  BETWEEN the preseed's `/proc` wei read (`backend.c preseed_state_map`) and
  `PERF_EVENT_IOC_ENABLE` — the wei write happens before the watchpoint is
  armed, so no fire ever occurs; if the backend then runs a waitless loop
  uninterrupted for the whole capture (no sched_switch on a quiet 2-vCPU
  host), the entry stays frozen at the dead wait and live CPU* reads 0 while
  the terminal flush stays correct (same signature as the fixed bugs). The
  shipped fix is level-triggered per the recovery philosophy but keys on the
  STATE mismatch, not the originally-sketched `/proc/stat` RUNNING check
  (which would miss a stale entry on a preempted backend and can never see
  the wrong LABEL): each timer tick, every attached backend's actual
  `/proc` wei is compared to its state_map `last_event`; a STABLE mismatch
  (two reads 2ms apart, equal, both != `last_event`) on a FROZEN entry (no
  fire for ≥1s) with an entry re-check (a real fire between the reads
  disqualifies — the clobber guard) is repaired by the exact attach reseed
  (`preseed_state_map`) — the never-bounded stale interval is DROPPED, never
  emitted. One INFO line per repair + `state_reseeds_total` on the control
  socket. Deterministic regression `phase_stale_seed_sweep` via the
  `PGWT_TEST_STALE_SEED` hook (attach seed poisoned with a fake stale wait —
  the missed fire on every run) incl. the negative under
  `PGWT_TEST_NO_STALE_SWEEP` (sweep disabled, recovery active → live CPU*
  stays at the floor). Both runs also hold sched_switch inert
  (`PGWT_TEST_NO_SCHED_ONCPU`, the #56 house pattern) — a busy host otherwise
  opens the hog's stretch by preemption and defeats the negative (observed on
  the EL8 box) — with the sweep's repair reseed exempt from that hook's seed
  force (the one sanctioned non-fire opener; `preseed_state_map`).

- **REOPENED 2026-08-04 — a FOURTH straddle live-CPU mode exists.** A
  `capture-smoke (PG 17)` run on the U2b branch (which includes the #63
  sweep) failed with the exact `live CPU* = 0ms, trace correct` signature
  and **no `reseeded stale state` INFO line** — the sweep never fired, so
  the entry was NOT in the frozen-mismatch state and this is not the
  seed→arm race. The three shipped fixes (#52 scan recovery, #56 fire-time
  open, #63 stale-state sweep) each carry a deterministic regression and
  remain correct; a distinct mode remains. Next step (queued): make the
  failure self-diagnosing — on `phase_pure_cpu_straddle` live-CPU FAIL,
  dump the hog's state_map entry (a debug control-socket command or a
  targeted stderr dump at final tick) + the control metrics
  (`state_reseeds_total`, sched counters), so the next CI hit yields the
  entry's actual `{last_event, last_ts, on_cpu_ts, cpu_ns_total,
  last_cpu_ns, wp_live}` instead of another inference cycle.
  **FIRST CAPTURE (2026-08-04, PG18 CI, the dump's maiden run):** the hog's
  shutdown entry was HEALTHY AND COUNTABLE — `last_event=CPU*, cmd_open=1,
  on_cpu_ts open (age 90ms), cpu_ns_total=14.93s, last_cpu_ns=0` — i.e. the
  live read's inputs at shutdown yield cpu_open ≈ 15s, byte-identical in
  shape to a PASSING box run's end state. The fourth mode is therefore a
  TICK-TIME phenomenon invisible to the shutdown snapshot. Context captured:
  a ZOMBIE second CPU hog from a prior phase (cmd_open=0, still burning at
  shutdown) sharing the 2 vCPUs, `wp_attach_failures_total=1`,
  `wait_gap_cpu_ns_total=1.77s`. Instrumentation extended with
  `STATEDUMP-TICK` (per-tick cpu_open computation inputs in the live
  reader, same env gate) — the next hit shows what each tick actually
  computed.
  **SECOND CAPTURE (2026-08-04, PG13 CI, U3 branch):** shutdown entry again
  healthy/countable (we==0 from seed, on_cpu open, cpu_ns_total=16.7s,
  cpu_accounting=1, 23 tick prints) — and **ZERO STATEDUMP-TICK lines from
  ANY pid**: the live reader's we==0 open-interval block never executed
  during ticks, in a process whose shutdown dump iterates the same map
  successfully. The CPU math is exonerated; the failure lives in the tick
  path's reach (handle_timer gating, the tick-time map iteration, or an
  early skip taken by every entry). Third-generation instrumentation queued:
  STATEDUMP-TIMER (per-tick gate decision) + STATEDUMP-SCAN (iteration and
  skip-path counters).
  **RESOLVED 2026-08-11, merged 2026-08-12 (PR #81) — FOURTH mode was an unbounded event-ring drain.**
  The main-loop liveness trap caught the whole 13.78s capture inside
  `event_ring_drain` (`loop_iterations=1`, `timer_ticks=0`; the hog's state
  remained healthy/countable and its terminal trace flush was correct).
  Finer callback-stage timing plus the bounded
  `PGWT_TEST_EVENT_CALLBACK_DELAY_US` fault pinned the leaf: bundled libbpf
  1.4.7's `ring_buffer__consume()` reloads `producer_pos` until an iteration
  sees no new data, so a sustained producer can keep one consume call alive
  indefinitely. The forced pre-fix run processed 164601 callbacks in one
  20.686s drain; aggregate callback time was 20.673s, no callback exceeded
  21.5ms, no writer/summary/accumulator leaf exceeded the 50ms strike line,
  `timer_ticks=0`, live CPU*=0, while /proc measured 18.38s and the terminal
  trace carried 21.30s. The fix caps each main-loop drain at 64 fully-processed
  records by using the documented negative-callback stop path; a yielded
  backlog re-enters epoll with timeout 0, so timer/signal/control fds run
  between batches without adding a 10ms sleep or dropping the consumed record.
  Shutdown's final correctness drain stays exhaustive. Deterministic positive
  and negative coverage lives in `phase_event_ring_drain_budget`: fixed run
  `timer_ticks=3`, CPU*=7059ms, `/proc=18.40s`, trace=21.32s; test-only
  `PGWT_TEST_NO_EVENT_DRAIN_BUDGET` restores the pre-fix direction on demand
  (`timer_ticks=0`, CPU*=0, `/proc=18.39s`, trace=21.30s).

- **Multi-window %DB: windowed-delta drift of measured-CPU vs wall.** The
  multi-window `--view` (Last 1s / Last 3s) differences two cumulative ring
  snapshots; CPU* carries MEASURED on-CPU ns while DB Time carries WALL. Per single
  snapshot the visible parents sum to ≤100% (Off-CPU* is the non-negative
  remainder), but across a window edge where CPU intervals **close**, the
  closed-interval accounting uses wall (`event_stream.c → evt->duration_ns`) while
  the open interval uses measured (`map_reader.c cpu_open`), so the two cumulative
  curves drift and the delta's visible-parent %DB sum can sit a few % over 100%
  (**observed 106–110% under pgbench**, varying with sched_switch timing; worst when
  Off-CPU* ≈ 0). **Cosmetic** — the authoritative conservation holds exactly
  (`test_data_offcpu_identity`, `test_daemon_server`, `test_multi_window` Test 3).
  Amplified (not caused) by the `map_reader` open-on-CPU fix (which correctly
  restored fork-caught backends' CPU magnitude). **Proper fix:** clamp CPU* to its
  wall share per window in the delta, or render Off-CPU* as an explicit parent so
  the column sums to 100% by construction. Related: the LIVE display accounts a
  CLOSED on-CPU segment at WALL, not measured (measured `cpu_ns` is folded only into
  lifetime counters) — reconcile both when picked up.

- **Live view: account the OPEN interval of a repeated WAIT.** `pgwt_read_state_map`
  (`src/map_reader.c`) still skips a backend's current OPEN interval when `d->accum`
  already holds a CLOSED segment for the same `(pid, wait_event)` — the
  `has_closed_data` guard (which was made **CPU-only** on 2026-07-19; see fixed
  bug #51). The analogous **WAIT** case is still guarded: a backend that already
  completed one LWLock/Lock/IO wait and is CURRENTLY in another of the SAME class
  has that in-progress wait suppressed in the live `--view` until it closes — real
  ongoing wait time that should show. Deferred because the guard is **load-bearing**
  for existing wait accounting (asserted by `test_deterministic`, whose `pg_sleep`
  keep-alive would otherwise count as a 6th in-progress PgSleep); removing it safely
  needs the recorded-trace path checked for the same double-count invariant.
  **Scope:** additive — make the open-wait read symmetric with the open-CPU read,
  then re-baseline `test_deterministic`'s keep-alive to an idle (ClientRead) hold.

- **On-CPU-spin vs off-CPU-blocked, *within* each wait class.** Sub-split every wait
  class into ON-CPU (spinning) vs OFF-CPU (genuinely blocked) time **while keeping
  the wait label**, e.g. `LWLock:LockManager 500 ms (420 ms on-CPU spin / 80 ms
  blocked)`, `IO:DataFileRead 300 ms (0 ms spin / 300 ms blocked)`. **Why it
  matters** (user's diagnostic point, 2026-07-19): the wait label is the actionable
  signal — "LWLock:LockManager" points straight at lock-manager contention in a way
  "high CPU" never could, so spin CPU must **stay under its wait label**, never
  relabeled as anonymous CPU* (the rejected Option B). But *within* the label the
  split tells you HOW: mostly **on-CPU spin** → a hot lightweight lock burning cores
  (fix the hot path / reduce acquisition rate); mostly **off-CPU blocked** → a
  genuinely serialized wait (heavyweight lock, IO — fix the blocking dependency).
  Same label, opposite remediation. **Feasible now:** S3 already measures on/off-CPU
  per context switch exactly and trace v3 records per-interval `cpu_ns`; a wait
  interval's `cpu_ns` IS its exact on-CPU spin (already summed into the
  `wait_gap_cpu` observability metric). The work is compute + presentation only:
  carry per-wait-class spin `cpu_ns` (a second column per wait row) into
  time_model / the AAS tooltip / the views — no new capture mechanism. Keep DB-Time
  conservation (spin is already inside the wait's wall time; the sub-split only
  *labels* it). Sampled tier can't produce it — show only where measured `cpu_ns`
  exists.

- **Off-CPU analysis (runqueue latency, voluntary vs involuntary).** S3's
  `sched_switch` program sees `prev_state` at every switch, so it can distinguish
  voluntary (blocked) from involuntary (preempted) switches and measure
  runqueue-wait latency per backend — a richer "scheduler pressure" view than the
  single Off-CPU* residual v0.13 ships (which lumps runqueue + genuinely-unaccounted
  together). Deferred; the data is **one field away** in the existing sched_switch
  handler.

- **Sampler feed from the exact CPU accumulator.** The sampled (default) tier still
  estimates CPU from sample counts. S3's `state_map.cpu_ns_total` is exact and
  always maintained (the sched_switch program runs whenever the full-tier BPF is
  loaded). A later phase could have the sampler read `cpu_ns_total` deltas per tick
  to tighten sampled CPU* toward exact, without changing the sampled tier's
  ~0-overhead character (one map lookup per backend per tick — already done for
  query_id).

- **Parallel-worker CPU roll-up to the leader's query.** T2 counts parallel workers'
  CPU under their **own** query_id in foreground/execution; rolling it up to the
  leader's query needs `leader_pid` in `backends.jsonl` (a documented gap in the T2
  PR). Small, additive.

---

## Part 3 — Foundation & early plans (v0.1–v0.9)

This part consolidates the three foundational planning documents that predate the
tiered-capture rework: **`docs/ROADMAP.md`**, **`docs/DEVELOPMENT_PLAN.md`**, and
**`docs/REVIEW_AND_PLAN.md`** (the last dated 2026-03-17; the development plan's
sprints ran 2026-03-18 → 2026-03-24). Together they defined the tool's first
agenda — the core BPF tracer, the trace-file format, the daemon, the (Rust, then
web) investigation client, a 14-sprint execution plan, and a code-review + 6-layer
testing plan. **Almost the entire agenda shipped inside the v0.1–v0.9 pre-history
band** (see `CHANGELOG.md` `[0.1] – [0.9]`; the git tag history carries v0.1…v0.9).
This chapter *replaces* those three source docs; every item is reproduced with an
honest status.

**Status badges used below:**
- **[DONE vX.Y]** — shipped; cites sprint / PR / commit / file.
- **[SUPERSEDED by rework/Trust]** — the item's *goal* was met, but by a different
  mechanism, or the mechanism it declared done was later re-worked. The rework
  (`REWORK_PLAN.md`, v0.10–v0.12) and the Trust Milestone (`TRUST_MILESTONE_PLAN.md`,
  v0.13) are the two later programs that superseded pieces of this foundation.
- **[PARKED]** — planned, deliberately not done, resume-later.
- **[LEFT/OPEN]** — planned, never built, still open.

**Three nuances that recur and must not be lost** (flagged inline where they bite):
1. The **Rust TUI client** (ROADMAP Phases E.0/E.1) was **dropped** in Sprint 8 and
   replaced by `pgwt-server --dump` + the `pgwt` web client.
2. **"Merge daemon + server"** (DEVELOPMENT_PLAN Sprint 11 / REVIEW Arch 4) was
   **deferred, then superseded** by the rework's **control-socket** model — the
   daemon kept a separate process but grew a Unix-socket control plane (A0/D4).
3. Three correctness fixes this foundation **declared done** were later found
   **re-violated** and re-hardened by the Trust Milestone:
   **state_map GC** (Sprint 3.3 / Arch 5) → **T4** (CAP-1 sizing);
   **JSON escaping** (Sprint 3.1 / Issue 9) → **T6** (UI-6 client tooltip);
   **summary honesty** (0B.5, deferred) → **T1** (FID-2 "exact" label over sampled data).

---

### ROADMAP.md

Development roadmap of completed + planned phases. "Each phase builds on the
previous." Core tracer phases were numbered **1–8**; later work was lettered **A–H**.

#### Phase 1–8: Core Tracer — [DONE v0.2]
CLI redesign, 6 diagnostic views, multi-window support, auto-discovery. Concretely:
- Hardware watchpoint on `my_wait_event_info` **per backend** (CPU debug registers).
- BPF **ring buffer** with raw event streaming.
- **Accumulator with 6 views**: `time_model`, `system_event`, `session_event`,
  `histogram`, `query_event`, `active`.
- **Multi-window** time comparisons (`--window 5s,1m,5m`).
- **Auto-detect** single PG instance.
- **TUI and text output** modes (auto-detect TTY).

Evidence: merged to master, **tag v0.2**. *Nuance:* the CLI redesign's fuller
`--format` matrix (`tui | text | json | csv`, `docs/CLI_REDESIGN.md`) only landed
`tui`/`text`; `json`/`csv` output formatters were never shipped — see **still-open
remnants**.

#### Phase A: BPF Ring Buffer Dual-Path — [DONE v0.1–v0.9]
Raw event streaming alongside aggregated maps; the foundation for trace recording.

#### Phase B: Accumulator + 6 Views — [DONE v0.1–v0.9]
In-memory accumulation of ring-buffer events into the 6 diagnostic views (as above).

#### Phase C: Event File Writer + Reader + Replay — [DONE v0.1–v0.9]
- **Columnar LZ4-compressed** trace files (4096 events/block, ~36× compression).
- **Block index footer** for time-range binary search.
- **Hourly rotation**, configurable **retention (default 24h)**.
- **Offline replay** mode (`--replay`).

Evidence: `src/event_writer.c`, `src/event_reader.c`, `src/replay.c`. *Later touched:*
trace format bumped to **v2 (typed blocks)** in rework A1; a durability/format spec
(`docs/TRACE_FORMAT.md`) + never-truncate recovery + collision-safe rotation were
added by Trust **T5** (DUR-1/2/3). `--replay` was made **fidelity-aware** by Trust
**T1** (FID-5) — a plain replay of a tiered trace previously showed near-zero activity.

#### Phase D: Daemon Mode — [DONE v0.1–v0.9]
- Persistent monitoring with `--daemon`.
- Automatic PostgreSQL **restart detection** (postmaster health check).
- **BPF destroy+reload on PG restart** (rodata is immutable after load).
- **PGDATA inference** from `/proc/<pid>/cwd`.

Evidence: `src/daemon.c`. *Later:* the daemon gained a **control socket + self-metrics**
in rework A0 (see Arch 4 supersession); postmaster-restart-exits-daemon became a
documented ops contract in Trust T4 (CAP-11).

#### Phase E.0: Investigation Client — Foundation — [DONE v0.1–v0.9, then SUPERSEDED by rework Sprint 8]
Originally a **Rust TUI** trace-file analysis client. Delivered:
- **Trace file reader** (Rust): `.trace.lz4` (header, footer, block index, LZ4
  decompression, columnar decode).
- **Streaming reader** for `current.trace` (no footer — scans blocks sequentially,
  tolerates truncated blocks).
- **4 table views**: Overview (time_model), Top Events, Top Sessions, Top Queries.
- **Wait event name resolution**: full PG18 name tables ported to Rust.
- **`--dump` mode**: non-interactive stdout summary.
- **`--generate-test`**: synthetic OLTP data generator (10 sessions, 6 roles, time
  phases, burst/idle).

**[SUPERSEDED]** The Rust TUI was **removed in Sprint 8**. Its functions were
re-homed to `pgwt-server --dump` (CLI text) and the `pgwt` web client.

#### Phase E.1: AAS Stacked Bar Chart — [DONE v0.1–v0.9, then SUPERSEDED by rework Sprint 8 / web UI]
The centerpiece visual (Average Active Sessions over time), in the Rust TUI:
- **AAS bucketing**: per-bucket, per-wait-class accumulation with overlap-aware
  duration splitting.
- **Half-block rendering** (`'▄'`) for 2× vertical resolution on any terminal.
- **Pixel-perfect rendering** via `ratatui-image` (iTerm2, Sixel, Kitty), auto-detected.
- **Transparent background** (RGBA blends with terminal theme).
- **Chart height** 20 rows (40 virtual in half-block).
- **11 wait-class color palette** (CPU=green, IO=blue, Lock=red, LwLock=pink, IPC=cyan…).
- **Decorations**: auto-scaled Y-axis, HH:MM:SS X-axis, inline legend.

**[SUPERSEDED]** Removed with the Rust client (Sprint 8). The AAS chart concept
carried forward into the web UI (Phase F.3, ECharts), later migrated to a single
ECharts implementation with a custom drag-select overlay in rework B3.

#### Phase F: Web Investigation Client — [DONE v0.1–v0.9]
**Goal:** an Oracle ASH / RDS Performance Insights–class interactive tool, running on
the DBA's laptop, connecting to the DB server over SSH.

**Architecture (preserved):**
```
[DBA laptop]                          [DB server]
pgwt (Go binary)                      pgwt-server (C binary)
  ├─ spawns: ssh user@host pgwt-server  ├─ reads trace files (event_reader.c)
  ├─ localhost HTTP server (net/http)   ├─ computes aggregates (compute.c,
  ├─ WebSocket bridge: browser ↔ SSH    │   map_reader.c, wait_event.c)
  ├─ static assets (//go:embed)         └─ JSON lines on stdin/stdout
  └─ auto-opens browser
```
*Why this architecture:* like git/rsync/mosh it spawns real `ssh` (inherits
`~/.ssh/config`, agent, ProxyJump, known_hosts — zero auth code); server side is pure
C reusing existing reader/compute code (no new DB-server deps); client is a single Go
static binary (stdlib HTTP/embed, trivial cross-compile); web frontend (ECharts + JS)
gives full interactive charts. *Usage:* `pgwt root@db-server` → browser opens
`http://localhost:8384`.

##### F.1: Server side — `pgwt-server` (C) — [DONE v0.1–v0.9]
Binary `src/server.c` (+ `src/compute.c`). Reuses `event_reader.c` (trace read, LZ4,
block index), `replay.c` (event iteration, time-range filter), `wait_event.c` (PG17/PG18
name tables), `map_reader.c` (accumulator).
**Protocol** = JSON lines over stdin/stdout, request/response matched by `"id"`. The
foundational command set and shapes (preserved):
- `info` → `{from_ns,to_ns,num_events,num_cpus}`
- `aas` (`from`,`to`,`buckets`,`filters`) → `{buckets:[{t,cpu,io,lock,…}],max_aas}`
- `top_events` → rows `{event_id,name,class,count,total_ms,avg_us,max_us,pct,aas}`, `db_time_ms`
- `top_sessions` → rows `{pid,db_time_ms,cpu_pct,top_wait}`
- `top_queries` → rows `{query_id,count,total_ms,pct,top_wait}`
- `time_model` → `{db_time_ms,idle_time_ms,aas,classes:[{name,ms,pct,aas}],wall_ms}`
- **Filters** (all optional, AND logic): `{class,event_id,pid,query_id}`.
- **Build:** compiled by the existing Makefile against the same object files, no new deps.

##### F.2: Client side — `pgwt` (Go) — [DONE v0.1–v0.9]
Directory `web/` (Go module): `main.go` (CLI args, spawn ssh, HTTP server, open
browser), `bridge.go` (WebSocket ↔ SSH stdin/stdout bridge), `static/` (embedded
`index.html`, `app.js`, `style.css`), `go.mod`/`go.sum`.
Deps minimal: `gorilla/websocket`; everything else stdlib (`net/http`, `os/exec`,
`embed`, `encoding/json`). **SSH spawning:** `exec.Command("ssh", host, "pgwt-server",
traceDir)` with stdin/stdout pipes. **WS bridge:** browser JSON → SSH stdin; SSH stdout
JSON → browser; matched by `"id"` for concurrency.
*Later hardened:* Trust **T6** fixed the bridge — per-connection id namespacing (UI-2),
SSH respawn + keepalives (UI-3), reject `.error` envelopes (UI-1), WS session token
(UI-12).

##### F.3: Web frontend (HTML + JS + ECharts) — [DONE v0.1–v0.9]
Layout (preserved): header w/ connection pill + time range → AAS stacked area chart →
tabs `[Overview][Events][Sessions][Queries]` → breadcrumbs → summary bar
(DB Time / Wall / AAS) → data table (click = drill).
- **Chart:** ECharts stacked area, 11 wait-class colors, tooltip with per-class AAS +
  %, interactive drag-to-zoom, responsive resize.
- **Tables:** click-row drill-down (adds filter, pivots view); breadcrumb trail with
  colored dots (`● IO > ● IO:DataFileRead > PID 1234`); color dots by class/event;
  percentage bars behind `%DB`/`CPU%`/`Wait%`; summary header (DB Time, Wall, AAS,
  Idle, CPUs); sortable columns.
- **Connectivity:** WS auto-reconnect exponential backoff (2s → 16s); status indicator
  (Connecting / Connected / Reconnecting).
*Later:* the whole `app.js` was restructured into pure-builder views + view-manager +
transport in rework **B3**; AAS moved off ApexCharts onto ECharts with a custom
selection overlay; ECharts vendored (no CDN).

##### F.4: Implementation steps — [DONE v0.1–v0.9] (all 7)
| Step | What | Status |
|---|---|---|
| 1 | `pgwt-server` — info + aas commands | DONE |
| 2 | `pgwt` Go skeleton — SSH + HTTP + WS bridge | DONE |
| 3 | AAS stacked area chart in browser | DONE |
| 4 | `pgwt-server` — all 6 commands | DONE |
| 5 | Data tables + drill-down + breadcrumbs | DONE |
| 6 | Color dots, percentage bars, summary header | DONE |
| 7 | Auto-reconnect, tooltip %, style polish | DONE |

#### Phase G: Daemon Enhancements

##### G.1: Query Text Capture — [DONE v0.1–v0.9]
**Goal:** show actual SQL text alongside `query_id` without pg_stat_statements lookups.
**Mechanism (preserved):** the daemon reads `PgBackendStatus.st_activity` from shared
memory via `process_vm_readv()` on first sighting of a new `query_id` (real parameter
values, not normalized). Daemon side (`src/query_text.c`): discover `st_activity`
offset (DWARF preferred, PG17/18 x86_64 known-offset fallback — same as `st_query_id`
discovery); discover `BackendStatusArray` base (ELF symbol resolution,
`src/discovery.c`); track seen `query_id`s (hash set); on each tick, for backends with
an unseen `query_id`, read `st_activity` (1024 B `PGBE_ACTIVITY_SIZE`, null-terminated)
and write `{query_id,text,ts}` to sidecar **`query_texts.jsonl`** (one line per unique
id, rotated with the same retention). Server side: `pgwt-server` loads
`query_texts.jsonl` and serves `text` in `top_queries`. Offset math:
`BackendStatusArray + backend_index*sizeof(PgBackendStatus) + st_activity_offset`,
`backend_index = MyBackendId - 1`.
Evidence: `src/query_text.c` (present, wired into `src/server.c`). *Later hardened:*
Trust **T5** made `query_texts.jsonl` **append-only with dedup-on-load** (DUR-4),
matched trace-file perms, logged the 4096-id cap, documented PII/redaction (DUR-10).

##### G.2: Plan Identifier Capture (PG18+) — [LEFT/OPEN]
**Goal:** capture the execution-plan hash per backend to detect plan regressions
correlated with wait spikes. **Background:** PG18 added `int64 st_plan_id` to
`PgBackendStatus` (right after `st_query_id`); core PG does **not** compute it (stays 0
unless a `planner_hook` extension populates it; no `compute_plan_id` GUC); on PG17
`st_plan_id` does not exist.
- **PG version matrix (preserved):** 14–16 → `my_wait_event_info` not writable / no
  plan_id; 17 → `st_query_id` `uint64`, no `st_plan_id`, full wait tracing works; 18+ →
  both `int64`, plan_id requires a planner_hook extension.
- **Daemon plan (unbuilt):** discover `st_plan_id` offset (DWARF; known = `st_query_id`
  offset + 8; skip on PG17); read alongside `st_query_id` (+8 bytes, no extra syscall);
  store a `plan_id` column in the columnar block (bump trace file v1→v2, reader handles
  both).
- **Server/client plan (unbuilt):** show `plan_id` in queries view when non-zero;
  highlight when one `query_id` shows multiple `plan_id`s over time; investigation flow
  = spike → drill to query → see plan_id changed at the spike → regression confirmed.
- **Requirement:** non-zero plan_id needs a `planner_hook` extension
  (**pg_stat_sql_plans**, **pg_store_plans**, or custom `result->planId = hash`);
  without one `st_plan_id` is always 0 and the feature is silently inactive (document
  clearly).

**Status:** **deferred at Sprint 14.4** (needs BPF changes + `st_plan_id` offset
discovery), and its **web UI (14.5)** depends on it. **Still open** — see remnants.

#### Phase H: Live Mode — [DONE v0.1–v0.9 (Sprint 12), mechanism SUPERSEDED]
**Goal (as written):** connect the web client to a running daemon for real-time
streaming — `pgwt-server` connects to the daemon via a Unix socket, pushes live AAS
over the SSH channel; the **active view works only in live mode** (needs BPF
state_map); dual historical (trace files) + live (daemon) in the same web UI.

**[DONE, different mechanism]** Live mode shipped in **Sprint 12** but **not** by the
socket-push design above: instead `pgwt-server` **streams `current.trace`** (the file
the daemon is actively writing) via a footer-less block reader, caches immutable
`.trace.lz4` per session, and re-reads only `current.trace` on a 5-second auto-refresh
→ 1–5 s latency, zero persistent server process, historical + live merged. (Sprint 12
explicitly avoided merging daemon+server; see Sprint 11 / Arch 4.)
*Later:* the actual **daemon control socket** arrived in rework A0 (status/metrics/
escalate — not AAS push); the "Live = last N min means NOW" contract was fixed by
Trust **T6** (UI-11 UTC labels; UI-1 dead-transport visibility) and the summary path
that silently broke the default "last 15 min" tiered query was fixed by Trust **T1**
(FID-2).

#### Legacy: Rust TUI Client (Removed) — [SUPERSEDED by Sprint 8]
The `client/` Rust TUI (Phases E.0–E.1) was removed in Sprint 8; covered by
`pgwt-server --dump` (CLI text summary) and `pgwt` (web client with drill-down + AAS).
*(A stale `client/target` build-artifact dir lingers untracked in the working tree;
the source is gone.)*

#### Architecture Notes (preserved from ROADMAP)
- **Hardware watchpoint approach:** CPU debug registers set a hardware watchpoint on
  `my_wait_event_info`; PG17+ writes wait events through `*my_wait_event_info`, caught
  by the watchpoint → exact, non-sampled transitions at ns precision, **"~5% TPS
  overhead"**. *Correction:* the rework's measurements put the **full watchpoint tier
  at 29–30% overhead** (the documented range) and the **sampled tier at ≈0%** — the
  "~5%" figure is superseded; per-backend watchpoints were reworked toward the
  per-task/tiered model.
- **BPF constraints:** rodata immutable after skeleton load → destroy+reload on PG
  restart (daemon); hardware watchpoints limited to **~4 per CPU** (kill stale
  processes between runs); `pgwt_daemon` struct ~**27 MB** (heap-allocated).
- **Trace file design:** columnar blocks (4096 events), LZ4 (~36×), block-index footer,
  footer-less `current.trace` streaming, hourly rotation to `YYYY-MM-DD_HH.trace.lz4`,
  retention default 24h.
- **Client–server protocol:** SSH exec channel (stdin/stdout of `pgwt-server`),
  newline-delimited JSON, `id`-matched request/response — "same model as git over SSH,
  rsync --server, ParaView pvserver."
- **UX reference sources:** Oracle EM Performance Hub / ASH Analytics (stacked area,
  two-tier time slider, click-to-filter, pivot dropdown, filter tags, CPU-cores line,
  load-map treemap); AWS RDS Performance Insights ("slice by", mini wait bars per SQL,
  Max vCPU line, top-25 per dimension); PASH-Viewer (range select, Top SQL drill, plan
  display); pg_ash (ASCII stacked bars, Unicode blocks, "Other" rollup, per-query wait
  profile, semantic colors).

---

### DEVELOPMENT_PLAN.md (14 sprints)

A sequenced, dependency-explicit execution plan referencing `REVIEW_AND_PLAN.md`
(bugs/arch/tests), `TRACING_ANALYSIS_PLAN.md` (analysis phases 2–5), and `ROADMAP.md`
(G.2, H). **It is largely a self-marked-complete plan** — Sprints 1–10 and 12–14 carry
✅, Sprint 11 was deferred, and a tail of UI/plan-id sub-items (13.2, 13.4, 14.2, 14.4,
14.5, 14.6) was explicitly deferred. All ✅ sprints are corroborated by commits in the
git log.

#### Sprint 1: Bug Fixes + Ship Uncommitted Work — [DONE v0.1–v0.9] (2026-03-18)
All 6 bugs + 2 features + 3 test fixes. Items: **1.1** timeline bar displacement
(`s = ts_ns − dur_ns`, `server.c`, Bug 1); **1.2** histogram bucket mismatch → hardcoded
thresholds (`summary_writer.c`, `compute.c`, Bug 2); **1.3** fd leak on re-init
(`backend.c`, Bug 3); **1.4** unfiltered `class_ns` in query visitor (`compute.c`,
Bug 4); **1.5** `next_pow2` overflow clamp at `1<<30` (`server.c`, Bug 5); **1.6**
heatmap `grid_size` → `size_t` + clamp 100K (`compute.c`, Bug 6); **1.7** double refresh
in drill/drill-up/clear (`app.js`, Issue 13); **1.8** `[local]` socket cmdline support;
**1.9** session-timeline event coalescing; **1.10** add `bg_worker` known backend type
(PG18) to tests. Result: 16/16 suites (354 checks), Rocky 9 / PG18.3.
**Commits:** `eacb880`, `cfde454`, `d1255c4`, `549417d`.

#### Sprint 2: Test Infrastructure + Synthetic Data Correctness — [DONE v0.1–v0.9] (2026-03-18)
No root / no PG. **2.1** `gen_test_traces.c` (JSON scenario → `.trace`+`.summary`+
`backends.jsonl`+`query_texts.jsonl`, `mono_to_wall=0` for deterministic ts); **2.2**
`server_harness.py` (`ServerHarness`/`TestRunner`, PG18 event-ID constants); **2.3**
`test_data_time_model.py` (0B.1, 9/9); **2.4** `test_data_aas.py` (0B.2, 7/7); **2.5**
`test_data_events.py` (0B.3, 14/14); **2.6** `test_data_sessions.py` (4/4); **2.7**
`test_data_queries.py` (Bug 4 regression, 6/6); **2.8** `test_data_filters.py` (0B.4,
9/9); **2.9** `test_data_timeline.py` (0B.6, Bug 1 regression, 10/10); **2.10**
`test_data_idle.py` (0B.7, 3/3); **2.11** `test_data_edge.py` (0B.8, 10/10); **2.12**
`test_bucket.c` (exhaustive `pgwt_duration_to_bucket`, 25/25); **2.13** into
`tests/Makefile` + `run_all.sh`.
**Bug 7 found & fixed:** `compute_aas` used `ev_start = timestamp_ns` instead of
`timestamp_ns − duration_ns` (AAS shifted forward by event duration) — both class-bucket
and event-detail paths. **Deferred:** `test_data_summary.py` (**0B.5**, summary-vs-raw)
pushed to Sprint 3 (needs summary-path fixes for synthetic timestamps). 72 checks, 0%
tolerance; 26/26 suites.

#### Sprint 3: Code Hardening — [DONE v0.1–v0.9] (2026-03-18)
Fragility fixes (not bugs-today): **3.1** JSON escaping for all server output
(`json_escape_stdout` in `server.c`, `json_escape_fp` in `backend_meta.c`, Issue 9);
**3.2** `/proc/<pid>/stat` parsing → `fgets`+`strrchr(')')`+`sscanf` (`backend.c`,
`discovery.c`, Issue 7); **3.3** state_map GC — periodic 60-tick sweep with `kill(pid,0)`
+ `pgwt_handle_exit()` (`daemon.c`, Arch 5); **3.4** move large stack arrays to `malloc`
(`sorted[4096]` 40KB, `entries[MAX_BACKENDS]` 96KB in `output.c`, + a missing-braces fix
in `dispatch()` error path, Issue 12); **3.5** WebSocket **origin check** localhost-only
(`bridge.go`, Issue 10; empty origin/curl still allowed).
**⚠ Two of these were re-violated later:** **3.1 JSON escaping** was replaced server-side
by cJSON (Sprint 6) but re-surfaced **client-side** — Trust **T6 (UI-6)** found query
text concatenated **unescaped** into tooltip HTML (`builders/timeline.js`). **3.3
state_map GC** addressed *leaked* entries, but Trust **T4 (CAP-1)** found the map was
**undersized** (`max_entries=512` < `MAX_BACKENDS` 1024 < `max_connections`) → silent
under-count above 512 backends, re-hardened (checked inserts + `state_map_full`
counter). **3.5 origin check** was augmented by a **WS session token** in T6 (UI-12).

#### Sprint 4: Live Data Correctness Tests — [DONE v0.1–v0.9] (2026-03-18)
Root + PG. Full BPF→file→compute pipeline vs real workloads. **4.1** `test_percentage.py`
(0A.1, 50/50 sleep+CPU); **4.2** `test_aas_accuracy.py` (0A.2, 4 sleepers → AAS≈4);
**4.3** `test_session_accuracy.py` (0A.3); **4.4** `test_query_accuracy.py` (0A.4,
query_id can be negative); **4.5** `test_partition.py` (0A.5, CPU+…= DB Time, error
0.0000%); **4.6** `test_idle_exclusion.py` (0A.6 — **redesigned**: only `Activity:*` is
idle, ClientRead/PgSleep are not); **4.7** `test_daemon_server.py` (0A.10, daemon CLI ≈
pgwt-server); **4.8** into `run_all.sh`. 44 checks pass; 32/33 suites (the one failure
= `test_overhead` 19% vs 15% gate, a pre-existing flaky test).

#### Sprint 5: Web UI Tests (Playwright) — [DONE v0.1–v0.9] (2026-03-19)
**5.1** `mock_server.py` (HTTP on P + WebSocket on P+1, canned JSON for all 8 commands,
no SSH/no root); **5.2** `test_web_ui.py` (18 tests / 67 checks: load, tabs, summary bar,
tables, sort, drill-down, breadcrumbs, histogram, timeline, time picker, zoom, chart,
reconnection); **5.3** exact data-display values (DB Time 12.5s, Wall 3600s, AAS 3.47,
Idle 45s, CPUs 4; cell values; timeline positions w/ duration); **5.4** regressions
(Bug 1 bars start at `s`; Bug 13 drill = exactly 1 aas + 1 table; filter persistence
across tabs). Monkey-patches the WS constructor via `add_init_script` (no app.js change).
25 tests / 98 checks. *Later:* rework **B1/B2/B4** added the console-error guard, chaos
mode, and visual-regression snapshots.

#### Sprint 6: Integrate cJSON — [DONE v0.1–v0.9] (2026-03-19)
Replaces hand-rolled JSON parse/generate (Issue 8 substring parser + Issue 9 escaping).
**6.1** vendor `cJSON.c`/`.h` (v1.7.18, MIT, 3443 lines); **6.2** request parsing via
`cJSON_Parse`+`cJSON_GetObjectItem`; **6.3** `parse_filters` via cJSON; **6.4** all 8
handlers + dispatch + errors via cJSON tree + `cJSON_PrintUnformatted`; **6.5**
`backend_meta.c` output via cJSON; **6.6** Makefile (both `USER_SRCS`/`SERVER_SRCS`,
`-lm`); **6.7** all Sprint 2+5 tests (33/33). Removed `json_escape_*`, `skip_ws`,
`json_string/int64/uint64/int`, hand-rolled parsers; added `cjson_add_uint64`
(raw number to avoid double precision loss on ns), `cjson_create_uint64`, `emit_json`;
`.jsonl` loaders migrated; heatmap labels → UTF-8. `server.c` −101 lines net.

#### Sprint 7: Dynamic Event Name Resolution — [DONE v0.1–v0.9] (2026-03-24)
Stop hardcoding PG-version wait-event tables (Arch 1). **7.1** at startup query
`pg_wait_events` via psql (PG17+); **7.2** store sidecar `wait_event_names.json`
(`{"IO":[…],"Lock":[…]}` in enum order); **7.3** `pgwt-server` reads sidecar, falls back
to hardcoded; **7.4** web client already displays server names; **7.5** hardcoded tables
kept as fallback for old traces (`wait_event.c` dynamic→hardcoded); **7.6** detect psql
user from postmaster owner (`/proc/PID/status` UID → `getpwuid()`). Heap `dyn_names[16]
[512]` keyed by (class_byte,event_id); port from `postmaster.pid` line 4; sidecar ~5.5KB
for PG18; multi-instance safe. 136 C unit tests. **Commits:** `6fd90f9`, `0ffd565`.

#### Sprint 8: Drop Rust TUI Client — [DONE v0.1–v0.9] (2026-03-24)
(Arch 2.) **8.1** add `--dump` to `pgwt-server` (time model + top events/sessions/
queries text summary); **8.2** delete `client/` (Rust TUI, **−4782 lines**); **8.3**
update README/INSTALL/ROADMAP (drop pgwt-cli refs). Net **+133 / −4782**. Single C
codebase for all analysis. **Commit:** `008f624`.

#### Sprint 9: Performance Optimization + Benchmarks — [DONE v0.1–v0.9] (2026-03-24)
Replace O(n²) hot paths with hash tables + baselines (Issue 11, Layers 4/5). **9.1**
`event_ht_entry` (2048 slots) for time-model accum; **9.2** `wait_ht_entry` (256 slots)
for session/query per-wait; **9.3** `pid_idx_ht` (1024) for timeline PID index; **9.4**
`gen_bench_traces.c` (1M/10M realistic events); **9.5** `bench_server.py` (per-command
latency); **9.6** baselines (1M: 101ms=10M/s; 10M: 924ms=10.8M/s); **9.7** ASan/Valgrind
Makefile targets (`make test-asan`, `test-valgrind`, `bench`). Helpers `hash32/64`,
`*_find_or_insert`. Summary path <1ms regardless of event count. **Commits:** `711f590`,
`7fd8f62`.

#### Sprint 10: Tracing Analysis Phase 2 — Transitions + Fingerprinting — [DONE v0.1–v0.9] (2026-03-24)
(`TRACING_ANALYSIS_PLAN.md` Phase 2.) **10.1** server `transitions` endpoint (4096-slot
(from,to) hash, Sankey links JSON); **10.2** web Sankey tab (ECharts, hover count/%/dur);
**10.3** `pgwt_compute_fingerprints()` (per-query class distribution + top transition;
signature `"IO:30%|CPU:22%|LWLock:21%"`); **10.4** `fingerprints` endpoint; **10.5**
10/10 transition tests. Hash `(from*0x45d9f3b)^(to*0x9e3779b9)`; filters idle + exit
sentinels; self-transitions handled with space suffix. **+517 lines. Commit:** `efe531e`.
Endpoints confirmed present (`src/server.c` dispatches `transitions`, `fingerprints`).

#### Sprint 11: Merge Daemon + Server — [SUPERSEDED by rework control-socket model]
**Deferred** in the plan: "the per-session SSH-spawned model (pgwt → ssh → pgwt-server)
works well without merging; security, ops simplicity, resilience favor the current
architecture; revisit if shared cache across users becomes a need." This is **REVIEW
Arch 4** — the plan's own "single biggest simplification." **[SUPERSEDED]** The rework
instead gave the daemon a **control socket + self-metrics** (A0 / design D4:
`{trace_dir}/pgwt.sock`, mode 0600, JSON-line `status`/`escalate`/`deescalate`/`metrics`;
`pgwt-server` proxies as a `control` command), keeping daemon and server as **separate
processes** — and delivered live mode via streaming `current.trace` (Sprint 12), not by
merging. Arch 4's *goals* (live streaming, caching, active-backend visibility) were met
without the merge.

#### Sprint 12: Live Mode (Phase H) — [DONE v0.1–v0.9] (2026-03-24)
Near-real-time AAS + drill-down from the daemon's live trace, **without** merging
daemon+server. Architecture: `pgwt-server` reads `current.trace` (footer-less streaming
block reader), caches immutable `.trace.lz4` per session, re-reads only `current.trace`
on refresh → 1–5s latency, zero persistent server. **12.1** streaming reader (stop at
EOF/incomplete header, detect new blocks); **12.2** include `current.trace` in file list
(rescan per request); **12.3** per-session cache for immutable files keyed by
`(filename,time_range)` (**this realizes REVIEW Arch 6**); **12.4** web auto-refresh
toggle (5s polling, appends AAS buckets); **12.5** "Live" quick range = last 5 min,
`viewTo`="now" each refresh; **12.6** partial-`current.trace` reading test. *Later:* the
"last N min = NOW" contract and the summary path that broke it were re-hardened by Trust
T6/T1.

#### Sprint 13: Tracing Analysis Phase 3 — Concurrency — [DONE v0.1–v0.9 backend; UI deferred] (2026-03-24)
(`TRACING_ANALYSIS_PLAN.md` Phase 3.) **13.1** peak concurrency per AAS bucket (sorted
active intervals, max overlapping sessions per event/bucket) — **DONE**; **13.2** web
peak-concurrency markers — **⏸ [PARKED]** (data via `concurrency` endpoint, UI polish
pass); **13.3** burst detection (sliding window 10ms default, N+ sessions entering same
wait simultaneously, returns PIDs) — **DONE**; **13.4** web burst annotations — **⏸
[PARKED]** (data via `concurrency` endpoint). Params `burst_window_ns` (10ms),
`burst_threshold` (4). Endpoint `concurrency` returns `peaks[]`+`bursts[]`. **Commit:**
`2e6483f`. Endpoint confirmed present in `src/server.c`.

#### Sprint 14: Advanced Analysis (Phases 4–5) + Phase G.2 — [DONE v0.1–v0.9 backend; UI + plan_id deferred] (2026-03-24)
(`TRACING_ANALYSIS_PLAN.md` Phases 4–5 + ROADMAP G.2.) **14.1** lock chain detection
(scan Lock waits, find likely blocker PID via CPU-interval overlap; heuristic, no
pg_locks) — **DONE**; **14.2** web lock-chain viz — **⏸ [PARKED]** (data via
`lock_chains`); **14.3** cross-session interference scoring (overlapping same-event
intervals across PIDs, 4096-slot (pid_a,pid_b) hash, normalized 0–1) — **DONE**; **14.4**
plan-id capture (PG18+) — **⏸ [LEFT/OPEN]** (needs BPF changes + `st_plan_id` offset
discovery = ROADMAP G.2); **14.5** plan-change detection UI — **⏸ [LEFT/OPEN]** (depends
on 14.4); **14.6** HMM anomaly detection — **⏸ [PARKED, research]** (see note). Endpoints
`lock_chains` (waiter/blocker/lock/wait_ms), `interference` (pid_a/pid_b/score/overlap_ms)
— both confirmed present in `src/server.c`. **Commit:** `5af0d0b`.
*Note on 14.6:* a **different** anomaly engine shipped later — the rework **A5**
AAS-vs-baseline + lock-fraction rules engine (`src/anomaly.c`), not HMM. HMM proper
remains parked research.

#### Summary timeline & dependency graph (preserved)
```
Sprint 1  Bug fixes + uncommitted        ✅   Sprint 8  Drop Rust TUI              ✅
Sprint 2  Test infra + synthetic         ✅   Sprint 9  Perf + benchmarks          ✅
Sprint 3  Code hardening                 ✅   Sprint 10 Transitions (Phase 2)      ✅
Sprint 4  Live data correctness          ✅   Sprint 11 Merge daemon+server        ⏸ DEFERRED
Sprint 5  Web UI tests (Playwright)      ✅   Sprint 12 Live mode (Phase H)        ✅
Sprint 6  cJSON integration              ✅   Sprint 13 Concurrency (Phase 3)      ✅
Sprint 7  Dynamic event names            ✅   Sprint 14 Advanced (Phases 4-5)+G.2  ✅
```
Dependencies: 1 → {2 → 3 → {4,5,9→11, 6→{7→8},11}}, 1 → {10 → 13 → 14}, 11 → 12.

---

### REVIEW_AND_PLAN.md (review + architecture + testing)

Comprehensive codebase review, dated **2026-03-17**: Part I bugs + fragility, Part II
architecture (keep / change / never-change), Part III a 6-layer testing plan, Part IV
implementation priority. **Thesis:** "The #1 job of this tool is to tell the truth" —
data correctness is paramount. *(The Trust Milestone later found that thesis had to be
re-enforced against re-introduced honesty violations.)*

#### Part I — Real bugs (all fixed in Sprint 1)
- **Bug 1 — Timeline bars shifted forward by one duration** — **[DONE v0.1–v0.9,
  Sprint 1.1]**. BPF emits `timestamp_ns=now`, `duration_ns=now−last_ts` (event ran
  `[ts−dur, ts]`), but server sent `s=ts` and app.js drew `[s, s+d]`. Fix: send
  `s = ev->timestamp_ns − ev->duration_ns` (`server.c`, `app.js:1349`). *Priority #1.*
- **Bug 2 — Histogram bucket mismatch (server vs daemon builds)** — **[DONE, Sprint 1.2]**.
  `summary_writer.c:12-24` loop version off-by-one vs `map_reader.c:131-150` hardcoded
  (1µs→bucket 0 vs 1, etc.). Fix: shared hardcoded thresholds. *Priority #2.*
- **Bug 3 — `pgwt_handle_init` leaks watchpoint fd on re-init** — **[DONE, Sprint 1.3]**.
  `backend.c:239` overwrote `wp_fd` without closing (leak one perf_event fd per queued
  INIT after scan). Fix: `pgwt_close_watchpoint()` before line 239. *Priority #3.*
- **Bug 4 — `tq_summary_visitor` accumulates unfiltered `class_ns`** — **[DONE,
  Sprint 1.4]**. `compute.c:~1501`: `effective_ns` filtered but `class_ns[c]`
  unconditional → correct total, wrong per-class breakdown under filter.
- **Bug 5 — `next_pow2` infinite loop on large input** — **[DONE, Sprint 1.5]**.
  `server.c:35-41`: `n>2^30` overflows signed `int p` → loop never terminates (huge
  `query_texts.jsonl`). Fix: unsigned / clamp.
- **Bug 6 — Integer overflow in heatmap `grid_size`** — **[DONE, Sprint 1.6]**.
  `compute.c:~820`: `int grid_size = actual_buckets*HISTOGRAM_BUCKETS` overflows →
  tiny `calloc` + OOB writes. Fix: `size_t` / clamp.

#### Part I — Suboptimal / fragile (Issues 7–13)
- **Issue 7 — `/proc/<pid>/stat` parsing with `%s`** — **[DONE, Sprint 3.2]**.
  `fscanf("%d %255s %c…")` breaks on comm with spaces/parens (`backend.c:100`,
  `discovery.c:459`). Fix: last-`)` + parse after.
- **Issue 8 — JSON key substring matching** — **[DONE, Sprint 6 (cJSON)]**.
  `json_int64/json_string` used `strstr(json,"\"key\"")` (`server.c:101-135`) — safe
  only while no key is a suffix of another. *Nuance:* the analogous substring scan on
  the **Go bridge** (`extractID`) was flagged again by Trust **T6 (UI-8)** as an
  unstated "id-first" protocol invariant — parse-or-document.
- **Issue 9 — No JSON escaping in server output** — **[DONE, Sprint 3.1 stopgap →
  Sprint 6 cJSON]**. Unescaped usename/datname/event/cmd could corrupt the JSON stream.
  **⚠ Re-violated client-side** → Trust **T6 (UI-6)** (unescaped SQL in tooltip HTML).
- **Issue 10 — WebSocket accepts any origin** — **[DONE, Sprint 3.5 → augmented by
  Trust T6]**. `CheckOrigin: return true` (`bridge.go:19`) = real CSRF (any site could
  read trace data). Fixed to localhost-only; T6 (UI-12) added a **session token**
  because localhost-origin alone is weak auth for a socket that can escalate against
  production.
- **Issue 11 — O(n²) linear scans in hot paths** — **[DONE, Sprint 9]**. time_model
  accum (`compute.c:~334`), session/query per-wait (`~599,~715`), timeline PID lookup
  (`server.c`) → hash tables.
- **Issue 12 — Large stack allocations in `output.c`** — **[DONE, Sprint 3.4]**.
  `deltas[3]`≈312KB, `sorted[4096]`≈640KB on stack → `malloc`.
- **Issue 13 — Double `refresh()` in drill-down** — **[DONE, Sprint 1.7; mechanism
  SUPERSEDED by rework B3]**. `drillDown`→`switchTab`(refresh)+`refresh()` = 2 requests
  (`app.js:~1008`). Fixed then; the whole refresh/generation-counter model was replaced
  by the **view-manager + single-flight transport** in rework B3.

#### Part II — What's right (keep exactly as-is) — [all preserved / kept]
Hardware watchpoints on `wait_event_info` ("the killer insight" — no sampling, no PG
patches, ns precision; uprobes fragile, ptrace slow, sampling loses short events); SSH
exec transport (zero auth/TLS/firewall, git-over-SSH model); compute-on-read with tiered
storage (raw 24h + summaries 1yr); columnar LZ4 files (Parquet/Arrow too heavy); Go for
the web client; C for the daemon (libbpf + overhead). *All retained through rework +
Trust (the rework's "Non-goals" reaffirm: no userspace rewrite, server keeps computing).*

#### Part II — What I'd change (Arch 1–6)
- **Arch 1 — Dynamic event name resolution** — **[DONE, Sprint 7]**. Replace hardcoded
  PG17/18 tables (the LWLock tranche table was PG18-only → wrong on PG17) with startup
  read of `pg_wait_events`, sidecar name map, forward-compatible with PG19+.
- **Arch 2 — Drop the Rust TUI client** — **[DONE, Sprint 8]**. Three trace-reader
  implementations (C/Rust/JS) → drop Rust, add `pgwt-server --dump`.
- **Arch 3 — Use a real JSON library in C** — **[DONE, Sprint 6]**. cJSON replaces
  `printf`/`strstr` JSON (the direct cause of Issues 8/9).
- **Arch 4 — Merge daemon and server into one process** — **[SUPERSEDED by rework
  control-socket model]**. The review's "single change that would most simplify the
  architecture" (daemon accepts Unix-socket client connections; live mode "comes for
  free"; consequences of *not* merging: every session re-scans, no aggregate caching,
  no live streaming, no active-backend view). **Deferred at Sprint 11, then superseded**:
  the rework kept processes separate and instead added the **control socket** (A0/D4:
  status/escalate/metrics, `pgwt-server` proxy) + streaming `current.trace` live mode
  (Sprint 12) + per-session immutable-file cache (Sprint 12.3). The *ends* were met by
  a less invasive means.
- **Arch 5 — Garbage-collect BPF state_map** — **[DONE, Sprint 3.3; re-hardened by
  Trust T4]**. Fixed 4096-entry map leaks if an EXIT lifecycle event is lost (ringbuf
  overflow) → periodic 60s sweep vs `/proc/<pid>/status`. **⚠ Trust T4 (CAP-1)** later
  found the deeper problem was **sizing** (`max_entries=512` < `MAX_BACKENDS` 1024) →
  silent under-count > 512 backends; re-hardened (checked inserts + `state_map_full`).
- **Arch 6 — Cache computed results in the server** — **[DONE, Sprint 12.3]**. Cache
  last-N results keyed by (command,from,to,filters); adjacent queries share work.
  Realized as the **per-session immutable-file cache** keyed by `(filename,time_range)`
  in Sprint 12 (only `current.trace` re-read on refresh).

#### Part II — What I explicitly would NOT change — [all kept]
BPF-watchpoint capture; columnar-LZ4 format; SSH-exec transport; Go+ECharts client;
compute-on-read; tiered storage (raw + summaries).

**Architecture summary (preserved):** "fundamentally sound; core decisions correct;
the changes are about **consolidation** — fewer moving parts, fewer duplicated
implementations, dynamic not hardcoded data; the biggest single win would be merging
daemon+server." *(That single "biggest win" is the one recommendation not taken as
written — see Arch 4.)*

#### Part III — Testing plan (6 layers)

**Current test state (preserved, ~6,200 lines):** 4 C unit tests (varint, columnar
encoding, file format, wait-event names), 3 shell integration (CLI validation,
lifecycle, overhead), 11 Python functional (accuracy, cross-validation, deterministic
counts). *Covered then:* pg_sleep duration (±30%), deterministic event count, DB Time
sanity, IO vs pg_stat_io (0.1–3×), backend coverage vs pg_stat_activity, names vs
pg_wait_events. *Not covered then (the gap this plan closed):* are percentages / per-
session / per-query numbers correct? server == CLI? summary == raw? AAS correct?
timeline timestamps correct? filtering correct? numbers hold over hours?

- **Layer 0A — Live data correctness (root + PG)** — **[largely DONE, Sprint 4; two
  sub-tests LEFT]**. 0A.1 percentage split, 0A.2 AAS≈4, 0A.3 per-session, 0A.4
  per-query, 0A.5 partition (<0.1%), 0A.6 idle exclusion, 0A.10 daemon→server agreement
  → **DONE** (Sprint 4.1–4.7). **0A.7** IO% vs pg_stat_io — pre-existing cross-check
  test (later **tightened by Trust TST-6/13**; the loose ~0.80 ratio was called
  "vacuous"). **0A.8** timed lock contention (LOCK TABLE + blocked SELECT) — **LEFT** as
  a dedicated live test (later exercised by Trust **T0** capture-smoke's blocked
  `LOCK TABLE`). **0A.9** live duration percentiles (P50/P95/P99 invariants) — **LEFT**
  as a live test (percentile math covered synthetically by 0B.3).
- **Layer 0B — Synthetic data correctness (no root)** — **[DONE, Sprint 2; 0B.5
  deferred→superseded]**. 0B.1 time-model arithmetic, 0B.2 AAS buckets, 0B.3 event
  stats+percentiles, 0B.4 filtering, 0B.6 timeline (Bug 1 regression), 0B.7 idle
  exclusion, 0B.8 edge cases → **DONE**. **0B.5 summary-vs-raw agreement** (+ Bug 2
  histogram regression) — **deferred at Sprint 2, then SUPERSEDED by Trust T1**: summary
  honesty was found broken (**FID-2**: `should_use_summaries()` stamped
  `"fidelity":"exact"` over sampled data and silently dropped windows ≥120s in default
  tiered mode) and re-hardened with `tests/test_data_summary_honesty.py`.
- **Layer 1 — Server protocol tests** — **[largely DONE via server_harness.py +
  rework B1]**. Black-box spawn/JSON-in/JSON-out for all commands (info, aas,
  top_events, top_sessions, top_queries, session_timeline, time_model, heatmap);
  protocol correctness (well-formed JSON, all fields, error handling); plus: unknown
  command → error w/ id; missing fields → error; concurrent ids matched; large
  responses not truncated; all-filters-set. Realized through `server_harness.py`
  (Sprint 2.2) and the rework's `tests/test_protocol_drift.py` (B1, real server vs mock
  schema diff).
- **Layer 2 — Web UI tests (Playwright)** — **[DONE, Sprint 5; expanded by rework
  B1/B2/B4]**. Functional (load, tabs, drill-down flow, breadcrumbs, clear filters,
  chart zoom, column sort); data-display correctness (summary/table cells match canned
  responses; timeline positions = Bug 1 regression; % bar widths); regression (Bug 13
  single refresh; filter persistence; reconnection). Mock server serves `web/static/` +
  canned WS. Rework added console-error guard, chaos mode, visual snapshots.
- **Layer 3 — Compute unit tests (C)** — **[partial: `test_bucket.c` DONE, broader
  `test_compute.c` LEFT]**. `pgwt_duration_to_bucket` exhaustive 0–20000µs (**Bug 2
  regression**, both implementations identical) → `test_bucket.c` **DONE** (Sprint 2.12).
  The planned `test_compute.c` (direct in-memory calls to `compute_aas`,
  `compute_top_events/sessions/queries`, filter matching, summary serialize→deserialize
  roundtrip) was **not built as a standalone C harness** — that surface is covered
  functionally through the server by the Layer 0B Python tests.
- **Layer 4 — Performance benchmarks** — **[DONE, Sprint 9]**. Compute throughput
  1M/10M events × commands; server latency p50/p95/p99; file read throughput; memory
  stability (1000 sequential requests → RSS flat). `gen_bench_traces.c` + `bench_server.py`.
- **Layer 5 — Memory safety** — **[DONE, Sprint 9.7]**. AddressSanitizer build target;
  unit + protocol tests under ASan; Valgrind leak detection on the server. (`make
  test-asan` / `test-valgrind`; a `pgwt-server-asan` build artifact exists at repo root.)

#### Part IV — Implementation priority (preserved)
- **Bug-fix order + effort:** Bug 1 (users see wrong timestamps, 5 min) → Bug 2 (wrong
  histogram on summary path, 5 min) → Bug 3 (fd exhaustion on long daemons, 5 min) →
  Bug 4 (wrong class breakdown w/ filters, 15 min) → Bug 5 (hang on huge jsonl, 5 min)
  → Bug 6 (memory corruption on malicious input, 5 min). *(All done in Sprint 1.)*
- **Architecture change effort:** Arch 1 Medium, Arch 2 Low, Arch 3 Medium, Arch 4
  High, Arch 5 Low, Arch 6 Medium.
- **Testing value ranking:** (1) 0B synthetic — **Highest** "proves math is right",
  no root; (2) 0A live — **Highest** "proves end-to-end", root+PG; (3) Layer 2 Web UI —
  **High** "eliminates manual clicking"; (4) Layer 3 compute C — Medium; (5) Layer 1
  protocol — Medium; (6) Layer 4 perf — Medium; (7) Layer 5 memory — Medium.
- **Test file structure (preserved):** keep all existing (`test_wait_event.c`,
  `test_cmdline.c`, `test_event_writer.c`, `test_event_reader.c`, `test_accuracy.py`,
  `test_deterministic.py`, `test_cross_validate.py`, `run_all.sh`); **new 0B**
  `gen_test_traces.c` + `test_data_{time_model,aas,events,sessions,queries,filters,
  timeline,summary,idle,edge}.py`; **new 0A** `test_{percentage,aas_accuracy,
  session_accuracy,query_accuracy,partition,idle_exclusion,daemon_server}.py`; **new
  Layer 2** `mock_server.py` + `test_web_ui.py`; **new Layer 3** `test_compute.c` +
  `test_bucket.c`; **new Layer 4** `gen_bench_traces.c` + `bench_server.py`.

---

### Foundation: still-open remnants

Everything from these three docs that is **not** fully done, gathered in one place:

1. **G.2 / Plan-identifier capture (PG18+)** — **[LEFT/OPEN]**. ROADMAP Phase G.2 +
   DEVELOPMENT_PLAN **14.4** (daemon: `st_plan_id` offset discovery + `plan_id` column +
   BPF changes) and **14.5** (plan-change detection UI). Requires a `planner_hook`
   extension for non-zero plan_id. Never built.

2. **Deferred analysis-view UIs** — **[PARKED → now REWORK B6]**. Backend endpoints
   shipped (Sprints 10/13/14: `transitions`, `fingerprints`, `concurrency`,
   `lock_chains`, `interference`), but their web visualizations were deferred: **13.2**
   peak-concurrency markers, **13.4** burst annotations, **14.2** lock-chain viz,
   **14.5** plan-change viz. These are subsumed by **REWORK Phase B6** (per-execution
   waterfall = the 10046 view, latency scatter, transition matrix) — **explicitly
   parked** and gated behind the Trust Milestone: *"no feature work (B6…) until Track T
   is done"*, resume after T1/T3 (all three B6 views are EXACT-required).

3. **HMM anomaly detection (14.6, `TRACING_ANALYSIS_PLAN.md` Phase 5)** — **[PARKED,
   research]**. Never built as HMM. A *different* anomaly engine shipped in rework **A5**
   (AAS-vs-baseline + lock-fraction rules, `src/anomaly.c`), later hardened by Trust
   **T3** (ESC-4/7). HMM proper remains research.

4. **`--format json` / `--format csv` output modes** — **[LEFT/OPEN]**. ROADMAP Phase
   1–8 shipped only "TUI and text output modes"; the CLI redesign's fuller `--format`
   matrix (`tui | text | json | csv`, incl. the planned `src/output_csv.c` and JSONL
   per-interval output, `docs/CLI_REDESIGN.md` Phase 12) never landed `json`/`csv`.
   (A Prometheus exporter, a sibling of this, is likewise noted as future in the rework.)

5. **Live-test sub-cases 0A.8 / 0A.9** — **[LEFT as dedicated live tests]**. Timed lock
   contention (0A.8) and live duration-percentile invariants (0A.9) were never built as
   standalone Layer-0A tests (0A.8 later exercised by Trust **T0** capture-smoke;
   percentile math covered synthetically by 0B.3).

6. **Standalone `test_compute.c` (Layer 3)** — **[LEFT/OPEN]**. The direct-C-call
   compute harness (aas / top_events / sessions / queries / filter matching / summary
   roundtrip) was not built; only `test_bucket.c` shipped. That surface is exercised
   through the server by the Layer-0B Python suite instead.

7. **Superseded-but-noted mechanisms (not "open," recorded for honesty):** the
   Rust TUI (Phases E.0/E.1, dropped Sprint 8); "merge daemon+server" (Sprint 11 /
   Arch 4, superseded by the control socket); and the three foundation fixes re-violated
   and re-hardened by Trust — **state_map GC** (3.3/Arch 5 → T4 CAP-1), **JSON escaping**
   (3.1/Issue 9 → T6 UI-6), **summary honesty** (0B.5 → T1 FID-2).

---

## Part 4 — Analysis features, CLI redesign & the research vision

This chapter consolidates three source documents and **replaces them**:
`docs/TRACING_ANALYSIS_PLAN.md` (the analysis-feature plan), `docs/CLI_REDESIGN.md`
(the CLI/UX redesign), and `docs/QT_PM_ANALYSIS.md` (the long-range Queueing-Theory /
Process-Mining research vision). Nothing from them is dropped; each item carries an
honest status badge verified against `src/server.c`, `src/compute.c`, `src/output.c`,
`web/static/`, `README.md`, and git.

**The shared premise.** pg_wait_tracer *traces* rather than *samples*: it captures
**every** wait-state transition with nanosecond precision, not a 1 Hz / 100 Hz sample.
That single fact is what makes exact percentiles, transition sequences, micro-burst
detection, per-execution timelines, and (aspirationally) process mining possible at all.
Every feature below exists to exploit it.

**Status badges used:** **[DONE vX]** (endpoint/file evidence) · **[DONE backend / UI
LEFT]** (compute+endpoint shipped, visualization deferred) · **[DROPPED]** · **[SUPERSEDED
by …]** · **[LEFT / research]** · **[PARKED]**.

---

### Tracing-analysis features (5 phases)

Each phase builds on the previous. **Crucial distinction preserved throughout:** Phases
1–4 all shipped at the **compute/endpoint layer** in `src/server.c`
(`handle_session_timeline`, `handle_transitions`, `handle_fingerprints`,
`handle_concurrency` (with burst detection), `handle_lock_chains`, `handle_interference`,
plus `handle_variants`), but **several of their UI visualizations were deferred**. The web
client (`web/static/`, registered in `app.js`) today wires views for *timeline*,
*transitions (Sankey)*, and *concurrency* — but **not** for fingerprinting, lock chains,
or cross-session interference (those endpoints have no front-end). Phase 5's HMM was
**never built**; the shipped `src/anomaly.c` is a different, simpler mechanism (see #9).

**Data available per event** (the raw record the whole plan is built on):

```
{pid, old_event (wait_event_id), duration_ns, query_id, wall_timestamp}
```

#### Phase 1 — Foundation (immediate value)

**#1 Session Timeline — Gantt chart for a PID / query_id.** **[DONE]**
The "EXPLAIN ANALYZE for wait events." Click a session or query → see every wait event as
a colored bar on a time axis. A PID (backend/connection) runs many queries over its
lifetime; the timeline shows all events for the PID with query_id boundaries marked
(vertical lines when query_id changes). A DBA sees: "this connection ran query A (fast),
then query B (stuck on lock for 3 s), then query C."
- **Entry points:** Sessions tab → click PID → timeline for that backend, all queries
  visible; Queries tab → click query → all executions of that query_id across PIDs (each
  PID as a separate swimlane — useful for parallel queries too).
- **Filtering:** by PID (default from sessions drill-down); by query_id (from queries
  drill-down, shows only that query's events); combined PID + query_id → single execution
  trace.
- **Server:** `session_timeline` endpoint — returns raw events for PID/query_id in a time
  range, capped to a reasonable count (e.g. 10K events, warn if truncated).
- **UI:** ECharts custom series (gantt-style horizontal bars). Color by wait class,
  tooltip shows event name + exact duration + query_id.
- *Evidence:* `handle_session_timeline` (server.c ~2340), `pid_idx_ht` hash index for the
  timeline second pass (DEVELOPMENT_PLAN 9.3); web `views/timeline.js` +
  `lib/builders/timeline.js`, registered as a view in `app.js`; drill pivot
  `pid → timeline` present in `app.js`.

**#2 Wait Event Duration Percentiles — P50/P95/P99/max per event.** **[DONE]**
Exact percentile columns on the Events table. The heatmap shows distribution shape over
time; percentiles give the single-number summary.

```
Event               Count     Total    Avg     P50      P95      P99      Max
IO:DataFileRead     1.2M      45.2s    37μs    12μs     180μs    2.1ms    45ms
LWLock:WALWrite     340K      12.1s    35μs    8μs      95μs     850μs    120ms
```

Instantly tells a DBA: "average is 37μs but P99 is 2.1ms — there's a long tail."
- Server collects durations per event_id, computes percentiles; **raw-events path only**
  (aggregated summaries don't preserve individual durations).
- *Evidence:* `hist_percentile()` in `compute.c` feeds `p50_us/p95_us/p99_us` into the
  top-events / query rows (compute.c ~890, ~983, ~1826). Exact-only: idle/summary records
  explicitly excluded from avg/max/percentiles.

#### Phase 2 — Unique to tracing (the differentiator)

**#3 Wait Transitions (Markov) — per-query state-flow diagram + Sankey.** **[DONE]**
For a selected query_id, compute the state-transition probability matrix from sequential
events; visualize as a Sankey. A DBA sees "this query always goes CPU → DataFileRead (85%)
→ WALWrite (60%) → CPU" and understands *coupled* bottlenecks — fixing IO won't help if a
WAL lock always follows. **Something no sampling tool can do.**
- Server: `transitions` endpoint — scans events sorted by (pid, timestamp), builds NxN
  transition counts, normalizes, returns the matrix for a query_id/PID.
- UI: Sankey (chord alternative) showing top transitions with probabilities.
- *Evidence:* `handle_transitions` (server.c ~2515); web `views/transitions.js` +
  `lib/builders/transitions.js`, registered in `app.js`; auto-refresh is suppressed on the
  transitions tab (it's a snapshot analysis).

**#4 Wait Pattern Fingerprinting — plan-change detection.** **[DONE backend / UI LEFT]**
Build a transition signature per query_id per time window; compare across windows — if IO
event count jumps 100× → seq scan replaced index scan → flag as a plan change. Detects plan
changes **without EXPLAIN**, purely from wait-event shapes:
- Index scan: CPU → few IO:DataFileRead → CPU
- Seq scan: CPU → many consecutive IO:DataFileRead → CPU
- Hash join: CPU (long) → IO burst → CPU
- Reuses the transition computation from #3; compares fingerprints across time windows
  (e.g. hourly). Planned UI: a "pattern changed" indicator in the Queries tab, click →
  see the diff.
- *Evidence:* `handle_fingerprints` endpoint exists (server.c ~2613). **No front-end
  consumes it** — there is no fingerprint view/indicator in `web/static/`. → visualization
  deferred (part of the REWORK B6 analysis-view backlog).

#### Phase 3 — Concurrency analysis

**#5 Peak Concurrency — micro-burst detection.** **[DONE]**
Adds `max_concurrent` per class/event to each AAS bucket. Sampling averages these away;
tracing preserves them. Detects thundering herd: "43 sessions hit buffer_mapping
simultaneously at 10:04:05." Small extension to the AAS compute (track max simultaneous
sessions per wait state within a bucket); UI overlays it on the AAS chart as markers /
secondary axis.
- *Evidence:* `handle_concurrency` (server.c ~2751); concurrency compute in `compute.c`
  (~2727, "high overlap = noisy neighbors contending"); web `views/concurrency.js` +
  `lib/builders/concurrency.js`, registered in `app.js` (auto-refresh suppressed on tab).

**#6 Burst Detection & Annotations.** **[DONE backend / UI LEFT]**
Find moments where N sessions simultaneously enter the *same* wait state within a narrow
window (< 10 ms); surface as clickable annotations on the AAS chart. Algorithm: scan
events sorted by timestamp, sliding-window convergence detection. Detects checkpoint
storms, lock convoys, buffer-mapping contention. Planned UI: annotation markers on the AAS
timeline, click → session timeline for affected PIDs (builds on #5 and #1).
- *Evidence:* burst compute is implemented in `compute.c` (§"Concurrency / Burst
  Detection" ~2431: `pgwt_burst`, `cmp_burst_desc`, `burst_window_ns`, `burst_threshold`;
  CPU/`old_event==0` skipped). The **clickable AAS annotation markers → drill-to-timeline
  UI** is not wired. → visualization deferred.

#### Phase 4 — Cross-session analysis

**#7 Lock Chain Detection.** **[DONE backend / UI LEFT]**
Find concurrent `Lock:transactionid` waiters and reconstruct wait chains: scan events for
overlapping Lock waits, match by PID + query_id correlation, reconstruct "A waits for B, B
waits for C." Shows exact lock-hold duration and chain depth. Planned UI: a new "Locks"
section/tab, or an overlay within the session timeline.
- *Evidence:* `handle_lock_chains` endpoint exists (server.c ~2669). **No Locks view/tab
  in `web/static/`.** → visualization deferred.

**#8 Cross-Session Interference Scoring.** **[DONE backend / UI LEFT]**
Temporal correlation between PIDs — mathematically prove "noisy neighbor" effects: "every
time PID 100 does IO, PIDs 200–250 enter LWLock." Cross-correlation at a configurable time
resolution. Computationally expensive — a targeted investigation tool, not real-time.
Planned UI: an interference report with victim/aggressor identification.
- *Evidence:* `handle_interference` endpoint exists (server.c ~2710). **No interference
  report in the front-end.** → visualization deferred.

#### Phase 5 — ML / anomaly detection (research)

**#9 HMM Anomaly Detection.** **[LEFT / research — NOT built]**
Train a Hidden Markov Model on "normal" transition matrices per query_id; alert when
transition probabilities deviate significantly. Would detect structural anomalies (buffer
cache poisoning, plan regression) **before** latency spikes — the transition pattern
changes even if total time hasn't yet. Requires a baseline collection period. Research
phase: evaluate whether the signal-to-noise ratio justifies the complexity.
- **Status:** deferred as a research task (DEVELOPMENT_PLAN 14.6: "HMM anomaly detection
  (research/evaluate) — Deferred").
- **Do not confuse with the shipped `src/anomaly.c`.** That file is a *different, simpler*
  mechanism from the tiered-capture rework (Track A / Phase A5): a small rules engine that
  watches the always-on sampled stream and **auto-escalates to full-fidelity capture** on
  an incident. Its two rules are (1) **AAS-vs-baseline** — fire when `aas > k *
  rolling_baseline` sustained N ticks; (2) **Lock-class fraction** — fire when the
  Lock-class share of active (idle-excluded) samples exceeds a threshold, sustained N
  ticks. It has hysteresis + cooldown + a rolling-hour escalation budget, and a pure,
  unit-tested rule core (`pgwt_anomaly_eval`). It is **not** an HMM and does **not** model
  per-query transition matrices. (QT/PM §3.3 conformance checking is proposed as the
  *actionable* alternative to HMM — see the research vision below.)

#### Key insight: tracing vs sampling (preserved reference table)

| Capability | Sampling (1 Hz) | Tracing |
|---|---|---|
| Events shorter than 1 s | Missed | Captured |
| Event count vs duration | Unknown | Exact |
| State transition sequence | Lost | Preserved |
| Exact percentiles | Estimated | Exact |
| Micro-burst detection | Averaged away | Visible |
| CPU starvation (long CPU events) | Invisible | Detectable |
| Per-execution query analysis | Impossible | Session timeline |
| Plan change detection (no EXPLAIN) | Impossible | Wait fingerprint |

#### Note — the REWORK "Phase B6" new-analysis-view backlog

Beyond the original five phases, the UI rework (`docs/REWORK_PLAN.md`) defines a **Phase
B6 — New analysis views** that is **⬜ not started (deferred; resumes after Trust-Milestone
T1/T3)**. All three are EXACT-data-required (must report "needs full-fidelity data" over
sampled-only windows):
1. **Per-execution waterfall** (the true Oracle-10046 view): fingerprint → executions list
   (sorted by duration) → one execution's lifetime as a span timeline, each wait a colored
   bar, gaps = on-CPU, phase boundaries from `PGWT_MARKER_PLAN_START`/`EXEC_START`. New
   `executions` + `execution_detail` server views; ECharts custom series.
2. **Latency scatter**: every wait a dot on (time, log duration), colored by class —
   exposes modes/outlier bands aggregates hide. Needs **server-side density downsampling**
   (cap points per screen-pixel bucket, report dropped count, no silent truncation);
   click/lasso → drill to events.
3. **Transition matrix**: the Sankey's scalable sibling (from-event rows × to-event
   columns, color = count/time, grouped by class), readable past ~20 nodes where the
   Sankey saturates; a toggle Sankey ⇄ matrix on the transitions tab; reuses the existing
   transitions computation.

---

### CLI redesign (§1–§18)

Motivation: the original CLI was TUI-only (screen-clearing), cumulative-only, showed wait
*classes* but not individual events in the time model, and had no way to investigate the
query↔event relationship the way Oracle ASH does. The redesign brings real DBA
investigation workflows. **Nearly all of it shipped.** The two intentional departures:
(a) `--format json/csv` was **dropped** in favour of structured JSON over the
`pgwt-server` web protocol; (b) the bespoke `.pgwt` snapshot recording format was
**superseded** by the daemon's columnar `.trace.lz4` + `--replay`.

**Decisions made (design ground rules):** auto-detect TTY (terminal → `tui`, pipe →
`text`); 3 configurable time windows (not just delta+cumulative); `--count N` for exact
snapshot control; Prometheus deferred, `--format json` "first"; CLI-first, daemon later;
auto-discover the PG instance when only one cluster runs; a semi-interactive "top-like"
active-sessions view (`--sort`, no ncurses); no query text initially (cmdline parsing for
backend info only); query text from shared memory planned (Phase E.2 — `st_activity` via
`process_vm_readv`); plan identifier from shared memory planned (Phase E.3 — `st_plan_id`,
PG18+ only).

#### §1 Time Windows (`--window`) **[DONE]**
Three configurable windows so the DBA sees NOW, RECENTLY, OVERALL:
`--window 5s,1m,5m` (default) / `10s,5m,30m` / `5s,1m,1h`. First window always equals
`--interval`; format `Ns`/`Nm`/`Nh`. **Implementation:** ring buffer of snapshots, one per
tick, size = largest_window / interval; each snapshot = time_model (88 B) + compacted
system_events (~5 KB) + compacted query_events (~10 KB) → 1 h at 5 s = 720 × 15 KB ≈
10.8 MB (acceptable). Delta for window W = current − snapshot from (W/interval) ticks ago.
- *Evidence:* `parse_windows()`, `PGWT_MAX_WINDOWS` (pg_wait_tracer.c ~138); help text
  `-w, --window <W1,W2,W3>`.

#### §2 Enhanced time_model: event hierarchy **[DONE]**
The old time_model showed only class-level totals (Wait: IO, Wait: LWLock). Now it shows
the top specific events per class as indented sub-categories, e.g.:

```
  Stat Name                 Last 5s   % DB    Last 1m   % DB    Last 5m   % DB
  DB Time                   5088.6  100.0%   62340.1  100.0%  312450.8  100.0%
    CPU                     1498.6   29.4%   13712.3   22.0%   72312.1   23.1%
    IO                       862.3   16.9%   12340.5   19.8%   98234.2   31.4%
      IO:DataFileRead        621.2   12.2%    9823.1   15.8%   82123.4   26.3%
      IO:DataFileWrite       198.4    3.9%    2012.3    3.2%   12345.6    4.0%
      IO:WALSync              42.7    0.8%     505.1    0.8%    3765.2    1.2%
    LWLock                   423.1    8.3% ...
    Lock                     312.4    6.1% ...
    Client                    88.2    1.7% ...
  (Activity/Idle)          12560.4     —     62340.1     —    312450.8     —
```

Design choices: top 3 events per class (configurable via `--top N`); only show events ≥ 1%
of DB Time (avoid clutter); classes with 0 time hidden; CPU is a single line (not a wait
class, no sub-events). This is the "one view to rule them all": hot classes, the specific
driving events, and how they compare across time windows, at a glance.
- *Evidence:* `MAX_SUB_EVENTS`/`MAX_SUB_EVENTS_M = 3`, per-class top-sub-event selection in
  `output.c` (~184, ~313).

#### §3 ASH-like query↔event analysis **[DONE]**
Answers the two Oracle-ASH questions: (1) *which queries cause this wait?* (event→queries)
and (2) *what is this query waiting on?* (query→events). New `--query-id <ID>` filter.
Three `query_event` view modes:
- **Mode A (default):** top query–event combinations (query_id, wait event, waits, total,
  avg, max, %DB).
- **Mode B (`--event <NAME>`):** top queries for a specific wait event, adds **% Event** =
  fraction of that event's total time ("query 5678… is responsible for 31.4% of all
  DataFileRead time").
- **Mode C (`--query-id <ID>`):** all events for one query, adds **% Query** = fraction of
  that query's total time ("this query spends 58.2% on CPU, 31.5% on DataFileRead").
- `query_event` also supports the 3 time windows when `--window` is set (Last 5s / 1m / 5m
  sections).
- *Evidence:* CLI flags `-e/--event`, `-Q/--query-id` (pg_wait_tracer.c ~232–234); web
  `views/queries.js` with the event↔query drill pivots
  (`{class:'events', event_id:'queries', pid:'timeline', query_id:'events'}` in app.js).

#### §4 Histogram windows **[DONE]**
Three latency distributions side-by-side (one per window):
`--view histogram --event IO:DataFileRead --window 5s,1m,5m` → a per-bucket table
(`<1`, `1-2`, … `>=16K` μs) with count + % for each of Last 5s / 1m / 5m, so the DBA reads
"distribution is stable across windows — no latency shift."
- *Evidence:* web `views/histogram.js` + `lib/builders/histogram.js` (registered in
  app.js); CLI histogram view path.

#### §5 Auto-discovery of the PostgreSQL instance **[DONE]**
When neither `--pid` nor `--pgdata` is given, auto-detect the postmaster (ported from
`tests/testutil.sh:find_postmaster()`): `pgrep -x postgres` → filter out children (parent
comm also "postgres") → exactly 1 postmaster ⇒ use it; multiple ⇒ list each with version +
PGDATA and FATAL "use --pid/--pgdata"; none ⇒ FATAL "no running PostgreSQL instance."
Version from exe path via `readlink /proc/PID/exe`; PGDATA from `/proc/PID/environ`
(`PGDATA=`) or the `-D` cmdline flag. Both `--pid` (multi-instance hosts) and `--pgdata`
(systemd/scripts, reads `postmaster.pid`) are kept.
- *Evidence:* `src/discovery.c` (~41 KB) + `discovery.h`.

#### §6 Active Sessions view (`--view active`) **[DONE]**
A "top-like" refreshing view of currently active backends — what a DBA opens first.
Columns: **PID · State** (`on cpu` | `waiting` | `idle`, from BPF tracing state) **· Wait
Event · Wait (ms)** (time in current wait, from BPF timestamp delta) **· DB Time (ms)**
(cumulative) **· Backend Type** (from `/proc/PID/cmdline`: client backend, autovacuum
worker, wal writer, checkpointer, bgwriter, walreceiver, …). Sorting via `--sort
wait_time|db_time|pid|event`. Semi-interactive (screen-clear refresh in TUI mode, no
runtime key bindings). *Future column:* query text from shared memory (see §15).
- *Evidence:* Active-sessions output block in `output.c` (~1075–1163); `PGWT_VIEW_ACTIVE`
  dispatch in `daemon.c`; `--sort` enum (`PGWT_SORT_EVENT` etc., pg_wait_tracer.c ~133);
  `src/cmdline.c`/`cmdline.h` (backend-type parser); web `views/active.js` (persistent,
  single-flight channel).

#### §7 Session-event windowing (`--view session_event`) **[DONE]**
**Summary mode** (default): latest interval only (a per-backend snapshot is already
interval-sized). **Detail mode** (`--pid-filter <PID> --window 5s,1m,5m`): time-windowed
deep dive on one backend — e.g. "PID 34521 is currently stuck on Lock:Transaction (96.8%
of last 5s) but historically was mostly DataFileRead (41.2% of last 5m)."
- *Evidence:* `-P/--pid-filter` flag (pg_wait_tracer.c ~233); session_event detail path in
  output.c; web `views/sessions.js`.

#### §8 Filtering **[DONE]**
`--class IO` / `--class IO,LWLock` (system_event); `--min-pct N` (hide events below N% of
DB Time); `--top N` (top events per class in time_model, default 3). *Evidence:* flags
parsed in pg_wait_tracer.c; `--top` honored via `MAX_SUB_EVENTS`.

#### §9 `--format` flag — tui/text **[DONE]**; json/csv **[DROPPED]**
`--format tui` (screen-clearing interactive, default for a terminal) and `--format text`
(no screen clear, timestamp per interval, default for pipes) both ship, with
`isatty(stdout)` auto-detect. **`--format json` (JSONL per interval) and `--format csv`
(flat rows) were dropped** — the format *tokens* are still accepted by the parser
(`pg_wait_tracer.c` maps `"json"`→`PGWT_FMT_JSON`, `"csv"`→`PGWT_FMT_CSV`) but **no
formatter is wired** (there is no `src/output_json.c` / `src/output_csv.c`, and nothing in
the output/daemon path consumes those enum values). Structured JSON is instead delivered by
the **`pgwt-server` web protocol**, whose handlers emit JSON via vendored **cJSON**
(DEVELOPMENT_PLAN Sprint 6: all handlers build a cJSON tree + `cJSON_PrintUnformatted`).
The doc's promise "JSON includes everything — all windows, event hierarchy, query details"
is met by the web protocol, not by a CLI JSONL mode.

#### §10 `--count N` flag **[DONE]**
`--count 1` (one-shot: one interval, print, exit), `--count 10` (10 intervals then exit).
*Evidence:* `-n/--count` (pg_wait_tracer.c ~230).

#### §11 Full CLI summary (preserved reference)
```
Usage: pg_wait_tracer [OPTIONS]
Target (auto-detect if omitted, single instance):
  -p, --pid <PID>          Postmaster PID
  -D, --pgdata <DIR>       PGDATA directory (reads postmaster.pid)
Views:
  -V, --view <VIEW>        time_model | system_event | session_event |
                            histogram | query_event | active
Output control:
  -f, --format <FMT>       tui | text | json | csv  (default: auto-detect)
  -i, --interval <SEC>     Refresh interval (default: 5)
  -d, --duration <SEC>     Stop after N seconds
  -n, --count <N>          Stop after N intervals
  -w, --window <W1,W2,W3>  Time windows (default: interval only)
Filters:
  -e, --event <NAME>       Event filter (histogram: required; query_event: by event)
  -P, --pid-filter <PID>   Show detail for a specific backend (session_event)
  -Q, --query-id <ID>      Filter query_event to one query
  -C, --class <CLASS>      Filter by wait class (system_event)
      --min-pct <N>        Hide events below N% of DB Time
      --top <N>            Top events per class in time_model (default: 3)
      --sort <COL>         Sort column for active view (wait_time|db_time|pid|event)
Other:
  -v, --verbose            Verbose output to stderr
  -h, --help               Show this help
```
(The `json`/`csv` tokens remain in the help string but are non-functional per §9. The live
CLI additionally grew tiered-capture / replay / anomaly flags — `--replay`, `--from`,
`--to`, `-T <trace-dir>`, `--mode tiered`, `--anomaly-*` — documented in the daemon/trace
chapters.)

#### §12 Implementation order (preserved reference — Phases 1–16)
| Phase | What | Effort |
|-------|------|--------|
| 1 | Auto-discovery + `--count` + `--format` infra + TTY auto-detect | Medium |
| 2 | Snapshot ring buffer + `--window` parsing + delta computation | Medium |
| 3 | Enhanced time_model with event hierarchy (subcategories) | Medium |
| 4 | Multi-window time_model (side-by-side columns) | Medium |
| 5 | Multi-window system_event (3 sections) | Small |
| 6 | ASH-like query_event (`--event`, `--query-id`) | Medium |
| 7 | Multi-window histogram | Small |
| 8 | Active sessions view (`--view active`, `--sort`, cmdline parsing) | Medium |
| 9 | Session event windowing (`--pid-filter` with windows) | Small |
| 10 | Text format (no screen clear, timestamps) | Small |
| 11 | JSON format | Medium |
| 12 | CSV format | Small |
| 13 | Filtering (`--class`, `--min-pct`, `--top`) | Small |
| 14 | Update README + tests | Medium |
| 15 | Recording & replay (`--record`, `--replay`, `--from`, `--to`) | Medium |
| 16 | SQL query text exposure (shared memory or eBPF uprobe) | Medium-Large |
*Status:* Phases 1–10 and 13–16 essentially delivered (11/12 = JSON/CSV dropped per §9;
15 superseded per §14).

#### §13 Files to modify (preserved reference + what actually exists)
Planned edits touched `pg_wait_tracer.[ch]`, `daemon.[ch]`, `map_reader.[ch]`,
`output.[ch]`, `Makefile`, and several **new** files. Reality against the tree:
- **`src/cmdline.c` / `.h`** — **exists** (backend-type parser).
- **`src/query_text.c` / `.h`** — **exists** (query text reader — §15, shipped).
- **`src/output_json.c`** — **does not exist** (JSON via web/cJSON instead; §9 dropped).
- **`src/output_csv.c`** — **does not exist** (§9 dropped).
- **`src/recording.c` / `.h`** — **does not exist** (superseded by the daemon trace writer
  `event_writer.c` + reader `event_reader.c` + `replay.c`; §14).
- `map_reader.[ch]` — exists (snapshot / ring / window-delta helpers).

#### §14 Recording & Replay ("SAR-like time travel") **[SUPERSEDED by the daemon's `.trace.lz4` + `--replay`]**
*The vision (as written):* a bespoke `--record perf.pgwt` writing one `pgwt_snapshot`
(time_model + system_events + query_events) + timestamp per tick to a binary file
(~15 KB/snapshot → ~10.8 MB/h at 5 s → ~260 MB/24 h), and `--replay perf.pgwt [--from … --to …]`
that loads snapshots back into the ring buffer and renders the same views via the same
delta logic (`pgwt_ring_delta`), no BPF/root needed for viewing. Active sessions were noted
as not replayable (real-time BPF state not captured in snapshots).
*What shipped instead:* the daemon persists a **columnar `.trace.lz4`** event stream (block
codec, delta-encoded timestamps, footer with block count; `event_writer.c` /
`event_reader.c`), rotated hourly (`YYYY-MM-DD_HH[.N].trace.lz4`) with retention, plus
`.summary.lz4` sidecars. **`--replay`** reads those trace files offline (no root, no
PostgreSQL) with `--from`/`--to` time-range seek and `-T <trace-dir>`, feeding the same
view/output code (`event_reader.c` = "block decode, time-range seek, replay";
`pg_wait_tracer.c` replay path forces text mode; the whole `pgwt-server` is literally "a
trace-file replay server for the web client"). So the *capability* (record now, analyze
later, share the recording, no-root viewing, time-range replay) is **delivered** — via a
richer full-event format than the planned per-tick snapshot. The `.pgwt` snapshot format
itself is **dropped**.

#### §15 SQL Query Text Exposure **[DONE — Option A (shared memory)]**
*Decision as written:* two approaches were evaluated — **Option A (shared memory):** read
`PgBackendStatus.st_activity` via `process_vm_readv()` after resolving `BackendStatusArray`
from ELF symbols (pros: ~100–150 lines, no extra BPF, same data as `pg_stat_activity`; cons:
PG-version-dependent struct offsets, static-symbol resolution maybe needing debuginfo,
point-in-time race, `track_activity_query_size` cap); **Option B (eBPF uprobe)** on
`exec_simple_query()` / `pgstat_report_activity()` capturing the string via
`bpf_probe_read_user_str()` into a per-PID map (pros: no shmem-layout dependency,
signature stable 15+ yrs; cons: ~256 B/read truncation, large fixed BPF map
(1024 B × max_backends ≈ 1 MB/1000 backends), extended-query-protocol needs extra hooks).
A full decision matrix (complexity, PG-version sensitivity, debuginfo dep, text length,
overhead, extended-protocol, tool consistency) is preserved in the source; **Option A was
chosen** because `st_activity` gives the *actual running SQL with real parameter values*,
more useful for debugging than normalized text; texts are deduplicated by `query_id`,
first-seen kept as representative.
*What shipped:* `src/query_text.c` reads `st_activity_raw` from `PgBackendStatus` via
`process_vm_readv` (resolving `st_activity_offset`), dedups by id (`seen_check_or_insert`),
and writes a **`query_texts.jsonl`** sidecar (with compaction + load-existing on restart).
Surfaced in the web **queries** view as a query-text hover/tooltip cell
(`views/queries.js`). Query text appears (per the plan) in the active view, query_event
view, and session detail. → Option A implemented; Option B not pursued.

#### §16 Daemon mode **[DONE]**
*As written (framed "future, not in Phase 1–16"):* a long-lived `--daemon` process holds
the BPF programs + ring buffer; thin CLI clients connect over a Unix socket, request views
with filters, and format the JSON response — avoiding BPF attach/detach per ad-hoc query,
allowing multiple simultaneous clients, and forming the base for a Prometheus exporter and
a PG-extension SQL wrapper (`pg_wait_tracer_time_model()` …). *Reality:* daemon mode is now
the **core architecture**, not a future add-on — `src/daemon.c` (main epoll/timer/signal
loop, `d->daemon_mode`), a **control socket** (`src/control.c`, `status`/`metrics`
commands), tiered capture, and the `pgwt-server` request/response protocol that the web
client speaks. See `docs/DAEMON_ARCHITECTURE.md`. (Prometheus exporter remains a parked
future consumer; metric names already stable.)

#### §17 Query Plan Identification **[LEFT / OPEN]**
*Decision as written:* capture `st_plan_id` from `PgBackendStatus` in shared memory
(**PG18+ only**) via the same `process_vm_readv` read (essentially free — extend the
existing read by 8 bytes; PG18 added `int64 st_plan_id` right after `st_query_id`; on PG17
the field doesn't exist). **Limitation:** core PG does not compute `planId` automatically —
without a `planner_hook` extension (`pg_store_plans`, `pg_stat_sql_plans`) `st_plan_id` is
always 0. Full execution plans are **not** feasible to capture passively (plan tree is in
backend-local memory, dozens of node types; `auto_explain` only logs; `pg_store_plans`
storing plan text by (queryid, planid) is the best option if users want the actual text).
Intended flow: AAS spike → drill to query → see `plan_id` changed at the spike → plan
regression confirmed → examine via `pg_store_plans`/`EXPLAIN`.
*Status:* **not implemented** — no `st_plan_id`/`plan_id` reference exists anywhere in
`src/`. This is the one CLI-redesign item still fully open.

#### §18 Verification (preserved reference)
The doc lists a canonical command set: auto-detect (`sudo ./pg_wait_tracer`); explicit
`--pid`; enhanced time_model; `--window 5s,1m,5m`; `--view active [--sort db_time]`;
`--view session_event --pid-filter 34521 --window …`; ASH `--view query_event --event
IO:DataFileRead` and `--query-id …`; histogram across windows; one-shot `--format json
--count 1` (now non-functional per §9); pipe to file (auto text); multi-instance list;
record/replay (`--record …` now superseded, `--replay …` shipped); active `--show-query`
(Phase 16); and `sudo tests/run_all.sh`.

---

### Queueing-Theory / Process-Mining research vision (long-range)

This is the aspirational, long-range research program (`docs/QT_PM_ANALYSIS.md`): an applied
mathematics framework for turning nanosecond wait-event traces into actionable insight by
combining **queueing theory (QT)** and **process mining (PM)**. **It is almost entirely
unbuilt.** The *only* part that ships is §3.2 Discovery (the DFG/transitions endpoint and
the variants endpoint). Everything else — QT metrics, sojourn-shape classification, W(N)
regression, Little's-Law stationarity, conformance, enhancement, the S1–S4 signal engine,
the Investigation Canvas, the rule engine, and every §7 endpoint (QT metrics, conformance,
anomaly, MVA prediction) — is **[LEFT / research]**. It is preserved here in full as the
backlog.

#### §1 What the trace data actually represents
Each record = *backend PID was in state X for `duration_ns`, then moved to state Y, during
query Q.* Critically, `duration_ns` is **sojourn time** (queueing delay + service time),
**not** pure service time and **not** pure wait time. What it bundles per class:

| Event class | What `duration_ns` includes | Decomposable? |
|---|---|---|
| **CPU** | useful work + OS run-queue starvation (not distinguishable) | No |
| **IO:DataFileRead** | IO scheduler queue + physical read + possible OS cache hit | No |
| **Lock:transactionid** | almost pure queueing (waiting for another backend) | Mostly yes |
| **LWLock:\*** | spin + sleep waiting for a lightweight lock | Mostly yes |
| **Client:ClientRead** | waiting for the application (not a shared resource) | N/A |
| **IPC:SyncRep** | waiting for replication acknowledgment | Depends on setup |

#### §2 Queueing Theory — what's honestly computable **[LEFT / research]**
- **§2.1 Directly measurable (no assumptions):** **N(t)** = backends in state X (count
  overlapping intervals); **λ(t)** = arrivals into X (transitions in / window); **W** =
  sojourn time = mean(duration_ns); **sojourn distribution** = exact empirical CDF from all
  duration values; **X(t)** = throughput (departures/s = transitions out / window).
- **§2.2 What sojourn-distribution shapes reveal** (mechanism without knowing capacity c):
  **Exponential** (single log-CDF slope) → memoryless, healthy simple queue; **Bimodal**
  (two peaks) → two populations, e.g. IO cache-hit ~5 μs vs physical ~200 μs, peak ratio =
  effective cache-hit rate; **Heavy tail** (P99 ≫ P50) → contention/starvation, tail index
  = severity (LWLock convoys); **Nearly deterministic** (tight spike) → predictable service
  (sequential-scan page reads); **Shifted exponential** (flat left + decay) → constant
  service + random queueing, shift point estimates Ws. *Track shape over time:* unimodal→
  bimodal = cache behavior changed; exponential→heavy-tailed = contention escalating.
- **§2.3 Window size for λ, μ:** not fixed — adaptive per event, governed by sample count
  (target N ≥ 30), not clock time (or EWMA updated per event). Guidance: LWLock
  (10K–100K/s) ~1 ms; IO:DataFileRead (100–10K/s) 10–300 ms; Lock:transactionid (1–100/s)
  1–30 s; BufferPin (0.1–10/s) 10 s–5 m.
- **§2.4 W(N) regression — the key trick:** for each occurrence of X we know its
  `duration_ns` (W) *and* N(t_arrival) (concurrency on entry). Plot W vs N: the **intercept
  (W at N=1)** estimates pure service time **Ws**; the **knee** reveals capacity **c** from
  data alone; the **slope after the knee** = queueing behavior; a **flat line** = c is
  large / resource not shared. Works best where N varies.
- **§2.5 What we CANNOT directly measure** (and the estimator): **c** (capacity) — not in
  trace → config (cores, WAL locks) or W(N) knee; **Ws** — bundled with Wq → W at N=1, or
  min/P5, or W(N) intercept; **Wq** — bundled with Ws → W − Ws; **ρ = λ/(c·μ)** — needs c →
  only when c known/inferred.
- **§2.6 Little's Law (system level):** L = λ·W (L = avg backends in state = our N). Needs
  no knowledge of c, distribution, or discipline. Primary use = **stationarity check**:
  R(t) = L(t)/(λ(t)·W(t)); R≈1 stable, R>1 backends accumulating (load↑ / resource
  degrading), R<1 draining; the moment R leaves 1 marks a regime change.
- **§2.7 Throughput-saturation detection:** compare λ(t) (arrivals) vs X(t) (departures);
  X≈λ steady; X<λ sustained → overloaded, queue building; gap (λ−X) = queue-buildup rate.
  Model-free overload detector, no assumptions about c or distribution.

#### §3 Process Mining — applied to PostgreSQL traces
- **§3.1 Vocabulary mapping:** **Case** = one query execution (EXEC_START→…→EXEC_END for
  (pid, query_id)); **Activity** = a wait event; **Event** = one trace record; **Trace** =
  ordered wait-event sequence for one execution; **Event log** = the whole trace file.
- **§3.2 Level A — Discovery: what actually happens.** **[DONE]**
  The **Directly-Follows Graph (DFG)** — for every pair (A,B) count transitions A→B — is
  **already implemented (the `transitions` endpoint, `handle_transitions`)**, and **loop
  compression via the `variants` endpoint (`handle_variants`)** is also shipped. *Preserved
  caveats/roadmap for the rest of Discovery (LEFT):* a DFG cannot distinguish **sequence**
  (A then B always) from **choice/XOR** (sometimes A, sometimes B — index vs seq scan) —
  both yield identical edges; richer **process models** would capture Sequence, Choice
  (XOR), Parallelism (AND — parallel workers, order varies), and Loop (A repeats N times —
  the part variants already compresses). Recommended algorithms: **Heuristics Miner**
  (noise-tolerant, frequency-based) and **Inductive Miner** (guarantees a sound model,
  recursive decomposition). Value: the *actual execution recipe as observed by the kernel*
  — not what EXPLAIN says should happen, including WAL/lock/IPC interactions EXPLAIN never
  shows.
- **§3.3 Level B — Conformance: what changed.** **[LEFT / research]**
  Build a reference model from a "healthy" baseline period, replay new traces, measure
  deviation. **Fitness (0–1):** fraction of observed behavior the model explains (1.0 = all
  traces fit; 0.7 = 30% unexplained). **Precision (0–1):** how much the model allows that
  wasn't observed (1.0 tight; 0.5 too permissive). Detects **plan regression** (loop count
  3→80 IO reads), **lock contention appearing** (Lock events in traces that never had
  them), a **new IO path** (TOAST reads after a schema change), and separates **structural
  change** (conformance) from **performance change on the same structure** (QT distribution
  analysis). Explicitly framed as the actionable superior to Phase-5 HMM: conformance says
  *what* changed ("Lock:transactionid appeared where the model says CPU→IO") vs HMM's
  "probability dropped to 0.03 — investigate further."
- **§3.4 Level C — Enhancement: where time is spent.** **[LEFT / research]**
  Overlay sojourn-time stats on the discovered model. Key insight: **the bottleneck is a
  path, not a node.** top_events says "IO:DataFileRead = 45% of DB time" (incomplete); the
  enhanced model says "…but only on the loop path when loop_count > 50; when < 10, IO is
  negligible and time is in LWLock." Worked root-cause example (150 ms total): CPU 8 ms
  (5%) → LOOP×85 [IO:DataFileRead 102 ms 68% + CPU 25.5 ms 17%] → LWLock:WALInsertLock 5 ms
  (3%) → IO:WALWrite 9.5 ms (6%); conclusion: slow because of **85 loop iterations**, not
  slow IO (each read is 1.2 ms) — fix the plan, not storage.

#### §4 Four fundamental signals **[LEFT / research]**
All DB performance problems reduce to combinations of four:

| Signal | Detection method | Meaning |
|---|---|---|
| **S1 Resource saturation** | ρ→1, or W grows with N, or X<λ | a resource can't keep up |
| **S2 Process-shape change** | conformance drop, new variant, loop-count shift | the recipe changed |
| **S3 Distribution shift** | sojourn CDF changes shape (KS test, mixture model) | same path, different performance |
| **S4 Resource coupling** | cross-correlation of λ/N between events; causal chain in model | resources affect each other |

#### §5 Twelve practical problems (the DBA/developer's view) **[LEFT / research]**
Preserved in full as the motivating catalogue (each = signals + the QT+PM answer):
1. **"DB is slow right now — what's the bottleneck?"** (S1+S4). 40 backends on
   Lock:transactionid — 10 slots (fine) or 1 (crisis)? W(N) knee at N≈2 → capacity ~2, 40
   arrivals = overloaded; λ>X → queue building; PM: UPDATE users = 70% of lock arrivals,
   lock hold = `[Lock→CPU→IO:WALWrite→CPU]` = 3 ms service + 8 ms WAL = 11 ms → **WAL
   latency extends lock hold; fix WAL → fix locks.**
2. **"Query slower after deploy, EXPLAIN same plan"** (S2+S3). Conformance: baseline
   `CPU→IO(×3)→CPU→WAL→CPU`, current `IO(×80)` — loop 3→80; distribution IO unimodal(4 μs
   cache) → bimodal(20% 4 μs, 80% 300 μs physical). **Same plan, data grew / cache evicted
   — hunt the new cache-polluting queries.**
3. **"How many connections should I configure?"** (S1+S2). MVA throughput curve from trace:
   peaks at N=32–40; above 48 Lock:transactionid saturates and throughput drops; PM: query
   models *gain* Lock steps at high N. **Pool 32–40; reads higher, writes < 30.**
4. **"Periodic latency spikes every 5 min"** (S1+S2). λ(IO:DataFileWrite) spikes every
   300 s, ρ(IO)=0.95; W(IO:DataFileRead) ×5 (reads vs checkpoint writes); PM: query models
   gain IO:DataFileWrite (FPI) + IO:WALSync during spikes. **checkpoint_completion_target
   0.9, or separate WAL disk.**
5. **"Is my query design efficient?"** (S2+S3). Model `CPU→LOOP×500[IO→CPU→IO→CPU]` = two
   reads/iter = nested loop; variant 90% do 400–600, 3% do 5000+ (skewed key); inner IO
   bimodal; worst case 5000×2×300 μs ≈ 3 s. **Hash join for large keys, or pg_prewarm.**
6. **"Reporting queries kill OLTP"** (S3+S4). Cross-correlation: N(IO, REPORT)↑ → W(IO,
   OLTP)↑ 200 ms later; OLTP IO unimodal(5 μs)→bimodal only while reports run; report =
   `CPU→LOOP×100000[IO→CPU]` seq scan; OLTP hit rate 95%→40%. **Move reports to a replica /
   off-peak.**
7. **Autovacuum interference** (S1+S4). BufferPin spikes when autovacuum runs; OLTP gains a
   BufferPin step; W(N) for BufferPin c=1/page (vacuum holds cleanup lock);
   N(BufferPin,OLTP) correlates with N(CPU,autovacuum).
8. **Index bloat over weeks** (S2 gradual). Same query/plan, IO loop 10→15→25→40 over
   weeks; each IO fast (no distribution shift) → not cache. **Index bloat / data growth;
   track loop count per query_id over days.**
9. **Parallel-query worker starvation** (S4+S1). Workers' IO arrivals correlated; IO
   saturates only during parallel queries (ρ 0.3→0.9); leader model shows
   IPC:ParallelFinish grow with worker count → diminishing returns above K workers.
10. **Connection storm at app restart** (S1+S2). CPU saturates 2–3 s; models dominated by
    auth/startup steps; λ(CPU) ×10 exceeds capacity.
11. **Replication lag from specific write patterns** (S2+S1). WAL-heavy models (large
    updates, FPI); query 0xABCD = `CPU→LOOP×200[IO:DataFileWrite→CPU→IO:WALWrite→CPU]`;
    λ(IO:WALWrite,0xABCD)↑ → replication lag↑.
12. **Temp-file spill** (S2+S3). IO:DataFileWrite appears in queries that never write;
    conformance drop (new write loop); new IO latency population. **work_mem exceeded,
    sort/hash spilled.**

#### §6 Interface concept: the Investigation Canvas **[LEFT / research]**
- **§6.1 Three layers.** **Layer 1 — Timeline (WHEN):** the existing AAS chart with
  automated anomaly markers overlaid, phrased in DBA language ("Lock:transactionid
  approaching saturation", never "ρ > 0.8"); QT/PM stay invisible as the detection engine.
  **Layer 2 — Investigation (WHAT/WHY):** click a marker → an *evidence-chain* panel
  (Finding → Evidence 1 W(N) chart with service time + knee → Evidence 2 top contributor
  queries → Evidence 3 process model showing WAL inside the lock critical section →
  Suggested cause → Suggested action → [Compare to baseline]/[Affected queries]/[Predict]);
  evidence blocks are charts/models/tables connected by narrative, with cross-links.
  **Layer 3 — Prediction (WHAT-IF):** tweak parameters ("pool = 30?" → MVA recomputes
  throughput; "WAL latency halves?" → W(N) recomputes ρ; "add an index?" → loop 200→3,
  predict new ρ(IO)).
- **§6.2 Investigation pattern:** every investigation follows **SIGNAL → SCOPE → MECHANISM
  → ROOT CAUSE → ACTION** (signal auto-detected from S1–S4; scope narrows to
  resource/query/time; mechanism = QT+PM evidence; root cause = causal chain across
  resources; action = what to change with predicted impact).
- **§6.3 Implementation — rule engine + guided exploration.** **Option A (primary): rule
  engine** — codify ~15–25 investigation patterns as deterministic, transparent,
  explainable rules, e.g. `resource_saturation` (WHEN W(N) slope > threshold after knee AND
  N > knee×2 THEN compute W(N), find top-λ queries, extract each process model, check if
  other events sit inside the critical section, emit evidence chain, suggest reduce
  concurrency / speed critical path) and `cache_eviction` (WHEN IO CDF unimodal→bimodal AND
  the shift correlates with N(IO, other_query) THEN compute before/after distributions,
  identify the evicting query by cross-correlation, show victim/aggressor models, suggest
  separate workloads / more cache). **Option B (fallback): guided exploration** — when no
  rule fires but markers exist, present raw signals and suggest related views; over time,
  codify repeated exploration paths into new rules.

#### §7 Architecture — new server endpoints needed **[LEFT / research]**
- **§7.1 QT-metrics endpoint** — per event per window: `λ`, `X`, `mean_W_us`, `N_avg`,
  `N_max`, `littles_ratio`, a `distribution` block (`p5/p50/p95/p99_us`, `shape`,
  `peaks_us[]`, `peak_weights[]`), `W_N_pairs[[N,W]…]`, `W_N_knee`, `W_N_intercept_us`.
- **§7.2 Conformance endpoint** — `query_id`, `baseline_period`, `current_period`,
  `fitness`, and `deviations[]` (e.g. `{new_activity, Lock:transactionid, freq 0.15}`,
  `{loop_count_change, IO:DataFileRead, baseline_avg 3.2, current_avg 78.5}`).
- **§7.3 Anomaly-detection endpoint** — `findings[]` each `{signal (S1…), event, severity,
  summary, evidence_refs[], related_queries[], timestamp_ns}` (the machine-readable feed
  behind the Layer-1 markers).
- **§7.4 Prediction endpoint (MVA)** — request `{backends, modifications{IO_Ws_factor…}}` →
  result `{throughput_qps, bottleneck, per_resource[{event, ρ, mean_W_us}…]}` (closed
  queueing-network what-if).

#### §8 Implementation order (research phases) **[LEFT / research]**
1. **QT metrics** (foundation): W(N), distribution-shape classification, λ/X/N series,
   Little's-Law ratio — extends the existing top_events/heatmap infra. 2. **Anomaly
   detection (S1–S4):** saturation / distribution-shift / throughput-divergence detectors →
   timeline markers. 3. **Conformance checking:** store baseline process models (from
   variants), replay, compute fitness, detect structural change. 4. **Investigation rules:**
   codify 10–15 patterns → evidence chains + suggested actions. 5. **Prediction (MVA):**
   closed-network model from per-resource service demands → throughput curve + sensitivity.

#### §9 What QT + PM give together (preserved reference)
| What you learn | QT alone | PM alone | Both together |
|---|---|---|---|
| Which resource is stressed | ρ, W(N), saturation | — | — |
| Which queries hit it | — | path analysis | which queries take which paths under which conditions |
| Why it's stressed | service vs queueing | causal chains | root cause: "WAL latency extends lock hold" |
| When behavior changed | distribution shift, Little's Law | conformance score | structural vs performance change |
| What to do | capacity prediction | path optimization | which knob fixes which path |
| How much it matters | quantified (W↑, ρ) | identified (which variant) | "variant B is 10× slower AND affects 15% of traffic" |

#### §10 Prior art and related work (preserved in full)
The claimed novelty: **nobody combines complete event capture + queueing theory + process
mining for database wait events**; others do pieces in other domains.
- **§10.1 DB vendors & practitioners.** *Oracle (closest analog):* **Cary Millsap /
  Method R** — M/M/m models on 10046 trace data for response-time decomposition (*Optimizing
  Oracle Performance*, O'Reilly 2003; shared insight: Oracle "waits" are syscall durations =
  sojourn time, not queueing delay); **Craig Shallahamer / OraPub** — OR queueing theory on
  v$ views for capacity forecasting (*Forecasting Oracle Performance*); **Tanel Poder** —
  wait-chain analysis (`ash_wait_chains.sql`), empirical/forensic; Oracle ASH samples at
  1 Hz → loses the transition sequences PM needs. *SQL Server:* `sys.dm_os_wait_stats`
  decomposes `signal_wait_time` (run-queue queueing) from resource wait — implicit QT, but
  aggregate counters only. *Baron Schwartz / VividCortex* (now SolarWinds DPM) — the only
  commercial DB-monitoring tool publicly using QT as a design principle (Little's Law + Neil
  Gunther's USL; ebook *Essential Guide to Queueing Theory*). *PostgreSQL:* pg_wait_sampling
  (Postgres Professional, 100 Hz sampling); pganalyze, PoWA — dashboards over
  sampled/aggregate data, no mathematical modeling. *Cloud (RDS Performance Insights, Azure
  Intelligent Insights, Google Cloud SQL):* ML anomaly detection vs baselines, no QT/PM,
  1 s granularity too coarse.
- **§10.2 Academic.** *Queueing models for DBs:* **Rasha Osman** (QuePED, 2011) — DB tables
  as FCFS service centers in a queueing network, validated on TPC-C ~10% error; **Colombo &
  Ardagna** (2012) — survey incl. Queueing Petri Nets (QPNs); **DBSeer** (Barzan Mozafari,
  U Michigan) — statistical regression for DB performance prediction. *PM on system traces
  — a gap:* almost all PM targets business processes (hospitals, call centers); nobody has
  applied alpha/inductive miner or conformance checking to DB wait-event traces; a 2021
  paper addresses system logs lacking "case IDs" (exactly what EXEC_START/END solve); a 2025
  paper applies Petri nets to microservice request traces for anomaly detection. *Queue
  Mining — the direct QT+PM intersection:* **Arik Senderovich et al.** (Technion, CAiSE
  2014) founded "queue mining" (QT as the math basis within a PM framework to predict
  delays; applied to hospitals/call centers at second-to-minute resolution) — the closest
  existing work; math transfers directly, but ns DB traces vs minute-scale business
  processes differ fundamentally.
- **§10.3 Adjacent fields.** **Brendan Gregg / USE Method** — connects systems performance
  to QT (disk as M/M/1), BPF tracing; **Mor Harchol-Balter** (CMU) — *Performance Modeling
  and Design of Computer Systems: Queueing Theory in Action* (Cambridge 2013); **Neil
  Gunther / USL** — `C(N) = N / (1 + a(N−1) + bN(N−1))`, contention (a) + coherency (b),
  directly fits the throughput-vs-connections curve; **semiconductor wafer fabs** — most
  mature QT for complex multi-step re-entrant flows (MVA widely used; structurally like
  backends cycling through wait states; math transfers, timescales don't); **distributed
  tracing (OpenTelemetry/Jaeger/Zipkin)** — spans forming DAGs; a 2025 paper derives
  semi-Markov models from traces for latency prediction, but the community is mostly
  visualization, not modeling.
- **§10.4 Landscape comparison** (preserved):

| Capability | Oracle 10046 | Oracle ASH | pg_wait_sampling | VividCortex | pg_wait_tracer |
|---|---|---|---|---|---|
| Resolution | μs | 1 s sample | 10 ms sample | 1 s | **ns** |
| Capture | per-session | sample | sample | sample | **every transition** |
| Transitions | No | No | No | No | **Yes** |
| Cross-session | No | Yes | Yes | Yes | **Yes** |
| QT applied | Millsap (ext) | No | No | Yes (Little's, USL) | **Planned** |
| PM applicable | theoretically | No (no sequence) | No (no sequence) | No | **Yes** |

  The combination of complete capture + ns resolution + transition sequences +
  cross-session coverage is claimed unprecedented — the only DB-world dataset where
  classical PM algorithms *and* per-event QT are both applicable.
- **§10.5 Key people & references** (preserved): **Arik Senderovich** (York U — queue
  mining, closest work); **Cary Millsap** (Method R — QT on Oracle traces); **Baron
  Schwartz** (QT+USL for DB monitoring, commercial); **Brendan Gregg** (USE Method, BPF,
  systems QT); **Mor Harchol-Balter** (CMU — QT textbook); **Wil van der Aalst** (RWTH
  Aachen — father of process mining); **Neil Gunther** (USL throughput curve); **Rasha
  Osman** (Queueing Petri Nets for DB contention).

---

### Feature backlog: still-open

Everything in this chapter that is **not** shipped, in one list:

**Analysis-view UIs (endpoint/compute done, visualization deferred — the REWORK B6 bucket):**
- **Wait-pattern fingerprinting UI** (#4) — `handle_fingerprints` has no front-end;
  "pattern changed" indicator + diff view LEFT.
- **Burst-detection AAS annotations** (#6) — burst compute exists; the clickable AAS
  annotation markers → drill-to-timeline UI LEFT.
- **Lock-chain UI** (#7) — `handle_lock_chains` has no Locks view/tab.
- **Cross-session interference UI** (#8) — `handle_interference` has no report view.
- **REWORK B6 net-new views** (⬜ not started; the T1/T3 gate is met — now sequenced as **Track U Phase U3**, this Part):
  per-execution **waterfall** (10046 view), **latency scatter** (with server-side density
  downsampling), **transition matrix** (Sankey's scalable sibling / toggle).

**CLI redesign — open / departed:**
- **§17 Query Plan Identification (`st_plan_id`, PG18+)** — fully OPEN; no `plan_id` code
  exists. (Full execution-plan capture deemed infeasible passively.)
- **§9 `--format json` / `--format csv`** — **dropped**; tokens parse but no formatter.
  Reinstate only if a CLI-native JSONL/CSV is wanted (structured JSON already ships via the
  web/cJSON protocol). No `output_json.c` / `output_csv.c`.
- **§14 `.pgwt` snapshot recording** — **superseded** by daemon `.trace.lz4` + `--replay`;
  the bespoke format won't be built. `recording.c/.h` never created.
- Active-view `--show-query` column and query_event/session-detail query-text columns —
  capture ships (`query_text.c` → `query_texts.jsonl`, surfaced in the web queries view);
  the specific CLI columns are the remaining thin wiring.
- **Prometheus exporter** — PARKED future consumer (metric names already stable).

**Phase-5 ML:**
- **HMM anomaly detection** (#9) — LEFT / research (deferred; DEVELOPMENT_PLAN 14.6). The
  shipped `src/anomaly.c` is an unrelated AAS-baseline + Lock-fraction *escalation trigger*,
  not this HMM.

**The entire Queueing-Theory / Process-Mining research program (`QT_PM_ANALYSIS.md`) —
LEFT / research, except §3.2 Discovery (transitions + variants) which is DONE:**
- QT metrics (§2 / §7.1): N, λ, X, W, exact sojourn CDF, **sojourn-shape classification**
  (exponential/bimodal/heavy-tail/deterministic/shifted), adaptive windowing, **W(N)
  regression** (knee→c, intercept→Ws), **Little's-Law stationarity R(t)**, throughput-
  saturation (λ vs X).
- PM **conformance** (§3.3 / §7.2): baseline models, fitness + precision, structural-change
  detection.
- PM **enhancement** (§3.4): sojourn overlay on the model, "bottleneck is a path" root-cause
  attribution.
- **S1–S4 signal engine** (§4) and the **anomaly-findings endpoint** (§7.3).
- **Investigation Canvas** (§6): 3 layers, evidence chains, SIGNAL→SCOPE→MECHANISM→ROOT
  CAUSE→ACTION pattern, **rule engine** (~15–25 rules) + guided exploration.
- **MVA prediction endpoint** (§7.4) and the Layer-3 what-if predictor.
- Richer **process discovery** beyond DFG/variants (Heuristics/Inductive Miner; sequence /
  XOR / AND / loop structure).
- (Research phases §8: QT metrics → anomaly → conformance → rules → MVA, in that order.)

### Community-proposed features: span tracing, waterfall UI, OTel export, plan-node attribution

Proposed by **@AesaKamar** in PRs **#33** (span tracing + parallel-worker waterfall UI)
and **#34** (superset: + plan-node algebra), 2026-07-02/04. Evaluated 2026-07-28 with a
four-slice review (BPF/attach, parser/server, web/tooling/tests, trace-format). The PRs
forked pre-Trust-Milestone (84 commits behind; 12-file conflicts; as a raw diff they
revert T5/T6/CAP/PIE fixes and collide with the v3 trace format), so they cannot merge
as-is — but the **ideas are adopted** here, with the review findings preserved as the
constraints any implementation must satisfy.

**F-C1. Traceparent / sqlcommenter capture — ADOPT (small).**
Parse W3C `traceparent` from SQL comments to link DB waits to app-side distributed
traces. The PR's mechanism is right: parse at query-text capture time, store
`trace_id`/`parent_span_id` as extra JSONL fields in `query_texts.jsonl` — no hot path,
no trace-format bump; and its parser is memory-safe (55-char remain guard). Deltas
required: accept the **quoted** sqlcommenter form `traceparent='00-…'` (real OTel
SQLCommenter output; the PR parses only unquoted `:`/`=`), keep master's DUR-4 append +
DUR-10 0640 perms (the PR predates them), reject all-zero ids (W3C invalid), and bound
the rescan cost (the PR's loop is O(n²) on crafted text).

**F-C2. Per-execution waterfall view + `leader_pid` grouping — ADOPT (medium).**
Gantt of one execution: leader + parallel-worker rows, wait segments colored by class —
the missing "zoom in on this one execution" story (read-only over already-captured
data; `leader_pid` is a +2-line backend_meta addition). Reusable from the PR:
`lib/builders/waterfall.js` is verified CORRECT (class encoding matches the densified
order end-to-end) and `tests/test_data_waterfall.py` is a real deterministic test.
Deltas required: escape every interpolation (the PR's view has stored-XSS via
`innerHTML` for query_text/db/user/parent_span_id — it never imports `esc`); no
hardcoded `:8385` WS redirect (master serves HTTP+WS on one port; the hack breaks the
default deployment); drop the `?v=5` cache-bust import; server side needs a
`pt_map_clear` freeing per-entry `nodes` (the PR leaks them on every `info` refresh)
and ONE query_id string representation (the PR splits signed `top_queries` %lld vs
unsigned `execution_detail` %llu — filter round-trips break for high-bit ids;
`parse_filters` must use strtoull).

**F-C3. OTel/OTLP export tool — ADOPT (small).**
Offline `tools/export_otel.py`: finished trace → OTLP JSON for Jaeger/Tempo; zero
capture risk; pairs with F-C1 to join app↔DB traces. Deltas required: the wait-class
array must follow the densified order (CPU, IO, Lock, LWLock, IPC, Client, Timeout,
BufferPin, Activity, Extension — the PR's order mislabels 7 of 10 classes); worker
CPU-gap spans must use the worker's own start/end, not the leader's (the PR emits
children outside their parent — invalid OTLP nesting); 64-bit int attributes as strings
per proto3-JSON; validate hex ids before emitting.

**F-C4. Plan-shape sidecar (once per query_id) — ADOPT IDEA, REDESIGN (medium).**
At the existing `ExecutorStart` uprobe, walk `PlannedStmt→Plan` once per NEW query_id
(bounded) and emit a `plan_trees.jsonl` sidecar `{q, nodes:[{id,type,…}]}` so the UI
shows the plan shape next to the wait profile. The PR's PG14–17 offsets verified
correct against real headers (`Plan.plan_node_id`@40, `lefttree`@64, `righttree`@72,
sizeof(Plan)=104, `PlannedStmt.planTree`@32). Deltas required: **PG18 `planTree` is 40,
not 32** (PG18 inserted `planId` after `queryId`); NodeTag numbers are per-major
build-generated — store RAW tags and map per-version offline (the PR hardcodes a PG17
table → wrong operator labels on 14/15/16/18, the lock-class-mislabel anti-pattern);
fail-loud offset validation like `pgwt_validate_wait_addr` (the PR walks silently);
loud dedup-cap warnings (the PR's 4096-entry `seen[]` fills silently — the DUR-10
anti-pattern).

**F-C5. Per-wait plan-node attribution — RESEARCH (parked).**
The end goal: ASH-style row-source attribution (which plan node was running during each
wait). The PR's mechanism is REJECTED: uprobe+uretprobe pairs on 19 per-tuple `Exec*`
functions at pid=-1 cost ~2 kernel traps per tuple instance-wide; uretprobes are
skipped by `elog(ERROR)` `siglongjmp` unwinds → the node stack desyncs permanently →
cross-query misattribution (fail-loud violation); and the PR's `va>0x400000` offset
heuristic never fires on PIE builds (PGDG/EL9) — master's `pgwt_vaddr_to_file_offset`
is the fix it reverted. Candidate mechanisms (unvalidated): (a) `bpf_get_stackid` at
the existing watchpoint fire → symbolize offline to the `Exec*` frame → active node
TYPE per wait, zero new probes; (b) uprobe `ExecProcNodeFirst` only — PG swaps the
ExecProcNode pointer after the first call, so it fires once per node instance →
node-start timeline at O(plan nodes); (c) no capture at all — post-hoc join with
`auto_explain` per-node timings. Trace-format constraint for any exact variant: master
v3 owns transitions column 7 (`cpu_ns` varint, frozen in `golden/rev3`); an
`active_node_id` must be a NEW column 8 under **v4 = "cpu_ns AND active_node_id"**,
with the range reader gate and a `rev4/` golden fixture (the PR's v3/v4 redefinition is
byte-incompatible both directions).

### Track U — UI: the OEM loop + instrument (consolidated plan)  **[COMPLETE 2026-08-09 — U0 ✅ (#59), U1 ✅ (#60), U2a ✅ (#62), U2b ✅ (#64, gate GREEN), U2 ✅ (#66), U3 ✅ (#67), U4 compare mode ✅ (#72). Track U shipped end to end; otherwise only the parked list]**

**Goal:** the full ASH investigation loop — notice → localize (brush) → attribute
(re-rank) → isolate (drill) → inspect → compare → share — with instrument-grade
interaction on the AAS surface: continuous cursor-anchored zoom/pan over a
client-side camera + multi-resolution strip cache (the TradingView/Perfetto
model; the incumbents OEM ASH / RDS PI are request/response dashboards, so this
is how to surpass them, not the entry price). **Evidence docs** (analysis lives
there, the PLAN lives here): `docs/VISUALIZATION_REVIEW.md` (findings P1–P11,
the 7-word defect decoder, non-issues) and `docs/INSTRUMENT_ARCHITECTURE.md`
(camera primer, paradigm analysis, measured ECharts pipeline floor, costed
paths, §6 amendments). Renderer verdict: ECharts stays for secondary views;
the AAS pane is expected to move to a uPlot substrate behind a written gate
(U2b). Subsumes the REWORK **B6** bucket (waterfall / scatter / matrix → U3).

**Phase U0 — Quick wins  ✅ [DONE — PR #59, 2026-07-31]** (validation: node 144/144, Playwright UI 209/209 incl. the new error-card test, chaos 25/25, snapshots 13/13 — no baseline regen; `?ws=` override gated to loopback per review; U1 follow-ups noted: stricter [pgwt] default, empty-state fixture cells, VERSION chromium-build confirmation)
- **Error visibility (review P1):** `console.error` at the four swallow sites
  (`lib/view-manager.js:112-121`, `app.js:333-334`, `app.js:243-246`); per-pane
  error card (view, command, server `code`/`hint`, retry) for build/mount throws
  and `transport:false` envelopes — including rendering the DUR-9
  `window_too_large` refusal (today silently dropped; the string appears nowhere
  in `web/static`); null-guard `fmtPct`/`fmtAas`/`fmtCount`/`pctBar` to "—";
  re-scope the Playwright console-error guard to an allowlist.
- **Mock reachability:** `?ws=` param or `ws_url` in `/session`; delete the five
  copy-pasted FakeWS monkey-patches (`test_web_ui.py:1231/1519/1595`,
  chaos `:576`, snapshots `:141-152`).
- **Snapshot provenance:** pin `playwright==X.Y.Z` in both CI jobs; commit
  `tests/web_snapshots/VERSION`.
- **run_all.sh:** run the `node --test` layer (skip without node locally, fail
  in CI); local pixel snapshots behind `PGWT_RUN_SNAPSHOTS=1`.
- **Misplaced marks (P6):** burst-marker containing-bucket clamp
  (`floor((ts−peaks[0].t)/bucket_ns)`, clamped) + last-bucket unit test (fix
  `concurrency.test.mjs:39-47` which pins the off-by-one); timeline `clip:true`
  + clamp `[s,s+d]` to `[from,to]` in the builder (raw start kept for tooltip)
  + pre-window-start test.
- **Escalation markLine** clamp/axis pin (it has never rendered) + flip
  `fidelity.test.mjs:118,147`.
- `animation:false` in every builder, pinned by one assert each.
- **Live-vs-investigation (P4):** pause live on drill/breadcrumb exactly as
  zoom does; unify the Live span (`app.js:653` vs `:211`); pauses-live as a
  view property (wire the unused `opts.userInitiated` hook,
  `view-manager.js:82`).
- Suite hygiene: fix the tautological `check(x or True)` (`test_web_ui.py:638`);
  port-poll instead of the 1.5 s sleep; on-failure screenshot + console tail as
  CI artifacts.

**Phase U1 — Seeing infrastructure + chart identity  ✅ [DONE — PR #60, 2026-07-31]** (49-cell gallery; First-catch: the gallery exposed that stacked areas on a value x-axis only ever painted series[0] — fixed in U2a; 13 tight-threshold cell baselines; identity cluster shipped incl. mutation-tested legend coverage)
- **Fixture gallery** `web/static/dev/gallery.html` + `tests/fixtures/*.mjs`:
  every pure builder × curated states (empty, single-point, dense 300
  buckets/50 PIDs/16+ events, degenerate/all-zero/unicode/hostile-SQL,
  sampled/mixed/escalated fidelity) + recorded **live-tick replay** with a play
  button + recorded **gesture scripts** (this is also U2b's gate harness; the
  Node-SSR pipeline floor 8.9→42.5 ms p50/step at 300→2400 buckets is the
  pre-registered baseline).
- `docs/VISUAL_CHECKLIST.md` — the 7-word decoder (STABILITY / CONTINUITY /
  ALIGNMENT / OCCLUSION / SEMANTICS / HIERARCHY / FEEDBACK) keyed to gallery
  cells.
- **Identity cluster (P2):** one color service (event → deterministic tint
  within its class hue; retire `EVENT_PALETTE`); stable stack order by event
  identity; legend selection keyed by series **name** with reset on set change;
  all annotations moved to a dedicated silent annotation series (fixes the
  vanish-on-hover, empirically verified); flip `aas.test.mjs:70, 48-56` which
  pin the bugs.
- Gallery-cell snapshots with tight thresholds (~8/0.002) — first visual
  coverage of drilled/sorted/error/fidelity-variant states.

**Phase U2 — Chassis + drill wiring + URL state  ✅ [DONE — PR #66, 2026-08-04]** (seven wires + URL hash state + yMax/heatmap/DFG policies + table honesty; adversarial review found 3 blocking/4 high/5 medium — all fixed with per-finding pins, state-unity enforced architecturally via the single mutateFilters gate; node 282/282, Playwright 338/338, OEM loop 12/12)
- **Chassis:** one themed base option (kills the triplicated chrome);
  instance-reuse mount helper with the animation policy (the
  `views/histogram.js:64-65` pattern everywhere — ends dispose/re-init churn,
  P5); ms time coordinates at the builder boundary; click→intent plumbing.
- **The seven drill wires (P3):** (1) AAS `chart.on('click')` → class drill
  (event drill in breakdown mode); (2) burst/peak rows → `zoomTo(burst ± pad)`;
  (3) session-timeline zoom via `attachSelection` refetch; (4) heatmap cell →
  event+time drill; (5) DFG node → event drill (server emits `event_id` in node
  JSON — one line at `server.c:2563`); (6) events percentile cells → histogram
  tab; (7) persistent filter bar + tab badges; histogram dropdowns write
  through to FilterStack (delete the second filter state).
- **URL state (P9):** `{tab, from, to, live|span, filters, sort}` in the hash
  (~50 lines; ns as strings; restored `live=1` re-anchors to NOW).
- **Axis/anchor policies:** yMax CPU-line cap + hysteresis, window quantization
  (merges into the camera's power-of-2 quantization); heatmap `log1p` +
  single-hue ramp + sticky visualMap max + open-dropdown guard (P8).
- **Fidelity beyond the AAS chart (P10):** per-pane fidelity badges;
  percentile-basis footnotes ("exact-captured only: N of M"); "N others"
  truncation rows.
- DFG: persisted instance, rAF-throttled slider, preserved drag positions,
  layout re-run on resize (P5); P11 minors opportunistically.
- **Color-service adoption backlog (U1 review F4):** remaining flat-class-hue
  sites — `builders/transitions.js:69,152` (DFG nodes/variant steps; flip
  `transitions.test.mjs:70` and regenerate `transitions_dfg.png`) and
  `builders/table-configs.js:31,64` (overview/events pctBar fills) — adopt
  `eventColor()` so "same event = same color" holds in every view (hue-family
  is already consistent; this is tint-level completeness).
- **Server backlog (small):** closed-data watermark + clock-generation stamp in
  responses (`server.c:1168-1184` re-anchoring hazard, `server.c:1070-1077`
  escalation window); wire compression (permessage-deflate in `bridge.go` or
  `ssh -C`; 4–5× measured); strip channel through `transport.send` bypassing
  the epoch-discard (`view-manager.js:109`) for cache fills. Explicitly
  deferred: FIFO cancellation/priority (`server.c:3300-3313`) — compute is
  3–22 ms; revisit only if prefetch traffic materializes.

**Phase U2a — Camera-lite: the instrument on ECharts  ✅ [DONE — PR #62, 2026-07-31]** (camera+stripcache pure modules 35 tests; wheel-zoom/shift-drag-pan, zero round-trips in-gesture; type:'time'+useUTC fixes First-catch; gate measured RED → U2b scheduled)
- `lib/camera.js` (~150–200 LOC, pure, Node-tested): TimeWindow,
  cursor-anchored zoom math, power-of-2 LOD quantization, **follow/detached
  state machine** with tested transitions (pan detaches — the 5 s tick must
  never re-anchor the user; follow never shows a stale right edge; restored
  live re-anchors to NOW).
- `lib/stripcache.js`: strips keyed `{from,to,resolution,generation}`,
  stale-but-useful retention.
- AAS pane: ms value-axis pinned to `[from,to]`; preload strip beyond the
  visible window; `dataZoom:{type:'inside'}` wheel+drag with
  `filterMode:'none'`; settle-refine (debounced ~150 ms) fetches the finer
  strip and swaps it under the fixed axis; live tick stays poll-and-replace 5 s.
- Measure the gate via U1's recorded gesture scripts at ≥1200 cached buckets ×
  18 series, in a real browser.

**Phase U2b — uPlot AAS instrument pane  ✅ [DONE — 2026-08-04, gate GREEN]**
- **Gate (written, pre-registered): TRIPPED RED 2026-07-31 — begin (b)
  immediately.** Measured on the real app in real Chromium (U2a harness
  `tests/test_gesture_gate.py`, 3 runs, 1258-bucket × 16-series cached strip,
  40 wheel-zoom + 20 shift-drag-pan steps each, input-arrival →
  chart-'finished'): combined gesture-to-paint **p50 ≈ 43 ms, p95 = 61.2 /
  59.4 / 67.6 ms** — ~2× the 33 ms begin-immediately line, ~4× the 16.7 ms
  60 fps bar (secondary action→paint p50 ≈ 16 ms matches the pre-registered
  SSR floor 20.4/32.8, confirming the ECharts pipeline as the budget item).
  Verdict per the rule: **U2b is scheduled, no longer gated.**
- **Outcome — measured 2026-08-04 (the same harness, extended to an A/B: both
  renderers in one invocation, same mock/strip/steps, 3 runs per renderer):
  uPlot combined gesture-to-paint p50 ≈ 2.6–3.4 ms, p95 = 5.4 / 7.0 / 5.2 ms;
  the retained ECharts path re-measured p50 ≈ 46–48 ms, p95 = 65.5 / 69.4 /
  81.0 ms in the same sitting (a first sitting the same day: uPlot p95
  5.3/5.3/5.2 vs ECharts 61.7/65.6/69.6 — preserved under `history`; both
  sittings GREEN). Number → rule → action: worst uPlot run p95
  7.0 ms ≤ 16.7 ms → GREEN — target met with ~2.4× headroom (~9–12× the
  ECharts pipeline); no tuning pass, bespoke-canvas fallback unexercised.** Probe
  equivalence is documented in the harness: uPlot 1.6.32 commits via
  queueMicrotask — scale mutation and the full synchronous canvas write are
  atomic inside the commit, and pending scales are invisible before it, so a
  wrapped rAF callback observing the moved x scale can never precede the
  canvas write (the double-rAF chain queued in the camera dispatch guarantees
  a same-frame callback — load-bearing, not merely a fallback): the same
  pre-compositor canvas-written instant ECharts' 'finished' reports. Numbers
  live in `tests/results/gesture_gate.json` (the pre-swap RED record is
  preserved under `history`); re-runnable via `PGWT_RUN_GESTURE_GATE=1`.
- **What shipped:** vendored uPlot 1.6.32 (MIT, sha256-pinned provenance in
  `web/static/vendor/README.md`); `lib/uplot-aas.js` pure module (official
  stack.js recipe, hidden-series slot stability, `hitTest`, honesty overlays
  as absolute camera-space geometry — 33 Node tests incl. cross-renderer
  parity: identical names/colors/yMax/window/axis-label text/overlay
  coordinates vs `buildAasOption`); `views/active.js` mount swap behind a
  `?renderer=echarts|uplot` seam (default uplot; ECharts kept fully intact as
  A/B baseline + rollback). The camera is now GENUINELY authoritative —
  closes U2a's "authoritative by convention" caveat: our handlers mutate the
  camera (wheel = cursor-anchored zoom, shift+drag = pan, plain drag = brush,
  dblclick = zoom-out — language unchanged) and the renderer draws FROM
  camera state via one rAF-coalesced `u.setScale` over resident data; strip
  swaps ride `setData(…, false)` under the fixed window. Honesty overlays
  redraw against the CURRENT x scale on every draw (rects clamp, lines drop
  but never move into view) — verified at ~22× zoom-in, ~9× zoom-out, and
  panned fully off-data. Gallery gained 6 `uplot-aas` twin cells; the AAS
  pane baselines regenerated + 6 cell baselines added (the priced ~2–4 day
  rebaseline; residual diffs were exactly the predicted tick placement/font
  raster/antialiasing — colors, alpha, and overlay coordinates parity-pinned).
- Other triggers, any one suffices (recorded for completeness; all fired-or-
  future ones remain future): raw-**sample** attribution (needs
  client-resident raw samples — a data-residency milestone favoring an owned
  renderer); single-camera **compare** overlay design; live cadence ≤1 s
  (ECharts has no stacked-area append path — `appendData` is scatter/`lines`
  only).
- Substrate was: **uPlot** (~500–900 owned LOC; official 159-LOC `stack.js`
  recipe; Grafana precedent; the `ROADMAP_AND_STATUS.md:202` pre-scoped
  adapter swap) — landed at ~560 module LOC. Fallback bespoke canvas
  (~2.5–4k LOC / 5–8 weeks; calibration table in INSTRUMENT_ARCHITECTURE.md
  §4b) not needed. Discarded on swap, as priced: camera↔dataZoom sync glue
  stays only on the `?renderer=echarts` path + one AAS pixel-rebaseline.
- **Guardrails (every phase):** honesty overlays (sampled shading, escalation
  bands, N-CPUs line, DUR-9 refusal regions) are camera-space view-model
  geometry under EVERY renderer, never renderer decorations; camera/cache/
  stacked-geometry stay pure Node-tested modules; U0/U1 (error visibility +
  identity) land before any instrument work is judged.

**Phase U3 — B6 views on the chassis  ✅ [DONE — PR #67, 2026-08-04]**  *(subsumes REWORK B6; staged delegation — server → gate → UI → adversarial review, 3 blocking/5 high found+fixed with pins → supervisor probes; ships the per-execution 10046 waterfall, latency scatter, transition matrix + executions/execution_detail/exec_scatter commands, leader_pid in backends.jsonl, honest EXACT refusals end to end)*

**Phase U4 — Compare mode  ✅ [DONE — PR #72, 2026-08-09; docs/COMPARE_MODE_DESIGN.md]** *(build → 3-Opus adversarial review [8 findings, all fixed+pinned] → supervisor caught a 9th [gitignore `*-diff.png` collision on the ghost-diff cell name; renamed `compare-ghostdiff` + added a reserved-suffix guard] → gate. 340/340 node unit, 422/422 Playwright, 4 CI-generated compare baselines committed. D1–D7 shipped UI-only, zero server changes; the diff-lane per-window autoscale is a confirmed intentional qualitative lane. Deferred as designed: pinned fixed baselines v1.1, compare on EXACT-required views, server-side compare command v2.)*
The last OEM capability (AWR-Compare-Periods class): baseline-as-OFFSET
(compare survives live follow — "now vs yesterday, continuously"; one camera
+ one scalar, no second-camera sync), ghost-outline + per-class diff strip
on the AAS pane, delta-first tables (A|B|Δ|× with honest new/gone labels and
a change-floor truncation row), per-window fidelity badges with a
mixed-evidence warning chip (flagged, never silent, never blocked),
`cmp=1&b_off=` in the URL hash. Zero server changes in v1 (B rides the same
strip cache and commands). EXACT-required views excluded from v1; pinned
fixed baselines and a server-side compare command recorded as v1.1/v2.
Build = the established triangle, UI-only, ~600–900 LOC.
- Per-execution **waterfall** (the 10046 view; same custom-series technique as
  the session timeline) — **zoom + click-to-inspect in its definition of
  done**; **transition matrix** (heatmap builder as template; log/piecewise
  visualMap from day one); **latency scatter** (server-side density capping;
  ECharts large-mode + log y; 2D box-select by extending `pixelRangeToTime`).
- **Compare mode** — the one genuinely new capability (every ASH peer ends
  here); design after URL state lands; two cameras over one strip cache is
  substantially cheaper than two dashboards.

**Parked / acknowledged (decisions, not omissions):** accessibility is zero
(no aria/roles/keyboard; mouse-only drill); no `@media` handling; long-tab
memory soak (add to chaos when instance reuse lands — a leaked chart then
lives forever); CLI (`src/output.c`) presentation-semantics audit against the
web's once pinned; UTC/local toggle; `saveAsImage` export once a toolbox
exists.

**What NOT to do (from both docs, binding):** no wholesale library migration
and no renderer trials outside the builder/mount seam; don't slim the vendored
bundle; don't hand-roll a DFG renderer; no SPA framework/router beyond hash
state; no preemptive table virtualization; no `appendData` streaming pursuit;
no multi-value filter algebra yet (chips over FilterStack cover the 80% case);
no batch visual fixes before the gallery exists; **never put the server inside
the pan/zoom gesture loop.**

---

## Housekeeping notes (doc hygiene)

Surfaced while consolidating; worth acting on:

1. **Stale status markers.** The old `TRUST_MILESTONE_PLAN.md` still showed
   `🔄`/`⬜` on phases that had in fact shipped; those have been reconciled to
   real status here.
2. **DoD item 6 is literally unmet.** The Trust Milestone's Definition of Done
   item 6 read: "`run_all.sh --require-live` green **twice** on EL8 **and** EL9
   with **zero** live-section skips." The v0.13 matrix ran **64 pass / 0 fail /
   3 skip** on each platform — the 3 are the established, loud web-UI /
   nested-container / watchpoint skips, not silent gaps. Materially satisfied,
   but the checklist's literal "zero skips" bar is not met. Either accept-and-
   annotate the 3 skips or tighten the bar.
3. **Spec conflict (measured CPU).** `S3_SCHED_SWITCH_CPU.md` §8 said to **drop**
   `wait_gap_cpu_ns_total`, but `T8_MEASURED_CPU_REVISION.md` §3.3 said to
   **keep** it as an observability metric. The code **kept** it (the revision
   decision won). `S3_SCHED_SWITCH_CPU.md` (retained) should be corrected.
4. **Superseded baselines.** Several correctness guarantees the early review
   docs declared "fixed" (state_map GC, JSON escaping, summary/fidelity honesty)
   were later found **re-violated** and re-hardened by the Trust Milestone
   (T4 / T6 / T1). The Part 3 items are historical baselines, not the current
   correctness authority — the Part 2 (Trust) versions are.

## Appendix — where each replaced doc's content now lives

| Replaced doc | Now in |
|---|---|
| `REWORK_PLAN.md` | Part 1 |
| `TRUST_MILESTONE_PLAN.md` | Part 2 |
| `T8_MEASURED_CPU_PLAN.md` | Part 2 (measured-CPU evolution) |
| `T8_MEASURED_CPU_REVISION.md` | Part 2 (measured-CPU evolution) |
| `FUTURE_WORK.md` | Part 2 (deferred / future list) + Executive summary |
| `ROADMAP.md` | Part 3 |
| `DEVELOPMENT_PLAN.md` | Part 3 |
| `REVIEW_AND_PLAN.md` | Part 3 |
| `TRACING_ANALYSIS_PLAN.md` | Part 4 |
| `CLI_REDESIGN.md` | Part 4 |
| `QT_PM_ANALYSIS.md` | Part 4 (research vision) |

## Retained reference/spec docs (not consolidated)

These remain the authoritative deep references for shipped subsystems and are
cross-linked from the relevant Parts above:

- `S3_SCHED_SWITCH_CPU.md` — measured-CPU implementation spec (the shipping design).
- `AAS_SEMANTICS_DECISION.md` — the decomposed-AAS semantic decision (T2).
- `T2_IOWORKER_STUDY.md` — the PG18 io_worker / AAS empirical study.
- `TRACE_FORMAT.md` — on-disk trace/summary format &amp; durability policy.
- `DAEMON_ARCHITECTURE.md` — daemon runtime architecture reference.
- `CHANGELOG.md`, `RELEASING.md`, `README.md`, `INSTALL.md` — release history,
  release checklist, user docs.
