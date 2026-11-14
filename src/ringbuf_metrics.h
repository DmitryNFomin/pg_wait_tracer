/* Full-provider BPF drop metrics. Portable so map reads can be unit-tested
 * without libbpf or a loaded BPF object. Refs #871. */
#ifndef PGWT_RINGBUF_METRICS_H
#define PGWT_RINGBUF_METRICS_H

#include "provider.h"
#include "counter_vector.h"

static const char *const pgwt_ringbuf_site_metric_names[PGWT_RING_FAIL_SITE_MAX] = {
    [PGWT_RING_FAIL_WAIT_TRANSITION] = "ringbuf_wait_transition_drops_total",
    [PGWT_RING_FAIL_EXIT_TRANSITION] = "ringbuf_exit_transition_drops_total",
    [PGWT_RING_FAIL_MARKER] = "ringbuf_marker_drops_total",
    [PGWT_RING_FAIL_COMMAND_BOUNDARY] = "ringbuf_command_boundary_drops_total",
    [PGWT_RING_FAIL_LIFECYCLE_INIT_HINT] = "lifecycle_init_hint_drops_total",
    [PGWT_RING_FAIL_LIFECYCLE_FORK] = "ringbuf_lifecycle_fork_drops_total",
    [PGWT_RING_FAIL_LIFECYCLE_EXIT] = "ringbuf_lifecycle_exit_drops_total",
    [PGWT_RING_FAIL_QUERY_TEXT] = "ringbuf_query_text_drops_total",
    [PGWT_RING_FAIL_PG13_QUERY_TEXT] = "ringbuf_pg13_query_text_drops_total",
};

/* Preserve the aggregate's event-ring-only meaning. Unknown is explicit;
 * callers must not present either zero or a previous successful read. */
static inline void pgwt_read_ringbuf_metrics(struct pgwt_metrics *m,
                                             int aggregate_fd, int site_fd,
                                             int ncpu, pgwt_counter_lookup_fn lookup)
{
    struct pgwt_counter_value c = pgwt_read_percpu_counter(aggregate_fd, 0, ncpu, lookup);
    m->ringbuf_drops_total = c.value;
    m->ringbuf_drops_valid = c.valid;
    for (uint32_t site = 0; site < PGWT_RING_FAIL_SITE_MAX; ++site) {
        c = pgwt_read_percpu_counter(site_fd, site, ncpu, lookup);
        m->ringbuf_site_drops[site] = c.value;
        m->ringbuf_site_valid[site] = c.valid;
    }
}

#endif
