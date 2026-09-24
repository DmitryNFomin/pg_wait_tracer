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

/* Per-pid command-window sweep state for the LIVE paths (issue #98) — the
 * live twin of compute.c pgwt_tag_events' tag_pid_state. Driven by the
 * CMD_START/CMD_END markers the on_report_activity uprobe emits into the
 * same per-pid event stream; consumed by every closed record and peeked by
 * the open state_map stretch at tick time. */
struct pgwt_live_cmd_gate {
    uint8_t  seen;        /* a CMD marker was seen for this pid (else: unmarked) */
    uint8_t  open;        /* inside a command window at the sweep point */
    uint64_t anchor_ns;   /* start of the current open run, or the last record end */
    uint64_t banked_ns;   /* closed command-open ns since the last record */
};

/* Per-PID accumulator */
struct pgwt_pid_accum {
    uint32_t pid;
    bool     active;          /* has data */
    /* T2: cached process category for live accumulation (0 = not yet
     * resolved against the registry; else 1 + a PGWT_EVENT_FLAG_* category
     * bit or 1 + 0 for foreground). Lets the hot event path skip the linear
     * registry scan after the first event of a pid. Only a RESOLVED category
     * (registry entry with parsed metadata) is cached (#97 review nit b): a
     * pid seen before its argv rewrite must not pin as foreground for life;
     * cat_scan_tick (1 + the display tick of the last unresolved scan)
     * throttles the rescan to once per tick. */
    uint32_t cat_flag_plus1;
    int      cat_scan_tick;
    struct pgwt_live_cmd_gate cmd_gate;   /* issue #98, see above */
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
 * registry and cached in the pid's accumulator entry `pa` (NULL when the
 * accumulator is full: resolved, not cached). Shared by the closed-record
 * path (event_stream.c) and the open-interval scan; both pass the pa they
 * already hold, so the hot path does ONE pid lookup per event. */
uint32_t pgwt_live_pid_cat_flag(struct pgwt_daemon *d,
                                struct pgwt_pid_accum *pa, uint32_t pid);

/* How the LIVE view decides whether a client backend's on-CPU interval is
 * in-command (issue #98) — for the startup header and control-socket
 * status: "markers" (CMD_START/CMD_END sweep, majority rule) or "ungated"
 * (command gate unavailable: every client we==0 counts as CPU*). */
const char *pgwt_live_cpu_gate_name(const struct pgwt_daemon *d);

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

/* ── Live-view interval accounting (issues #97, #98) ─────────────────────
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
 * bucket agree. Everything else is returned unchanged.
 *
 * WHETHER an interval is in-command (#98) is decided by TIME, not by the
 * gate value at emission: PostgreSQL calls pgstat_report_activity(IDLE)
 * before the post-command ClientRead begins, so the record that closes a
 * waitless statement's whole on-CPU run is emitted with the gate already
 * clear — read at emission, the entire run looked like non-command CPU
 * (~83% of pgbench's CPU dropped from live DB Time on the gate box: the
 * 4.6-7.1x server/CLI CPU ratio). Both live paths now sweep the pid's
 * CMD_START/CMD_END markers (pgwt_live_cmd_gate_*) and apply the server's
 * majority rule — in-command iff at least half of the interval's wall
 * overlaps a command window (compute.c pgwt_tag_events, same arithmetic).
 * A pid with no marker yet, or a daemon whose gate is inactive, keeps
 * we==0 as CPU* exactly like the server's seen_cmd rule — reported, never
 * silent (header + status live_cpu_gate, metrics live_cmd_markers_total /
 * live_cpu_unmarked_ns_total / live_cpu_gate_fallback_total). */
uint32_t pgwt_live_effective_event(uint32_t we, uint32_t cat_flag,
                                   bool cmd_gate_active, bool cmd_open);

/* Marker sweep: feed a CMD_START/CMD_END marker (timestamp = the boundary
 * instant) into the pid's gate. Other markers are ignored. */
void pgwt_live_cmd_gate_marker(struct pgwt_live_cmd_gate *g, uint32_t marker,
                               uint64_t ts_ns);

/* Command-open ns inside the closed record [t0, t1) — the banked closed
 * runs since the previous record plus the currently-open run clipped to the
 * interval — and ADVANCE the sweep to t1 (records of one pid are contiguous
 * in an exact span, so banked time since the previous record belongs to
 * this one). Mirrors pgwt_tag_events' per-record step; 0 for an unmarked
 * pid. */
uint64_t pgwt_live_cmd_gate_take(struct pgwt_live_cmd_gate *g,
                                 uint64_t t0_ns, uint64_t t1_ns);

/* Same quantity for the OPEN stretch [t0, now) at tick time WITHOUT
 * advancing (its closing record consumes it later), clipped to the
 * stretch's wall: state_map's last_ts can run ahead of the last drained
 * record, so banked time may predate t0. */
uint64_t pgwt_live_cmd_gate_peek(const struct pgwt_live_cmd_gate *g,
                                 uint64_t t0_ns, uint64_t now_ns);

/* The server's majority rule (pgwt_tag_events): a zero-length interval
 * takes the instantaneous gate; otherwise in-command iff at least half of
 * the wall overlaps a command window. */
bool pgwt_live_cmd_majority(uint64_t open_ns, uint64_t wall_ns,
                            bool open_now);

/* Resolve an interval's in-command flag from the pid's gate `g` (NULL = no
 * per-pid state: accumulator full). `consume` = closed record (advance the
 * sweep), else the open stretch (peek). Returns true when the pid's markers
 * decided it; false when it fell back — to in-command for an unmarked pid
 * (the server's seen_cmd rule) or to `fallback_open` (the emission-time
 * gate) when g is NULL. */
bool pgwt_live_cmd_gate_classify(struct pgwt_live_cmd_gate *g, bool consume,
                                 uint64_t t0_ns, uint64_t t1_ns,
                                 bool fallback_open, bool *in_cmd);

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
    bool     cmd_open;        /* in-command by the marker majority rule (#98) */
    bool     closed;          /* closed record: also feeds the histograms */
    struct pgwt_pid_accum *pa; /* pre-resolved per-pid entry (NULL = look up) */
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
