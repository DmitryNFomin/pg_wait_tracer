/* test_durability.c — Phase T5 regression tests (DUR-1/2/3/5/7/8)
 *
 * Covers the file-lifecycle guarantees:
 *   DUR-1: a daemon crash (kill -9) never erases the previous current.trace/
 *          current.summary — restart recovers it into a readable archive;
 *          torn tails are dropped, committed blocks survive.
 *   DUR-2: rotation and recovery never clobber an existing archive
 *          (numeric-suffix collision-safe names; covers restart-in-the-same-
 *          hour and the DST-fold case, which produce the same target name).
 *   DUR-3: size-based retention (--retention-gb): oldest archives deleted
 *          first, live current files never touched; orphaned meta files
 *          cleaned.
 *   DUR-7: a corrupt meta high-watermark or block header never drives the
 *          reader into a huge allocation or a garbage index.
 *   DUR-8: SAMPLES pushes are batched ~1 s per block (not per tick), the
 *          per-block period preserves the SMP-3 weights, a period jump cuts
 *          a new block, and the block index stays time-sorted when
 *          transitions and samples interleave.
 *
 * Server build (-DPGWT_SERVER): no BPF needed, runs in CI's server jobs.
 */
#include "event_writer.h"
#include "event_reader.h"
#include "summary_writer.h"
#include "summary_reader.h"
#include "pg_wait_tracer.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <signal.h>
#include <sys/wait.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <dirent.h>
#include <time.h>
#include <errno.h>

static int tests_run = 0;
static int tests_passed = 0;

