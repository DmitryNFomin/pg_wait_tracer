/* timeline_format.h — on-disk format of timeline mode (#1006 unit 1).
 *
 * THE FROZEN CONTRACT. docs/TIMELINE_FORMAT.md says the same in prose; the
 * two must agree. A file is a sequence of segments `timeline-NNNN.pgwtl` in
 * the trace directory. Each segment is one 64-byte header followed by
 * 64-byte records; the last record of a cleanly closed segment is a STATS
 * trailer. All integers are little-endian; every time is CLOCK_MONOTONIC ns
 * (the clock of bpf_ktime_get_ns) unless the field name says wall.
 *
 * Reader rules (they are what makes the format extensible):
 *   - refuse a header whose magic or version is unknown;
 *   - step by the header's rec_size, never by sizeof;
 *   - SKIP a record of an unknown kind (do not reject the file);
 *   - IGNORE unknown flag bits;
 *   - a segment without a STATS trailer is TRUNCATED: its loss count is
 *     unknown, never zero;
 *   - a ZERO_MARKER wait has end_ns == start_ns, no duration, and is never
 *     summed into wait time (it is counted separately).
 *
 * Userspace only (BPF never writes this format; see timeline_wire.h).
 */
#ifndef PGWT_TIMELINE_FORMAT_H
#define PGWT_TIMELINE_FORMAT_H

#include <stdint.h>
#include <stddef.h>

#define PGWT_TL_MAGIC          "PGWTTL01"   /* 8 bytes, no terminator */
#define PGWT_TL_MAGIC_LEN      8
#define PGWT_TL_VERSION        1
#define PGWT_TL_HEADER_SIZE    64
#define PGWT_TL_REC_SIZE       64

#define PGWT_TL_FILE_PREFIX    "timeline-"
#define PGWT_TL_FILE_SUFFIX    ".pgwtl"     /* timeline-NNNN.pgwtl */
#define PGWT_TL_SUMMARY_FILE   "timeline-summary.json"
#define PGWT_TL_SEGMENT_BYTES  (256ULL * 1024 * 1024)  /* rotation size */

/* Not measured (on_cpu_ns, runq_ns). Same value as PGWT_CPU_NS_UNKNOWN in
 * pg_wait_tracer.h; never 0 for "not measured". */
#define PGWT_TL_NS_UNKNOWN     0xFFFFFFFFFFFFFFFFULL

/* ── header.caps ──────────────────────────────────────────────── */
/* The WaitEventSetWait uprobe was attached when the segment was opened. */
#define PGWT_TL_CAP_MARKER_PROBE_ATTACHED  0x00000001u
/* Bits 1-2: where query ids come from. */
#define PGWT_TL_CAP_QID_SOURCE_SHIFT       1
#define PGWT_TL_CAP_QID_SOURCE_MASK        0x00000006u
#define PGWT_TL_QID_SOURCE_NONE            0u   /* no query ids */
#define PGWT_TL_QID_SOURCE_ST_QUERY_ID     1u   /* PgBackendStatus.st_query_id */
#define PGWT_TL_QID_SOURCE_PG13_RESERVED   2u   /* PG13 synthetic ids (unit 3) */
#define PGWT_TL_CAP_QID_SOURCE(caps) \
    (((caps) & PGWT_TL_CAP_QID_SOURCE_MASK) >> PGWT_TL_CAP_QID_SOURCE_SHIFT)
/* on_cpu_ns / runq_ns carry measured values (unit 3). 0 in unit 1. */
#define PGWT_TL_CAP_CPU_SPLIT_PRESENT      0x00000008u

/* ── record.kind ──────────────────────────────────────────────── */
enum pgwt_tl_kind {
    PGWT_TL_KIND_WAIT        = 0,    /* one wait of one process */
    PGWT_TL_KIND_NO_WAIT     = 1,    /* gap with no wait event (unit 3) */
    PGWT_TL_KIND_PROBE_STATE = 2,    /* global (pid 0): marker probe off span */
    PGWT_TL_KIND_LOSS        = 3,    /* span of one pid with lost raw records */
    PGWT_TL_KIND_BACKEND     = 4,    /* backend lifetime (reserved) */
    PGWT_TL_KIND_STATS       = 255,  /* segment trailer, see pgwt_tl_stats */
};

