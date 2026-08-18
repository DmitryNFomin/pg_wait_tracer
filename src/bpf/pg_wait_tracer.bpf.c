/* pg_wait_tracer BPF programs — wait event tracing via ringbuf
 *
 * Programs:
 *   on_watchpoint  — fires on PGPROC->wait_event_info write, emits trace event
 *   on_bootstrap   — fires when InitProcess() writes my_wait_event_info pointer
 *   on_fork        — fires on postmaster fork, notifies daemon of new backend
 *   on_exit        — fires on backend exit, closes last interval, notifies daemon
 *
 * Modes (set via lightweight_mode before load):
 *   0 = full trace: emit every event to event_ringbuf (for --trace / recording)
 *   1 = lightweight: accumulate durations in accum_map (for --view, lower overhead)
 */

#ifndef __BPF__
#define __BPF__
#endif
#include "pg_wait_tracer.h"
#include <bpf/bpf_helpers.h>
#include <bpf/bpf_tracing.h>
#include <bpf/bpf_core_read.h>
#include <bpf/usdt.bpf.h>

/* ringbuf submit flags (may not be in all header versions) */
#ifndef BPF_RB_NO_WAKEUP
#define BPF_RB_NO_WAKEUP (1ULL << 0)
#endif

/* ── Constants from userspace (set before load via .rodata) ── */
volatile const u32 target_postmaster_pid = 0;
volatile const u64 my_wait_ptr_addr = 0;
/* PG<17 (MyProc path): my_wait_ptr_addr is the MyProc PGPROC* global, not a
 * uint32* pointing at the field. on_bootstrap reads *MyProc (the backend's
 * PGPROC) and adds this offset to reach wait_event_info. 0 on PG17+ (the
 * global already points at the field). */
volatile const u32 pgproc_wait_offset = 0;
volatile const u64 my_be_entry_addr = 0;  /* address of MyBEEntry global */
volatile const u32 st_query_id_offset = 0; /* offsetof(PgBackendStatus, st_query_id) */
volatile const u32 lightweight_mode = 0;   /* 0=full trace, 1=lightweight (no ringbuf) */
volatile const u32 skip_query_id = 0;      /* 1=skip query_id reads (saves 1 probe_read) */
volatile const u64 debug_query_string_addr = 0; /* VA of debug_query_string global */

/* T8 measured CPU: 1 when the daemon's startup probe confirmed the kernel BTF
 * carries task_struct->se.sum_exec_runtime (docs/ROADMAP_AND_STATUS.md).
 * When set, the watchpoint handler reads that exact accumulator at every wait
 * boundary and emits the per-interval CPU delta in pgwt_trace_event.cpu_ns; 0
 * leaves cpu_ns at 0 and userspace stamps the UNKNOWN sentinel (gap-inference
 * fallback). A CO-RE bpf_core_field_exists() guard makes the read compile and
 * load even where the field is absent, so this is a belt-and-suspenders gate. */
volatile const bool cpu_accounting = 0;

/* The sched_switch link stays loaded so escalation never has a tracepoint
 * attach race, but its hot body is exact-tier-only. Userspace flips this BSS
 * byte before exact preseed/watchpoint arm and clears it after watchpoint
 * disarm. Sampled operation therefore pays one predictable branch, not two
 * state-map lookups on every system context switch. */
volatile bool exact_cpu_active = false;

/* TEST HOOK (PGWT_TEST_NO_SCHED_ONCPU): deterministically reproduce the
 * live-view CPU*=0 straddle flake. When 1, on_sched_switch does NOT open
 * on_cpu_ts for the incoming task, so a backend's on-CPU stretch is opened
 * ONLY by the seed (current_wei==0) or a watchpoint fire. A backend that seeds
 * OFF-CPU (in a real wait at attach, so on_cpu_ts starts 0) and then runs a
 * waitless pure-CPU loop therefore reads live CPU* == 0 without the
 * watchpoint-fire on_cpu_ts open below, and CPU* > 0 with it — on EVERY run,
 * independent of the scheduler. This isolates exactly that fix. Test-only;
 * never set in production (it disables sched-driven CPU accounting). */
volatile const bool test_no_sched_oncpu = 0;

/* Command-open gate (T2, docs/AAS_SEMANTICS_DECISION.md): per-PG-version
 * BackendState enum values for the pgstat_report_activity uprobe. PG13-17:
 * STATE_RUNNING=2, STATE_FASTPATH=4; PG18+ inserted STATE_STARTING so they
 * shift to 3/5. 0 = gate unavailable (unknown version / attach failure):
 * cmd_open then stays 0 and client-backend on-CPU samples are NOT recorded
 * (loud daemon warning — under-count, never the ungated ~3x over-count). */
volatile const u32 bs_state_running = 0;
volatile const u32 bs_state_fastpath = 0;

/* PG13 query attribution (Route B1): standard_ExecutorStart(QueryDesc*) uprobe
 * walks QueryDesc->plannedstmt->queryId (populated by pg_stat_statements' hook)
 * into the state_map query_id slot. 0 => disabled (the PG17+
 * pgstat_report_query_id uprobe is used instead). Offsets are header-derived
 * (postgresql13-devel). */
volatile const u32 pg13_query_attr = 0;          /* 1 = std_ExecutorStart path active */
volatile const u32 pg13_synthetic_attr = 0;      /* 1 = PG13 activity-text seed path */
volatile const u32 pg13_qd_plannedstmt_off = 0;  /* offsetof(QueryDesc, plannedstmt) */
volatile const u32 pg13_ps_queryid_off = 0;      /* offsetof(PlannedStmt, queryId) */
volatile const u32 pg13_qd_sourcetext_off = 0;   /* offsetof(QueryDesc, sourceText) */

/* ── Maps ─────────────────────────────────────────────────── */

