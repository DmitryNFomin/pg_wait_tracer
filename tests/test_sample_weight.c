/* #863: exact integer conservation, through the real sampler and trace writer.
 * --fixture DIR emits the same measured stream for test_data_fidelity.py. */
#include "sampler.h"
#include "daemon.h"
#include "map_reader.h"
#include "event_writer.h"
#include "event_reader.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <dirent.h>

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
    struct pgwt_daemon *d = calloc(1, sizeof(*d));
    d->sample_rate_hz = 10;
    CHECK(pgwt_sampler_start(d) == 0, "restart_start_first_run");
    d->sampler->last_tick_ns = BASE;
    pgwt_sampler_stop(d);
    CHECK(pgwt_sampler_start(d) == 0, "restart_start_second_run");
    CHECK(pgwt_sampler_effective_period(P, d->sampler->last_tick_ns,
          BASE + 3600 * S) == P, "restart_does_not_credit_gap");
    pgwt_sampler_stop(d);
    free(d);
}

static int write_fixture(const char *dir, int near_periods, int escalation)
{
    struct pgwt_event_writer *w = calloc(1, sizeof(*w));
    if (!w || pgwt_writer_init(w, dir, 18, 24, NULL) != 0) return -1;
    w->nominal_sample_period_ns = P;
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
    CHECK(pgwt_reader_block_candidate(&r, 0, BASE - P, BASE - P / 2,
                                      NULL, NULL) == 1,
          "writer_reader_sample_lookahead");
    CHECK(pgwt_reader_block_candidate(&r, 0, BASE - 3 * P, BASE - 2 * P,
                                      NULL, NULL) == 0,
          "writer_reader_sample_lookahead_bounded");
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
            if (total >= expected_count || events[i].duration_ns != expected[total]) wrong++;
            sum += events[i].duration_ns;
        }
    }
    for (int i = 0; i < expected_count; i++) expected_sum += expected[i];
    CHECK(total == expected_count, "writer_record_count");
    CHECK(r.num_blocks == 1, "writer_batches_jittered_ticks");
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

/* The portable test never activates live watchpoints. */
struct pgwt_backend *pgwt_find_backend(struct pgwt_backend_table *bt, pid_t pid)
{
    (void)bt; (void)pid;
    abort();
}

static uint64_t overlap(uint64_t ts, uint64_t weight, uint64_t lo, uint64_t hi)
{
    uint64_t left = ts - weight;
    if (left < lo) left = lo;
    if (ts > hi) ts = hi;
    return ts > left ? ts - left : 0;
}