/* ── record.flags ─────────────────────────────────────────────── */
/* Assembled without the WaitEventSetWait marker (unit 1: attach failed;
 * unit 2: the probe was switched off). On PROBE_STATE: the probe was off. */
#define PGWT_TL_F_MARKER_OFF          0x0001u
/* A marker with no call of its word: end_ns == start_ns, n_syscalls 0, no
 * duration, never summed into wait time. */
#define PGWT_TL_F_ZERO_MARKER         0x0002u
/* The wait was already in progress when the tracer attached: start_ns is
 * the attach time, not the wait's start. */
#define PGWT_TL_F_TRUNC_START         0x0004u
/* The wait was still in progress at stop (or at the process's exit):
 * end_ns is that time, not the wait's end. */
#define PGWT_TL_F_TRUNC_END           0x0008u
/* No query id could be read (PG13 until unit 3, or a failed read):
 * query_id is 0 and means nothing. */
#define PGWT_TL_F_QID_UNAVAILABLE     0x0010u
/* Inside a command whose query id is not set: query_id is 0. */
#define PGWT_TL_F_IN_CMD_NO_QID       0x0020u
/* query_id is a PG13 synthetic id (reserved, unit 3). */
#define PGWT_TL_F_PG13_SYNTH_QID      0x0040u
/* Wait from the built-in LWLock tracepoints, not a system call (reserved,
 * unit 4). */
#define PGWT_TL_F_LWLOCK_TRACEPOINT   0x0080u

/* ── header: 64 bytes, little-endian ──────────────────────────── */
struct pgwt_tl_header {
    char     magic[8];        /* PGWT_TL_MAGIC */
    uint16_t version;         /* PGWT_TL_VERSION */
    uint16_t rec_size;        /* PGWT_TL_REC_SIZE */
    uint32_t pg_version;      /* server_version_num (e.g. 170004); major*10000
                               * when only the major is known; 0 unknown */
    uint32_t postmaster_pid;
    uint32_t caps;            /* PGWT_TL_CAP_* */
    uint64_t mono_start_ns;   /* CLOCK_MONOTONIC at capture start */
    uint64_t wall_start_ns;   /* CLOCK_REALTIME at the same instant */
    uint8_t  boot_id[16];     /* /proc/sys/kernel/random/boot_id, binary;
                               * monotonic times compare only within one */
    uint32_t segment_index;   /* NNNN of timeline-NNNN.pgwtl */
    uint32_t reserved;        /* 0 */
};

/* ── record: 64 bytes, little-endian ──────────────────────────── */
struct pgwt_tl_record {
    uint64_t start_ns;
    uint64_t end_ns;          /* == start_ns for ZERO_MARKER */
    uint64_t query_id;        /* 0 = none (see QID flags) */
    uint64_t on_cpu_ns;       /* PGWT_TL_NS_UNKNOWN unless CPU_SPLIT_PRESENT */
    uint64_t runq_ns;         /* PGWT_TL_NS_UNKNOWN unless CPU_SPLIT_PRESENT */
    uint32_t pid;             /* 0 for global kinds (PROBE_STATE) */
    uint32_t wait_event_info; /* WAIT: the event; 0 for other kinds */
    uint32_t n_syscalls;      /* WAIT: system calls merged into it */
    uint16_t kind;            /* enum pgwt_tl_kind */
    uint16_t flags;           /* PGWT_TL_F_* */
    uint32_t reserved[2];     /* 0 */
};

/* STATS trailer: the same 64 bytes, with kind (offset 52) and flags
 * (offset 54) where every record has them. One per segment, last. */
struct pgwt_tl_stats {
    uint64_t records;         /* records in this segment before the trailer */
    uint64_t lost_raw_total;  /* raw wire records lost since capture start
                               * (ring reserve failures), cumulative */
    uint64_t end_ns;          /* CLOCK_MONOTONIC when the trailer was written */
    uint32_t n_wait;          /* per-kind counts in this segment */
    uint32_t n_zero_marker;   /*   (WAIT records with ZERO_MARKER) */
    uint32_t n_no_wait;
    uint32_t n_probe_state;
    uint32_t n_loss;
    uint32_t n_backend;
    uint32_t n_other;         /* records of any other kind */
    uint16_t kind;            /* PGWT_TL_KIND_STATS */
    uint16_t flags;           /* 0 */
    uint32_t stats_flags;     /* PGWT_TL_STATS_* */
    uint32_t reserved;        /* 0 */
};
#define PGWT_TL_STATS_FINAL    0x1u  /* capture stopped cleanly after this */
#define PGWT_TL_STATS_ROTATED  0x2u  /* segment closed by rotation */

