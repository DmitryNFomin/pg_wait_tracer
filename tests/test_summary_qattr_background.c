/* #667: query-less background/maintenance load must be unattributed in the
 * summary writer, with pacing excluded and no category eligible to borrow
 * an id. The #596 sibling exercises compute.c; this uses the real writer's
 * public API and inspects its in-memory accumulator before closing it. */
#include "summary_writer.h"
#include "idle_rule.h"
#include "wait_event.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#define MS 1000000ULL
#define LOCK_RELATION WEI(PG_WAIT_LOCK, 0)
/* First free bit above PGWT_EVENT_FLAG_RIGHT_OPEN (0x800). */
#define FUTURE_CAT 0x1000U

static int failures;
static char g_base[300];

#define CHECK(got, want) do {                                         \
    double actual = (double)(got), expected = (double)(want);           \
    if (actual != expected) {                                         \
        printf("FAIL: %s = %.1f, expected %.1f\n", #got, actual, expected); \
        failures++;                                                  \
    }                                                                \
} while (0)

static const char *fresh_dir(const char *name)
{
    static char buf[600];
    snprintf(buf, sizeof(buf), "%s/%s", g_base, name);
    char cmd[700];
    snprintf(cmd, sizeof(cmd), "rm -rf '%s'", buf);
    int rc = system(cmd);
    CHECK(rc, 0);
    if (rc != 0)
        return NULL;
    rc = mkdir(buf, 0755);
    CHECK(rc, 0);
    return rc == 0 ? buf : NULL;
}

static struct pgwt_summary_writer *new_writer(const char *name)
{
    const char *dir = fresh_dir(name);
    if (!dir)
        return NULL;
    struct pgwt_summary_writer *w = calloc(1, sizeof(*w));
    CHECK(w != NULL, 1);
    if (!w)
        return NULL;
    int rc = pgwt_summary_writer_init(w, dir, 24, NULL);
    CHECK(rc, 0);
    if (rc != 0) {
        free(w);
        return NULL;
    }
    /* Allocation failure otherwise silently disables the path under test. */
    CHECK(w->qattr != NULL, 1);
    if (!w->qattr) {
        pgwt_summary_destroy(w);
        free(w);
        return NULL;
    }
    return w;
}

static void close_writer(struct pgwt_summary_writer *w)
{
    /* All assertions run BEFORE close flushes/resets the accumulator. */
    CHECK(pgwt_summary_close(w), 0);
    pgwt_summary_destroy(w);
    free(w);
}

static struct pgwt_trace_event wait_event_at(uint32_t pid, uint32_t wei,
                                             uint64_t end_ms, uint64_t ms,
                                             uint64_t query_id)
{
    /* Every record stays in the same second, so no assertion depends on a
     * disk flush. Category bits deliberately live ONLY in the push argument,
     * as in production; evt->flags stays zero (the N7 dead-guard trap). */
    return (struct pgwt_trace_event) {
        .timestamp_ns = end_ms * MS,
        .duration_ns = ms * MS,
        .pid = pid,
        .old_event = wei,
        .new_event = LOCK_RELATION,
        .query_id = query_id,
        .cpu_ns = PGWT_CPU_NS_UNKNOWN,
    };
}

static const struct pgwt_summary_query *query_row(
    const struct pgwt_summary_writer *w, uint64_t query_id)
{
    for (int i = 0; i < SUMMARY_MAX_QUERIES; i++) {
        const struct pgwt_summary_query *q = &w->accum.queries[i];
        if (q->query_id != 0 && q->query_id == query_id)
            return q;
    }
    return NULL;
}

/* Resolve by NAME: Timeout ids vary by PostgreSQL version. Both predicate
 * checks matter: a session-idle event would not exercise the pacing guard. */
static uint32_t pacing_timeout_wei(const char *name)
{
    for (uint32_t id = 0; id < 32; id++) {
        const char *n = pgwt_event_name(WEI(PG_WAIT_TIMEOUT, id));
        if (!n || strcmp(n, name) != 0)
            continue;
        uint32_t wei = WEI(PG_WAIT_TIMEOUT, id);
        CHECK(pgwt_is_idle_event(wei), 1);
        CHECK(pgwt_is_session_idle_event(wei), 0);
        return wei;
    }
    printf("FAIL: no Timeout id named %s -- pacing leg cannot run\n", name);
    failures++;
    return 0;
}

static void io_worker_refused_first(void)
{
    struct pgwt_summary_writer *w = new_writer("io_worker");
    if (!w)
        return;
    /* The push-site !io_worker routing supplies the IO_WORKER-first rule;
     * summary_qattr_step needs no internal (unreachable) IO_WORKER guard. */
    struct pgwt_trace_event evt = wait_event_at(10, LOCK_RELATION, 1500, 100, 0);
    evt.new_event = PGWT_EVENT_EXIT;
    CHECK(pgwt_summary_push_event(w, &evt, PGWT_EVENT_FLAG_IO_WORKER,
                                  evt.old_event), 0);
    CHECK(w->qattr_unattributed_ns_total, 0);
    CHECK(w->accum.num_queries, 0);

    /* IO_WORKER must win even when BACKGROUND is also set. */
    evt = wait_event_at(20, LOCK_RELATION, 1750, 200, 0);
    evt.new_event = PGWT_EVENT_EXIT;
    CHECK(pgwt_summary_push_event(w, &evt, PGWT_EVENT_FLAG_IO_WORKER |
                                  PGWT_EVENT_FLAG_BACKGROUND, evt.old_event), 0);
    CHECK(w->qattr_unattributed_ns_total, 0);
    CHECK(w->accum.num_queries, 0);
    close_writer(w);
}

