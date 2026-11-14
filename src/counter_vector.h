/* Capture loss accounting, in merge-v1 §4 order. No transport/merge policy. */
#ifndef PGWT_COUNTER_VECTOR_H
#define PGWT_COUNTER_VECTOR_H

#include <stdbool.h>
#include <stdint.h>
#include "pg_wait_tracer.h"

struct pgwt_counter_value {
    uint64_t value;
    bool valid;
};
typedef int (*pgwt_counter_lookup_fn)(int fd, const void *key, void *value);

/* ncpu must be the possible CPU count, not the online CPU count. */
struct pgwt_counter_value pgwt_read_percpu_counter(int fd, uint32_t key,
                                                  int ncpu,
                                                  pgwt_counter_lookup_fn lookup);
struct pgwt_counter_value pgwt_counter_add(struct pgwt_counter_value a,
                                           uint64_t b);

enum pgwt_counter_component {
    PGWT_C_LIFECYCLE_FORK_DROP,
    PGWT_C_LIFECYCLE_INIT_DROP,
    PGWT_C_LIFECYCLE_EXIT_DROP,
    PGWT_C_WAIT_READ_FAIL,
    PGWT_C_QUERY_TEXT_DROP,
    PGWT_C_PG13_QUERY_TEXT_DROP,
    PGWT_C_EXACT_QUERY_UPDATE_FAIL,
    PGWT_C_EXACT_CMD_UPDATE_FAIL,
    PGWT_C_EXACT_PHASE_UPDATE_FAIL,
    PGWT_C_ACCUM_UPDATE_FAIL,
    PGWT_C_FORK_STATE_UPDATE_FAIL,
    PGWT_C_EVENT_RING_DROP,
    PGWT_C_STATE_MAP_UPDATE_FAIL,
    PGWT_C_SEEN_QUERY_IDS_UPDATE_FAIL,
    PGWT_C_WRITER_PUSH_FAIL,
    PGWT_COUNTER_COMPONENTS
};
extern const char *const pgwt_counter_component_names[PGWT_COUNTER_COMPONENTS];
struct pgwt_counter_vector {
    struct pgwt_counter_value components[PGWT_COUNTER_COMPONENTS];
    bool valid;
};
struct pgwt_counter_sources {
    int fail_fd, site_fd, event_ring_fd;
    int ncpu;
    uint64_t lifecycle_init_lost_total;
    uint64_t init_state_read_fail_total;
    uint64_t state_map_full_total;
    uint64_t writer_push_fail_total;
};
struct pgwt_counter_vector pgwt_read_counter_vector(
    const struct pgwt_counter_sources *sources, pgwt_counter_lookup_fn lookup);

#endif
