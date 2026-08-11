/* daemon.h — Daemon state and event loop */
#ifndef PGWT_DAEMON_H
#define PGWT_DAEMON_H

#include "pg_wait_tracer.h"
#include "backend.h"
#include "event_writer.h"
#include "summary_writer.h"
#include "query_text.h"
#include "backend_meta.h"
#include "map_reader.h"
#include "snapshot.h"
#include "provider.h"
#include "escalation.h"
#include "anomaly.h"

#include <stdbool.h>
#include <stdint.h>
#include <sys/types.h>

/* Forward declaration for skeleton */
struct pg_wait_tracer_bpf;
struct ring_buffer;
struct pgwt_control;
struct pgwt_sampler;

/* Self-observability counters (D6). Plain uint64 increments on the
 * single-threaded event path — exposed via the control socket
 * (src/control.c) with stable snake_case metric names. */
struct pgwt_counters {
    uint64_t events_total;             /* trace events consumed from event_ringbuf */
    uint64_t lifecycle_events_total;   /* fork/init/exit/query_text events */
    uint64_t wp_attach_failures_total; /* watchpoint attach failures (incl. bootstrap) */
    uint64_t prev_events_total;        /* events_total at previous timer tick */
    double   events_per_sec;           /* recent rate, refreshed each tick */

    /* Sampled tier (A2). Maintained by the sampler in sampler.c. */
    uint64_t samples_total;            /* SAMPLES records written */
    uint64_t prev_samples_total;       /* samples_total at previous timer tick */
    double   samples_per_sec;          /* recent sample rate, refreshed each tick */
    uint64_t sample_read_faults_total; /* process_vm_readv partial/EFAULT fallbacks */

    /* Capture hardening (T4). All of these mean "something the operator
     * must know about is happening" — each is paired with a loud log. */
    uint64_t state_map_full_total;     /* userspace state_map inserts failed (map full, CAP-1) */
    uint64_t invalid_wait_reads_total; /* wait_event_info reads with a garbage class byte (CAP-2/5) */
    uint64_t sampler_ticks_missed_total; /* sampler timer expirations coalesced/missed (SMP-3) */
    uint64_t state_reseeds_total;      /* frozen-stale state_map entries reseeded by
                                        * pgwt_sweep_stale_state (seed→arm race repair) */

    /* T8 measured-CPU observability (docs/ROADMAP_AND_STATUS.md).
     * Lifetime totals over the exact tier's measured intervals — the closed
     * ringbuf transitions plus the terminal/de-escalation flush records (open
     * intervals are display-time only and never counted here). All stay 0 on a
     * legacy-capability daemon (cpu_ns carries the UNKNOWN sentinel).
     *  - cpu_ns_total:          measured on-CPU ns of we==0 gaps (clamped to gap)
     *  - offcpu_ns_total:       the gap−cpu remainder (runqueue/throttle/unacct)
     *  - cpu_clamped_ns_total:  cpu_ns that EXCEEDED its gap wall (clock skew)
     *  - wait_gap_cpu_ns_total: cpu measured during WAIT-labeled gaps — the
     *                           self-check quantity (should stay ≈0: a sleeping
     *                           task burns no CPU). */
    uint64_t cpu_ns_total;
    uint64_t offcpu_ns_total;
    uint64_t cpu_clamped_ns_total;
    uint64_t wait_gap_cpu_ns_total;

    /* T2 decomposed-AAS observability (docs/AAS_SEMANTICS_DECISION.md). */
    uint64_t noncmd_cpu_samples_total; /* client we==0 readings outside a command (not recorded) */
    uint64_t cmd_gate_recovered_total; /* on-CPU client samples the edge-gate missed, recovered
                                        * from debug_query_string ground truth (EL9 fix) */
    uint64_t io_worker_samples_total;  /* io_worker readings taken (excluded from AAS) */
    uint64_t io_worker_busy_total;     /* ... of which busy (on-CPU or a real wait) */
    uint64_t prev_io_worker_samples;   /* snapshots at previous display tick */
    uint64_t prev_io_worker_busy;
    double   io_worker_busy_pct;       /* busy% over the last display interval */
};

/* View modes */
enum pgwt_view {
    PGWT_VIEW_TIME_MODEL = 0,
    PGWT_VIEW_SYSTEM_EVENT,
    PGWT_VIEW_SESSION_EVENT,
    PGWT_VIEW_HISTOGRAM,
    PGWT_VIEW_QUERY_EVENT,
    PGWT_VIEW_ACTIVE,
};

/* Exit reasons (for supervision loop) */
enum pgwt_exit_reason {
    PGWT_EXIT_NORMAL = 0,   /* signal, duration, or count */
    PGWT_EXIT_PG_DEAD,      /* postmaster died */
};