/* Per-PID state: last event + timestamp for duration computation.
 * Must be regular HASH (not PERCPU) because a backend can migrate
 * between CPUs — we need the same last_ts regardless of CPU.
 *
 * CAP-1: sized to MAX_BACKENDS — the same capacity as the userspace backend
 * registry. It was 512 (< MAX_BACKENDS < common max_connections): above 512
 * live backends every insert failed silently and those backends NEVER
 * recorded an event. Inserts are now also CHECKED (fail_counters slot
 * PGWT_BPF_FAIL_STATE_MAP + the userspace state_map_full_total counter), and
 * the daemon may shrink the map before load via PGWT_STATE_MAP_ENTRIES
 * (test hook — proves the loud path without 1024 real connections).
 * Memory cost at 1024 entries × ~40B values is ~100KB — negligible. */
struct {
    __uint(type, BPF_MAP_TYPE_HASH);
    __uint(max_entries, MAX_BACKENDS);
    __type(key, u32);
    __type(value, struct pgwt_pid_state);
} state_map SEC(".maps");

/* Stage 3 exact attribution is split by writer ownership. exact_attr_map is
 * BPF-only edge state; exact_seed_map is userspace-only coherent window-start
 * state. exact_config selects the generation and attach boundary used to merge
 * them. This prevents a userspace snapshot from overwriting a newer uprobe
 * edge, including on a command that changes while escalation is attaching. */
struct {
    __uint(type, BPF_MAP_TYPE_HASH);
    __uint(max_entries, MAX_BACKENDS);
    __type(key, u32);
    __type(value, struct pgwt_exact_attr);
} exact_attr_map SEC(".maps");

struct {
    __uint(type, BPF_MAP_TYPE_HASH);
    __uint(max_entries, MAX_BACKENDS);
    __type(key, u32);
    __type(value, struct pgwt_exact_seed);
} exact_seed_map SEC(".maps");

struct {
    __uint(type, BPF_MAP_TYPE_ARRAY);
    __uint(max_entries, 1);
    __type(key, u32);
    __type(value, struct pgwt_exact_config);
} exact_config_map SEC(".maps");

struct {
    __uint(type, BPF_MAP_TYPE_PERCPU_ARRAY);
    __uint(max_entries, PGWT_UPROBE_FIRE_MAX);
    __type(key, u32);
    __type(value, u64);
} uprobe_fire_counts SEC(".maps");

/* Ring buffer for lifecycle events — pollable via epoll */
struct {
    __uint(type, BPF_MAP_TYPE_RINGBUF);
    __uint(max_entries, 64 * 1024);
} lifecycle_rb SEC(".maps");

/* Ring buffer for raw trace events — every wait event transition */
struct {
    __uint(type, BPF_MAP_TYPE_RINGBUF);
    __uint(max_entries, 64 * 1024 * 1024);  /* 64MB */
} event_ringbuf SEC(".maps");

/* Raw first-seen dedup includes the backend identity. A backend's database
 * and user are immutable, so (pid,qid) prevents one context from suppressing
 * another; userspace performs the final (db,user,qid) dedup. LRU eviction is
 * safe because a repeat only produces metadata that userspace drops. */
struct pgwt_seen_query_key {
    u32 pid;
    u32 reserved;
    u64 query_id;
};
struct {
    __uint(type, BPF_MAP_TYPE_LRU_HASH);
    __uint(max_entries, 4096);
    __type(key, struct pgwt_seen_query_key);
    __type(value, u8);
} seen_query_ids SEC(".maps");

/* Accumulator map for lightweight mode: per-event duration totals.
 * Key = wait_event_info (u32), Value = {total_ns, count}.
 * PERCPU to avoid atomic ops and cache-line bouncing in BPF hot path.
 * Daemon reads and zeroes this periodically, summing across CPUs. */
struct {
    __uint(type, BPF_MAP_TYPE_PERCPU_HASH);
    __uint(max_entries, ACCUM_MAP_MAX_ENTRIES);
    __type(key, u32);
    __type(value, struct pgwt_accum_val);
} accum_map SEC(".maps");

/* event_ringbuf drop counter (A2). Single per-CPU u64 bumped when a
 * bpf_ringbuf_output() into event_ringbuf fails (capacity or reservation
 * contention, including NMI context). PERCPU so the hot path stays
 * atomic-free; the daemon sums across CPUs and surfaces it as
 * ringbuf_drops_total on the control socket. */
struct {
    __uint(type, BPF_MAP_TYPE_PERCPU_ARRAY);
    __uint(max_entries, 1);
    __type(key, u32);
    __type(value, u64);
} ringbuf_drops SEC(".maps");

/* Map-insert failure counters (CAP-1/CAP-6). Slots defined in
 * pg_wait_tracer.h (PGWT_BPF_FAIL_*). PERCPU so the hot path stays
 * atomic-free; the daemon sums across CPUs and surfaces them as
 * state_map_full_total / seen_query_ids_full_total on the control socket.
 * A full map must NEVER be silent: it means backends that record nothing
 * (state_map) or query text that is never captured (seen_query_ids). */
struct {
    __uint(type, BPF_MAP_TYPE_PERCPU_ARRAY);
    __uint(max_entries, PGWT_BPF_FAIL_MAX);
    __type(key, u32);
    __type(value, u64);
} fail_counters SEC(".maps");

static __always_inline void count_map_fail(u32 slot)
{
    u64 *c = bpf_map_lookup_elem(&fail_counters, &slot);
    if (c)
        (*c)++;
}

/* Emit a trace event to event_ringbuf, counting drops on failure.
 * bpf_ringbuf_output() returns 0 on success, negative when reservation/output
 * fails; an NMI-context reservation can lose spinlock contention even while
 * the ring has free capacity. */
static __always_inline void emit_event(const struct pgwt_trace_event *evt)
{
    long rc = bpf_ringbuf_output(&event_ringbuf, (void *)evt, sizeof(*evt),
                                 BPF_RB_NO_WAKEUP);
    if (rc) {
        u32 zero = 0;
        u64 *drops = bpf_map_lookup_elem(&ringbuf_drops, &zero);
        if (drops)
            (*drops)++;
    }
}

