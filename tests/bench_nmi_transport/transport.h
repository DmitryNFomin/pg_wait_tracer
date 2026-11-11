/* Standalone measurement ABI. Refs #871. */
#ifndef NMI_TRANSPORT_H
#define NMI_TRANSPORT_H
#ifdef __BPF__
#define U64 __u64
#define U32 __u32
#else
#include <stdint.h>
#define U64 uint64_t
#define U32 uint32_t
#endif

enum transport_kind { TRANSPORT_SHARED, TRANSPORT_PERCPU_RING, TRANSPORT_PERF };
struct transport_record {
    U64 ts;
    U32 cpu, tid;
    U64 seq;
    unsigned char pad[40];
};
struct transport_counts {
    U64 attempts, eagain, enospc, other, reserve_null;
};
_Static_assert(sizeof(struct transport_record) == 64, "fixed transport record");
#undef U64
#undef U32
#endif
