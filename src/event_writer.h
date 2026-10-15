/* event_writer.h — Raw event file writer: columnar encoding + LZ4 */
#ifndef PGWT_EVENT_WRITER_H
#define PGWT_EVENT_WRITER_H

#include "pg_wait_tracer.h"
#include "trace_closure.h"
#include "open_ckpt.h"

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include <stdio.h>
#include <time.h>
#include <sys/types.h>

/* ── On-disk format constants ─────────────────────────────── */

#define PGWT_TRACE_MAGIC      0x54574750   /* "PGWT" little-endian */
/* v3 (T8): TRANSITIONS blocks gain a 7th column, cpu_ns (measured on-CPU
 * nanoseconds per interval; PGWT_CPU_NS_UNKNOWN sentinel when not measured).
 * SAMPLES blocks are unchanged. v2 files remain readable — the reader surfaces
 * cpu_ns = PGWT_CPU_NS_UNKNOWN for their events and the compute layer falls
 * back to gap-inference. See docs/TRACE_FORMAT.md. */
#define PGWT_TRACE_VERSION    3
#define PGWT_FLAG_LZ4         0x0001
/* The remaining bits of the header `flags` field carry the closure status
 * (#424 deliverable 1): PGWT_FLAG_CLOSURE_TRACKED / _CLOSED_CLEAN / _ROTATED
 * / _RECOVERED, defined with their reading rules in src/trace_closure.h. No
 * layout change and no version bump — a pre-#424 reader ignores the field's
 * spare bits, and a pre-#424 FILE reads back as "unknown", never as clean. */

#define PGWT_BLOCK_EVENTS     4096         /* events per block */

/* Hard cap on blocks per trace/summary file (DUR-7/8). The writer forces an
 * early rotation when a file reaches it; readers use it as the sanity cap for
 * footer block counts and meta committed counts (a corrupted footer/meta must
 * never drive a multi-GB allocation). 1M blocks = a 16 MB footer index; with
 * ~1 s SAMPLES batching even 1000 Hz × 1024 backends (≈250 full blocks/s)
 * stays under it for a full hour (~900k blocks). Readers accept up to
 * PGWT_MAX_READ_BLOCKS (4×) to cover blocks written between the writer cap
 * being hit and the forced rotation taking effect. */
#define PGWT_MAX_FILE_BLOCKS  (1024 * 1024)
#define PGWT_MAX_READ_BLOCKS  (4 * PGWT_MAX_FILE_BLOCKS)

/* Reader/recovery sanity cap for one compressed block (matches the historic
 * fallback-scan cap; a real block is ≤ ~190 KB uncompressed). */
#define PGWT_MAX_BLOCK_COMPRESSED (10u * 1024 * 1024)

/* DUR-8: batch buffered sample-type records into ~1 s blocks instead of one
 * tiny block per sampler tick (~10× fewer blocks at 10 Hz). */
#define PGWT_SAMPLE_BATCH_NS  1000000000ULL

/* Block type (trace format v2). Identifies what a block's columns mean.
 *   TRANSITIONS — wait-event transition intervals (exact fidelity): the
 *                 full columnar layout (ts, pid, old_event, new_event,
 *                 duration_ns, query_id). This is what the watchpoint
 *                 ("full") provider writes.
 *   SAMPLES     — point observations of a backend's current wait event
 *                 (sampled fidelity): a reduced columnar layout
 *                 (ts, pid, event, query_id) — no old_event, no duration.
 *                 This is what the A2 sampler writes. Each sample means
 *                 "at timestamp T, pid P was in event E"; A3 treats it as
 *                 a point observation worth `sample_period_ns`, never an
 *                 interval.
 *
 *                 Event id 0 (T2): a first-class CPU sample — "at T, pid P
 *                 was on-CPU doing requested work". The capture side gates
 *                 which we==0 readings are recorded (client backends only
 *                 inside a command; background types always; see
 *                 docs/AAS_SEMANTICS_DECISION.md), so consumers treat id 0
 *                 exactly like any active sample. Pre-T2 writers never
 *                 emitted id 0, so old traces read back unchanged; old
 *                 readers decode id 0 as the CPU pseudo-event they already
 *                 knew from transitions. No layout change. */
