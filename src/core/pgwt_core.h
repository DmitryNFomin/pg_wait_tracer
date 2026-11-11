#ifndef PGWT_CORE_H
#define PGWT_CORE_H

/* Portable C11 API for contracts/wait_time-v1/contracts/metrics/wait_time.md.
 * One evidence object describes ONE backend generation. Times are uint64_t ns.
 * The computation allocates nothing, borrows all input storage, and does not
 * reorder or modify it. Results own fixed storage for at most four reasons;
 * their string pointers refer to static literals. Work is O(waits^2 + coverage).
 * Callers provide two distinct, non-null result objects and readable arrays of
 * the stated lengths. Events are byte strings (UTF-8 for JSON), length delimited
 * so even embedded NULs compare exactly. No other project header is required.
 */
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef struct {
    const char *data;
    size_t length;
} pgwt_core_event;

typedef struct {
    uint64_t start, end;
} pgwt_core_span;

typedef struct {
    pgwt_core_event event;
    uint64_t start, end;
} pgwt_core_wait;

typedef struct {
    pgwt_core_event event;
    uint64_t start, as_of;
} pgwt_core_open_wait;

/* Input adapters report shape failures here, independently by field. Native C
 * callers leave invalid_fields zero. The core checks semantic validity itself.
 * OPEN_AS_OF_INVALID must be set whenever an existing open record's as_of is
 * missing or not an in-range integer, independently of OPEN_INVALID (7.2).
 * Missing open sets OPEN_INVALID, with has_open false. Non-null malformed open
 * sets has_open true. This preserves whether a coverage gap is knowable.
 */
enum {
    PGWT_CORE_LIFE_INVALID = 1u << 0,
    PGWT_CORE_COVERAGE_INVALID = 1u << 1,
    PGWT_CORE_WAITS_INVALID = 1u << 2,
    PGWT_CORE_OPEN_INVALID = 1u << 3,
    PGWT_CORE_OPEN_AS_OF_INVALID = 1u << 4
};

typedef struct {
    uint64_t life_start, life_end;
    bool has_life_end;
    const pgwt_core_span *coverage;
    size_t coverage_count;
    const pgwt_core_wait *waits;
    size_t wait_count;
    bool has_open;
    pgwt_core_open_wait open;
    unsigned invalid_fields;
} pgwt_core_evidence;

enum {
    PGWT_CORE_QUERY_EVENT_INVALID = 1u << 0,
    PGWT_CORE_QUERY_FROM_INVALID = 1u << 1,
    PGWT_CORE_QUERY_TO_INVALID = 1u << 2,
    PGWT_CORE_QUERY_OBJECT_INVALID = 1u << 3
};

typedef struct {
    pgwt_core_event event;
    uint64_t from, to;
    unsigned invalid_fields;
} pgwt_core_query;

typedef struct {
    bool present;             /* false serializes as JSON null */
    uint64_t value;
} pgwt_core_optional_u64;

typedef struct {
    const char *metric;      /* wait_time or completed_waits */
    unsigned version;
    pgwt_core_optional_u64 value;
    const char *unit;        /* ns or count */
    const char *basis;       /* observed */
    const char *completeness; /* complete or unavailable */
    const char *reasons[4];  /* alphabetical; reason_count entries */
    size_t reason_count;
    bool detail_present;    /* absent unless wait_time is complete */
    struct { uint64_t closed_ns, open_ns; } detail;
    struct {
        /* recorded serializes as recorded_ns or recorded_count by metric. */
        pgwt_core_optional_u64 recorded, covered_ns, required_ns;
    } diagnostic;
} pgwt_core_result;

void pgwt_core_wait_metrics(const pgwt_core_evidence *evidence,
                            const pgwt_core_query *query,
                            pgwt_core_result *wait_time,
                            pgwt_core_result *completed);

#endif
