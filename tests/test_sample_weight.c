/* #863: exact integer conservation, through the real sampler and trace writer.
 * --fixture DIR emits the same measured stream for test_data_fidelity.py. */
#include "sampler.h"
#include "event_writer.h"
#include "event_reader.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static int checks, failures;
#define CHECK(c, name) do { checks++; if (!(c)) { \
    fprintf(stderr, "FAIL: %s\n", name); failures++; } } while (0)

static const uint64_t P = 100000000ULL;
static const uint64_t S = 1000000000ULL;
static const uint64_t BASE = 10000000000000ULL;
static const uint64_t NEAR_PERIODS[] = {
    100000000, 100000001, 99999999, 100000002,
};

static void check_sum(const uint64_t *intervals, int n, const char *name)
{
    uint64_t now = BASE, expected = 0, actual = 0;
    for (int i = 0; i < n; i++) {
        uint64_t previous = now;
        now += intervals[i];
        expected += intervals[i];
        actual += pgwt_sampler_effective_period(P, previous, now);
    }
    if (actual != expected)
        fprintf(stderr, "%s: weight=%llu elapsed=%llu\n", name,
                (unsigned long long)actual, (unsigned long long)expected);
    CHECK(actual == expected, name);
}

static void test_intervals(void)
{
    uint64_t intervals[10000];
    uint32_t seed = 863;
    /* Fixed-seed LCG; rejection avoids modulo bias in the integer uniform
     * distribution [0.9p, 1.1p]. No libc rand()/platform dependence. */
    const uint32_t width = (uint32_t)(P / 5 + 1);
    const uint32_t reject = (uint32_t)(-width) % width;
    for (int i = 0; i < 10000; i++) {
        do { seed = seed * 1664525U + 1013904223U; } while (seed < reject);
        intervals[i] = P - P / 10 + seed % width;
    }
    check_sum(intervals, 10000, "jitter_sum_10000");
    for (int i = 0; i < 10000; i++) intervals[i] = P;
    check_sum(intervals, 10000, "equal_nominal_sum");
    for (int i = 0; i < 10000; i++) intervals[i] = P - P / 10;
    check_sum(intervals, 10000, "equal_short_sum");
    intervals[5000] = 5 * P;
    check_sum(intervals, 10000, "five_period_gap_sum");

    CHECK(pgwt_sampler_effective_period(P, 0, BASE) == P,
          "first_tick_nominal_edge");
    CHECK(pgwt_sampler_effective_period(P, BASE, BASE) == P,
          "equal_clock_nominal_fallback");
    CHECK(pgwt_sampler_effective_period(P, BASE, BASE - 1) == P,
          "backwards_clock_nominal_fallback");
    CHECK(pgwt_sampler_effective_period(P, BASE, BASE + 60 * S) == 60 * S,
          "stall_bound_inclusive");
    uint64_t gap = 61 * S + 123;
    uint64_t credited = pgwt_sampler_effective_period(P, BASE, BASE + gap);
    CHECK(credited == 60 * S, "stall_capped_at_60s");
    CHECK(gap - credited == S + 123, "stall_remainder_uncredited");
    CHECK(pgwt_sampler_effective_period(P, BASE + gap, BASE + gap + P / 2)
          == P / 2, "post_stall_uses_new_tick");

    uint64_t sequence[] = {P / 2, 5 * P, gap, P / 2};
    uint64_t now = BASE, weights = 0, elapsed = 0;
    for (int i = 0; i < 4; i++) {
        uint64_t last = now;
        now += sequence[i];
        elapsed += sequence[i];
        weights += pgwt_sampler_effective_period(P, last, now);
    }
    CHECK(weights + (gap - 60 * S) == elapsed,
          "stall_sequence_conserves_credited_plus_unobserved");

    /* Restart allocates a fresh zeroed sampler (sampler_start). Its first
     * observation gets only the edge term, never the previous run's gap. */
    struct pgwt_sampler restarted = {0};
    CHECK(pgwt_sampler_effective_period(P, restarted.last_tick_ns,
          BASE + 3600 * S) == P, "restart_does_not_credit_gap");
}