enum pgwt_block_type {
    PGWT_BLOCK_TRANSITIONS = 0,
    PGWT_BLOCK_SAMPLES     = 1,
};

/* File header (28 bytes) */
struct pgwt_trace_file_header {
    uint32_t magic;
    uint16_t version;
    uint16_t flags;              /* PGWT_FLAG_LZ4 | closure bits (trace_closure.h).
                                  * Rewritten in place at offset 6, once, when
                                  * the file is finalized. */
    uint32_t pg_version;
    uint64_t start_time_ns;      /* CLOCK_REALTIME at file creation */
    uint64_t clock_offset_ns;    /* CLOCK_MONOTONIC at file creation */
} __attribute__((packed));

/* Block header (v2: 40 bytes).
 * block_type/sample_period_ns are new in v2. sample_period_ns is the
 * nominal interval between samples in a SAMPLES block (0 for TRANSITIONS).
 * reserved keeps the 8-byte fields naturally aligned and leaves room for
 * a future v3 without another size bump. */
struct pgwt_trace_block_header {
    uint64_t first_timestamp_ns;
    uint64_t last_timestamp_ns;
    uint32_t num_events;
    uint32_t compressed_size;
    uint32_t uncompressed_size;
    uint16_t block_type;         /* enum pgwt_block_type */
    uint16_t reserved;           /* 0; reserved for future use */
    uint64_t sample_period_ns;   /* SAMPLES: nominal sample interval; TRANSITIONS: 0 */
} __attribute__((packed));

/* Block index entry (16 bytes) */
struct pgwt_block_index_entry {
    uint64_t timestamp_ns;
    uint64_t file_offset;
} __attribute__((packed));

/* ── Writer state ─────────────────────────────────────────── */

struct pgwt_event_writer {
    /* Configuration */
    char          trace_dir[256];
    int           pg_version;
    int           retention_hours;
    uint64_t      retention_bytes;  /* size cap for archived files (0 = off) */
    gid_t         trace_gid;      /* group for trace files, (gid_t)-1 = no chown */
    bool          enabled;
    bool          verbose;
    /* T8: when false (legacy capability — no measured CPU), every TRANSITIONS
     * record is written with cpu_ns = PGWT_CPU_NS_UNKNOWN so the compute layer
     * infers CPU from gaps. The daemon sets it from its startup CPU-accounting
     * probe (src/daemon.c §5.4). Default false is the safe legacy behavior. */
    bool          cpu_measured;

    /* Current file */
    FILE         *fp;
    char          current_path[512];
    int           current_hour;           /* day*24 + hour for rotation */
    bool          force_rotate;           /* block cap reached — rotate now */
    uint64_t      file_start_wall_ns;
    uint64_t      file_start_mono_ns;
    /* #424: the header `flags` value as last written for THIS file. The
     * finalize path ORs its terminal bit into this instead of rebuilding the
     * field from scratch, so a bit a future writer sets at creation cannot be
     * erased by a close. */
    uint16_t      file_flags;

    /* Block index (written as footer) */
    struct pgwt_block_index_entry *block_index;
    int           num_blocks;
    int           block_index_cap;

    /* Event buffer (current block) */
    struct pgwt_trace_event events[PGWT_BLOCK_EVENTS];
    int           num_events;

    /* Pending sample-type record buffer (DUR-8): many small pushes (one per
     * sampler tick) are batched into ~1 s blocks. Type-agnostic — any
     * non-TRANSITIONS block type routed through pgwt_writer_push_samples'
     * buffering path shares it; a type or period change flushes first. */
    struct pgwt_trace_event sample_buf[PGWT_BLOCK_EVENTS];
    int           num_sample_buf;
    uint16_t      sample_buf_type;        /* enum pgwt_block_type of batch */
    uint64_t      sample_buf_period_ns;   /* period of the first batched push */
    uint64_t      sample_buf_weight_ns;   /* Σ period_i per record (weighted mean) */

    /* Scratch buffers (allocated once) */
    uint8_t      *encode_buf;
    size_t        encode_buf_size;
    uint8_t      *compress_buf;
    size_t        compress_buf_size;

    /* #424 publication high-watermark: the largest TRANSITIONS timestamp
     * actually written to the file. Monotonic; never reset by rotation (the
     * clock does not restart, and the live snapshot compares against it). */
    uint64_t      committed_through_ns;

