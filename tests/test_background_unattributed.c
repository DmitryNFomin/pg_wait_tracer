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

static struct pgwt_trace_event wait_event_at(uint32_t pid, uint32_t wei,
                                             uint64_t end_ms, uint64_t ms,
                                             uint64_t query_id)
{
    /* Records are END-stamped and every leg closes at or before 2000 ms with
     * its whole duration inside the [1000 ms, 2000 ms] window the asserts
     * use. No clock, no sleep, no file. The ONE ordering this fixture does
     * depend on is stated where it matters: the backward pass visits the
     * array back to front, so a record that must act as "the id reported
     * LATER" is placed after its victim AND end-stamped after it. */
    return (struct pgwt_trace_event) {
        .timestamp_ns = end_ms * MS,
        .duration_ns = ms * MS,
        .pid = pid,
        .old_event = wei,
        .query_id = query_id,
        .cpu_ns = PGWT_CPU_NS_UNKNOWN,
    };
}

static struct pgwt_trace_event wait_event_wei(uint32_t pid, uint32_t wei,
                                              uint64_t ms, uint64_t query_id)
{
    return wait_event_at(pid, wei, 2000, ms, query_id);
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

/* total_ms of the row for `qid`, or -1.0 when there is NO such row. Not 0.0:
 * zero is a legitimate total, so returning it would make a MISSING row read
 * as a present empty one -- the absence case passing as the wrong-value case. */
static double row_total_ms(const struct pgwt_queries_result *q, uint64_t qid)
{
    for (int i = 0; i < q->num_rows; i++)
        if (q->rows[i].query_id == qid)
            return q->rows[i].total_ms;
    return -1.0;
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
        /* NEVER BACKFILLED (src/compute.h's #596 bullet says so, and until
         * now nothing tested it: every background pid above owns exactly ONE
         * record, so there was no id available to borrow and the refusal was
         * never exercised). pid 50 is the same category and reports a query
         * id on its LATER record -- an autovacuum worker does exactly this,
         * it reports the VACUUM's id. The earlier record must stay
         * unattributed: borrowing 77 would bill maintenance time to a user
         * statement, the silent wrong answer #596 exists to prevent. */
        wait_event_at(50, LOCK_RELATION, 1500, 200, 0),
        wait_event_at(50, LOCK_RELATION, 2000, 50, 77),
    };
    const struct pgwt_pid_cat cats[] = {
        { .pid = 10, .flag = category },
        { .pid = 30, .flag = PGWT_EVENT_FLAG_IO_WORKER },
        { .pid = 40, .flag = category },
        { .pid = 50, .flag = category },
    };
    struct pgwt_filter filter = {0};
    struct pgwt_queries_result queries;
    struct pgwt_tm_result overview;
    char event_name[64];
    pgwt_event_full_name(LOCK_RELATION, event_name, sizeof(event_name));
    CHECK(strcmp(event_name, "Lock:relation"), 0);
    CHECK(pgwt_tag_events_window(events, 6, cats, 4, 2000 * MS), 1);
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
    /* the never-backfilled leg: an id WAS available on this pid, and the
     * earlier record must still refuse it */
    CHECK(events[4].query_id, 0);
    CHECK(events[4].flags & PGWT_EVENT_FLAG_QUERY_BACKFILL, 0);
    CHECK(events[4].flags & PGWT_EVENT_FLAG_QUERY_UNATTRIB,
          PGWT_EVENT_FLAG_QUERY_UNATTRIB);
    CHECK(events[5].query_id, 77);   /* the id it reported itself is kept */

    pgwt_compute_top_queries(events, 6, &filter, 1000 * MS, 2000 * MS,
                             1000.0, &queries);
    pgwt_compute_time_model(events, 6, &filter, 1000 * MS, 2000 * MS,
                            1000.0, &overview);
    printf("%s: unattributed_ms=%.1f unattributed_count=%llu "
           "num_rows=%d db_time_ms=%.1f overview_ms=%.1f\n",
           name, queries.unattributed_ms,
           (unsigned long long)queries.unattributed_count,
           queries.num_rows, queries.db_time_ms, overview.db_time_ms);
    CHECK(queries.unattributed_ms, 700.0);   /* 500 (pid 10) + 200 (pid 50) */
    CHECK(queries.unattributed_count, 2);
    CHECK(queries.num_rows, 2);              /* 42 and the pid's own 77 */
    CHECK(queries.db_time_ms, 150.0);        /* 100 + 50 */
    CHECK(overview.db_time_ms, 850.0);       /* 600 + 200 + 50 */
    /* CONSERVATION, from two independent producers: top_queries' attributed
     * + unattributed must equal the time model's DB Time. The two numbers
     * come from pgwt_compute_top_queries and pgwt_compute_time_model
     * separately, not from one sum compared with itself, so an attribution
     * leak shows up here even when every absolute above is re-baselined.
     * Exact for this fixture: 150 attributed + 700 unattributed = 850. */
    CHECK(queries.db_time_ms + queries.unattributed_ms, overview.db_time_ms);
    /* By id, not by index: the row order is top_queries' sort, which is not
     * what this test is about, and an index would re-break on any re-sort. */
    CHECK(row_total_ms(&queries, 42), 100.0);
    CHECK(row_total_ms(&queries, 77), 50.0);
    free(queries.rows);
    free(overview.rows);
}

/* An event-flag bit no category producer emits today: the NEXT category,
 * whatever it turns out to be. 0x1000 is the first free bit above
 * PGWT_EVENT_FLAG_RIGHT_OPEN (0x800) in src/pg_wait_tracer.h. */
#define FUTURE_CAT 0x1000U

