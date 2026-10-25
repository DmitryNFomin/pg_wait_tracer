/* #387: measured CPU must come from a nonzero in-window fraction.
 * Optional argv[1]: write byte snapshots of NONEMPTY results for cmp against
 * a binary built with the pre-fix compute.c (no pointers in the snapshot).
 * No tolerance: component assertions are exact; snapshots include every
 * scalar and row, including padding zeroed by the production allocator.
 */
#include "compute.h"
#include "summary_reader.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MS(n) ((uint64_t)(n) * 1000000ULL)
#define FROM MS(100)
#define TO MS(110)
#define LOCK 0x03000000u

static int failures;
static FILE *snapshot;
static const struct pgwt_filter all = {0};

#define CHECK(cond) do { \
    if (!(cond)) { \
        fprintf(stderr, "FAIL: %s (%s:%d)\n", #cond, __FILE__, __LINE__); \
        failures++; \
    } \
} while (0)

/* Summary streaming is not used by the raw time model under test. */
int pgwt_visit_summaries(const char *dir, uint64_t from, uint64_t to,
                         pgwt_summary_visitor visitor, void *ctx)
{
    (void)dir; (void)from; (void)to; (void)visitor; (void)ctx;
    return -1;
}

static void test_empty_window(void)
{
    struct pgwt_tm_result tm;
    pgwt_compute_time_model(NULL, 0, &all, FROM, TO, 10.0, &tm);
    CHECK(!tm.has_measured_cpu);
    CHECK(tm.db_time_ms == 0.0);
    CHECK(tm.cpu_ms == 0.0);
    CHECK(tm.offcpu_ms == 0.0); /* unavailable: flag, not this scalar, decides */
    CHECK(tm.rows != NULL && tm.num_rows == 1); /* absence returns a result */
    free(tm.rows);
    puts("test_empty_window complete");
}

static void test_zero_fraction_records(void)
{
    /* The end-at-FROM and zero-duration cases have timestamps IN the window,
     * which is why merely checking count or timestamp misses this defect.
     * Exercise both measured-CPU assignments independently and together.
     */
    struct pgwt_trace_event events[8] = {0};
    for (int cls = 0; cls < 2; cls++) {
        for (int shape = 0; shape < 4; shape++) {
            struct pgwt_trace_event *ev = &events[cls * 4 + shape];
            ev->pid = 1;
            ev->old_event = cls ? LOCK : 0;
            ev->cpu_ns = MS(1);
            ev->duration_ns = shape == 3 ? 0 : MS(2);
            ev->timestamp_ns = shape == 0 ? FROM - MS(1) :
                               shape == 1 ? FROM :
                               shape == 2 ? TO + MS(2) : FROM + MS(5);
            struct pgwt_tm_result tm;
            pgwt_compute_time_model(ev, 1, &all, FROM, TO, 10.0, &tm);
            CHECK(!tm.has_measured_cpu);
            CHECK(tm.db_time_ms == 0.0 && tm.offcpu_ms == 0.0);
            CHECK(tm.cpu_ms == 0.0 && tm.wait_gap_cpu_ms == 0.0);
            CHECK(tm.rows != NULL && tm.num_rows == 1);
            free(tm.rows);
        }
    }
    struct pgwt_tm_result tm;
    pgwt_compute_time_model(events, 8, &all, FROM, TO, 10.0, &tm);
    CHECK(!tm.has_measured_cpu);
    CHECK(tm.db_time_ms == 0.0 && tm.offcpu_ms == 0.0);
    CHECK(tm.rows != NULL && tm.num_rows == 1);
    free(tm.rows);
    puts("test_zero_fraction_records complete (8 individual cases + combined)");
}

