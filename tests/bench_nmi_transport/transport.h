/* Standalone measurement ABI, never a product record. Refs #871. */
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
enum transport_site { TRANSPORT_DATA, TRANSPORT_BOUNDARY, TRANSPORT_SITES };
struct transport_config {
    U32 transport, reserved;
    U64 boundary_offset_ns; /* deliberately wrong +1 ms in the seal control */
};
struct transport_record {
    U64 ts;
    U32 cpu, tid;
    U64 seq;                 /* attempts, independently per CPU and site */
    U32 site;
    unsigned char pad[36];
};
struct transport_counts {
    U64 attempts, successes, failures, eagain, enospc, other, reserve_null;
};
_Static_assert(sizeof(struct transport_record) == 64, "fixed measurement record");
_Static_assert(sizeof(struct transport_counts) % 8 == 0, "percpu lookup stride");
#undef U64
#undef U32
#endif
