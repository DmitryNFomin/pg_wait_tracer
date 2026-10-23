/* test_event_reader.c — Unit tests for event reader: roundtrip, time range, time parser */
#include "event_writer.h"
#include "event_reader.h"
#include "map_reader.h"
#include "wait_event.h"

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>
#include <sys/stat.h>
#include <time.h>

static int tests_run = 0;
static int tests_passed = 0;

#define CHECK(cond, fmt, ...) do { \
    tests_run++; \
    if (cond) { tests_passed++; } \
    else { printf("  FAIL: " fmt "\n", ##__VA_ARGS__); } \
} while(0)

/* ── Helper: create a trace file with known events ────────── */

static const char *TEST_DIR = "/tmp/test_event_reader";

static void setup_test_dir(void)
{
    mkdir(TEST_DIR, 0755);
}

static void cleanup_test_dir(void)
{
    /* Remove files in test dir */
    char cmd[512];
    snprintf(cmd, sizeof(cmd), "rm -rf %s", TEST_DIR);
    system(cmd);
}

static int write_test_file(const struct pgwt_trace_event *events,
                           int count, int pg_version)
{
    struct pgwt_event_writer w;
    if (pgwt_writer_init(&w, TEST_DIR, pg_version, 24, NULL) != 0)
        return -1;

    /* Rename current.trace to our test file name */
    for (int i = 0; i < count; i++) {
        if (pgwt_writer_push_event(&w, &events[i]) != 0) {
            pgwt_writer_close(&w);
            pgwt_writer_destroy(&w);
            return -1;
        }
    }
    pgwt_writer_close(&w);
    pgwt_writer_destroy(&w);

    /* Writer creates current.trace in TEST_DIR */
    return 0;
}

/* ── Test 1: Single-block roundtrip ──────────────────────────── */

static void test_roundtrip_single_block(void)
{
    printf("--- Roundtrip (single block) ---\n");
    cleanup_test_dir();
    setup_test_dir();

    /* Create 100 known events */
    int N = 100;
    struct pgwt_trace_event *orig = calloc(N, sizeof(*orig));
    for (int i = 0; i < N; i++) {
        orig[i].timestamp_ns = 1000000ULL + i * 1000ULL;  /* 1ms, +1us each */
        orig[i].pid = 1000 + (i % 10);
        orig[i].old_event = 0x03000000 + i;  /* IO class */
        orig[i].new_event = 0x03000000 + i + 1;
        orig[i].duration_ns = 500 + i * 10;
        orig[i].query_id = (i % 3 == 0) ? 0 : (42 + i);
    }

    CHECK(write_test_file(orig, N, 18) == 0, "write test file");

    /* Read back */
    char path[512];
    snprintf(path, sizeof(path), "%s/current.trace", TEST_DIR);

    struct pgwt_event_reader reader;
    CHECK(pgwt_reader_open(&reader, path) == 0, "reader_open");
    CHECK(reader.header.magic == PGWT_TRACE_MAGIC, "magic");
    CHECK(reader.header.version == PGWT_TRACE_VERSION, "version");
    CHECK(reader.header.pg_version == 18, "pg_version");
    CHECK(reader.num_blocks == 1, "num_blocks=%d (expected 1)", reader.num_blocks);

    struct pgwt_trace_event *decoded = calloc(PGWT_BLOCK_EVENTS, sizeof(*decoded));
    int count = pgwt_reader_decode_block(&reader, 0, decoded, PGWT_BLOCK_EVENTS);
    CHECK(count == N, "decoded count=%d (expected %d)", count, N);

    int mismatches = 0;
    for (int i = 0; i < count && i < N; i++) {
        if (decoded[i].timestamp_ns != orig[i].timestamp_ns ||
            decoded[i].pid != orig[i].pid ||
            decoded[i].old_event != orig[i].old_event ||
            decoded[i].new_event != orig[i].new_event ||
            decoded[i].duration_ns != orig[i].duration_ns ||
            decoded[i].query_id != orig[i].query_id) {
            if (mismatches == 0) {
                printf("  First mismatch at i=%d:\n", i);
                printf("    ts: %lu vs %lu\n",
                       (unsigned long)decoded[i].timestamp_ns,
                       (unsigned long)orig[i].timestamp_ns);
                printf("    pid: %u vs %u\n", decoded[i].pid, orig[i].pid);
                printf("    old: 0x%x vs 0x%x\n", decoded[i].old_event, orig[i].old_event);
                printf("    new: 0x%x vs 0x%x\n", decoded[i].new_event, orig[i].new_event);
                printf("    dur: %lu vs %lu\n",
                       (unsigned long)decoded[i].duration_ns,
                       (unsigned long)orig[i].duration_ns);
                printf("    qid: %lu vs %lu\n",
                       (unsigned long)decoded[i].query_id,
                       (unsigned long)orig[i].query_id);
            }
            mismatches++;
        }
    }
    CHECK(mismatches == 0, "field mismatches: %d", mismatches);

    pgwt_reader_close(&reader);
    free(decoded);
    free(orig);
    cleanup_test_dir();
}

/* ── Test 2: Multi-block roundtrip ──────────────────────────── */

static void test_roundtrip_multi_block(void)
{
    printf("--- Roundtrip (multi-block) ---\n");
    cleanup_test_dir();
    setup_test_dir();

    /* Write more events than one block (4096+100 = 2 blocks) */
    int N = PGWT_BLOCK_EVENTS + 100;
    struct pgwt_trace_event *orig = calloc(N, sizeof(*orig));
    for (int i = 0; i < N; i++) {
        orig[i].timestamp_ns = 1000000ULL + i * 500ULL;
        orig[i].pid = 2000 + (i % 20);
        orig[i].old_event = 0x01000000 + (i % 50);  /* LWLock class */
        orig[i].new_event = 0;
        orig[i].duration_ns = 100 + (i % 1000) * 100;
        orig[i].query_id = (i % 5 == 0) ? 12345ULL : 0;
    }

    CHECK(write_test_file(orig, N, 17) == 0, "write multi-block file");

    char path[512];
    snprintf(path, sizeof(path), "%s/current.trace", TEST_DIR);

    struct pgwt_event_reader reader;
    CHECK(pgwt_reader_open(&reader, path) == 0, "reader_open");
    CHECK(reader.num_blocks == 2, "num_blocks=%d (expected 2)", reader.num_blocks);
    CHECK(reader.header.pg_version == 17, "pg_version=%d", reader.header.pg_version);

    struct pgwt_trace_event *decoded = calloc(PGWT_BLOCK_EVENTS, sizeof(*decoded));
    int total = 0;
    int mismatches = 0;

    for (int bi = 0; bi < reader.num_blocks; bi++) {
        int count = pgwt_reader_decode_block(&reader, bi, decoded, PGWT_BLOCK_EVENTS);
        CHECK(count > 0, "block %d: count=%d", bi, count);

        for (int i = 0; i < count && total + i < N; i++) {
            int idx = total + i;
            if (decoded[i].timestamp_ns != orig[idx].timestamp_ns ||
                decoded[i].pid != orig[idx].pid ||
                decoded[i].old_event != orig[idx].old_event ||
                decoded[i].duration_ns != orig[idx].duration_ns ||
                decoded[i].query_id != orig[idx].query_id) {
                mismatches++;
            }
        }
        total += count;
    }

    CHECK(total == N, "total events=%d (expected %d)", total, N);
    CHECK(mismatches == 0, "multi-block mismatches: %d", mismatches);

    pgwt_reader_close(&reader);
    free(decoded);
    free(orig);
    cleanup_test_dir();
}

/* ── Test 3: Block search (binary search) ────────────────── */

static void test_block_search(void)
{
    printf("--- Block search ---\n");
    cleanup_test_dir();
    setup_test_dir();

    /* Create 3 blocks worth of events with distinct timestamp ranges */
    int N = PGWT_BLOCK_EVENTS * 3;
    struct pgwt_trace_event *orig = calloc(N, sizeof(*orig));
    for (int i = 0; i < N; i++) {
        orig[i].timestamp_ns = 10000000ULL + i * 1000ULL;  /* 10ms base, 1us each */
        orig[i].pid = 3000;
        orig[i].old_event = 0x0A000001;
        orig[i].new_event = 0;
        orig[i].duration_ns = 500;
        orig[i].query_id = 0;
    }

    CHECK(write_test_file(orig, N, 18) == 0, "write 3-block file");

    char path[512];
    snprintf(path, sizeof(path), "%s/current.trace", TEST_DIR);

    struct pgwt_event_reader reader;
    CHECK(pgwt_reader_open(&reader, path) == 0, "reader_open");
    CHECK(reader.num_blocks == 3, "num_blocks=%d (expected 3)", reader.num_blocks);

    /* Block 0: ts 10000000 .. 10000000+4095*1000 = 10000000..14095000 */
    /* Block 1: ts 14096000 .. 18191000 */
    /* Block 2: ts 18192000 .. 22287000 */

    /* Search for timestamp at start → should find block 0 */
    int bi = pgwt_reader_find_block(&reader, 10000000ULL);
    CHECK(bi == 0, "find_block(start)=%d (expected 0)", bi);

    /* Search for timestamp in middle of block 1 */
    bi = pgwt_reader_find_block(&reader, 16000000ULL);
    CHECK(bi == 1, "find_block(mid_block1)=%d (expected 1)", bi);

    /* Search for timestamp in block 2 */
    bi = pgwt_reader_find_block(&reader, 20000000ULL);
    CHECK(bi == 2, "find_block(block2)=%d (expected 2)", bi);

    /* Search before any data → should return 0 */
    bi = pgwt_reader_find_block(&reader, 1000ULL);
    CHECK(bi == 0, "find_block(before)=%d (expected 0)", bi);

    /* Search after all data → should return last block */
    bi = pgwt_reader_find_block(&reader, 99999999ULL);
    CHECK(bi == 2, "find_block(after)=%d (expected 2)", bi);

    pgwt_reader_close(&reader);
    free(orig);
    cleanup_test_dir();
}

/* ── Test 4: Time range replay ────────────────────────────── */

static void test_time_range_replay(void)
{
    printf("--- Time range replay ---\n");
    cleanup_test_dir();
    setup_test_dir();

    /* Create events across a range */
    int N = 200;
    struct pgwt_trace_event *orig = calloc(N, sizeof(*orig));
    for (int i = 0; i < N; i++) {
        orig[i].timestamp_ns = 1000000ULL + i * 10000ULL;  /* 10us apart */
        orig[i].pid = 4000;
        orig[i].old_event = 0x0A000001;  /* IO class (0x0A) */
        orig[i].new_event = 0;
        orig[i].duration_ns = 1000;  /* 1us each */
        orig[i].query_id = 0;
    }

    CHECK(write_test_file(orig, N, 18) == 0, "write file for replay");

    pgwt_init_event_names(18);

    char path[512];
    snprintf(path, sizeof(path), "%s/current.trace", TEST_DIR);

    struct pgwt_event_reader reader;
    CHECK(pgwt_reader_open(&reader, path) == 0, "reader_open");

    struct pgwt_trace_event *decoded = calloc(PGWT_BLOCK_EVENTS, sizeof(*decoded));
    struct pgwt_block_info binfo;
    int count = pgwt_reader_decode_block_info(&reader, 0, decoded,
                                              PGWT_BLOCK_EVENTS, &binfo);
    CHECK(count == N, "decoded count=%d", count);

    /* Replay with time bounds: skip first 50, take next 100 */
    uint64_t from_mono = 1000000ULL + 50 * 10000ULL;   /* event 50 */
    uint64_t to_mono   = 1000000ULL + 150 * 10000ULL;  /* event 150 */

    struct pgwt_accumulator *acc = calloc(1, sizeof(*acc));
    pgwt_accum_init(acc);
    pgwt_replay_events(acc, decoded, count, from_mono, to_mono, &binfo, NULL);

    /* Should have accumulated ~101 events (50..150 inclusive) */
    CHECK(acc->num_system_events > 0, "system events accumulated");
    CHECK(acc->num_pids == 1, "num_pids=%d (expected 1)", acc->num_pids);
    CHECK(acc->pids[0].pid == 4000, "pid=%d", acc->pids[0].pid);

    /* Find the event stats for our IO event */
    struct pgwt_event_stats *se = pgwt_find_system_event(acc, 0x0A000001);
    CHECK(se != NULL, "system event found");
    if (se) {
        CHECK(se->count == 101, "count=%lu (expected 101)", (unsigned long)se->count);
    }

    /* Replay all events (no bounds) */
    pgwt_accum_init(acc);
    pgwt_replay_events(acc, decoded, count, 0, 0, &binfo, NULL);
    se = pgwt_find_system_event(acc, 0x0A000001);
    CHECK(se != NULL, "system event found (full)");
    if (se) {
        CHECK(se->count == (uint64_t)N, "full count=%lu (expected %d)",
              (unsigned long)se->count, N);
    }

    pgwt_reader_close(&reader);
    free(acc);
    free(decoded);
    free(orig);
    cleanup_test_dir();
}

/* ── Test 5: Time parser ─────────────────────────────────── */

static void test_time_parser(void)
{
    printf("--- Time parser ---\n");
    uint64_t ns;

    /* "now" should return something close to current time */
    CHECK(pgwt_parse_time("now", &ns) == 0, "parse 'now'");
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    uint64_t now_ns = (uint64_t)ts.tv_sec * 1000000000ULL + ts.tv_nsec;
    CHECK(ns > now_ns - 1000000000ULL && ns <= now_ns + 1000000ULL,
          "'now' within 1s of current time");

    /* ISO 8601 with T separator */
    CHECK(pgwt_parse_time("2025-01-15T12:30:45", &ns) == 0, "parse ISO T");
    CHECK(ns > 0, "ISO T returned non-zero");

    /* ISO 8601 with space separator */
    CHECK(pgwt_parse_time("2025-01-15 12:30:45", &ns) == 0, "parse ISO space");
    CHECK(ns > 0, "ISO space returned non-zero");

    /* Both ISO forms should give same result */
    uint64_t ns_t, ns_s;
    pgwt_parse_time("2025-06-01T00:00:00", &ns_t);
    pgwt_parse_time("2025-06-01 00:00:00", &ns_s);
    CHECK(ns_t == ns_s, "ISO T and space equal");

    /* Relative: 1h */
    uint64_t before;
    CHECK(pgwt_parse_time("1h", &before) == 0, "parse '1h'");
    CHECK(before < now_ns, "'1h' is before now");
    CHECK(now_ns - before > 3599ULL * 1000000000ULL &&
          now_ns - before < 3601ULL * 1000000000ULL,
          "'1h' ~3600s ago");

    /* Relative: 30m */
    CHECK(pgwt_parse_time("30m", &before) == 0, "parse '30m'");
    CHECK(now_ns - before > 1799ULL * 1000000000ULL &&
          now_ns - before < 1801ULL * 1000000000ULL,
          "'30m' ~1800s ago");

    /* Relative: 90s */
    CHECK(pgwt_parse_time("90s", &before) == 0, "parse '90s'");
    CHECK(now_ns - before > 89ULL * 1000000000ULL &&
          now_ns - before < 91ULL * 1000000000ULL,
          "'90s' ~90s ago");

    /* Relative compound: 1h30m */
    CHECK(pgwt_parse_time("1h30m", &before) == 0, "parse '1h30m'");
    CHECK(now_ns - before > 5399ULL * 1000000000ULL &&
          now_ns - before < 5401ULL * 1000000000ULL,
          "'1h30m' ~5400s ago");

    /* Invalid inputs */
    CHECK(pgwt_parse_time("", &ns) == -1, "empty string fails");
    CHECK(pgwt_parse_time("garbage", &ns) == -1, "garbage fails");
    CHECK(pgwt_parse_time(NULL, &ns) == -1, "NULL fails");
}

/* ── Test 6: Clock conversion ─────────────────────────────── */

static void test_clock_conversion(void)
{
    printf("--- Clock conversion ---\n");
    cleanup_test_dir();
    setup_test_dir();

    int N = 10;
    struct pgwt_trace_event events[10];
    for (int i = 0; i < N; i++) {
        events[i].timestamp_ns = 5000000ULL + i * 1000ULL;
        events[i].pid = 5000;
        events[i].old_event = 0;
        events[i].new_event = 0;
        events[i].duration_ns = 100;
        events[i].query_id = 0;
    }

    CHECK(write_test_file(events, N, 18) == 0, "write file");

    char path[512];
    snprintf(path, sizeof(path), "%s/current.trace", TEST_DIR);

    struct pgwt_event_reader reader;
    CHECK(pgwt_reader_open(&reader, path) == 0, "reader_open");

    /* mono_to_wall = start_time_ns - clock_offset_ns */
    /* wall_to_mono and mono_to_wall should be inverses */
    uint64_t wall = 1700000000ULL * 1000000000ULL;  /* ~2023 */
    uint64_t mono = pgwt_reader_wall_to_mono(&reader, wall);
    uint64_t back = pgwt_reader_mono_to_wall(&reader, mono);
    CHECK(back == wall, "wall→mono→wall roundtrip: %lu vs %lu",
          (unsigned long)back, (unsigned long)wall);

    uint64_t mono2 = 12345678ULL;
    uint64_t wall2 = pgwt_reader_mono_to_wall(&reader, mono2);
    uint64_t mono2b = pgwt_reader_wall_to_mono(&reader, wall2);
    CHECK(mono2b == mono2, "mono→wall→mono roundtrip: %lu vs %lu",
          (unsigned long)mono2b, (unsigned long)mono2);

    pgwt_reader_close(&reader);
    cleanup_test_dir();
}

/* ── Test 7: Scan trace files ─────────────────────────────── */

static void test_scan_trace_files(void)
{
    printf("--- Scan trace files ---\n");
    cleanup_test_dir();
    setup_test_dir();

    /* Create some fake trace files with proper format */
    /* First write a real current.trace */
    struct pgwt_trace_event evt = {
        .timestamp_ns = 1000000, .pid = 6000,
        .old_event = 0, .new_event = 0,
        .duration_ns = 100, .query_id = 0,
    };
    CHECK(write_test_file(&evt, 1, 18) == 0, "write current.trace");

    /* Create fake hourly files with a real header each. #405: the scanner
     * now takes placement from the HEADER, not the name, so these headers
     * are given start_time_ns values that deliberately do NOT match their
     * "_10"/"_11"/"_12" names — if the scanner silently fell back to the
     * name for any of them, the assertions below would catch it. */
    const char *fake_files[] = {
        "2025-01-15_10.trace.lz4",
        "2025-01-15_11.trace.lz4",
        "2025-01-15_12.trace.lz4",
    };
    const uint64_t fake_start_ns[] = {
        5000000000ULL, 6000000000ULL, 7000000000ULL,
    };
    for (int i = 0; i < 3; i++) {
        char p[512];
        snprintf(p, sizeof(p), "%s/%s", TEST_DIR, fake_files[i]);
        FILE *fp = fopen(p, "wb");
        if (fp) {
            struct pgwt_trace_file_header hdr = {
                .magic = PGWT_TRACE_MAGIC,
                .version = PGWT_TRACE_VERSION,
                .pg_version = 18,
                .start_time_ns = fake_start_ns[i],
            };
            fwrite(&hdr, sizeof(hdr), 1, fp);
            fclose(fp);
        }
    }

    struct pgwt_trace_file_entry entries[16];
    int n = pgwt_scan_trace_files(TEST_DIR, entries, 16);

    /* current.trace + 3 hourly files, every matching name always listed. */
    CHECK(n == 4, "scan found %d files (expected 4)", n);

    /* current.trace should be in the list */
    int found_current = 0;
    for (int i = 0; i < n; i++) {
        if (strstr(entries[i].path, "current.trace")) {
            found_current = 1;
            CHECK(entries[i].start_wall_ns > 0, "current.trace has start_wall_ns");
            CHECK(!entries[i].placement_unknown,
                  "current.trace placement is known");
        }
    }
    CHECK(found_current, "current.trace found in scan");

    /* Each hourly entry takes its placement from the HEADER, not the name. */
    for (int f = 0; f < 3; f++) {
        const struct pgwt_trace_file_entry *e = NULL;
        for (int i = 0; i < n; i++)
            if (strstr(entries[i].path, fake_files[f])) e = &entries[i];
        CHECK(e != NULL, "%s found in scan", fake_files[f]);
        CHECK(e && !e->placement_unknown,
              "%s: placement known (valid header)", fake_files[f]);
        CHECK(e && e->start_wall_ns == fake_start_ns[f],
              "%s: start_wall_ns=%llu from header (expected %llu, NOT the "
              "name's hour)", fake_files[f],
              e ? (unsigned long long)e->start_wall_ns : 0ULL,
              (unsigned long long)fake_start_ns[f]);
    }

    /* Verify sorted by start_wall_ns */
    int sorted = 1;
    for (int i = 1; i < n; i++) {
        if (entries[i].start_wall_ns < entries[i-1].start_wall_ns) {
            sorted = 0;
            break;
        }
    }
    CHECK(sorted, "entries sorted by start_wall_ns");

    cleanup_test_dir();
}

/* ── Test 7a: #405 bypass — a header that cannot be read must disable the
 * file, never fall back to guessing from its (TZ-dependent) name ──────── */

static void test_scan_trace_files_unknown_placement(void)
{
    printf("--- Scan trace files: unreadable header is disabled, not guessed ---\n");
    cleanup_test_dir();
    setup_test_dir();

    /* A name that matches the archive pattern but whose header is too short
     * to read at all -- the case this is checking is ABSENT placement data,
     * not merely wrong data. The old mktime-from-name code did not care: it
     * would have derived a (possibly TZ-shifted) start from "2025-03-01_09"
     * regardless. The fix must not do that either. */
    char p[512];
    snprintf(p, sizeof(p), "%s/2025-03-01_09.trace.lz4", TEST_DIR);
    FILE *fp = fopen(p, "wb");
    CHECK(fp != NULL, "create truncated archive");
    if (fp) {
        /* 4 bytes: shorter than sizeof(struct pgwt_trace_file_header), so
         * the header fread() itself fails -- not just a magic mismatch. */
        uint32_t partial = PGWT_TRACE_MAGIC;
        fwrite(&partial, sizeof(partial), 1, fp);
        fclose(fp);
    }

    struct pgwt_trace_file_entry entries[8];
    int n = pgwt_scan_trace_files(TEST_DIR, entries, 8);
    CHECK(n == 1, "the unreadable archive is still LISTED (%d), not hidden",
          n);
    CHECK(n == 1 && entries[0].placement_unknown,
          "...marked placement_unknown");
    CHECK(n == 1 && entries[0].start_wall_ns == 0,
          "...with start_wall_ns == 0, not a (possibly TZ-shifted) guess "
          "derived from its name (got %llu)",
          n == 1 ? (unsigned long long)entries[0].start_wall_ns : 1ULL);

    /* And src/replay.c's consumer-side disable must never be fooled into
     * treating this entry as a safe bound for a NEIGHBOUR either: with this
     * entry as `next`, quick_skip must refuse to use it regardless of
     * from_wall_ns. */
    struct pgwt_trace_file_entry two[2] = { entries[0], entries[0] };
    CHECK(!pgwt_trace_file_quick_skip(two, 2, 0, 1ULL),
          "an unknown-placement neighbour never causes a quick-skip");

    /* The short-read case above is ABSENT data; these two are WRONG data of
     * exactly the right length -- a different way the header validation can
     * fail, sharing the same fallback but exercised through a different
     * predicate (magic, then version) rather than fread() itself. Both must
     * land on the identical disabled/listed outcome, never on whatever
     * garbage start_time_ns the bytes happen to contain. */
    cleanup_test_dir();
    setup_test_dir();
    snprintf(p, sizeof(p), "%s/2025-04-02_14.trace.lz4", TEST_DIR);
    fp = fopen(p, "wb");
    CHECK(fp != NULL, "create wrong-magic archive");
    if (fp) {
        struct pgwt_trace_file_header hdr = {
            .magic = 0xDEADBEEFU,           /* full-length, wrong magic */
            .version = PGWT_TRACE_VERSION,
            .pg_version = 18,
            .start_time_ns = 9999999999ULL, /* must NOT surface anywhere */
        };
        fwrite(&hdr, sizeof(hdr), 1, fp);
        fclose(fp);
    }
    n = pgwt_scan_trace_files(TEST_DIR, entries, 8);
    CHECK(n == 1, "wrong-magic archive still LISTED (%d)", n);
    CHECK(n == 1 && entries[0].placement_unknown,
          "...marked placement_unknown (bad magic, not a short read)");
    CHECK(n == 1 && entries[0].start_wall_ns == 0,
          "...start_wall_ns == 0, not the header's bogus %llu",
          n == 1 ? (unsigned long long)entries[0].start_wall_ns : 1ULL);

    cleanup_test_dir();
    setup_test_dir();
    snprintf(p, sizeof(p), "%s/2025-04-02_15.trace.lz4", TEST_DIR);
    fp = fopen(p, "wb");
    CHECK(fp != NULL, "create future-version archive");
    if (fp) {
        struct pgwt_trace_file_header hdr = {
            .magic = PGWT_TRACE_MAGIC,
            .version = PGWT_TRACE_VERSION + 1,  /* this build cannot read it */
            .pg_version = 18,
            .start_time_ns = 8888888888ULL,     /* must NOT surface anywhere */
        };
        fwrite(&hdr, sizeof(hdr), 1, fp);
        fclose(fp);
    }
    n = pgwt_scan_trace_files(TEST_DIR, entries, 8);
    CHECK(n == 1, "future-version archive still LISTED (%d)", n);
    CHECK(n == 1 && entries[0].placement_unknown,
          "...marked placement_unknown (unsupported version, not a short "
          "read): pgwt_reader_open refuses this version (unknown layout) "
          "and the scanner must agree rather than read a field whose "
          "offset a future layout could move");
    CHECK(n == 1 && entries[0].start_wall_ns == 0,
          "...start_wall_ns == 0, not the header's bogus %llu",
          n == 1 ? (unsigned long long)entries[0].start_wall_ns : 1ULL);

    cleanup_test_dir();
}

/* ── Test 7b: #405 — archive placement must survive a reader in a
 * different timezone from the writer ───────────────────────────────── */

/* pgwt_scan_trace_files derives an archived file's placement from its NAME
 * ("YYYY-MM-DD_HH.trace.lz4"), via mktime() in the READER's current
 * timezone. The name is written by the CAPTURE box's localtime_r, so a
 * reader in a different zone parses the same digits as a different instant
 * — the identical #402 displacement already fixed on the summary-file side
 * (tests/test_idle_summary.c's "routing_tz" case), here for traces.
 *
 * This reproduces it at the exact point of consequence: src/replay.c's
 * "quick filter" (`files[fi+1].start_wall_ns < from_wall` => skip file fi)
 * decides whether an hourly file is read at all. We fabricate two archived
 * files with KNOWN, true (header) start times one hour apart, pick --from
 * inside the first file's true coverage, and show that the scan's own
 * reported start_wall_ns values flip that decision from "keep" to "wrongly
 * skip" under TZ=Asia/Kolkata (+5:30, no DST, so the offset is exact and
 * stable) — reproducing a needed file being silently dropped. */

static void write_fake_trace_header(const char *path, uint64_t start_time_ns)
{
    FILE *fp = fopen(path, "wb");
    if (!fp) return;
    struct pgwt_trace_file_header hdr = {
        .magic = PGWT_TRACE_MAGIC,
        .version = PGWT_TRACE_VERSION,
        .pg_version = 18,
        .start_time_ns = start_time_ns,
    };
    fwrite(&hdr, sizeof(hdr), 1, fp);
    fclose(fp);
}

/* Name an archived file the way the writer does: localtime_r of its true
 * start, truncated to the hour. Called with TZ=UTC in effect (the fixture's
 * assumed capture-box zone) so the name reflects a UTC hour exactly. */
static void archive_name_for(uint64_t start_time_ns, char *out, size_t outsz)
{
    time_t sec = (time_t)(start_time_ns / 1000000000ULL);
    struct tm tm;
    localtime_r(&sec, &tm);
    snprintf(out, outsz, "%04d-%02d-%02d_%02d.trace.lz4",
             tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday, tm.tm_hour);
}

/* The quick-filter predicate exactly as src/replay.c applies it: file `fi`
 * is skipped when the NEXT file's reported start is already before `from`. */
static int replay_would_skip(uint64_t next_start_wall_ns, uint64_t from_wall_ns)
{
    return next_start_wall_ns > 0 && next_start_wall_ns < from_wall_ns;
}

static void test_scan_trace_files_timezone(void)
{
    printf("--- Scan trace files: reader timezone != writer timezone (#405) ---\n");
    cleanup_test_dir();
    setup_test_dir();

    /* Fixed, round-hour UTC instants so the fixture is deterministic and
     * never depends on wall-clock "now". 2025-06-15T10:00:00Z / 11:00:00Z. */
    const uint64_t T0 = 1749981600ULL;          /* file A's TRUE start */
    const uint64_t T1 = T0 + 3600ULL;           /* file B's TRUE start, 1h later */
    const uint64_t FROM = T0 + 1800ULL;         /* --from: 30 min into file A */

    char name_a[64], name_b[64];
    char *saved_tz = getenv("TZ");
    char saved_copy[128] = "";
    int had_tz = (saved_tz != NULL);
    if (had_tz) snprintf(saved_copy, sizeof(saved_copy), "%s", saved_tz);

    /* Name the archives as a UTC capture box would (localtime_r there). */
    setenv("TZ", "UTC", 1);
    tzset();
    archive_name_for(T0 * 1000000000ULL, name_a, sizeof(name_a));
    archive_name_for(T1 * 1000000000ULL, name_b, sizeof(name_b));

    char path_a[700], path_b[700];
    snprintf(path_a, sizeof(path_a), "%s/%s", TEST_DIR, name_a);
    snprintf(path_b, sizeof(path_b), "%s/%s", TEST_DIR, name_b);
    write_fake_trace_header(path_a, T0 * 1000000000ULL);
    write_fake_trace_header(path_b, T1 * 1000000000ULL);

    /* The reader is now 5.5h east of the writer. */
    setenv("TZ", "Asia/Kolkata", 1);
    tzset();

    /* NON-VACUITY: if TZ made no difference here, the rest of this test
     * would pass on the old, buggy scanner too and prove nothing. Asia/Kolkata
     * has no DST, so this offset is exact and never flaky. */
    struct tm tmb;
    memset(&tmb, 0, sizeof(tmb));
    int y = 0, mo = 0, d = 0, h = 0;
    CHECK(sscanf(name_b, "%4d-%2d-%2d_%2d.trace.lz4", &y, &mo, &d, &h) == 4,
          "archived name parses: %s", name_b);
    tmb.tm_year = y - 1900; tmb.tm_mon = mo - 1; tmb.tm_mday = d;
    tmb.tm_hour = h; tmb.tm_isdst = -1;
    time_t name_b_local = mktime(&tmb);
    uint64_t buggy_b_wall_ns = (uint64_t)name_b_local * 1000000000ULL;
    CHECK(buggy_b_wall_ns != T1 * 1000000000ULL,
          "under TZ=Asia/Kolkata the NAME-derived start for %s (%llu) must "
          "differ from its true header start (%llu) -- a 0h offset here "
          "would make this fixture assert nothing",
          name_b, (unsigned long long)buggy_b_wall_ns,
          (unsigned long long)(T1 * 1000000000ULL));

    struct pgwt_trace_file_entry entries[8];
    int n = pgwt_scan_trace_files(TEST_DIR, entries, 8);
    CHECK(n == 2, "scan finds both archived files (%d)", n);

    const struct pgwt_trace_file_entry *ea = NULL, *eb = NULL;
    for (int i = 0; i < n; i++) {
        if (strstr(entries[i].path, name_a)) ea = &entries[i];
        if (strstr(entries[i].path, name_b)) eb = &entries[i];
    }
    CHECK(ea != NULL && eb != NULL, "both archived entries identified");

    /* THE FIX'S TARGET: the scanner must report the HEADER's start_time_ns,
     * which is the same absolute instant on every host, not the name read
     * in whatever zone the reader happens to be in. */
    CHECK(ea && ea->start_wall_ns == T0 * 1000000000ULL,
          "file A: scanner reports %llu, header says %llu (TZ-independent)",
          ea ? (unsigned long long)ea->start_wall_ns : 0ULL,
          (unsigned long long)(T0 * 1000000000ULL));
    CHECK(eb && eb->start_wall_ns == T1 * 1000000000ULL,
          "file B: scanner reports %llu, header says %llu (TZ-independent)",
          eb ? (unsigned long long)eb->start_wall_ns : 0ULL,
          (unsigned long long)(T1 * 1000000000ULL));

    /* THE CONSEQUENCE: src/replay.c's quick filter must NOT conclude file A
     * is entirely before --from. File A truly covers [T0, T1) and FROM=T0+30m
     * sits inside it, so file A is needed. On the buggy scanner, file B's
     * mis-shifted start_wall_ns lands far enough in the past (TZ offset
     * dwarfs the 1h file spacing) that the filter wrongly fires. */
    CHECK(eb && !replay_would_skip(eb->start_wall_ns, FROM * 1000000000ULL),
          "replay's quick filter must KEEP file A (needed) -- it would skip "
          "it if file B's reported start (%llu) were still before --from "
          "(%llu)",
          eb ? (unsigned long long)eb->start_wall_ns : 0ULL,
          (unsigned long long)(FROM * 1000000000ULL));

    if (had_tz) setenv("TZ", saved_copy, 1);
    else unsetenv("TZ");
    tzset();

    cleanup_test_dir();
}

/* ── Test 8: Replay accumulates time model correctly ──────── */

static void test_replay_time_model(void)
{
    printf("--- Replay time model ---\n");
    cleanup_test_dir();
    setup_test_dir();

    pgwt_init_event_names(18);

    int N = 50;
    struct pgwt_trace_event *events = calloc(N, sizeof(*events));

    /* 20 CPU events (old_event=0), 20 IO events, 10 LWLock events */
    for (int i = 0; i < N; i++) {
        events[i].timestamp_ns = 2000000ULL + i * 1000ULL;
        events[i].pid = 7000;
        events[i].new_event = 0;
        events[i].query_id = 0;

        if (i < 20) {
            events[i].old_event = 0;                /* CPU */
            events[i].duration_ns = 1000;           /* 1us */
        } else if (i < 40) {
            events[i].old_event = 0x0A000001;       /* IO class (0x0A) */
            events[i].duration_ns = 5000;           /* 5us */
        } else {
            events[i].old_event = 0x01000001;       /* LWLock class (0x01) */
            events[i].duration_ns = 2000;           /* 2us */
        }
    }

    CHECK(write_test_file(events, N, 18) == 0, "write file");

    char path[512];
    snprintf(path, sizeof(path), "%s/current.trace", TEST_DIR);

    struct pgwt_event_reader reader;
    CHECK(pgwt_reader_open(&reader, path) == 0, "reader_open");

    struct pgwt_trace_event *decoded = calloc(PGWT_BLOCK_EVENTS, sizeof(*decoded));
    struct pgwt_block_info binfo;
    int count = pgwt_reader_decode_block_info(&reader, 0, decoded,
                                              PGWT_BLOCK_EVENTS, &binfo);
    CHECK(count == N, "decoded count=%d", count);

    struct pgwt_accumulator *acc = calloc(1, sizeof(*acc));
    pgwt_accum_init(acc);
    pgwt_replay_events(acc, decoded, count, 0, 0, &binfo, NULL);

    /* Verify time model */
    CHECK(acc->tm.cpu_time_ns == 20 * 1000ULL,
          "cpu_time=%lu (expected %lu)",
          (unsigned long)acc->tm.cpu_time_ns, (unsigned long)(20 * 1000ULL));
    CHECK(acc->tm.io_time_ns == 20 * 5000ULL,
          "io_time=%lu (expected %lu)",
          (unsigned long)acc->tm.io_time_ns, (unsigned long)(20 * 5000ULL));
    CHECK(acc->tm.lwlock_time_ns == 10 * 2000ULL,
          "lwlock_time=%lu (expected %lu)",
          (unsigned long)acc->tm.lwlock_time_ns, (unsigned long)(10 * 2000ULL));

    /* db_time = cpu + io + lwlock (no idle events) */
    uint64_t expected_db = 20*1000ULL + 20*5000ULL + 10*2000ULL;
    CHECK(acc->tm.db_time_ns == expected_db,
          "db_time=%lu (expected %lu)",
          (unsigned long)acc->tm.db_time_ns, (unsigned long)expected_db);

    pgwt_reader_close(&reader);
    free(acc);
    free(decoded);
    free(events);
    cleanup_test_dir();
}

/* ── Main ─────────────────────────────────────────────────── */

int main(void)
{
    test_roundtrip_single_block();
    test_roundtrip_multi_block();
    test_block_search();
    test_time_range_replay();
    test_time_parser();
    test_clock_conversion();
    test_scan_trace_files();
    test_scan_trace_files_unknown_placement();
    test_scan_trace_files_timezone();
    test_replay_time_model();

    printf("\n=== %d / %d tests passed ===\n", tests_passed, tests_run);
    return (tests_passed == tests_run) ? 0 : 1;
}
