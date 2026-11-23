/* test_timeline_assemble.c — timeline wait assembler (#1006 unit 1).
 *
 * S0: pins the BPF wire layout of src/timeline_wire.h with golden bytes
 * (never generated from the structs) and checks that the stub assembler
 * refuses instead of pretending to have produced nothing. S1 replaces the
 * stub checks with one case per rule branch, the mutants and the
 * differential driver (tests/test_timeline_assemble_diff.py runs this
 * binary).
 *
 * Built on the Mac by scripts/check.sh (host C, ASan/UBSan) and on Linux by
 * tests/Makefile; listed in tests/unit_tests.list.
 */
#include "../src/timeline_wire.h"
#include "../src/timeline_assemble.h"

#include <errno.h>
#include <stdio.h>
#include <string.h>

static int fails;
#define CHECK(expr) do { if (!(expr)) { \
    fprintf(stderr, "FAIL line %d: %s\n", __LINE__, #expr); fails++; } } while (0)

/* CALL: hdr{size 72, version 1, kind 1, flags 0x0003, reserved 0,
 * pid 0x01020304, tid 0x05060708}, t0, t1, qid, cpu, run_delay, w, ret -4,
 * sc_id 232, reserved 0. Little-endian, written out by hand. */
static const unsigned char golden_call[72] = {
    0x48,0x00, 0x01, 0x01, 0x03,0x00, 0x00,0x00,
    0x04,0x03,0x02,0x01, 0x08,0x07,0x06,0x05,
    0x10,0x00,0x00,0x00,0x00,0x00,0x00,0x00,      /* t0 = 16 */
    0x20,0x00,0x00,0x00,0x00,0x00,0x00,0x00,      /* t1 = 32 */
    0x88,0x77,0x66,0x55,0x44,0x33,0x22,0x11,      /* qid */
    0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,      /* task_cpu_ns unknown */
    0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,      /* run_delay_ns unknown */
    0x03,0x00,0x00,0x0a,                          /* w = 0x0A000003 (IO) */
    0xfc,0xff,0xff,0xff,                          /* ret = -4 */
    0xe8,0x00,0x00,0x00,                          /* sc_id = 232 */
    0x00,0x00,0x00,0x00,
};

static void test_wire_layout(void)
{
    printf("=== 1. wire layout, sizes and golden bytes ===\n");
    const uint16_t endian = 1;
    CHECK(*(const unsigned char *)&endian == 1);
    CHECK(PGWT_TL_WIRE_VERSION == 1);
    CHECK(PGWT_TL_WIRE_CALL == 1 && PGWT_TL_WIRE_MARK == 2);
    CHECK(PGWT_TL_WIRE_SEED == 3 && PGWT_TL_WIRE_EXIT == 4);
    CHECK(PGWT_TL_WIRE_F_LOST_BEFORE == 0x1);
    CHECK(PGWT_TL_WIRE_F_IN_CMD == 0x2);
    CHECK(PGWT_TL_WIRE_F_QID_UNAVAILABLE == 0x4);
    CHECK(PGWT_TL_CPU_NS_UNKNOWN == PGWT_CPU_NS_UNKNOWN);
    CHECK(sizeof(struct pgwt_tl_wire_hdr) == 16);
    CHECK(sizeof(struct pgwt_tl_wire_call) == 72);
    CHECK(sizeof(struct pgwt_tl_wire_mark) == 48);
    CHECK(sizeof(struct pgwt_tl_wire_seed) == 48);
    CHECK(sizeof(struct pgwt_tl_wire_exit) == 24);
    CHECK(sizeof(struct pgwt_tl_inflight) == 32);

    struct pgwt_tl_wire_call c;
    memcpy(&c, golden_call, sizeof(c));
    CHECK(c.hdr.size == 72);
    CHECK(c.hdr.version == PGWT_TL_WIRE_VERSION);
    CHECK(c.hdr.kind == PGWT_TL_WIRE_CALL);
    CHECK(c.hdr.flags == (PGWT_TL_WIRE_F_LOST_BEFORE | PGWT_TL_WIRE_F_IN_CMD));
    CHECK(c.hdr.reserved == 0);
    CHECK(c.hdr.pid == 0x01020304u && c.hdr.tid == 0x05060708u);
    CHECK(c.t0 == 16 && c.t1 == 32);
    CHECK(c.qid == UINT64_C(0x1122334455667788));
    CHECK(c.task_cpu_ns == PGWT_TL_CPU_NS_UNKNOWN);
    CHECK(c.run_delay_ns == PGWT_TL_CPU_NS_UNKNOWN);
    CHECK(c.w == 0x0A000003u);
    CHECK((int32_t)c.ret == -4);
    CHECK(c.sc_id == 232 && c.reserved == 0);
}

static void test_stub_refuses(void)
{
    printf("=== 2. S0 stub refuses (never an empty success) ===\n");
    struct pgwt_tl_asm_config cfg;
    memset(&cfg, 0, sizeof(cfg));
    errno = 0;
    CHECK(pgwt_tl_asm_create(&cfg) == NULL);
    CHECK(pgwt_tl_asm_feed(NULL, golden_call, sizeof(golden_call)) == -ENOSYS);
    CHECK(pgwt_tl_asm_flush_idle(NULL, 0, NULL, NULL) == -ENOSYS);
    CHECK(pgwt_tl_asm_finish(NULL, 0, NULL, 0) == -ENOSYS);
    CHECK(PGWT_TL_LWLOCK_MERGE_GAP_NS == 20000);
}

int main(int argc, char **argv)
{
    (void)argv;
    if (argc > 1) {
        fprintf(stderr, "usage: test_timeline_assemble (S0: no options yet)\n");
        return 2;
    }
    test_wire_layout();
    test_stub_refuses();
    if (fails) {
        fprintf(stderr, "test_timeline_assemble: %d check(s) failed\n", fails);
        return 1;
    }
    printf("test_timeline_assemble: all checks passed\n");
    return 0;
}