/* ── Helpers ──────────────────────────────────────────────── */

/* Read query_id using cached be_entry pointer (1 probe_read instead of 2).
 * Falls back to double-deref if cache is empty. */
static __always_inline u64 read_query_id_cached(u64 cached_be_entry)
{
    if (!st_query_id_offset) return 0;
    u64 be_entry = cached_be_entry;
    if (!be_entry) {
        if (!my_be_entry_addr) return 0;
        bpf_probe_read_user(&be_entry, sizeof(be_entry),
                             (void *)my_be_entry_addr);
        if (!be_entry) return 0;
    }
    u64 qid = 0;
    bpf_probe_read_user(&qid, sizeof(qid),
                         (void *)(be_entry + st_query_id_offset));
    return qid;
}

/* Resolve and return PgBackendStatus* for caching in state_map. */
static __always_inline u64 resolve_be_entry(void)
{
    if (!my_be_entry_addr) return 0;
    u64 be_entry = 0;
    bpf_probe_read_user(&be_entry, sizeof(be_entry),
                         (void *)my_be_entry_addr);
    return be_entry;
}

static __always_inline void count_uprobe_fire(u32 slot)
{
    u64 *count = bpf_map_lookup_elem(&uprobe_fire_counts, &slot);
    if (count)
        (*count)++;
}

static __always_inline struct pgwt_exact_config *exact_config(void)
{
    u32 zero = 0;
    return bpf_map_lookup_elem(&exact_config_map, &zero);
}

/* Generation zero is the permanent degraded-layout sampled fallback. A nonzero
 * exact generation can be quiesced before its links are detached, giving
 * userspace a finite ring-buffer boundary while preserving the edge+seed state
 * needed to close intervals at the exact window end. */
static __always_inline bool exact_admission_open(void)
{
    struct pgwt_exact_config *cfg = exact_config();
    return !cfg || cfg->generation == 0 || cfg->admitting;
}

/* Resolve current exact attribution without ever copying a seed over an edge.
 * Generation 0 is the permanent degraded-layout fallback: the continuously
 * attached edge map is authoritative. For a nonzero exact generation, start
 * from its coherent seed and override each field independently only with an
 * edge from the same generation stamped at/after the attach boundary. */
static __always_inline void resolve_exact_attr(u32 pid, u64 *query_id,
                                               u16 *cmd_open,
                                               u16 *query_quality)
{
    struct pgwt_exact_attr *edge =
        bpf_map_lookup_elem(&exact_attr_map, &pid);
    struct pgwt_exact_config *cfg = exact_config();
    struct pgwt_exact_seed *seed =
        bpf_map_lookup_elem(&exact_seed_map, &pid);
    u32 packed = 0;
    pgwt_exact_merge_attr(cfg, edge, seed, query_id, &packed);
    *cmd_open = (u16)(packed & 0xffff);
    *query_quality = (u16)(packed >> 16);
}

static __always_inline void note_query_edge(u32 pid, u64 query_id, u64 now)
{
    struct pgwt_exact_config *cfg = exact_config();
    u64 generation = cfg ? cfg->generation : 0;
    struct pgwt_exact_attr *edge =
        bpf_map_lookup_elem(&exact_attr_map, &pid);
    if (edge) {
        edge->query_generation = generation;
        edge->query_edge_ts = now;
        edge->query_id = query_id;
        edge->query_quality = query_id ? PGWT_QUERY_QUALITY_REAL
                                       : PGWT_QUERY_QUALITY_NONE;
        return;
    }
    struct pgwt_exact_attr initial = {
        .query_generation = generation,
        .query_edge_ts = now,
        .query_id = query_id,
        .query_quality = query_id ? PGWT_QUERY_QUALITY_REAL
                                  : PGWT_QUERY_QUALITY_NONE,
    };
    bpf_map_update_elem(&exact_attr_map, &pid, &initial, BPF_ANY);
}

static __always_inline void note_cmd_edge(u32 pid, u16 cmd_open, u64 now)
{
    struct pgwt_exact_config *cfg = exact_config();
    u64 generation = cfg ? cfg->generation : 0;
    struct pgwt_exact_attr *edge =
        bpf_map_lookup_elem(&exact_attr_map, &pid);
    if (edge) {
        edge->cmd_generation = generation;
        edge->cmd_edge_ts = now;
        edge->cmd_open = cmd_open;
        return;
    }
    struct pgwt_exact_attr initial = {
        .cmd_generation = generation,
        .cmd_edge_ts = now,
        .cmd_open = cmd_open,
    };
    bpf_map_update_elem(&exact_attr_map, &pid, &initial, BPF_ANY);
}

static __always_inline void note_phase_edge(u32 pid, u32 marker, u64 now)
{
    struct pgwt_exact_config *cfg = exact_config();
    u64 generation = cfg ? cfg->generation : 0;
    struct pgwt_exact_attr *edge =
        bpf_map_lookup_elem(&exact_attr_map, &pid);
    if (!edge) {
        struct pgwt_exact_attr initial = {
            .phase_generation = generation,
        };
        bpf_map_update_elem(&exact_attr_map, &pid, &initial, BPF_ANY);
        edge = bpf_map_lookup_elem(&exact_attr_map, &pid);
        if (!edge)
            return;
    }
    if (edge->phase_generation != generation) {
        edge->phase_generation = generation;
        edge->phase_flags = 0;
        edge->plan_start_ts = 0;
        edge->exec_start_ts = 0;
    }
    if (marker == PGWT_MARKER_PLAN_START) {
        edge->phase_flags |= PGWT_EXACT_PHASE_PLAN;
        edge->plan_start_ts = now;
    } else if (marker == PGWT_MARKER_PLAN_END) {
        edge->phase_flags &= ~PGWT_EXACT_PHASE_PLAN;
        edge->plan_start_ts = 0;
    } else if (marker == PGWT_MARKER_EXEC_START) {
        edge->phase_flags |= PGWT_EXACT_PHASE_EXEC;
        edge->exec_start_ts = now;
    } else if (marker == PGWT_MARKER_EXEC_END) {
        edge->phase_flags &= ~PGWT_EXACT_PHASE_EXEC;
        edge->exec_start_ts = 0;
    }
}

