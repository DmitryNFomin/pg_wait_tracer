/* summary_writer.h — Per-second summary file writer
 *
 * Accumulates raw trace events into per-second snapshots containing:
 * - Time model (11 wait classes)
 * - Per-event stats with histograms
 * - Per-session (PID) stats
 * - Per-query stats
 *
 * Writes .summary.lz4 files alongside .trace.lz4 files.
 * Server uses summaries for time ranges >= 120s (instant response). */
#ifndef PGWT_SUMMARY_WRITER_H
#define PGWT_SUMMARY_WRITER_H

#include "pg_wait_tracer.h"
#include "query_attr.h"

/* Import class index definitions from compute.h without pulling full header */
#ifndef PGWT_NUM_CLASSES
#define PGWT_NUM_CLASSES 11

#define PGWT_CLASS_CPU       0
#define PGWT_CLASS_IO        1
#define PGWT_CLASS_LOCK      2
#define PGWT_CLASS_LWLOCK    3
#define PGWT_CLASS_IPC       4
#define PGWT_CLASS_CLIENT    5
#define PGWT_CLASS_TIMEOUT   6
#define PGWT_CLASS_BUFFERPIN 7
#define PGWT_CLASS_ACTIVITY  8
#define PGWT_CLASS_EXTENSION 9
#define PGWT_CLASS_UNKNOWN   10
#endif

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include <stdio.h>
#include <sys/types.h>

/* ── On-disk format constants ─────────────────────────────── */

#define PGWT_SUMMARY_MAGIC    0x53574750   /* "PGWS" little-endian */
#define PGWT_SUMMARY_VERSION  2

/* Limits for per-second accumulator hash tables */
#define SUMMARY_MAX_EVENTS    1024
#define SUMMARY_MAX_SESSIONS  MAX_BACKENDS   /* 1024 */
#define SUMMARY_MAX_QUERIES   2048

/* ── Per-second accumulator ───────────────────────────────── */

struct pgwt_summary_event {
    uint32_t event_id;         /* wait_event_info, 0 = unused slot */
    uint64_t count;
    uint64_t total_ns;
    uint64_t max_ns;
    uint64_t histogram[HISTOGRAM_BUCKETS];
};

struct pgwt_summary_session {
    uint32_t pid;              /* 0 = unused slot */
    uint64_t db_time_ns;
    uint64_t cpu_ns;
    uint32_t top_wait_id;      /* event_id of highest-duration wait */
    uint64_t top_wait_ns;
};

#define SUMMARY_QUERY_TOP_EVENTS 8

struct pgwt_summary_query_event {
    uint32_t event_id;
    uint64_t count;
    uint64_t total_ns;
};

struct pgwt_summary_query {
    uint64_t query_id;         /* 0 = unused slot */
    uint64_t count;
    uint64_t total_ns;
    uint32_t top_wait_id;
    uint64_t top_wait_ns;
    /* v2: per-class time breakdown + top events */
    uint64_t class_ns[PGWT_NUM_CLASSES];
    struct pgwt_summary_query_event top_events[SUMMARY_QUERY_TOP_EVENTS];
    int      num_top_events;
};

struct pgwt_summary_accum {
    uint64_t second_wall_ns;   /* wall-clock second boundary (rounded down) */
    uint64_t second_mono_ns;   /* monotonic second boundary */
    uint32_t total_events;     /* total events accumulated this second */

    /* Time model (system-wide per-class totals) */
    uint64_t class_ns[PGWT_NUM_CLASSES];

    /* Per-event hash table (open addressing) */
    struct pgwt_summary_event events[SUMMARY_MAX_EVENTS];
    int num_events;            /* count of occupied slots */

    /* Per-session hash table (open addressing) */
    struct pgwt_summary_session sessions[SUMMARY_MAX_SESSIONS];
    int num_sessions;

    /* Per-query hash table (open addressing) */
    struct pgwt_summary_query queries[SUMMARY_MAX_QUERIES];
    int num_queries;
};

