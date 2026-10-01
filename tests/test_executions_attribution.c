/* test_executions_attribution.c — #222 review round 3, item 2.
 *
 * pgwt_compute_executions keeps one "is the top-of-stack row still the one
 * that JUST started" bit per pid (exec_pid_state.top_attributable). An
 * EXEC_START whose timestamp falls past the caller's `to_ns` pushes no row
 * — and, before this fix, left that bit at whatever the previous push set
 * it to, so a later wait event could still be credited to an EARLIER,
 * still-open row that the new statement has in fact superseded.
 *
 * Why this needs a C test and not a Python one against pgwt-server: the
 * hazard is unreachable through the server, because
 * server_load_events_fi() stops the stream at `ts > sample_to_m`, so no
 * event after the skipped EXEC_START is ever loaded. That is caller
 * discipline, not a property of the function. pgwt_compute_executions is a
 * public entry point taking an arbitrary event array, so the invariant is
 * pinned here, at the only layer where it can be observed at all.
 *
 * Pure: links map_reader.c (-DPGWT_SERVER) + snapshot.c + compute.c, same
 * as test_live_accum. No daemon, no PostgreSQL, no root. */
#include "map_reader.h"
#include "snapshot.h"
#include "wait_event.h"
#include "pg_wait_tracer.h"
#include "compute.h"
#include "summary_reader.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

static int tests_run = 0;
static int tests_passed = 0;