/* Sort modes (for active sessions view) */
enum pgwt_sort_mode {
    PGWT_SORT_WAIT_TIME = 0,  /* default: by current wait duration */
    PGWT_SORT_DB_TIME,
    PGWT_SORT_PID,
    PGWT_SORT_EVENT,
};

/* Output formats */
enum pgwt_format {
    PGWT_FMT_TUI = 0,   /* screen-clearing interactive (default for TTY) */
    PGWT_FMT_TEXT,       /* no screen clear, timestamp per interval (default for pipe) */
    PGWT_FMT_JSON,       /* JSONL — one JSON object per interval (future) */
    PGWT_FMT_CSV,        /* flat rows, one per event per interval (future) */
};

struct pgwt_daemon {
    /* BPF */
    struct pg_wait_tracer_bpf *skel;
    struct ring_buffer *rb;          /* lifecycle ringbuf consumer */
    struct ring_buffer *event_rb;    /* trace event ringbuf consumer */

    /* epoll */
    int epoll_fd;
    int timer_fd;
    int sample_timer_fd;   /* high-rate sampler tick (sampled/tiered); -1 if unused */
    int signal_fd;

    /* Configuration */
    pid_t       postmaster_pid;
    /* Address of the global the daemon dereferences to reach each backend's
     * wait_event_info. On PG17+ this is `my_wait_event_info` (a uint32* that
     * already points AT the field). On PG<17 (use_myproc=true) it is the
     * `MyProc` PGPROC* global; dereferencing it yields the backend's PGPROC,
     * to which pgproc_wait_offset is added to reach wait_event_info. */
    uint64_t    my_wait_ptr_addr;
    bool        use_myproc;          /* PG<17: my_wait_ptr_addr is MyProc (PGPROC*) */
    int         pgproc_wait_offset;  /* offsetof(PGPROC, wait_event_info); 0 if N/A */
    bool        wait_offset_validated; /* PG<17: a resolved addr held a sane value */
    uint64_t    my_be_entry_addr; /* address of MyBEEntry (for query_id) */
    uint64_t    debug_query_string_addr; /* VA of debug_query_string global;
                                          * non-NULL in a backend == inside a
                                          * command (same window pg_stat_activity
                                          * state='active' spans). Read per-pid by
                                          * the sampler as the race-free command
                                          * gate (0 = symbol not resolved). */
    int         interval;         /* seconds */
    int         duration;         /* seconds, 0 = unlimited */
    enum pgwt_view view;
    const char *event_filter;     /* for histogram view */
    pid_t       pid_filter;       /* for session_event detail */
    bool        verbose;
    bool        quiet;               /* suppress view output (record only) */
    int         pg_major_version;   /* 14, 15, 16, 17, or 18 */
    int         st_query_id_offset; /* 0 = not available */
    int         st_activity_offset; /* st_activity_raw in PgBackendStatus, 0 = N/A */

    /* PG13 query attribution (Route B1 via pg_stat_statements). PG13 has no
     * in-core query_id; when pgss is loaded its post_parse_analyze hook
     * populates PlannedStmt.queryId (matching pg_stat_statements.queryid). We
     * uprobe ExecutorStart(QueryDesc*) and walk QueryDesc->plannedstmt->queryId
     * into the SAME state_map query_id slot the PG17+ path uses, so both tiers
     * work unchanged. Active only when use_pg13_query_attr is true. */
    bool        pgss_loaded;            /* pg_stat_statements in postmaster maps */
    bool        use_pg13_query_attr;    /* PG13 + pgss: use ExecutorStart uprobe */
    int         pg13_qd_plannedstmt_off; /* offsetof(QueryDesc, plannedstmt) */
    int         pg13_ps_queryid_off;     /* offsetof(PlannedStmt, queryId) */
    int         pg13_qd_sourcetext_off;  /* offsetof(QueryDesc, sourceText) */
    enum pgwt_format format;         /* output format (TUI/TEXT/JSON/CSV) */
    int         count;               /* max intervals, 0 = unlimited */
    int         tick;                /* current interval number */
    uint64_t    query_id_filter;     /* filter query_event to one query, 0 = no filter */
    enum pgwt_sort_mode sort_mode;   /* sort mode for active view */
    bool        replay_mode;             /* true when running --replay */
    bool        daemon_mode;             /* true when running --daemon */