/* Read wait_event_info directly from PGPROC address (1 probe_read).
 * Used when we have the cached address from state_map. */
static __always_inline u32 read_wait_event_direct(u64 addr)
{
    u32 val = 0;
    if (addr)
        bpf_probe_read_user(&val, 4, (void *)addr);
    return val;
}

/* Read the current wait_event_info value via double-dereference.
 * my_wait_ptr_addr → pointer → uint32 value.
 * Used only on first event for a PID (before we cache the address). */
static __always_inline u32 read_wait_event(u64 *out_addr)
{
    u32 val = 0;
    u64 ptr = 0;

    bpf_probe_read_user(&ptr, sizeof(ptr), (void *)my_wait_ptr_addr);
    if (out_addr)
        *out_addr = ptr;
    if (ptr)
        bpf_probe_read_user(&val, 4, (void *)ptr);
    return val;
}

/* S3 (docs/S3_SCHED_SWITCH_CPU.md): exact on-CPU ns for a backend AT INSTANT
 * `now`, derived from the sched_switch-maintained accumulator. cpu_ns_total is
 * the sum of completed on-CPU stretches; if the backend is on-CPU right now
 * (on_cpu_ts != 0, e.g. it just wrote wait_event_info in the watchpoint
 * handler) the current stretch `now - on_cpu_ts` is added. No tick
 * quantization — replaces the stale-between-ticks se.sum_exec_runtime read.
 * Returns 0 when the feature is off (cpu_accounting=0) so the caller emits
 * cpu_ns 0 and userspace stamps the UNKNOWN sentinel (→ gap-inference). */
static __always_inline u64 read_task_cpu_ns_for(struct pgwt_pid_state *st, u64 now)
{
    if (!cpu_accounting || !st)
        return 0;
    u64 t = st->cpu_ns_total;
    if (st->on_cpu_ts && now >= st->on_cpu_ts)
        t += now - st->on_cpu_ts;
    return t;
}

/* ── Program: on_sched_switch ─────────────────────────────────
 * Exact CPU accounting (S3). Fires on EVERY context switch, system-wide;
 * for the two tasks involved it does a state_map lookup and either closes
 * (prev, going off-CPU) or opens (next, coming on-CPU) an on-CPU stretch.
 * Untracked pids miss both lookups and return in ~tens of ns (the overhead
 * measured within noise at 110-126k switches/s, docs/S3 §7). A given pid is
 * on at most one CPU, so its entry is touched by one CPU at a time — no
 * atomics needed. The link stays loaded, but the body is armed only for full
 * mode or an exact tiered window. */
SEC("tp_btf/sched_switch")
int BPF_PROG(on_sched_switch, bool preempt,
             struct task_struct *prev, struct task_struct *next)
{
    if (!cpu_accounting || !exact_cpu_active)
        return 0;
    /* TEST HOOK: with sched_switch inert, on_cpu_ts is established ONLY by the
     * seed (current_wei==0, also forced to 0 under this hook — see backend.c)
     * or a watchpoint fire. A backend seeded off-CPU that then runs a waitless
     * loop therefore has live CPU iff the watchpoint-fire on_cpu_ts open exists,
     * making the straddle-CPU repro deterministic. Test-only. */
    if (test_no_sched_oncpu)
        return 0;
    u64 now = bpf_ktime_get_ns();

    u32 ppid = BPF_CORE_READ(prev, pid);
    struct pgwt_pid_state *ps = bpf_map_lookup_elem(&state_map, &ppid);
    u32 npid = BPF_CORE_READ(next, pid);
    struct pgwt_pid_state *ns = bpf_map_lookup_elem(&state_map, &npid);
    if (ps && ps->on_cpu_ts && now >= ps->on_cpu_ts) {
        ps->cpu_ns_total += now - ps->on_cpu_ts;
        ps->on_cpu_ts = 0;
    }

    if (ns)
        ns->on_cpu_ts = now;
    return 0;
}

/* Accumulate duration for a wait event into accum_map (PERCPU — no atomics). */
static __always_inline void accum_add(u32 event, u64 duration)
{
    struct pgwt_accum_val *av = bpf_map_lookup_elem(&accum_map, &event);
    if (av) {
        av->total_ns += duration;
        av->count += 1;
    } else {
        struct pgwt_accum_val new_av = { .total_ns = duration, .count = 1 };
        bpf_map_update_elem(&accum_map, &event, &new_av, BPF_NOEXIST);
    }
}

/* ── Program 1: on_watchpoint ─────────────────────────────── */
/* Fires on every write to PGPROC->wait_event_info.
 * State machine: computes duration of PREVIOUS state, accumulates. */

