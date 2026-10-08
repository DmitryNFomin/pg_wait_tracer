/* event_reader.h — Trace file reader: block index, LZ4 decompress, columnar decode */
#ifndef PGWT_EVENT_READER_H
#define PGWT_EVENT_READER_H

#include "event_writer.h"
#include "pg_wait_tracer.h"

#include <stdint.h>
#include <stdio.h>

/* ── Single-file reader state ──────────────────────────── */

struct pgwt_event_reader {
    FILE         *fp;
    char          path[512];

    struct pgwt_trace_file_header header;

    struct pgwt_block_index_entry *block_index;
    int           num_blocks;

    /* Clock conversion: wall_ns = mono_ns + mono_to_wall */
    int64_t       mono_to_wall;

    /* Scratch buffers */
    uint8_t      *compress_buf;
    size_t        compress_buf_size;
    uint8_t      *decode_buf;
    size_t        decode_buf_size;
};

/* Open a trace file: read header, footer, block index.
 * Returns 0 on success, -1 on error. */
int pgwt_reader_open(struct pgwt_event_reader *r, const char *path);

/* Close file and free internal buffers. */
void pgwt_reader_close(struct pgwt_event_reader *r);

/* Per-block metadata surfaced to consumers (trace format v2). */
struct pgwt_block_info {
    enum pgwt_block_type block_type;   /* TRANSITIONS or SAMPLES */
    uint64_t sample_period_ns;         /* SAMPLES: nominal interval; else 0 */
    uint64_t first_timestamp_ns;       /* monotonic */
    uint64_t last_timestamp_ns;        /* monotonic */
    /* Records in the block as WRITTEN, straight from the block header. Lets a
     * consumer identify a block completely without decompressing it — the
     * paint-latency block aggregate (src/block_agg.h) revalidates a cached
     * aggregate against this, so a block rewritten at the same offset with a
     * different record count cannot be answered from the stale entry. */
    uint32_t num_events;
};

/* Decode a single block by index into out[].
 * Returns number of events decoded, or -1 on error.
 *
 * Both block types decode into pgwt_trace_event. For a SAMPLES block, each
 * record is a point observation: new_event = sampled event id, old_event = 0,
 * duration_ns = 0, and flags has PGWT_EVENT_FLAG_SAMPLE set so consumers can
 * distinguish samples from transition intervals per-record. For a TRANSITIONS
 * block, flags is 0 and all columns are populated as before. */
int pgwt_reader_decode_block(struct pgwt_event_reader *r, int block_idx,
                              struct pgwt_trace_event *out, int max_events);

/* Like pgwt_reader_decode_block(), but also fills *info with the block's
 * type and sample_period_ns (NULL allowed). Use this when block-level
 * fidelity matters (e.g. A3's exact-wins merge needs the block time range
 * and sample period). */
int pgwt_reader_decode_block_info(struct pgwt_event_reader *r, int block_idx,
                                   struct pgwt_trace_event *out, int max_events,
                                   struct pgwt_block_info *info);

/* Read just a block's header metadata (no decode). Returns 0 / -1. */
int pgwt_reader_block_info(struct pgwt_event_reader *r, int block_idx,
                           struct pgwt_block_info *info);

/* Find the first block that could contain events at or after mono_ns.
 * Returns block index (0-based). */
int pgwt_reader_find_block(const struct pgwt_event_reader *r, uint64_t mono_ns);

/* Convert wall-clock nanoseconds to monotonic for this file. */
uint64_t pgwt_reader_wall_to_mono(const struct pgwt_event_reader *r, uint64_t wall_ns);

/* Convert monotonic nanoseconds to wall-clock for this file. */
uint64_t pgwt_reader_mono_to_wall(const struct pgwt_event_reader *r, uint64_t mono_ns);

/* ── Multi-file scanning ───────────────────────────────── */

struct pgwt_trace_file_entry {
    char     path[512];
    uint64_t start_wall_ns;
    int      year, month, day, hour;
};

/* Scan trace_dir for trace files. Returns count, sorted by time ascending. */
int pgwt_scan_trace_files(const char *trace_dir,
                           struct pgwt_trace_file_entry *entries,
                           int max_entries);

/* Parse time string → nanoseconds since Unix epoch.
 * Supports: ISO 8601 ("2025-02-25T14:30:00"), relative ("1h", "30m"), "now". */
int pgwt_parse_time(const char *str, uint64_t *wall_ns);

/* ── Replay accumulation (fidelity-aware — T1/FID-5) ────── */

struct pgwt_accumulator;

/* Counters replay maintains so the caller can report honestly what the
 * accumulated numbers are made of. */
struct pgwt_replay_stats {
    uint64_t transitions;       /* exact records accumulated */
    uint64_t samples;           /* sampled records accumulated (ASH math) */
    uint64_t markers_skipped;   /* structural markers skipped */
    uint64_t sample_period_ns;  /* sample period seen (0 if none) */
};

/* Replay one decoded block into an accumulator, filtering by time range.
 * `bi` is the block's header info (required):
 *   - TRANSITIONS records accumulate exactly (durations, histograms);
 *     markers (exec/plan/escalation) are structural and skipped.
 *   - SAMPLES records are point observations: each is worth one
 *     sample_period_ns of time (ASH estimate) for counts/time-model/query
 *     totals, but contributes NOTHING to min/max/histograms — those are
 *     real-latency-only and would be fabricated from samples.
 * `st` (optional) is updated with what was accumulated. */
void pgwt_replay_events(struct pgwt_accumulator *acc,
                         const struct pgwt_trace_event *events, int count,
                         uint64_t from_mono_ns, uint64_t to_mono_ns,
                         const struct pgwt_block_info *bi,
                         struct pgwt_replay_stats *st);

#endif /* PGWT_EVENT_READER_H */