static void test_stream(void)
{
    char dir[] = "/tmp/pgwt-weight-stream-XXXXXX", path[512];
    CHECK(mkdtemp(dir) != NULL, "stream_tempdir");
    struct pgwt_event_writer *w = calloc(1, sizeof(*w));
    struct pgwt_daemon *d = calloc(1, sizeof(*d));
    struct pgwt_accumulator *live = calloc(1, sizeof(*live));
    pgwt_accum_init(live);
    d->event_accum = live;
    d->sample_rate_hz = 10;
    CHECK(pgwt_sampler_start(d) == 0, "stream_sampler_start");
    CHECK(pgwt_writer_init(w, dir, 18, 24, NULL) == 0, "stream_writer_init");
    w->nominal_sample_period_ns = P;
    struct pgwt_trace_event *pushed = calloc(12000, sizeof(*pushed));
    struct pgwt_trace_event *batch = calloc(5000, sizeof(*batch));
    uint64_t now = BASE, credited = 0, elapsed = 0, missing = 0;
    int count = 0;
    uint64_t after_empty_ts = 0;
    for (int tick = 0; tick < 40; tick++) {
        uint64_t dt = tick == 15 ? 61 * S + 123 :
                      (tick % 2 ? 9 * P / 10 + tick : 11 * P / 10 + tick);
        if (tick) { now += dt; elapsed += dt; }
        uint64_t weight = pgwt_sampler_tick_period(d->sampler, now);
        /* No writer call: this is the production count==0 return boundary.
         * Distinct empty and failed-read ticks still advance the clock. */
        if (tick == 5 || tick == 6) {
            missing += dt;
            CHECK(d->sampler->last_tick_ns == now, "empty_failed_tick_advances_clock");
            continue;
        }
        if (tick == 7) after_empty_ts = now;
        if (tick) credited += weight;
        int n = tick == 25 ? 5000 : (tick % 2 ? 2 : 1);
        for (int i = 0; i < n; i++) {
            batch[i] = (struct pgwt_trace_event){.timestamp_ns = now,
                .pid = (uint32_t)(1000 + i), .new_event = WEI(PG_WAIT_IO, 1),
                .query_id = 100};
            pushed[count] = batch[i];
            pushed[count++].duration_ns = weight;
        }
        pgwt_sampler_accumulate(d, batch, n, weight);
        CHECK(pgwt_writer_push_samples(w, batch, n, weight) == 0, "stream_push");
    }
    CHECK(credited + missing + S + 123 == elapsed,
          "stream_elapsed_equals_credited_empty_and_stall_excess");
    pgwt_writer_close(w);
    pgwt_writer_destroy(w); free(w);
    snprintf(path, sizeof(path), "%s/current.trace", dir);
    struct pgwt_event_reader r;
    CHECK(pgwt_reader_open(&r, path) == 0, "stream_reader_open");
    CHECK(r.header.version == 4, "stream_file_v4");
    CHECK(r.nominal_sample_period_ns == P, "stream_durable_nominal_cadence");
    struct pgwt_trace_event *decoded = calloc(12000, sizeof(*decoded));
    int total = 0, wrong = 0, time_cut = 0, capacity_cut = 0;
    int gap_cut = 0, clamp_cut = 0, max_later = 0;
    uint64_t sum = 0, expect_sum = 0;
    for (int b = 0; b < r.num_blocks; b++) {
        struct pgwt_block_info bi;
        int n = pgwt_reader_decode_block_info(&r, b, decoded + total,
                                              PGWT_BLOCK_EVENTS, &bi);
        CHECK(n > 0, "stream_block_decodes");
        if (n <= 0) continue;
        uint64_t max = 0;
        for (int i = 0; i < n; i++) {
            int k = total + i;
            if (k >= count || decoded[k].timestamp_ns != pushed[k].timestamp_ns ||
                decoded[k].pid != pushed[k].pid ||
                decoded[k].duration_ns != pushed[k].duration_ns) wrong++;
            if (decoded[k].duration_ns > max) max = decoded[k].duration_ns;
            sum += decoded[k].duration_ns;
        }
        struct pgwt_block_info header;
        CHECK(pgwt_reader_block_info(&r, b, &header) == 0 &&
              header.max_sample_weight_ns == max, "stream_header_maximum_exact");
        if (max > bi.sample_period_ns) max_later++;
        if (bi.last_timestamp_ns - bi.first_timestamp_ns >= S) time_cut++;
        if (bi.num_events == PGWT_BLOCK_EVENTS) capacity_cut++;
        if (bi.sample_period_ns == 60 * S) clamp_cut++;
        if (bi.first_timestamp_ns == after_empty_ts) gap_cut++;
        total += n;
    }
    for (int i = 0; i < count; i++) expect_sum += pushed[i].duration_ns;
    CHECK(total == count && wrong == 0, "stream_every_sample_weight_exact");
    CHECK(sum == expect_sum && live->tm.db_time_ns == expect_sum,
          "stream_reader_equals_live_integer_sum");
    CHECK(time_cut && capacity_cut, "stream_time_and_capacity_boundaries");
    CHECK(gap_cut && clamp_cut, "stream_empty_and_clamp_start_new_blocks");
    CHECK(max_later, "stream_maximum_is_not_first_weight");

    struct pgwt_accumulator *replay = calloc(1, sizeof(*replay));
    struct pgwt_replay_stats stats = {0};
    pgwt_accum_init(replay);
    pgwt_replay_file(&r, replay, 0, 0, batch, PGWT_BLOCK_EVENTS, NULL, &stats, NULL);
    CHECK(replay->tm.db_time_ns == live->tm.db_time_ns,
          "stream_cli_replay_equals_live_integer_sum");
    CHECK(stats.sample_period_ns == P, "stream_cli_reports_nominal_cadence");
    /* Many window edges inside ticks. Compare each integer overlap and
     * adjacent windows, with a pid whose observations include empty ticks. */
    int windows_wrong = 0;
    for (int i = 1; i < 40; i++) {
        uint64_t lo = BASE + (uint64_t)i * P / 3 + 7;
        uint64_t mid = lo + P + 13, hi = mid + 3 * P + 17;
        uint64_t expected = 0, halves = 0;
        for (int k = 0; k < count; k++) {
            expected += overlap(pushed[k].timestamp_ns, pushed[k].duration_ns, lo, hi);
            halves += overlap(decoded[k].timestamp_ns, decoded[k].duration_ns, lo, mid);
            halves += overlap(decoded[k].timestamp_ns, decoded[k].duration_ns, mid, hi);
        }
        pgwt_accum_init(replay);
        pgwt_replay_file(&r, replay, lo, hi, batch, PGWT_BLOCK_EVENTS, NULL, NULL, NULL);
        if (replay->tm.db_time_ns != expected || halves != expected) windows_wrong++;
    }
    CHECK(windows_wrong == 0, "stream_windows_exact_and_additive_cli");
    free(replay); free(decoded); free(batch); free(pushed);
    pgwt_reader_close(&r);
    pgwt_sampler_stop(d); free(d); free(live);
    unlink(path); snprintf(path, sizeof(path), "%s/current.trace.meta", dir);
    unlink(path); rmdir(dir);
}

