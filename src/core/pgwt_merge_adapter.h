#ifndef PGWT_MERGE_ADAPTER_H
#define PGWT_MERGE_ADAPTER_H

#include "pgwt_merge.h"

/* U1a: synthetic premises only. No daemon, transport or production proof path.
 * Source indices are CPU numeric order, then E,L. Payloads use explicit LE v1. */
typedef enum { MA_NOT_ATTEMPTED, MA_PENDING, MA_PROVED, MA_FAILED } ma_proof;
typedef enum { MA_TRACE, MA_LIFECYCLE, MA_TEXT, MA_BOUNDARY, MA_LOSS,
               MA_CHECK, MA_WRITER_REJECT, MA_ATTACH_EVENT } ma_kind;
typedef enum { MA_EXACT, MA_SAMPLE, MA_ADMIN } ma_origin;
typedef enum { MA_NONE, MA_WATCHPOINT_PERF, MA_ORDERED_RING } ma_mechanism;
typedef enum { MA_NO_BOUNDARY, MA_RING_BOUNDARY, MA_IDLE_PERF } ma_boundary;
typedef enum { MA_BEGIN, MA_COMPLETE, MA_INTERRUPTED,
               MA_DESTRUCTIVE_RESEED, MA_NONIDEMPOTENT_INIT } ma_attach_event;
typedef enum { MA_NOT_REQUIRED, MA_WAIT, MA_LOST } ma_binding_kind;
/* Astra r2's twelve obligations, in its fixed bit order. Unknown is false. */
enum {
    MA_ALLOCATOR = 1u << 0, MA_FRESH_FORK = 1u << 1,
    MA_MATCHING_COOKIE = 1u << 2, MA_ATTACH_ORDER = 1u << 3,
    MA_PRESERVE_STATE = 1u << 4, MA_SAFE_CLEANUP = 1u << 5,
    MA_EMISSION_COOKIE = 1u << 6, MA_SEQUENCE = 1u << 7,
    MA_FAILURE_ACCOUNTING = 1u << 8, MA_CONTINUOUS = 1u << 9,
    MA_FRESH_FIRST = 1u << 10, MA_IDEMPOTENT_INIT = 1u << 11,
    MA_ALL_FACTS = (1u << 12) - 1
};
typedef struct {
    uint64_t value[PGWT_MERGE_COUNTERS];
    bool valid, component_valid[PGWT_MERGE_COUNTERS];
} ma_snapshot;
typedef struct {
    ma_kind kind;
    uint32_t version;
    ma_origin origin;
    size_t source, size;
    uint64_t position, cookie, q, w, lost, control_pid;
    bool cookie_present, has_q;
    ma_boundary boundary;
    ma_attach_event attach_event;
    uint64_t facts_known, facts_true;
    const unsigned char *bytes;
} ma_envelope;
typedef struct {
    uint64_t pid, g, facts_known, facts_true;
    bool identity_bound, fork_forwarded, failure_reported;
    ma_proof proof;
} ma_lifetime;
typedef struct {
    uint64_t query_id, dependency_id;
    ma_binding_kind kind;
} ma_binding;
typedef struct {
    const ma_lifetime *lifetimes; size_t lifetime_count;
    const ma_binding *bindings; size_t binding_count;
    const ma_envelope *const *held; size_t source_count;
} ma_table;
typedef struct {
    ma_mechanism mechanism;
    bool qualification_complete;
    uint64_t initial_cutoff;
} ma_source_decl;
typedef struct {
    pgwt_merge_mode mode;
    bool synthetic_premises;
    const ma_source_decl *sources; size_t source_count;
} ma_config;
typedef struct ma ma;
typedef struct {
    pgwt_merge_input input[2];
    size_t count;                       /* always one or two */
    bool hold, has_lifetime_update;
    ma_lifetime next_lifetime;
} ma_result;

ma *ma_create(const ma_config *, pgwt_merge_reason *error);
void ma_destroy(ma *);
const pgwt_merge_source *ma_sources(const ma *, size_t *count);
bool ma_core_config(const ma *, const pgwt_merge_config *base,
                    pgwt_merge_config *out);
/* Pure reducer: does not mutate table/envelope or allocate. The caller delivers
 * inputs in order and commits next_lifetime after delivery, even if suspended.
 * Stop feeding on EXIT_REQUIRED. A hold emits a fresh CHECK and retains caller
 * ownership of the envelope and bytes. Pin it in table.held[source]; no later
 * source position may pass it. Keep retained storage immutable until disposition.
 * If caller retention is exhausted it must report allocation_fail and pause.
 * Binding declarations are capture-scoped and immutable after first use; their
 * stable nonzero dependency IDs cannot alias between different query IDs.
 * Results borrow payloads until synchronous core feeding completes; core copies.
 * Sink callers refresh post with failure-time d and post-failure counters and
 * return false on rejection; never submit that rejection a second time here. */
ma_result ma_map(const ma *, const ma_envelope *, const ma_table *,
                 uint64_t d, const ma_snapshot *);

#endif
