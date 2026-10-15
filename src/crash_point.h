/* crash_point.h — deterministic process death at a named point.
 *
 * #424 review finding: the two file handovers (rotation, and recovery's
 * archive) each have a window between renaming the trace and renaming its
 * open-interval checkpoint. A crash inside such a window is exactly the case
 * the durability work exists to survive, and it is also the case no external
 * `kill -9` can hit reliably — the window is microseconds wide and the test
 * would be a race, which is not a test.
 *
 * So the windows are named, and a process started with
 *
 *     PGWT_CRASH_AT=<name>
 *
 * dies at the first arrival at that point, by SIGKILL, the same way the OOM
 * killer or an impatient operator ends it: no atexit handlers, no stdio
 * flush, no close. The test then asserts what the NEXT startup makes of the
 * state left behind.
 *
 * WHY A TEST HOOK IS IN PRODUCTION CODE. It makes an otherwise unprovable
 * property provable. Both of the bugs this exists to catch — rotation
 * deleting a checkpoint before a successor existed, and recovery renaming the
 * trace before its sidecar — survived a green end-to-end gate, because the
 * only alternative to a hook is to CONSTRUCT the intermediate state in a test
 * and assert the next start resolves it. That proves the resolver handles a
 * state, never that the product actually passes through only resolvable
 * states, which is precisely the gap both bugs lived in. The hook closes it:
 * a real process really dies, mid-handover, at a point the product reached by
 * itself.
 *
 * SAFETY, by construction:
 *   - no variable set            → getenv returns NULL        → no-op
 *   - variable set to ""         → explicit empty check       → no-op
 *   - variable set to anything
 *     that is not an exact match → strcmp fails               → no-op
 *   There is NO default point, no prefix match, no "any", and no numeric
 *   index that could land on a point by accident: a crash happens only when a
 *   full string equals one of the four names below, which are spelled out as
 *   macros so a test cannot arm something a typo invented.
 *
 * COST. The points sit only in the rotation and recovery handovers, which run
 * at most once an hour and once per start, so the getenv is on no hot path.
 * The variable is read on every arrival rather than cached, so a test can arm
 * one point, let the child die, and have the next process — which does not
 * set it — run clean.
 */
#ifndef PGWT_CRASH_POINT_H
#define PGWT_CRASH_POINT_H

#include <signal.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

/* Named crash points. Kept here so a test cannot drift from the product by
 * misspelling one: both sides use these macros. */
#define PGWT_CRASH_ROTATE_AFTER_CKPT   "rotate_after_ckpt"
#define PGWT_CRASH_ROTATE_AFTER_TRACE  "rotate_after_trace"
#define PGWT_CRASH_RECOVER_AFTER_CKPT  "recover_after_ckpt"
#define PGWT_CRASH_RECOVER_AFTER_TRACE "recover_after_trace"

static inline void pgwt_crash_point(const char *name)
{
    const char *want = getenv("PGWT_CRASH_AT");
    if (want && *want && name && strcmp(want, name) == 0) {
        raise(SIGKILL);
        _exit(137);   /* unreachable; SIGKILL cannot be caught or blocked */
    }
}

#endif /* PGWT_CRASH_POINT_H */
