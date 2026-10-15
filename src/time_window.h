/* time_window.h — THE object the raw TIME reducers consume (#424, wiring)
 *
 * WHY THIS TYPE EXISTS AT ALL. src/open_overlay.c can synthesize the spans a
 * near-now window is missing, but an `attach` call on its own repairs
 * nothing: the six handlers that need live time would still reduce over a
 * bare `(events, count)` pair, so the overlay would be built, labelled and
 * then ignored. That is the failure mode this header is written against —
 * not "the overlay is wrong" but "the overlay was never consumed".
 *
 * So the records and the overlay travel TOGETHER, in one object, and the
 * records-only signatures of the time reducers are GONE from compute.h. A
 * handler that forgets the overlay no longer gets a silently short answer;
 * it gets a compile error, because `struct pgwt_trace_event *` is not a
 * `struct pgwt_window *`. Omission is prevented by the type system, not by
 * review.
 *
 * WHAT IT DOES NOT PREVENT, stated plainly so nobody over-trusts it: a
 * handler can still DELIBERATELY bind a window to a fail-closed overlay it
 * made itself. That is not an omission, and it is not silent either — such
 * an overlay carries status `unavailable` with the reason
 * "overlay not requested", and pgwt_overlay_add_json() renders that status on
 * the response. The type system stops the accident; the rendered status
 * stops the accident from being invisible.
 *
 * FAIL-CLOSED, IN PRODUCTION, NOT UNDER AN ASSERT. pgwt_window_bind() refuses
 * a NULL overlay, and pgwt_window_valid() refuses a zero-initialised window —
 * `(struct pgwt_window){0}` is the one bypass the compiler cannot catch, so
 * it is caught at run time, in release builds, by every reducer, which then
 * reports an ABSENT answer (`failed`) rather than an empty one.
 *
 * SPAN VISIBILITY IS GATED ON STATUS. pgwt_window_nspans() returns 0 unless
 * the overlay is INCLUDED. A reducer therefore cannot read spans out of an
 * overlay that was refused, went unavailable, or was never filled: there is
 * no code path in which a degraded overlay contributes time.
 */
#ifndef PGWT_TIME_WINDOW_H
#define PGWT_TIME_WINDOW_H

#include <stddef.h>
#include <stdint.h>

#include "open_overlay.h"
#include "pg_wait_tracer.h"

/* "WIND". Set only by pgwt_window_bind(); any other bit pattern — most
 * importantly all-zero — is an invalid window. */
#define PGWT_WINDOW_MAGIC 0x57494e44u

struct pgwt_window {
    uint32_t magic;
    const struct pgwt_trace_event *events;
    int      count;
    /* Never NULL in a bound window. Always initialised (pgwt_overlay_init
     * leaves it UNAVAILABLE), so there is no "uninitialised verdict" state. */
    const struct pgwt_overlay *overlay;
};

/* Bind records + overlay into one window. Returns 0 on success, -1 on
 * refusal, and a refused window is left invalid (magic cleared) so that a
 * caller which ignores the return value still cannot reduce over it.
 *
 * Refuses: no window, no overlay, a negative count, or a positive count with
 * no array behind it. */
static inline int pgwt_window_bind(struct pgwt_window *w,
                                   const struct pgwt_trace_event *events,
                                   int count,
                                   const struct pgwt_overlay *overlay)
{
    if (!w)
        return -1;
    w->magic   = 0;
    w->events  = NULL;
    w->count   = 0;
    w->overlay = NULL;
    if (!overlay)
        return -1;
    if (count < 0)
        return -1;
    if (count > 0 && !events)
        return -1;
    w->events  = events;
    w->count   = count;
    w->overlay = overlay;
    w->magic   = PGWT_WINDOW_MAGIC;
    return 0;
}

/* The production guard. A reducer calls this FIRST and reports absence when
 * it fails — never an empty result that looks like a measured zero. */
static inline int pgwt_window_valid(const struct pgwt_window *w)
{
    if (!w)
        return 0;
    if (w->magic != PGWT_WINDOW_MAGIC)
        return 0;
    if (!w->overlay)
        return 0;
    if (w->count < 0)
        return 0;
    if (w->count > 0 && !w->events)
        return 0;
    return 1;
}

static inline const struct pgwt_trace_event *
pgwt_window_events(const struct pgwt_window *w)
{
    return pgwt_window_valid(w) ? w->events : NULL;
}

static inline int pgwt_window_count(const struct pgwt_window *w)
{
    return pgwt_window_valid(w) ? w->count : 0;
}

/* 0 unless the overlay is INCLUDED: a refused / unavailable / unbuilt
 * overlay contributes nothing, by construction rather than by each reducer
 * remembering to check the status. */
static inline size_t pgwt_window_nspans(const struct pgwt_window *w)
{
    if (!pgwt_window_valid(w))
        return 0;
    if (w->overlay->status != PGWT_OVERLAY_INCLUDED)
        return 0;
    if (!w->overlay->spans)
        return 0;
    return w->overlay->count;
}

static inline const struct pgwt_overlay_span *
pgwt_window_span(const struct pgwt_window *w, size_t i)
{
    return i < pgwt_window_nspans(w) ? &w->overlay->spans[i] : NULL;
}

#endif /* PGWT_TIME_WINDOW_H */
