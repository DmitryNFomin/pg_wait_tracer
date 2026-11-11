/* #852: sessions are created by non-idle observations, including closes
 * with zero remaining occupancy. Idle time and occurrence data still reach
 * events[]. Exercise both insertion paths and the real on-disk round trip. */
#include "summary_writer.h"
#include "summary_reader.h"
#include "wait_event.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/stat.h>

#define SEC 1000000000ULL
#define IDLE WEI(PG_WAIT_CLIENT, 0)
#define WAIT WEI(PG_WAIT_LOCK, 0)

static int failures;
#define CHECK(cond) do { if (!(cond)) { \
    printf("FAIL line %d: %s\n", __LINE__, #cond); failures++; \
} } while (0)

static const struct pgwt_summary_session *session(
    const struct pgwt_summary_accum *acc, uint32_t pid)
{
    for (int i = 0; i < SUMMARY_MAX_SESSIONS; i++)
        if (acc->sessions[i].pid == pid)
            return &acc->sessions[i];
    return NULL;
}

struct probe { int records, sessions, idle_rows; uint64_t idle_ns; };
static int visit(const struct pgwt_summary_accum *acc, void *arg)
{
    struct probe *p = arg;
    p->records++;
    p->sessions += acc->num_sessions;
    p->idle_rows += session(acc, 9000) != NULL;
    p->idle_ns += acc->idle_ns;
    return 0;
}

static void run_case(const char *base, const char *name, int open)
{
    char dir[512], path[600];
    snprintf(dir, sizeof(dir), "%s/%s", base, name);
    CHECK(mkdir(dir, 0700) == 0);
    struct pgwt_summary_writer *w = calloc(1, sizeof(*w));
    CHECK(w != NULL);
    if (!w) return;
    int rc = pgwt_summary_writer_init(w, dir, 24, NULL);
    CHECK(rc == 0);
    if (rc != 0) { free(w); return; }
    pgwt_summary_pin_clock(w, 10 * SEC, 10 * SEC);
    CHECK(w->open != NULL);

    if (open) {
        /* The file opens lazily on the first pushed record. */
        struct pgwt_trace_event idle = {
            .pid = 9000, .old_event = IDLE, .new_event = IDLE,
            .timestamp_ns = 10 * SEC + SEC / 10, .duration_ns = SEC / 10,
            .cpu_ns = PGWT_CPU_NS_UNKNOWN,
        };
        CHECK(pgwt_summary_push_event(w, &idle, 0, IDLE) == 0);
        /* Negative control for the idle-only case: open non-idle time DOES
         * allocate a session. No close record has arrived yet. */
        pgwt_summary_note_open(w, 9000, IDLE, 10 * SEC + SEC / 10, 0,
                               10 * SEC + SEC / 10);
        pgwt_summary_note_open(w, 1000, WAIT, 10 * SEC, 0,
                               10 * SEC + SEC / 10);
        CHECK(pgwt_summary_flush_completed(w, 11 * SEC +
                                           PGWT_SUMMARY_FLUSH_LAG_NS) == 0);
        CHECK(w->num_blocks == 1);
        if (open == 2) {
            /* All occupancy is already on disk in second 10. The close in
             * second 11 must retain its session and top-wait occurrence. */
            struct pgwt_trace_event ev = {
                .pid = 1000, .old_event = WAIT, .new_event = 0,
                .timestamp_ns = 11 * SEC, .duration_ns = SEC,
                .cpu_ns = PGWT_CPU_NS_UNKNOWN,
            };
            CHECK(pgwt_summary_push_event(w, &ev, 0, WAIT) == 0);
            const struct pgwt_summary_session *ss = session(&w->accum, 1000);
            CHECK(ss != NULL);
            if (ss) {
                CHECK(ss->db_time_ns == 0);
                CHECK(ss->top_wait_ns == SEC && ss->top_wait_id == WAIT);
            }
        }
    } else {
        struct pgwt_trace_event ev = {
            .pid = 9000, .old_event = IDLE, .new_event = WAIT,
            .timestamp_ns = 10 * SEC + SEC / 2, .duration_ns = SEC / 10,
            .cpu_ns = PGWT_CPU_NS_UNKNOWN,
        };
        CHECK(pgwt_summary_push_event(w, &ev, 0, IDLE) == 0);
        CHECK(w->accum.num_sessions == 0);
        CHECK(session(&w->accum, 9000) == NULL);
        CHECK(w->accum.idle_ns == SEC / 10 && w->accum.num_events == 1);
        CHECK(w->accum.sessions_overflow == 0);
        /* Negative control: a non-idle close must still allocate, even
         * when a zero duration makes ALL session payload fields zero. */
        ev.pid = 1000; ev.old_event = 0; ev.duration_ns = 0;
        CHECK(pgwt_summary_push_event(w, &ev, 0, 0) == 0);
        CHECK(w->accum.num_sessions == 1);
        CHECK(session(&w->accum, 1000) != NULL);
    }
    CHECK(pgwt_summary_close(w) == 0);
    pgwt_summary_destroy(w);
    free(w);

    struct probe p = {0};
    CHECK(pgwt_visit_summaries(dir, 0, 0, visit, &p) > 0);
    CHECK(p.records == (open == 2 ? 2 : 1));
    CHECK(p.idle_rows == 0);
    CHECK(p.sessions == (open == 2 ? 2 : 1));
    CHECK(p.idle_ns == (open ? SEC : SEC / 10));
    const char *files[] = {"current.summary", "current.summary.meta"};
    for (int i = 0; i < 2; i++) {
        snprintf(path, sizeof(path), "%s/%s", dir, files[i]);
        CHECK(unlink(path) == 0);
    }
    CHECK(rmdir(dir) == 0);
}

int main(void)
{
    char base[] = "/tmp/pgwt_session_rows_XXXXXX";
    CHECK(mkdtemp(base) != NULL);
    pgwt_init_event_names(18);
    run_case(base, "closed", 0);
    run_case(base, "open", 1);
    run_case(base, "credited_close", 2);
    CHECK(rmdir(base) == 0);
    printf("test_session_rows: %s (%d failures)\n",
           failures ? "FAIL" : "PASS", failures);
    return failures ? 1 : 0;
}
