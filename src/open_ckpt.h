/* open_ckpt.h — the open-interval CHECKPOINT: the one piece of evidence a
 * crashed capture can leave about the intervals that were open when it died.
 *
 * #424 deliverable 2. Deliverable 1 made an unclean end DETECTABLE (the
 * closure bits in trace_closure.h); detectable is not recoverable. An
 * interval only produces a trace record when it ENDS, so a SIGKILLed daemon
 * never writes the wait every backend was sitting in — on a 30 s stall that
 * is the single most interesting fact about the capture.
 *
 * A checkpoint is exactly "the records the shutdown flush WOULD have written
 * at instant T": built by pgwt_build_open_stretch (src/open_interval.h — the
 * ONE arithmetic, deliberately not re-implemented here) and written to a
 * sidecar beside the trace. On a crash the latest checkpoint survives; on
 * recovery it is validated and carried next to the archive it belongs to.
 *
 * FOUR things this format exists to make inseparable — the adviser's point
 * that splitting them risks double counting or lost time:
 *
 *  1. IDENTITY. Every checkpoint names the trace file it describes (that
 *     file's start wall/mono stamps) and its own sequence number. A sidecar
 *     left by an earlier file, a different daemon or a different trace dir
 *     can therefore never be applied to this one. Without identity the first
 *     stale sidecar silently adds time to an unrelated capture.
 *  2. DURABLE PUBLICATION. The writer publishes the closed-record prefix
 *     FIRST, records the watermark it reached in the checkpoint, then writes
 *     the sidecar atomically (tmp → fsync → rename → fsync dir). So
 *     committed_through_ns in a surviving checkpoint is always a value the
 *     trace really reached, never a promise.
 *  3. RECOVERY. The next daemon start validates the sidecar against the file
 *     it recovers and keeps it beside the archive, or deletes it. It is NEVER
 *     materialised into the trace stream as ordinary records: the trace block
 *     layout has no field that could mark a record as reconstructed (flags
 *     are not persisted), and silently mixing reconstructed time into
 *     observed time is the exact dishonesty this issue is about.
 *  4. SUPPRESSION. A checkpointed interval may have closed normally before
 *     the crash, in which case the trace already holds its closing record and
 *     counting the checkpoint too would double count. The rule lives here,
 *     once, as a pure predicate, so the live path and the recovered path
 *     cannot drift apart.
 *
 * What a checkpoint is NOT: a complete record. Its duration is a LOWER BOUND
 * — the interval was open at least from start to the checkpoint instant, and
 * however much longer it ran before the crash is unobservable. A consumer
 * must present it as such.
 */
#ifndef PGWT_OPEN_CKPT_H
#define PGWT_OPEN_CKPT_H

#include "pg_wait_tracer.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#define PGWT_OPEN_CKPT_MAGIC    0x4F574750u   /* "PGWO" little-endian */
#define PGWT_OPEN_CKPT_VERSION  1u

/* Sanity cap on the record count (DUR-7's rule: a corrupt count must never
 * drive a large allocation). One record per backend is the physical maximum;
 * 4x MAX_BACKENDS leaves room for a bigger MAX_BACKENDS without a format
 * change and still bounds the buffer at ~200 KB. */
#define PGWT_OPEN_CKPT_MAX_RECORDS (4 * MAX_BACKENDS)

/* Sidecar file name beside the live trace, and the suffix used when recovery
 * carries a validated checkpoint next to the archive it describes. */
#define PGWT_OPEN_CKPT_NAME     "current.trace.open"
#define PGWT_OPEN_CKPT_TMP      "current.trace.open.tmp"
#define PGWT_OPEN_CKPT_SUFFIX   ".open"

/* One checkpointed open interval: byte-for-byte the closing record the
 * shutdown flush would have written, plus the in-memory flags the trace
 * block layout cannot carry (cmd_open, synthetic query id), which are part of
 * the attribution and would otherwise be lost. duration_ns is measured from
 * the interval's start to the checkpoint instant, so the interval's start is
 * timestamp_ns - duration_ns. */