#define CHECK(cond, fmt, ...) do { \
    tests_run++; \
    if (cond) { tests_passed++; } \
    else { printf("  FAIL(%d): " fmt "\n", __LINE__, ##__VA_ARGS__); } \
} while (0)

#define BASE_DIR "/tmp/pgwt_durability_test"

/* ── helpers ────────────────────────────────────────────── */

static void rm_rf(const char *dir)
{
    char cmd[600];
    snprintf(cmd, sizeof(cmd), "rm -rf %s", dir);
    if (system(cmd) != 0) { /* ignore */ }
}

static char *path_of(const char *dir, const char *name)
{
    static char buf[600];
    snprintf(buf, sizeof(buf), "%s/%s", dir, name);
    return buf;
}

static int file_exists(const char *dir, const char *name)
{
    struct stat st;
    return stat(path_of(dir, name), &st) == 0;
}

static long file_size(const char *dir, const char *name)
{
    struct stat st;
    if (stat(path_of(dir, name), &st) != 0)
        return -1;
    return (long)st.st_size;
}

/* Count directory entries whose name ends with `suffix`. */
static int count_suffix(const char *dir, const char *suffix)
{
    DIR *d = opendir(dir);
    if (!d) return -1;
    int n = 0;
    size_t sl = strlen(suffix);
    struct dirent *e;
    while ((e = readdir(d)) != NULL) {
        size_t nl = strlen(e->d_name);
        if (nl >= sl && strcmp(e->d_name + nl - sl, suffix) == 0)
            n++;
    }
    closedir(d);
    return n;
}

/* First entry matching suffix (caller copies). */
static int find_suffix(const char *dir, const char *suffix, char *out,
                       size_t out_size)
{
    DIR *d = opendir(dir);
    if (!d) return -1;
    int found = -1;
    size_t sl = strlen(suffix);
    struct dirent *e;
    while ((e = readdir(d)) != NULL) {
        size_t nl = strlen(e->d_name);
        if (nl >= sl && strcmp(e->d_name + nl - sl, suffix) == 0) {
            snprintf(out, out_size, "%s/%s", dir, e->d_name);
            found = 0;
            break;
        }
    }
    closedir(d);
    return found;
}

static struct pgwt_trace_event mk_event(uint64_t ts, uint32_t pid,
                                        uint32_t old_ev, uint64_t dur,
                                        uint64_t qid)
{
    struct pgwt_trace_event ev = {0};
    ev.timestamp_ns = ts;
    ev.pid = pid;
    ev.old_event = old_ev;
    ev.new_event = 0;
    ev.duration_ns = dur;
    ev.query_id = qid;
    return ev;
}

/* Push exactly `blocks` full blocks of transitions; returns final ts. */
static uint64_t push_blocks(struct pgwt_event_writer *w, uint64_t ts,
                            int blocks)
{
    for (int b = 0; b < blocks; b++) {
        for (int i = 0; i < PGWT_BLOCK_EVENTS; i++) {
            struct pgwt_trace_event ev =
                mk_event(ts, 1000 + (i % 4), 0x0A000001, 1000, 42);
            pgwt_writer_push_event(w, &ev);
            ts += 1000;
        }
    }
    return ts;
}

static int read_meta_committed(const char *dir, const char *name)
{
    FILE *f = fopen(path_of(dir, name), "r");
    if (!f) return -1;
    int committed = -1;
    if (fscanf(f, "%d", &committed) != 1)
        committed = -1;
    fclose(f);
    return committed;
}

/* Total decoded events across all blocks of a trace file. */
static long count_events_in_file(const char *path, int *num_blocks)
{
    struct pgwt_event_reader r;
    if (pgwt_reader_open(&r, path) != 0) {
        if (num_blocks) *num_blocks = -1;
        return -1;
    }
    if (num_blocks) *num_blocks = r.num_blocks;
    static struct pgwt_trace_event buf[PGWT_BLOCK_EVENTS];
    long total = 0;
    for (int b = 0; b < r.num_blocks; b++) {
        int n = pgwt_reader_decode_block(&r, b, buf, PGWT_BLOCK_EVENTS);
        if (n > 0)
            total += n;
    }
    pgwt_reader_close(&r);
    return total;
}

/* ── DUR-1: kill -9 → restart recovers committed data ─────── */

/* Child: write 2 committed blocks, then hang. Parent kills it with SIGKILL
 * once the meta high-watermark reaches 2 — a REAL post-flush kill -9. */
static void test_recover_after_sigkill(void)
{
    printf("--- DUR-1: kill -9 post-flush, restart recovers ---\n");
    const char *dir = BASE_DIR "/kill9";
    rm_rf(dir);

    pid_t child = fork();
    if (child == 0) {
        struct pgwt_event_writer *w = calloc(1, sizeof(*w));
        if (pgwt_writer_init(w, dir, 18, 24, NULL) != 0)
            _exit(1);
        push_blocks(w, 1000000000000ULL, 2);
        /* 100 buffered (uncommitted) events on top — the "unflushed tail" */
        uint64_t ts = 2000000000000ULL;
        for (int i = 0; i < 100; i++) {
            struct pgwt_trace_event ev = mk_event(ts, 1000, 0x0A000001, 10, 1);
            pgwt_writer_push_event(w, &ev);
            ts += 10;
        }
        for (;;) pause();
    }
    CHECK(child > 0, "fork");

    /* Wait for the meta watermark to commit 2 blocks, then SIGKILL. */
    int committed = -1;
    for (int i = 0; i < 500; i++) {
        committed = read_meta_committed(dir, "current.trace.meta");
        if (committed >= 2)
            break;
        usleep(10000);
    }
    CHECK(committed == 2, "child committed 2 blocks (got %d)", committed);
    kill(child, SIGKILL);
    waitpid(child, NULL, 0);

    CHECK(file_exists(dir, "current.trace"), "current.trace left behind");

    /* Restart: init must recover, never truncate. */
    struct pgwt_event_writer *w2 = calloc(1, sizeof(*w2));
    CHECK(pgwt_writer_init(w2, dir, 18, 24, NULL) == 0, "restart init");
    CHECK(!file_exists(dir, "current.trace"),
          "current.trace archived on restart");
    CHECK(!file_exists(dir, "current.trace.meta"), "stale meta removed");
    CHECK(count_suffix(dir, ".trace.lz4") == 1, "one archive present");

    char archive[600];
    CHECK(find_suffix(dir, ".trace.lz4", archive, sizeof(archive)) == 0,
          "archive found");
    int nb = 0;
    long events = count_events_in_file(archive, &nb);
    CHECK(nb == 2, "recovered archive has 2 blocks (got %d)", nb);
    CHECK(events == 2 * PGWT_BLOCK_EVENTS,
          "all committed events readable (got %ld)", events);

    pgwt_writer_close(w2);
    pgwt_writer_destroy(w2);
    free(w2);
}

/* Mid-block kill: a torn tail (partial block header/payload) after the
 * committed blocks must be dropped by recovery, never crash a reader. */
static void test_recover_torn_tail(void)
{
    printf("--- DUR-1: torn tail (kill mid-block-write) ---\n");
    const char *dir = BASE_DIR "/torn";
    rm_rf(dir);

    struct pgwt_event_writer *w = calloc(1, sizeof(*w));
    CHECK(pgwt_writer_init(w, dir, 18, 24, NULL) == 0, "init");
    push_blocks(w, 1000000000000ULL, 1);
    /* Simulate kill -9 mid-fwrite: release the fd without footer, then
     * append a plausible-but-truncated block (header claims more payload
     * than the file holds). */
    fclose(w->fp);
    w->fp = NULL;
    pgwt_writer_destroy(w);
    free(w);

    FILE *f = fopen(path_of(dir, "current.trace"), "ab");
    CHECK(f != NULL, "append torn tail");
    struct pgwt_trace_block_header torn = {
        .first_timestamp_ns = 3000000000000ULL,
        .last_timestamp_ns = 3000000001000ULL,
        .num_events = 100,
        .compressed_size = 5000,   /* payload NOT fully written */
        .uncompressed_size = 6000,
        .block_type = 0,
    };
    fwrite(&torn, sizeof(torn), 1, f);
    uint8_t junk[123];
    memset(junk, 0xAB, sizeof(junk));
    fwrite(junk, 1, sizeof(junk), f);   /* only 123 of 5000 bytes */
    fclose(f);

    /* Also leave a torn meta tmp (kill mid-meta-rename). */
    f = fopen(path_of(dir, "current.trace.meta.tmp"), "w");
    if (f) { fprintf(f, "1"); fclose(f); }

    struct pgwt_event_writer *w2 = calloc(1, sizeof(*w2));
    CHECK(pgwt_writer_init(w2, dir, 18, 24, NULL) == 0, "restart init");
    CHECK(!file_exists(dir, "current.trace.meta.tmp"),
          "torn meta tmp removed");
    char archive[600];
    CHECK(find_suffix(dir, ".trace.lz4", archive, sizeof(archive)) == 0,
          "archive exists");
    int nb = 0;
    long events = count_events_in_file(archive, &nb);
    CHECK(nb == 1, "torn tail dropped, 1 committed block (got %d)", nb);
    CHECK(events == PGWT_BLOCK_EVENTS, "committed events intact (got %ld)",
          events);
    pgwt_writer_close(w2);
    pgwt_writer_destroy(w2);
    free(w2);
}

/* Restart twice within one hour: BOTH archives must survive (DUR-1+2). */
static void test_restart_twice_same_hour(void)
{
    printf("--- DUR-1/2: two restarts in one hour keep both archives ---\n");
    const char *dir = BASE_DIR "/twice";
    rm_rf(dir);

    for (int round = 0; round < 2; round++) {
        struct pgwt_event_writer *w = calloc(1, sizeof(*w));
        CHECK(pgwt_writer_init(w, dir, 18, 24, NULL) == 0, "init %d", round);
        push_blocks(w, 1000000000000ULL + (uint64_t)round * 1000000000ULL, 1);
        /* crash: no footer, no close */
        fclose(w->fp);
        w->fp = NULL;
        pgwt_writer_destroy(w);
        free(w);
    }
    /* Third start recovers round 2's file. */
    struct pgwt_event_writer *w = calloc(1, sizeof(*w));
    CHECK(pgwt_writer_init(w, dir, 18, 24, NULL) == 0, "third init");
    int archives = count_suffix(dir, ".trace.lz4");
    CHECK(archives == 2, "both archives present (got %d)", archives);

    /* Each archive must decode its full block. */
    DIR *d = opendir(dir);
    struct dirent *e;
    while ((e = readdir(d)) != NULL) {
        if (!strstr(e->d_name, ".trace.lz4"))
            continue;
        int nb = 0;
        long events = count_events_in_file(path_of(dir, e->d_name), &nb);
        CHECK(events == PGWT_BLOCK_EVENTS, "%s readable (%ld events)",
              e->d_name, events);
    }
    closedir(d);
    pgwt_writer_close(w);
    pgwt_writer_destroy(w);
    free(w);
}

/* Empty / header-only / garbage current.trace handling. */
static void test_recover_degenerate_files(void)
{
    printf("--- DUR-1: degenerate current.trace forms ---\n");
    const char *dir = BASE_DIR "/degen";
    rm_rf(dir);
    mkdir(BASE_DIR, 0755);
    mkdir(dir, 0755);

    /* Garbage (wrong magic) file must be preserved as *.corrupt.<ts>. */
    FILE *f = fopen(path_of(dir, "current.trace"), "w");
    fprintf(f, "this is not a trace file, but it is somebody's data");
    fclose(f);

    struct pgwt_event_writer *w = calloc(1, sizeof(*w));
    CHECK(pgwt_writer_init(w, dir, 18, 24, NULL) == 0, "init over garbage");
    CHECK(!file_exists(dir, "current.trace"), "garbage moved aside");
    CHECK(count_suffix(dir, ".trace.lz4") == 0, "garbage NOT archived");
    int corrupt = 0;
    DIR *d = opendir(dir);
    struct dirent *e;
    while ((e = readdir(d)) != NULL)
        if (strstr(e->d_name, ".corrupt."))
            corrupt++;
    closedir(d);
    CHECK(corrupt == 1, "garbage preserved as .corrupt (got %d)", corrupt);
    pgwt_writer_destroy(w);
    free(w);
}

/* ── DUR-2: rotation must not clobber an existing archive ─── */

static void test_rotation_collision(void)
{
    printf("--- DUR-2: rotation onto an existing archive name ---\n");
    const char *dir = BASE_DIR "/rotcol";
    rm_rf(dir);

    struct pgwt_event_writer *w = calloc(1, sizeof(*w));
    CHECK(pgwt_writer_init(w, dir, 18, 24, NULL) == 0, "init");
    push_blocks(w, 1000000000000ULL, 1);

    /* Pre-create the exact archive name this rotation would use (what a
     * restart-mid-hour or DST fold produces). */
    time_t start_sec = (time_t)(w->file_start_wall_ns / 1000000000ULL);
    struct tm tm;
    localtime_r(&start_sec, &tm);
    char victim[600];
    snprintf(victim, sizeof(victim), "%s/%04d-%02d-%02d_%02d.trace.lz4",
             dir, tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday, tm.tm_hour);
    FILE *f = fopen(victim, "w");
    fprintf(f, "PRECIOUS");
    fclose(f);

    /* Force rotation (fake an hour change). */
    w->current_hour -= 1;
    CHECK(pgwt_writer_check_rotation(w) == 0, "rotation");
    CHECK(w->fp == NULL, "rotated (file closed)");

    /* The pre-existing archive is intact; the rotated file got a suffix. */
    f = fopen(victim, "r");
    char buf[32] = {0};
    if (f) { if (fread(buf, 1, 8, f) != 8) buf[0] = 0; fclose(f); }
    CHECK(strncmp(buf, "PRECIOUS", 8) == 0,
          "existing archive NOT clobbered");
    CHECK(count_suffix(dir, ".trace.lz4") == 2,
          "rotated file archived under a suffixed name");

    char suffixed[600];
    snprintf(suffixed, sizeof(suffixed), "%s/%04d-%02d-%02d_%02d.1.trace.lz4",
             dir, tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday, tm.tm_hour);
    int nb = 0;
    long events = count_events_in_file(suffixed, &nb);
    CHECK(events == PGWT_BLOCK_EVENTS,
          "suffixed archive readable (%ld events)", events);

    /* Discovery must see the suffixed archive. */
    struct pgwt_trace_file_entry entries[16];
    int nfiles = pgwt_scan_trace_files(dir, entries, 16);
    CHECK(nfiles >= 1, "scan_trace_files finds suffixed archives (%d)",
          nfiles);

    pgwt_writer_destroy(w);
    free(w);
}

/* ── DUR-8: SAMPLES batching ─────────────────────────────── */

static void push_sample_tick(struct pgwt_event_writer *w, uint64_t ts,
                             int nsamples, uint64_t period)
{
    static struct pgwt_trace_event batch[64];
    for (int i = 0; i < nsamples; i++) {
        batch[i] = (struct pgwt_trace_event){0};
        batch[i].timestamp_ns = ts;
        batch[i].pid = 2000 + i;
        batch[i].new_event = 0x0A000001;
        batch[i].query_id = 7;
    }
    pgwt_writer_push_samples(w, batch, nsamples, period);
}

static void test_sample_batching(void)
{
    printf("--- DUR-8: ~1 s SAMPLES batching ---\n");
    const char *dir = BASE_DIR "/batch";
    rm_rf(dir);

    struct pgwt_event_writer *w = calloc(1, sizeof(*w));
    CHECK(pgwt_writer_init(w, dir, 18, 24, NULL) == 0, "init");

    /* 25 ticks at 10 Hz (100 ms apart), 10 samples each = 2.4 s span.
     * Old behavior: 25 blocks. Batched: one block per ~1 s → 3 blocks. */
    uint64_t t0 = 1000000000000ULL;
    uint64_t period = 100000000ULL;   /* 100 ms */
    for (int tick = 0; tick < 25; tick++)
        push_sample_tick(w, t0 + tick * period, 10, period);

    /* Nothing on disk breaches per-tick blocks: committed block count must
     * be well under the tick count while the batch is pending. */
    int committed = read_meta_committed(dir, "current.trace.meta");
    CHECK(committed >= 1 && committed <= 3,
          "batched blocks committed, not per-tick (%d for 25 ticks)",
          committed);

    pgwt_writer_close(w);

    char cur[600];
    snprintf(cur, sizeof(cur), "%s/current.trace", dir);
    struct pgwt_event_reader r;
    CHECK(pgwt_reader_open(&r, cur) == 0, "reader open");
    CHECK(r.num_blocks >= 2 && r.num_blocks <= 4,
          "25 ticks → ~3 blocks (got %d)", r.num_blocks);

    long total = 0;
    static struct pgwt_trace_event buf[PGWT_BLOCK_EVENTS];
    for (int b = 0; b < r.num_blocks; b++) {
        struct pgwt_block_info bi;
        int n = pgwt_reader_decode_block_info(&r, b, buf, PGWT_BLOCK_EVENTS,
                                              &bi);
        CHECK(bi.block_type == PGWT_BLOCK_SAMPLES, "block %d is SAMPLES", b);
        CHECK(bi.sample_period_ns == period,
              "block %d period preserved (%llu)", b,
              (unsigned long long)bi.sample_period_ns);
        for (int i = 0; i < n; i++) {
            if (!(buf[i].flags & PGWT_EVENT_FLAG_SAMPLE))
                break;
            total++;
        }
    }
    CHECK(total == 250, "all 250 samples decoded (got %ld)", total);
    pgwt_reader_close(&r);
    pgwt_writer_destroy(w);
    free(w);
}

static void test_sample_period_jump_cuts_block(void)
{
    printf("--- DUR-8: SMP-3 stall tick gets its own block ---\n");
    const char *dir = BASE_DIR "/stall";
    rm_rf(dir);

    struct pgwt_event_writer *w = calloc(1, sizeof(*w));
    CHECK(pgwt_writer_init(w, dir, 18, 24, NULL) == 0, "init");

    uint64_t t0 = 1000000000000ULL;
    uint64_t nominal = 100000000ULL;   /* 100 ms */
    /* 3 normal ticks, then a stalled tick weighted 500 ms (SMP-3), then 2
     * normal ticks. The stall tick must not share a block. */
    push_sample_tick(w, t0 + 0 * nominal, 5, nominal);
    push_sample_tick(w, t0 + 1 * nominal, 5, nominal);
    push_sample_tick(w, t0 + 2 * nominal, 5, nominal);
    push_sample_tick(w, t0 + 8 * nominal, 5, 500000000ULL);
    push_sample_tick(w, t0 + 9 * nominal, 5, nominal);
    push_sample_tick(w, t0 + 10 * nominal, 5, nominal);
    pgwt_writer_close(w);

    char cur[600];
    snprintf(cur, sizeof(cur), "%s/current.trace", dir);
    struct pgwt_event_reader r;
    CHECK(pgwt_reader_open(&r, cur) == 0, "reader open");
    CHECK(r.num_blocks == 3, "period jumps cut blocks (got %d)",
          r.num_blocks);

    /* Verify per-block periods: 100 ms, 500 ms, 100 ms — the total
     * estimated time (Σ count × period) must equal the exact per-tick sum. */
    uint64_t total_weight = 0;
    static struct pgwt_trace_event buf[PGWT_BLOCK_EVENTS];
    for (int b = 0; b < r.num_blocks; b++) {
        struct pgwt_block_info bi;
        int n = pgwt_reader_decode_block_info(&r, b, buf, PGWT_BLOCK_EVENTS,
                                              &bi);
        total_weight += (uint64_t)n * bi.sample_period_ns;
    }
    uint64_t expected = 5 * (5 * nominal + 500000000ULL);
    CHECK(total_weight == expected,
          "SMP-3 weights preserved exactly (%llu vs %llu)",
          (unsigned long long)total_weight, (unsigned long long)expected);
    pgwt_reader_close(&r);
    pgwt_writer_destroy(w);
    free(w);
}

static void test_interleaved_index_sorted(void)
{
    printf("--- DUR-8: interleaved transitions+samples keep index sorted ---\n");
    const char *dir = BASE_DIR "/interleave";
    rm_rf(dir);

    struct pgwt_event_writer *w = calloc(1, sizeof(*w));
    CHECK(pgwt_writer_init(w, dir, 18, 24, NULL) == 0, "init");

    uint64_t ts = 1000000000000ULL;
    uint64_t period = 100000000ULL;
    /* Interleave: transitions trickle in while sample ticks arrive; the
     * transitions block fills much later than the first pending samples. */
    for (int round = 0; round < 30; round++) {
        for (int i = 0; i < 700; i++) {   /* 700 transitions per round */
            struct pgwt_trace_event ev = mk_event(ts, 1000, 0x0A000001, 100, 1);
            pgwt_writer_push_event(w, &ev);
            ts += 100000;   /* 0.1 ms apart → 70 ms per round */
        }
        push_sample_tick(w, ts, 8, period);
        ts += period / 2;
    }
    pgwt_writer_close(w);

    char cur[600];
    snprintf(cur, sizeof(cur), "%s/current.trace", dir);
    struct pgwt_event_reader r;
    CHECK(pgwt_reader_open(&r, cur) == 0, "reader open");
    CHECK(r.num_blocks >= 4, "several blocks written (%d)", r.num_blocks);
    int sorted = 1;
    for (int b = 1; b < r.num_blocks; b++)
        if (r.block_index[b].timestamp_ns < r.block_index[b - 1].timestamp_ns)
            sorted = 0;
    CHECK(sorted, "block index sorted by first timestamp");
    pgwt_reader_close(&r);
    pgwt_writer_destroy(w);
    free(w);
}

/* ── DUR-7: corrupt meta / block headers must not break the reader ── */

static void test_meta_sanity_caps(void)
{
    printf("--- DUR-7: corrupt meta and block headers ---\n");
    const char *dir = BASE_DIR "/meta";
    rm_rf(dir);

    struct pgwt_event_writer *w = calloc(1, sizeof(*w));
    CHECK(pgwt_writer_init(w, dir, 18, 24, NULL) == 0, "init");
    push_blocks(w, 1000000000000ULL, 1);
    fflush(w->fp);

    char cur[600];
    snprintf(cur, sizeof(cur), "%s/current.trace", dir);

    /* Absurd committed count: reader must cap, fall back, and still read
     * the one real block (never a multi-GB allocation). */
    FILE *f = fopen(path_of(dir, "current.trace.meta"), "w");
    fprintf(f, "999999999\n");
    fclose(f);
    int nb = 0;
    long events = count_events_in_file(cur, &nb);
    CHECK(nb == 1, "absurd meta count rejected, scan fallback (%d)", nb);
    CHECK(events == PGWT_BLOCK_EVENTS, "data still readable (%ld)", events);

    /* Meta claims 3 committed but only 1 block exists: reader stops at
     * what's really there. */
    f = fopen(path_of(dir, "current.trace.meta"), "w");
    fprintf(f, "3\n");
    fclose(f);
    events = count_events_in_file(cur, &nb);
    CHECK(nb == 1, "meta over-claim clamped to real blocks (%d)", nb);

    /* Garbage block header after the real block, meta claiming 2: the
     * meta-path scan must stop at the invalid header (same caps as the
     * fallback scan). */
    f = fopen(cur, "ab");
    struct pgwt_trace_block_header bad = {
        .first_timestamp_ns = 1,
        .last_timestamp_ns = 2,
        .num_events = 4000000000u,          /* > PGWT_BLOCK_EVENTS */
        .compressed_size = 4000000000u,     /* > 10 MB cap */
        .uncompressed_size = 4000000000u,
        .block_type = 0,
    };
    fwrite(&bad, sizeof(bad), 1, f);
    fclose(f);
    f = fopen(path_of(dir, "current.trace.meta"), "w");
    fprintf(f, "2\n");
    fclose(f);
    events = count_events_in_file(cur, &nb);
    CHECK(nb == 1, "garbage block header stops the meta scan (%d)", nb);
    CHECK(events == PGWT_BLOCK_EVENTS, "real data unaffected (%ld)", events);

    /* Recovery over the same garbage must also survive and archive 1 block. */
    fclose(w->fp);
    w->fp = NULL;
    pgwt_writer_destroy(w);
    free(w);
    struct pgwt_event_writer *w2 = calloc(1, sizeof(*w2));
    CHECK(pgwt_writer_init(w2, dir, 18, 24, NULL) == 0, "recovery init");
    char archive[600];
    CHECK(find_suffix(dir, ".trace.lz4", archive, sizeof(archive)) == 0,
          "recovered archive exists");
    events = count_events_in_file(archive, &nb);
    CHECK(nb == 1 && events == PGWT_BLOCK_EVENTS,
          "recovered archive intact (%d blocks, %ld events)", nb, events);
    pgwt_writer_destroy(w2);
    free(w2);
}

/* ── DUR-3: size-based retention + orphan cleanup ─────────── */

static void write_fake_archive(const char *dir, const char *name, int kb)
{
    FILE *f = fopen(path_of(dir, name), "w");
    char buf[1024];
    memset(buf, 'x', sizeof(buf));
    for (int i = 0; i < kb; i++)
        fwrite(buf, 1, sizeof(buf), f);
    fclose(f);
}

static void test_size_cap_retention(void)
{
    printf("--- DUR-3: --retention-gb size cap ---\n");
    const char *dir = BASE_DIR "/sizecap";
    rm_rf(dir);
    mkdir(BASE_DIR, 0755);
    mkdir(dir, 0755);

    write_fake_archive(dir, "2025-01-01_10.trace.lz4", 100);
    write_fake_archive(dir, "2025-01-01_11.trace.lz4", 100);
    write_fake_archive(dir, "2025-01-01_11.1.trace.lz4", 100); /* recovered */
    write_fake_archive(dir, "2025-01-01_12.trace.lz4", 100);
    write_fake_archive(dir, "2025-01-01_10.summary.lz4", 50);
    write_fake_archive(dir, "current.trace", 10);
    write_fake_archive(dir, "query_texts.jsonl", 5);

    struct pgwt_event_writer *w = calloc(1, sizeof(*w));
    snprintf(w->trace_dir, sizeof(w->trace_dir), "%s", dir);
    w->retention_hours = 0;              /* hours-based retention off */
    w->retention_bytes = 280 * 1024;     /* forces deleting oldest ~2 files */

    CHECK(pgwt_writer_cleanup_old_files(w) == 0, "cleanup runs");

    CHECK(file_exists(dir, "current.trace"), "live current.trace kept");
    CHECK(file_exists(dir, "query_texts.jsonl"), "sidecar kept");
    CHECK(!file_exists(dir, "2025-01-01_10.trace.lz4"),
          "oldest archive deleted first");
    CHECK(!file_exists(dir, "2025-01-01_10.summary.lz4"),
          "oldest summary deleted too");
    CHECK(file_exists(dir, "2025-01-01_12.trace.lz4"),
          "newest archive kept");

    /* Total remaining must fit the cap. */
    long total = 0;
    DIR *d = opendir(dir);
    struct dirent *e;
    while ((e = readdir(d)) != NULL) {
        long s = file_size(dir, e->d_name);
        if (s > 0)
            total += s;
    }
    closedir(d);
    CHECK(total <= 280 * 1024, "disk usage under the cap (%ld)", total);

    /* Orphan meta cleanup: current.trace.meta with NO current.trace, aged. */
    rm_rf(dir);
    mkdir(dir, 0755);
    write_fake_archive(dir, "current.trace.meta", 1);
    write_fake_archive(dir, "current.summary.meta.tmp", 1);
    struct timeval old_tv[2];
    gettimeofday(&old_tv[0], NULL);
    old_tv[0].tv_sec -= 7200;
    old_tv[1] = old_tv[0];
    utimes(path_of(dir, "current.trace.meta"), old_tv);
    utimes(path_of(dir, "current.summary.meta.tmp"), old_tv);
    w->retention_bytes = 0;
    CHECK(pgwt_writer_cleanup_old_files(w) == 0, "orphan cleanup runs");
    CHECK(!file_exists(dir, "current.trace.meta"), "orphan meta removed");
    CHECK(!file_exists(dir, "current.summary.meta.tmp"),
          "orphan meta tmp removed");

    /* A meta belonging to a LIVE current.trace must be kept even when old. */
    write_fake_archive(dir, "current.trace", 1);
    write_fake_archive(dir, "current.trace.meta", 1);
    utimes(path_of(dir, "current.trace.meta"), old_tv);
    CHECK(pgwt_writer_cleanup_old_files(w) == 0, "cleanup runs");
    CHECK(file_exists(dir, "current.trace.meta"),
          "live file's meta NOT treated as orphan");

    free(w);
}

/* ── DUR-1: summary writer recovery ───────────────────────── */

static void test_summary_recovery(void)
{
    printf("--- DUR-1: current.summary recovery ---\n");
    const char *dir = BASE_DIR "/summary";
    rm_rf(dir);
    mkdir(BASE_DIR, 0755);
    mkdir(dir, 0755);

    struct pgwt_summary_writer *w = calloc(1, sizeof(*w));
    CHECK(pgwt_summary_writer_init(w, dir, 24, NULL) == 0, "init");
    /* Two seconds of events → 2 blocks (flush on second boundary + explicit) */
    for (int s = 0; s < 2; s++) {
        for (int i = 0; i < 50; i++) {
            struct pgwt_trace_event ev =
                mk_event(1000000000000ULL + (uint64_t)s * 1000000000ULL
                         + i * 1000000, 1000 + i % 3, 0x0A000001, 500000, 9);
            pgwt_summary_push_event(w, &ev);
        }
        pgwt_summary_flush(w);
    }
    CHECK(read_meta_committed(dir, "current.summary.meta") == 2,
          "2 summary blocks committed");
    /* Crash: no footer. */
    fclose(w->fp);
    w->fp = NULL;
    pgwt_summary_destroy(w);
    free(w);

    struct pgwt_summary_writer *w2 = calloc(1, sizeof(*w2));
    CHECK(pgwt_summary_writer_init(w2, dir, 24, NULL) == 0, "restart init");
    CHECK(!file_exists(dir, "current.summary"), "current.summary archived");
    CHECK(count_suffix(dir, ".summary.lz4") == 1, "summary archive present");

    char archive[600];
    CHECK(find_suffix(dir, ".summary.lz4", archive, sizeof(archive)) == 0,
          "archive found");
    struct pgwt_summary_reader r;
    CHECK(pgwt_summary_reader_open(&r, archive) == 0, "summary reader open");
    CHECK(r.num_blocks == 2, "2 blocks recovered (got %d)", r.num_blocks);
    struct pgwt_summary_accum *acc = malloc(sizeof(*acc));
    CHECK(pgwt_summary_reader_decode_block(&r, 0, acc) == 0, "block decodes");
    CHECK(acc->num_events > 0, "decoded block has events");
    free(acc);
    pgwt_summary_reader_close(&r);
    pgwt_summary_destroy(w2);
    free(w2);
}

/* ── #128: the summary writer's per-pid attribution table ───── */

static int qattr_occupied(const struct pgwt_summary_writer *w)
{
    int n = 0;
    for (int i = 0; i < SUMMARY_QATTR_SLOTS; i++)
        if (w->qattr[i].pid != 0)
            n++;
    return n;
}

/* A foreground non-idle record with no id: deferred on the pid (allocates
 * its slot). `exit` marks it as the backend's closing record. */
static void qattr_push(struct pgwt_summary_writer *w, uint64_t ts,
                       uint32_t pid, int exit)
{
    struct pgwt_trace_event ev =
        mk_event(ts, pid, WEI(PG_WAIT_LOCK, 0), 1000000, 0);
    if (exit)
        ev.new_event = PGWT_EVENT_EXIT;
    pgwt_summary_push_event(w, &ev);
}

static void test_summary_qattr_reclaim(void)
{
    printf("--- #128: per-pid attribution slots are reclaimed on exit, "
           "bounded when the table is full ---\n");
    const char *dir = BASE_DIR "/qattr";
    rm_rf(dir);
    mkdir(BASE_DIR, 0755);
    mkdir(dir, 0755);

    struct pgwt_summary_writer *w = calloc(1, sizeof(*w));
    CHECK(pgwt_summary_writer_init(w, dir, 24, NULL) == 0, "init");
    CHECK(w->qattr != NULL, "attribution table allocated");
    uint64_t ts = 2000000000000ULL;

    /* Normal path: a pid takes a slot on its first deferred record and
     * gives it back on its exit record; the unresolved time is counted. */
    qattr_push(w, ts += 1000000, 5000, 0);
    CHECK(qattr_occupied(w) == 1, "first deferred record allocates the slot");
    qattr_push(w, ts += 1000000, 5000, 1);
    CHECK(qattr_occupied(w) == 0, "exit record frees the slot (got %d)",
          qattr_occupied(w));
    CHECK(w->qattr_unattributed_ns_total == 2000000,
          "the exiting pid's unresolved 2 ms is counted (%llu ns)",
          (unsigned long long)w->qattr_unattributed_ns_total);
    CHECK(w->qattr_table_full_total == 0, "table never full so far");

    /* Fill every slot with live pids, then one more: counted, not silent. */
    for (uint32_t p = 1; p <= SUMMARY_QATTR_SLOTS; p++)
        qattr_push(w, ts += 1000000, 10000 + p, 0);
    CHECK(qattr_occupied(w) == SUMMARY_QATTR_SLOTS,
          "%d distinct pids fill the table (got %d)", SUMMARY_QATTR_SLOTS,
          qattr_occupied(w));
    qattr_push(w, ts += 1000000, 99999, 0);
    CHECK(w->qattr_table_full_total == 1 &&
          qattr_occupied(w) == SUMMARY_QATTR_SLOTS,
          "the %dth pid is refused and counted (full=%llu)",
          SUMMARY_QATTR_SLOTS + 1,
          (unsigned long long)w->qattr_table_full_total);

    /* The reviewer's hang: an exit on a FULL table has no empty slot to stop
     * the backward shift. Must return, free exactly one slot, duplicate
     * nothing. */
    qattr_push(w, ts += 1000000, 10000 + 1, 1);
    CHECK(qattr_occupied(w) == SUMMARY_QATTR_SLOTS - 1,
          "exit on a full table frees exactly one slot (got %d)",
          qattr_occupied(w));
    int dup = 0;
    for (int i = 0; i < SUMMARY_QATTR_SLOTS; i++)
        for (int j = i + 1; j < SUMMARY_QATTR_SLOTS; j++)
            if (w->qattr[i].pid && w->qattr[i].pid == w->qattr[j].pid)
                dup++;
    CHECK(dup == 0, "no pid is duplicated by the shift (%d duplicates)", dup);

    /* Every remaining pid must still be found by its probe chain: each
     * exit reclaims ITS entry (a broken chain would allocate a fresh slot
     * for the exit record and leave the old entry behind). */
    uint64_t full_before = w->qattr_table_full_total;
    for (uint32_t p = 2; p <= SUMMARY_QATTR_SLOTS; p++)
        qattr_push(w, ts += 1000000, 10000 + p, 1);
    CHECK(qattr_occupied(w) == 0,
          "every other pid's exit found and freed its own slot (%d left)",
          qattr_occupied(w));
    CHECK(w->qattr_table_full_total == full_before,
          "…without the table ever reading full again");
    /* And the freed table is usable: the refused pid gets a slot now. */
    qattr_push(w, ts += 1000000, 99999, 0);
    CHECK(qattr_occupied(w) == 1 && w->qattr_table_full_total == full_before,
          "a new pid takes a reclaimed slot");

    pgwt_summary_destroy(w);
    free(w);
    rm_rf(dir);
}

/* ── main ─────────────────────────────────────────────────── */

int main(void)
{
    printf("=== test_durability (T5: DUR-1/2/3/7/8) ===\n");
    mkdir(BASE_DIR, 0755);

    test_recover_after_sigkill();
    test_recover_torn_tail();
    test_restart_twice_same_hour();
    test_recover_degenerate_files();
    test_rotation_collision();
    test_sample_batching();
    test_sample_period_jump_cuts_block();
    test_interleaved_index_sorted();
    test_meta_sanity_caps();
    test_size_cap_retention();
    test_summary_recovery();
    test_summary_qattr_reclaim();

    rm_rf(BASE_DIR);
    printf("\n%d/%d tests passed\n", tests_passed, tests_run);
    return tests_passed == tests_run ? 0 : 1;
}