static void background_maint_busy_is_unattributed(void)
{
    struct pgwt_summary_writer *w = new_writer("busy");
    if (!w)
        return;
    const uint32_t categories[] = {
        PGWT_EVENT_FLAG_BACKGROUND, PGWT_EVENT_FLAG_MAINT,
    };
    const uint64_t qids[] = {42, 77};
    uint64_t expected_ns = 0;
    char name[64];
    pgwt_event_full_name(LOCK_RELATION, name, sizeof(name));
    CHECK(strcmp(name, "Lock:relation"), 0);
    CHECK(pgwt_is_idle_event(LOCK_RELATION), 0);
    for (int i = 0; i < 2; i++) {
        uint32_t pid = 30 + i;
        struct pgwt_trace_event evt = wait_event_at(pid, LOCK_RELATION,
                                                    1500, 200, 0);
        CHECK(pgwt_summary_push_event(w, &evt, categories[i], evt.old_event), 0);
        expected_ns += evt.duration_ns;
        CHECK(w->qattr_unattributed_ns_total, expected_ns);
        CHECK(query_row(w, qids[i]) == NULL, 1);
        CHECK(w->accum.num_queries, i);

        /* An id IS available later on this same pid. The earlier work must
         * remain unattributed rather than backfilled into this statement. */
        evt = wait_event_at(pid, LOCK_RELATION, 1750, 50, qids[i]);
        CHECK(pgwt_summary_push_event(w, &evt, categories[i], evt.old_event), 0);
        const struct pgwt_summary_query *q = query_row(w, qids[i]);
        CHECK(q != NULL, 1);
        if (q) {
            CHECK(q->total_ns, 50 * MS);
            CHECK(q->count, 1);
        }
        CHECK(w->qattr_unattributed_ns_total, expected_ns);
    }
    CHECK(w->accum.num_queries, 2);
    close_writer(w);
}

static void pacing_excluded_by_load_rule(void)
{
    uint32_t pacing = pacing_timeout_wei("CheckpointWriteDelay");
    if (!pacing)
        return;
    struct pgwt_summary_writer *w = new_writer("pacing");
    if (!w)
        return;
    struct pgwt_trace_event evt = wait_event_at(40, LOCK_RELATION, 1200, 50, 42);
    CHECK(pgwt_summary_push_event(w, &evt, 0, evt.old_event), 0);
    uint64_t before = w->qattr_unattributed_ns_total;
    evt = wait_event_at(50, pacing, 1950, 900, 0);
    /* EXIT forces the old defer path to resolve NOW. Without it, checking
     * before a future boundary could pass while pacing time stayed pending. */
    evt.new_event = PGWT_EVENT_EXIT;
    CHECK(pgwt_summary_push_event(w, &evt, PGWT_EVENT_FLAG_BACKGROUND,
                                  evt.old_event), 0);
    CHECK(w->qattr_unattributed_ns_total, before);
    CHECK(w->accum.num_queries, 1);
    const struct pgwt_summary_query *q = query_row(w, 42);
    CHECK(q != NULL, 1);
    if (q) {
        CHECK(q->total_ns, 50 * MS);
        CHECK(q->count, 1);
    }
    close_writer(w);
}

static void unknown_category_is_dropped_not_borrowed(void)
{
    CHECK(FUTURE_CAT & (PGWT_EVENT_FLAG_IO_WORKER | PGWT_EVENT_FLAG_MAINT |
                        PGWT_EVENT_FLAG_BACKGROUND), 0);
    struct pgwt_summary_writer *w = new_writer("unknown");
    if (!w)
        return;
    struct pgwt_trace_event evt = wait_event_at(60, LOCK_RELATION, 1500, 200, 0);
    CHECK(pgwt_summary_push_event(w, &evt, FUTURE_CAT, evt.old_event), 0);
    CHECK(w->qattr_unattributed_ns_total, 0);
    CHECK(w->accum.num_queries, 0);
    /* Same pid, later id: the first record must not borrow this id, nor
     * count as unattributed. The explicit id still files its OWN record. */
    evt = wait_event_at(60, LOCK_RELATION, 1750, 50, 77);
    CHECK(pgwt_summary_push_event(w, &evt, FUTURE_CAT, evt.old_event), 0);
    CHECK(w->qattr_unattributed_ns_total, 0);
    CHECK(w->accum.num_queries, 1);
    const struct pgwt_summary_query *q = query_row(w, 77);
    CHECK(q != NULL, 1);
    if (q) {
        CHECK(q->total_ns, 50 * MS);
        CHECK(q->count, 1);
    }
    close_writer(w);
}

int main(void)
{
    pgwt_init_event_names(18);
    snprintf(g_base, sizeof(g_base), "/tmp/pgwt_sqbg_%d", (int)getpid());
    int rc = mkdir(g_base, 0755);
    CHECK(rc, 0);
    if (rc == 0) {
        io_worker_refused_first();
        background_maint_busy_is_unattributed();
        pacing_excluded_by_load_rule();
        unknown_category_is_dropped_not_borrowed();
        char cmd[400];
        snprintf(cmd, sizeof(cmd), "rm -rf '%s'", g_base);
        CHECK(system(cmd), 0);
    }
    printf("%s: %d failure(s)\n", failures ? "FAIL" : "PASS", failures);
    return failures ? 1 : 0;
}