struct pgwt_open_ckpt_record {
    uint64_t timestamp_ns;   /* the checkpoint instant (CLOCK_MONOTONIC) */
    uint64_t duration_ns;    /* open for at least this long — a LOWER BOUND */
    uint64_t query_id;
    uint64_t cpu_ns;         /* measured, or PGWT_CPU_NS_UNKNOWN */
    uint32_t pid;
    uint32_t event;          /* the wait event the backend is inside (0 = CPU) */
    uint32_t flags;          /* PGWT_EVENT_FLAG_* (not persisted in traces) */
    uint32_t reserved;       /* 0 */
} __attribute__((packed));

struct pgwt_open_ckpt_header {
    uint32_t magic;
    uint16_t version;
    uint16_t flags;                  /* 0 */
    /* IDENTITY: the trace file this checkpoint describes, named by the two
     * stamps in that file's own header (start_time_ns / clock_offset_ns). */
    uint64_t trace_start_wall_ns;
    uint64_t trace_start_mono_ns;
    uint64_t seq;                    /* 1, 2, 3 … within one trace file */
    uint64_t checkpoint_mono_ns;     /* when the snapshot was taken */
    uint64_t committed_through_ns;   /* writer watermark, already published */
    uint32_t num_records;
    uint32_t checksum;               /* over the record array only */
} __attribute__((packed));

struct pgwt_open_ckpt_identity {
    uint64_t trace_start_wall_ns;
    uint64_t trace_start_mono_ns;
};

/* Why a checkpoint was refused. Every refusal is named: a caller logs the
 * reason, and a test can prove each path is reachable. There is no "probably
 * fine" branch — a checkpoint that cannot be validated is not used. */
enum pgwt_open_ckpt_rc {
    PGWT_OPEN_CKPT_OK = 0,
    /* There is no checkpoint here at all. Kept distinct from every "there is
     * one but it is wrong" verdict below, because the two are different facts
     * and an operator reading a log must not have to guess which happened:
     * absent means nobody checkpointed, damaged means somebody did and we
     * cannot trust it. */
    PGWT_OPEN_CKPT_ABSENT,
    PGWT_OPEN_CKPT_IO,             /* present, could not be read into memory */
    PGWT_OPEN_CKPT_TOO_SHORT,      /* fewer bytes than a header */
    PGWT_OPEN_CKPT_BAD_MAGIC,
    PGWT_OPEN_CKPT_BAD_VERSION,
    PGWT_OPEN_CKPT_COUNT_INSANE,   /* > PGWT_OPEN_CKPT_MAX_RECORDS */
    PGWT_OPEN_CKPT_TRUNCATED,      /* header's count exceeds the bytes present */
    PGWT_OPEN_CKPT_BAD_CHECKSUM,
    PGWT_OPEN_CKPT_IDENTITY_MISMATCH,
};

static inline const char *pgwt_open_ckpt_rc_name(enum pgwt_open_ckpt_rc rc)
{
    switch (rc) {
    case PGWT_OPEN_CKPT_OK:                return "ok";
    case PGWT_OPEN_CKPT_ABSENT:            return "absent";
    case PGWT_OPEN_CKPT_IO:                return "unreadable";
    case PGWT_OPEN_CKPT_TOO_SHORT:         return "too-short";
    case PGWT_OPEN_CKPT_BAD_MAGIC:         return "bad-magic";
    case PGWT_OPEN_CKPT_BAD_VERSION:       return "bad-version";
    case PGWT_OPEN_CKPT_COUNT_INSANE:      return "insane-count";
    case PGWT_OPEN_CKPT_TRUNCATED:         return "truncated";
    case PGWT_OPEN_CKPT_BAD_CHECKSUM:      return "bad-checksum";
    case PGWT_OPEN_CKPT_IDENTITY_MISMATCH: return "identity-mismatch";
    default:                               return "unknown";
    }
}

/* CRC-32 (IEEE 802.3, reflected), computed bit-by-bit. No table, no zlib: the
 * writer links neither, and a checkpoint is a few KB written once a tick. */
static inline uint32_t pgwt_open_ckpt_crc32(const void *data, size_t len)
{
    const uint8_t *p = (const uint8_t *)data;
    uint32_t crc = 0xFFFFFFFFu;
    for (size_t i = 0; i < len; i++) {
        crc ^= p[i];
        for (int b = 0; b < 8; b++)
            crc = (crc >> 1) ^ (0xEDB88320u & (uint32_t)(-(int32_t)(crc & 1)));
    }
    return ~crc;
}