SEC("perf_event")
int on_watchpoint(struct bpf_perf_event_data *ctx)
{
    if (!exact_admission_open())
        return 0;
    u32 pid = bpf_get_current_pid_tgid() >> 32;

    struct pgwt_pid_state *st = bpf_map_lookup_elem(&state_map, &pid);
    if (st) {
        /* Fast path: read wait_event directly (1 probe_read vs 2) */
        u32 new_event = read_wait_event_direct(st->wait_event_addr);

        /* A sampled-tier seed entry (wp_live == 0) carries query_id /
         * cmd_open only — its last_event/last_ts are NOT interval state.
         * First watchpoint fire on such an entry (defensive: preseed runs
         * before enable, CAP-8) starts interval tracking without closing a
         * fabricated interval. */
        if (!st->wp_live) {
            u64 seed_now = bpf_ktime_get_ns();
            if (!exact_admission_open())
                return 0;
            st->last_event = new_event;
            st->last_ts = seed_now;
            /* S3: snapshot the exact on-CPU ns as this interval's base. This
             * is a pseudo-gap (seed → first real boundary); do NOT emit for
             * it, just establish last_cpu_ns so the next real transition's
             * cpu_ns delta is correct. */
            st->last_cpu_ns = read_task_cpu_ns_for(st, seed_now);
            /* Same on-CPU open as the real-transition path below: the backend
             * is executing this write, so if no sched_switch-in has opened its
             * stretch, open it here — otherwise the live read stays flat at 0
             * for a backend that runs uninterrupted after seeding. No-op when
             * cpu_accounting is off (read_task_cpu_ns_for returns 0 anyway). */
            if (st->on_cpu_ts == 0)
                st->on_cpu_ts = seed_now;
            st->wp_live = 1;
            return 0;
        }

        /* Skip redundant writes — watchpoint fires even if value unchanged.
         * Return BEFORE touching last_cpu_ns: the CPU accrued between two
         * suppressed fires belongs to the next real gap, so the base must not
         * advance here (T8 §5.1). */
        if (new_event == st->last_event)
            return 0;

        u64 now = bpf_ktime_get_ns();
        if (!exact_admission_open())
            return 0;
        u64 duration = now - st->last_ts;

        /* S3: exact on-CPU ns at this fire; the CPU consumed since the last
         * boundary is the delta. 0 when the feature is off or a base was never
         * seeded (userspace then stamps UNKNOWN → gap-inference). */
        u64 cpu_now = read_task_cpu_ns_for(st, now);
        u64 cpu_delta = (cpu_now && st->last_cpu_ns && cpu_now >= st->last_cpu_ns)
                        ? cpu_now - st->last_cpu_ns : 0;
        if (!exact_admission_open())
            return 0;

        if (lightweight_mode) {
            /* Lightweight: accumulate in BPF map, no ringbuf */
            accum_add(st->last_event, duration);
        } else {
            u64 query_id = 0;
            u16 cmd_open = 0;
            u16 query_quality = 0;
            resolve_exact_attr(pid, &query_id, &cmd_open, &query_quality);
            /* Full trace: attribution comes from the generation-aware merge
             * of BPF edges and the separate coherent window-start seed. */
            struct pgwt_trace_event evt = {
                .timestamp_ns = now,
                .pid = pid,
                .old_event = st->last_event,
                .new_event = new_event,
                .flags = (cmd_open ? PGWT_EVENT_FLAG_CMD_OPEN : 0) |
                    (query_quality == PGWT_QUERY_QUALITY_PG13_SYNTH
                     ? PGWT_EVENT_FLAG_QUERY_SYNTH : 0),
                .duration_ns = duration,
                .query_id = query_id,
                .cpu_ns = cpu_delta,
            };
            emit_event(&evt);
        }

        /* Transition to new state */
        st->last_event = new_event;
        st->last_ts = now;
        /* Advance the CPU base on every emitted transition (T8). */
        st->last_cpu_ns = cpu_now;

        /* S3 live-read fix: the backend is ON-CPU at this fire — it is executing
         * this write. If no sched_switch-in has opened its on-CPU stretch (we
         * attached mid-run and never saw its switch-in, or it seeded from a
         * transient wait so on_cpu_ts started 0), on_cpu_ts is 0 here and
         * read_task_cpu_ns_for() returns a FLAT cpu_ns_total. A backend that
         * then runs uninterrupted (no sched_switch — common when a CPU-bound
         * backend owns a core on a low-CPU host) keeps exact == last_cpu_ns, so
         * the live view reads cpu_open == 0 for the whole on-CPU interval that
         * starts here — even though the terminal flush (post-switch accumulator)
         * records it correctly. Open the stretch now so the live read measures
         * it. The "== 0" guard never clobbers a stretch sched_switch is already
         * tracking; `now` is a conservative lower bound on the true start. */
        if (st->on_cpu_ts == 0)
            st->on_cpu_ts = now;

    } else {
        /* First event for this PID — initialize via double-deref,
         * cache the resolved address for future fast-path reads */
        u64 wait_addr = 0;
        u32 new_event = read_wait_event(&wait_addr);
        u64 init_now = bpf_ktime_get_ns();
        struct pgwt_pid_state new_st = {
            .last_event = new_event,
            .wp_live = 1,   /* created BY a live watchpoint */
            .last_ts = init_now,
            .last_query_id = 0,
            .be_entry_ptr = resolve_be_entry(),
            .wait_event_addr = wait_addr,
            /* S3: the backend is on-CPU now (it just wrote wait_event_info);
             * open its on-CPU stretch here so the first interval measures. The
             * base is 0 — cpu_ns deltas are taken from this attach onward.
             * (test_no_sched_oncpu forces 0 so this path cannot contribute live
             * CPU either — under the hook on_cpu_ts is established ONLY by the
             * real-transition open above, isolating that fix for the test.) */
            .on_cpu_ts = test_no_sched_oncpu ? 0 : init_now,
            .cpu_ns_total = 0,
            .last_cpu_ns = 0,
        };
        if (!exact_admission_open())
            return 0;
        /* CAP-1: a failed insert (map full) means this backend will never
         * record a single event — count it so the daemon can scream. */
        if (bpf_map_update_elem(&state_map, &pid, &new_st, BPF_ANY))
            count_map_fail(PGWT_BPF_FAIL_STATE_MAP);
    }
    return 0;
}

/* ── Program 2: on_bootstrap ──────────────────────────────── */
/* Fires when InitProcess() writes the my_wait_event_info pointer.
 * Sends {pid, PGPROC->wait_event_info addr} to daemon via ring buffer. */

