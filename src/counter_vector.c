#include "counter_vector.h"

const char *const pgwt_counter_component_names[PGWT_COUNTER_COMPONENTS] = {
    "lifecycle_fork_drop", "lifecycle_init_drop", "lifecycle_exit_drop",
    "wait_read_fail", "query_text_drop", "pg13_query_text_drop",
    "exact_query_update_fail", "exact_cmd_update_fail", "exact_phase_update_fail",
    "accum_update_fail", "fork_state_update_fail", "event_ring_drop",
    "state_map_update_fail", "seen_query_ids_update_fail", "writer_push_fail",
};

struct pgwt_counter_value pgwt_counter_add(struct pgwt_counter_value a, uint64_t b)
{
    if (!a.valid || UINT64_MAX - a.value < b)
        return (struct pgwt_counter_value){0, false};
    a.value += b;
    return a;
}

struct pgwt_counter_value pgwt_read_percpu_counter(int fd, uint32_t key,
                                                  int ncpu,
                                                  pgwt_counter_lookup_fn lookup)
{
    if (fd < 0 || ncpu <= 0 || !lookup)
        return (struct pgwt_counter_value){0, false};
    uint64_t values[ncpu];
    if (lookup(fd, &key, values) != 0)
        return (struct pgwt_counter_value){0, false};
    struct pgwt_counter_value total = {0, true};
    for (int cpu = 0; cpu < ncpu; ++cpu)
        total = pgwt_counter_add(total, values[cpu]);
    return total;
}

struct pgwt_counter_vector pgwt_read_counter_vector(
    const struct pgwt_counter_sources *s, pgwt_counter_lookup_fn lookup)
{
    struct pgwt_counter_vector v = {0};
    /* Explicit source table: a fork-state insert is NOT a STATE_MAP failure.
     * INIT hints and INIT_NO_STATE are diagnostics, not lost INITs. */
    const int fds[PGWT_COUNTER_COMPONENTS] = {
        s->site_fd, -1, s->site_fd, s->fail_fd, s->site_fd, s->site_fd,
        s->fail_fd, s->fail_fd, s->fail_fd, s->fail_fd, s->fail_fd,
        s->event_ring_fd, s->fail_fd, s->fail_fd, -1,
    };
    const uint32_t slots[PGWT_COUNTER_COMPONENTS] = {
        PGWT_RING_FAIL_LIFECYCLE_FORK, 0, PGWT_RING_FAIL_LIFECYCLE_EXIT,
        PGWT_BPF_FAIL_WAIT_READ, PGWT_RING_FAIL_QUERY_TEXT,
        PGWT_RING_FAIL_PG13_QUERY_TEXT, PGWT_BPF_FAIL_EXACT_QUERY_UPDATE,
        PGWT_BPF_FAIL_EXACT_CMD_UPDATE, PGWT_BPF_FAIL_EXACT_PHASE_UPDATE,
        PGWT_BPF_FAIL_ACCUM_UPDATE, PGWT_BPF_FAIL_FORK_STATE_UPDATE, 0,
        PGWT_BPF_FAIL_STATE_MAP, PGWT_BPF_FAIL_SEEN_QIDS, 0,
    };
    for (int i = 0; i < PGWT_COUNTER_COMPONENTS; ++i) {
        if (i == PGWT_C_LIFECYCLE_INIT_DROP)
            v.components[i] = (struct pgwt_counter_value){
                s->lifecycle_init_lost_total, s->init_state_read_fail_total == 0};
        else if (i == PGWT_C_WRITER_PUSH_FAIL)
            v.components[i] = (struct pgwt_counter_value){s->writer_push_fail_total, true};
        else
            v.components[i] = pgwt_read_percpu_counter(fds[i], slots[i], s->ncpu, lookup);
    }
    v.components[PGWT_C_STATE_MAP_UPDATE_FAIL] = pgwt_counter_add(
        v.components[PGWT_C_STATE_MAP_UPDATE_FAIL], s->state_map_full_total);
    v.valid = true;
    for (int i = 0; i < PGWT_COUNTER_COMPONENTS; ++i)
        v.valid = v.valid && v.components[i].valid;
    return v;
}