#define CHECK(cond, fmt, ...) do {                                   \
    tests_run++;                                                     \
    if (cond) { tests_passed++; printf("  PASS: " fmt "\n", ##__VA_ARGS__); } \
    else      { printf("  FAIL(%d): " fmt "\n", __LINE__, ##__VA_ARGS__); }  \
} while (0)

/* compute.c's only foreign symbol (summary streaming) — unused here. */
int pgwt_visit_summaries(const char *trace_dir, uint64_t from_wall_ns,
                         uint64_t to_wall_ns, pgwt_summary_visitor visitor,
                         void *ctx)
{
    (void)trace_dir; (void)from_wall_ns; (void)to_wall_ns;
    (void)visitor; (void)ctx;
    return -1;
}

#define MS 1000000ULL
#define BASE 1000000000000ULL
#define PID 4242
/* IO:DataFileRead -- (class << 24) | event_num, the same encoding
 * tests/server_harness.py builds its fixtures with. Any non-marker,
 * non-zero wait_event_info would do; this one is simply recognisable. */
#define WE_IO_DATA_FILE_READ ((0x0AU << 24) | 21U)

static struct pgwt_trace_event marker_ev(uint64_t ts, uint32_t kind,
                                         uint64_t qid)
{
    struct pgwt_trace_event e;
    memset(&e, 0, sizeof(e));
    e.timestamp_ns = ts;
    e.pid = PID;
    e.old_event = kind;
    e.new_event = kind;
    e.query_id = qid;
    e.cpu_ns = PGWT_CPU_NS_UNKNOWN;
    return e;
}

/* A wait transition: `old_event` lasted `dur` and ended at `ts`, i.e. its
 * interval is [ts - dur, ts]. interval_overlaps() in compute.c reads it
 * exactly that way, which is why an event whose ts sits past `to_ns` can
 * still overlap the window. */
static struct pgwt_trace_event wait_ev(uint64_t ts, uint64_t dur, uint64_t qid)
{
    struct pgwt_trace_event e;
    memset(&e, 0, sizeof(e));
    e.timestamp_ns = ts;
    e.pid = PID;
    e.old_event = WE_IO_DATA_FILE_READ;
    e.new_event = 0;                       /* back on CPU */
    e.duration_ns = dur;
    e.query_id = qid;
    e.cpu_ns = PGWT_CPU_NS_UNKNOWN;
    return e;
}

static int find_row(const struct pgwt_executions_result *res, uint64_t start_ns)
{
    for (int i = 0; i < res->num_rows; i++)
        if (res->rows[i].start_ns == start_ns)
            return i;
    return -1;
}

/* ── 1. the regression ────────────────────────────────────────────────────
 *
 * A: EXEC_START inside the window, never ended (orphan, stays on the stack).
 * B: EXEC_START past to_ns — no row is kept for it.
 * W: a wait interval that ENDS past to_ns but STARTS inside it, so
 *    interval_overlaps() accepts it.
 *
 * W belongs to B, and B has no row, so W must be credited to nothing. With
 * top_attributable left stale by the skipped EXEC_START it was credited to
 * A instead: n_events == 1 on a row whose statement had already been
 * superseded. */
static void test_skipped_start_does_not_leave_the_previous_row_attributable(void)
{
    const uint64_t from_ns = BASE;
    const uint64_t to_ns   = BASE + 100 * MS;
    struct pgwt_trace_event events[] = {
        marker_ev(BASE + 10 * MS, PGWT_MARKER_EXEC_START, 111),   /* A */
        marker_ev(BASE + 150 * MS, PGWT_MARKER_EXEC_START, 222),  /* B, past to_ns */
        wait_ev(BASE + 160 * MS, 100 * MS, 222),                  /* W, overlaps */
    };
    struct pgwt_executions_result res;
    pgwt_compute_executions(events, (int)(sizeof(events) / sizeof(events[0])),
                            from_ns, to_ns, NULL, NULL, 0, &res);

    CHECK(!res.failed, "compute did not fail (failed=%d)", res.failed);
    /* The gate must be able to SEE: if the fixture produced no row for A at
     * all, the n_events assertion below would pass vacuously. */
    CHECK(res.num_rows == 1,
          "exactly one row survives -- A (in-window start); B's start is "
          "past to_ns so it is not kept (num_rows=%d)", res.num_rows);
    int a = find_row(&res, BASE + 10 * MS);
    CHECK(a >= 0, "row A is present and identified by its start_ns (idx=%d)", a);
    if (a >= 0) {
        CHECK(res.rows[a].in_progress == 1,
              "row A is still open (it never got an EXEC_END)");
        CHECK(res.rows[a].n_events == 0,
              "the wait event after the SKIPPED EXEC_START is credited to "
              "NO row -- not back-dated onto the superseded row A "
              "(n_events=%d, expected 0)", res.rows[a].n_events);
        CHECK(res.rows[a].matches_event_filter == 0,
              "and row A is not marked as matching the event filter "
              "(matches_event_filter=%d)", res.rows[a].matches_event_filter);
    }
    free(res.rows);
}

/* ── 2. anti-bypass: the clear must not be a blanket "attribute nothing" ──
 *
 * The cheapest way to make test 1 pass is to break attribution outright.
 * The ordinary case — one EXEC_START inside the window, one wait event
 * inside the window — must still credit that row. */
static void test_ordinary_in_window_event_is_still_attributed(void)
{
    const uint64_t from_ns = BASE;
    const uint64_t to_ns   = BASE + 100 * MS;
    struct pgwt_trace_event events[] = {
        marker_ev(BASE + 10 * MS, PGWT_MARKER_EXEC_START, 111),
        wait_ev(BASE + 30 * MS, 5 * MS, 111),
        marker_ev(BASE + 40 * MS, PGWT_MARKER_EXEC_END, 111),
    };
    struct pgwt_executions_result res;
    pgwt_compute_executions(events, (int)(sizeof(events) / sizeof(events[0])),
                            from_ns, to_ns, NULL, NULL, 0, &res);
    CHECK(!res.failed, "compute did not fail (failed=%d)", res.failed);
    CHECK(res.num_rows == 1, "one completed row (num_rows=%d)", res.num_rows);
    if (res.num_rows == 1) {
        CHECK(res.rows[0].in_progress == 0, "the row closed at its EXEC_END");
        CHECK(res.rows[0].n_events == 1,
              "an ordinary in-window wait event IS still attributed -- the "
              "fix did not disable attribution wholesale (n_events=%d, "
              "expected 1)", res.rows[0].n_events);
    }
    free(res.rows);
}

/* ── 3. anti-bypass: a SECOND in-window start must still take over ────────
 *
 * The clear added for the skipped branch must not change the behaviour of
 * the branch that DOES push: after a second in-window EXEC_START, events
 * belong to the new (innermost) row, not to the older one. */
static void test_second_in_window_start_takes_attribution(void)
{
    const uint64_t from_ns = BASE;
    const uint64_t to_ns   = BASE + 100 * MS;
    struct pgwt_trace_event events[] = {
        marker_ev(BASE + 10 * MS, PGWT_MARKER_EXEC_START, 111),  /* A, orphan */
        marker_ev(BASE + 20 * MS, PGWT_MARKER_EXEC_START, 222),  /* B */
        wait_ev(BASE + 30 * MS, 5 * MS, 222),
    };
    struct pgwt_executions_result res;
    pgwt_compute_executions(events, (int)(sizeof(events) / sizeof(events[0])),
                            from_ns, to_ns, NULL, NULL, 0, &res);
    CHECK(!res.failed, "compute did not fail (failed=%d)", res.failed);
    CHECK(res.num_rows == 2, "both in-window starts keep a row (num_rows=%d)",
          res.num_rows);
    int a = find_row(&res, BASE + 10 * MS);
    int b = find_row(&res, BASE + 20 * MS);
    CHECK(a >= 0 && b >= 0, "rows A and B are both present (a=%d b=%d)", a, b);
    if (a >= 0 && b >= 0) {
        CHECK(res.rows[b].n_events == 1,
              "the event is credited to B, the statement that just started "
              "(B n_events=%d, expected 1)", res.rows[b].n_events);
        CHECK(res.rows[a].n_events == 0,
              "and NOT to the older orphaned row A (A n_events=%d, "
              "expected 0)", res.rows[a].n_events);
    }
    free(res.rows);
}

/* ── 4. the skipped start must still keep its other effects ───────────────
 *
 * Clearing top_attributable must not accidentally close, drop or otherwise
 * disturb the earlier row: A is still open, still reported, and still has
 * its own real start. (A row silently vanishing would make test 1 pass for
 * the wrong reason.) */
static void test_skipped_start_leaves_the_earlier_row_intact(void)
{
    const uint64_t from_ns = BASE;
    const uint64_t to_ns   = BASE + 100 * MS;
    struct pgwt_trace_event events[] = {
        marker_ev(BASE + 10 * MS, PGWT_MARKER_EXEC_START, 111),
        marker_ev(BASE + 150 * MS, PGWT_MARKER_EXEC_START, 222),
    };
    struct pgwt_executions_result res;
    pgwt_compute_executions(events, (int)(sizeof(events) / sizeof(events[0])),
                            from_ns, to_ns, NULL, NULL, 0, &res);
    CHECK(!res.failed, "compute did not fail (failed=%d)", res.failed);
    CHECK(res.num_rows == 1, "row A survives the skipped start (num_rows=%d)",
          res.num_rows);
    if (res.num_rows == 1) {
        CHECK(res.rows[0].start_ns == BASE + 10 * MS,
              "with its own real start_ns, not the skipped one");
        CHECK(res.rows[0].in_progress == 1,
              "and still open -- the skipped start did not close it");
        CHECK(res.rows[0].end_inferred == 0,
              "and not marked inferred-ended (end_inferred=%d)",
              res.rows[0].end_inferred);
    }
    free(res.rows);
}

int main(void)
{
    printf("=== test_executions_attribution ===\n");
    printf("--- skipped EXEC_START clears attributability (#222 item 2) ---\n");
    test_skipped_start_does_not_leave_the_previous_row_attributable();
    printf("--- ordinary in-window attribution still works ---\n");
    test_ordinary_in_window_event_is_still_attributed();
    printf("--- a second in-window start still takes over ---\n");
    test_second_in_window_start_takes_attribution();
    printf("--- the skipped start leaves the earlier row intact ---\n");
    test_skipped_start_leaves_the_earlier_row_intact();

    printf("\n%d/%d tests passed\n", tests_passed, tests_run);
    return tests_passed == tests_run ? 0 : 1;
}
