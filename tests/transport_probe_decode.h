/* libbpf's PERF_SAMPLE_RAW callback excludes the u32 advertised length,
 * but includes its alignment padding (80 wire bytes arrive as 84 bytes).
 * See fcda6b39:tests/bench_nmi_transport/reader.h. Padding is not wire data. */
#ifndef PGWT_TRANSPORT_PROBE_DECODE_H
#define PGWT_TRANSPORT_PROBE_DECODE_H
#include <stddef.h>
#include <string.h>
#include "../src/pgwt_transport_wire.h"
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