SEC("perf_event")
int on_bootstrap(struct bpf_perf_event_data *ctx)
{
    u32 pid = bpf_get_current_pid_tgid() >> 32;

    u64 ptr = 0;
    bpf_probe_read_user(&ptr, sizeof(ptr), (void *)my_wait_ptr_addr);

    if (ptr != 0) {
        /* PG17+: ptr already points at wait_event_info (offset 0).
         * PG<17: ptr is the backend's PGPROC; add the field offset. */
        u64 wait_addr = ptr + pgproc_wait_offset;
        struct pgwt_lifecycle_event *ev;
        ev = bpf_ringbuf_reserve(&lifecycle_rb, sizeof(*ev), 0);
        if (ev) {
            ev->type = PGWT_LIFECYCLE_INIT;
            ev->pid = pid;
            ev->addr = wait_addr;
            ev->timestamp = bpf_ktime_get_ns();
            bpf_ringbuf_submit(ev, 0);
        }
    }
    return 0;
}

/* ── Program 3: on_fork ───────────────────────────────────── */
/* Detects new PG backends forked by postmaster. */

SEC("tp/sched/sched_process_fork")
int on_fork(struct trace_event_raw_sched_process_fork *ctx)
{
    u32 parent = ctx->parent_pid;
    u32 child  = ctx->child_pid;

    if (parent != target_postmaster_pid)
        return 0;

    /* Enroll the child in the attribution maps before userspace receives the
     * fork event. During an exact window this closes the fork->registry race:
     * query/activity edges from a fast child can already be generation-tagged
     * while its wait address and watchpoint are still being resolved. */
    struct pgwt_pid_state child_seed = {};
    if (bpf_map_update_elem(&state_map, &child, &child_seed, BPF_NOEXIST) &&
        bpf_map_lookup_elem(&state_map, &child) == NULL)
        count_map_fail(PGWT_BPF_FAIL_STATE_MAP);

    struct pgwt_lifecycle_event *ev;
    ev = bpf_ringbuf_reserve(&lifecycle_rb, sizeof(*ev), 0);
    if (ev) {
        ev->type = PGWT_LIFECYCLE_FORK;
        ev->pid = child;
        ev->addr = 0;
        ev->timestamp = bpf_ktime_get_ns();
        bpf_ringbuf_submit(ev, 0);
    }
    return 0;
}

/* ── Program 4: on_exit ───────────────────────────────────── */
/* Detects exiting backends. Closes last open interval first. */

SEC("tp/sched/sched_process_exit")
int on_exit(struct trace_event_raw_sched_process_template *ctx)
{
    u32 pid = ctx->pid;

    struct pgwt_pid_state *st = bpf_map_lookup_elem(&state_map, &pid);
    if (!st)
        return 0;

    u64 now = bpf_ktime_get_ns();

    /* Close the last open interval — ONLY if a live watchpoint was
     * maintaining it. A sampled-tier seed entry (wp_live == 0) carries
     * query_id/cmd_open only: last_ts is the SEED time, and "closing" it
     * manufactured a phantom interval spanning the backend's whole sampled
     * lifetime, mislabeled as its last_event (usually 0 = CPU) — T2 study
     * defect 2: every pgbench disconnect back-filled ~120 s of phantom CPU
     * into the trace. The sampled tier's data is the SAMPLES stream; there
     * is no exact interval to close. */
    if (st->wp_live && exact_admission_open()) {
        u64 duration = now - st->last_ts;

        /* S3: close the last interval with the exact on-CPU delta — a command
         * computing right up to disconnect (old_event==0) records its CPU
         * instead of vanishing. read_task_cpu_ns_for handles the still-open
         * on-CPU stretch (the exiting task is on-CPU running the exit path). */
        u64 cpu_now = read_task_cpu_ns_for(st, now);
        u64 cpu_delta = (cpu_now && st->last_cpu_ns && cpu_now >= st->last_cpu_ns)
                        ? cpu_now - st->last_cpu_ns : 0;

        if (lightweight_mode) {
            accum_add(st->last_event, duration);
        } else {
            u64 query_id = 0;
            u16 cmd_open = 0;
            u16 query_quality = 0;
            resolve_exact_attr(pid, &query_id, &cmd_open, &query_quality);
            /* Emit final trace event (exit sentinel) */
            struct pgwt_trace_event evt = {
                .timestamp_ns = now,
                .pid = pid,
                .old_event = st->last_event,
                .new_event = PGWT_EVENT_EXIT,
                .flags = (cmd_open ? PGWT_EVENT_FLAG_CMD_OPEN : 0) |
                    (query_quality == PGWT_QUERY_QUALITY_PG13_SYNTH
                     ? PGWT_EVENT_FLAG_QUERY_SYNTH : 0),
                .duration_ns = duration,
                .query_id = query_id,
                .cpu_ns = cpu_delta,
            };
            if (exact_admission_open())
                emit_event(&evt);
        }
    }

    /* Notify daemon */
    struct pgwt_lifecycle_event *ev;
    ev = bpf_ringbuf_reserve(&lifecycle_rb, sizeof(*ev), 0);
    if (ev) {
        ev->type = PGWT_LIFECYCLE_EXIT;
        ev->pid = pid;
        ev->addr = 0;
        ev->timestamp = now;
        bpf_ringbuf_submit(ev, 0);
    }
    return 0;
}

/* ── Programs 5-8: USDT query lifecycle probes ────────────── */
/* Emit marker events into the same ringbuf as wait events.
 * Uses bpf_ktime_get_ns() — same clock as on_watchpoint.
 * Only active in full trace mode (checked at emit time). */

static __always_inline void emit_marker(u32 marker)
{
    if (lightweight_mode || !exact_admission_open())
        return;

    u32 pid = bpf_get_current_pid_tgid() >> 32;
    u64 now = bpf_ktime_get_ns();

    u64 qid = 0;
    u16 cmd_open = 0;
    u16 query_quality = 0;
    if (!bpf_map_lookup_elem(&state_map, &pid))
        return;
    resolve_exact_attr(pid, &qid, &cmd_open, &query_quality);
    note_phase_edge(pid, marker, now);
    if (!exact_admission_open())
        return;

    struct pgwt_trace_event evt = {
        .timestamp_ns = now,
        .pid = pid,
        .old_event = marker,
        .new_event = marker,
        .flags = query_quality == PGWT_QUERY_QUALITY_PG13_SYNTH
               ? PGWT_EVENT_FLAG_QUERY_SYNTH : 0,
        .duration_ns = 0,
        .query_id = qid,
    };
    emit_event(&evt);
}

