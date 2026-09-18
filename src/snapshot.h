/* snapshot.h — Compact snapshots and ring buffer for time-windowed analysis */
#ifndef PGWT_SNAPSHOT_H
#define PGWT_SNAPSHOT_H

#include "map_reader.h"

#include <stdint.h>

#define MAX_SNAP_EVENTS  512
#define MAX_SNAP_QUERIES 1024

/* Compact per-event snapshot: additive counters only (no min/max) */
struct pgwt_snap_event {
    uint32_t wait_event;
    uint64_t count;
    uint64_t total_ns;
    uint64_t histogram[HISTOGRAM_BUCKETS];
};

/* Compact per-(query, event) snapshot */
struct pgwt_snap_query_event {
    uint64_t query_id;
    uint32_t wait_event;
    uint64_t count;
    uint64_t total_ns;
};

/* One point-in-time snapshot of cumulative state */
struct pgwt_snapshot {
    /* Set by pgwt_ring_delta only: number of fields whose "cumulative"
     * counter went DOWN across the window and were clamped to 0 instead of
     * wrapping (see sat_sub in snapshot.c). 0 for a pushed snapshot. The
     * daemon folds it into counters.ring_delta_clamps_total (metrics). */
    uint32_t clamped_fields;
    struct pgwt_time_model tm;
    int num_events;
    struct pgwt_snap_event events[MAX_SNAP_EVENTS];
    int num_query_events;
    struct pgwt_snap_query_event query_events[MAX_SNAP_QUERIES];
};

/* Circular buffer of snapshots */
struct pgwt_ring {
    struct pgwt_snapshot *slots;   /* malloc'd array */
    int capacity;                  /* number of slots */
    int head;                      /* next write index (wraps around) */
    int count;                     /* valid entries (≤ capacity) */
};

/* Allocate ring buffer with given capacity. Returns 0 on success. */
int pgwt_ring_init(struct pgwt_ring *ring, int capacity);

/* Free ring buffer memory. */
void pgwt_ring_free(struct pgwt_ring *ring);

/* Compact current accumulator state and push as a new snapshot. */
void pgwt_ring_push(struct pgwt_ring *ring, const struct pgwt_accumulator *acc);

/* Compute delta between latest snapshot and ticks_ago snapshot.
 * Writes result to out. Returns 0 on success, -1 if not enough history. */
int pgwt_ring_delta(const struct pgwt_ring *ring, int ticks_ago,
                    struct pgwt_snapshot *out);

/* Get the latest snapshot. Returns NULL if ring is empty. */
const struct pgwt_snapshot *pgwt_ring_latest(const struct pgwt_ring *ring);

#endif /* PGWT_SNAPSHOT_H */
