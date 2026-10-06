/* summary_reader.h — Summary file reader: block index, LZ4 decompress, deserialize */
#ifndef PGWT_SUMMARY_READER_H
#define PGWT_SUMMARY_READER_H

#include "summary_writer.h"
#include "event_writer.h"

#include <stdint.h>
#include <stdio.h>

/* ── Single-file reader state ──────────────────────────── */

struct pgwt_summary_reader {
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

/* PREFLIGHT, for callers choosing between the summary fast path and a raw
 * recompute. Returns 1 if every summary file whose hour overlaps
 * [from_wall_ns, to_wall_ns] can actually be COMPUTED from -- readable header,
 * right magic, and version == PGWT_SUMMARY_VERSION -- and 0 otherwise.
 *
 * Why a caller cannot just run pgwt_visit_summaries and look at the count:
 * the visitor SKIPS any file it fails to open and returns the records from the
 * rest, so a window containing one older-version file comes back as a
 * plausible PARTIAL answer with no error anywhere. A 0 here means "recompute
 * from raw events or report an error", never "there is no data".
 *
 * It refuses (returns 0) whenever it could not establish the answer: a failed
 * directory scan, a file it cannot open, a short or unparseable header. An
 * EMPTY overlap set returns 1 with *out_considered == 0 -- there are no
 * summaries to blend, which is a legitimately clean answer, and the caller
 * still gets the count so it can tell "all good" from "nothing there".
 *
 * out_considered / out_unusable may be NULL. */
int pgwt_summaries_window_current(const char *trace_dir,
                                  uint64_t from_wall_ns, uint64_t to_wall_ns,
                                  int *out_considered, int *out_unusable);

/* Open a summary file: read header, footer, block index.
 * Returns 0 on success, -1 on error. */
int pgwt_summary_reader_open(struct pgwt_summary_reader *r, const char *path);

/* Close file and free internal buffers. */
void pgwt_summary_reader_close(struct pgwt_summary_reader *r);

/* Decode a single block by index into an accumulator.
 * Returns 0 on success, -1 on error. */
int pgwt_summary_reader_decode_block(struct pgwt_summary_reader *r,
                                      int block_idx,
                                      struct pgwt_summary_accum *out);

/* Find the first block at or after wall_ns. Returns block index. */
int pgwt_summary_reader_find_block(const struct pgwt_summary_reader *r,
                                    uint64_t wall_ns);

/* ── Multi-file scanning ───────────────────────────────── */

struct pgwt_summary_file_entry {
    char     path[512];
    uint64_t start_wall_ns;
    int      year, month, day, hour;
};

/* Scan trace_dir for summary files. Returns count, sorted by time ascending. */
int pgwt_scan_summary_files(const char *trace_dir,
                             struct pgwt_summary_file_entry *entries,
                             int max_entries);

/* ── Streaming visitor: decode records one at a time ────── */

/* Visitor callback: called once per decoded summary record.
 * Return 0 to continue, non-zero to stop early. */
typedef int (*pgwt_summary_visitor)(const struct pgwt_summary_accum *record,
                                     void *ctx);

/* Stream summary records in [from_ns, to_ns], calling visitor for each.
 * Returns number of records visited, or -1 on error. */
int pgwt_visit_summaries(const char *trace_dir,
                          uint64_t from_wall_ns, uint64_t to_wall_ns,
                          pgwt_summary_visitor visitor, void *ctx);

#endif /* PGWT_SUMMARY_READER_H */
