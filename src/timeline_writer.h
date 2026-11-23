/* timeline_writer.h — writer and reader of timeline-NNNN.pgwtl (#1006).
 *
 * Writer: header, 64-byte records, a STATS trailer at the end of every
 * segment, rotation at segment_bytes, retention oldest segment first, fsync
 * of the file and the directory on close, mode 0640 and the trace group.
 * Reader: the reader rules of timeline_format.h (refuse unknown magic or
 * version, step by rec_size, skip unknown kinds, ignore unknown flags,
 * missing STATS trailer = TRUNCATED).
 *
 * Self-contained: libc only (no other src/ unit), so the unit tests and the
 * summary link it alone.
 *
 * Status: S0 stub with the FROZEN prototypes; S2 implements.
 */
#ifndef PGWT_TIMELINE_WRITER_H
#define PGWT_TIMELINE_WRITER_H

#include <stddef.h>
#include <stdint.h>
#include <sys/types.h>

#include "timeline_format.h"

struct pgwt_tl_writer_config {
    const char *dir;               /* trace directory (exists) */
    uint64_t    segment_bytes;     /* rotation size; 0 = PGWT_TL_SEGMENT_BYTES */
    uint64_t    retention_bytes;   /* 0 = unlimited; else delete the oldest
                                    * closed segments first to stay under it */
    gid_t       trace_gid;         /* (gid_t)-1 = leave group as created */
    /* Template for every segment header: pg_version, postmaster_pid, caps,
     * mono_start_ns, wall_start_ns, boot_id. The writer sets magic, version,
     * rec_size and segment_index itself. */
    struct pgwt_tl_header header;
};

struct pgwt_tl_writer;

/* Opens the first segment (index after the highest existing one). 0 or a
 * negative errno; *out is NULL on failure. */
int pgwt_tl_writer_open(struct pgwt_tl_writer **out,
                        const struct pgwt_tl_writer_config *cfg);

/* Appends one record (any kind except STATS, which only the writer writes);
 * rotates first when the segment is full. 0 or a negative errno. */
int pgwt_tl_writer_append(struct pgwt_tl_writer *w,
                          const struct pgwt_tl_record *rec);

/* Pushes buffered records to the file (no fsync). 0 or a negative errno. */
int pgwt_tl_writer_flush(struct pgwt_tl_writer *w);

/* Writes the final STATS trailer (PGWT_TL_STATS_FINAL, end_ns,
 * lost_raw_total), fsyncs file and directory, frees w. 0 or a negative
 * errno; w is freed either way. */
int pgwt_tl_writer_close(struct pgwt_tl_writer *w, uint64_t end_ns,
                         uint64_t lost_raw_total);

/* Header helpers. init sets magic, version, rec_size and zeroes the rest;
 * check returns 0 for a header a reader accepts, else -EINVAL. */
void pgwt_tl_header_init(struct pgwt_tl_header *h);
int  pgwt_tl_header_check(const struct pgwt_tl_header *h);

/* /proc/sys/kernel/random/boot_id as 16 bytes. 0 or a negative errno
 * (out zeroed). */
int pgwt_tl_read_boot_id(uint8_t out[16]);

/* Segment indexes present in dir, ascending; returns how many were found
 * (may exceed max; only max are stored) or a negative errno. */
int pgwt_tl_scan_segments(const char *dir, uint32_t *indexes, int max);

/* Path of segment `index` in dir. 0, or -ENAMETOOLONG. */
int pgwt_tl_segment_path(const char *dir, uint32_t index, char *buf, size_t bufsz);

struct pgwt_tl_reader;

/* Opens one segment and validates its header. 0 or a negative errno. */
int pgwt_tl_reader_open(struct pgwt_tl_reader **out, const char *path);

const struct pgwt_tl_header *pgwt_tl_reader_header(const struct pgwt_tl_reader *r);

/* Next record of a KNOWN kind (STATS included; unknown kinds are skipped
 * and counted). 1 = *out filled, 0 = end of segment, negative errno = error
 * (a partial trailing record is end of segment, the segment TRUNCATED). */
int pgwt_tl_reader_next(struct pgwt_tl_reader *r, struct pgwt_tl_record *out);

/* After next() returned 0: 1 when the segment has no STATS trailer as its
 * last record (TRUNCATED: loss unknown), else 0. */
int pgwt_tl_reader_truncated(const struct pgwt_tl_reader *r);

/* The STATS trailer: 0 and *out filled when present, else -ENOENT. */
int pgwt_tl_reader_stats(const struct pgwt_tl_reader *r, struct pgwt_tl_stats *out);

uint64_t pgwt_tl_reader_skipped(const struct pgwt_tl_reader *r);

void pgwt_tl_reader_close(struct pgwt_tl_reader *r);

#endif /* PGWT_TIMELINE_WRITER_H */
