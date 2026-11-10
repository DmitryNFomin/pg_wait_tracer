/* Window-clipped time shared by server compute and offline replay. */
#ifndef PGWT_WINDOW_CLIP_H
#define PGWT_WINDOW_CLIP_H

#include "pg_wait_tracer.h"

/* A wait covers [timestamp - duration, timestamp]. Zero bounds mean
 * unbounded on that side. Impossible records (duration > timestamp) and
 * records wholly outside the window contribute zero, without underflow.
 * With both bounds zero, every valid record contributes its full duration. */
static inline uint64_t event_window_ns(const struct pgwt_trace_event *ev,
                                      uint64_t from_ns, uint64_t to_ns)
{
    if (ev->duration_ns > ev->timestamp_ns)
        return 0;
    uint64_t start = ev->timestamp_ns - ev->duration_ns;
    uint64_t end   = ev->timestamp_ns;
    if (from_ns != 0 && start < from_ns) start = from_ns;
    if (to_ns   != 0 && end   > to_ns)   end   = to_ns;
    return end > start ? end - start : 0;
}

/* Allocate measured CPU by cumulative integer endpoints. For the record
 * [s,e), F(t) = round_half_up(cpu_ns * (clamp(t,s,e) - s) / (e-s));
 * a window receives F(to) - F(from). Adjacent windows therefore telescope
 * to exactly the record's CPU, including ties and values above 2^53.
 * The 128-bit products and rounding additions cannot overflow: each factor
 * is at most UINT64_MAX, leaving room for duration_ns / 2 in the product.
 *
 * This is an additive integer accounting rule; it does not tell us where
 * inside the interval the CPU was actually used. Zero bounds are unbounded,
 * as in event_window_ns. The caller supplies a known CPU measurement.
 * Over-wall CPU is clamped to this window's wall; the optional integer-ns
 * diagnostic receives the excess, which is additive over partitions too.
 * A zero-duration record contributes no CPU time: its entire measurement
 * is excess, owned by the window containing its instant under [from,to).
 * This also applies with unbounded endpoints; empty windows own no instant. */
static inline uint64_t pgwt_cpu_clip_ns(const struct pgwt_trace_event *ev,
                                        uint64_t from_ns, uint64_t to_ns,
                                        uint64_t *clamped_ns)
{
    uint64_t dur = ev->duration_ns;
    if (dur == 0) {
        if (clamped_ns &&
            (from_ns == 0 || ev->timestamp_ns >= from_ns) &&
            (to_ns == 0 || ev->timestamp_ns < to_ns))
            *clamped_ns += ev->cpu_ns;
        return 0;
    }
    if (dur > ev->timestamp_ns) return 0;
    uint64_t s = ev->timestamp_ns - dur;
    uint64_t a = s, b = ev->timestamp_ns;
    if (from_ns != 0 && a < from_ns) a = from_ns;
    if (to_ns != 0 && b > to_ns) b = to_ns;
    if (b <= a) return 0;
    uint64_t fa = (uint64_t)(((__uint128_t)ev->cpu_ns * (a - s) + dur / 2) / dur);
    uint64_t fb = (uint64_t)(((__uint128_t)ev->cpu_ns * (b - s) + dur / 2) / dur);
    uint64_t share = fb - fa, wall = b - a;
    if (share > wall) {
        if (clamped_ns) *clamped_ns += share - wall;
        return wall;
    }
    return share;
}

/* Off-CPU is the nonnegative integer remainder of the CPU gap wall. Summary
 * window edges can carry more CPU than wall after open-gap credits are placed.
 * Preserve that excess in the optional integer-ns clamp diagnostic. */
static inline uint64_t pgwt_offcpu_ns(uint64_t gap_ns, uint64_t cpu_ns,
                                     uint64_t *clamped_ns)
{
    if (clamped_ns && cpu_ns > gap_ns)
        *clamped_ns += cpu_ns - gap_ns;
    return gap_ns > cpu_ns ? gap_ns - cpu_ns : 0;
}

#endif