SEC("usdt")
int BPF_USDT(on_exec_start)
{
    emit_marker(PGWT_MARKER_EXEC_START);
    return 0;
}

SEC("usdt")
int BPF_USDT(on_exec_done)
{
    emit_marker(PGWT_MARKER_EXEC_END);
    return 0;
}

SEC("usdt")
int BPF_USDT(on_plan_start)
{
    emit_marker(PGWT_MARKER_PLAN_START);
    return 0;
}

SEC("usdt")
int BPF_USDT(on_plan_done)
{
    emit_marker(PGWT_MARKER_PLAN_END);
    return 0;
}

/* ── Program 9: uprobe on pgstat_report_query_id ─────────── */
/* Captures query_id directly from the function argument,
 * bypassing shared memory. Also captures query text from
 * debug_query_string on first occurrence of each query_id. */

SEC("uprobe")
int on_report_query_id(struct pt_regs *ctx)
{
    count_uprobe_fire(PGWT_UPROBE_FIRE_QUERY);
    if (!exact_admission_open())
        return 0;
    u64 query_id = PT_REGS_PARM1(ctx);
    u32 pid = bpf_get_current_pid_tgid() >> 32;
    struct pgwt_pid_state *st = bpf_map_lookup_elem(&state_map, &pid);
    if (st) {
        u64 now = bpf_ktime_get_ns();
        note_query_edge(pid, query_id, now);
        /* Retained only for state-map diagnostics/backward compatibility;
         * exact consumers resolve from edge+seed maps. */
        st->last_query_id = query_id;
    }

    /* PostgreSQL calls pgstat_report_query_id(0) after each statement
     * to reset the query_id. We must honor it — otherwise events between
     * statements (like Client:ClientRead) get attributed to the previous
     * query, inflating its Client wait time. */
    if (query_id == 0) {
        return 0;
    }
    if (!exact_admission_open())
        return 0;

    /* Capture query text on first occurrence of this query_id.
     * Reads debug_query_string (global in postgres, always set before
     * pgstat_report_query_id is called). Emits via lifecycle_rb
     * (not event_ringbuf — text events are metadata, not trace data). */
    if (lightweight_mode || skip_query_id || !debug_query_string_addr)
        return 0;

    struct pgwt_seen_query_key seen_key = {
        .pid = pid,
        .query_id = query_id,
    };
    if (bpf_map_lookup_elem(&seen_query_ids, &seen_key))
        return 0;  /* already captured text for this query_id */

    /* Read the debug_query_string pointer, then the string */
    u64 str_ptr = 0;
    bpf_probe_read_user(&str_ptr, sizeof(str_ptr),
                         (void *)debug_query_string_addr);
    if (!str_ptr)
        return 0;

    struct pgwt_query_text_event *evt;
    evt = bpf_ringbuf_reserve(&lifecycle_rb, sizeof(*evt), 0);
    if (!evt)
        return 0; /* leave unseen: a later execution retries raw capture */
    evt->type = PGWT_LIFECYCLE_QUERY_TEXT;
    evt->pid = pid;
    evt->query_id = query_id;
    long text_len = bpf_probe_read_user_str(evt->text, sizeof(evt->text),
                                            (void *)str_ptr);
    if (text_len <= 1) {
        bpf_ringbuf_discard(evt, 0);
        return 0; /* unreadable/empty is not a successful first-seen */
    }
    bpf_ringbuf_submit(evt, 0);

    /* Mark as seen. CAP-6: when the map is full the insert fails and text
     * for NEW query_ids is never captured again — count it (the daemon
     * logs once and surfaces seen_query_ids_full_total). */
    u8 one = 1;
    if (bpf_map_update_elem(&seen_query_ids, &seen_key, &one, BPF_ANY))
        count_map_fail(PGWT_BPF_FAIL_SEEN_QIDS);

    return 0;
}

/* ── Program: uprobe on pgstat_report_activity (command-open gate, T2) ────
 * pgstat_report_activity(BackendState state, const char *cmd_str) is the
 * function PostgreSQL itself uses to drive pg_stat_activity.state. Gating
 * on it gives the tracer the exact same "active" window: query message
 * received (STATE_RUNNING) -> command complete (STATE_IDLE /
 * STATE_IDLEINTRANSACTION*), which INCLUDES parse, plan, bind, execute and
 * commit/abort processing. The sampler reads cmd_open to decide whether a
 * client backend's we==0 reading is a first-class CPU sample (in-command)
 * or non-command churn (docs/AAS_SEMANTICS_DECISION.md: ungated counting
 * measured ~3x true activity on chatty OLTP).
 *
 * While a watchpoint is live for the pid (full mode / tiered escalated),
 * each gate FLIP also lands in the event stream as a CMD_START/CMD_END
 * marker so offline consumers can classify exact-tier we==0 intervals the
 * same way — no step artifact at tier switches.
 *
 * The uprobe only UPDATES existing state_map entries (like the query-id
 * uprobes); entries are seeded by the scan/fork/preseed paths. Stage 3 keeps
 * it attached only for full/exact windows or a degraded fallback. */