static void test_legacy(const char *argv0)
{
    char path[1024];
    CHECK(realpath(argv0, path) != NULL, "legacy_fixture_executable_path");
    char *slash = strrchr(path, '/');
    if (!slash) return;
    snprintf(slash + 1, (size_t)(path + sizeof(path) - slash - 1),
             "fixtures/sample_weights_v3/current.trace");
    struct pgwt_event_reader r;
    CHECK(pgwt_reader_open(&r, path) == 0, "legacy_fixture_open");
    if (!r.fp) return;
    CHECK(r.header.version == 3 && r.num_blocks == 2, "legacy_fixture_version_and_blocks");
    CHECK(r.nominal_sample_period_ns == 0, "legacy_nominal_unknown");
    uint64_t ts[] = {BASE, BASE + 90000000, BASE + 290000000};
    uint64_t weights[] = {P, P, 110000000};
    struct pgwt_trace_event events[PGWT_BLOCK_EVENTS];
    int total = 0, wrong = 0;
    for (int b = 0; b < r.num_blocks; b++) {
        struct pgwt_block_info bi;
        int n = pgwt_reader_decode_block_info(&r, b, events, PGWT_BLOCK_EVENTS, &bi);
        if (n <= 0) { wrong++; continue; }
        for (int i = 0; i < n; i++, total++) {
            if (total >= 3 || events[i].timestamp_ns != ts[total] ||
                events[i].duration_ns != weights[total] ||
                bi.max_sample_weight_ns != bi.sample_period_ns) wrong++;
        }
    }
    CHECK(total == 3 && wrong == 0, "legacy_stored_inflated_mean_weights_retained");
    pgwt_reader_close(&r);
}

static int write_lookahead_fixture(const char *dir)
{
    struct pgwt_event_writer *w = calloc(1, sizeof(*w));
    if (!w || pgwt_writer_init(w, dir, 18, 24, NULL)) return 1;
    w->nominal_sample_period_ns = P;
    uint64_t weights[] = {P, 9 * P, P - 1, P - 1};
    uint64_t now = BASE;
    for (int i = 0; i < 4; i++) {
        if (i == 3 && pgwt_writer_publish(w)) return 1;
        if (i) now += weights[i];
        struct pgwt_trace_event e = {.timestamp_ns = now, .pid = 1000,
            .new_event = 0, .query_id = 100};
        if (pgwt_writer_push_samples(w, &e, 1, weights[i])) return 1;
    }
    pgwt_writer_close(w); pgwt_writer_destroy(w); free(w);
    char path[512];
    snprintf(path, sizeof(path), "%s/current.trace", dir);
    FILE *fp = fopen(path, "r+b");
    struct pgwt_trace_file_header h;
    if (!fp || fread(&h, sizeof(h), 1, fp) != 1) return 1;
    h.start_time_ns = h.clock_offset_ns = BASE;
    rewind(fp); int ok = fwrite(&h, sizeof(h), 1, fp) == 1;
    fclose(fp); return !ok;
}

