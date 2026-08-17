/* pg_wait_tracer.h — Shared types between BPF and userspace */
#ifndef PG_WAIT_TRACER_H
#define PG_WAIT_TRACER_H

#ifdef __BPF__
#include "vmlinux.h"
#else
#include <stdint.h>
typedef uint8_t  u8;
typedef uint16_t u16;
typedef uint32_t u32;
typedef uint64_t u64;
#endif

/* ── Version (T7 / TST-11) ────────────────────────────────── */
/* Build version: the Makefile injects `git describe --tags --always --dirty`
 * as -DPGWT_BUILD_VERSION="..." so every binary reports exactly the commit it
 * was built from. The fallback covers direct compiles (unit tests build some
 * individual source files without the top-level Makefile) — a shipped binary
 * is always built by make and never reports it. */
#ifndef PGWT_BUILD_VERSION
#define PGWT_BUILD_VERSION   "unknown"
#endif

/* Client/server protocol revision (the JSON-line command surface pgwt-server
 * speaks and the Go client consumes). A skewed Mac-client/Linux-server pair
 * is the NORMAL deployment state, so the server reports this in every `info`
 * response and the client warns loudly on mismatch — warn, never refuse.
 * Bump ONLY on a breaking protocol change (renamed/retyped fields, changed
 * command semantics); additive fields do not bump it. Keep in sync with
 * `protocolRev` in web/main.go and PROTOCOL_REV in tests/mock_server.py
 * (tests/test_protocol_drift.py enforces the mock side). */
#define PGWT_PROTOCOL_REV    1

/* ── Histogram ────────────────────────────────────────────── */
#define HISTOGRAM_BUCKETS    16
/* Bucket boundaries in microseconds:
 * 0: <1, 1: 1-2, 2: 2-4, 3: 4-8, 4: 8-16, 5: 16-32,
 * 6: 32-64, 7: 64-128, 8: 128-256, 9: 256-512, 10: 512-1024,
 * 11: 1ms-2ms, 12: 2-4ms, 13: 4-8ms, 14: 8-16ms, 15: >=16ms */

/* ── Limits ───────────────────────────────────────────────── */
#define MAX_BACKENDS         1024
#define MAX_EVENTS_PER_PID   256
#define MAX_CPUS             128

/* ── BPF Map Structs ──────────────────────────────────────── */

/* Per-PID state in state_map */
struct pgwt_pid_state {
    u32 last_event;     /* previous wait_event_info value (0 = on CPU) */
    /* wp_live: 1 while a LIVE hardware watchpoint maintains last_event /
     * last_ts for this pid (full mode, or tiered while escalated). 0 for
     * sampled-tier seeds, whose entries only carry query_id/cmd_open — their
     * last_event/last_ts are NOT interval state. on_exit must only close an
     * interval when wp_live is set: closing a seed entry fabricated a
     * phantom full-window "CPU" interval per disconnect (T2 study defect 2:
     * 4 clients x 120 s of phantom CPU in a trace whose sampler saw 0.017
     * AAS). */
    u16 wp_live;
    /* cmd_open: 1 while a client backend is inside a command — the same
     * window as pg_stat_activity state='active' (query message received ->
     * command complete, incl. parse/plan/execute/commit). Maintained by the
     * on_report_activity uprobe; read by the sampler to gate on-CPU (we==0)
     * samples for client backends (docs/AAS_SEMANTICS_DECISION.md). */
    u16 cmd_open;
    u64 last_ts;        /* ktime_ns of last transition */
    u64 last_query_id;  /* query_id set by on_report_query_id uprobe */
    u64 be_entry_ptr;   /* cached PgBackendStatus* (avoids 1 probe_read per event) */
    u64 wait_event_addr; /* direct PGPROC->wait_event_info address (saves 1 probe_read) */
    /* S3 exact CPU accounting (docs/S3_SCHED_SWITCH_CPU.md). The
     * on_sched_switch program maintains, per tracked backend:
     *   cpu_ns_total — Σ of completed on-CPU stretches (exact ns), and
     *   on_cpu_ts    — ktime the current on-CPU stretch began (0 = off-CPU).
     * The exact on-CPU ns AT ANY INSTANT is
     *   cpu_ns_total + (on_cpu_ts ? now - on_cpu_ts : 0)
     * — no tick quantization, no sub-tick leak (unlike the dropped
     * se.sum_exec_runtime read, which was stale between ticks). */
    u64 cpu_ns_total;
    u64 on_cpu_ts;
    /* last_cpu_ns: the exact on-CPU ns (per above) at the last emitted
     * boundary — the base for the closing interval's cpu_ns delta. Advanced
     * only on an emitted transition (NOT on redundant-write suppression, or
     * CPU between suppressed fires would be lost). 0 = feature off. */
    u64 last_cpu_ns;
};

