/* libbpf's PERF_SAMPLE_RAW callback excludes the u32 advertised length,
 * but includes its alignment padding (80 wire bytes arrive as 84 bytes).
 * See fcda6b39:tests/bench_nmi_transport/reader.h. Padding is not wire data. */
#ifndef PGWT_TRANSPORT_PROBE_DECODE_H
#define PGWT_TRANSPORT_PROBE_DECODE_H
#include <stddef.h>
#include <string.h>
#include "../src/pgwt_transport_wire.h"
struct probe_stamp { __u32 cpu, round; __u64 stamp; };
/* -1: helper passed an unfinished producer, 0: no challenge evidence (refuse),
 * 1: a request actually issued during the spin has its own later boundary.
 * Round matching prevents a stale or unrelated acknowledgement from passing. */
static inline int probe_spin_evidence(__u32 cpu, __u64 started, __u64 ended,
    const struct probe_stamp *requests, size_t nr,
    const struct probe_stamp *boundaries, size_t nb)
{
    for (size_t i = 0; i < nb; ++i) {
        const struct probe_stamp *b = &boundaries[i];
        if (b->cpu != cpu) continue;
        if (b->stamp >= started && b->stamp <= ended) return -1;
    }
    for (size_t j = 0; j < nr; ++j) {
        const struct probe_stamp *r = &requests[j];
        if (r->cpu != cpu || r->stamp < started || r->stamp >= ended) continue;
        for (size_t i = 0; i < nb; ++i) {
            const struct probe_stamp *b = &boundaries[i];
            if (b->cpu == cpu && b->round == r->round && b->stamp > ended) return 1;
        }
    }
    return 0;
}
static inline size_t probe_perf_wire_size(const void *data, size_t raw_size)
{
    struct pgwt_wire_hdr hdr;
    if (raw_size < sizeof(hdr)) return 0;
    memcpy(&hdr, data, sizeof(hdr));
    size_t wire = sizeof(hdr) + hdr.payload_len;
    size_t advertised = ((wire + sizeof(__u32) + 7) & ~(size_t)7) - sizeof(__u32);
    return raw_size == advertised ? wire : 0;
}
#endif
