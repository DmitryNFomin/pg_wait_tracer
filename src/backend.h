/* backend.h — Backend lifecycle manager: watchpoint attach/detach */
#ifndef PGWT_BACKEND_H
#define PGWT_BACKEND_H

#include "pg_wait_tracer.h"
#include "cmdline.h"
#include "synthetic_query.h"

#include <sys/types.h>
#include <stdint.h>
#include <stdbool.h>

struct pgwt_backend {
    pid_t    pid;
    int      wp_fd;            /* real watchpoint perf_event fd (-1 if none) */
    int      bootstrap_fd;     /* bootstrap watchpoint fd (-1 if none) */
    uint64_t wp_addr;          /* PGPROC->wait_event_info address */
    /* Is wp_addr in a MAP_SHARED mapping? -1 unknown (not yet checked),
     * 0 process-local, 1 shared. SMP-2: the sampler may only batch-read
     * SHARED addresses through one pid; local ones need per-pid reads. */
    int      wp_addr_shared;
    uint64_t attach_ts;        /* monotonic ns when attached */
    bool     is_alive;
    /* meta has been filled by pgwt_parse_cmdline. A zeroed meta reads as
     * backend_type == CLIENT (enum value 0), which is WRONG for an unparsed
     * entry — consumers must treat !meta_parsed as UNKNOWN. The sampled-tier
     * fork path parses lazily (the process title is only set after init). */
    bool     meta_parsed;
    struct pgwt_metadata meta;
    uint32_t databaseid;      /* PgBackendStatus context, 0 until observed */
    uint32_t userid;
    uint64_t pgbs_addr;        /* PG13 cached shared PgBackendStatus row */
    uint64_t pgbs_resolve_retry_ns;
    uint8_t  pgbs_resolve_attempts;
    struct pgwt_pg13_synthetic_cache *pg13_synthetic_cache;
};

struct pgwt_backend_table {
    struct pgwt_backend entries[MAX_BACKENDS];
    int count;
};

/* Forward declaration */
struct pgwt_daemon;

/* Initialize the backend table. */
void pgwt_backend_init(struct pgwt_backend_table *bt);

/* Scan existing backends at startup. Attaches real watchpoints directly
 * (skips bootstrap — they are already initialized). */
int pgwt_scan_existing_backends(struct pgwt_daemon *d);

/* Periodic level-triggered recovery for the initial-scan straddle race: attach
 * any postmaster child that still lacks a live watchpoint but is now resolvable
 * + initialized (the one-shot scan may have missed/limbo'd it under load).
 * Idempotent; safe to call every tick. Returns the count recovered. */
int pgwt_recover_unattached_backends(struct pgwt_daemon *d);

/* Periodic state-consistency sweep for ATTACHED backends (the sibling of the
 * recovery above): reseed any state_map entry frozen on a wait the backend
 * provably left long ago — the seed→arm race, where the seeded wait ended
 * before PERF_EVENT_IOC_ENABLE so its ending write never fired. Conservative
 * two-read staleness predicate; repair drops the never-bounded stale interval
 * rather than fabricating it. Idempotent; safe to call every tick. Returns
 * the count reseeded. */
int pgwt_sweep_stale_state(struct pgwt_daemon *d);

/* Handle a new fork from postmaster. Attaches bootstrap watchpoint. */
int pgwt_handle_fork(struct pgwt_daemon *d, pid_t child_pid);

/* Handle initialization complete (bootstrap fired). Switch to real watchpoint. */
int pgwt_handle_init(struct pgwt_daemon *d, pid_t pid, uint64_t addr);

/* Handle backend exit. Close watchpoint, flush stats, cleanup. */
int pgwt_handle_exit(struct pgwt_daemon *d, pid_t pid);

/* Attach a hardware watchpoint to an already-resolved backend (be->wp_addr
 * must be set) and pre-seed its BPF state_map with the current wait state.
 * Used by the full-mode scan and by the A4 escalation engine. Returns 0 on
 * success, -1 on attach failure (caller decides cleanup). No-op (returns 0)
 * if the backend already has a watchpoint. */
int pgwt_attach_backend_watchpoint(struct pgwt_daemon *d,
                                   struct pgwt_backend *be);

/* Resolve a backend's wait_event_info address using the version-appropriate
 * path (PG17+ my_wait_event_info global vs PG<17 MyProc+offset). Returns 0 if
 * the backend has not yet set the pointer (still in early init). */
uint64_t pgwt_resolve_backend_wait_addr(struct pgwt_daemon *d, pid_t pid);

/* CAP-2/3: confirm the resolved wait_event_info addresses with a NON-ZERO
 * class-valid reading before trusting them. Called once after the initial
 * backend scan when the scan itself produced no proof (every backend read
 * zero at that instant — also exactly what a WRONG offset reads forever).
 * Re-polls the resolved backends briefly; on the PG<17 hardcoded-offset path
 * an unconfirmed offset is FATAL (returns -1, refuse to attach); on PG17+
 * (address derived from the backend's own pointer) it degrades to a loud
 * warning. A garbage reading is FATAL on every version. */
int pgwt_confirm_wait_offset(struct pgwt_daemon *d);

/* Find backend by PID. Returns NULL if not found. */
struct pgwt_backend *pgwt_find_backend(struct pgwt_backend_table *bt, pid_t pid);

/* Close all watchpoints (cleanup). */
void pgwt_close_all_backends(struct pgwt_backend_table *bt);

#endif /* PGWT_BACKEND_H */