    /* #424 deliverable 2: open-interval checkpoint sequence within the
     * CURRENT file. Reset on every file open, because the checkpoint's
     * identity is the file's start stamps — a seq from a previous file could
     * never be compared against this one's. */
    uint64_t      ckpt_seq;

    /* Stats */
    uint64_t      total_events_written;
    uint64_t      total_bytes_written;
};

/* ── Public API ───────────────────────────────────────────── */

int  pgwt_writer_init(struct pgwt_event_writer *w, const char *trace_dir,
                      int pg_version, int retention_hours,
                      const char *group_name);
int  pgwt_writer_push_event(struct pgwt_event_writer *w,
                            const struct pgwt_trace_event *evt);

/* Write a SAMPLES block (trace format v2). The A2 sampler calls this once
 * per drain with the samples it collected this tick-batch. Only the pid,
 * new_event (= sampled event), query_id, and timestamp_ns fields of each
 * struct are used; old_event/duration_ns are ignored (samples carry
 * neither). `sample_period_ns` is recorded in the block header so the
 * compute layer (A3) can weight each sample. Samples must be sorted by
 * timestamp ascending (delta-varint encoding requires it). A pending
 * TRANSITIONS block, if any, is flushed first so block boundaries stay
 * clean. Returns 0 on success, -1 on error. */
int  pgwt_writer_push_samples(struct pgwt_event_writer *w,
                              const struct pgwt_trace_event *samples,
                              int count, uint64_t sample_period_ns);

/* #424: flush every pending buffer so the closed-record prefix is visible to
 * a concurrent reader, and report the high-watermark it reached. Returns 0 on
 * success, -1 if a block write failed. Safe (no-op) before the first event. */
int  pgwt_writer_publish(struct pgwt_event_writer *w);
uint64_t pgwt_writer_committed_through_ns(const struct pgwt_event_writer *w);

/* #424 deliverable 2: checkpoint the intervals that are open RIGHT NOW, so a
 * crash leaves evidence of them instead of nothing.
 *
 * `recs` are the closing records the shutdown flush WOULD write at
 * `now_mono_ns` — build them with pgwt_build_open_stretch (src/open_interval.h),
 * never by hand: the skip rules and the measured-CPU arithmetic live there,
 * and a second copy of them is how the live screen and the trace drift apart.
 * `count` may be 0, and a 0-record checkpoint is NOT a no-op: it is what
 * supersedes an older checkpoint whose intervals have since closed. Skipping
 * it would leave a stale claim behind for recovery to find, which is the
 * double-count this contract exists to prevent.
 *
 * The call does NOT flush the writer's buffers. It records the watermark the
 * writer has already reached, which is by construction a point the trace
 * really contains. Flushing here would cut a block boundary on every call —
 * at a one-second tick that changes block layout, footer index size and the
 * block-aggregate's cost — so the decision belongs to the caller, which can
 * call pgwt_writer_publish() at whatever cadence it has MEASURED. The sidecar
 * itself is written atomically and durably: tmp → fsync → rename → fsync(dir).
 *
 * Returns 0 on success; -1 without writing anything if there is no open trace
 * file (a checkpoint with no file to name has no identity and must never be
 * written), if `count` exceeds PGWT_OPEN_CKPT_MAX_RECORDS, if a record is
 * self-inconsistent (duration longer than its own timestamp), or if any file
 * step failed. */
int  pgwt_writer_checkpoint_open(struct pgwt_event_writer *w,
                                 uint64_t now_mono_ns,
                                 const struct pgwt_open_ckpt_record *recs,
                                 int count);

/* Write a checkpoint for the trace identified by `id` into `dir`, with the
 * same atomic, durable sequence pgwt_writer_checkpoint_open uses (tmp →
 * fsync → rename → fsync dir). Exposed because the writer-bound entry point
 * needs an open file, and two callers legitimately have an identity without
 * one: a tool importing or repairing a capture, and the test stub that
 * stands in for the daemon's not-yet-wired producer. Sharing this function
 * is what keeps the stub from becoming a second, drifting implementation of
 * the format. Returns 0, or -1 on a refusal or any file step failing. */
