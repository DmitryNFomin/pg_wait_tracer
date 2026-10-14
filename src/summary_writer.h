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
/* v6 (2026-10-13, issues #315 and #376): io_worker records are EXCLUDED at
 * the writer, from every accumulator -- class_ns, idle_ns, events[]
 * (histograms included), sessions[] and queries[]. ONE content change, no
 * layout change.
 *
 * WHY: docs/AAS_SEMANTICS_DECISION.md records that io_worker time is outside
 * DB Time and outside Idle (it is surfaced as an io_worker busy %, because
 * the requesting backend already shows AioIoCompletion for the same logical
 * I/O). src/compute.c implements that on the raw paths. The summary writer
 * did not, so the same window answered differently depending on which path
 * served it: +400% on DB Time on a synthetic probe (#315), and on the PG18
 * demo capture (tests/results/demo_rehearsal/trace, 1851 s, 3 io_workers)
 * summary Idle ran +5,549,649.64 ms over raw -- +23.36% on the whole
 * capture, +308.16% on a 150 s window containing the capture end, +2.43% on
 * the same window one second earlier -- because each io_worker's
 * Activity:IoWorkerMain interval closes at capture end and is bucketed whole
 * into that second. (#376's issue text quotes a different pair,
 * 406,476.45 vs 528,770.62 ms; that window could not be reconstructed, so
 * the numbers here are the ones this branch measured, with the harness kept
 * in tests/results/issue376-measurement.)
 *
 * WHY THE VERSION HAS TO MOVE even though the byte layout is identical: the
 * version is what tells a reader which ACCOUNTING RULE the precomputed
 * per-second totals were written under -- the same reason v3, v4 and v5
 * exist. A v5 second counts io_worker time and a v6 second does not, so a
 * window holding both reports a DB Time and an Idle that is neither. The
 * existing machinery handles files already on disk and is NOT changed here:
 * pgwt_summary_reader_open refuses a non-current version with a WARN, and
 * pgwt-server preflights the window (should_use_summaries ->
 * pgwt_summaries_window_current) so a refusal becomes a RAW recompute rather
 * than a plausible partial answer; startup recovery still archives an intact
 * older file instead of calling it corrupt. A v5 capture therefore stays
 * fully readable, from its raw events, which are the source of truth and
 * already carry the pid categories (pgwt_tag_events + backends.jsonl).
 *
 * HOW the writer knows: it is TOLD. `cat_flag` is a parameter of
 * pgwt_summary_push_event, resolved by the producer before the push
 * (src/event_stream.c hoists pgwt_live_pid_cat_flag above it;
 * src/escalation.c resolves in flush_open_intervals). `evt->flags` is EMPTY
 * of category bits on the live path, so a flags-only check inside the writer
 * would be dead code in the daemon -- see hole N7 in
 * tests/test_agg_raw_crosscheck.c.
 *
 * RESIDUAL, stated because it is real and small: pgwt_live_pid_cat_flag
 * returns 0 (foreground) while a pid's metadata is still unparsed, so an
 * io_worker's earliest records can be written as foreground, while the raw
 * path tags them post hoc from backends.jsonl and excludes them. That window
 * is bounded by discovery's first metadata parse for that pid. MEASURED on
 * the demo capture: 0 ms of 5,553,133.69 ms of io_worker time (no io_worker
 * record closes inside the capture's first second) -- small there, but not
 * zero by construction. The writer counts what it excluded
 * (io_worker_events_excluded_total / io_worker_ns_excluded_total, exported by
 * the control socket) so the residual stays observable rather than assumed.
 *
 * NOT fixed by v6: the summary path still reports no io_worker BUSY total
 * (cat_ms[]/io_worker_busy_pct are raw-path-only, hole N1 -- they were 0 on
 * the summary path before this change too, so nothing was lost), and the
 * writer still buckets a long idle interval WHOLE into the second in which
 * it closes while the raw path clips it to the requested window
 * (event_window_ns). That straddler mechanism is general -- a long
 * Client:ClientRead behaves the same way -- so "the summary equals raw on
 * any window" is NOT what this version claims; it claims the io_worker
 * accounting rule now matches. */
