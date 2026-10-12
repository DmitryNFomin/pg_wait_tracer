/* #359: exactly 4096 pairs must return; 4097 used to spin forever.
 * Each time-disjoint group contributes one pair, repeated in three rounds
 * with reversed PID order and distinct durations. The 50,000 interval cap
 * is never reached. Build with PGWT_INTERFERENCE_PARENT to run this SAME
 * fixture against the parent (which has no result.failed) under timeout.
 * --pairs selects the at-cap control or cap+1 reproduction independently.
 */
#include "compute.h"
#include "summary_reader.h"
#include "wait_event.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MS 1000000ULL
#define WAIT_A WEI(PG_WAIT_LWLOCK, 1)
#define WAIT_B WEI(PG_WAIT_LWLOCK, 2)

/* Interference does not read summaries. */
int pgwt_visit_summaries(const char *d, uint64_t a, uint64_t b,
                         pgwt_summary_visitor visitor, void *ctx)
{
    (void)d; (void)a; (void)b; (void)visitor; (void)ctx;
    return -1;
}

static int checks, failures;
#define CHECK(cond, msg) do { \
    checks++; \
    if (!(cond)) { failures++; printf("FAIL: %s (line %d)\n", msg, __LINE__); } \
} while (0)

static struct pgwt_trace_event *fixture(int pairs)
{
    struct pgwt_trace_event *events = calloc((size_t)pairs * 6, sizeof(*events));
    if (!events) { fprintf(stderr, "fixture allocation failed\n"); exit(2); }
    uint64_t ts = 1000000000000ULL;
    int n = 0;
    for (int round = 0; round < 3; round++) {
        for (int k = 1; k <= pairs; k++) {
            uint64_t duration = (uint64_t)(round + 1) * k * MS;
            for (int side = 0; side < 2; side++) {
                struct pgwt_trace_event *ev = &events[n++];
                ev->pid = side == (round & 1) ? 0 : (uint32_t)k;
                ev->old_event = round == 2 ? WAIT_B : WAIT_A;
                ev->timestamp_ns = ts + duration;
                ev->duration_ns = duration;
                ev->cpu_ns = PGWT_CPU_NS_UNKNOWN;
            }
            ts += duration + MS;
        }
    }
    return events;
}

static void run_pairs(int pairs, int limit)
{
    struct pgwt_trace_event *events = fixture(pairs);
    struct pgwt_filter f = {0};
    struct pgwt_interference_result res;
    printf("CALL pairs=%d intervals=%d limit=%d\n", pairs, pairs * 6, limit);
    fflush(stdout);
    pgwt_compute_interference(events, pairs * 6, &f, limit, &res);
#ifndef PGWT_INTERFERENCE_PARENT
    CHECK(res.failed == 0, "successful result has no failure flag");
#endif
    int want = pairs < limit ? pairs : limit;
    CHECK(res.num_rows == want, "every requested pair is returned");
    CHECK(res.rows != NULL, "result has rows");
    uint64_t total = 0, expected_total = 0;
    if (res.rows) {
        for (int i = 0; i < res.num_rows; i++) {
            int k = pairs - i;
            CHECK(res.rows[i].pid_a == 0 && res.rows[i].pid_b == (uint32_t)k,
                  "unordered pairs remain distinct and sorted by overlap");
            CHECK(res.rows[i].overlap_ns == 6 * (uint64_t)k * MS,
                  "all rounds contribute their exact overlap");
            CHECK(res.rows[i].score == (double)k / pairs,
                  "score uses the maximum across all pairs");
            CHECK(res.rows[i].top_event == WAIT_B,
                  "largest overlap retains its wait event");
            total += res.rows[i].overlap_ns;
        }
    }
    for (int i = 0; i < want; i++)
        expected_total += 6 * (uint64_t)(pairs - i) * MS;
    CHECK(total == expected_total, "returned overlap total is exact");
    if (!failures)
        printf("RETURNED num_rows=%d overlap_ns=%llu\n", res.num_rows,
               (unsigned long long)total);
    free(res.rows);
    free(events);
}

#ifndef PGWT_INTERFERENCE_PARENT
static void run_refusals(void)
{
    struct pgwt_trace_event *events = fixture(4097);
    struct pgwt_filter f = {0};
    const char *points[] = {
        "interference_intervals", "triple_map_grow",
        "interference_entries", "interference_rows"
    };
    for (size_t i = 0; i < sizeof(points) / sizeof(points[0]); i++) {
        struct pgwt_interference_result res;
        memset(&res, 0xa5, sizeof(res));
        if (setenv("PGWT_TEST_ALLOC_FAIL", points[i], 1) != 0) exit(2);
        pgwt_compute_interference(events, 4097 * 6, &f, 30, &res);
        unsetenv("PGWT_TEST_ALLOC_FAIL");
        CHECK(res.failed == 1, "allocation failure sets the failure flag");
        CHECK(res.rows == NULL, "allocation failure withholds all rows");
        CHECK(res.num_rows == 0, "allocation failure clears the count");
        printf("REFUSAL point=%s failed=%d num_rows=%d\n",
               points[i], res.failed, res.num_rows);
        free(res.rows);
    }
    free(events);

    struct pgwt_interference_result empty;
    memset(&empty, 0xa5, sizeof(empty));
    pgwt_compute_interference(NULL, 0, &f, 30, &empty);
    CHECK(empty.failed == 0 && empty.num_rows == 0 && empty.rows == NULL,
          "empty input is a successful empty result");
}
#endif

int main(int argc, char **argv)
{
    if (argc == 3 && strcmp(argv[1], "--pairs") == 0) {
        int pairs = atoi(argv[2]);
        if (pairs != 4096 && pairs != 4097) return 2;
        run_pairs(pairs, pairs);
    } else if (argc == 1) {
        run_pairs(4096, 4096);
        run_pairs(4097, 4097);
        run_pairs(4097, 30);
#ifndef PGWT_INTERFERENCE_PARENT
        run_refusals();
#endif
    } else {
        return 2;
    }
    printf("%d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
