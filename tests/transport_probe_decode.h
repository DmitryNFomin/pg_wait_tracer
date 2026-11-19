/* libbpf's PERF_SAMPLE_RAW callback excludes the u32 advertised length,
 * but includes its alignment padding (80 wire bytes arrive as 84 bytes).
 * See fcda6b39:tests/bench_nmi_transport/reader.h. Padding is not wire data. */
#ifndef PGWT_TRANSPORT_PROBE_DECODE_H
#define PGWT_TRANSPORT_PROBE_DECODE_H
#include <stddef.h>
#include <stdio.h>
#include <string.h>
#include "../src/pgwt_transport_wire.h"
/* A request: `issued` is read before the helper is woken and `stamp` after
 * the wake call returned. A boundary: `stamp` is its w; `issued` is unused. */
struct probe_stamp { __u32 cpu, round; __u64 issued, stamp; };
/* -1: a boundary of this CPU's helper lies inside the unfinished producer's
 *     section, so the fence is falsified.
 *  0: no challenge evidence: refuse, never pass.
 *  1: the wake of this CPU's helper began and returned inside the section
 *     (so the helper was runnable on that CPU while the producer was still
 *     unfinished), and a boundary of this CPU carrying that request's round
 *     or a later one was stamped after the section ended (so the helper did
 *     run, its records do arrive, and it ran only afterwards).
 * A boundary from an OLDER round than the request is stale: it can have been
 * requested before the section and proves nothing. A later round is not
 * stale: rounds only grow, and the round is read by the helper's program
 * when it runs. Requiring the identical round would refuse correct runs
 * whenever the round advances between the wake and the helper's first run,
 * which is routine when the requester shares the helper's CPU. */
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
        if (r->cpu != cpu || r->issued < started || r->stamp >= ended) continue;
        for (size_t i = 0; i < nb; ++i) {
            const struct probe_stamp *b = &boundaries[i];
            if (b->cpu == cpu && b->round >= r->round && b->stamp > ended) return 1;
        }
    }
    return 0;
}
/* Per-pid instrumentation under connection churn. A backend can exit before
 * or during any attach step; that is an exit race, counted and reported, and
 * it must neither end the probe nor be mistaken for an attach failure.
 *
 * Liveness from /proc/<pid>/stat text: a task that is gone (no text), a
 * zombie or dead state, or one already exiting (PF_EXITING, flags bit 0x4:
 * the kernel refuses a perf event on it) is not live. The command name can
 * contain spaces and parentheses, so fields are read after the LAST ')'. */
static inline int probe_stat_live(const char *stat)
{
    if (!stat) return 0;
    const char *end = strrchr(stat, ')');
    if (!end) return 0;
    char state = 0; unsigned long flags = 0; int field[5];
    if (sscanf(end + 1, " %c %d %d %d %d %d %lu", &state, &field[0], &field[1],
               &field[2], &field[3], &field[4], &flags) != 7) return 0;
    return state != 'Z' && state != 'X' && state != 'x' && !(flags & 0x4UL);
}
/* 1: exit race. 0: a real failure on a live backend, which must fail the
 * probe. ESRCH (3) is the kernel itself reporting that the task is gone or
 * exiting. Any other error is the race only if the task is no longer live;
 * errno alone never excuses a failure on a live task. */
static inline int probe_attach_is_exit_race(int error, int live)
{
    return error == 3 /* ESRCH */ || !live;
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
