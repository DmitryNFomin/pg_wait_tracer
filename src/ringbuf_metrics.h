/* Full-provider BPF drop metrics. Portable so map reads can be unit-tested
 * without libbpf or a loaded BPF object. Refs #871. */
#ifndef PGWT_RINGBUF_METRICS_H
#define PGWT_RINGBUF_METRICS_H

#include "provider.h"

static const char *const pgwt_ringbuf_site_metric_names[PGWT_RING_FAIL_SITE_MAX] = {
    [PGWT_RING_FAIL_WAIT_TRANSITION] = "ringbuf_wait_transition_drops_total",
    [PGWT_RING_FAIL_EXIT_TRANSITION] = "ringbuf_exit_transition_drops_total",
    [PGWT_RING_FAIL_MARKER] = "ringbuf_marker_drops_total",
    [PGWT_RING_FAIL_COMMAND_BOUNDARY] = "ringbuf_command_boundary_drops_total",
    [PGWT_RING_FAIL_LIFECYCLE_INIT] = "ringbuf_lifecycle_init_drops_total",
    [PGWT_RING_FAIL_LIFECYCLE_FORK] = "ringbuf_lifecycle_fork_drops_total",
    [PGWT_RING_FAIL_LIFECYCLE_EXIT] = "ringbuf_lifecycle_exit_drops_total",
    [PGWT_RING_FAIL_QUERY_TEXT] = "ringbuf_query_text_drops_total",
    [PGWT_RING_FAIL_PG13_QUERY_TEXT] = "ringbuf_pg13_query_text_drops_total",
};

typedef int (*pgwt_ringbuf_lookup_fn)(int fd, const void *key, void *value);

/* Preserve the aggregate's event-ring-only meaning. Site slots also include
 * lifecycle/query-text reserve failures, so their sum is a different count.
 * Never reset a live counter. A failed lookup leaves that metric untouched. */
static inline void pgwt_read_ringbuf_metrics(struct pgwt_metrics *m,
                                             int aggregate_fd, int site_fd,
                                             int ncpu, pgwt_ringbuf_lookup_fn lookup)
{
    if (ncpu <= 0)
        ncpu = 1;
    uint64_t per_cpu[ncpu];
    uint32_t key = 0;
    if (aggregate_fd >= 0 && lookup(aggregate_fd, &key, per_cpu) == 0) {
        uint64_t total = 0;
        for (int cpu = 0; cpu < ncpu; ++cpu)
            total += per_cpu[cpu];
        m->ringbuf_drops_total = total;
    }
    if (site_fd < 0)
        return;
    for (key = 0; key < PGWT_RING_FAIL_SITE_MAX; ++key) {
        if (lookup(site_fd, &key, per_cpu) != 0)
            continue;
        uint64_t total = 0;
        for (int cpu = 0; cpu < ncpu; ++cpu)
            total += per_cpu[cpu];
        m->ringbuf_site_drops[key] = total;
    }
}

#endif