/* A validated, read-only view of a checkpoint buffer. */
struct pgwt_open_ckpt_view {
    struct pgwt_open_ckpt_header       header;
    const struct pgwt_open_ckpt_record *records;   /* points into the buffer */
    int                                 count;
};

/* Validate a checkpoint buffer and, on success, describe it.
 *
 * `expect` is the identity of the trace file the caller is about to apply
 * this checkpoint to; pass NULL only when there is no file to check against
 * (inspection tools). A NULL `expect` does NOT make the checkpoint trusted —
 * it makes it unattributed, which a caller must not then count. */
static inline enum pgwt_open_ckpt_rc
pgwt_open_ckpt_parse(const void *buf, size_t len,
                     const struct pgwt_open_ckpt_identity *expect,
                     struct pgwt_open_ckpt_view *out)
{
    if (out) {
        memset(&out->header, 0, sizeof(out->header));
        out->records = NULL;
        out->count = 0;
    }
    if (!buf || len < sizeof(struct pgwt_open_ckpt_header))
        return PGWT_OPEN_CKPT_TOO_SHORT;

    struct pgwt_open_ckpt_header h;
    memcpy(&h, buf, sizeof(h));
    if (h.magic != PGWT_OPEN_CKPT_MAGIC)
        return PGWT_OPEN_CKPT_BAD_MAGIC;
    if (h.version != PGWT_OPEN_CKPT_VERSION)
        return PGWT_OPEN_CKPT_BAD_VERSION;
    if (h.num_records > (uint32_t)PGWT_OPEN_CKPT_MAX_RECORDS)
        return PGWT_OPEN_CKPT_COUNT_INSANE;

    size_t need = sizeof(h) +
        (size_t)h.num_records * sizeof(struct pgwt_open_ckpt_record);
    if (len < need)
        return PGWT_OPEN_CKPT_TRUNCATED;

    const struct pgwt_open_ckpt_record *recs =
        (const struct pgwt_open_ckpt_record *)((const uint8_t *)buf + sizeof(h));
    uint32_t crc = pgwt_open_ckpt_crc32(
        recs, (size_t)h.num_records * sizeof(*recs));
    if (crc != h.checksum)
        return PGWT_OPEN_CKPT_BAD_CHECKSUM;

    /* Identity LAST, so a mismatch is reported as a mismatch and not hidden
     * behind a checksum failure — the two mean very different things to an
     * operator (a stale sidecar vs. a damaged one). */
    if (expect &&
        (h.trace_start_wall_ns != expect->trace_start_wall_ns ||
         h.trace_start_mono_ns != expect->trace_start_mono_ns))
        return PGWT_OPEN_CKPT_IDENTITY_MISMATCH;

    if (out) {
        out->header = h;
        out->records = recs;
        out->count = (int)h.num_records;
    }
    return PGWT_OPEN_CKPT_OK;
}

/* The interval's start instant. */
static inline uint64_t
pgwt_open_ckpt_start_ns(const struct pgwt_open_ckpt_record *r)
{
    return r->timestamp_ns - r->duration_ns;
}

/* SUPPRESSION (point 4). Does this trace record make the checkpointed
 * interval redundant?
 *
 * Per pid, intervals are sequential: a backend is inside exactly one wait or
 * on-CPU stretch at a time, and each closing record ends the stretch that was
 * open before it. So a closing record for the SAME pid whose end lands after
 * the checkpointed interval's start means that stretch did close and WAS
 * recorded — counting the checkpoint as well would double count the same
 * nanoseconds.
 *
 * The comparison is on the interval START, not on the checkpoint instant: the
 * closing record for a stretch open at the checkpoint ends later than the
 * checkpoint, so comparing against the checkpoint instant would suppress
 * nothing and every such interval would be counted twice.
 *
 * Equality is NOT suppression: a closing record ending exactly at the
 * interval's start is the record of the PREVIOUS stretch (intervals are
 * half-open, [start, end)), and suppressing on it would silently drop every
 * genuinely-open interval whose predecessor closed on the same nanosecond. */
static inline bool
pgwt_open_ckpt_suppressed_by(const struct pgwt_open_ckpt_record *ckpt,
                             const struct pgwt_trace_event *closing)
{
    if (!ckpt || !closing)
        return false;
    if (closing->pid != ckpt->pid)
        return false;
    return closing->timestamp_ns > pgwt_open_ckpt_start_ns(ckpt);
}

#endif /* PGWT_OPEN_CKPT_H */
