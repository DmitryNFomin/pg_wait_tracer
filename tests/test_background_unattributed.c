/* #596: query-less background/maintenance work must be visible as
 * unattributed time. IO_WORKER time stays outside the query model.
 * In-memory exact records; no CMD markers or pre-stamped category flags. */
#include "compute.h"
#include "idle_rule.h"
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

static struct pgwt_trace_event wait_event_wei(uint32_t pid, uint32_t wei,
                                              uint64_t ms, uint64_t query_id)
{
    /* Records are END-stamped: every leg below closes at 2000 ms and so lies
     * wholly inside the [1000 ms, 2000 ms] window the asserts use. No clock,
     * no sleep, no file -- nothing here depends on wall time or on ordering. */
    return (struct pgwt_trace_event) {
        .timestamp_ns = 2000 * MS,
        .duration_ns = ms * MS,
        .pid = pid,
        .old_event = wei,
        .query_id = query_id,
        .cpu_ns = PGWT_CPU_NS_UNKNOWN,
    };
}

static struct pgwt_trace_event wait_event(uint32_t pid, uint64_t ms,
                                           uint64_t query_id)
{
    return wait_event_wei(pid, LOCK_RELATION, ms, query_id);
}

/* The wait_event_info of a PACING Timeout event, by name.
 *
 * This leg exists to exercise compute.c's `!pgwt_is_idle_event(we)` guard,
 * and inside the #596 branch that guard can ONLY ever fire for a pacing
 * Timeout: Activity and Client:ClientRead are pgwt_is_session_idle_event and
 * have already `continue`d several lines earlier. So picking the wrong id
 * would not weaken the leg, it would DELETE it while leaving it green.
 * Hence: resolve by name, assert the id really is load-idle and really is
 * not session-idle, and FAIL if no such id exists rather than falling back
 * to id 0 (which is CheckpointWriteDelay on no PostgreSQL in particular).
 * The mask is empty until pgwt_init_event_names() runs -- idle_rule.c's
 * fail-safe default -- which is itself a way this leg could go vacuous. */
static uint32_t pacing_timeout_wei(const char *name)
{
    for (uint32_t id = 0; id < 32; id++) {
        const char *n = pgwt_event_name(WEI(PG_WAIT_TIMEOUT, id));
        if (!n || strcmp(n, name) != 0)
            continue;
        uint32_t wei = WEI(PG_WAIT_TIMEOUT, id);
        CHECK(pgwt_is_idle_event(wei), 1);          /* load-idle: pacing */
        CHECK(pgwt_is_session_idle_event(wei), 0);  /* NOT a cmd boundary */
        return wei;
    }
    printf("FAIL: no Timeout id named %s -- the pacing leg cannot see its "
           "own input, so it is refusing instead of passing\n", name);
    failures++;
    return 0;
}

static void control(uint32_t category, const char *name)
{
    uint32_t pacing = pacing_timeout_wei("CheckpointWriteDelay");
    struct pgwt_trace_event events[] = {
        wait_event(10, 500, 0),
        wait_event(20, 100, 42),
        wait_event(30, 700, 0),
        /* Same category as pid 10, but PACING: a checkpointer spreading its
         * writes is not load. MEASURED with compute.c's !pgwt_is_idle_event
         * guard deleted: this record gets FLAG_QUERY_UNATTRIB (1024) while
         * unattributed_ms stays 500 -- pgwt_compute_top_queries re-applies
         * the idle rule downstream, so the money column does NOT move and
         * only the flag assertion below sees it. That is exactly why the
         * flag is asserted on the record and not inferred from the total:
         * every other consumer of these flags (the live tier, the UI's
         * unattributed rows) reads the record, not top_queries' sum. */
        wait_event_wei(40, pacing, 900, 0),
    };
    const struct pgwt_pid_cat cats[] = {
        { .pid = 10, .flag = category },
        { .pid = 30, .flag = PGWT_EVENT_FLAG_IO_WORKER },
        { .pid = 40, .flag = category },
    };
    struct pgwt_filter filter = {0};
    struct pgwt_queries_result queries;
    struct pgwt_tm_result overview;
    char event_name[64];
    pgwt_event_full_name(LOCK_RELATION, event_name, sizeof(event_name));
    CHECK(strcmp(event_name, "Lock:relation"), 0);
    CHECK(pgwt_tag_events_window(events, 4, cats, 3, 2000 * MS), 1);
    CHECK(events[0].query_id, 0);
    CHECK(events[0].flags & PGWT_EVENT_FLAG_QUERY_BACKFILL, 0);
    CHECK(events[0].flags & category, category);
    CHECK(events[0].flags & PGWT_EVENT_FLAG_QUERY_UNATTRIB,
          PGWT_EVENT_FLAG_QUERY_UNATTRIB);
    CHECK(events[2].query_id, 0);
    CHECK(events[2].flags & PGWT_EVENT_FLAG_QUERY_UNATTRIB, 0);
    /* the pacing leg: tagged, but neither attributed nor unattributed */
    CHECK(events[3].query_id, 0);
    CHECK(events[3].flags & PGWT_EVENT_FLAG_QUERY_UNATTRIB, 0);
    CHECK(events[3].flags & PGWT_EVENT_FLAG_QUERY_BACKFILL, 0);

    pgwt_compute_top_queries(events, 4, &filter, 1000 * MS, 2000 * MS,
                             1000.0, &queries);
    pgwt_compute_time_model(events, 4, &filter, 1000 * MS, 2000 * MS,
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
    /* CONSERVATION, from two independent producers: top_queries' attributed
     * + unattributed must equal the time model's DB Time. The two numbers
     * come from pgwt_compute_top_queries and pgwt_compute_time_model
     * separately, not from one sum compared with itself, so an attribution
     * leak shows up here even when every absolute above is re-baselined.
     * Exact for this fixture: 100 (query 42) + 500 (background) = 600. */
    CHECK(queries.db_time_ms + queries.unattributed_ms, overview.db_time_ms);
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