/* v7 (2026-10-14, issue #408): the per-second record now carries the
 * MEASURED-CPU SPLIT, and the writer applies the non-command-CPU relabel the
 * raw paths apply. Two content changes, one layout change (a 40-byte record
 * trailer).
 *
 * WHAT WAS WRONG, measured on the PG18 demo capture run.id 1791973533
 * (1929 s, 7,416,121 records; harness and output in
 * tests/results/issue408-measurement):
 *
 *   1. THE SPLIT WAS NOT IN THE FILE. The raw time model reports CPU as two
 *      rows -- "CPU (running)" (the conserved sum of each interval's measured
 *      cpu_ns) and "CPU (waiting for a core)" (the runqueue residual
 *      DB - CPU* - Sigma waits) -- whenever any record carried a measured
 *      cpu_ns. The summary record stored neither quantity, so
 *      pgwt_compute_time_model_from_summaries could only report
 *      has_measured_cpu = 0 and emit ONE combined "CPU*" row. At the full
 *      span that is 340,625.997 + 225,927.106 ms shown as one 586,586.148 ms
 *      row: the runqueue-wait signal -- the subject of #386's whole Off-CPU
 *      investigation -- simply disappears from the Overview on every window
 *      of 120 s or more, which is every window the demo shows.
 *
 *   2. THE NON-COMMAND RELABEL WAS NOT APPLIED. A foreground backend's
 *      we == 0 interval OUTSIDE a command is post/between-command idle time,
 *      not CPU (docs/AAS_SEMANTICS_DECISION.md). Both raw paths file it under
 *      PGWT_WEI_NONCMD_CPU: the server's tagging pass (pgwt_tag_events) and
 *      the live accumulators (pgwt_live_effective_event, issue #97). The
 *      writer never did -- it saw the raw ringbuf id, and the daemon resolved
 *      the command gate ~65 lines BELOW the summary push -- so that time sat
 *      in the summary's CPU class and therefore inside DB Time, while raw
 *      counts it as Idle. MEASURED: +20,033.044 ms on the full span, which is
 *      the WHOLE of the DB Time divergence there (raw 4,210,812.782 vs
 *      summary 4,230,845.827 = +20,033.044, and Idle is low by exactly the
 *      same -20,033.044) -- +3.54% on the CPU row itself. The identity holds
 *      to three decimals, which is why this is not a pid-categorisation
 *      effect: no other mechanism contributes at the full span.
 *
 * HOW the writer knows, both times: it is TOLD, the same way v6 is told a
 * pid's category. pgwt_summary_push_event takes `eff_event` -- the wait id
 * the PRODUCER resolved through the shared classifier
 * (pgwt_live_effective_event in src/event_stream.c, which now runs the
 * command-gate sweep ABOVE the push; pgwt_tag_events' output for offline
 * producers). A flags- or gate-only check inside the writer would be dead
 * code in the daemon, exactly as hole N7 in tests/test_agg_raw_crosscheck.c
 * records for the category bits. PGWT_SUMMARY_EFF_UNRESOLVED means "the
 * producer did not resolve one": the writer then uses evt->old_event (the v6
 * behaviour) and COUNTS it (eff_unresolved_events_total), so the residual is
 * observable rather than assumed. An eff_event that is neither
 * evt->old_event nor PGWT_WEI_NONCMD_CPU is refused the same way
 * (eff_rejected_events_total) -- the relabel is the only rewrite this
 * parameter may express.
 *
 * WHY THE VERSION HAS TO MOVE: both changes are ACCOUNTING-RULE changes, the
 * same reason v3, v4, v5 and v6 exist. A v6 second charges non-command CPU to
 * DB Time and a v7 second charges it to Idle, so a window holding both
 * reports a DB Time that is neither; and a v6 second cannot answer "what was
 * measured CPU" at all, so a mixed window would emit the two-row split
 * carrying only part of its span. The layout also grows, which makes the bump
 * mandatory rather than conventional: a v6 record ends after the 12-byte v4
 * trailer, so a reader expecting the v7 trailer mis-parses it.
 *
 * FILES ALREADY ON DISK are handled by machinery that exists and is unchanged
 * here: pgwt_summary_reader_open refuses any file whose version is not
 * PGWT_SUMMARY_VERSION (with a WARN naming both), and pgwt-server preflights
 * the window (should_use_summaries -> pgwt_summaries_window_current) so a
 * refusal becomes a RAW RECOMPUTE, never a plausible partial answer. Since
 * #403 that fallback is correct rather than a silent zero. Startup recovery
 * still finalizes and archives an intact older file instead of calling it
 * corrupt. A v6 capture therefore stays fully readable from its raw events,
 * which carry cpu_ns and the CMD markers and are the source of truth for
 * both rules. No migration is possible: the measured-CPU scalars are simply
 * not in a v6 record.
 *
 * SCOPE, stated because it is easy to over-read:
 *   - The split is reported on the UNFILTERED window only. The stored
 *     scalars are record-wide, which IS the unfiltered population; a
 *     class/event/query-filtered request asks about a different population
 *     the file cannot answer, so that path keeps has_measured_cpu = 0 and the
 *     single "CPU*" row (the v6 behaviour, honest about what it cannot
 *     split). Per-query and per-session CPU are unchanged.
 *   - v7 says NOTHING about #379: the writer still charges a long interval
 *     WHOLE to the second in which it closes, while the raw path clips it to
 *     the window. On the capture above that is +4,699 ms of the +14,153 ms
 *     DB Time divergence at a 900 s window (the rest being the relabel) and
 *     most of the Idle divergence there. "The summary equals raw on any
 *     window" is NOT what this version claims; at the FULL span, where no
 *     record straddles an edge, the two paths now agree to 0.000 ms. */