static void test_nonempty_windows(void)
{
    /* Whole, clipped, measured zero, wait-only, UNKNOWN-only, mixed measured
     * and UNKNOWN, clamped CPU, and an unrelated out-of-window measurement.
     * These all have positive DB Time, so a before/after cmp cannot pass by
     * comparing two empty results. Also exercise one-nanosecond overlap.
     */
    for (int shape = 0; shape < 9; shape++) {
        struct pgwt_trace_event events[] = {
            {.pid = 1, .timestamp_ns = FROM + MS(6), .duration_ns = MS(6),
             .cpu_ns = MS(3)},
            {.pid = 1, .old_event = LOCK, .timestamp_ns = TO,
             .duration_ns = MS(4), .cpu_ns = MS(1)},
            {.pid = 1, .timestamp_ns = FROM, .duration_ns = MS(2),
             .cpu_ns = MS(1)}
        };
        uint64_t from = FROM, to = TO;
        int count = 2;
        switch (shape) {
        case 1: from += MS(2); to -= MS(2); break;
        case 2: events[0].cpu_ns = events[1].cpu_ns = 0; break;
        case 3: events[0].old_event = LOCK; break;
        case 4: events[0].cpu_ns = events[1].cpu_ns = PGWT_CPU_NS_UNKNOWN; break;
        case 5: events[0].cpu_ns = PGWT_CPU_NS_UNKNOWN; break;
        case 6: events[0].cpu_ns = MS(9); break;
        case 7: count = 3; break;
        case 8: from = events[0].timestamp_ns - 1; to = events[0].timestamp_ns; break;
        }
        struct pgwt_tm_result tm;
        pgwt_compute_time_model(events, count, &all, from, to, 10.0, &tm);
        CHECK(tm.db_time_ms > 0.0);
        /* #575: THIS TRIPWIRE FIRED AS DESIGNED, and it is updated with the
         * fix rather than around it.
         *
         * It used to read `shape != 4` — i.e. "only the all-UNKNOWN window
         * lacks a measurement" — which recorded the OLD, wider predicate:
         * has_measured_cpu was also raised by measured spin inside a WAIT.
         * That is a different question from "is this response's CPU row a
         * measurement", and the difference was visible: with an UNKNOWN CPU
         * gap beside a measured wait (shape 5), time_model labelled the
         * gap's unmeasured wall "CPU (running)" and printed a 0.0 ms
         * runqueue sibling next to it.
         *
         * The predicate now is: an in-window CPU GAP carried a measurement.
         *   shape 3 — events[0] is turned into a LOCK, so the window has NO
         *             on-CPU gap at all; only wait spin. -> 0
         *   shape 4 — both records UNKNOWN.                -> 0
         *   shape 5 — the ONLY gap is UNKNOWN; the measurement that remains
         *             is the LOCK's spin.                  -> 0
         * every other shape has a measured gap (including shape 2's measured
         * ZERO, which is a signal, and shape 8's one-nanosecond overlap). */
        CHECK(tm.has_measured_cpu ==
              (shape != 3 && shape != 4 && shape != 5));
        if (shape == 0 || shape == 7) {
            CHECK(tm.db_time_ms == 10.0);
            CHECK(tm.cpu_ms == 3.0);
            CHECK(tm.offcpu_ms == 3.0);
            CHECK(tm.wait_gap_cpu_ms == 1.0);
            CHECK(tm.cpu_clamped_ms == 0.0);
        }
        if (shape == 1) {
            CHECK(tm.db_time_ms == 6.0);
            CHECK(tm.cpu_ms == 2.0);
            CHECK(tm.offcpu_ms == 2.0);
            CHECK(tm.wait_gap_cpu_ms == 0.5);
        }
        if (shape == 2) CHECK(tm.cpu_ms == 0.0 && tm.offcpu_ms == 6.0);
        if (shape == 3) CHECK(tm.cpu_ms == 0.0 && tm.offcpu_ms == 0.0);
        if (shape == 4 || shape == 5) CHECK(tm.cpu_ms == 6.0 && tm.offcpu_ms == 0.0);
        if (shape == 6) CHECK(tm.cpu_ms == 6.0 && tm.cpu_clamped_ms == 3.0);
        if (snapshot) {
            /* memset(out) and calloc(rows) make padding deterministic on the
             * same build platform. Normalize only the allocated pointer. */
            struct pgwt_tm_row *rows = tm.rows;
            tm.rows = NULL;
            CHECK(fwrite(&tm, sizeof(tm), 1, snapshot) == 1);
            CHECK(fwrite(rows, sizeof(*rows), tm.num_rows, snapshot) ==
                  (size_t)tm.num_rows);
            tm.rows = rows;
        }
        free(tm.rows);
    }
    puts("test_nonempty_windows complete (9 nonempty snapshots)");
}

int main(int argc, char **argv)
{
    if (argc > 1) {
        snapshot = fopen(argv[1], "wb");
        if (!snapshot) { perror(argv[1]); return 1; }
    }
    test_empty_window();
    test_zero_fraction_records();
    test_nonempty_windows();
    if (snapshot) CHECK(fclose(snapshot) == 0);
    printf("%s: test_time_model_absence (%d failures)\n",
           failures ? "FAIL" : "PASS", failures);
    return failures ? 1 : 0;
}