enum pgwt_query_attribution_quality {
    PGWT_QUERY_QUALITY_NONE = 0,
    PGWT_QUERY_QUALITY_REAL = 1,
    PGWT_QUERY_QUALITY_PG13_SYNTH = 2,
};

/* Stage 3 tiered exact-probe state. Probe edges and escalation preseeds live
 * in separate maps deliberately: userspace writes only pgwt_exact_seed, while
 * BPF writes only pgwt_exact_attr. A probe edge stamped after the generation
 * boundary therefore cannot be overwritten by a slower coherent snapshot. */
struct pgwt_exact_config {
    u64 generation;
    u64 attach_boundary_ns;
    u32 admitting;
    u32 reserved;
};

struct pgwt_exact_seed {
    u64 generation;
    u64 snapshot_ts;
    u64 query_id;
    u16 cmd_open;
    u16 query_quality;
    u32 reserved32;
};

#define PGWT_EXACT_PHASE_PLAN  (1U << 0)
#define PGWT_EXACT_PHASE_EXEC  (1U << 1)

struct pgwt_exact_attr {
    u64 query_generation;
    u64 query_edge_ts;
    u64 query_id;
    u64 cmd_generation;
    u64 cmd_edge_ts;
    u64 phase_generation;
    u64 plan_start_ts;
    u64 exec_start_ts;
    u16 cmd_open;
    u16 phase_flags;
    u16 query_quality;
    u16 reserved16;
};

/* Shared userspace/BPF merge rule. The coherent seed supplies the generation
 * boundary state; each BPF-owned field overrides it only when that field has
 * a same-generation edge stamped at or after attach. Generation zero is the
 * degraded-layout fallback and reads its pinned edges directly. */
static inline void pgwt_exact_merge_attr(const struct pgwt_exact_config *cfg,
                                         const struct pgwt_exact_attr *edge,
                                         const struct pgwt_exact_seed *seed,
                                         u64 *query_id, u32 *cmd_quality)
{
    *query_id = 0;
    u16 cmd_open = 0;
    u16 query_quality = PGWT_QUERY_QUALITY_NONE;
    if (!cfg || cfg->generation == 0) {
        if (edge) {
            *query_id = edge->query_id;
            cmd_open = edge->cmd_open;
            query_quality = edge->query_quality;
        }
        *cmd_quality = (u32)cmd_open | ((u32)query_quality << 16);
        return;
    }
    if (seed && seed->generation == cfg->generation) {
        *query_id = seed->query_id;
        cmd_open = seed->cmd_open;
        query_quality = seed->query_quality;
    }
    if (!edge) {
        *cmd_quality = (u32)cmd_open | ((u32)query_quality << 16);
        return;
    }
    if (edge->query_generation == cfg->generation &&
        edge->query_edge_ts >= cfg->attach_boundary_ns)
    {
        *query_id = edge->query_id;
        query_quality = edge->query_quality;
    }
    if (edge->cmd_generation == cfg->generation &&
        edge->cmd_edge_ts >= cfg->attach_boundary_ns)
        cmd_open = edge->cmd_open;
    *cmd_quality = (u32)cmd_open | ((u32)query_quality << 16);
}

/* ── Lifecycle Events (ring buffer) ───────────────────────── */

enum pgwt_lifecycle_type {
    PGWT_LIFECYCLE_FORK = 1,
    PGWT_LIFECYCLE_INIT = 2,
    PGWT_LIFECYCLE_EXIT = 3,
    PGWT_LIFECYCLE_QUERY_TEXT = 4,
};

