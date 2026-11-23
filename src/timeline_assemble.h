/* timeline_assemble.h — streaming wait assembler of timeline mode (#1006).
 *
 * Turns the BPF wire records of timeline_wire.h (CALL, MARK, SEED, EXIT)
 * into the WAIT and LOSS records of timeline_format.h, with the rule
 * `reconstruct_mark` (markz) of the 2026-11-23 kill test:
 *   - a MARK opens a new wait;
 *   - a pending MARK followed by another word, another MARK or EXIT gives a
 *     ZERO_MARKER wait (end == start, no duration, n_syscalls 0);
 *   - plain IO-class calls (not sleep primitives) are one wait each unless
 *     the wait was opened by a MARK;
 *   - same-word LWLock calls merge only when the gap is <= the merge gap
 *     (20 us);
 *   - SEED gives TRUNC_START; EXIT flushes the pid;
 *   - the query id is the first call's; a zero-call wait takes the pid's
 *     last query id.
 *
 * Streaming: state per pid in a hash. Input is NOT assumed sorted across
 * pids (ring order); for one pid the ring order is program order.
 * Output goes to the emit callback in the order waits close; it is not
 * sorted by start time across pids.
 *
 * Status: S0 stub with the FROZEN prototypes; S1 implements.
 */
#ifndef PGWT_TIMELINE_ASSEMBLE_H
#define PGWT_TIMELINE_ASSEMBLE_H

#include <stddef.h>
#include <stdint.h>

#include "timeline_format.h"

/* Same-word LWLock calls further apart than this are separate waits. */
#define PGWT_TL_LWLOCK_MERGE_GAP_NS  20000ULL

/* Receives every closed record (WAIT, LOSS). Return 0, or a negative errno
 * that the assembler returns from the call that emitted (and counts). */
typedef int (*pgwt_tl_emit_fn)(void *ctx, const struct pgwt_tl_record *rec);

/* Asked by pgwt_tl_asm_flush_idle: does `pid` have a system call in flight
 * right now? Return 1 and set *t0_ns to its sys_enter time, 0 if not, or a
 * negative errno when it cannot tell (the wait is then kept open). */
typedef int (*pgwt_tl_inflight_fn)(void *ctx, uint32_t pid, uint64_t *t0_ns);

struct pgwt_tl_asm_config {
    pgwt_tl_emit_fn emit;          /* required */
    void           *emit_ctx;
    uint64_t        lwlock_merge_gap_ns;  /* 0 = PGWT_TL_LWLOCK_MERGE_GAP_NS */
    uint16_t        record_flags;  /* OR-ed into every WAIT: MARKER_OFF when
                                    * the uprobe is not attached,
                                    * QID_UNAVAILABLE when no query ids */
    uint16_t        reserved0;
    uint32_t        reserved1;
};

/* A system call still in flight at stop (read from the BPF per-pid state):
 * it becomes a wait with TRUNC_END ending at stop_ns. */
struct pgwt_tl_inflight {
    uint64_t t0_ns;       /* sys_enter */
    uint64_t qid;         /* as on the wire */
    uint32_t pid;
    uint32_t w;           /* wait_event_info read at sys_enter */
    uint32_t sc_id;       /* syscall number */
    uint16_t wire_flags;  /* PGWT_TL_WIRE_F_* as on the wire */
    uint16_t reserved;
};

struct pgwt_tl_asm_stats {
    uint64_t wire_records;    /* records fed */
    uint64_t wire_rejected;   /* unknown version/kind or short size */
    uint64_t waits;           /* WAIT records emitted (incl. zero-marker) */
    uint64_t zero_marker;     /* of which ZERO_MARKER */
    uint64_t loss_spans;      /* LOSS records emitted */
    uint64_t emit_failures;   /* emit callback returned < 0 */
    uint64_t pids_open;       /* pids with state right now */
};

struct pgwt_tl_assembler;

/* NULL on bad config (no emit) or allocation failure. */
struct pgwt_tl_assembler *pgwt_tl_asm_create(const struct pgwt_tl_asm_config *cfg);
void pgwt_tl_asm_destroy(struct pgwt_tl_assembler *a);

/* One wire record as drained from the ring (`size` = the ring sample size).
 * 0, -EINVAL for a record that is rejected (counted), or the emit error. */
int pgwt_tl_asm_feed(struct pgwt_tl_assembler *a, const void *rec, size_t size);

/* Close open waits that nothing can extend any more as of now_ns (an
 * LWLock wait whose merge gap has passed). Never closes a wait whose pid has
 * a call in flight per `in_flight` (amendment 5). in_flight may be NULL only
 * in tests that guarantee no call is in flight. Returns waits closed or a
 * negative errno. */
int pgwt_tl_asm_flush_idle(struct pgwt_tl_assembler *a, uint64_t now_ns,
                           pgwt_tl_inflight_fn in_flight, void *ctx);

/* Stop: turn `inflight` into TRUNC_END waits ending at stop_ns, then close
 * every open wait and pending marker. The assembler is empty afterwards.
 * 0 or a negative errno. */
int pgwt_tl_asm_finish(struct pgwt_tl_assembler *a, uint64_t stop_ns,
                       const struct pgwt_tl_inflight *inflight, size_t n_inflight);

void pgwt_tl_asm_get_stats(const struct pgwt_tl_assembler *a,
                           struct pgwt_tl_asm_stats *out);

/* 1 when sc_id is a blocking primitive PostgreSQL waits in, else 0.
 * x86_64 numbers, the kill test's set: 7 poll, 23 select, 35 nanosleep,
 * 65 semop, 202 futex, 220 semtimedop, 230 clock_nanosleep, 232 epoll_wait,
 * 270 pselect6, 271 ppoll, 281 epoll_pwait, 441 epoll_pwait2. */
int pgwt_tl_is_sleep_syscall(uint32_t sc_id);

#endif /* PGWT_TIMELINE_ASSEMBLE_H */
