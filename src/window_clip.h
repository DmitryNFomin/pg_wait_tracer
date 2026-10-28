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

#endif
