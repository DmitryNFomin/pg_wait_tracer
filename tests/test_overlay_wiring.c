/* test_overlay_wiring.c — #424: the WINDOW is what the time reducers take,
 * and an invalid or degraded one must produce an ABSENT answer, never a
 * zero that reads as a measurement.
 *
 * WHAT THIS GUARDS, and why each case is here rather than implied by the
 * others. src/time_window.h makes omission a compile error — a handler
 * cannot pass a bare record array any more. The compile half is checked by
 * tests/test_window_signature.sh. What the compiler CANNOT catch is the
 * zero-initialised window: `(struct pgwt_window){0}` has the right type and
 * no magic, so it type-checks and would silently reduce over nothing. That
 * is the bypass this file exists for, and it is checked against every one of
 * the six reducers, not against one of them.
 *
 * The second bypass is subtler and is the one that would make the whole
 * wiring unfalsifiable: a window bound to an overlay that is UNAVAILABLE or
 * REFUSED but still carries a spans array. If span visibility were gated on
 * the pointer rather than on the status, a degraded overlay would contribute
 * in-flight time that the daemon never vouched for. Section 3 asserts the
 * spans are invisible in that state AND visible when the status is INCLUDED
 * — a one-sided assertion would pass with the accessor stubbed to "always
 * 0", which is exactly the gate-that-cannot-see failure.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "compute.h"
#include "summary_reader.h"
#include "time_window.h"

/* compute.c's only foreign symbol (summary streaming) — unused here. */
int pgwt_visit_summaries(const char *trace_dir, uint64_t from_wall_ns,
                         uint64_t to_wall_ns, pgwt_summary_visitor visitor,
                         void *ctx)
{
    (void)trace_dir; (void)from_wall_ns; (void)to_wall_ns;
    (void)visitor; (void)ctx;
    return -1;
}

static int checks = 0, failures = 0;

static void ck(int cond, const char *what)
{
    checks++;
    if (!cond) {
        failures++;
        printf("  FAIL: %s\n", what);
    } else {
        printf("  ok: %s\n", what);
    }
}

/* One record so that "the reducer returned nothing" cannot be confused with
 * "there was nothing to return". Every absence assertion below is made over
 * an input that WOULD produce rows through a valid window — section 4 proves
 * that by running the same record through one. */
static struct pgwt_trace_event one_record(void)
{
    struct pgwt_trace_event ev;
    memset(&ev, 0, sizeof(ev));
    ev.pid = 4242;
    ev.timestamp_ns = 2000ULL * 1000000ULL;   /* ends at 2 s */
    ev.duration_ns  = 1000ULL * 1000000ULL;   /* 1 s long */
    ev.old_event = 0x01000001;                /* a real wait event id */
    ev.new_event = 0;
    ev.cpu_ns = PGWT_CPU_NS_UNKNOWN;
    return ev;
}

/* Run all six reducers over `w` and assert each reports `failed` and no
 * rows. `label` names the invalid-window shape under test. */