int  pgwt_open_ckpt_write(const char *dir,
                          const struct pgwt_open_ckpt_identity *id,
                          uint64_t seq, uint64_t now_mono_ns,
                          uint64_t committed_through_ns,
                          const struct pgwt_open_ckpt_record *recs, int count,
                          gid_t trace_gid);

/* Read and validate a checkpoint sidecar.
 *
 * `path` is the sidecar (`<archive>.trace.lz4.open`, or
 * `<dir>/current.trace.open` for a live capture). `expect` is the identity of
 * the trace file the caller intends to apply it to — pass NULL ONLY to
 * inspect, never to count. On PGWT_OPEN_CKPT_OK, *out describes the records
 * and `*buf_out` owns the memory they point into (free() it). On any other
 * return nothing is allocated and *out is zeroed: a checkpoint that cannot be
 * validated is not used, and the reason is named (pgwt_open_ckpt_rc_name).
 *
 * Reminder the caller may not skip: these durations are LOWER BOUNDS, and a
 * record must be dropped when the trace already holds the closing record for
 * the same stretch — pgwt_open_ckpt_suppressed_by() in src/open_ckpt.h. */
enum pgwt_open_ckpt_rc
pgwt_open_ckpt_load(const char *path,
                    const struct pgwt_open_ckpt_identity *expect,
                    struct pgwt_open_ckpt_view *out, void **buf_out);

/* #424: how the trace file at `path` ended — see src/trace_closure.h for the
 * verdicts and the rules behind them. Returns 0 when the header was read and
 * *out holds the verdict; -1 when the file could not be opened or is not a
 * trace file, in which case *out is PGWT_CLOSURE_UNKNOWN (a file we cannot
 * inspect is never reported clean). */
int  pgwt_trace_file_closure(const char *path, enum pgwt_closure_status *out);

int  pgwt_writer_check_rotation(struct pgwt_event_writer *w);
int  pgwt_writer_close(struct pgwt_event_writer *w);

/* Retention (DUR-3). Deletes archived files past retention_hours (if > 0),
 * cleans orphans (stale current.*.meta / *.meta.tmp with no writer), and —
 * when retention_bytes > 0 — deletes the OLDEST archived .trace.lz4 /
 * .summary.lz4 / preserved .corrupt files until total trace-dir usage fits
 * under the cap. Never touches the live current.trace / current.summary or
 * the sidecar .jsonl files. */
int  pgwt_writer_cleanup_old_files(struct pgwt_event_writer *w);
void pgwt_writer_destroy(struct pgwt_event_writer *w);

/* ── Shared file-lifecycle helpers (used by summary_writer too) ── */

/* Build a collision-safe archive path "<dir>/YYYY-MM-DD_HH<ext>"; if that
 * exists, "<dir>/YYYY-MM-DD_HH.<n><ext>" for the first free n (DUR-2: a
 * restart mid-hour or a DST fold must never clobber an existing archive).
 * Returns 0 on success, -1 if no free name could be found. */
int pgwt_archive_path_nonclobber(const char *dir, const struct tm *tm,
                                 const char *ext, char *out, size_t out_size);

/* fsync a directory so a just-renamed file survives power loss (DUR-5).
 * Returns 0, or -1 if the directory could not be opened or fsynced — a
 * failure a caller must report rather than swallow (#424 review): the rename
 * is visible to this kernel either way, so it does not invalidate the
 * process-death guarantee, but it does mean the name may not survive a power
 * cut, and silence there is how an unmet durability claim survives. */
int pgwt_fsync_dir(const char *dir);

/* ── Exposed for unit testing ─────────────────────────────── */

/* Encode a TRANSITIONS block (full columnar layout). */
size_t pgwt_encode_block(const struct pgwt_trace_event *events, int count,
                         uint8_t *out, size_t out_size);
/* Encode a SAMPLES block (reduced layout: ts, pid, event, query_id).
 * Reads each event's new_event field as the sampled event id. */
size_t pgwt_encode_sample_block(const struct pgwt_trace_event *events, int count,
                                uint8_t *out, size_t out_size);
int pgwt_encode_varint(uint64_t val, uint8_t *out);
int pgwt_decode_varint(const uint8_t *in, size_t avail, uint64_t *val);

#endif /* PGWT_EVENT_WRITER_H */