static int write_fixture(const char *dir, int near_periods, int escalation)
{
    struct pgwt_event_writer *w = calloc(1, sizeof(*w));
    if (!w || pgwt_writer_init(w, dir, 18, 24, NULL) != 0) return -1;
    uint64_t now = BASE;
    /* First tick is an explicit nominal edge before the queried window.
     * Alternating 90/110ms intervals tile exactly 60s. The near-period
     * variant catches averaging/truncation in the writer, including when
     * different backend counts would move weight between sessions. */
    int n = near_periods ? 4 : 601;
    for (int i = 0; i < n; i++) {
        uint64_t previous = i ? now : 0;
        if (i) now += near_periods ? NEAR_PERIODS[i] : (i % 2 ? 9 * P / 10 : 11 * P / 10);
        uint64_t weight = near_periods ? NEAR_PERIODS[i]
            : pgwt_sampler_effective_period(P, previous, now);
        /* The production sampler keeps ticking through exact windows. The
         * first tick after END straddles it; server exact-wins clipping must
         * remove the exact prefix, rather than credit the window twice. */
        uint64_t start = BASE + 10 * S + P / 2;
        uint64_t end = BASE + 20 * S + P / 2;
        if (escalation && previous < start && now >= start) {
            struct pgwt_trace_event marker = {.timestamp_ns = start,
                .old_event = PGWT_MARKER_ESCALATE_START,
                .new_event = PGWT_MARKER_ESCALATE_START};
            if (pgwt_writer_push_event(w, &marker) != 0) return -1;
        }
        if (escalation && previous < end && now >= end) {
            struct pgwt_trace_event exact = {.timestamp_ns = end,
                .pid = 1000, .old_event = WEI(PG_WAIT_IO, 1),
                .duration_ns = end - start, .query_id = 100};
            struct pgwt_trace_event marker = {.timestamp_ns = end,
                .old_event = PGWT_MARKER_ESCALATE_END,
                .new_event = PGWT_MARKER_ESCALATE_END};
            if (pgwt_writer_push_event(w, &exact) != 0 ||
                pgwt_writer_push_event(w, &marker) != 0) return -1;
        }
        struct pgwt_trace_event samples[2] = {
            {.timestamp_ns = now, .pid = 1000, .new_event = 0, .query_id = 100},
            {.timestamp_ns = now, .pid = 1001, .new_event = 0, .query_id = 100},
        };
        int count = near_periods && i % 2 ? 2 : 1;
        if (pgwt_writer_push_samples(w, samples, count, weight) != 0) return -1;
    }
    pgwt_writer_close(w);
    pgwt_writer_destroy(w);
    free(w);
    /* Pin the fixture clock just like gen_test_traces: wall == monotonic,
     * avoiding real wall-clock rounding in the server's JSON bounds. */
    char path[512];
    snprintf(path, sizeof(path), "%s/current.trace", dir);
    FILE *fp = fopen(path, "r+b");
    struct pgwt_trace_file_header header;
    if (!fp) return -1;
    if (fread(&header, sizeof(header), 1, fp) != 1) {
        fclose(fp);
        return -1;
    }
    header.start_time_ns = BASE;
    header.clock_offset_ns = BASE;
    rewind(fp);
    int ok = fwrite(&header, sizeof(header), 1, fp) == 1;
    fclose(fp);
    if (!ok) return -1;
    return 0;
}

static void test_writer(void)
{
    char dir[] = "/tmp/pgwt-weight-XXXXXX", path[256];
    CHECK(mkdtemp(dir) != NULL, "writer_tempdir");
    CHECK(write_fixture(dir, 1, 0) == 0, "writer_fixture");
    snprintf(path, sizeof(path), "%s/current.trace", dir);
    struct pgwt_event_reader r;
    if (pgwt_reader_open(&r, path) != 0) {
        CHECK(0, "writer_reader_open"); return;
    }
    uint64_t expected[6];
    int expected_count = 0;
    for (int tick = 0; tick < 4; tick++) {
        for (int pid = 0; pid < (tick % 2 ? 2 : 1); pid++)
            expected[expected_count++] = NEAR_PERIODS[tick];
    }
    struct pgwt_trace_event events[PGWT_BLOCK_EVENTS];
    int total = 0, wrong = 0;
    uint64_t sum = 0, expected_sum = 0;
    for (int b = 0; b < r.num_blocks; b++) {
        struct pgwt_block_info bi;
        int n = pgwt_reader_decode_block_info(&r, b, events, PGWT_BLOCK_EVENTS, &bi);
        for (int i = 0; i < n; i++, total++) {
            if (total >= expected_count || bi.sample_period_ns != expected[total]) wrong++;
            sum += bi.sample_period_ns;
        }
    }
    for (int i = 0; i < expected_count; i++) expected_sum += expected[i];
    CHECK(total == expected_count, "writer_record_count");
    CHECK(wrong == 0, "writer_preserves_each_tick_weight");
    CHECK(sum == expected_sum, "writer_preserves_integer_sum");
    pgwt_reader_close(&r);
    /* Only this test's writer artifacts live in its private temp directory. */
    const char *files[] = {"current.trace", "current.trace.meta"};
    for (int i = 0; i < 2; i++) {
        snprintf(path, sizeof(path), "%s/%s", dir, files[i]); unlink(path);
    }
    rmdir(dir);
}

int main(int argc, char **argv)
{
    if (argc == 3 && strcmp(argv[1], "--fixture") == 0)
        return write_fixture(argv[2], 0, 0) != 0;
    if (argc == 3 && strcmp(argv[1], "--escalation-fixture") == 0)
        return write_fixture(argv[2], 0, 1) != 0;
    test_intervals();
    test_writer();
    printf("sample_weight: %d/%d checks passed\n", checks - failures, checks);
    return failures != 0;
}
