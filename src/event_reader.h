/* event_reader.h — Trace file reader: block index, LZ4 decompress, columnar decode */
#ifndef PGWT_EVENT_READER_H
#define PGWT_EVENT_READER_H

#include "event_writer.h"
#include "pg_wait_tracer.h"

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>

/* ── Single-file reader state ──────────────────────────── */

struct pgwt_event_reader {
    FILE         *fp;
    char          path[512];

    struct pgwt_trace_file_header header;

    struct pgwt_block_index_entry *block_index;
    int           num_blocks;

    /* #429: is block_index[] actually ascending in timestamp_ns?
     *
     * It is written in COMMIT order (event_writer.c appends one entry per
     * flushed block) and nothing enforces timestamp order on top of that.
     * The writer flushes whichever pending buffer holds the older first
     * timestamp, which keeps the two streams in order pairwise — but not
     * across a lagging ring drain, where TRANSITIONS carry timestamps
     * seconds behind the sampler's. Then a SAMPLES block commits with a
     * far-future first timestamp and the next TRANSITIONS block commits
     * after it with an earlier one. Measured on a trace written through the
     * public writer API: [1000 ms, 6000 ms, 1100 ms].
     *
     * Computed once at open, so a consumer can keep a binary search on the
     * files where the invariant does hold and must fall back to a full
     * candidacy scan where it does not. Never assume it: this flag exists
     * because the assumption was silently wrong for `--mode full`. */
    int           block_index_sorted;

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

/* Lower-bound HINT by first timestamp: the last block whose first timestamp
 * is at or before mono_ns, or 0.
 *
 * Two things it is NOT (#429):
 *  - it is not valid on an unsorted index, so it returns 0 (scan everything)
 *    whenever r->block_index_sorted is false, rather than the arbitrary
 *    answer a binary search gives on commit-ordered data;
 *  - even on a sorted index it is NOT a safe scan start on its own. Blocks
 *    OVERLAP in time (a SAMPLES block batches ~1 s of ticks across several
 *    TRANSITIONS blocks), so a block two or more positions earlier can still
 *    reach into the window. Candidacy-test the earlier blocks with
 *    pgwt_reader_block_candidate() instead of trusting this index as a
 *    boundary. */
int pgwt_reader_find_block(const struct pgwt_event_reader *r, uint64_t mono_ns);

/* Can block `bi` hold a record overlapping [from_mono_ns, to_mono_ns]?
 * Header-only — no decompression, one small read.
 *
 * Returns 1 = possible (decode it), 0 = provably not, -1 = the header could
 * not be read. -1 is NOT "no": a caller that cannot see must refuse or pay
 * for the decode, never silently drop the block.
 *
 * `*info` (optional) receives the header. `*right_open` (optional) is set
 * when every record in the block closes after `to` — the block can only
 * contribute still-running straddlers, which is where a caller's lookahead
 * budget applies. 0 / 0 bounds mean unbounded on that side.
 *
 * This is the sound replacement for "binary-search the index, then iterate
 * forward until a block starts after `to`": that pattern assumes the index
 * is sorted (it is in commit order) AND that blocks do not overlap (they
 * do), and it fails SILENTLY — a window missing a whole block reports
 * itself complete. */
int pgwt_reader_block_candidate(struct pgwt_event_reader *r, int bi,
                                uint64_t from_mono_ns, uint64_t to_mono_ns,
                                struct pgwt_block_info *info, int *right_open);

/* Convert wall-clock nanoseconds to monotonic for this file. */
uint64_t pgwt_reader_wall_to_mono(const struct pgwt_event_reader *r, uint64_t wall_ns);

/* Convert monotonic nanoseconds to wall-clock for this file. */
uint64_t pgwt_reader_mono_to_wall(const struct pgwt_event_reader *r, uint64_t mono_ns);

/* ── Multi-file scanning ───────────────────────────────── */

struct pgwt_trace_file_entry {
    char     path[512];
    uint64_t start_wall_ns;
    int      year, month, day, hour;
    /* #405: true when neither the file's header nor (deliberately — see
     * pgwt_scan_trace_files) its name could place it in time. start_wall_ns
     * is 0 in that case. The entry is still LISTED (never dropped, so it
     * stays visible rather than becoming an invisible gap), but a caller
     * must refuse to use it for an answer rather than guess. */
    bool     placement_unknown;
};

/* Scan trace_dir for trace files. Returns count, sorted by time ascending. */
int pgwt_scan_trace_files(const char *trace_dir,
                           struct pgwt_trace_file_entry *entries,
                           int max_entries);

/* True if, given `entries` sorted ascending by start_wall_ns (as
 * pgwt_scan_trace_files returns them), the file at `idx` can be skipped by a
 * replay bounded below by `from_wall_ns` (0 = no lower bound): the file
 * immediately after it already starts at or after `from_wall_ns`, so nothing
 * in `entries[idx]` can still be open there.
 *
 * An unknown placement — on EITHER the entry at `idx` or its successor —
 * never causes a skip: unknown coverage counts against skipping, never for
 * it, same rule pgwt_summaries_window_current uses for summaries. This is
 * the exact predicate src/replay.c's file loop applies; factored out here so
 * it can be pinned without linking the daemon (tests/test_replay_window.c). */
bool pgwt_trace_file_quick_skip(const struct pgwt_trace_file_entry *entries,
                                int n, int idx, uint64_t from_wall_ns);

/* Parse time string → nanoseconds since Unix epoch.
 * Supports: ISO 8601 ("2025-02-25T14:30:00"), relative ("1h", "30m"), "now". */
int pgwt_parse_time(const char *str, uint64_t *wall_ns);

/* ── Replay accumulation (fidelity-aware — T1/FID-5) ────── */

struct pgwt_accumulator;

/* Counters replay maintains so the caller can report honestly what the
 * accumulated numbers are made of. */
struct pgwt_replay_stats {
    uint64_t transitions;       /* exact records that CLOSED in the window */
    uint64_t samples;           /* sampled records accumulated (ASH math) */
    uint64_t markers_skipped;   /* structural markers skipped */
    uint64_t sample_period_ns;  /* sample period seen (0 if none) */
    /* #424: exact records admitted by OVERLAP whose close is past `to` —
     * clipped time was credited, no occurrence was counted. Non-zero means
     * the right-edge scan actually found something, which is what makes a
     * later refactor that drops the lookahead visible instead of silent. */
    uint64_t straddlers_admitted;
    /* The right-edge scan stopped early (lookahead budget exhausted, or a
     * block header could not be read). The window's tail may under-report:
     * say so rather than return a short answer as if it were complete. */
    int      right_edge_truncated;
    /* Blocks that could not be decoded at all (corrupt or truncated file).
     * Their records are missing from the answer; the old loop skipped them
     * without a word, which reads as a complete smaller window. */
    uint64_t blocks_unreadable;
    /* #534: measured CPU that EXCEEDED its interval's wall, summed over the
     * records this window replayed, in NANOSECONDS -- the same unit and the
     * same meaning as the live path's cpu_clamped_ns_total (src/daemon.h,
     * src/event_stream.c), the summary writer's and compute's. Non-zero is a
     * MEASUREMENT-QUALITY signal: clock skew, a schedstat read straddling a
     * boundary, or an accounting error. event_reader.c has always clamped
     * this quantity; it used to discard the remainder, so a skewed trace
     * replayed as though it were clean and silence was indistinguishable
     * from zero. Unscaled by the window clip on purpose: it is a property of
     * the RECORD's measurement, not a time quantity being apportioned.
     *
     * Only exact, on-CPU (we == 0) records with a known cpu_ns contribute: a
     * SAMPLES record has no wall of its own, and CPU measured during a
     * WAIT-labelled gap is the separate wait_gap self-check quantity. */
    uint64_t cpu_clamped_ns;
};

/* How many TRANSITIONS blocks past `--to` one replay run may decode looking
 * for still-running straddlers before it gives up and sets
 * right_edge_truncated. A block holds up to PGWT_BLOCK_EVENTS records, so
 * this is a large window in practice; it exists because the trace format
 * carries no bound on how long a single wait can be. */
#define PGWT_REPLAY_LOOKAHEAD_BLOCKS 1024

/* Replay one decoded block into an accumulator, filtering by time range.
 * `bi` is the block's header info (required):
 *   - TRANSITIONS records accumulate exactly (durations, histograms);
 *     markers (exec/plan/escalation) are structural and skipped.
 *   - SAMPLES records are point observations: each is worth one
 *     sample_period_ns of time (ASH estimate) for counts/time-model/query
 *     totals, but contributes NOTHING to min/max/histograms — those are
 *     real-latency-only and would be fabricated from samples.
 * `st` (optional) is updated with what was accumulated.
 *
 * Window rule (#424), identical to the server's raw path:
 *   - TIME (event/class/session/query totals) is selected by OVERLAP
 *     (close >= from && start <= to) and CLIPPED to [from, to];
 *   - per-OCCURRENCE statistics (count, histogram, min/max) are selected by
 *     the record's own close timestamp and use the whole duration, so a
 *     straddler is never counted twice across adjacent windows.
 * from_mono_ns == 0 / to_mono_ns == 0 mean unbounded on that side. */
void pgwt_replay_events(struct pgwt_accumulator *acc,
                         const struct pgwt_trace_event *events, int count,
                         uint64_t from_mono_ns, uint64_t to_mono_ns,
                         const struct pgwt_block_info *bi,
                         struct pgwt_replay_stats *st);

/* Replay one open trace file into `acc`, decoding blocks through `buf`
 * (capacity `max_events`, normally PGWT_BLOCK_EVENTS).
 *
 * Holds the block-level half of the window rule above: the scan starts at
 * the block containing `from_mono_ns` and, crucially, does NOT stop at the
 * last block before `to_mono_ns` — a wait still running at `to` closes in a
 * later block (or a later file), and stopping made the record-level overlap
 * rule unreachable. That forward scan is budgeted: `lookahead_budget`
 * (optional, normally PGWT_REPLAY_LOOKAHEAD_BLOCKS, shared across all files
 * of one replay) counts TRANSITIONS blocks decoded past `to`; when it runs
 * out, or a block header cannot be read, st->right_edge_truncated is set.
 *
 * Returns 0, or -1 on bad arguments. `out_decoded` (optional) receives the
 * number of records decoded. */
int pgwt_replay_file(struct pgwt_event_reader *r,
                     struct pgwt_accumulator *acc,
                     uint64_t from_mono_ns, uint64_t to_mono_ns,
                     struct pgwt_trace_event *buf, int max_events,
                     int *lookahead_budget,
                     struct pgwt_replay_stats *st,
                     uint64_t *out_decoded);

#endif /* PGWT_EVENT_READER_H */
