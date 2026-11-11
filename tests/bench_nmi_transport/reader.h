/* Portable decoding/validation for the measurement reader. Refs #871. */
#ifndef NMI_TRANSPORT_READER_H
#define NMI_TRANSPORT_READER_H
#include "transport.h"
#include <stdbool.h>
#include <stddef.h>
#include <string.h>

/* PERF_SAMPLE_RAW only: u32 size, then data (NOT an aligned C struct).
 * perf_prepare_sample/perf_sample_save_raw round up (sizeof(u32) + payload)
 * to 8 bytes and INCLUDE the trailing padding in the advertised raw size.
 * For our 64-byte payload this is exactly 68 bytes, not an arbitrary >=64.
 * Kernel framing padding has unspecified contents; record.pad does not. */
static inline bool transport_decode_perf(struct transport_record *r,
                                         const void *sample, size_t size)
{
    const size_t raw_size = ((sizeof(*r) + sizeof(uint32_t) + 7) & ~(size_t)7)
        - sizeof(uint32_t);
    uint32_t advertised;
    if (size != sizeof(advertised) + raw_size) return false;
    memcpy(&advertised, sample, sizeof(advertised));
    if (advertised != raw_size) return false;
    memcpy(r, (const unsigned char *)sample + sizeof(advertised), sizeof(*r));
    return true;
}

static inline bool transport_record_valid(const struct transport_record *r,
                                          uint32_t expected_tid)
{
    if (!r->ts || !r->seq || !expected_tid || r->tid != expected_tid) return false;
    for (size_t i = 0; i < sizeof(r->pad); ++i)
        if (r->pad[i]) return false;
    return true;
}
#endif
