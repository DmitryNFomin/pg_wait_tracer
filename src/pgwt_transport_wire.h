/* Transport-only v1 envelope. All integers on the wire are little-endian.
 * Payloads and the v4 capture format are unchanged. No producer in U1b-0. */
#ifndef PGWT_TRANSPORT_WIRE_H
#define PGWT_TRANSPORT_WIRE_H

#ifdef __BPF__
#include "vmlinux.h"
#elif defined(__linux__)
#include <linux/types.h>
#else
#include <stdint.h>
typedef uint8_t __u8;
typedef uint16_t __u16;
typedef uint32_t __u32;
typedef uint64_t __u64;
#endif

#define PGWT_WIRE_VERSION 1
enum {
    PGWT_WIRE_TRACE = 1, PGWT_WIRE_LIFECYCLE = 2,
    PGWT_WIRE_TEXT = 3, PGWT_WIRE_BOUNDARY = 4
};
enum {
    PGWT_WIRE_DEST_CPU = 0, PGWT_WIRE_DEST_E = 1, PGWT_WIRE_DEST_L = 2
};
#define PGWT_WIRE_HAS_Q 0x1U

struct pgwt_wire_hdr {
    __u16 version;       /* PGWT_WIRE_VERSION */
    __u8 kind;           /* PGWT_WIRE_* payload kind */
    __u8 flags;          /* bit 0: HAS_Q; zero in sequence mode N */
    __u16 payload_len;   /* TRACE 48, LIFECYCLE 24, TEXT 272, BOUNDARY 16 */
    __u16 cpu;           /* emitting CPU, including for shared E/L rings */
    __u64 seq;           /* attempt per (cpu,dest), before publication;
                         * a failed attempt consumes one sequence number */
    __u64 cookie;        /* backend lifetime g at emission; 0 means absent */
    __u64 q;             /* reserved backend sequence; 0 in mode N */
};

struct pgwt_wire_boundary {
    __u64 w;             /* cutoff clock read BEFORE reserve/output */
    __u32 round;         /* copied from transport_ctl.round */
    __u32 dest;          /* CPU, E (event_ringbuf), or L (lifecycle_rb) */
};

struct pgwt_transport_ctl {
    __u32 round;         /* daemon's completion round, copied by BPF */
    __u32 el_request;    /* daemon request for shared E/L boundaries */
};

/* CPU acknowledgements are boundaries with round above consumed E/L round.
 * Consumption position belongs to userspace and is never a wire field. */
#define PGWT_WIRE_OFFSET(type, field, value) \
    _Static_assert(__builtin_offsetof(struct type, field) == (value), \
                   #type "." #field " offset")
_Static_assert(sizeof(struct pgwt_wire_hdr) == 32, "wire header size");
PGWT_WIRE_OFFSET(pgwt_wire_hdr, version, 0);
PGWT_WIRE_OFFSET(pgwt_wire_hdr, kind, 2);
PGWT_WIRE_OFFSET(pgwt_wire_hdr, flags, 3);
PGWT_WIRE_OFFSET(pgwt_wire_hdr, payload_len, 4);
PGWT_WIRE_OFFSET(pgwt_wire_hdr, cpu, 6);
PGWT_WIRE_OFFSET(pgwt_wire_hdr, seq, 8);
PGWT_WIRE_OFFSET(pgwt_wire_hdr, cookie, 16);
PGWT_WIRE_OFFSET(pgwt_wire_hdr, q, 24);
_Static_assert(sizeof(struct pgwt_wire_boundary) == 16, "boundary size");
PGWT_WIRE_OFFSET(pgwt_wire_boundary, w, 0);
PGWT_WIRE_OFFSET(pgwt_wire_boundary, round, 8);
PGWT_WIRE_OFFSET(pgwt_wire_boundary, dest, 12);
_Static_assert(sizeof(struct pgwt_transport_ctl) == 8, "transport control size");
PGWT_WIRE_OFFSET(pgwt_transport_ctl, round, 0);
PGWT_WIRE_OFFSET(pgwt_transport_ctl, el_request, 4);
#undef PGWT_WIRE_OFFSET
#endif
