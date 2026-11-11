#ifndef PGWT_MERGE_H
#define PGWT_MERGE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* Pure C11 merge-v1 coordinator. Source indices are numeric CPUs, then E, L.
 * Inputs are consumed synchronously. Payload bytes are copied into fixed slots.
 * The sink must not reenter the coordinator; output pointers live only until
 * the callback returns. No allocations occur after create. */
#define PGWT_MERGE_COUNTERS 15

typedef enum { PGWT_MERGE_N, PGWT_MERGE_Q } pgwt_merge_mode;
typedef enum { PGWT_MERGE_OK, PGWT_MERGE_SUSPENDED,
               PGWT_MERGE_EXIT_REQUIRED } pgwt_merge_status;
typedef enum {
    PGWT_MERGE_COUNTER_CHANGE, PGWT_MERGE_COUNTER_UNKNOWN,
    PGWT_MERGE_PERF_LOST, PGWT_MERGE_CLOCK_REGRESSION,
    PGWT_MERGE_SEAL_VIOLATION, PGWT_MERGE_AMBIGUOUS_TIE,
    PGWT_MERGE_SEQUENCE_GAP, PGWT_MERGE_UNKNOWN_EVIDENCE,
    PGWT_MERGE_DEPENDENCY_LOST, PGWT_MERGE_WRITER_PUSH_FAIL,
    PGWT_MERGE_ALLOCATION_FAIL, PGWT_MERGE_DUPLICATE_IDENTITY,
    PGWT_MERGE_SORT_ORDER_BREACH, PGWT_MERGE_SEQUENCE_REUSE,
    PGWT_MERGE_MALFORMED_ENVELOPE, PGWT_MERGE_QUEUE_OWNERSHIP,
    PGWT_MERGE_REASON_COUNT
} pgwt_merge_reason;
typedef enum { PGWT_MERGE_FORK, PGWT_MERGE_INIT, PGWT_MERGE_TRANS,
               PGWT_MERGE_EXIT } pgwt_merge_rank;
typedef enum { PGWT_MERGE_READY, PGWT_MERGE_WAIT,
               PGWT_MERGE_LOST } pgwt_merge_dependency;
typedef struct {
    bool known;
    uint64_t value[PGWT_MERGE_COUNTERS];
} pgwt_merge_counters;
typedef struct { uint64_t d; pgwt_merge_counters counters; } pgwt_merge_observation;
typedef struct {
    uint64_t pid, g, next_q;
    bool identity_proved, sequence_proved;
} pgwt_merge_attachment;
typedef struct {
    bool qualified;                 /* contract §3 boundary mechanism proof */
    uint64_t cutoff;                /* accepted starting state, normally zero */
} pgwt_merge_source;
typedef struct {
    size_t source;
    uint64_t o, t, pid, g, q;
    pgwt_merge_rank rank;
    bool identity_known, has_q;
    pgwt_merge_dependency dependency;
    const void *payload;
    size_t payload_size;
} pgwt_merge_record;
typedef enum {
    PGWT_MERGE_DATA, PGWT_MERGE_TEXT, PGWT_MERGE_BOUNDARY,
    PGWT_MERGE_LOSS, PGWT_MERGE_FAULT, PGWT_MERGE_CHECK,
    PGWT_MERGE_REARM, PGWT_MERGE_WRITER_REJECTED
} pgwt_merge_input_kind;
typedef struct {
    pgwt_merge_input_kind kind;
    pgwt_merge_observation observation;
    pgwt_merge_record record;       /* DATA/TEXT; TEXT uses source,o,payload */
    size_t source;                 /* BOUNDARY/LOSS */
    uint64_t w, p, n;               /* boundary cutoff/position or LOST count */
    pgwt_merge_reason reason;       /* FAULT: closed reason list above */
    const pgwt_merge_attachment *attachments; /* REARM: complete fresh proofs */
    size_t attachment_count;
    bool attachment_proofs_fresh, identity_repaired;
} pgwt_merge_input;
typedef enum { PGWT_MERGE_OUT_TEXT, PGWT_MERGE_OUT_DATA,
               PGWT_MERGE_OUT_FRONTIER, PGWT_MERGE_OUT_VOID } pgwt_merge_output_kind;
typedef struct {
    pgwt_merge_output_kind kind;
    pgwt_merge_record record;
    uint64_t F, d, epoch;
    pgwt_merge_counters counters;
    pgwt_merge_reason reason;
} pgwt_merge_output;
/* Return true only after successful publication. post starts as the current
 * observation; refresh counters after publication and set failure detection d
 * on rejection. A rejected DATA is not an output (contract §3). */
typedef bool (*pgwt_merge_sink)(void *, const pgwt_merge_output *,
                                pgwt_merge_observation *post);
typedef struct {
    pgwt_merge_mode mode;
    const pgwt_merge_source *sources;
    size_t source_count;            /* >= 2, last two indices are E and L */
    size_t staged_capacity, payload_capacity, attachment_capacity;
    const pgwt_merge_attachment *attachments;
    size_t attachment_count;
    pgwt_merge_observation observation; /* readable epoch baseline */
    bool suspended, has_frontier;
    uint64_t frontier;
    pgwt_merge_counters last_observed;
    pgwt_merge_sink sink;
    void *sink_context;
} pgwt_merge_config;
typedef struct pgwt_merge pgwt_merge;
typedef struct {
    pgwt_merge_status status;
    uint64_t epoch, last_invalid_epoch, diagnostic_cutoff;
    bool epoch_invalid, has_frontier, void_failed, qualified;
    uint64_t frontier, faults[PGWT_MERGE_REASON_COUNT];
} pgwt_merge_report;

/* On allocation failure create attempts allocation_fail VOID and returns NULL.
 * status distinguishes that from an invalid/overflowing configuration breach. */
pgwt_merge *pgwt_merge_create(const pgwt_merge_config *, pgwt_merge_status *status);
void pgwt_merge_destroy(pgwt_merge *);
pgwt_merge_status pgwt_merge_feed(pgwt_merge *, const pgwt_merge_input *);
pgwt_merge_report pgwt_merge_get_report(const pgwt_merge *);
const char *pgwt_merge_reason_name(pgwt_merge_reason);

#endif
