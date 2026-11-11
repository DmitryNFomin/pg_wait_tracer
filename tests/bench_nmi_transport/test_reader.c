/* Refs #871: reproduce PERF_SAMPLE_RAW framing without Linux or libbpf. */
#include "reader.h"
#include <assert.h>
#include <stdio.h>

int main(void)
{
    /* perf header is 8 bytes; raw length is 4; data starts at offset 12,
     * not 16. Nonzero framing padding is allowed, unlike record.pad. */
    _Alignas(8) unsigned char event[88] = {0};
    unsigned char *sample = event + 8;
    struct transport_record record = { .ts = 123, .cpu = 2, .tid = 456, .seq = 1 };
    uint32_t size = 68;
    memcpy(sample, &size, sizeof(size));
    memcpy(sample + sizeof(size), &record, sizeof(record));
    memset(sample + sizeof(size) + sizeof(record), 0xa5, 4);
    struct transport_record decoded;
    assert(transport_decode_perf(&decoded, sample, 72));
    assert(memcmp(&record, &decoded, sizeof(record)) == 0);
    assert(decoded.cpu == 2 && transport_record_valid(&decoded, 456));
    for (size_t n = 0; n < 80; ++n)
        if (n != 72) assert(!transport_decode_perf(&decoded, sample, n));
    for (uint32_t n = 0; n < 80; ++n) {
        memcpy(sample, &n, sizeof(n));
        assert(transport_decode_perf(&decoded, sample, 72) == (n == 68));
    }
    assert(!transport_record_valid(&record, 0));
    assert(!transport_record_valid(&record, 457));
    decoded = record; decoded.ts = 0;
    assert(!transport_record_valid(&decoded, 456));
    decoded = record; decoded.seq = 0;
    assert(!transport_record_valid(&decoded, 456));
    decoded = record; decoded.tid = 0;
    assert(!transport_record_valid(&decoded, 456));
    for (size_t i = 0; i < sizeof(record.pad); ++i) {
        decoded = record; decoded.pad[i] = 1;
        assert(!transport_record_valid(&decoded, 456));
    }
    puts("nmi transport reader: PASS (padded/unaligned sample, exact sizes, fields)");
    return 0;
}
