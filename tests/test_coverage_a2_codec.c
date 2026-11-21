/* Refs #933: carrier-only golden bytes; no A2 policy assertions.
 * Literal payloads below were calculated using Python struct.pack('<IHBBQQQQ')
 * and a separate CRC32C calculation, not the product encoder. */
#include "coverage_record.h"
#include <stdio.h>
#include <string.h>

static int failures;
#define CHECK(ok, name) do { if (!(ok)) { \
    printf("FAIL %s\n", name); failures++; } } while (0)

static const uint8_t untraced_open[48] = {
    0x50, 0x47, 0x57, 0x43, 0x01, 0x00, 0x03, 0x05,
    0x08, 0x07, 0x06, 0x05, 0x04, 0x03, 0x02, 0x01,
    0x18, 0x17, 0x16, 0x15, 0x14, 0x13, 0x12, 0x11,
    0x28, 0x27, 0x26, 0x25, 0x24, 0x23, 0x22, 0x21,
    0x38, 0x37, 0x36, 0x35, 0x34, 0x33, 0x32, 0x31,
    0x22, 0x9c, 0x39, 0xfd, 0x00, 0x00, 0x00, 0x00,
};
static const uint8_t untraced_close[48] = {
    0x50, 0x47, 0x57, 0x43, 0x01, 0x00, 0x04, 0x00,
    0x48, 0x47, 0x46, 0x45, 0x44, 0x43, 0x42, 0x41,
    0x58, 0x57, 0x56, 0x55, 0x54, 0x53, 0x52, 0x51,
    0x28, 0x27, 0x26, 0x25, 0x24, 0x23, 0x22, 0x21,
    0x68, 0x67, 0x66, 0x65, 0x64, 0x63, 0x62, 0x61,
    0xcd, 0x14, 0xfb, 0xe8, 0x00, 0x00, 0x00, 0x00,
};
static const uint8_t a2_bucket[48] = {
    0x50, 0x47, 0x57, 0x43, 0x01, 0x00, 0x07, 0x00,
    0x78, 0x77, 0x76, 0x75, 0x74, 0x73, 0x72, 0x71,
    0x88, 0x87, 0x86, 0x85, 0x84, 0x83, 0x82, 0x81,
    0x98, 0x97, 0x96, 0x95, 0x94, 0x93, 0x92, 0x91,
    0xa8, 0xa7, 0xa6, 0xa5, 0xa4, 0xa3, 0xa2, 0xa1,
    0x77, 0xaa, 0xaf, 0xcc, 0x00, 0x00, 0x00, 0x00,
};

static int is_zero(const struct pgwt_coverage_record *r)
{
    const uint8_t *bytes = (const uint8_t *)r;
    for (size_t i = 0; i < sizeof(*r); i++)
        if (bytes[i] != 0) return 0;
    return 1;
}

static void golden(const uint8_t *bytes, const struct pgwt_coverage_record *r,
                   const char *name)
{
    uint8_t out[80];
    memset(out, 0xa5, sizeof(out));
    CHECK(pgwt_coverage_encode(r, out) == 48, name);
    CHECK(memcmp(out, bytes, 48) == 0, name);
    CHECK(out[48] == 0xa5, "48-byte payload boundary");
    CHECK(pgwt_crc32c(bytes, 40) == pgwt_cov_get_u32(bytes + 40), "golden CRC");
    struct pgwt_coverage_record back;
    CHECK(pgwt_coverage_decode(bytes, 48, &back) == PGWT_COVD_OK, name);
    CHECK(back.kind == r->kind && back.reason == r->reason &&
          back.a == r->a && back.b == r->b && back.c == r->c && back.d == r->d,
          name);
    CHECK(pgwt_coverage_encode(&back, out) == 48 && memcmp(bytes, out, 48) == 0,
          "round-trip bytes");
    for (size_t bit = 0; bit < 48 * 8; bit++) {
        memcpy(out, bytes, 48);
        out[bit / 8] ^= (uint8_t)(1u << (bit % 8));
        CHECK(pgwt_coverage_decode(out, 48, &back) != PGWT_COVD_OK && is_zero(&back),
              "corrupt A2 payload refused and zeroed");
    }
}

static void combinations(void)
{
    uint8_t raw[80], out[80];
    for (unsigned kind = 0; kind <= 255; kind++) {
        for (unsigned reason = 0; reason <= 255; reason++) {
            /* The expected wire numbers are independent of product enums. */
            int known = (kind >= 1 && kind <= 5) || kind == 7;
            int valid = known && (kind == 3 ? reason >= 1 && reason <= 5 : reason == 0);
            struct pgwt_coverage_record r = {.kind = kind, .reason = reason};
            size_t n = kind == 1 ? 80 : 48;
            size_t encoded = pgwt_coverage_encode(&r, out);
            CHECK(encoded == (valid ? n : 0),
                  kind == 8 ? "kind 8 encode refused" : "kind/reason encode matrix");
            memset(raw, 0, sizeof(raw));
            memcpy(raw, a2_bucket, 40);
            raw[6] = (uint8_t)kind;
            raw[7] = (uint8_t)reason;
            pgwt_cov_put_u32(raw + n - 8, pgwt_crc32c(raw, n - 8));
            struct pgwt_coverage_record back;
            enum pgwt_coverage_decode_rc rc = pgwt_coverage_decode(raw, n, &back);
            CHECK((rc == PGWT_COVD_OK) == valid,
                  kind == 4 && reason == 5 ? "reason 5 GAP_CLOSE refused" :
                  kind == 8 ? "kind 8 decode refused" : "kind/reason decode matrix");
            CHECK(valid || is_zero(&back), "invalid record zeroed");
        }
    }
    for (size_t n = 0; n <= sizeof(raw); n++) {
        if (n == 48) continue;
        memset(raw, 0, sizeof(raw));
        memcpy(raw, a2_bucket, 48);
        if (n >= 8)
            pgwt_cov_put_u32(raw + n - 8, pgwt_crc32c(raw, n - 8));
        struct pgwt_coverage_record back;
        CHECK(pgwt_coverage_decode(raw, n, &back) != PGWT_COVD_OK && is_zero(&back),
              "A2_BUCKET requires exactly 48 bytes");
    }
}

int main(void)
{
    golden(untraced_open, &(struct pgwt_coverage_record){
        .kind = 3, .reason = 5, .a = 0x0102030405060708ULL,
        .b = 0x1112131415161718ULL, .c = 0x2122232425262728ULL, .d = 0x3132333435363738ULL,
    }, "untraced_open golden fields and bytes");
    golden(untraced_close, &(struct pgwt_coverage_record){
        .kind = 4, .reason = 0, .a = 0x4142434445464748ULL,
        .b = 0x5152535455565758ULL, .c = 0x2122232425262728ULL, .d = 0x6162636465666768ULL,
    }, "untraced_close golden fields and bytes");
    golden(a2_bucket, &(struct pgwt_coverage_record){
        .kind = 7, .reason = 0, .a = 0x7172737475767778ULL,
        .b = 0x8182838485868788ULL, .c = 0x9192939495969798ULL, .d = 0xa1a2a3a4a5a6a7a8ULL,
    }, "a2_bucket golden fields and bytes");
    combinations();
    if (!failures) puts("PASS A2 codec: golden bytes/CRC, round trips, all 65536 kind/reason combinations, sizes");
    return failures ? 1 : 0;
}
