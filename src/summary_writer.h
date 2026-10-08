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
/* v3 (2026-10-06, the timer-sleep/idle accounting change): `class_ns`,
 * `queries[].class_ns`, `queries[].count`, `queries[].total_ns`,
 * `sessions[].top_wait_*` and `queries[].top_wait_*` now EXCLUDE idle events
 * (pgwt_is_idle_event -- Activity, Client:ClientRead and the Timeout pacing
 * sleeps). `events[]` is unchanged and still carries EVERY event including the
 * idle ones, which is where every read path gets both the idle totals and the
 * named Idle sub-rows from.
 *
 * Why the version had to move: v1/v2 precomputed those per-second totals at
 * WRITE time under the OLD rule, so a trace directory holding v2 seconds and
 * v3 seconds would BLEND two accounting rules inside one window and report a
 * DB Time that is neither. The reader therefore refuses v1/v2 for computation
 * (src/summary_reader.c) and pgwt-server preflights the window so a refusal
 * becomes a RAW recompute, never a plausible-looking partial answer
 * (should_use_summaries / pgwt_summaries_window_current). Startup recovery
 * still archives an intact older file rather than calling it corrupt. */
/* v4 (2026-10-07): the SAME change as v3 plus the exact idle scalars --
 * `pgwt_summary_query::idle_ns` (8 bytes per query) and the record trailer
 * (`idle_ns` + `events_overflow`, 12 bytes).
 *
 * Why this is a second bump rather than an edit to v3: v3 had already been
 * written to disk by an earlier build of this branch, with the OLD layout,
 * under the same version number. A reader that assumes the new bytes exist
 * mis-parses those files -- and it fails in the shape v3 was introduced to
 * close, because the file-level preflight only inspects the header, approves
 * it, and then the visitor silently SKIPS the blocks that fail to decode,
 * yielding a plausible partial window with no error and no raw fallback.
 * "No such file should exist anywhere" is a weaker guarantee than "an old
 * file is refused", so the version moves and the preflight refuses v3. */
/* v5 (2026-10-08, aggregate-vs-raw cross-check fix #317): ONE content change
 * at the writer, no layout change. `queries[].top_events[]` now also carries
 * event 0 (CPU), which accum_query_add's `old_ev != 0` guard had been
 * dropping -- so the Events tab drilled into a single query over a >= 120 s
 * window was missing that query's whole CPU* row and its db_time_ms was short
 * by the query's entire CPU time (measured raw 7002.426 ms vs summary
 * 2949.426 ms, -57.9%).
 *
 * WHY THE VERSION HAS TO MOVE even though the byte layout is identical: the
 * version is what tells a reader which ACCOUNTING RULE the precomputed
 * per-second totals were written under -- that is the whole reason v3 and v4
 * exist. A v4 second and a v5 second answer "what is in this query's event
 * list" differently, so a window holding both would report a per-query event
 * breakdown that is neither: the v4 seconds contribute no CPU row and the v5
 * seconds do, and the CPU* row would silently carry only part of the window.
 *
 * SCOPE, stated because it is easy to over-read: v5 says nothing about
 * io_worker records. The summary records still CONTAIN io_worker time (see
 * the N7 hole in tests/test_agg_raw_crosscheck.c and issue #315) -- excluding
 * it needs a daemon-side fix that does not exist yet, and claiming otherwise
 * here would be a false contract that a later version bump could only
 * withdraw by refusing these files too.
 *
 * COMPATIBILITY FOR FILES ALREADY ON DISK, handled by machinery that already
 * exists and is not changed here:
 *   - pgwt_summary_reader_open refuses any file whose header version is not
 *     PGWT_SUMMARY_VERSION, with a WARN naming both versions
 *     (src/summary_reader.c), so a v4 file is never decoded for computation;
 *   - pgwt-server PREFLIGHTS the window (should_use_summaries ->
 *     pgwt_summaries_window_current) and takes the RAW path for any window
 *     containing a non-current file. That is the part that matters: refusing
 *     alone would have left pgwt_visit_summaries silently SKIPPING those
 *     files and answering from the rest -- a plausible partial window
 *     labelled "fidelity":"exact". So an existing v4 capture is still fully
 *     readable, it is just recomputed from its raw events, which is the
 *     source of truth and does carry every query's CPU time;
 *   - startup recovery still FINALIZES and archives an intact older
 *     current.summary rather than calling it corrupt, so nothing is deleted.
 * No migration is possible or needed: a v4 record simply does not contain the
 * per-query CPU entry, and the raw trace already holds what is required to
 * recompute it. */
#define PGWT_SUMMARY_VERSION  5

/* #277: how long after a second ends it is treated as complete by the
 * periodic flush. The daemon's timer handler runs BEFORE the event-ring
 * drain in the same main-loop pass (and the ring is polled every 10 ms), so
 * an event belonging to the just-ended second can still be in flight. Late
 * arrivals beyond this are still counted exactly once -- they fold into the
 * next open second -- this only keeps them in the right second. */
#define PGWT_SUMMARY_FLUSH_LAG_NS  100000000ULL   /* 100 ms */

/* Limits for per-second accumulator hash tables */
#define SUMMARY_MAX_EVENTS    1024
#define SUMMARY_MAX_SESSIONS  MAX_BACKENDS   /* 1024 */
#define SUMMARY_MAX_QUERIES   2048