static void expect_all_six_absent(const struct pgwt_window *w,
                                  const char *label)
{
    struct pgwt_filter f;
    memset(&f, 0, sizeof(f));
    const uint64_t from = 0, to = 3000ULL * 1000000ULL;
    char buf[160];

    struct pgwt_aas_result aas;
    pgwt_compute_aas(w, &f, from, to, 3, 0, AAS_MAX_EVENT_SERIES, &aas);
    snprintf(buf, sizeof(buf), "%s: aas reports failed", label);
    ck(aas.failed == 1, buf);
    snprintf(buf, sizeof(buf), "%s: aas has no buckets", label);
    ck(aas.num_buckets == 0 && aas.buckets == NULL, buf);
    free(aas.buckets);
    free(aas.event_aas);

    struct pgwt_tm_result tm;
    pgwt_compute_time_model(w, &f, from, to, 3000.0, &tm);
    snprintf(buf, sizeof(buf), "%s: time_model reports failed", label);
    ck(tm.failed == 1, buf);
    snprintf(buf, sizeof(buf), "%s: time_model has no rows and no db_time",
             label);
    ck(tm.num_rows == 0 && tm.db_time_ms == 0.0, buf);
    free(tm.rows);

    struct pgwt_events_result evr;
    pgwt_compute_top_events(w, &f, from, to, 3000.0, &evr);
    snprintf(buf, sizeof(buf), "%s: top_events reports failed", label);
    ck(evr.failed == 1, buf);
    snprintf(buf, sizeof(buf), "%s: top_events has no rows", label);
    ck(evr.num_rows == 0 && evr.rows == NULL, buf);
    free(evr.rows);

    struct pgwt_sessions_result sr;
    pgwt_compute_top_sessions(w, &f, from, to, 3000.0, &sr);
    snprintf(buf, sizeof(buf), "%s: top_sessions reports failed", label);
    ck(sr.failed == 1, buf);
    snprintf(buf, sizeof(buf), "%s: top_sessions has no rows", label);
    ck(sr.num_rows == 0 && sr.rows == NULL, buf);
    free(sr.rows);

    struct pgwt_queries_result qr;
    pgwt_compute_top_queries(w, &f, from, to, 3000.0, &qr);
    snprintf(buf, sizeof(buf), "%s: top_queries reports failed", label);
    ck(qr.failed == 1, buf);
    snprintf(buf, sizeof(buf), "%s: top_queries has no rows", label);
    ck(qr.num_rows == 0 && qr.rows == NULL, buf);
    free(qr.rows);

    struct pgwt_concurrency_result cr;
    pgwt_compute_concurrency(w, &f, from, to, 3, 10000000ULL, 4, &cr);
    snprintf(buf, sizeof(buf), "%s: concurrency reports failed", label);
    ck(cr.failed == 1, buf);
    snprintf(buf, sizeof(buf), "%s: concurrency has no buckets", label);
    ck(cr.num_buckets == 0, buf);
    free(cr.peak_sessions);
    free(cr.peak_event);
    free(cr.bursts);
}

