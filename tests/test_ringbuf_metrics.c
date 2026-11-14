/* Exercise the full provider's map reader without Linux/libbpf. Refs #871. */
#include "ringbuf_metrics.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

enum { AGGREGATE_FD = 17, SITE_FD = 23 };
static int aggregate_calls, site_calls[PGWT_RING_FAIL_SITE_MAX];
static int failed_fd = -1, failed_key = -1, possible_cpus = 3;

static int lookup(int fd, const void *key_ptr, void *values_ptr)
{
    uint32_t key = *(const uint32_t *)key_ptr;
    uint64_t *values = values_ptr;
    assert(fd == AGGREGATE_FD || fd == SITE_FD);
    if (fd == AGGREGATE_FD) {
        assert(key == 0);
        ++aggregate_calls;
    } else {
        assert(key < PGWT_RING_FAIL_SITE_MAX);
        ++site_calls[key];
    }
    if (fd == failed_fd && (int)key == failed_key)
        return -1;
    /* CPU 2 has most of the count: omitting a possible but idle CPU fails.
     * Counts above 2^53 also catch accidental double conversion. */
    const uint64_t aggregate[] = {1, 2, UINT64_C(9007199254740993)};
    for (int cpu = 0; cpu < possible_cpus; ++cpu)
        values[cpu] = fd == AGGREGATE_FD ? aggregate[cpu] : (key + 1) * aggregate[cpu];
    return 0;
}

int main(void)
{
    const char *const expected_names[] = {
        "ringbuf_wait_transition_drops_total", "ringbuf_exit_transition_drops_total",
        "ringbuf_marker_drops_total", "ringbuf_command_boundary_drops_total",
        "lifecycle_init_hint_drops_total", "ringbuf_lifecycle_fork_drops_total",
        "ringbuf_lifecycle_exit_drops_total", "ringbuf_query_text_drops_total",
        "ringbuf_pg13_query_text_drops_total",
    };
    _Static_assert(sizeof(expected_names) / sizeof(*expected_names) == PGWT_RING_FAIL_SITE_MAX,
                   "every site has a stable control metric name");
    struct pgwt_metrics m = {0};
    const uint64_t expected_total = UINT64_C(9007199254740996);
    pgwt_read_ringbuf_metrics(&m, AGGREGATE_FD, SITE_FD, 3, lookup);
    assert(aggregate_calls == 1);
    assert(m.ringbuf_drops_valid);
    assert(m.ringbuf_drops_total == expected_total);
    for (int site = 0; site < PGWT_RING_FAIL_SITE_MAX; ++site) {
        assert(site_calls[site] == 1);
        assert(m.ringbuf_site_valid[site]);
        assert(m.ringbuf_site_drops[site] == (site + 1) * expected_total);
        assert(strcmp(pgwt_ringbuf_site_metric_names[site], expected_names[site]) == 0);
    }
    /* Repeated reads are cumulative snapshots, not additions or map resets. */
    pgwt_read_ringbuf_metrics(&m, AGGREGATE_FD, SITE_FD, 3, lookup);
    assert(m.ringbuf_drops_total == expected_total);
    assert(m.ringbuf_site_drops[PGWT_RING_FAIL_PG13_QUERY_TEXT] == 9 * expected_total);
    /* A missing/failed aggregate must not suppress per-site observations. */
    m = (struct pgwt_metrics){0};
    pgwt_read_ringbuf_metrics(&m, -1, SITE_FD, 3, lookup);
    assert(m.ringbuf_drops_total == 0 && !m.ringbuf_drops_valid);
    assert(m.ringbuf_site_drops[0] == expected_total);
    failed_fd = AGGREGATE_FD; failed_key = 0;
    pgwt_read_ringbuf_metrics(&m, AGGREGATE_FD, SITE_FD, 3, lookup);
    assert(m.ringbuf_drops_total == 0 && !m.ringbuf_drops_valid);
    assert(m.ringbuf_site_drops[8] == 9 * expected_total);
    /* A failed read must also invalidate and erase a previous successful
     * value, so consumers cannot silently keep stale metrics. */
    failed_fd = -1;
    pgwt_read_ringbuf_metrics(&m, AGGREGATE_FD, SITE_FD, 3, lookup);
    assert(m.ringbuf_drops_valid && m.ringbuf_drops_total == expected_total);
    failed_fd = AGGREGATE_FD; failed_key = 0;
    pgwt_read_ringbuf_metrics(&m, AGGREGATE_FD, SITE_FD, 3, lookup);
    assert(!m.ringbuf_drops_valid && m.ringbuf_drops_total == 0);
    /* A failed site affects neither the aggregate nor the other slots. */
    m = (struct pgwt_metrics){0};
    failed_fd = SITE_FD; failed_key = PGWT_RING_FAIL_MARKER;
    pgwt_read_ringbuf_metrics(&m, AGGREGATE_FD, SITE_FD, 3, lookup);
    assert(m.ringbuf_drops_total == expected_total);
    assert(m.ringbuf_site_drops[PGWT_RING_FAIL_MARKER] == 0);
    assert(!m.ringbuf_site_valid[PGWT_RING_FAIL_MARKER]);
    assert(m.ringbuf_site_drops[PGWT_RING_FAIL_COMMAND_BOUNDARY] == 4 * expected_total);
    /* Missing site map keeps the legacy aggregate path intact. */
    m = (struct pgwt_metrics){0};
    pgwt_read_ringbuf_metrics(&m, AGGREGATE_FD, -1, 3, lookup);
    assert(m.ringbuf_drops_total == expected_total);
    assert(m.ringbuf_site_drops[0] == 0);
    failed_fd = -1; possible_cpus = 1;
    pgwt_read_ringbuf_metrics(&m, AGGREGATE_FD, SITE_FD, 0, lookup);
    assert(!m.ringbuf_drops_valid && !m.ringbuf_site_valid[8]);
    pgwt_read_ringbuf_metrics(&m, AGGREGATE_FD, SITE_FD, 1, lookup);
    assert(m.ringbuf_drops_valid && m.ringbuf_drops_total == 1 && m.ringbuf_site_drops[8] == 9);
    puts("ringbuf metrics: PASS (all sites/CPUs, stable names, cumulative reads, lookup failures)");
    return 0;
}
