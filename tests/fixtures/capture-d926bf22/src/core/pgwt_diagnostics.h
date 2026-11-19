#ifndef PGWT_DIAGNOSTICS_H
#define PGWT_DIAGNOSTICS_H

#include <stdint.h>

/* Diagnostic metadata only: never admission, sizing or certification input. */
typedef enum {
    PGWT_POOL_NONE,
    PGWT_POOL_TRANSPORT_QUEUE,
    PGWT_POOL_CAPTURE_ARRIVAL,
    PGWT_POOL_CAPTURE_LIFETIME,
    PGWT_POOL_CAPTURE_BINDING,
    PGWT_POOL_CAPTURE_RETAINED,
    PGWT_POOL_CORE_STAGING,
    PGWT_POOL_CORE_PAYLOAD,
    PGWT_POOL_CORE_IDENTITY,
    PGWT_POOL_CORE_DEPENDENCY,
    PGWT_POOL_CORE_FRONTIER,
    PGWT_POOL_CORE_SETUP,
    PGWT_POOL_COUNT
} pgwt_pool;

typedef struct { uint64_t occupancy, capacity, high; } pgwt_pool_usage;
typedef struct { pgwt_pool pool; pgwt_pool_usage usage; } pgwt_allocation_failure;

static inline const char *pgwt_pool_name(pgwt_pool pool)
{
    static const char *const names[] = {
        "none", "transport_queue", "capture_arrival", "capture_lifetime",
        "capture_binding", "capture_retained", "core_staging", "core_payload",
        "core_identity", "core_dependency", "core_frontier", "core_setup"
    };
    return (unsigned)pool < PGWT_POOL_COUNT ? names[pool] : "unknown";
}

static inline void pgwt_pool_set(pgwt_pool_usage *p, uint64_t occupancy)
{
    p->occupancy = occupancy;
    if (occupancy > p->high) p->high = occupancy;
}

#endif
