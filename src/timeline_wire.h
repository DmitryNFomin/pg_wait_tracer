/* timeline_wire.h — BPF-to-userspace records of timeline mode (#1006 unit 1).
 *
 * The BPF side of `--mode timeline` emits these records into one dedicated
 * ring buffer (BPF_RB_NO_WAKEUP, drained by a userspace thread every 5 ms);
 * src/timeline_assemble.c turns them into the on-disk records of
 * src/timeline_format.h. This header is shared by BPF C and userspace C.
 *
 * Status: FROZEN FOR UNIT 1 ONLY. The on-disk format (timeline_format.h) is
 * the long-lived contract; this wire may change in a later unit, which is why
 * every record starts with a size and a version. A consumer must refuse (and
 * count) a record whose version it does not know or whose size is smaller
 * than the size of its kind; it must use hdr.size, never sizeof, to step.
 *
 * Clocks: every *_ns time is bpf_ktime_get_ns(), i.e. CLOCK_MONOTONIC ns.
 * All integers are native-endian (the ring never leaves the host); x86_64 and
 * aarch64 are little-endian, the same as the on-disk format.
 *
 * Ordering: the ring is shared by every CPU, so records of different pids
 * interleave arbitrarily. PostgreSQL processes are single-threaded, so for
 * ONE pid the ring order of CALL/MARK/SEED/EXIT is the program order; the
 * assembler relies on that and on nothing else (amendment 5: input is not
 * sorted across pids).
 */
#ifndef PGWT_TIMELINE_WIRE_H
#define PGWT_TIMELINE_WIRE_H

#include "pg_wait_tracer.h"   /* u8..u64 on both sides; PGWT_CPU_NS_UNKNOWN */

#define PGWT_TL_WIRE_VERSION 1

/* hdr.kind */
enum pgwt_tl_wire_kind {
    /* One system call that ENTERED with a nonzero wait word, emitted at its
     * sys_exit. [t0, t1] = sys_enter .. sys_exit. */
    PGWT_TL_WIRE_CALL = 1,
    /* Entry of WaitEventSetWait (uprobe), arg 5 = wait_event_info. */
    PGWT_TL_WIRE_MARK = 2,
    /* First sight of a pid at a sys_exit: the call entered before the
     * tracer attached. [t0, t1] = attach time .. that sys_exit. */
    PGWT_TL_WIRE_SEED = 3,
    /* The process exited (emitted only when pid == tgid, so an io_uring
     * worker thread never closes its backend). */
    PGWT_TL_WIRE_EXIT = 4,
};

/* hdr.flags bits (any kind) */
/* A raw record of this pid was lost (ring reserve failed) after the
 * previous record of this pid that did arrive: the per-pid loss bit, cleared
 * by the record that carries it. The assembler turns it into a LOSS span. */
#define PGWT_TL_WIRE_F_LOST_BEFORE      0x0001
/* CALL/SEED: the backend was inside a command (st_state running or
 * fastpath) when the query id was read at sys_enter. */
#define PGWT_TL_WIRE_F_IN_CMD           0x0002
/* CALL/SEED: the query id could not be read (no st_query_id, e.g. PG13, or
 * the read failed); qid is 0 and means nothing. */
#define PGWT_TL_WIRE_F_QID_UNAVAILABLE  0x0004

/* Written into task_cpu_ns / run_delay_ns while they are not measured
 * (all of unit 1). Never 0 for "not measured" (amendment 2). */
#define PGWT_TL_CPU_NS_UNKNOWN  PGWT_CPU_NS_UNKNOWN

/* Ring size in timeline mode: 64 MiB. The kill test measured 3.2 MB/s with
 * 32-byte records; a 72-byte CALL scales that to about 7.2 MB/s, i.e. about
 * 9 s of headroom against a 5 ms drain period. Outside timeline mode the ring
 * is set to the minimum at open. */
#define PGWT_TL_RING_BYTES  (64u * 1024u * 1024u)

struct pgwt_tl_wire_hdr {
    u16 size;      /* bytes of the whole record incl. this header */
    u8  version;   /* PGWT_TL_WIRE_VERSION */
    u8  kind;      /* enum pgwt_tl_wire_kind */
    u16 flags;     /* PGWT_TL_WIRE_F_* */
    u16 reserved;  /* 0 */
    u32 pid;       /* tgid of the PostgreSQL process */
    u32 tid;       /* thread that ran the event (== pid for a backend) */
};

struct pgwt_tl_wire_call {
    struct pgwt_tl_wire_hdr hdr;
    u64 t0;            /* sys_enter */
    u64 t1;            /* sys_exit */
    u64 qid;           /* query id read at sys_enter (see hdr.flags) */
    u64 task_cpu_ns;   /* cumulative task CPU ns at sys_exit (unit 3);
                        * PGWT_TL_CPU_NS_UNKNOWN in unit 1 */
    u64 run_delay_ns;  /* cumulative sched run_delay ns at sys_exit (unit 3);
                        * PGWT_TL_CPU_NS_UNKNOWN in unit 1 */
    u32 w;             /* wait_event_info read at sys_enter, nonzero */
    u32 ret;           /* syscall return value as (u32)(s32)ret: read it
                        * back as int32_t (-4 = EINTR) */
    u32 sc_id;         /* syscall number */
    u32 reserved;      /* 0 */
};

