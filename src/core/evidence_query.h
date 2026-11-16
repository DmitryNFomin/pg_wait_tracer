#ifndef PGWT_EVIDENCE_QUERY_H
#define PGWT_EVIDENCE_QUERY_H

#include "pgwt_core.h"

/* Consumer boundary: no decoded records, projection, or prepared state escapes.
 * Inputs are immutable for this call only. Results contain integers and static
 * strings and remain valid after all buffers have been destroyed. No cache.
 * The bounded pilot refuses more than 256 files/stops rather than truncating.
 */
#define PGWT_PILOT_MAX_FILES 256

typedef struct { const unsigned char *data; size_t size; } pgwt_evidence_buffer;
typedef struct {
    pgwt_core_event capture;
    uint32_t pid;
    bool has_fork;
    uint64_t fork;
    bool invalid;
} pgwt_evidence_target;
typedef struct {
    size_t file;
    uint64_t block_offset, record_offset;
    bool has_record_offset, descriptor;
    const char *reason;
} pgwt_query_stop;
typedef struct {
    size_t files, codec_stop_count;
    bool codec_failed, capture_ambiguous, has_replay_stop;
    pgwt_query_stop codec_stops[PGWT_PILOT_MAX_FILES], replay_stop;
    bool baseline_mismatch, expected_baseline_known, declared_baseline_known;
    uint64_t expected_baseline[15], declared_baseline[15];
} pgwt_query_storage;

/* 0: both metrics, 1: unsupported (no metrics), -1: API/allocation/limit error
 * (no metrics). reason is an optional static string. v2-v4 have no exact
 * evidence and therefore produce the core's evidence_invalid result. */
int pgwt_evidence_query(const pgwt_evidence_buffer *buffers, size_t n,
                        const pgwt_evidence_target *target,
                        const pgwt_core_query *query,
                        pgwt_core_result *wait_time, pgwt_core_result *completed,
                        pgwt_query_storage *storage, const char **reason);
#endif