#define PGWT_SUMMARY_VERSION  7

/* `eff_event` sentinel: the producer resolved no effective wait id, so the
 * writer uses evt->old_event and counts the push. Not 0 -- 0 is a valid id
 * (the on-CPU gap), which is exactly the id this parameter exists to
 * reclassify. */
#define PGWT_SUMMARY_EFF_UNRESOLVED  0xFFFFFFFFu

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
 *              + v7: 5 * 8 (the measured-CPU scalars)
 */
#define PGWT_SUMMARY_SERIALIZE_MAX                                            \
    ((size_t)(PGWT_NUM_CLASSES * 8)                                           \
     + (size_t)SUMMARY_MAX_EVENTS   * (28 + HISTOGRAM_BUCKETS * 8)            \
     + (size_t)SUMMARY_MAX_SESSIONS * 32                                      \
     + (size_t)SUMMARY_MAX_QUERIES  * (36 + PGWT_NUM_CLASSES * 8 + 1          \
                                       + SUMMARY_QUERY_TOP_EVENTS * 20 + 8)   \
     + 12 + 40)

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

    /* v7 (#408): the MEASURED-CPU SPLIT for this second, i.e. everything
     * pgwt_compute_time_model derives its two CPU rows from. Without these
     * the summary path could only report has_measured_cpu = 0 and one
     * combined "CPU*" row -- see the v7 note above.
     *
     * The CPU* total is cpu_measured_ns + cpu_unknown_ns, exactly as the raw
     * path's single cpu_measured_ns accumulator holds both tiers; the
     * runqueue row is then the residual DB - CPU* - Sigma waits, computed by
     * the reader from these and class_ns (the same leak-immune arithmetic the
     * raw path uses -- never a per-interval wall-minus-cpu split, because
     * se.sum_exec_runtime is only current at a tick or a context switch).
     *
     * cpu_measured_records is NOT derivable from the sums: a MEASURED ZERO is
     * a signal ("this window can separate running from waiting, and nothing
     * ran") while an absent measurement is not, and the raw path distinguishes
     * them by counting records, not nanoseconds. */
    uint64_t cpu_measured_ns;       /* Sigma clamped cpu_ns over we==0 gaps   */
    uint64_t cpu_unknown_ns;        /* Sigma wall over UNKNOWN we==0 gaps     */
    uint64_t cpu_clamped_ns;        /* Sigma (cpu_ns - wall) clamped away     */
    uint64_t wait_gap_cpu_ns;       /* Sigma cpu_ns on WAIT records (obs.)    */
    uint64_t cpu_measured_records;  /* records with cpu_ns != UNKNOWN         */

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
    /* v6 (#315/#376): records dropped because their pid is an io_worker, and
     * their wall ns. Not a diagnostic afterthought: it is the only way to see
     * the pgwt_live_pid_cat_flag residual in production (a pid whose metadata
     * is not parsed yet resolves to foreground, so its earliest records are
     * accumulated while the raw path -- tagging post hoc from backends.jsonl
     * -- excludes them). Compare against the raw io_worker total for the same
     * capture: the difference IS the residual. Exported by the control socket
     * as summary_io_worker_events_excluded_total /
     * summary_io_worker_ns_excluded_total. */
    uint64_t      io_worker_events_excluded_total;
    uint64_t      io_worker_ns_excluded_total;
    /* v7 (#408): pushes whose producer resolved no effective wait id
     * (PGWT_SUMMARY_EFF_UNRESOLVED -- the record keeps evt->old_event, i.e.
     * the v6 behaviour), and pushes whose eff_event was neither
     * evt->old_event nor PGWT_WEI_NONCMD_CPU and was therefore REFUSED. Both
     * are the non-command relabel's residual made observable, the same role
     * io_worker_*_excluded_total play for v6: a non-zero
     * eff_unresolved_events_total means some of this file's we==0 time is
     * classified the pre-v7 way, and eff_rejected_events_total should be 0 in
     * any build (it means a producer is passing an id the rule does not
     * allow). Exported by the control socket as
     * summary_eff_unresolved_events_total /
     * summary_eff_rejected_events_total. */
    uint64_t      eff_unresolved_events_total;
    uint64_t      eff_rejected_events_total;

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