    /* Capture tier (A2). mode picks the provider; default PGWT_MODE_FULL
     * (today's watchpoint behavior). sample_rate_hz is the sampled tier's
     * fixed rate (1..1000, default 10). */
    enum pgwt_mode mode;
    int         sample_rate_hz;
    int         escalation_budget_s;     /* tiered: full-fidelity s / rolling hour */
    const struct pgwt_capture_provider *provider; /* active provider vtable */
    struct pgwt_sampler *sampler;        /* sampled-tier state, NULL if unused */
    struct pgwt_escalation escalation;   /* tiered escalation engine (D5/A4) */
    struct pgwt_anomaly anomaly;         /* anomaly-trigger rules engine (D5/A5) */
    /* Anomaly config (parsed flags; copied into anomaly state after init so
     * --anomaly-* overrides apply on top of the rate-derived defaults). */
    double      anomaly_aas_factor;      /* --anomaly-aas-factor (<=0 = default) */
    double      anomaly_aas_abs_floor;   /* --anomaly-aas-abs-floor (<=0 = default) */
    double      anomaly_aas_abs_delta;   /* --anomaly-aas-abs-delta (<=0 = default) */
    double      anomaly_aas_secondary_factor; /* --anomaly-aas-secondary-factor */
    int         anomaly_aas_ticks;       /* --anomaly-aas-ticks (<=0 = default) */
    double      anomaly_lock_fraction;   /* --anomaly-lock-fraction (<0 = default) */
    double      anomaly_lock_min_aas;    /* --anomaly-lock-min-aas (<0 = default, ESC-4) */
    int         anomaly_cooldown_s;      /* --anomaly-cooldown-s (<0 = default) */
    int         anomaly_window_s;        /* --anomaly-window-s: per-trigger
                                          * escalation duration (<=0 = default) */
    /* T2: the pgstat_report_activity uprobe attached and the BackendState
     * layout is known — the command-open gate is live. When false, we==0
     * classification falls back to the pre-T2 behavior (all exact CPU counts
     * as CPU) instead of silently mislabeling everything idle. */
    bool        cmd_gate_active;
    /* T8 measured CPU (docs/ROADMAP_AND_STATUS.md). cpu_accounting: the
     * exact tier measures per-interval CPU (BPF se.sum_exec_runtime deltas) and
     * the trace carries real cpu_ns; when false the daemon runs legacy
     * gap-inference and stamps the UNKNOWN sentinel. schedstat_ok: field 1 of
     * /proc/<pid>/schedstat is readable, so the userspace seed/flush/live paths
     * can read the same accumulator. The startup probe sets both (and reports
     * the tier loudly + in control-socket status). */
    bool        cpu_accounting;
    bool        schedstat_ok;
    uint32_t    lightweight_mode;        /* 1 = BPF accumulator only (no ringbuf) */
    uint32_t    skip_query_id;          /* 1 = skip query_id reads in BPF */
    uint32_t    skip_usdt;             /* 1 = skip USDT query lifecycle probes */
    enum pgwt_exit_reason exit_reason;   /* why the event loop exited */
    char        pgdata[512];             /* stored for restart detection */

    /* Trace file recording */
    const char *trace_dir;                  /* NULL = disabled */
    int         trace_retention;            /* hours, default 24 */
    double      trace_retention_gb;          /* size cap in GiB, 0 = off (DUR-3) */
    const char *trace_group;                /* group for trace files, default "dba" */
    struct pgwt_event_writer *event_writer;     /* NULL if disabled */
    struct pgwt_summary_writer *summary_writer; /* NULL if disabled */
    struct pgwt_query_text_capture *query_text_capture; /* NULL if disabled */
    struct pgwt_backend_meta_writer *backend_meta;       /* NULL if disabled */

    /* Time windows */
#define PGWT_MAX_WINDOWS 3
    int         num_windows;                    /* 0 = no windowing */
    int         windows[PGWT_MAX_WINDOWS];      /* window sizes in seconds */
    struct pgwt_ring ring;                       /* snapshot ring buffer */

    /* Event stream */
    struct pgwt_accumulator *event_accum;   /* heap-allocated, cumulative from events */

    /* Control socket (D4) — created when trace_dir is set */
    struct pgwt_control *control;           /* NULL if disabled/unavailable */
    struct pgwt_counters counters;          /* self-observability (D6) */

    /* Log-once latches for loud degradation warnings (T4). The counters
     * above keep counting; the log fires once so it cannot flood. */
    bool state_map_full_logged;      /* CAP-1 */
    bool seen_qids_full_logged;      /* CAP-6 */
    bool invalid_wait_reads_logged;  /* CAP-2/5 backstop */

    /* State */
    struct pgwt_backend_table backends;
    struct pgwt_accumulator accum;
    volatile bool running;
    uint64_t start_ts;