struct pgwt_lifecycle_event {
    u32 type;       /* enum pgwt_lifecycle_type */
    u32 pid;
    u64 addr;       /* for INIT: PGPROC->wait_event_info address */
    u64 timestamp;
};

/* ── Raw Trace Event (ringbuf, 36 bytes) ─────────────────── */

struct pgwt_trace_event {
    u64 timestamp_ns;   /* absolute ktime_get_ns() */
    u32 pid;            /* backend PID */
    u32 old_event;      /* previous wait_event_info */
    u32 new_event;      /* new wait_event_info (0xFFFFFFFF = exit) */
    u32 flags;          /* reader-side annotation (PGWT_EVENT_FLAG_*); 0 on the wire */
    u64 duration_ns;    /* time spent in old_event */
    u64 query_id;       /* active query during old_event */
    /* T8 measured CPU: exact on-CPU nanoseconds consumed during old_event
     * (the se.sum_exec_runtime delta over the interval). For a wait-labeled
     * old_event this is ≈0 (the self-check: a sleeping task accrues no CPU);
     * for an on-CPU gap (old_event==0) it is the measured CPU, and
     * duration_ns - cpu_ns is the off-CPU / runqueue-unaccounted remainder.
     * PGWT_CPU_NS_UNKNOWN when not measured (legacy capability, v2 files,
     * SAMPLES records) — the compute layer then falls back to gap-inference. */
    u64 cpu_ns;
};

/* Sentinel for cpu_ns: "not measured" (compute falls back to gap-inference).
 * A real cpu_ns is a per-interval nanosecond delta, always far below this. */
#define PGWT_CPU_NS_UNKNOWN  0xFFFFFFFFFFFFFFFFULL

/* flags bits — NEVER persisted to the trace file (neither block layout
 * encodes a flags column). Producers annotate in memory:
 *
 *   - the trace reader sets SAMPLE on records decoded from a SAMPLES block:
 *     a point observation (new_event = sampled event; old_event = 0;
 *     duration_ns = 0), worth one sample_period_ns, never an interval;
 *   - BPF/on_watchpoint sets CMD_OPEN on ringbuf transition events emitted
 *     while the backend's command-open gate was set, so the LIVE accumulator
 *     can classify we==0 intervals without a marker sweep (the offline
 *     paths reconstruct the same gate from CMD_START/CMD_END markers);
 *   - the sampler/server annotate CATEGORY bits (process-type driven) so
 *     accumulation and the anomaly engine can apply the decomposed AAS
 *     model (docs/AAS_SEMANTICS_DECISION.md) without a registry lookup per
 *     consumer. */
#define PGWT_EVENT_FLAG_SAMPLE     0x1U
#define PGWT_EVENT_FLAG_CMD_OPEN   0x2U   /* command open at emission */
#define PGWT_EVENT_FLAG_IO_WORKER  0x4U   /* pid is an io_worker (excluded from AAS/DB Time) */
#define PGWT_EVENT_FLAG_MAINT      0x8U   /* pid is an autovacuum worker */
#define PGWT_EVENT_FLAG_BACKGROUND 0x10U  /* pid is another aux process */
#define PGWT_EVENT_FLAG_PLAN       0x20U  /* inside a PLAN marker window */
#define PGWT_EVENT_FLAG_EXEC       0x40U  /* inside an EXEC marker window */
#define PGWT_EVENT_FLAG_QUERY_SYNTH 0x80U /* PG13 escalation straddler seed */
#define PGWT_EVENT_FLAG_SAMPLE_CONT 0x100U /* reader-only split continuation */

#define PGWT_EVENT_EXIT  0xFFFFFFFFU  /* sentinel new_event for process exit */

/* Query lifecycle markers — emitted as trace events with these sentinels.
 * old_event = marker type, new_event = marker type, duration_ns = 0.
 * Inserted into the per-PID stream between wait events. */
#define PGWT_MARKER_EXEC_START   0xFFFFFFF0U  /* query execution start (PortalRun entry) */
#define PGWT_MARKER_EXEC_END     0xFFFFFFF1U  /* query execution end (PortalRun return) */
#define PGWT_MARKER_PLAN_START   0xFFFFFFF2U  /* query planning start (pg_plan_query entry) */
#define PGWT_MARKER_PLAN_END     0xFFFFFFF3U  /* query planning end (pg_plan_query return) */