/* Push a raw trace event. Handles second-boundary detection and flushing.
 *
 * `cat_flag` is the pid's T2 category, as the PRODUCER resolved it: 0 =
 * foreground/unresolved, else PGWT_EVENT_FLAG_IO_WORKER / _MAINT /
 * _BACKGROUND (the values pgwt_live_pid_cat_flag and the server's
 * bm_type_to_cat_flag return). v6: an io_worker record is excluded from
 * every accumulator, matching the raw paths in src/compute.c.
 *
 * It is a PARAMETER rather than a check on evt->flags because evt->flags
 * carries no category bits on the live path -- the daemon never sets
 * PGWT_EVENT_FLAG_IO_WORKER, the server applies it at raw LOAD time via
 * pgwt_tag_events. A flags-only check in the writer is therefore dead in the
 * daemon (hole N7, tests/test_agg_raw_crosscheck.c). Making it an argument
 * also means no caller can inherit the wrong answer silently: every producer
 * must say what it resolved. Pass 0 for a pid-less marker (the writer drops
 * markers anyway, and resolving a category for pid 0 would be meaningless).
 *
 * `eff_event` (v7, #408) is the EFFECTIVE wait id the producer resolved
 * through the shared classifier -- pgwt_live_effective_event() on the live
 * path, pgwt_tag_events()' rewrite offline. Its only legal value other than
 * evt->old_event is PGWT_WEI_NONCMD_CPU: a foreground we==0 interval outside
 * a command is post/between-command IDLE time, not CPU, and both raw paths
 * file it that way (docs/AAS_SEMANTICS_DECISION.md, issues #97 and #408). It
 * is a PARAMETER for the same reason cat_flag is: the command gate lives in
 * the producer's per-pid accumulator, and a writer-side guess would be a
 * second copy of the rule that could drift from the one the raw paths use.
 * PGWT_SUMMARY_EFF_UNRESOLVED means "not resolved" -- the writer uses
 * evt->old_event and counts the push (eff_unresolved_events_total), so a
 * producer that cannot resolve degrades to the v6 behaviour VISIBLY instead
 * of silently. Anything else is refused and counted
 * (eff_rejected_events_total). */
int  pgwt_summary_push_event(struct pgwt_summary_writer *w,
                              const struct pgwt_trace_event *evt,
                              uint32_t cat_flag,
                              uint32_t eff_event);

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