/* Worst-case serialized size of one per-second record, COMPUTED from the
 * table bounds rather than guessed.
 *
 * pgwt_summary_serialize() writes into a fixed buffer and (before v3) ignored
 * its out_size entirely, so the only thing standing between a full record and
 * a heap overrun was that 800 KB happened to be enough. v3 adds 8 bytes per
 * query (idle_ns) and 12 per record, which cut the old margin to ~26 KB --
 * close enough that the next field to be added would have been the one that
 * silently overran. The bound is now derived here and the writer both sizes
 * its buffer from it and refuses to write past out_size.
 *
 *   class_ns   PGWT_NUM_CLASSES * 8
 *   events     SUMMARY_MAX_EVENTS   * (4+8+8+8 + HISTOGRAM_BUCKETS*8)
 *   sessions   SUMMARY_MAX_SESSIONS * (4+8+8+4+8)
 *   queries    SUMMARY_MAX_QUERIES  * (36 + PGWT_NUM_CLASSES*8 + 1
 *                                      + SUMMARY_QUERY_TOP_EVENTS*20 + 8)
 *   trailer    8 (idle_ns) + 4 (events_overflow)
 */
#define PGWT_SUMMARY_SERIALIZE_MAX                                            \
    ((size_t)(PGWT_NUM_CLASSES * 8)                                           \
     + (size_t)SUMMARY_MAX_EVENTS   * (28 + HISTOGRAM_BUCKETS * 8)            \
     + (size_t)SUMMARY_MAX_SESSIONS * 32                                      \
     + (size_t)SUMMARY_MAX_QUERIES  * (36 + PGWT_NUM_CLASSES * 8 + 1          \
                                       + SUMMARY_QUERY_TOP_EVENTS * 20 + 8)   \
     + 12)

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
    /* v3: EXACT idle total for this query. top_events[] holds only 8 entries,
     * so a query with 8 busier non-idle events would otherwise report Idle = 0
     * while its DB Time was right -- DB Time + Idle would stop accounting for
     * the window under a query filter. The named Idle CHILDREN still come from
     * top_events (bounded); the difference is emitted as one labelled
     * remainder row, so the total is exact even when the breakdown is not. */
    uint64_t idle_ns;
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
    /* v3: events[] could not take an entry because all SUMMARY_MAX_EVENTS
     * slots were occupied. Since v3 moved idle time OUT of class_ns, events[]
     * is the only per-event source left, so a full table under-reports the
     * Idle breakdown and any class/event-FILTERED total while unfiltered
     * DB Time stays correct -- a mismatch with no signal. This is that
     * signal. ~270 distinct PG wait events against 1024 slots is headroom,
     * not a proof, and it is not a proof at all for Extension events. */
    uint32_t events_overflow;

    /* v3: EXACT system-wide idle total (Activity + Client:ClientRead + the
     * Timeout pacing sleeps), accumulated at the writer independently of
     * events[] so it survives a full table. */
    uint64_t idle_ns;

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

    /* #277: the monotonic second of the last record actually written. A
     * record is immutable once on disk, so a second that has been written
     * can never be reopened -- the reader would sum it twice. An event that
     * arrives late for such a second is folded into the oldest second still
     * open (late_events_folded_total counts those, so the fold is never
     * silent; the control socket publishes it as
     * summary_late_events_folded_total). have_flushed_second distinguishes
     * "nothing written yet" from "wrote the second at monotonic 0".
     *
     * The clamp runs before accum_event, which returns early for markers
     * (PGWT_IS_MARKER) and for PGWT_EVENT_EXIT -- so a late marker is
     * counted here while contributing no counts and no time. The counter is
     * a skew signal, not an exact count of mis-timed wait events. */
    uint64_t      last_flushed_second_mono_ns;
    bool          have_flushed_second;
    uint64_t      late_events_folded_total;

    /* Scratch buffers */
    uint8_t      *encode_buf;
    size_t        encode_buf_size;
    uint8_t      *compress_buf;
    size_t        compress_buf_size;

    /* Stats */
    /* Seconds that could not be serialised/compressed. Non-zero means the
     * window has holes; it was previously impossible to tell, because every
     * caller of flush_accum discarded its return value. */
    uint64_t      flush_failures_total;
    /* Σ of every record's events_overflow, so the control socket can report
     * one number instead of a reader having to sum the blocks. */
    uint64_t      events_overflow_total;
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

/* Force-flush the current accumulator whatever its state (rotation, close,
 * and offline generators that drive the writer with synthetic timestamps).
 * NOT for the periodic daemon tick: see pgwt_summary_flush_completed. */
int  pgwt_summary_flush(struct pgwt_summary_writer *w);

/* Periodic (daemon-tick) flush. Writes the accumulated second only once it
 * is complete -- i.e. once now_mono_ns has passed its end plus
 * PGWT_SUMMARY_FLUSH_LAG_NS -- and leaves a second still receiving events
 * alone. #277: the unconditional flush wrote the in-progress second, and
 * the rest of that same second was then written again as a superset, so
 * every summaries-path aggregate counted part of each second twice.
 *
 * now_mono_ns is CLOCK_MONOTONIC, the same clock as evt->timestamp_ns
 * (bpf_ktime_get_ns). It is a parameter, not a clock read, so the decision
 * is testable without depending on wall time. */
int  pgwt_summary_flush_completed(struct pgwt_summary_writer *w,
                                   uint64_t now_mono_ns);

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
