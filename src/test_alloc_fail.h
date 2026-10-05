/* test_alloc_fail.h — one-shot allocation-failure injection for tests.
 *
 * PGWT_TEST_ALLOC_FAIL=<point> makes the named allocation fail at the call
 * site that checks for it. Production never sets it. It exists because the
 * refusal paths it reaches are otherwise unreachable, and an unreachable
 * refusal is indistinguishable from one that approves — which is the shape
 * of both #275 and #276.
 *
 * Extracted verbatim from src/pid_index.h (#275) when src/triple_map.h
 * (#276) needed the same hook; pid_index.h now includes this header, so its
 * behaviour and its injection point name ("pid_index_grow") are unchanged.
 */
#ifndef PGWT_TEST_ALLOC_FAIL_H
#define PGWT_TEST_ALLOC_FAIL_H

#include <stdlib.h>
#include <string.h>

static inline int pgwt_test_alloc_fail(const char *point)
{
    const char *v = getenv("PGWT_TEST_ALLOC_FAIL");
    return v != NULL && strcmp(v, point) == 0;
}

#endif /* PGWT_TEST_ALLOC_FAIL_H */