/* The structs are written and read as-is: the layout is little-endian
 * only on a little-endian host (x86_64, aarch64). */
_Static_assert(__BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__,
               "timeline format structs assume a little-endian host");

#define PGWT_TL_OFFSET(type, field, value) \
    _Static_assert(offsetof(struct type, field) == (value), \
                   #type "." #field " offset")
_Static_assert(sizeof(struct pgwt_tl_header) == PGWT_TL_HEADER_SIZE, "timeline header size");
PGWT_TL_OFFSET(pgwt_tl_header, magic, 0);
PGWT_TL_OFFSET(pgwt_tl_header, version, 8);
PGWT_TL_OFFSET(pgwt_tl_header, rec_size, 10);
PGWT_TL_OFFSET(pgwt_tl_header, pg_version, 12);
PGWT_TL_OFFSET(pgwt_tl_header, postmaster_pid, 16);
PGWT_TL_OFFSET(pgwt_tl_header, caps, 20);
PGWT_TL_OFFSET(pgwt_tl_header, mono_start_ns, 24);
PGWT_TL_OFFSET(pgwt_tl_header, wall_start_ns, 32);
PGWT_TL_OFFSET(pgwt_tl_header, boot_id, 40);
PGWT_TL_OFFSET(pgwt_tl_header, segment_index, 56);
PGWT_TL_OFFSET(pgwt_tl_header, reserved, 60);
_Static_assert(sizeof(struct pgwt_tl_record) == PGWT_TL_REC_SIZE, "timeline record size");
PGWT_TL_OFFSET(pgwt_tl_record, start_ns, 0);
PGWT_TL_OFFSET(pgwt_tl_record, end_ns, 8);
PGWT_TL_OFFSET(pgwt_tl_record, query_id, 16);
PGWT_TL_OFFSET(pgwt_tl_record, on_cpu_ns, 24);
PGWT_TL_OFFSET(pgwt_tl_record, runq_ns, 32);
PGWT_TL_OFFSET(pgwt_tl_record, pid, 40);
PGWT_TL_OFFSET(pgwt_tl_record, wait_event_info, 44);
PGWT_TL_OFFSET(pgwt_tl_record, n_syscalls, 48);
PGWT_TL_OFFSET(pgwt_tl_record, kind, 52);
PGWT_TL_OFFSET(pgwt_tl_record, flags, 54);
PGWT_TL_OFFSET(pgwt_tl_record, reserved, 56);
_Static_assert(sizeof(struct pgwt_tl_stats) == PGWT_TL_REC_SIZE, "timeline STATS size");
PGWT_TL_OFFSET(pgwt_tl_stats, records, 0);
PGWT_TL_OFFSET(pgwt_tl_stats, lost_raw_total, 8);
PGWT_TL_OFFSET(pgwt_tl_stats, end_ns, 16);
PGWT_TL_OFFSET(pgwt_tl_stats, n_wait, 24);
PGWT_TL_OFFSET(pgwt_tl_stats, n_zero_marker, 28);
PGWT_TL_OFFSET(pgwt_tl_stats, n_no_wait, 32);
PGWT_TL_OFFSET(pgwt_tl_stats, n_probe_state, 36);
PGWT_TL_OFFSET(pgwt_tl_stats, n_loss, 40);
PGWT_TL_OFFSET(pgwt_tl_stats, n_backend, 44);
PGWT_TL_OFFSET(pgwt_tl_stats, n_other, 48);
PGWT_TL_OFFSET(pgwt_tl_stats, kind, 52);
PGWT_TL_OFFSET(pgwt_tl_stats, flags, 54);
PGWT_TL_OFFSET(pgwt_tl_stats, stats_flags, 56);
PGWT_TL_OFFSET(pgwt_tl_stats, reserved, 60);
#undef PGWT_TL_OFFSET

#endif /* PGWT_TIMELINE_FORMAT_H */
