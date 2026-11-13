#ifndef PGWT_EVIDENCE_REPLAY_H
#define PGWT_EVIDENCE_REPLAY_H

#include "pgwt_core.h"

/* Logical U5 input, not a trace codec. Accepts independent initial files in
 * one boot, timestamp-resolved PIDs/generations (including provisional LIFE),
 * exits, attachments, END, global VOID/revocation and query-specific run selection.
 * Legacy v2-v4 files retain no exact evidence; unqualified v5 retains LIFE only.
 * Other kinds and same-capture file chains
 * remain explicitly unsupported, even in an uncertified suffix. Strings are length-delimited; {NULL,0} means a
 * nonwaiting state. Callers supply readable arrays/strings of stated lengths.
 */
typedef enum {
    PGWT_EVIDENCE_OTHER, PGWT_EVIDENCE_LIFE, PGWT_EVIDENCE_ATTACH_OK,
    PGWT_EVIDENCE_TRANS, PGWT_EVIDENCE_FRONTIER, PGWT_EVIDENCE_END,
    PGWT_EVIDENCE_VOID
} pgwt_evidence_kind;

typedef struct {
    bool known;
    uint64_t value[15];
} pgwt_evidence_counters;

typedef struct {
    pgwt_evidence_kind kind;
    uint64_t t;
    uint32_t pid;
    bool has_fork;
    uint64_t fork;
    /* LIFE */
    uint64_t start, exit;
    bool start_known, has_exit;
    /* ATTACH_OK: false means a transition anchor. */
    bool reread;
    pgwt_core_event seed, state;
    /* TRANS */
    pgwt_core_event old_event, new_event;
    uint64_t duration;
    pgwt_evidence_counters counters;
    /* END: exactly detach, deescalate, or stop. */
    pgwt_core_event end_reason;
    /* VOID: opaque nonempty diagnostic; false means retain prior certificates.
     * Unknown counters ignore value[] storage. */
    pgwt_core_event void_reason;
    bool has_invalid_from;
    uint64_t invalid_from;
    /* Set by a codec/adapter when the logical shape could not be decoded. */
    bool unsupported_shape;
} pgwt_evidence_record;

typedef struct {
    unsigned version;
    pgwt_core_event boot, capture;
    uint64_t file_no;
    bool has_previous, present, qualified, unsupported_shape;
    pgwt_evidence_counters baseline;
    const pgwt_evidence_record *records;
    size_t record_count;
} pgwt_evidence_file;

typedef struct {
    const pgwt_evidence_file *files;
    size_t file_count;
    pgwt_core_event capture;
    uint32_t pid;
    bool has_fork;
    uint64_t fork;
    bool unsupported_shape;
} pgwt_evidence_input;

typedef enum {
    PGWT_REPLAY_ERROR = -1, PGWT_REPLAY_OK = 0, PGWT_REPLAY_UNSUPPORTED = 1
} pgwt_replay_status;

typedef struct {
    pgwt_core_evidence projection;
    pgwt_core_span coverage; /* storage for projection.coverage */
    pgwt_core_wait *waits;   /* owned; event bytes borrow the input */
    pgwt_core_result wait_time, completed_waits;
} pgwt_evidence_output;

/* No result objects are emitted on non-OK status; output is zeroed. Reason is
 * an optional static error string. Output must be fresh or freed before reuse.
 * On success, projection and result storage live until output_free; input
 * strings must remain immutable and alive. Output must not be copied/moved
 * because projection.coverage points into it. No prepared handle escapes. */
pgwt_replay_status pgwt_evidence_replay(const pgwt_evidence_input *input,
                                       const pgwt_core_query *query,
                                       pgwt_evidence_output *output,
                                       const char **reason);
void pgwt_evidence_output_free(pgwt_evidence_output *output);

#endif
