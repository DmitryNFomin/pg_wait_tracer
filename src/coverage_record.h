/* coverage_record.h — the trace v6 COVERAGE block payload (block type 3).
 *
 * Coverage label spec r2 §1 (Refs #947, #871). A COVERAGE block is a raw
 * (never compressed) payload behind an ordinary v4-layout block header:
 *
 *   header  first_ts = last_ts = observation (mono at write), num_events = 1,
 *           compressed_size = uncompressed_size = payload size,
 *           sample_period_ns = 0, v4 extension (max weight) = 0.
 *   payload little-endian:
 *           u32 magic "PGWC", u16 schema = 1, u8 kind, u8 reason,
 *           u64 a, u64 b, u64 c, u64 d,
 *           BOOT only: u8 boot_nonce[16], u8 kernel_boot_id[16],
 *           u32 crc32c(every byte before it), u32 0.
 *           48 bytes; BOOT 80 bytes.
 *
 *   kind          a           b            c               d
 *   1 BOOT        boot_start  file_no      postmaster      mode
 *                                          start (wall,
 *                                          0 = unknown)
 *   2 WATERMARK   W           floor_db     floor_idle      0
 *   3 GAP_OPEN    start       0            gap_id          detection
 *   4 GAP_CLOSE   end         0            gap_id          0
 *   5 SEAL        W_final     0            0               0
 *   3 GAP_OPEN    fork_ns     pid          gap_id          detection_ns
 *                 (reason 5 UNTRACED_LIFETIME)
 *   4 GAP_CLOSE   exit_ns     g            gap_id          receive_ns
 *                 (reason 0, matching an untraced lifetime)
 *   7 A2_BUCKET   J_k.start   J_k.end      N_k             U_k
 *
 * Every time except BOOT's postmaster start is CLOCK_MONOTONIC (the clock the
 * trace's records use). Header-only and pure: the writer (event_writer.c),
 * the reader and the guard tests share THIS encoder and decoder, so there is
 * exactly one definition of the bytes. */
#ifndef PGWT_COVERAGE_RECORD_H
#define PGWT_COVERAGE_RECORD_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#define PGWT_COVERAGE_MAGIC   0x43574750u   /* "PGWC" little-endian */
#define PGWT_COVERAGE_SCHEMA  1u

#define PGWT_COVERAGE_PAYLOAD_SIZE       48u
#define PGWT_COVERAGE_BOOT_PAYLOAD_SIZE  80u

enum pgwt_coverage_kind {
    PGWT_COVK_BOOT      = 1,
    PGWT_COVK_WATERMARK = 2,
    PGWT_COVK_GAP_OPEN  = 3,
    PGWT_COVK_GAP_CLOSE = 4,
    PGWT_COVK_SEAL      = 5,
    /* 6 is reserved; there is no A2_POLICY record. */
    PGWT_COVK_A2_BUCKET = 7,
};

/* GAP_OPEN reasons (the u8 `reason` byte; 0 on every other kind). */
enum pgwt_coverage_gap_reason {
    PGWT_COVR_NONE             = 0,
    PGWT_COVR_PAUSE            = 1,
    PGWT_COVR_DROP             = 2,   /* the counter vector moved */
    PGWT_COVR_REVOKED          = 3,
    PGWT_COVR_COUNTERS_UNKNOWN = 4,   /* the counter vector could not be read */
    PGWT_COVR_UNTRACED_LIFETIME = 5,
};

/* BOOT `d`: the capture mode. Low byte = enum pgwt_mode (src/provider.h);
 * the bits above name the two modes that write no WATERMARK on master. */
#define PGWT_COVERAGE_MODE_LIGHTWEIGHT  0x100u
#define PGWT_COVERAGE_MODE_EXACT_PERF   0x200u

struct pgwt_coverage_record {
    uint8_t  kind;
    uint8_t  reason;
    uint64_t a, b, c, d;
    uint8_t  boot_nonce[16];      /* BOOT only */
    uint8_t  kernel_boot_id[16];  /* BOOT only; all zero = unknown */
};

enum pgwt_coverage_decode_rc {
    PGWT_COVD_OK = 0,
    PGWT_COVD_SHORT,        /* fewer bytes than the kind needs */
    PGWT_COVD_BAD_MAGIC,
    PGWT_COVD_BAD_SCHEMA,
    PGWT_COVD_BAD_KIND,
    PGWT_COVD_BAD_SIZE,     /* size does not match the kind */
    PGWT_COVD_BAD_CRC,
    PGWT_COVD_BAD_PAD,      /* trailing u32 not zero */
    PGWT_COVD_BAD_REASON,   /* unknown reason or reason on the wrong kind */
};

/* CRC-32C (Castagnoli, reflected polynomial 0x82F63B78, init and final xor
 * 0xFFFFFFFF). Check value: "123456789" -> 0xE3069283. Bitwise: a coverage
 * payload is at most 76 bytes, written about once a second. */
static inline uint32_t pgwt_crc32c(const void *data, size_t len)
{
    const uint8_t *p = (const uint8_t *)data;
    uint32_t crc = 0xFFFFFFFFu;
    for (size_t i = 0; i < len; i++) {
        crc ^= p[i];
        for (int k = 0; k < 8; k++)
            crc = (crc >> 1) ^ (0x82F63B78u & (0u - (crc & 1u)));
    }
    return crc ^ 0xFFFFFFFFu;
}

