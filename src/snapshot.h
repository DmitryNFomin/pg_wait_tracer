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
    /* Set by pgwt_ring_delta only (#202): the amount by which this window's
     * top-level rows OVERSHOOT its DB Time, i.e.
     *   max(0, cpu + offcpu + Sigma(wait classes) - db_time_ns).
     * 0 for a pushed snapshot, and 0 for a healthy window — the live
     * accumulator's conservation contract (map_reader.h) makes it exact.
     *
     * clamped_fields only sees a field going DOWN, so it is blind to this
     * direction, which is the one #202 arrived in: nothing decreased, no
     * clamp fired, and the rows still summed to 120% of DB Time. This is
     * the detector for it, so a future third mechanism reports itself in
     * metrics (ring_delta_overshoot_ns_total) instead of waiting to be
     * noticed by a gate-box test. Under-shoot is deliberately NOT counted:
     * it is the direction a residual row absorbs silently and is not
     * evidence of double counting. */
    uint64_t overshoot_ns;
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
