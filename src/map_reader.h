/* map_reader.h — BPF map reading, per-CPU summing, accumulation */
#ifndef PGWT_MAP_READER_H
#define PGWT_MAP_READER_H

#include "pg_wait_tracer.h"

#include <stdint.h>
#include <stdbool.h>

/* Per-event stats (userspace accumulator) */
struct pgwt_event_stats {
    uint32_t wait_event;
    uint64_t count;
    uint64_t total_ns;
    uint64_t min_ns;
    uint64_t max_ns;
    uint64_t histogram[HISTOGRAM_BUCKETS];
};

/* Per-PID accumulator */
struct pgwt_pid_accum {
    uint32_t pid;
    bool     active;          /* has data */
    /* T2: cached process category for live accumulation (0 = not yet
     * resolved against the registry; else 1 + a PGWT_EVENT_FLAG_* category
     * bit or 1 + 0 for foreground). Lets the hot event path skip the linear
     * registry scan after the first event of a pid. */
    uint32_t cat_flag_plus1;
    int      num_events;
    uint64_t db_time_ns;      /* total non-idle time */
    uint64_t cpu_time_ns;     /* event=0 time */
    uint64_t wait_time_ns;    /* all event!=0 time */
    uint32_t current_event;   /* current wait event from state_map */
    uint64_t current_wait_ns; /* time in current state */
    struct pgwt_event_stats events[MAX_EVENTS_PER_PID];
};

/* Time model (system-wide) */
struct pgwt_time_model {
    uint64_t db_time_ns;
    uint64_t cpu_time_ns;
    uint64_t io_time_ns;
    uint64_t lwlock_time_ns;
    uint64_t lock_time_ns;
    uint64_t bufferpin_time_ns;
    uint64_t client_time_ns;
    uint64_t ipc_time_ns;
    uint64_t timeout_time_ns;
    uint64_t extension_time_ns;
    uint64_t activity_time_ns;
};

/* Per-(query_id, event) stats */
struct pgwt_query_event_stats {
    uint64_t query_id;
    uint32_t wait_event;
    uint64_t count;
    uint64_t total_ns;
    uint64_t min_ns;
    uint64_t max_ns;
};
#define MAX_QUERY_EVENTS  4096

/* Full accumulator state */
struct pgwt_accumulator {
    /* Current snapshot */
    struct pgwt_pid_accum pids[MAX_BACKENDS];
    int num_pids;
    struct pgwt_time_model tm;

    /* Previous snapshot (for delta) */
    struct pgwt_time_model prev_tm;

    /* System-wide event aggregation */
    struct pgwt_event_stats system_events[4096];
    int num_system_events;

    /* Query-level event aggregation */
    struct pgwt_query_event_stats query_events[MAX_QUERY_EVENTS];
    int num_query_events;
};

/* Forward */
struct pgwt_daemon;

/* Initialize accumulator. */
void pgwt_accum_init(struct pgwt_accumulator *acc);

/* Read state_map for open intervals and current state (active view). */
void pgwt_read_state_map(struct pgwt_daemon *d);

/* T2 category flag (0 foreground / PGWT_EVENT_FLAG_{IO_WORKER,MAINT,
 * BACKGROUND}) of a pid for live accumulation, resolved against the
 * registry once and cached in the accumulator's pid entry. Shared by the
 * closed-record path (event_stream.c) and the open-interval scan. */
uint32_t pgwt_live_pid_cat_flag(struct pgwt_daemon *d,
                                struct pgwt_accumulator *acc, uint32_t pid);

/* Read BPF accum_map (lightweight mode) and merge into event_accum. */
void pgwt_read_accum_map(struct pgwt_daemon *d);

/* Sum one BPF fail_counters slot (PGWT_BPF_FAIL_*) across CPUs (CAP-1/6).
 * Returns 0 when the skeleton/map is unavailable. */
uint64_t pgwt_read_bpf_fail_counter(struct pgwt_daemon *d, uint32_t slot);

