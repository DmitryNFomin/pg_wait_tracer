/* test_window.h — TEST-ONLY window construction (#424 wiring).
 *
 * The raw time reducers take a `struct pgwt_window` (records + open-interval
 * overlay) and no longer accept a bare record array; see src/time_window.h
 * for why. A unit test that is not exercising the overlay still needs a
 * window, so TW(events, count) builds one whose overlay is the fail-closed
 * one pgwt_overlay_init() produces: status UNAVAILABLE, no spans, so
 * pgwt_window_nspans() is 0 and the reducer's answer is the closed-record
 * answer — bit-identical to the pre-#424 records-only call.
 *
 * This header is DELIBERATELY not available to src/: a handler must get its
 * window from the loader, which attaches a real overlay. Tests that DO
 * exercise the overlay build a real `struct pgwt_overlay` and call
 * pgwt_window_bind() directly (see tests/test_overlay_wiring.c).
 *
 * Header-only on purpose: every field is written directly, so no test has to
 * grow a link dependency on ../src/open_overlay.c to get a window.
 */
#ifndef PGWT_TEST_WINDOW_H
#define PGWT_TEST_WINDOW_H

#include <string.h>

#include "time_window.h"

/* The same fail-closed overlay pgwt_overlay_init() leaves behind. One
 * instance per translation unit; tests are single-threaded and never mutate
 * it. */
static struct pgwt_overlay pgwt_test_no_overlay__ = {
    .status = PGWT_OVERLAY_UNAVAILABLE,
    .reason = "overlay not requested",
    .spans  = NULL,
    .count  = 0,
};

static inline const struct pgwt_window *
pgwt_test_window__(struct pgwt_window *w,
                   const struct pgwt_trace_event *events, int count)
{
    /* Deliberately NOT papering over a bind refusal: if bind says no, the
     * window stays invalid and the reducer must report `failed`. A test that
     * expects a result will then fail loudly. */
    (void)pgwt_window_bind(w, events, count, &pgwt_test_no_overlay__);
    return w;
}

#define TW(events, count) \
    pgwt_test_window__(&(struct pgwt_window){0}, (events), (count))

#endif /* PGWT_TEST_WINDOW_H */