SEC("uprobe")
int on_report_activity(struct pt_regs *ctx)
{
    count_uprobe_fire(PGWT_UPROBE_FIRE_ACTIVITY);
    if (!exact_admission_open())
        return 0;
    if (!bs_state_running)
        return 0;   /* gate unavailable for this PG version */

    u32 state = (u32)PT_REGS_PARM1(ctx);
    u32 pid = bpf_get_current_pid_tgid() >> 32;
    struct pgwt_pid_state *st = bpf_map_lookup_elem(&state_map, &pid);
    if (!st)
        return 0;

    u16 open = (state == bs_state_running || state == bs_state_fastpath)
             ? 1 : 0;
    u64 old_query_id = 0;
    u16 old_open = 0;
    u16 old_quality = 0;
    resolve_exact_attr(pid, &old_query_id, &old_open, &old_quality);
    u64 now = bpf_ktime_get_ns();
    note_cmd_edge(pid, open, now);
    st->cmd_open = open;
    if (old_open == open)
        return 0;   /* stamp the generation, but no boundary crossed */

    /* PG13 has no core reset-to-zero query-id edge. A straddler seed must not
     * leak into the next statement: clear it before a new command marker and
     * after the old command's terminal marker. ExecutorStart will publish the
     * real ID later for statements that have one. */
    if ((pg13_synthetic_attr || pg13_query_attr) && open)
        note_query_edge(pid, 0, now);

    if (!lightweight_mode && st->wp_live) {
        u64 query_id = 0;
        u16 current_open = 0;
        u16 query_quality = 0;
        resolve_exact_attr(pid, &query_id, &current_open, &query_quality);
        u32 marker = open ? PGWT_MARKER_CMD_START : PGWT_MARKER_CMD_END;
        struct pgwt_trace_event evt = {
            .timestamp_ns = now,
            .pid = pid,
            .old_event = marker,
            .new_event = marker,
            .flags = query_quality == PGWT_QUERY_QUALITY_PG13_SYNTH
                   ? PGWT_EVENT_FLAG_QUERY_SYNTH : 0,
            .duration_ns = 0,
            .query_id = query_id,
        };
        if (exact_admission_open())
            emit_event(&evt);
    }
    if ((pg13_synthetic_attr || pg13_query_attr) && !open)
        note_query_edge(pid, 0, now);
    return 0;
}

/* ── Program 10: uprobe on standard_ExecutorStart (PG13 query attribution) ──
 * PG13 has no in-core query_id and no pgstat_report_query_id. When
 * pg_stat_statements is loaded its post_parse_analyze hook populates
 * PlannedStmt.queryId (matching pg_stat_statements.queryid) before
 * the executor starts. We probe standard_ExecutorStart rather than the
 * public ExecutorStart wrapper: with pgss loaded ExecutorStart_hook is set,
 * so ExecutorStart is a trampoline that tail-jumps into the hook chain and an
 * entry uprobe on it does not fire; standard_ExecutorStart is the real
 * function, always reached at the bottom of the hook chain.
 * arg0 = QueryDesc *queryDesc; walk
 *   queryDesc->plannedstmt (+pg13_qd_plannedstmt_off)
 *           ->queryId       (+pg13_ps_queryid_off, uint64)
 * and store it in the SAME state_map slot the PG17+ uprobe uses, so the
 * watchpoint (full tier) and sampler (sampled tier) pick it up unchanged.
 * One uprobe per query, not per event. Query text comes from
 * queryDesc->sourceText (a const char*), captured once per query_id. */

SEC("uprobe")
int on_executor_start(struct pt_regs *ctx)
{
    if (!pg13_query_attr)
        return 0;
    count_uprobe_fire(PGWT_UPROBE_FIRE_QUERY);
    if (!exact_admission_open())
        return 0;

    u64 query_desc = PT_REGS_PARM1(ctx);
    if (!query_desc)
        return 0;

    /* queryDesc->plannedstmt */
    u64 planned = 0;
    bpf_probe_read_user(&planned, sizeof(planned),
                        (void *)(query_desc + pg13_qd_plannedstmt_off));
    if (!planned)
        return 0;

    /* plannedstmt->queryId (uint64). With pgss loaded this is set; without it
     * the field is 0, so we leave the slot untouched (query attribution stays
     * "unavailable" rather than reporting a bogus 0 id). */
    u64 query_id = 0;
    bpf_probe_read_user(&query_id, sizeof(query_id),
                        (void *)(planned + pg13_ps_queryid_off));
    if (!query_id)
        return 0;

    u32 pid = bpf_get_current_pid_tgid() >> 32;
    struct pgwt_pid_state *st = bpf_map_lookup_elem(&state_map, &pid);
    if (st) {
        note_query_edge(pid, query_id, bpf_ktime_get_ns());
        st->last_query_id = query_id;
    }
    if (!exact_admission_open())
        return 0;

    /* Capture query text on first occurrence of this query_id, from
     * queryDesc->sourceText. Emitted via lifecycle_rb (metadata, not trace
     * data) — same consumer as the PG17+ debug_query_string path. */
    if (lightweight_mode || skip_query_id || !pg13_qd_sourcetext_off)
        return 0;

    struct pgwt_seen_query_key seen_key = {
        .pid = pid,
        .query_id = query_id,
    };
    if (bpf_map_lookup_elem(&seen_query_ids, &seen_key))
        return 0;  /* already captured text for this query_id */

    u64 src_ptr = 0;
    bpf_probe_read_user(&src_ptr, sizeof(src_ptr),
                        (void *)(query_desc + pg13_qd_sourcetext_off));
    if (!src_ptr)
        return 0;

    struct pgwt_query_text_event *evt;
    evt = bpf_ringbuf_reserve(&lifecycle_rb, sizeof(*evt), 0);
    if (!evt)
        return 0; /* leave unseen: a later execution retries raw capture */
    evt->type = PGWT_LIFECYCLE_QUERY_TEXT;
    evt->pid = pid;
    evt->query_id = query_id;
    long text_len = bpf_probe_read_user_str(evt->text, sizeof(evt->text),
                                            (void *)src_ptr);
    if (text_len <= 1) {
        bpf_ringbuf_discard(evt, 0);
        return 0;
    }
    bpf_ringbuf_submit(evt, 0);

    u8 one = 1;
    if (bpf_map_update_elem(&seen_query_ids, &seen_key, &one, BPF_ANY))
        count_map_fail(PGWT_BPF_FAIL_SEEN_QIDS);

    return 0;
}

char LICENSE[] SEC("license") = "GPL";