static inline void pgwt_cov_put_u16(uint8_t *p, uint16_t v)
{
    p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8);
}
static inline void pgwt_cov_put_u32(uint8_t *p, uint32_t v)
{
    for (int i = 0; i < 4; i++) p[i] = (uint8_t)(v >> (8 * i));
}
static inline void pgwt_cov_put_u64(uint8_t *p, uint64_t v)
{
    for (int i = 0; i < 8; i++) p[i] = (uint8_t)(v >> (8 * i));
}
static inline uint16_t pgwt_cov_get_u16(const uint8_t *p)
{
    return (uint16_t)(p[0] | (p[1] << 8));
}
static inline uint32_t pgwt_cov_get_u32(const uint8_t *p)
{
    uint32_t v = 0;
    for (int i = 3; i >= 0; i--) v = (v << 8) | p[i];
    return v;
}
static inline uint64_t pgwt_cov_get_u64(const uint8_t *p)
{
    uint64_t v = 0;
    for (int i = 7; i >= 0; i--) v = (v << 8) | p[i];
    return v;
}

static inline size_t pgwt_coverage_payload_size(uint8_t kind)
{
    return kind == PGWT_COVK_BOOT ? PGWT_COVERAGE_BOOT_PAYLOAD_SIZE
                                  : PGWT_COVERAGE_PAYLOAD_SIZE;
}

static inline bool pgwt_coverage_kind_valid(uint8_t kind)
{
    return (kind >= PGWT_COVK_BOOT && kind <= PGWT_COVK_SEAL) ||
           kind == PGWT_COVK_A2_BUCKET;
}

static inline bool pgwt_coverage_reason_valid(uint8_t kind, uint8_t reason)
{
    return kind == PGWT_COVK_GAP_OPEN
         ? reason >= PGWT_COVR_PAUSE && reason <= PGWT_COVR_UNTRACED_LIFETIME
         : reason == PGWT_COVR_NONE;
}

/* Encode `r` into `out` (at least PGWT_COVERAGE_BOOT_PAYLOAD_SIZE bytes).
 * Returns the payload size, or 0 for an invalid kind/reason. Explicit byte order,
 * never a struct copy: the bytes are the format, the struct is not. */
static inline size_t pgwt_coverage_encode(const struct pgwt_coverage_record *r,
                                          uint8_t *out)
{
    if (!pgwt_coverage_kind_valid(r->kind) ||
        !pgwt_coverage_reason_valid(r->kind, r->reason))
        return 0;
    size_t n = pgwt_coverage_payload_size(r->kind);
    memset(out, 0, n);
    pgwt_cov_put_u32(out + 0, PGWT_COVERAGE_MAGIC);
    pgwt_cov_put_u16(out + 4, (uint16_t)PGWT_COVERAGE_SCHEMA);
    out[6] = r->kind;
    out[7] = r->reason;
    pgwt_cov_put_u64(out + 8,  r->a);
    pgwt_cov_put_u64(out + 16, r->b);
    pgwt_cov_put_u64(out + 24, r->c);
    pgwt_cov_put_u64(out + 32, r->d);
    size_t off = 40;
    if (r->kind == PGWT_COVK_BOOT) {
        memcpy(out + off, r->boot_nonce, 16);
        memcpy(out + off + 16, r->kernel_boot_id, 16);
        off += 32;
    }
    pgwt_cov_put_u32(out + off, pgwt_crc32c(out, off));
    pgwt_cov_put_u32(out + off + 4, 0);
    return n;
}

/* Decode and verify one payload of exactly `len` bytes. On anything but
 * PGWT_COVD_OK *r is zeroed: a record that does not verify carries no
 * facts (the verdict turns it into a hole). */
static inline enum pgwt_coverage_decode_rc
pgwt_coverage_decode(const uint8_t *buf, size_t len,
                     struct pgwt_coverage_record *r)
{
    memset(r, 0, sizeof(*r));
    if (len < PGWT_COVERAGE_PAYLOAD_SIZE)
        return PGWT_COVD_SHORT;
    if (pgwt_cov_get_u32(buf) != PGWT_COVERAGE_MAGIC)
        return PGWT_COVD_BAD_MAGIC;
    if (pgwt_cov_get_u16(buf + 4) != PGWT_COVERAGE_SCHEMA)
        return PGWT_COVD_BAD_SCHEMA;
    uint8_t kind = buf[6];
    if (!pgwt_coverage_kind_valid(kind))
        return PGWT_COVD_BAD_KIND;
    size_t n = pgwt_coverage_payload_size(kind);
    if (len != n)
        return len < n ? PGWT_COVD_SHORT : PGWT_COVD_BAD_SIZE;
    size_t off = n - 8;
    if (pgwt_cov_get_u32(buf + off) != pgwt_crc32c(buf, off))
        return PGWT_COVD_BAD_CRC;
    if (pgwt_cov_get_u32(buf + off + 4) != 0)
        return PGWT_COVD_BAD_PAD;
    if (!pgwt_coverage_reason_valid(kind, buf[7]))
        return PGWT_COVD_BAD_REASON;
    r->kind = kind;
    r->reason = buf[7];
    r->a = pgwt_cov_get_u64(buf + 8);
    r->b = pgwt_cov_get_u64(buf + 16);
    r->c = pgwt_cov_get_u64(buf + 24);
    r->d = pgwt_cov_get_u64(buf + 32);
    if (kind == PGWT_COVK_BOOT) {
        memcpy(r->boot_nonce, buf + 40, 16);
        memcpy(r->kernel_boot_id, buf + 56, 16);
    }
    return PGWT_COVD_OK;
}

#endif /* PGWT_COVERAGE_RECORD_H */
