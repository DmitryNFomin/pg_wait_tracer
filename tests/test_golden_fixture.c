/* test_golden_fixture.c — On-disk format compatibility gate (Phase T7 / TST-10)
 *
 * The other format tests (test_trace_v2, test_durability) are SAME-CODE
 * round-trips: they write with today's writer and read with today's reader, so
 * they cannot catch a change that silently alters the on-disk bytes while
 * keeping the write/read pair self-consistent. This test decodes a COMMITTED
 * golden trace — bytes generated once at the current format revision and frozen
 * in git — and asserts the current reader recovers the exact pinned event
 * stream (a checksum over every decoded column of every block). If a future
 * change alters how existing files decode, this fails.
 *
 * The fixture lives in tests/fixtures/golden/rev<N>/ where <N> is the trace
 * format version (PGWT_TRACE_VERSION). On an INTENTIONAL format bump, do NOT
 * edit this test's expected value or overwrite the fixture: ADD a new
 * rev<N+1>/ fixture (see tests/fixtures/golden/README.md) so both the old and
 * the new on-disk shape stay covered — a reader must keep decoding files
 * written by every shipped version.
 *
 * Built with -DPGWT_SERVER against the server objects (no BPF skeleton),
 * runs anywhere.
 */
#include "event_reader.h"
#include "event_writer.h"
#include "pg_wait_tracer.h"

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>
#include <libgen.h>

static int tests_run = 0;
static int tests_passed = 0;