/* Find per-PID accumulator. Returns NULL if not found. */
struct pgwt_pid_accum *pgwt_find_pid_accum(struct pgwt_accumulator *acc, uint32_t pid);

/* Find system-wide event stats. Returns NULL if not found. */
struct pgwt_event_stats *pgwt_find_system_event(struct pgwt_accumulator *acc,
                                                 uint32_t wait_event);

/* Get or create helpers — shared between map_reader and event_stream */
struct pgwt_pid_accum *pgwt_get_or_create_pid(struct pgwt_accumulator *acc, uint32_t pid);
struct pgwt_event_stats *pgwt_get_or_create_event(struct pgwt_pid_accum *pa, uint32_t we);
struct pgwt_event_stats *pgwt_get_or_create_system_event(struct pgwt_accumulator *acc,
                                                          uint32_t we);
struct pgwt_query_event_stats *pgwt_get_or_create_query_event(
    struct pgwt_accumulator *acc, uint64_t query_id, uint32_t we);

/* Update time model by wait event class. */
void pgwt_update_time_model(struct pgwt_time_model *tm, uint32_t event,
                             uint64_t duration_ns);

/* ── Live-view interval accounting (issue #97) ───────────────────────────
 *
 * The live accumulators are fed from TWO paths that must classify an
 * interval identically: the CLOSED trace record (event_stream.c, at the
 * watchpoint transition) and the still-OPEN [last_ts, now) stretch read
 * from state_map at every tick (pgwt_read_state_map). The multi-window
 * view differences ring snapshots of the same accumulator, so any field
 * one path adds and the other does not shows up as a per-window delta
 * that no single snapshot ever had — #97's ">100% of DB Time" was exactly
 * that: the closed path filed a client backend's non-command we==0 record
 * under the CPU* row (wait_event 0) while routing its time to the idle
 * Activity bucket, so the CPU* row exceeded DB Time by the non-command CPU.
 *
 * pgwt_live_effective_event() is the ONE classification both paths use:
 * a foreground (client) on-CPU interval outside a command is idle
 * post/between-command time (docs/AAS_SEMANTICS_DECISION.md) and is
 * filed under PGWT_WEI_NONCMD_CPU — the same synthetic id the server's
 * tagging pass (compute.c pgwt_tag_events) assigns — so every row and
 * bucket agree. Everything else is returned unchanged. */
uint32_t pgwt_live_effective_event(uint32_t we, uint32_t cat_flag,
                                   bool cmd_gate_active, bool cmd_open);

/* One live interval to fold into an accumulator. Pure: no daemon, no BPF. */
struct pgwt_live_interval {
    uint32_t pid;
    uint32_t we;              /* RAW wait_event_info (0 = on-CPU) */
    uint64_t wall_ns;         /* wall duration — DB Time and every wait/idle row */
    uint64_t cpu_ns;          /* on-CPU ns for a we==0 interval: the CPU* row and
                               * tm.cpu_time_ns (measured for the open stretch;
                               * the closed record passes wall_ns — its display
                               * accounting is wall, see ROADMAP "Multi-window
                               * %DB"). Ignored when we != 0. */
    uint64_t query_id;        /* 0 = none */
    uint32_t cat_flag;        /* pid category: 0 foreground, or one of
                               * PGWT_EVENT_FLAG_{IO_WORKER,MAINT,BACKGROUND} */
    bool     cmd_gate_active; /* the command-open gate is maintained */
    bool     cmd_open;        /* gate value for this interval */
    bool     closed;          /* closed record: also feeds the histograms */
};

/* Fold one interval into per-pid, system, query and time-model accumulators.
 * Both live paths call this; the classification is pgwt_live_effective_event.
 * io_worker intervals stay visible in the per-pid/system rows but never enter
 * DB Time / the time model / per-pid load (compute.h category contract). */
void pgwt_accum_add_interval(struct pgwt_accumulator *acc,
                             const struct pgwt_live_interval *iv);

/* Log2 histogram bucket for a duration in nanoseconds. */
uint32_t pgwt_duration_to_bucket(uint64_t ns);

#endif /* PGWT_MAP_READER_H */
