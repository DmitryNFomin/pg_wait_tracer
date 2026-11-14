/* Literal source/order expectations from merge-v1 §4 and dispatch #871. */
#include "counter_vector.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

enum { FAIL_FD = 11, SITE_FD = 22, EVENT_FD = 33, NCPU = 3 };
static int bad_fd = -1, bad_key = -1;
static bool overflow;
static int calls[3][PGWT_RING_FAIL_SITE_MAX];
static int lookup(int fd, const void *key_ptr, void *values_ptr)
{
    uint32_t key = *(const uint32_t *)key_ptr;
    uint64_t *values = values_ptr;
    assert(fd == FAIL_FD || fd == SITE_FD || fd == EVENT_FD);
    assert(key < PGWT_RING_FAIL_SITE_MAX);
    int map = fd == FAIL_FD ? 0 : fd == SITE_FD ? 1 : 2;
    calls[map][key]++;
    if (fd == bad_fd && (int)key == bad_key)
        return -1;
    values[0] = (uint64_t)fd * 100 + key;
    values[1] = 10;
    values[2] = overflow ? UINT64_MAX : UINT64_C(9007199254740993);
    return 0;
}

int main(void)
{
    const char *const names[] = {
        "lifecycle_fork_drop", "lifecycle_init_drop", "lifecycle_exit_drop",
        "wait_read_fail", "query_text_drop", "pg13_query_text_drop",
        "exact_query_update_fail", "exact_cmd_update_fail", "exact_phase_update_fail",
        "accum_update_fail", "fork_state_update_fail", "event_ring_drop",
        "state_map_update_fail", "seen_query_ids_update_fail", "writer_push_fail",
    };
    /* Literal slot numbers are intentional: a shared mistaken enum/source
     * table must not make the test green. Diagnostic INIT slot 2 is unused. */
    const int fds[]  = {22, -1, 22, 11, 22, 22, 11, 11, 11, 11, 11, 33, 11, 11, -1};
    const int keys[] = {5,   0,  6,  3,  7,  8,  4,  5,  6,  7,  8,  0,  0,  1,  0};
    _Static_assert(PGWT_COUNTER_COMPONENTS == 15, "contract §4 length");
    struct pgwt_counter_sources s = {
        .fail_fd = FAIL_FD, .site_fd = SITE_FD, .event_ring_fd = EVENT_FD, .ncpu = NCPU,
        .lifecycle_init_lost_total = 37, .state_map_full_total = 41,
        .writer_push_fail_total = 43,
    };
    struct pgwt_counter_vector v = pgwt_read_counter_vector(&s, lookup);
    assert(v.valid);
    for (int i = 0; i < 15; ++i) {
        assert(strcmp(names[i], pgwt_counter_component_names[i]) == 0);
        assert(v.components[i].valid);
        uint64_t expected = fds[i] < 0 ? (i == 1 ? 37 : 43)
            : UINT64_C(9007199254740993) + 10 + fds[i] * 100 + keys[i];
        if (i == 12) expected += 41;
        assert(v.components[i].value == expected);
        if (fds[i] >= 0)
            assert(calls[fds[i] == FAIL_FD ? 0 : fds[i] == SITE_FD ? 1 : 2][keys[i]] == 1);
    }
    /* Every failed map lookup affects its component alone; valid zero is
     * distinct from unreadable. A second read may recover from a transient. */
    for (int i = 0; i < 15; ++i) {
        if (fds[i] < 0) continue;
        bad_fd = fds[i]; bad_key = keys[i];
        v = pgwt_read_counter_vector(&s, lookup);
        assert(!v.valid);
        for (int j = 0; j < 15; ++j)
            assert(v.components[j].valid == (j != i));
        assert(v.components[i].value == 0);
    }
    bad_fd = -1; bad_key = -1;
    s.init_state_read_fail_total = 1;
    v = pgwt_read_counter_vector(&s, lookup);
    assert(!v.valid && !v.components[1].valid);
    /* INIT total includes the owner-excluded no-opportunity losses. The
     * certification vector has no with-opportunity-only input. */
    assert(v.components[1].value == 37);
    s.init_state_read_fail_total = 0;
    assert(pgwt_read_counter_vector(&s, lookup).valid);
    s.ncpu = 0;
    assert(!pgwt_read_counter_vector(&s, lookup).valid);
    s.ncpu = NCPU;
    assert(!pgwt_read_counter_vector(&s, NULL).valid);
    s.fail_fd = -1;
    assert(!pgwt_read_counter_vector(&s, lookup).components[3].valid);
    s.fail_fd = FAIL_FD;
    overflow = true;
    assert(!pgwt_read_counter_vector(&s, lookup).valid);
    overflow = false;
    s.state_map_full_total = UINT64_MAX;
    v = pgwt_read_counter_vector(&s, lookup);
    assert(!v.valid && !v.components[12].valid);
    puts("counter vector: PASS (15 sources/order, every lookup failure, INIT unknown, overflow)");
    return 0;
}
