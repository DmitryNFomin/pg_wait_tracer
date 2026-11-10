/* Independent phase-marker capability, persisted per daemon start. */
#ifndef PGWT_PHASE_CAPABILITY_H
#define PGWT_PHASE_CAPABILITY_H

#include <stddef.h>
#include <stdint.h>
#include <sys/types.h>

#define PGWT_PHASE_CAPABILITY_FILE "phase_capabilities.jsonl"
#define PGWT_PHASE_CAPABILITY_VERSION 1
#define PGWT_PHASE_REASON_LEN 64
enum pgwt_cap_phase { PGWT_CAP_EXEC, PGWT_CAP_PLAN, PGWT_CAP_PHASE_COUNT };
enum pgwt_phase_state {
    PGWT_PHASE_UNKNOWN, PGWT_PHASE_AVAILABLE, PGWT_PHASE_UNAVAILABLE,
    PGWT_PHASE_MIXED
};
enum pgwt_phase_fact { PGWT_PHASE_ERROR = -1, PGWT_PHASE_ABSENT, PGWT_PHASE_PRESENT };

struct pgwt_phase_value {
    enum pgwt_phase_state state;
    char reason[PGWT_PHASE_REASON_LEN];
};
struct pgwt_phase_record {
    uint64_t start_wall_ns;
    char kernel_release[256];
    struct pgwt_phase_value phase[PGWT_CAP_PHASE_COUNT];
};
struct pgwt_phase_range {
    uint64_t from_ns, to_ns; /* half-open Unix wall time */
    struct pgwt_phase_value value;
};
struct pgwt_phase_result {
    struct pgwt_phase_value value;
    struct pgwt_phase_range *ranges;
    size_t n_ranges;
};
struct pgwt_phase_window {
    struct pgwt_phase_result phase[PGWT_CAP_PHASE_COUNT];
};

const char *pgwt_phase_state_name(enum pgwt_phase_state state);
struct pgwt_phase_value pgwt_phase_classify(enum pgwt_phase_fact kernel,
                                           enum pgwt_phase_fact binary);
/* Read PMU ABI only; never opens a perf event or attaches a probe. pmu_dir is
 * /sys/bus/event_source/devices/uprobe in production, a fixture in tests.
 * Missing attribute is ABSENT only inside an otherwise readable PMU. */
enum pgwt_phase_fact pgwt_phase_kernel_fact(const char *pmu_dir);

/* enumerate returns matching site count, or -1 on any inspection failure.
 * Both start and done notes are required, independently for each phase. */
typedef int (*pgwt_phase_note_fn)(void *ctx, const char *note_name);
void pgwt_phase_measure(const char *pmu_dir, pgwt_phase_note_fn enumerate,
                         void *ctx, struct pgwt_phase_value phase[PGWT_CAP_PHASE_COUNT]);

/* One append, permissions 0640 and supplied trace group, flushed to disk.
 * Every failure increments *write_failures and logs WARN. Recovering a
 * previous torn tail also increments/logs this counter, even on success.
 * Complete lines are preserved; only an uncommitted suffix is truncated.
 * Single writer per trace directory. Never exits.
 * Caller owns the counter (normally daemon-lifetime state). */
int pgwt_phase_append(const char *trace_dir, gid_t trace_gid,
                      const struct pgwt_phase_record *record,
                      uint64_t *write_failures);

/* Complete malformed lines are skipped; unsupported versions fail closed.
 * Output is initialized even on failure. Missing/unreadable/invalid files
 * return -1 with UNKNOWN; success returns 0 (possibly UNKNOWN coverage).
 * UNKNOWN anywhere dominates MIXED. Ranges are clipped/coalesced per phase.
 * Records apply until the next start, last record through to_ns. No host
 * fallback. Free output with pgwt_phase_window_free, also after failure. */
int pgwt_phase_read_window(const char *trace_dir, uint64_t from_ns,
                           uint64_t to_ns, struct pgwt_phase_window *out);
void pgwt_phase_window_free(struct pgwt_phase_window *out);

#endif