/* FAIL-SAFE DIRECTION over categories #596 does not name.
 *
 * Master's arm was `if (st->cat_flag != 0) continue;` -- TOTAL. #596 replaced
 * it with tests for three specific bits, which would let any future category
 * fall through to the backfill branch and borrow another statement's id. That
 * is not a smaller bug than the one #596 fixed, it is a worse one: a drop is a
 * visible under-count, a borrowed id is a confident wrong answer.
 *
 * So this pins the DIRECTION, not a number: an unrecognised category with an
 * id available on the same pid must still end up with query_id 0 and no
 * BACKFILL, and the time it represents must show up as a conservation GAP
 * (under-counted) rather than as somebody else's query time. */
static void unknown_category_is_dropped_not_borrowed(void)
{
    /* If 0x1000 ever becomes a real category this leg silently starts
     * testing that category instead of an unknown one, so say so out loud. */
    CHECK(FUTURE_CAT & (PGWT_EVENT_FLAG_IO_WORKER | PGWT_EVENT_FLAG_MAINT |
                        PGWT_EVENT_FLAG_BACKGROUND), 0);
    struct pgwt_trace_event events[] = {
        wait_event_at(60, LOCK_RELATION, 1500, 200, 0),
        wait_event_at(60, LOCK_RELATION, 2000, 50, 77),
    };
    const struct pgwt_pid_cat cats[] = { { .pid = 60, .flag = FUTURE_CAT } };
    struct pgwt_filter filter = {0};
    struct pgwt_queries_result queries;
    struct pgwt_tm_result overview;

    CHECK(pgwt_tag_events_window(events, 2, cats, 1, 2000 * MS), 1);
    CHECK(events[0].query_id, 0);
    CHECK(events[0].flags & PGWT_EVENT_FLAG_QUERY_BACKFILL, 0);
    CHECK(events[0].flags & PGWT_EVENT_FLAG_QUERY_UNATTRIB, 0);

    pgwt_compute_top_queries(events, 2, &filter, 1000 * MS, 2000 * MS,
                             1000.0, &queries);
    pgwt_compute_time_model(events, 2, &filter, 1000 * MS, 2000 * MS,
                            1000.0, &overview);
    printf("FUTURE_CAT: unattributed_ms=%.1f num_rows=%d db_time_ms=%.1f "
           "overview_ms=%.1f\n", queries.unattributed_ms, queries.num_rows,
           queries.db_time_ms, overview.db_time_ms);
    CHECK(queries.unattributed_ms, 0.0);
    CHECK(queries.num_rows, 1);
    CHECK(row_total_ms(&queries, 77), 50.0);
    /* the 200 ms is MISSING from top_queries and PRESENT in DB Time: an
     * under-count of exactly the dropped record, which is the direction we
     * want. If the fall-through ever returns, this gap becomes 0 and the
     * 200 ms reappears inside query 77 -- caught by the row check above. */
    CHECK(overview.db_time_ms - (queries.db_time_ms + queries.unattributed_ms),
          200.0);
    free(queries.rows);
    free(overview.rows);
}

/* PRECEDENCE: IO_WORKER is checked BEFORE the background/maintenance arm, so
 * a pid carrying both bits stays outside the query model instead of becoming
 * unattributed load. Without this leg the IO_WORKER early return is an
 * EQUIVALENT MUTANT -- for a lone IO_WORKER bit the `cat_flag != 0` fail-safe
 * below it produces the same answer, so deleting the early return breaks
 * nothing a one-bit fixture can see. Measured: with this leg absent, removing
 * the IO_WORKER arm left the suite GREEN (exit 0). No producer emits the
 * combination today (pgwt_backend_type_flag returns one value per backend
 * type); the ORDER is the contract being pinned, for whoever adds one. */
static void io_worker_outranks_background(void)
{
    struct pgwt_trace_event events[] = {
        wait_event_at(70, LOCK_RELATION, 2000, 300, 0),
    };
    const struct pgwt_pid_cat cats[] = {
        { .pid = 70, .flag = PGWT_EVENT_FLAG_IO_WORKER |
                             PGWT_EVENT_FLAG_BACKGROUND },
    };
    struct pgwt_filter filter = {0};
    struct pgwt_queries_result queries;
    struct pgwt_tm_result overview;

    CHECK(pgwt_tag_events_window(events, 1, cats, 1, 2000 * MS), 1);
    CHECK(events[0].query_id, 0);
    CHECK(events[0].flags & PGWT_EVENT_FLAG_QUERY_UNATTRIB, 0);
    CHECK(events[0].flags & PGWT_EVENT_FLAG_QUERY_BACKFILL, 0);

    pgwt_compute_top_queries(events, 1, &filter, 1000 * MS, 2000 * MS,
                             1000.0, &queries);
    pgwt_compute_time_model(events, 1, &filter, 1000 * MS, 2000 * MS,
                            1000.0, &overview);
    printf("IO_WORKER|BACKGROUND: unattributed_ms=%.1f num_rows=%d "
           "overview_ms=%.1f\n", queries.unattributed_ms, queries.num_rows,
           overview.db_time_ms);
    CHECK(queries.unattributed_ms, 0.0);
    CHECK(queries.num_rows, 0);
    CHECK(overview.db_time_ms, 0.0);   /* and outside DB Time, not just queries */
    free(queries.rows);
    free(overview.rows);
}

int main(void)
{
    pgwt_init_event_names(18);
    control(PGWT_EVENT_FLAG_BACKGROUND, "BACKGROUND");
    control(PGWT_EVENT_FLAG_MAINT, "MAINT");
    unknown_category_is_dropped_not_borrowed();
    io_worker_outranks_background();
    printf("%s: %d failure(s)\n", failures ? "FAIL" : "PASS", failures);
    return failures ? 1 : 0;
}