static void test_capture_metadata(void)
{
    char dir[] = "/tmp/pgwt-weight-generation-XXXXXX";
    CHECK(mkdtemp(dir) != NULL, "generation_tempdir");
    struct pgwt_event_writer *w = calloc(1, sizeof(*w));
    CHECK(pgwt_writer_init(w, dir, 18, 24, NULL) == 0, "generation_writer_init");
    w->nominal_sample_period_ns = P;
    struct pgwt_trace_event e = {.timestamp_ns = BASE, .pid = 1000};
    CHECK(pgwt_writer_push_samples(w, &e, 1, P) == 0, "generation_first_tick");
    e.timestamp_ns += P - 1;
    CHECK(pgwt_writer_push_samples(w, &e, 1, P - 1) == 0, "generation_jitter_tick");
    w->force_rotate = true;
    CHECK(pgwt_writer_check_rotation(w) == 0, "generation_rotation");
    e.timestamp_ns += P + 1;
    CHECK(pgwt_writer_push_samples(w, &e, 1, P + 1) == 0, "generation_after_rotation");
    pgwt_writer_close(w); pgwt_writer_destroy(w);

    /* Startup recovery must preserve the previous capture's nominal field,
     * even when this new capture has a different configured cadence. */
    CHECK(pgwt_writer_init(w, dir, 18, 24, NULL) == 0, "generation_restart_recovery");
    w->nominal_sample_period_ns = 2 * P;
    e.timestamp_ns += 2 * P;
    CHECK(pgwt_writer_push_samples(w, &e, 1, 2 * P) == 0, "generation_new_capture");
    pgwt_writer_close(w); pgwt_writer_destroy(w); free(w);
    struct pgwt_trace_file_entry files[8];
    int n = pgwt_scan_trace_files(dir, files, 8);
    CHECK(n == 3, "generation_rotation_recovery_and_current_files");
    int wrong = 0;
    uint64_t sum = 0;
    struct pgwt_trace_event events[PGWT_BLOCK_EVENTS];
    for (int f = 0; f < n; f++) {
        struct pgwt_event_reader r;
        if (pgwt_reader_open(&r, files[f].path) != 0) { wrong++; continue; }
        uint64_t nominal = strstr(files[f].path, "current.trace") ? 2 * P : P;
        if (r.nominal_sample_period_ns != nominal) wrong++;
        for (int b = 0; b < r.num_blocks; b++) {
            int count = pgwt_reader_decode_block(&r, b, events, PGWT_BLOCK_EVENTS);
            if (count < 0) { wrong++; continue; }
            for (int i = 0; i < count; i++) sum += events[i].duration_ns;
        }
        pgwt_reader_close(&r);
    }
    CHECK(wrong == 0, "nominal_metadata_survives_rotation_recovery_and_new_capture");
    CHECK(sum == 5 * P, "weights_survive_rotation_and_recovery");
    DIR *d = opendir(dir);
    struct dirent *ent;
    while (d && (ent = readdir(d))) {
        if (ent->d_name[0] == '.') continue;
        char path[600]; snprintf(path, sizeof(path), "%s/%s", dir, ent->d_name);
        unlink(path);
    }
    if (d) closedir(d);
    rmdir(dir);
}

int main(int argc, char **argv)
{
    if (argc == 3 && strcmp(argv[1], "--fixture") == 0)
        return write_fixture(argv[2], 0, 0) != 0;
    if (argc == 3 && strcmp(argv[1], "--escalation-fixture") == 0)
        return write_fixture(argv[2], 0, 1) != 0;
    if (argc == 3 && strcmp(argv[1], "--lookahead-fixture") == 0)
        return write_lookahead_fixture(argv[2]);
    test_intervals();
    test_writer();
    test_stream();
    test_legacy(argv[0]);
    test_capture_metadata();
    printf("sample_weight: %d/%d checks passed\n", checks - failures, checks);
    return failures != 0;
}
