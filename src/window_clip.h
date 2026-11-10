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

/* The shared CPU contract: prorate, clamp to the clipped gap wall, then round
 * once to integer ns. All consumers accumulate these integers before converting
 * to display units. clamped_ns is an optional measurement-skew diagnostic. */
static inline uint64_t pgwt_cpu_clip_ns(uint64_t cpu_ns, uint64_t clip_ns,
                                        uint64_t duration_ns, double *clamped_ns)
{
    if (clip_ns == 0 || duration_ns == 0) return 0;
    if (clip_ns == duration_ns && cpu_ns <= clip_ns) return cpu_ns;
    double frac = (double)clip_ns / (double)duration_ns;
    double measured = (double)cpu_ns * frac;
    if (measured >= (double)clip_ns) {
        if (clamped_ns) *clamped_ns += measured - (double)clip_ns;
        return clip_ns;
    }
    uint64_t rounded = (uint64_t)(measured + 0.5);
    return rounded < clip_ns ? rounded : clip_ns;
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
