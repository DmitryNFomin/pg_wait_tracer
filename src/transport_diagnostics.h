#ifndef PGWT_TRANSPORT_DIAGNOSTICS_H
#define PGWT_TRANSPORT_DIAGNOSTICS_H

#include <stdbool.h>
#include <stdio.h>
#include "core/pgwt_diagnostics.h"
#include "core/pgwt_merge.h"

#define PGWT_DIAGNOSTIC_BUCKETS 8
struct pgwt_distribution {
    uint64_t count, total, max, histogram[PGWT_DIAGNOSTIC_BUCKETS];
};
/* Time buckets: <100us, <1ms, <10ms, <100ms, <1s, <3s, <10s, >=10s.
 * Record buckets: <1, <16, <256, <4096, <65536, <262144, <1048576, >=1048576. */
static inline void pgwt_distribution_add(struct pgwt_distribution *d, uint64_t value, bool records)
{
    static const uint64_t time_edges[] = {100000, 1000000, 10000000, 100000000,
        1000000000, UINT64_C(3000000000), UINT64_C(10000000000)};
    static const uint64_t record_edges[] = {1, 16, 256, 4096, 65536, 262144, 1048576};
    const uint64_t *edges = records ? record_edges : time_edges;
    size_t bucket = 0;
    while (bucket < PGWT_DIAGNOSTIC_BUCKETS - 1 && value >= edges[bucket]) ++bucket;
    ++d->count; d->total += value; ++d->histogram[bucket];
    if (value > d->max) d->max = value;
}
static inline void pgwt_distribution_print(FILE *out, const char *name,
                                           const struct pgwt_distribution *d, bool records)
{
    fprintf(out, "exact-perf: phase name=%s unit=%s count=%llu mean=%llu max=%llu hist=",
        name, records ? "records" : "ns", (unsigned long long)d->count,
        (unsigned long long)(d->count ? d->total / d->count : 0), (unsigned long long)d->max);
    for (size_t i = 0; i < PGWT_DIAGNOSTIC_BUCKETS; ++i)
        fprintf(out, "%s%llu", i ? "," : "", (unsigned long long)d->histogram[i]);
    fputc('\n', out);
}
static inline void pgwt_pool_print(FILE *out, pgwt_pool pool, pgwt_pool_usage usage)
{
    fprintf(out, " pool=%s occupancy=%llu capacity=%llu high=%llu", pgwt_pool_name(pool),
        (unsigned long long)usage.occupancy, (unsigned long long)usage.capacity,
        (unsigned long long)usage.high);
}
/* This is the closed recovery policy, not a guess from the fault label. */
static inline bool pgwt_transient_capacity(pgwt_merge_reason reason, pgwt_pool pool)
{
    return reason == PGWT_MERGE_ALLOCATION_FAIL &&
        (pool == PGWT_POOL_TRANSPORT_QUEUE || pool == PGWT_POOL_CAPTURE_ARRIVAL ||
         pool == PGWT_POOL_CORE_STAGING);
}
static inline void pgwt_suspension_print(FILE *out, const pgwt_merge_output *record)
{
    fprintf(out, "exact-perf: suspended: %s at %llu; automatic REARM %s",
        pgwt_merge_reason_name(record->reason), (unsigned long long)record->d,
        pgwt_transient_capacity(record->reason, record->allocation.pool) ? "pending fresh proofs" : "disabled");
    if (record->reason == PGWT_MERGE_ALLOCATION_FAIL)
        pgwt_pool_print(out, record->allocation.pool, record->allocation.usage);
    fputc('\n', out);
}
/* schedstat is runtime_ns, run_delay_ns, timeslices. Missing/malformed reads
 * remain unknown; they do not become a measured zero or affect capture. */
static inline bool pgwt_schedstat_parse(FILE *f, uint64_t *delay)
{
    unsigned long long runtime, waiting, slices;
    if (!f || !delay || fscanf(f, "%llu %llu %llu", &runtime, &waiting, &slices) != 3) return false;
    *delay = (uint64_t)waiting;
    return true;
}
struct pgwt_schedstat_sample {
    uint64_t start, end;
    bool start_valid, end_valid;
};
static inline void pgwt_schedstat_print(FILE *out, const char *role, uint32_t tid,
                                         uint32_t cpu, struct pgwt_schedstat_sample s)
{
    fprintf(out, "exact-perf: schedstat role=%s tid=%u cpu=%u start_valid=%d end_valid=%d ",
        role, tid, cpu, s.start_valid, s.end_valid);
    if (s.start_valid) fprintf(out, "start_ns=%llu ", (unsigned long long)s.start);
    else fprintf(out, "start_ns=unknown ");
    if (s.end_valid) fprintf(out, "end_ns=%llu ", (unsigned long long)s.end);
    else fprintf(out, "end_ns=unknown ");
    if (s.start_valid && s.end_valid && s.end >= s.start)
        fprintf(out, "delta_ns=%llu\n", (unsigned long long)(s.end - s.start));
    else fprintf(out, "delta_ns=unknown\n");
}
#endif