int main(void)
{
    struct pgwt_trace_event ev = one_record();

    printf("\n=== 1. the zero-initialised window (the one bypass the "
           "compiler cannot catch) ===\n");
    {
        struct pgwt_window w;
        memset(&w, 0, sizeof(w));
        ck(pgwt_window_valid(&w) == 0, "zeroed window is not valid");
        expect_all_six_absent(&w, "zeroed window");
    }

    printf("\n=== 2. a NULL window ===\n");
    {
        ck(pgwt_window_valid(NULL) == 0, "NULL window is not valid");
        expect_all_six_absent(NULL, "NULL window");
    }

    printf("\n=== 3. bind refuses, and a refused bind leaves the window "
           "invalid ===\n");
    {
        struct pgwt_overlay ov;
        memset(&ov, 0, sizeof(ov));
        ov.status = PGWT_OVERLAY_UNAVAILABLE;

        struct pgwt_window w;
        /* The whole point of binding: no overlay, no window. A reducer
         * reached through this path would otherwise have been handed records
         * with nothing to say about the right edge. */
        memset(&w, 0xAB, sizeof(w));
        ck(pgwt_window_bind(&w, &ev, 1, NULL) == -1,
           "bind refuses a NULL overlay");
        ck(pgwt_window_valid(&w) == 0,
           "a window refused for a NULL overlay is left invalid");

        memset(&w, 0xAB, sizeof(w));
        ck(pgwt_window_bind(&w, NULL, 1, &ov) == -1,
           "bind refuses count > 0 with no record array");
        ck(pgwt_window_valid(&w) == 0,
           "a window refused for a missing array is left invalid");

        memset(&w, 0xAB, sizeof(w));
        ck(pgwt_window_bind(&w, &ev, -1, &ov) == -1,
           "bind refuses a negative count");
        ck(pgwt_window_valid(&w) == 0,
           "a window refused for a negative count is left invalid");

        ck(pgwt_window_bind(NULL, &ev, 1, &ov) == -1,
           "bind refuses a NULL window");

        /* An EMPTY window is legitimate: zero records is a measurement. */
        memset(&w, 0, sizeof(w));
        ck(pgwt_window_bind(&w, NULL, 0, &ov) == 0,
           "bind accepts an empty window (0 records is a measurement)");
        ck(pgwt_window_valid(&w) == 1, "the empty window is valid");
    }

    printf("\n=== 4. span visibility is gated on the overlay STATUS, both "
           "ways ===\n");
    {
        struct pgwt_overlay_span spans[2];
        memset(spans, 0, sizeof(spans));
        spans[0].pid = 11;
        spans[0].wall_ns = 5;
        spans[1].pid = 22;
        spans[1].wall_ns = 7;

        struct pgwt_overlay ov;
        memset(&ov, 0, sizeof(ov));
        ov.spans = spans;
        ov.count = 2;

        struct pgwt_window w;

        /* The negative side: a degraded overlay that still carries spans.
         * If visibility keyed off the pointer, these two spans would be
         * added as in-flight time no daemon vouched for. */
        ov.status = PGWT_OVERLAY_UNAVAILABLE;
        ck(pgwt_window_bind(&w, &ev, 1, &ov) == 0, "bind an UNAVAILABLE overlay");
        ck(pgwt_window_nspans(&w) == 0,
           "spans of an UNAVAILABLE overlay are invisible");
        ck(pgwt_window_span(&w, 0) == NULL,
           "span 0 of an UNAVAILABLE overlay is unreachable");

        ov.status = PGWT_OVERLAY_REFUSED;
        ck(pgwt_window_bind(&w, &ev, 1, &ov) == 0, "bind a REFUSED overlay");
        ck(pgwt_window_nspans(&w) == 0,
           "spans of a REFUSED overlay are invisible");

        /* The positive side. Without this the two assertions above would
         * also pass with pgwt_window_nspans() hard-wired to 0 — a gate that
         * cannot see anything approves everything. */
        ov.status = PGWT_OVERLAY_INCLUDED;
        ck(pgwt_window_bind(&w, &ev, 1, &ov) == 0, "bind an INCLUDED overlay");
        ck(pgwt_window_nspans(&w) == 2,
           "spans of an INCLUDED overlay ARE visible");
        ck(pgwt_window_span(&w, 1) != NULL &&
           pgwt_window_span(&w, 1)->pid == 22,
           "span 1 of an INCLUDED overlay reads back correctly");
        ck(pgwt_window_span(&w, 2) == NULL,
           "span index past the end is refused, not read out of bounds");

        /* An INCLUDED overlay with a NULL array is a contradiction; treat it
         * as no spans rather than dereferencing it. */
        ov.spans = NULL;
        ov.count = 2;
        ck(pgwt_window_bind(&w, &ev, 1, &ov) == 0,
           "bind an INCLUDED overlay with a NULL span array");
        ck(pgwt_window_nspans(&w) == 0,
           "an INCLUDED overlay with no array reports no spans");
    }

    printf("\n=== 5. a VALID window over the same record DOES produce rows "
           "(the absences above are the guard, not the input) ===\n");
    {
        struct pgwt_overlay ov;
        memset(&ov, 0, sizeof(ov));
        ov.status = PGWT_OVERLAY_UNAVAILABLE;   /* fail-closed, as init leaves it */
        struct pgwt_window w;
        ck(pgwt_window_bind(&w, &ev, 1, &ov) == 0, "bind a valid window");

        struct pgwt_filter f;
        memset(&f, 0, sizeof(f));
        struct pgwt_events_result evr;
        pgwt_compute_top_events(&w, &f, 0, 3000ULL * 1000000ULL, 3000.0, &evr);
        ck(evr.failed == 0, "top_events over a valid window does not fail");
        ck(evr.num_rows == 1, "top_events over a valid window returns the row");
        ck(evr.rows && evr.rows[0].count == 1,
           "the row counts the single record");
        free(evr.rows);

        struct pgwt_tm_result tm;
        pgwt_compute_time_model(&w, &f, 0, 3000ULL * 1000000ULL, 3000.0, &tm);
        ck(tm.failed == 0, "time_model over a valid window does not fail");
        ck(tm.db_time_ms > 0.0, "time_model over a valid window has DB time");
        free(tm.rows);
    }

    printf("\n%d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
