/* test_event_writer.c — Unit tests for event writer: varint, columnar, file format */
#include "event_writer.h"
#include "pg_wait_tracer.h"

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <assert.h>
#include <unistd.h>
#include <sys/stat.h>

static int tests_run = 0;
static int tests_passed = 0;

#define CHECK(cond, fmt, ...) do { \
    tests_run++; \
    if (cond) { tests_passed++; } \
    else { printf("  FAIL: " fmt "\n", ##__VA_ARGS__); } \
} while(0)

/* ── Varint tests ───────────────────────────────────────────── */

static void test_varint_roundtrip(uint64_t val, const char *label)
{
    uint8_t buf[10];
    int enc_len = pgwt_encode_varint(val, buf);
    CHECK(enc_len > 0 && enc_len <= 10,
          "varint(%s): encode length %d", label, enc_len);

    uint64_t decoded;
    int dec_len = pgwt_decode_varint(buf, enc_len, &decoded);
    CHECK(dec_len == enc_len,
          "varint(%s): decode length %d != encode length %d", label, dec_len, enc_len);
    CHECK(decoded == val,
          "varint(%s): decoded %lu != original %lu", label,
          (unsigned long)decoded, (unsigned long)val);
}

static void test_varint(void)
{
    printf("--- Varint ---\n");
    test_varint_roundtrip(0, "zero");
    test_varint_roundtrip(1, "one");
    test_varint_roundtrip(127, "max_1byte");
    test_varint_roundtrip(128, "min_2byte");
    test_varint_roundtrip(16383, "max_2byte");
    test_varint_roundtrip(16384, "min_3byte");
    test_varint_roundtrip(1000000, "1M");
    test_varint_roundtrip(1000000000ULL, "1G");
    test_varint_roundtrip(UINT64_MAX, "UINT64_MAX");

    /* Encoding size checks */
    uint8_t buf[10];
    CHECK(pgwt_encode_varint(0, buf) == 1, "varint(0) should be 1 byte");
    CHECK(pgwt_encode_varint(127, buf) == 1, "varint(127) should be 1 byte");
    CHECK(pgwt_encode_varint(128, buf) == 2, "varint(128) should be 2 bytes");
    CHECK(pgwt_encode_varint(16383, buf) == 2, "varint(16383) should be 2 bytes");
    CHECK(pgwt_encode_varint(16384, buf) == 3, "varint(16384) should be 3 bytes");

    /* Decode with insufficient bytes */
    buf[0] = 0x80;  /* continuation bit set but no more bytes */
    uint64_t val;
    int rc = pgwt_decode_varint(buf, 1, &val);
    CHECK(rc == -1, "varint: incomplete byte should return -1");
}

/* ── Columnar encoder tests ─────────────────────────────────── */

static void test_encode_block(void)
{
    printf("--- Columnar Encoder ---\n");

    /* Create test events with predictable data */
    int count = 100;
    struct pgwt_trace_event events[100];
    for (int i = 0; i < count; i++) {
        events[i].timestamp_ns = 1000000000ULL + i * 1000;  /* 1us apart */
        events[i].pid = 1234;
        events[i].old_event = 0x05000015;  /* IO:DataFileRead */
        events[i].new_event = 0;           /* CPU */
        events[i].duration_ns = 50000 + (i % 10) * 100;     /* ~50us */
        events[i].query_id = 12345678ULL;
    }

    /* Raw size: 100 events × (8+4+4+4+8+8) = 3600 bytes per column set */
    size_t raw_size = count * 36;

    uint8_t *out = malloc(raw_size * 2);
    size_t encoded = pgwt_encode_block(events, count, out, raw_size * 2);

    CHECK(encoded > 0, "encode_block returned %zu bytes", encoded);
    CHECK(encoded < raw_size,
          "encoded %zu should be < raw %zu (delta encoding saves space)",
          encoded, raw_size);

    /* With identical PIDs and events, columns 2-4 are constant,
     * timestamps are small deltas, durations are small values.
     * Encoded should be much smaller than raw. */
    printf("  INFO: %d events: raw=%zu, encoded=%zu (%.1fx)\n",
           count, raw_size, encoded, (double)raw_size / encoded);

    free(out);
}

/* ── File format tests ──────────────────────────────────────── */

static void test_file_roundtrip(void)
{
    printf("--- File Roundtrip ---\n");

    char tmpdir[] = "/tmp/pgwt_test_XXXXXX";
    if (!mkdtemp(tmpdir)) {
        printf("  SKIP: cannot create temp dir\n");
        return;
    }

    struct pgwt_event_writer w;
    int rc = pgwt_writer_init(&w, tmpdir, 18, 24, NULL);
    CHECK(rc == 0, "writer_init returned %d", rc);
    if (rc != 0) { rmdir(tmpdir); return; }

    /* Push 5000 events (more than one block of 4096) */
    int total = 5000;
    for (int i = 0; i < total; i++) {
        struct pgwt_trace_event evt = {
            .timestamp_ns = 1000000000ULL + (uint64_t)i * 10000,
            .pid = 1000 + (i % 4),
            .old_event = (i % 3 == 0) ? 0 : 0x05000015,
            .new_event = (i % 3 == 0) ? 0x05000015 : 0,
            .duration_ns = 5000 + (i % 100) * 100,
            .query_id = (i % 2 == 0) ? 99999ULL : 0,
        };
        rc = pgwt_writer_push_event(&w, &evt);
        CHECK(rc == 0, "push_event[%d] returned %d", i, rc);
        if (rc != 0) break;
    }

    /* Should have flushed at least 1 block (4096 events) */
    CHECK(w.num_blocks >= 1, "expected ≥1 block, got %d", w.num_blocks);
    CHECK(w.total_events_written >= 4096,
          "expected ≥4096 events written, got %lu",
          (unsigned long)w.total_events_written);

    /* Close (flushes remaining events + footer) */
    rc = pgwt_writer_close(&w);
    CHECK(rc == 0, "writer_close returned %d", rc);

    /* Verify file exists and has correct header */
    char path[512];
    snprintf(path, sizeof(path), "%s/current.trace", tmpdir);

    FILE *fp = fopen(path, "rb");
    CHECK(fp != NULL, "trace file %s should exist", path);
    if (!fp) { pgwt_writer_destroy(&w); rmdir(tmpdir); return; }

    /* Read and verify header */
    struct pgwt_trace_file_header hdr;
    size_t nr = fread(&hdr, sizeof(hdr), 1, fp);
    CHECK(nr == 1, "read file header");
    CHECK(hdr.magic == PGWT_TRACE_MAGIC,
          "magic 0x%08x == 0x%08x", hdr.magic, PGWT_TRACE_MAGIC);
    CHECK(hdr.version == PGWT_TRACE_VERSION,
          "version %d == %d", hdr.version, PGWT_TRACE_VERSION);
    /* #424: the flags field now also carries the closure bits (set when the
     * file is finalized — this file was closed, so CLOSURE_TRACKED and
     * CLOSED_CLEAN are present). The assertion keeps its original intent —
     * LZ4 set, and NO bit outside the two known groups — rather than being
     * loosened to "the LZ4 bit is somewhere in there": a stray bit in the
     * remaining 11 would still fail here. The closure bits themselves are
     * asserted in tests/test_trace_closure.c. */
    CHECK((hdr.flags & PGWT_FLAG_LZ4) != 0, "flags 0x%04x has LZ4", hdr.flags);
    CHECK((hdr.flags & ~(uint16_t)(PGWT_FLAG_LZ4 | PGWT_FLAG_CLOSURE_MASK)) == 0,
          "flags 0x%04x carries no bit outside LZ4 + the closure mask",
          hdr.flags);
    CHECK((hdr.flags & PGWT_FLAG_CLOSED_CLEAN) != 0,
          "flags 0x%04x marks this closed file CLEAN", hdr.flags);
    CHECK(hdr.pg_version == 18, "pg_version %d == 18", hdr.pg_version);
    CHECK(hdr.start_time_ns > 0, "start_time_ns > 0");
    CHECK(hdr.clock_offset_ns > 0, "clock_offset_ns > 0");

    /* Read first block header */
    struct pgwt_trace_block_header bh;
    nr = fread(&bh, sizeof(bh), 1, fp);
    CHECK(nr == 1, "read block header");
    CHECK(bh.num_events == 4096,
          "first block should have 4096 events, got %u", bh.num_events);
    CHECK(bh.compressed_size > 0, "compressed_size > 0");
    CHECK(bh.uncompressed_size > 0, "uncompressed_size > 0");
    CHECK(bh.compressed_size < bh.uncompressed_size,
          "compressed %u < uncompressed %u",
          bh.compressed_size, bh.uncompressed_size);

    /* Verify footer: seek to last 4 bytes for num_blocks */
    fseek(fp, -4, SEEK_END);
    uint32_t footer_num_blocks;
    nr = fread(&footer_num_blocks, sizeof(footer_num_blocks), 1, fp);
    CHECK(nr == 1, "read footer num_blocks");
    CHECK(footer_num_blocks == 2,
          "footer num_blocks=%u (expected 2: 4096 + 904)", footer_num_blocks);

    /* Read block index from footer */
    long footer_start = ftell(fp) - 4 -
                         footer_num_blocks * sizeof(struct pgwt_block_index_entry);
    fseek(fp, footer_start, SEEK_SET);

    struct pgwt_block_index_entry idx[2];
    nr = fread(idx, sizeof(idx[0]), footer_num_blocks, fp);
    CHECK(nr == footer_num_blocks, "read block index");
    CHECK(idx[0].file_offset == sizeof(struct pgwt_trace_file_header),
          "first block offset=%lu == %lu (header size)",
          (unsigned long)idx[0].file_offset,
          (unsigned long)sizeof(struct pgwt_trace_file_header));
    CHECK(idx[1].file_offset > idx[0].file_offset,
          "second block offset > first");
    CHECK(idx[0].timestamp_ns < idx[1].timestamp_ns,
          "block timestamps increase");

    /* Report compression ratio */
    fseek(fp, 0, SEEK_END);
    long file_size = ftell(fp);
    double raw_estimate = (double)total * 36;
    printf("  INFO: %d events → %ld bytes on disk (%.1fx vs raw)\n",
           total, file_size, raw_estimate / file_size);

    fclose(fp);
    unlink(path);
    pgwt_writer_destroy(&w);
    rmdir(tmpdir);
}

/* ── Cleanup test ───────────────────────────────────────────── */

static void test_cleanup_old_files(void)
{
    printf("--- Cleanup Old Files ---\n");

    char tmpdir[] = "/tmp/pgwt_cleanup_XXXXXX";
    if (!mkdtemp(tmpdir)) {
        printf("  SKIP: cannot create temp dir\n");
        return;
    }

    /* Create fake old trace files */
    char path[512];
    snprintf(path, sizeof(path), "%s/2020-01-01_00.trace.lz4", tmpdir);
    FILE *f = fopen(path, "w");
    if (f) { fprintf(f, "old"); fclose(f); }

    snprintf(path, sizeof(path), "%s/2099-12-31_23.trace.lz4", tmpdir);
    f = fopen(path, "w");
    if (f) { fprintf(f, "future"); fclose(f); }

    struct pgwt_event_writer w;
    memset(&w, 0, sizeof(w));
    snprintf(w.trace_dir, sizeof(w.trace_dir), "%s", tmpdir);
    w.retention_hours = 1;

    pgwt_writer_cleanup_old_files(&w);

    /* Old file should be deleted */
    snprintf(path, sizeof(path), "%s/2020-01-01_00.trace.lz4", tmpdir);
    CHECK(access(path, F_OK) != 0, "old file should be deleted");

    /* Future file should still exist */
    snprintf(path, sizeof(path), "%s/2099-12-31_23.trace.lz4", tmpdir);
    CHECK(access(path, F_OK) == 0, "future file should be kept");
    unlink(path);

    rmdir(tmpdir);
}

/* ── Main ───────────────────────────────────────────────────── */

int main(void)
{
    printf("=== test_event_writer ===\n");
    test_varint();
    test_encode_block();
    test_file_roundtrip();
    test_cleanup_old_files();

    printf("\n%d/%d tests passed\n", tests_passed, tests_run);
    return (tests_passed == tests_run) ? 0 : 1;
}
