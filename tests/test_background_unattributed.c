/* #596: query-less background/maintenance work must be visible as
 * unattributed time. IO_WORKER time stays outside the query model.
 * In-memory exact records; no CMD markers or pre-stamped category flags. */
#include "compute.h"
#include "summary_reader.h"
#include "wait_event.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MS 1000000ULL
#define LOCK_RELATION WEI(PG_WAIT_LOCK, 0)

static int failures;

#define CHECK(got, want) do {                                         \
    double actual = (double)(got), expected = (double)(want);           \
    if (actual != expected) {                                         \
        printf("FAIL: %s = %.1f, expected %.1f\n", #got, actual, expected); \
        failures++;                                                  \
    }                                                                \
} while (0)

/* The raw compute path never visits summaries. */
int pgwt_visit_summaries(const char *trace_dir, uint64_t from_wall_ns,
                         uint64_t to_wall_ns, pgwt_summary_visitor visitor,
                         void *ctx)
{
    (void)trace_dir; (void)from_wall_ns; (void)to_wall_ns;
    (void)visitor; (void)ctx;
    return -1;
}

static struct pgwt_trace_event wait_event(uint32_t pid, uint64_t ms,
                                           uint64_t query_id)
{
    return (struct pgwt_trace_event) {
        .timestamp_ns = 2000 * MS,
        .duration_ns = ms * MS,
        .pid = pid,
        .old_event = LOCK_RELATION,
        .query_id = query_id,
        .cpu_ns = PGWT_CPU_NS_UNKNOWN,
    };
}

static void control(uint32_t category, const char *name)
{
    struct pgwt_trace_event events[] = {
        wait_event(10, 500, 0),
        wait_event(20, 100, 42),
        wait_event(30, 700, 0),
    };
    const struct pgwt_pid_cat cats[] = {
        { .pid = 10, .flag = category },
        { .pid = 30, .flag = PGWT_EVENT_FLAG_IO_WORKER },
    };
    struct pgwt_filter filter = {0};
    struct pgwt_queries_result queries;
    struct pgwt_tm_result overview;
    char event_name[64];
    pgwt_event_full_name(LOCK_RELATION, event_name, sizeof(event_name));
    CHECK(strcmp(event_name, "Lock:relation"), 0);
    CHECK(pgwt_tag_events_window(events, 3, cats, 2, 2000 * MS), 1);
    CHECK(events[0].query_id, 0);
    CHECK(events[0].flags & PGWT_EVENT_FLAG_QUERY_BACKFILL, 0);
    CHECK(events[0].flags & category, category);
    CHECK(events[2].query_id, 0);
    CHECK(events[2].flags & PGWT_EVENT_FLAG_QUERY_UNATTRIB, 0);

    pgwt_compute_top_queries(events, 3, &filter, 1000 * MS, 2000 * MS,
                             1000.0, &queries);
    pgwt_compute_time_model(events, 3, &filter, 1000 * MS, 2000 * MS,
                            1000.0, &overview);
    printf("%s: unattributed_ms=%.1f unattributed_count=%llu "
           "num_rows=%d db_time_ms=%.1f overview_ms=%.1f\n",
           name, queries.unattributed_ms,
           (unsigned long long)queries.unattributed_count,
           queries.num_rows, queries.db_time_ms, overview.db_time_ms);
    CHECK(queries.unattributed_ms, 500.0);
    CHECK(queries.unattributed_count, 1);
    CHECK(queries.num_rows, 1);
    CHECK(queries.db_time_ms, 100.0);
    CHECK(overview.db_time_ms, 600.0);
    if (queries.num_rows == 1) {
        CHECK(queries.rows[0].query_id, 42);
        CHECK(queries.rows[0].total_ms, 100.0);
    }
    free(queries.rows);
    free(overview.rows);
}

int main(void)
{
    pgwt_init_event_names(18);
    control(PGWT_EVENT_FLAG_BACKGROUND, "BACKGROUND");
    control(PGWT_EVENT_FLAG_MAINT, "MAINT");
    printf("%s: %d failure(s)\n", failures ? "FAIL" : "PASS", failures);
    return failures ? 1 : 0;
}