/* Escalation window markers (A4). Written by the daemon (pid = 0, a value no
 * PG backend uses) when a tiered-mode full-fidelity window opens/closes, so a
 * reader/UI can show WHY exact data exists for a time range. Convention
 * (matches the query markers so duration-consuming views are unaffected):
 *   old_event = new_event = marker type
 *   duration_ns           = 0  (markers never carry a wait duration)
 *   query_id              = PGWT_ESC_PACK(window_seconds, reason): the granted
 *                           window length (START) or elapsed seconds (END) in
 *                           the high bits, the reason code in the low byte.
 * Forward-compatible: a reader that doesn't know these treats them as plain
 * markers (skipped from wait accumulation by PGWT_IS_MARKER). */
#define PGWT_MARKER_ESCALATE_START 0xFFFFFFF4U  /* full-fidelity window opened */
#define PGWT_MARKER_ESCALATE_END   0xFFFFFFF5U  /* full-fidelity window closed */

/* Command-boundary markers (T2). Emitted by the on_report_activity uprobe
 * (pgstat_report_activity state transitions) while a watchpoint is live for
 * the pid, so the exact tier records the same command-open window the
 * sampled tier gates on (pg_stat_activity state='active' semantics). The
 * compute layer uses them to classify we==0 (on-CPU) intervals: in-command
 * portions are CPU (foreground), the remainder is idle post-command time —
 * keeping AAS continuous across tier switches. Same shape as the query
 * markers: old_event = new_event = marker, duration_ns = 0, query_id = the
 * pid's current query_id at the boundary. */
#define PGWT_MARKER_CMD_START      0xFFFFFFF6U  /* command opened (state -> active) */
#define PGWT_MARKER_CMD_END        0xFFFFFFF7U  /* command closed (state -> idle/idle-in-txn) */

#define PGWT_IS_MARKER(e) ((e) >= 0xFFFFFFF0U && (e) <= 0xFFFFFFF7U)

/* Pack/unpack the escalation marker payload carried in query_id. */
#define PGWT_ESC_PACK(secs, reason) \
    (((uint64_t)(secs) << 8) | ((uint64_t)(reason) & 0xFFU))
#define PGWT_ESC_UNPACK_SECS(qid)   ((uint64_t)(qid) >> 8)
#define PGWT_ESC_UNPACK_REASON(qid) ((unsigned)((qid) & 0xFFU))

/* Reason a tiered escalation window opened/closed, recorded in the marker's
 * query_id slot. Manual is the only A4 producer; anomaly triggers (A5) and
 * budget/expiry close reasons are reserved here so the trace schema is stable
 * across phases. */
enum pgwt_escalation_reason {
    PGWT_ESC_REASON_MANUAL   = 0,  /* operator requested via control socket */
    PGWT_ESC_REASON_ANOMALY  = 1,  /* anomaly trigger (A5) */
    PGWT_ESC_REASON_EXPIRED  = 2,  /* window reached its deadline (close) */
    PGWT_ESC_REASON_REQUEST  = 3,  /* explicit deescalate request (close) */
    PGWT_ESC_REASON_SHUTDOWN = 4,  /* daemon stopping (close) */
    PGWT_ESC_REASON_BUDGET   = 5,  /* rolling-hour budget reached (mid-window close, ESC-1) */
    PGWT_ESC_REASON_CPU_SATURATION = 6, /* CPU-demand CUSUM trigger (AAS-1 Stage 3) */
};

/* ── Query Text Event (lifecycle ringbuf) ─────────────────── */
#define PGWT_QUERY_TEXT_LEN 256

struct pgwt_query_text_event {
    u32 type;       /* PGWT_LIFECYCLE_QUERY_TEXT */
    u32 pid;
    u64 query_id;
    char text[PGWT_QUERY_TEXT_LEN];
};