/* ── On-disk record header (precedes compressed payload) ──── */

struct pgwt_summary_block_header {
    uint64_t wall_ns;          /* wall-clock timestamp of this second */
    uint32_t num_events;       /* distinct event types */
    uint16_t num_sessions;     /* distinct PIDs */
    uint16_t num_queries;      /* distinct query_ids */
    uint32_t compressed_size;
    uint32_t uncompressed_size;
} __attribute__((packed));

/* ── Writer state ─────────────────────────────────────────── */

#define SUMMARY_QATTR_SLOTS 1024
struct pgwt_summary_qattr_slot {
    uint32_t pid;                 /* 0 = empty */
    struct pgwt_qattr_pid q;
};

struct pgwt_summary_writer {
    /* Configuration */
    char          trace_dir[256];
    int           retention_hours;
    gid_t         trace_gid;
    bool          enabled;
    bool          verbose;

    /* Clock offset: wall_ns - mono_ns at file creation */
    uint64_t      clock_offset_wall_ns;
    uint64_t      clock_offset_mono_ns;

    /* Current file */
    FILE         *fp;
    char          current_path[512];
    int           current_hour;

    /* Block index (footer) */
    struct pgwt_block_index_entry *block_index;
    int           num_blocks;
    int           block_index_cap;

    /* Per-second accumulator */
    struct pgwt_summary_accum accum;
    bool          accum_active;    /* has any events been accumulated? */

    /* Scratch buffers */
    uint8_t      *encode_buf;
    size_t        encode_buf_size;
    uint8_t      *compress_buf;
    size_t        compress_buf_size;

    /* Stats */
    uint64_t      total_records_written;
    uint64_t      total_bytes_written;

    /* #128 deferred per-query attribution (query_attr.h), per pid; heap
     * (SUMMARY_QATTR_SLOTS entries, open addressing, reclaimed on backend
     * exit), NULL = disabled. The two counters are exported by the control
     * socket (summary_query_attr_table_full_total,
     * summary_query_unattributed_ns_total): records that kept their
     * emission-time id because the table was full, and deferred time no
     * command claimed (the summary has no unattributed bucket). */
    struct pgwt_summary_qattr_slot *qattr;
    uint64_t      qattr_table_full_total;
    uint64_t      qattr_unattributed_ns_total;
};

/* ── Public API ───────────────────────────────────────────── */

int  pgwt_summary_writer_init(struct pgwt_summary_writer *w,
                               const char *trace_dir,
                               int retention_hours,
                               const char *group_name);

/* Push a raw trace event. Handles second-boundary detection and flushing. */
int  pgwt_summary_push_event(struct pgwt_summary_writer *w,
                              const struct pgwt_trace_event *evt);

/* Force-flush current accumulator (e.g. on rotation/close). */
int  pgwt_summary_flush(struct pgwt_summary_writer *w);

/* Check for hourly rotation. Call from timer handler. */
int  pgwt_summary_check_rotation(struct pgwt_summary_writer *w);

/* Close current file (flush + write footer). */
int  pgwt_summary_close(struct pgwt_summary_writer *w);

/* Delete old .summary.lz4 files beyond retention. */
int  pgwt_summary_cleanup_old_files(struct pgwt_summary_writer *w);

/* Free allocated buffers. */
void pgwt_summary_destroy(struct pgwt_summary_writer *w);

/* ── Serialization (exposed for reader/testing) ───────────── */

/* Serialize accumulator to buffer. Returns bytes written. */
size_t pgwt_summary_serialize(const struct pgwt_summary_accum *acc,
                               uint8_t *out, size_t out_size);

/* Deserialize buffer into accumulator. Returns 0 on success. */
int pgwt_summary_deserialize(const uint8_t *in, size_t in_size,
                              struct pgwt_summary_accum *acc,
                              int version);

#endif /* PGWT_SUMMARY_WRITER_H */
