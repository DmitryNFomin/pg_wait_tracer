/* test_trace_v2.c — Unit tests for trace format v2 (Phase A1)
 *
 * Round-trips a v2 file containing BOTH a TRANSITIONS block and a SAMPLES
 * block, asserting every column survives and that block_type /
 * sample_period_ns are preserved. Also asserts the v1-rejection behavior:
 * a file with an old version byte must fail to open with a clear error,
 * not crash.
 *
 * Built with -DPGWT_SERVER against the server objects so it needs no BPF
 * skeleton (compiles on a box without bpftool, and in CI). */
#include "event_writer.h"
#include "event_reader.h"

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>
#include <sys/stat.h>

static int tests_run = 0;
static int tests_passed = 0;

#define CHECK(cond, fmt, ...) do { \
    tests_run++; \
    if (cond) { tests_passed++; } \
    else { printf("  FAIL: " fmt "\n", ##__VA_ARGS__); } \
} while(0)

static const char *TEST_DIR = "/tmp/test_trace_v2";

static void cleanup_test_dir(void)
{
    char cmd[512];
    snprintf(cmd, sizeof(cmd), "rm -rf %s", TEST_DIR);
    if (system(cmd) != 0) { /* ignore */ }
}

/* ── Test 1: mixed TRANSITIONS + SAMPLES round-trip ───────── */

static void test_mixed_roundtrip(void)
{
    printf("--- v2 mixed (TRANSITIONS + SAMPLES) round-trip ---\n");
    cleanup_test_dir();
    mkdir(TEST_DIR, 0755);

    const int NT = 64;   /* transitions */
    const int NS = 40;   /* samples */
    const uint64_t SAMPLE_PERIOD = 100000000ULL;  /* 100 ms = 10 Hz */

    struct pgwt_trace_event trans[64];
    for (int i = 0; i < NT; i++) {
        trans[i].timestamp_ns = 1000000ULL + (uint64_t)i * 1000ULL;
        trans[i].pid = 1000 + (i % 8);
        trans[i].old_event = 0x0A000000U + (uint32_t)i;
        trans[i].new_event = 0x03000000U + (uint32_t)i;
        trans[i].flags = 0;
        trans[i].duration_ns = 500 + (uint64_t)i * 13;
        trans[i].query_id = (i % 4 == 0) ? 0 : (uint64_t)(9000 + i);
        /* T8 v3: exercise the measured cpu_ns column across its three shapes —
         * the UNKNOWN sentinel (10-byte varint), a genuine 0 (waits), and real
         * values — so the round-trip proves the varint column survives. */
        trans[i].cpu_ns = (i % 5 == 0) ? PGWT_CPU_NS_UNKNOWN
                        : (i % 5 == 1) ? 0
                        : (uint64_t)i * 100000;
    }

    /* Refs #804: tracer-owned ids must survive transition AND sample codecs. */
    trans[0].old_event = PGWT_WEI_CKPT_PACING;
    trans[0].new_event = PGWT_WEI_CKPT_PACING;
    struct pgwt_trace_event samp[40];
    for (int i = 0; i < NS; i++) {
        /* Samples come after the transitions, sorted ascending. */
        samp[i].timestamp_ns = 2000000ULL + (uint64_t)i * SAMPLE_PERIOD;
        samp[i].pid = 2000 + (i % 5);
        /* Every 7th sample is a first-class CPU sample (event id 0 — T2):
         * id 0 must round-trip like any other id, distinguishable from a
         * transition only by FLAG_SAMPLE. */
        samp[i].new_event = (i % 7 == 0) ? 0
                          : 0x01000000U + (uint32_t)i;  /* sampled event */
        /* old_event/duration deliberately set to nonzero garbage to prove
         * the writer ignores them and the reader returns them zeroed. */
        samp[i].old_event = 0xDEADBEEFU;
        samp[i].duration_ns = 0xCAFEull;
        samp[i].flags = 0;
        samp[i].query_id = (i % 3 == 0) ? 0 : (uint64_t)(7000 + i);
    }

    /* Write: a transition block, then a sample block. */
    samp[0].new_event = PGWT_WEI_CKPT_PACING;

    struct pgwt_event_writer w;
    CHECK(pgwt_writer_init(&w, TEST_DIR, 18, 24, NULL) == 0, "writer_init");
    w.cpu_measured = true;   /* keep the per-event cpu_ns (do not stamp UNKNOWN) */
    for (int i = 0; i < NT; i++)
        CHECK(pgwt_writer_push_event(&w, &trans[i]) == 0, "push_event %d", i);
    CHECK(pgwt_writer_push_samples(&w, samp, NS, SAMPLE_PERIOD) == 0,
          "push_samples");
    pgwt_writer_close(&w);
    pgwt_writer_destroy(&w);

    /* Read back. */
    char path[512];
    snprintf(path, sizeof(path), "%s/current.trace", TEST_DIR);

    struct pgwt_event_reader r;
    CHECK(pgwt_reader_open(&r, path) == 0, "reader_open");
    CHECK(r.header.version == PGWT_TRACE_VERSION, "version=%u", r.header.version);
    CHECK(r.header.version == 3, "version is 3 (T8 cpu_ns column)");
    CHECK(r.num_blocks == 2, "num_blocks=%d (expected 2)", r.num_blocks);

    struct pgwt_trace_event out[PGWT_BLOCK_EVENTS];
    struct pgwt_block_info info;

    /* Block 0: TRANSITIONS */
    int n0 = pgwt_reader_decode_block_info(&r, 0, out, PGWT_BLOCK_EVENTS, &info);
    CHECK(n0 == NT, "block0 count=%d (expected %d)", n0, NT);
    CHECK(info.block_type == PGWT_BLOCK_TRANSITIONS, "block0 type=TRANSITIONS");
    CHECK(info.sample_period_ns == 0, "block0 period=0");
    int tmiss = 0;
    for (int i = 0; i < n0 && i < NT; i++) {
        if (out[i].timestamp_ns != trans[i].timestamp_ns ||
            out[i].pid != trans[i].pid ||
            out[i].old_event != trans[i].old_event ||
            out[i].new_event != trans[i].new_event ||
            out[i].duration_ns != trans[i].duration_ns ||
            out[i].query_id != trans[i].query_id ||
            out[i].cpu_ns != trans[i].cpu_ns ||   /* T8 v3 column */
            out[i].flags != 0)
            tmiss++;
    }
    CHECK(tmiss == 0, "transition column mismatches: %d", tmiss);

    /* Block 1: SAMPLES */
    int n1 = pgwt_reader_decode_block_info(&r, 1, out, PGWT_BLOCK_EVENTS, &info);
    CHECK(n1 == NS, "block1 count=%d (expected %d)", n1, NS);
    CHECK(info.block_type == PGWT_BLOCK_SAMPLES, "block1 type=SAMPLES");
    CHECK(info.sample_period_ns == SAMPLE_PERIOD,
          "block1 period=%llu (expected %llu)",
          (unsigned long long)info.sample_period_ns,
          (unsigned long long)SAMPLE_PERIOD);
    int smiss = 0;
    for (int i = 0; i < n1 && i < NS; i++) {
        if (out[i].timestamp_ns != samp[i].timestamp_ns ||
            out[i].pid != samp[i].pid ||
            out[i].new_event != samp[i].new_event ||   /* sampled event */
            out[i].query_id != samp[i].query_id ||
            out[i].old_event != 0 ||                    /* cleared */
            out[i].duration_ns != 0 ||                  /* cleared */
            out[i].cpu_ns != PGWT_CPU_NS_UNKNOWN ||     /* samples carry no CPU */
            (out[i].flags & PGWT_EVENT_FLAG_SAMPLE) == 0)  /* flagged */
            smiss++;
    }
    CHECK(smiss == 0, "sample column mismatches: %d", smiss);

    /* block_info without decode should agree */
    struct pgwt_block_info bi0, bi1;
    CHECK(pgwt_reader_block_info(&r, 0, &bi0) == 0, "block_info(0)");
    CHECK(pgwt_reader_block_info(&r, 1, &bi1) == 0, "block_info(1)");
    CHECK(bi0.block_type == PGWT_BLOCK_TRANSITIONS, "bi0 transitions");
    CHECK(bi1.block_type == PGWT_BLOCK_SAMPLES, "bi1 samples");
    CHECK(bi1.sample_period_ns == SAMPLE_PERIOD, "bi1 period");

    pgwt_reader_close(&r);
    cleanup_test_dir();
}

/* ── Test 2: SAMPLES-only trace ───────────────────────────── */

static void test_samples_only(void)
{
    printf("--- v2 SAMPLES-only round-trip ---\n");
    cleanup_test_dir();
    mkdir(TEST_DIR, 0755);

    const int NS = 1000;
    const uint64_t SAMPLE_PERIOD = 10000000ULL;  /* 10 ms = 100 Hz */
    struct pgwt_trace_event *samp = calloc(NS, sizeof(*samp));
    for (int i = 0; i < NS; i++) {
        samp[i].timestamp_ns = 5000000ULL + (uint64_t)i * SAMPLE_PERIOD;
        samp[i].pid = 3000 + (i % 50);
        samp[i].new_event = (i % 7 == 0) ? 0 : (0x08000000U + (uint32_t)(i % 20));
        samp[i].query_id = (uint64_t)(100 + i);
    }

    struct pgwt_event_writer w;
    CHECK(pgwt_writer_init(&w, TEST_DIR, 17, 24, NULL) == 0, "writer_init");
    CHECK(pgwt_writer_push_samples(&w, samp, NS, SAMPLE_PERIOD) == 0,
          "push_samples (%d)", NS);
    pgwt_writer_close(&w);
    pgwt_writer_destroy(&w);

    char path[512];
    snprintf(path, sizeof(path), "%s/current.trace", TEST_DIR);

    struct pgwt_event_reader r;
    CHECK(pgwt_reader_open(&r, path) == 0, "reader_open");
    CHECK(r.num_blocks >= 1, "num_blocks=%d", r.num_blocks);

    struct pgwt_trace_event out[PGWT_BLOCK_EVENTS];
    int total = 0, miss = 0;
    for (int b = 0; b < r.num_blocks; b++) {
        struct pgwt_block_info info;
        int n = pgwt_reader_decode_block_info(&r, b, out, PGWT_BLOCK_EVENTS, &info);
        CHECK(info.block_type == PGWT_BLOCK_SAMPLES, "block %d is SAMPLES", b);
        CHECK(info.sample_period_ns == SAMPLE_PERIOD, "block %d period", b);
        for (int i = 0; i < n && total + i < NS; i++) {
            int idx = total + i;
            if (out[i].timestamp_ns != samp[idx].timestamp_ns ||
                out[i].pid != samp[idx].pid ||
                out[i].new_event != samp[idx].new_event ||
                out[i].query_id != samp[idx].query_id ||
                (out[i].flags & PGWT_EVENT_FLAG_SAMPLE) == 0)
                miss++;
        }
        total += n;
    }
    CHECK(total == NS, "total=%d (expected %d)", total, NS);
    CHECK(miss == 0, "samples-only mismatches: %d", miss);

    pgwt_reader_close(&r);
    free(samp);
    cleanup_test_dir();
}

/* ── Test 3: v1 (old version byte) rejected, not crashing ─── */

static void test_v1_rejected(void)
{
    printf("--- v1 file rejected with clear error ---\n");
    cleanup_test_dir();
    mkdir(TEST_DIR, 0755);

    /* Write a valid v2 file, then flip the version byte to 1. */
    struct pgwt_trace_event e = {
        .timestamp_ns = 1000000, .pid = 4000,
        .old_event = 0, .new_event = 0,
        .duration_ns = 100, .query_id = 0, .flags = 0,
    };
    struct pgwt_event_writer w;
    CHECK(pgwt_writer_init(&w, TEST_DIR, 18, 24, NULL) == 0, "writer_init");
    CHECK(pgwt_writer_push_event(&w, &e) == 0, "push_event");
    pgwt_writer_close(&w);
    pgwt_writer_destroy(&w);

    char path[512];
    snprintf(path, sizeof(path), "%s/current.trace", TEST_DIR);

    /* Patch version field (offset 4: after magic u32) to 1. */
    FILE *fp = fopen(path, "r+b");
    CHECK(fp != NULL, "open for patch");
    if (fp) {
        struct pgwt_trace_file_header hdr;
        CHECK(fread(&hdr, sizeof(hdr), 1, fp) == 1, "read header");
        CHECK(hdr.version == PGWT_TRACE_VERSION, "was current version");
        hdr.version = 1;
        fseek(fp, 0, SEEK_SET);
        CHECK(fwrite(&hdr, sizeof(hdr), 1, fp) == 1, "write v1 header");
        fclose(fp);
    }

    /* Also drop the meta file so the reader actually parses the header
     * (meta is keyed to a valid v2 layout). It is fine either way: the
     * version check happens before any block parsing. */
    char meta[600];
    snprintf(meta, sizeof(meta), "%s.meta", path);
    unlink(meta);

    struct pgwt_event_reader r;
    int rc = pgwt_reader_open(&r, path);
    CHECK(rc == -1, "v1 open rejected (rc=%d, expected -1)", rc);
    /* No crash: if open returned 0 we'd close to avoid a leak. */
    if (rc == 0) pgwt_reader_close(&r);

    cleanup_test_dir();
}

int main(void)
{
    test_mixed_roundtrip();
    test_samples_only();
    test_v1_rejected();

    printf("\n=== %d / %d tests passed ===\n", tests_passed, tests_run);
    return (tests_passed == tests_run) ? 0 : 1;
}