/* ── Wait Event Class IDs ─────────────────────────────────── */
#define PG_WAIT_LWLOCK      0x01
#define PG_WAIT_LOCK        0x03
#define PG_WAIT_BUFFERPIN   0x04
#define PG_WAIT_ACTIVITY    0x05
#define PG_WAIT_CLIENT      0x06
#define PG_WAIT_EXTENSION   0x07
#define PG_WAIT_IPC         0x08
#define PG_WAIT_TIMEOUT     0x09
#define PG_WAIT_IO          0x0A

#define WE_CLASS(x)  (((x) >> 24) & 0xFF)
#define WE_EVENT(x)  ((x) & 0x00FFFFFF)
/* Compose a wait_event_info from class byte + event id. */
#define WEI(cls, id) (((uint32_t)(cls) << 24) | (uint32_t)(id))

/* Client:ClientRead — idle between queries (like Oracle's SQL*Net message from client) */
#define PG_WAIT_CLIENT_READ  ((PG_WAIT_CLIENT << 24) | 0x000000)

/* Synthetic id for we==0 (on-CPU) exact intervals classified OUTSIDE a
 * command window (T2 decomposed AAS): post/between-command time is idle per
 * docs/AAS_SEMANTICS_DECISION.md. Activity class => excluded from load
 * (pgwt_is_idle_event) and hidden from event lists (pgwt_is_hidden_event)
 * exactly like other instrumented idle states. NEVER written to a trace —
 * assigned by the compute-side tagging pass (pgwt_tag_events) only. */
#define PGWT_WEI_NONCMD_CPU  (((uint32_t)PG_WAIT_ACTIVITY << 24) | 0x00FFFFFFU)

/* ── wait_event_info reading classification (CAP-2/3/5) ─────────
 * A reading from a resolved wait_event_info address falls in one of three
 * classes. Only a NON-ZERO reading with a known class byte PROVES the
 * address/offset is right: zero is also the most likely reading from a
 * WRONG offset (pointing into zeroed memory), so it must never be taken
 * as validation proof — only as "consistent, keep checking". */
#define PGWT_WEI_GARBAGE        0   /* unknown class byte — wrong offset */
#define PGWT_WEI_VALID_NONZERO  1   /* known wait class — proof of validity */
#define PGWT_WEI_ZERO           2   /* on-CPU; consistent but NOT proof */

#ifndef __BPF__
/* Pure classifier shared by the runtime validator (discovery.c), the
 * watchpoint preseed (backend.c) and the sampler read path (sampler.c).
 * PG wait classes occupy 0x01..0x0B (LWLock..InjectionPoint). */
static inline int pgwt_classify_wei(uint32_t wei)
{
    if (wei == 0)
        return PGWT_WEI_ZERO;
    unsigned cls = (wei >> 24) & 0xFF;
    return (cls >= 0x01 && cls <= 0x0B) ? PGWT_WEI_VALID_NONZERO
                                        : PGWT_WEI_GARBAGE;
}
#endif

/* ── BPF-side failure counters (CAP-1/CAP-6) ────────────────────
 * Slots in the fail_counters PERCPU_ARRAY map. The BPF programs bump these
 * when a map insert fails (map full); the daemon sums across CPUs and
 * surfaces them via the control socket so a full map is never silent. */
#define PGWT_BPF_FAIL_STATE_MAP  0  /* state_map insert failed (map full) */
#define PGWT_BPF_FAIL_SEEN_QIDS  1  /* seen_query_ids insert failed (full) */
#define PGWT_BPF_FAIL_MAX        2

/* Per-query trap counters. These are the Stages 3/4 overhead acceptance
 * signal: both stay exactly zero in de-escalated sampled mode on a validated
 * PG13-18 layout, and advance only while the exact bundle is attached (or
 * permanently on degraded fallback layouts). */
#define PGWT_UPROBE_FIRE_QUERY     0
#define PGWT_UPROBE_FIRE_ACTIVITY  1
#define PGWT_UPROBE_FIRE_MAX       2

/* ── BPF Accumulator (lightweight mode — no ringbuf) ───── */
#define ACCUM_MAP_MAX_ENTRIES 1024

struct pgwt_accum_val {
    u64 total_ns;   /* total time in this wait event */
    u64 count;      /* number of transitions into this event */
};

#endif /* PG_WAIT_TRACER_H */
