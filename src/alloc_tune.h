/* alloc_tune.h — glibc allocator tuning for pgwt-server.
 *
 * WHY: every analysis request builds 3-4 transient arrays of the whole
 * window's events (~274 MB each on a 7M-event, 900 s window: the cache->events
 * copy in server_load_events_fi_mode(), the exact-wins merge array, and the
 * loader's sort scratch). glibc serves blocks that large with mmap, which
 * hands back fresh zero pages; the process therefore pays kernel page-zeroing
 * (clear_page_erms) and a first-touch fault per 4 KB on EVERY request, plus
 * the same pages cold in memcpy. Measured on the gate box over a dense trace
 * (6,955,008 events, 900 s window, 4 warm calls, n=2 per config):
 * top_queries -16%, executions -27%, concurrency -24%, variants -28%,
 * exec_scatter -30%. tests/results/mallopt/mallopt_sweep.log.
 *
 * Keeping those blocks on the brk heap (M_MMAP_MAX = 0) and not handing the
 * heap back between requests (M_TRIM_THRESHOLD) lets the next request reuse
 * pages that are already faulted in. Both knobs are load-bearing and were
 * measured separately: M_MMAP_MAX alone, with the default 128 KB trim
 * threshold, recovers only a third of the saving (free() hands the top of the
 * heap straight back, so the next request faults it in again).
 *
 * COST: the process stops returning memory to the OS, so steady-state RSS
 * becomes the high-water mark of any single request. PGWT_ALLOC_TUNE=0 turns
 * the whole thing off at startup without a rebuild — the escape hatch for a
 * memory-constrained host.
 *
 * glibc only: mallopt()/M_MMAP_MAX are glibc extensions, and the project also
 * builds on macOS. Elsewhere pgwt_tune_allocator() is a documented no-op that
 * reports PGWT_ALLOC_TUNE_NONE.
 */
#ifndef PGWT_ALLOC_TUNE_H
#define PGWT_ALLOC_TUNE_H

#include <stdlib.h>

#ifdef __GLIBC__
#include <malloc.h>
#endif

/* Bits in pgwt_tune_allocator()'s return value: one per knob actually
 * accepted by the allocator. mallopt() returns 1 on success, 0 on failure,
 * and a partial result is a real outcome the caller must be able to see --
 * hence a bitmask rather than a bool. */
#define PGWT_ALLOC_TUNE_NONE      0
#define PGWT_ALLOC_TUNE_MMAP_MAX  (1 << 0)
#define PGWT_ALLOC_TUNE_TRIM      (1 << 1)
#define PGWT_ALLOC_TUNE_ALL       (PGWT_ALLOC_TUNE_MMAP_MAX | PGWT_ALLOC_TUNE_TRIM)

/* Never trim: the top chunk has to exceed this before free() calls brk back.
 * Sized above the largest heap this server is expected to hold so the
 * per-request arrays are never returned. mallopt() takes an int, so this is
 * also the practical ceiling (M_TRIM_THRESHOLD via the MALLOC_TRIM_THRESHOLD_
 * environment variable accepts wider values; the difference does not matter
 * once the threshold is past any real heap size). */
#define PGWT_ALLOC_TRIM_THRESHOLD (1536 * 1024 * 1024)

/* M_TOP_PAD is deliberately NOT set. Measured on the gate box (six tabs, four
 * warm calls, 900 s window, tests/results/mallopt/mallopt_sweep.log): adding
 * M_TOP_PAD=64 MB on top of the two knobs above changed no response time
 * beyond run-to-run spread and no RSS figure at all (1179 MB either way), and
 * only grew VmData by its own 64 MB. A knob that buys nothing is one more
 * thing to explain.
 */

/* Apply the tuning. Returns the bitmask of knobs that took effect:
 * PGWT_ALLOC_TUNE_ALL on a healthy glibc, PGWT_ALLOC_TUNE_NONE when disabled
 * by PGWT_ALLOC_TUNE=0 or when built against a non-glibc libc.
 *
 * MUST be called before the first large allocation: mallopt() changes how
 * subsequent allocations are served, it does not migrate existing ones. */
static inline int pgwt_tune_allocator(void)
{
    const char *off = getenv("PGWT_ALLOC_TUNE");
    if (off && off[0] == '0' && off[1] == '\0')
        return PGWT_ALLOC_TUNE_NONE;

#ifdef __GLIBC__
    int applied = PGWT_ALLOC_TUNE_NONE;
    if (mallopt(M_MMAP_MAX, 0))
        applied |= PGWT_ALLOC_TUNE_MMAP_MAX;
    if (mallopt(M_TRIM_THRESHOLD, PGWT_ALLOC_TRIM_THRESHOLD))
        applied |= PGWT_ALLOC_TUNE_TRIM;
    return applied;
#else
    return PGWT_ALLOC_TUNE_NONE;
#endif
}

#endif /* PGWT_ALLOC_TUNE_H */