#define CHECK(cond, fmt, ...) do { \
    tests_run++; \
    if (cond) { tests_passed++; } \
    else { printf("  FAIL: " fmt "\n", ##__VA_ARGS__); } \
} while (0)

/* FNV-1a over a byte range, folded into a running 64-bit accumulator. */
static uint64_t fnv1a(uint64_t h, const void *data, size_t len)
{
    const uint8_t *p = data;
    for (size_t i = 0; i < len; i++) {
        h ^= p[i];
        h *= 0x100000001b3ULL;
    }
    return h;
}

/* Fold one field so the checksum is layout-independent (no struct padding). */
static uint64_t mix_u64(uint64_t h, uint64_t v) { return fnv1a(h, &v, sizeof(v)); }
static uint64_t mix_u32(uint64_t h, uint32_t v) { return fnv1a(h, &v, sizeof(v)); }

/* Fill `out` with fixtures/golden/<rev>/trace resolved next to argv0's dir, so
 * the test works from any CWD (`make -C tests check` runs from tests/;
 * run_all.sh invokes by absolute path from the caller's CWD). */
static void golden_dir(const char *argv0, const char *rev, char *out, size_t outsz)
{
    char buf[600];
    ssize_t n = readlink("/proc/self/exe", buf, sizeof(buf) - 1);
    const char *base;
    char argv0copy[600];
    if (n > 0) {
        buf[n] = '\0';
        base = dirname(buf);   /* modifies buf in place */
    } else {
        snprintf(argv0copy, sizeof(argv0copy), "%s", argv0);
        base = dirname(argv0copy);
    }
    snprintf(out, outsz, "%s/fixtures/golden/%s/trace", base, rev);
}

/* Decode every block of the fixture at `dir`, folding the pinned columns into
 * an FNV-1a checksum. When include_cpu is set the record's cpu_ns joins the
 * mix (rev3+, where the column exists); rev2's frozen checksum predates that
 * column and must NOT include it. Returns 0 on open failure. */
static uint64_t decode_checksum(const char *dir, int expect_version,
                                int include_cpu, int *out_blocks,
                                int *out_events)
{
    char trace_path[800];
    snprintf(trace_path, sizeof(trace_path), "%s/current.trace", dir);

    struct pgwt_event_reader r;
    if (pgwt_reader_open(&r, trace_path) != 0) {
        printf("  FAIL: cannot open golden trace %s "
               "(regenerate — see fixtures/golden/README.md)\n", trace_path);
        return 0;
    }
    CHECK(r.header.version == (uint32_t)expect_version,
          "header version %u == %d", r.header.version, expect_version);

    uint64_t h = 0xcbf29ce484222325ULL;   /* FNV-1a offset basis */
    int total_events = 0;
    int v2_cpu_unknown = 1;   /* v2 fallback: every event must surface UNKNOWN */
    struct pgwt_trace_event evbuf[PGWT_BLOCK_EVENTS];
    for (int b = 0; b < r.num_blocks; b++) {
        struct pgwt_block_info info;
        int n = pgwt_reader_decode_block_info(&r, b, evbuf,
                                              PGWT_BLOCK_EVENTS, &info);
        if (n < 0) { printf("  FAIL: decode_block(%d) failed\n", b); continue; }
        h = mix_u32(h, (uint32_t)info.block_type);
        h = mix_u64(h, info.sample_period_ns);
        for (int i = 0; i < n; i++) {
            const struct pgwt_trace_event *e = &evbuf[i];
            h = mix_u64(h, e->timestamp_ns);
            h = mix_u32(h, e->pid);
            h = mix_u32(h, e->old_event);
            h = mix_u32(h, e->new_event);
            /* Legacy checksums pin on-disk columns; samples never stored a
             * duration column. The unified reader now surfaces their weight. */
            h = mix_u64(h, (e->flags & PGWT_EVENT_FLAG_SAMPLE) && expect_version < 4
                          ? 0 : e->duration_ns);
            h = mix_u64(h, e->query_id);
            h = mix_u32(h, e->flags);
            if (include_cpu)
                h = mix_u64(h, e->cpu_ns);
            if (e->cpu_ns != PGWT_CPU_NS_UNKNOWN)
                v2_cpu_unknown = 0;
        }
        total_events += n;
    }
    if (expect_version < 3)
        CHECK(v2_cpu_unknown,
              "v2-file fallback: every decoded event surfaces cpu_ns=UNKNOWN "
              "(compute then uses gap-inference)");
    printf("  decoded: blocks=%d events=%d checksum=0x%016llxULL\n",
           r.num_blocks, total_events, (unsigned long long)h);
    *out_blocks = r.num_blocks;
    *out_events = total_events;
    pgwt_reader_close(&r);
    return h;
}

/* Pinned decode of the rev2 golden fixture. Regenerate ONLY when intentionally
 * bumping the format (then add a NEW rev dir + a new expected value — never
 * silently change these):
 *
 *   total blocks, total events, and the FNV-1a checksum over every block's
 *   (block_type, sample_period_ns) header field and every decoded record's
 *   (timestamp_ns, pid, old_event, new_event, duration_ns, query_id, flags).
 *
 * The concrete numbers are asserted below; they are filled in from the first
 * run's report (printed when a mismatch occurs) and then frozen.
 */
#define GOLDEN_REV2_BLOCKS   2
#define GOLDEN_REV2_EVENTS   14
#define GOLDEN_REV2_CHECKSUM 0x7d8b576cb43cf60cULL

/* rev3 (T8): trace format v3 with the measured cpu_ns column. Its checksum
 * INCLUDES cpu_ns (rev2's must not). Pinned from the generator's first run. */
#define GOLDEN_REV3_BLOCKS   2
#define GOLDEN_REV3_EVENTS   14
#define GOLDEN_REV3_CHECKSUM 0x5f3f61f4d87d1185ULL

static void test_golden_rev2(const char *argv0)
{
    char dir[700];
    golden_dir(argv0, "rev2", dir, sizeof(dir));
    printf("--- golden rev2 decode + checksum (%s) ---\n", dir);

    int blocks = 0, events = 0;
    /* rev2 is a v2 file: no cpu_ns column, checksum excludes cpu_ns so the
     * frozen value is byte-for-byte the pre-T8 one. A current reader must keep
     * decoding v2 files identically (their events surface cpu_ns=UNKNOWN). */
    uint64_t h = decode_checksum(dir, 2, /*include_cpu=*/0, &blocks, &events);

    CHECK(blocks == GOLDEN_REV2_BLOCKS,
          "block count %d == %d", blocks, GOLDEN_REV2_BLOCKS);
    CHECK(events == GOLDEN_REV2_EVENTS,
          "event count %d == %d", events, GOLDEN_REV2_EVENTS);
    CHECK(h == GOLDEN_REV2_CHECKSUM,
          "checksum 0x%016llx == 0x%016llx (v2 on-disk decode drifted — do NOT "
          "edit this value; a mismatch is the regression this guards)",
          (unsigned long long)h, (unsigned long long)GOLDEN_REV2_CHECKSUM);
}

static void test_golden_rev3(const char *argv0)
{
    char dir[700];
    golden_dir(argv0, "rev3", dir, sizeof(dir));
    printf("--- golden rev3 decode + checksum (%s) ---\n", dir);

    int blocks = 0, events = 0;
    uint64_t h = decode_checksum(dir, 3, /*include_cpu=*/1, &blocks, &events);

    CHECK(blocks == GOLDEN_REV3_BLOCKS,
          "block count %d == %d", blocks, GOLDEN_REV3_BLOCKS);
    CHECK(events == GOLDEN_REV3_EVENTS,
          "event count %d == %d", events, GOLDEN_REV3_EVENTS);
    CHECK(h == GOLDEN_REV3_CHECKSUM,
          "checksum 0x%016llx == 0x%016llx (v3 on-disk decode drifted — do NOT "
          "edit this value)", (unsigned long long)h,
          (unsigned long long)GOLDEN_REV3_CHECKSUM);
}

static void test_golden_rev4(const char *argv0)
{
    char dir[800];
    golden_dir(argv0, "rev4", dir, sizeof(dir));
    int blocks = 0, events = 0;
    uint64_t h = decode_checksum(dir, 4, 1, &blocks, &events);
    CHECK(blocks == 2 && events == 14, "v4 frozen blocks/events");
    CHECK(h == 0xf40542171e1e0d59ULL, "v4 checksum 0x%016llx", (unsigned long long)h);
    char path[900];
    snprintf(path, sizeof(path), "%s/current.trace", dir);
    struct pgwt_event_reader r;
    CHECK(pgwt_reader_open(&r, path) == 0, "v4 frozen open");
    CHECK(r.nominal_sample_period_ns == 100000000, "v4 durable nominal period");
    struct pgwt_block_info bi;
    CHECK(pgwt_reader_block_info(&r, 1, &bi) == 0 &&
          bi.max_sample_weight_ns == 100000000, "v4 header-only maximum weight");
    pgwt_reader_close(&r);
}

int main(int argc, char **argv)
{
    printf("=== test_golden_fixture ===\n");
    test_golden_rev2(argc > 0 ? argv[0] : "");
    test_golden_rev3(argc > 0 ? argv[0] : "");
    test_golden_rev4(argc > 0 ? argv[0] : "");
    printf("\n%d/%d checks passed\n", tests_passed, tests_run);
    return tests_passed == tests_run ? 0 : 1;
}