    /* PGWT_DEBUG_DUMP_STATE main-loop liveness probe. These fields are read
     * and updated only when the debug env is set; production takes no clocks
     * and emits no diagnostics. debug_timer_entries is deliberately separate
     * from tick: tick advances only after handle_timer completes, so it cannot
     * tell "never entered" from "entered and stalled before completion". */
    bool        debug_dump_state;
    uint64_t    debug_loop_iterations;
    uint64_t    debug_timer_entries;
    uint64_t    debug_timer_expirations;
    uint64_t    debug_max_timer_expirations;
    uint64_t    debug_last_loop_ts_ns;
    uint64_t    debug_max_loop_gap_ns;
    int         debug_timer_settime_rc;   /* 0 or saved -errno */
    int         debug_timer_epoll_rc;     /* 0 or saved -errno */

    /* PGWT_DEBUG_SAMPLER_TRACE per-observation AAS-1 diagnostic. Cached once
     * per daemon lifecycle; when false the sampler takes no diagnostic clocks
     * and emits no STATEDUMP-SAMP lines. */
    bool        debug_sampler_trace;

    /* Placed at end of struct to survive field overflow corruption */
    char       *pg_binary_saved;        /* heap-allocated postgres binary path for USDT */
};

/* PGWT_DEBUG_DUMP_STATE duration probe shared by the daemon loop and the
 * synchronous backend/watchpoint paths it dispatches. The env-off case is
 * inline: no helper call and, critically, no clock syscall. Durations above
 * 50ms emit one STATEDUMP-BLOCK line. */
uint64_t pgwt_debug_monotonic_ns(void);
void pgwt_debug_block_report(const char *op, pid_t pid, uint64_t started_ns);

static inline uint64_t pgwt_debug_block_begin(const struct pgwt_daemon *d)
{
    return d->debug_dump_state ? pgwt_debug_monotonic_ns() : 0;
}

static inline void pgwt_debug_block_end(const struct pgwt_daemon *d,
                                        const char *op, pid_t pid,
                                        uint64_t started_ns)
{
    (void)d;
    if (started_ns)
        pgwt_debug_block_report(op, pid, started_ns);
}

/* True when watchpoints should be attached RIGHT NOW.
 *   FULL    — always (the original watchpoint behavior).
 *   SAMPLED — never (registry only, the sampler reads memory directly).
 *   TIERED  — only while escalated: the sampler runs always-on and full
 *             fidelity is armed for bounded windows. De-escalated, tiered
 *             behaves exactly like sampled (no traps on PG).
 * Used by the fork/exit lifecycle to decide whether a new backend gets a
 * bootstrap watchpoint, and by the daemon to gate watchpoint-only setup. */
static inline bool pgwt_mode_uses_watchpoints(const struct pgwt_daemon *d)
{
    if (d->mode == PGWT_MODE_SAMPLED)
        return false;
    if (d->mode == PGWT_MODE_TIERED)
        return d->escalation.active;
    if (d->mode == PGWT_MODE_COOP)
        return false;   /* A6: capture comes from the extension, not watchpoints
                           (and the stub refuses activation in this build) */
    return true;   /* PGWT_MODE_FULL */
}

/* True when the mode runs the always-on userspace sampler (sampled + tiered).
 * Distinct from pgwt_mode_uses_watchpoints(): in tiered mode BOTH can be true
 * at once (sampler always on, watchpoints on during an escalation window). */
static inline bool pgwt_mode_uses_sampler(const struct pgwt_daemon *d)
{
    return d->mode == PGWT_MODE_SAMPLED || d->mode == PGWT_MODE_TIERED;
}

/* Initialize daemon: load BPF, attach tracepoints, scan backends. */
int pgwt_daemon_init(struct pgwt_daemon *d);

/* Run the main event loop. Returns on signal or duration timeout. */
int pgwt_daemon_run(struct pgwt_daemon *d);

/* Clean up all resources. */
void pgwt_daemon_cleanup(struct pgwt_daemon *d);

/* T8: fold one measured interval's cpu_ns into the lifetime CPU counters
 * (docs/ROADMAP_AND_STATUS.md). Call once per emitted exact-tier
 * TRANSITIONS record — the closed ringbuf path (event_stream.c) and the
 * terminal/de-escalation flush (escalation.c). A marker or an UNKNOWN cpu_ns
 * is a no-op. we==0 splits into cpu_ns_total (clamped to dur) + offcpu_ns_total
 * (+ cpu_clamped_ns_total for the overshoot); a wait-labeled gap folds its
 * cpu_ns into wait_gap_cpu_ns_total (the ≈0 self-check). */
void pgwt_counters_add_cpu(struct pgwt_daemon *d, uint32_t we,
                            uint64_t dur_ns, uint64_t cpu_ns);

#endif /* PGWT_DAEMON_H */