struct pgwt_tl_wire_mark {
    struct pgwt_tl_wire_hdr hdr;
    u64 t;             /* WaitEventSetWait entry */
    u64 task_cpu_ns;   /* as in CALL; PGWT_TL_CPU_NS_UNKNOWN in unit 1 */
    u64 run_delay_ns;  /* as in CALL; PGWT_TL_CPU_NS_UNKNOWN in unit 1 */
    u32 w;             /* arg 5 of WaitEventSetWait = wait_event_info */
    u32 reserved;      /* 0 */
};

struct pgwt_tl_wire_seed {
    struct pgwt_tl_wire_hdr hdr;
    u64 t0;            /* tracer attach time (start unknown: TRUNC_START) */
    u64 t1;            /* the sys_exit where the pid was first seen */
    u64 qid;           /* as in CALL (read at that sys_exit) */
    u32 w;             /* wait word read at that sys_exit (0 = none) */
    u32 sc_id;         /* syscall number of that exit */
};

struct pgwt_tl_wire_exit {
    struct pgwt_tl_wire_hdr hdr;
    u64 t;             /* process exit */
};

/* Sizes and offsets are the contract; a change here is a version bump. */
#define PGWT_TL_WIRE_OFFSET(type, field, value) \
    _Static_assert(__builtin_offsetof(struct type, field) == (value), \
                   #type "." #field " offset")
_Static_assert(sizeof(struct pgwt_tl_wire_hdr) == 16, "timeline wire header size");
PGWT_TL_WIRE_OFFSET(pgwt_tl_wire_hdr, size, 0);
PGWT_TL_WIRE_OFFSET(pgwt_tl_wire_hdr, version, 2);
PGWT_TL_WIRE_OFFSET(pgwt_tl_wire_hdr, kind, 3);
PGWT_TL_WIRE_OFFSET(pgwt_tl_wire_hdr, flags, 4);
PGWT_TL_WIRE_OFFSET(pgwt_tl_wire_hdr, reserved, 6);
PGWT_TL_WIRE_OFFSET(pgwt_tl_wire_hdr, pid, 8);
PGWT_TL_WIRE_OFFSET(pgwt_tl_wire_hdr, tid, 12);
_Static_assert(sizeof(struct pgwt_tl_wire_call) == 72, "timeline wire CALL size");
PGWT_TL_WIRE_OFFSET(pgwt_tl_wire_call, t0, 16);
PGWT_TL_WIRE_OFFSET(pgwt_tl_wire_call, t1, 24);
PGWT_TL_WIRE_OFFSET(pgwt_tl_wire_call, qid, 32);
PGWT_TL_WIRE_OFFSET(pgwt_tl_wire_call, task_cpu_ns, 40);
PGWT_TL_WIRE_OFFSET(pgwt_tl_wire_call, run_delay_ns, 48);
PGWT_TL_WIRE_OFFSET(pgwt_tl_wire_call, w, 56);
PGWT_TL_WIRE_OFFSET(pgwt_tl_wire_call, ret, 60);
PGWT_TL_WIRE_OFFSET(pgwt_tl_wire_call, sc_id, 64);
PGWT_TL_WIRE_OFFSET(pgwt_tl_wire_call, reserved, 68);
_Static_assert(sizeof(struct pgwt_tl_wire_mark) == 48, "timeline wire MARK size");
PGWT_TL_WIRE_OFFSET(pgwt_tl_wire_mark, t, 16);
PGWT_TL_WIRE_OFFSET(pgwt_tl_wire_mark, task_cpu_ns, 24);
PGWT_TL_WIRE_OFFSET(pgwt_tl_wire_mark, run_delay_ns, 32);
PGWT_TL_WIRE_OFFSET(pgwt_tl_wire_mark, w, 40);
PGWT_TL_WIRE_OFFSET(pgwt_tl_wire_mark, reserved, 44);
_Static_assert(sizeof(struct pgwt_tl_wire_seed) == 48, "timeline wire SEED size");
PGWT_TL_WIRE_OFFSET(pgwt_tl_wire_seed, t0, 16);
PGWT_TL_WIRE_OFFSET(pgwt_tl_wire_seed, t1, 24);
PGWT_TL_WIRE_OFFSET(pgwt_tl_wire_seed, qid, 32);
PGWT_TL_WIRE_OFFSET(pgwt_tl_wire_seed, w, 40);
PGWT_TL_WIRE_OFFSET(pgwt_tl_wire_seed, sc_id, 44);
_Static_assert(sizeof(struct pgwt_tl_wire_exit) == 24, "timeline wire EXIT size");
PGWT_TL_WIRE_OFFSET(pgwt_tl_wire_exit, t, 16);
_Static_assert(PGWT_TL_CPU_NS_UNKNOWN == 0xFFFFFFFFFFFFFFFFULL,
               "timeline CPU sentinel is all ones");
#undef PGWT_TL_WIRE_OFFSET

#endif /* PGWT_TIMELINE_WIRE_H */
