/* test_timeline_writer.c — timeline-NNNN.pgwtl writer and reader (#1006).
 *
 * S0: pins the on-disk layout of src/timeline_format.h with golden bytes
 * (never generated from the structs): header, a WAIT record, a STATS
 * trailer, the kind and flag values; and checks the stub writer refuses.
 * S2 adds round trip, rotation boundary, trailer counts, unknown version
 * refused, unknown kind skipped, missing trailer = TRUNCATED.
 *
 * Built on the Mac by scripts/check.sh (host C, ASan/UBSan) and on Linux by
 * tests/Makefile; listed in tests/unit_tests.list.
 */
#include "../src/timeline_format.h"
#include "../src/timeline_writer.h"
#include "../src/pg_wait_tracer.h"

#include <errno.h>
#include <stdio.h>
#include <string.h>

static int fails;
#define CHECK(expr) do { if (!(expr)) { \
    fprintf(stderr, "FAIL line %d: %s\n", __LINE__, #expr); fails++; } } while (0)

static const unsigned char golden_header[64] = {
    'P','G','W','T','T','L','0','1',
    0x01,0x00, 0x40,0x00,                         /* version 1, rec_size 64 */
    0x14,0x98,0x02,0x00,                          /* pg_version 170004 */
    0x39,0x30,0x00,0x00,                          /* postmaster_pid 12345 */
    0x03,0x00,0x00,0x00,                          /* caps: probe, ST_QUERY_ID */
    0x00,0xe1,0xf5,0x05,0x00,0x00,0x00,0x00,      /* mono_start 100000000 */
    0x00,0x00,0x00,0x00,0x01,0x00,0x00,0x00,      /* wall_start 2^32 */
    0x00,0x11,0x22,0x33,0x44,0x55,0x66,0x77,      /* boot_id */
    0x88,0x99,0xaa,0xbb,0xcc,0xdd,0xee,0xff,
    0x07,0x00,0x00,0x00,                          /* segment_index 7 */
    0x00,0x00,0x00,0x00,
};

/* WAIT: IO:DataFileRead-like word, TRUNC_END | IN_CMD_NO_QID, 3 calls. */
static const unsigned char golden_wait[64] = {
    0xe8,0x03,0x00,0x00,0x00,0x00,0x00,0x00,      /* start_ns 1000 */
    0xd0,0x07,0x00,0x00,0x00,0x00,0x00,0x00,      /* end_ns 2000 */
    0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,      /* query_id 0 */
    0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,      /* on_cpu_ns unknown */
    0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,      /* runq_ns unknown */
    0x39,0x30,0x00,0x00,                          /* pid 12345 */
    0x01,0x00,0x00,0x0a,                          /* wait_event_info */
    0x03,0x00,0x00,0x00,                          /* n_syscalls 3 */
    0x00,0x00,                                    /* kind WAIT */
    0x28,0x00,                                    /* flags 0x0028 */
    0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
};

static const unsigned char golden_stats[64] = {
    0x05,0x00,0x00,0x00,0x00,0x00,0x00,0x00,      /* records 5 */
    0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,      /* lost_raw_total 0 */
    0x10,0x27,0x00,0x00,0x00,0x00,0x00,0x00,      /* end_ns 10000 */
    0x04,0x00,0x00,0x00, 0x01,0x00,0x00,0x00,     /* n_wait 4, n_zero 1 */
    0x00,0x00,0x00,0x00, 0x00,0x00,0x00,0x00,     /* no_wait, probe_state */
    0x01,0x00,0x00,0x00, 0x00,0x00,0x00,0x00,     /* loss 1, backend 0 */
    0x00,0x00,0x00,0x00,                          /* other 0 */
    0xff,0x00, 0x00,0x00,                         /* kind 255, flags 0 */
    0x01,0x00,0x00,0x00,                          /* stats_flags FINAL */
    0x00,0x00,0x00,0x00,
};

static void test_constants(void)
{
    printf("=== 1. format constants ===\n");
    CHECK(memcmp(PGWT_TL_MAGIC, "PGWTTL01", PGWT_TL_MAGIC_LEN) == 0);
    CHECK(PGWT_TL_VERSION == 1);
    CHECK(PGWT_TL_HEADER_SIZE == 64 && PGWT_TL_REC_SIZE == 64);
    CHECK(PGWT_TL_SEGMENT_BYTES == 256ULL * 1024 * 1024);
    CHECK(PGWT_TL_NS_UNKNOWN == PGWT_CPU_NS_UNKNOWN);
    CHECK(PGWT_TL_KIND_WAIT == 0 && PGWT_TL_KIND_NO_WAIT == 1);
    CHECK(PGWT_TL_KIND_PROBE_STATE == 2 && PGWT_TL_KIND_LOSS == 3);
    CHECK(PGWT_TL_KIND_BACKEND == 4 && PGWT_TL_KIND_STATS == 255);
    CHECK(PGWT_TL_F_MARKER_OFF == 0x01 && PGWT_TL_F_ZERO_MARKER == 0x02);
    CHECK(PGWT_TL_F_TRUNC_START == 0x04 && PGWT_TL_F_TRUNC_END == 0x08);
    CHECK(PGWT_TL_F_QID_UNAVAILABLE == 0x10 && PGWT_TL_F_IN_CMD_NO_QID == 0x20);
    CHECK(PGWT_TL_F_PG13_SYNTH_QID == 0x40 && PGWT_TL_F_LWLOCK_TRACEPOINT == 0x80);
    CHECK(PGWT_TL_CAP_MARKER_PROBE_ATTACHED == 0x1);
    CHECK(PGWT_TL_CAP_CPU_SPLIT_PRESENT == 0x8);
    CHECK(PGWT_TL_CAP_QID_SOURCE(0x2u) == PGWT_TL_QID_SOURCE_ST_QUERY_ID);
    CHECK(PGWT_TL_CAP_QID_SOURCE(0x4u) == PGWT_TL_QID_SOURCE_PG13_RESERVED);
    CHECK(PGWT_TL_CAP_QID_SOURCE(0x9u) == PGWT_TL_QID_SOURCE_NONE);
    CHECK(PGWT_TL_STATS_FINAL == 1 && PGWT_TL_STATS_ROTATED == 2);
}

static void test_golden_layout(void)
{
    printf("=== 2. header, WAIT and STATS golden bytes ===\n");
    CHECK(sizeof(struct pgwt_tl_header) == 64);
    CHECK(sizeof(struct pgwt_tl_record) == 64);
    CHECK(sizeof(struct pgwt_tl_stats) == 64);

    struct pgwt_tl_header h;
    memcpy(&h, golden_header, sizeof(h));
    CHECK(memcmp(h.magic, PGWT_TL_MAGIC, PGWT_TL_MAGIC_LEN) == 0);
    CHECK(h.version == 1 && h.rec_size == 64);
    CHECK(h.pg_version == 170004 && h.postmaster_pid == 12345);
    CHECK(h.caps == (PGWT_TL_CAP_MARKER_PROBE_ATTACHED |
                     (PGWT_TL_QID_SOURCE_ST_QUERY_ID << PGWT_TL_CAP_QID_SOURCE_SHIFT)));
    CHECK(h.mono_start_ns == 100000000ULL && h.wall_start_ns == (1ULL << 32));
    CHECK(h.boot_id[0] == 0x00 && h.boot_id[15] == 0xff);
    CHECK(h.segment_index == 7 && h.reserved == 0);

    struct pgwt_tl_record r;
    memcpy(&r, golden_wait, sizeof(r));
    CHECK(r.start_ns == 1000 && r.end_ns == 2000 && r.query_id == 0);
    CHECK(r.on_cpu_ns == PGWT_TL_NS_UNKNOWN && r.runq_ns == PGWT_TL_NS_UNKNOWN);
    CHECK(r.pid == 12345 && r.wait_event_info == 0x0A000001u);
    CHECK(r.n_syscalls == 3 && r.kind == PGWT_TL_KIND_WAIT);
    CHECK(r.flags == (PGWT_TL_F_TRUNC_END | PGWT_TL_F_IN_CMD_NO_QID));
    CHECK(r.reserved[0] == 0 && r.reserved[1] == 0);

    struct pgwt_tl_stats s;
    memcpy(&s, golden_stats, sizeof(s));
    CHECK(s.records == 5 && s.lost_raw_total == 0 && s.end_ns == 10000);
    CHECK(s.n_wait == 4 && s.n_zero_marker == 1 && s.n_loss == 1);
    CHECK(s.n_no_wait == 0 && s.n_probe_state == 0 && s.n_backend == 0);
    CHECK(s.n_other == 0 && s.kind == PGWT_TL_KIND_STATS && s.flags == 0);
    CHECK(s.stats_flags == PGWT_TL_STATS_FINAL && s.reserved == 0);
    /* kind sits at the same offset in every record */
    struct pgwt_tl_record as_rec;
    memcpy(&as_rec, golden_stats, sizeof(as_rec));
    CHECK(as_rec.kind == PGWT_TL_KIND_STATS);
}

static void test_stub_refuses(void)
{
    printf("=== 3. S0 stub refuses (never an empty success) ===\n");
    struct pgwt_tl_writer *w = (struct pgwt_tl_writer *)1;
    struct pgwt_tl_writer_config cfg;
    memset(&cfg, 0, sizeof(cfg));
    CHECK(pgwt_tl_writer_open(&w, &cfg) == -ENOSYS && w == NULL);
    struct pgwt_tl_reader *r = (struct pgwt_tl_reader *)1;
    CHECK(pgwt_tl_reader_open(&r, "/nonexistent") == -ENOSYS && r == NULL);
}

int main(void)
{
    test_constants();
    test_golden_layout();
    test_stub_refuses();
    if (fails) {
        fprintf(stderr, "test_timeline_writer: %d check(s) failed\n", fails);
        return 1;
    }
    printf("test_timeline_writer: all checks passed\n");
    return 0;
}
