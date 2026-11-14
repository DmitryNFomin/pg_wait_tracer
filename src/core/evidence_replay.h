#ifndef PGWT_EVIDENCE_REPLAY_H
#define PGWT_EVIDENCE_REPLAY_H

#include "pgwt_core.h"

/* Logical evidence-v1 input, not a trace codec. Validated same-capture file
 * prefixes preserve physical append order; gaps stop that capture. Independent
 * captures never stitch. Legacy v2-v4 retains no exact evidence; unqualified v5
 * retains LIFE only. Record/chain faults retain only the validated prefix.
 * Only an unbound target clock domain returns UNSUPPORTED; invalid API arguments
 * and allocation failures return ERROR.
 * Strings are length-delimited; {NULL,0} means nonwaiting. Callers supply readable
 * arrays/strings of stated lengths. */
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
    uint64_t file_no, previous; /* previous is used only when has_previous */
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

/* Diagnostic identifiers are NOT a normative vocabulary. This table maps
 * them to redelivery-ref/replay_effects.py labels where that bounded reference
 * implements the check; the other labels name existing full-replay checks.
 * NONE                         (no stop)
 * BASELINE_MISMATCH            baseline_mismatch
 * NONADVANCING_FRONTIER        nonadvancing_frontier
 * RECOVERY_BEFORE_VOID         recovery_before_void
 * OVERLAPPING_ATTACHMENT       overlapping_attachment
 * ATTACHMENT_OUTSIDE_LIFE      attachment_outside_life
 * MALFORMED_RECORD             malformed_record (outside bounded reference)
 * END_WITHOUT_RUN              end_without_run (outside bounded reference)
 * ANCHOR_MISMATCH              anchor_mismatch (outside bounded reference)
 * RUN_STATE_MISMATCH           run_state_mismatch (outside bounded reference)
 * RUN_DURATION_MISMATCH        run_duration_mismatch (outside bounded reference)
 * FILE_CHAIN                   file_chain (outside bounded reference)
 * CAPTURE_BOOT_CONFLICT        capture_boot_conflict (outside bounded reference)
 */
typedef enum {
    PGWT_REPLAY_STOP_NONE, PGWT_REPLAY_STOP_BASELINE_MISMATCH,
    PGWT_REPLAY_STOP_NONADVANCING_FRONTIER, PGWT_REPLAY_STOP_RECOVERY_BEFORE_VOID,
    PGWT_REPLAY_STOP_OVERLAPPING_ATTACHMENT, PGWT_REPLAY_STOP_ATTACHMENT_OUTSIDE_LIFE,
    PGWT_REPLAY_STOP_MALFORMED_RECORD, PGWT_REPLAY_STOP_END_WITHOUT_RUN,
    PGWT_REPLAY_STOP_ANCHOR_MISMATCH, PGWT_REPLAY_STOP_RUN_STATE_MISMATCH,
    PGWT_REPLAY_STOP_RUN_DURATION_MISMATCH, PGWT_REPLAY_STOP_FILE_CHAIN,
    PGWT_REPLAY_STOP_CAPTURE_BOOT_CONFLICT
} pgwt_replay_stop_reason;

typedef struct {
    bool stopped, descriptor;
    size_t file_index, record_index; /* input indices; record_index only if !descriptor */
    pgwt_replay_stop_reason reason;
    /* Only BASELINE_MISMATCH uses these; known=false represents null. */
    pgwt_evidence_counters expected_baseline, declared_baseline;
} pgwt_evidence_stop;

typedef struct {
    pgwt_core_event boot, capture; /* borrow input strings */
    pgwt_evidence_stop stop;       /* final cutoff, after ALL truncation passes */
    bool has_frontier, has_high_water;
    uint64_t frontier, high_water; /* F and H; zero is a valid value */
    uint64_t *frontiers, *revoked_frontiers; /* owned, increasing, distinct */
    size_t frontier_count, revoked_frontier_count;
    pgwt_evidence_counters baseline;
    /* Nullable latest lifecycle-uncertainty instant, matching the reference's
     * lifecycle_uncertainty field (the flag says whether uncertainty is set).
     * Retained old generations remain tainted; a fresh fork can be eligible. */
    bool has_lifecycle_uncertainty;
    uint64_t lifecycle_uncertainty;
} pgwt_evidence_capture_report;

typedef struct {
    pgwt_evidence_capture_report *captures; /* owned, first-descriptor input order */
    size_t capture_count;
} pgwt_evidence_report;

/* Additive report API. Same projection/metrics as pgwt_evidence_replay.
 * report is required, fresh or freed before reuse, and zeroed on non-OK status.
 * Each joined capture has one entry, including a chain stopped at a descriptor.
 * Stop indices name the original input arrays, never the joined pseudo-records.
 * A descriptor stop has no record index. Only stopped entries carry a reason.
 * State describes the retained prefix before the final cutoff, including VOID
 * revocations; H retains accepted publications even when their F is revoked.
 * This API does not reconcile unordered codec deliveries or report codec stops.
 * The caller supplies validated logical file prefixes in chain order as before.
 * Report arrays live until report_free; input strings obey output's lifetime.
 */
pgwt_replay_status pgwt_evidence_replay_report(const pgwt_evidence_input *input,
                                              const pgwt_core_query *query,
                                              pgwt_evidence_output *output,
                                              pgwt_evidence_report *report,
                                              const char **reason);
void pgwt_evidence_report_free(pgwt_evidence_report *report);
const char *pgwt_evidence_stop_label(pgwt_replay_stop_reason reason);

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
