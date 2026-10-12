/* event_sort.h — the loader's stable sort of a merged event window.
 *
 * WHY THIS EXISTS. server_load_events_fi_mode() concatenates one contiguous
 * run per contributing trace file and then has to restore timestamp order:
 * per-CPU blocks interleave at their seams, and sample clipping can move a
 * delayed fragment before records that preceded its physical block. The
 * order is load-bearing — see WHO DEPENDS ON THE ORDER below — so the fix
 * has always been a STABLE sort on timestamp_ns with the left side winning
 * ties.
 *
 * It used to be a fixed-width bottom-up mergesort: ceil(log2 n) passes, each
 * copying all n 48-byte records. Measured on a real 900 s window of a dense
 * capture (5,699,738 events, one trace file, no sampled records): 1779-1955
 * ms per request, 29.88% of warm self time across the whole backend.
 *
 * WHAT THE INPUT ACTUALLY LOOKS LIKE. Measured on the same window: 11,790
 * maximal non-decreasing runs, mean length 483, max backward timestamp jump
 * 904 us, and the runs' key intervals are at most 3 deep. So the array is
 * not k-way-ordered by file (there was only one file) and it is not sorted
 * either; it is a long chain of long runs that overlap by about one record
 * at each seam. Summed over all 11,790 seams, only 23,853 records (0.004n)
 * are in the wrong place.
 *
 * THE ALGORITHM is therefore a natural chain merge, in place:
 *
 *   events[0..sorted) is the stable sort of input[0..sorted); because every
 *   record is placed exactly once, `sorted` is also the start of the next
 *   still-untouched input run. Find that run's extent, find the prefix tail
 *   whose keys outrank the run's first key, and merge just those two pieces.
 *   When the tail is exhausted the write cursor has caught up with the read
 *   cursor, so the rest of the run is ALREADY in place and is not touched.
 *
 * Cost on the window above: one pass of key reads plus 23,853 record moves —
 * 36 ms, against 1779 ms for the fixed-width pass, with byte-identical
 * output.
 *
 * WHY THE OUTPUT IS BIT-IDENTICAL, not merely "also sorted". A stable sort
 * is a function, not a choice: for a single key it has exactly one output,
 * the unique permutation that is non-decreasing and preserves input order
 * among equal keys. Both the old pass and this one are stable merges on
 * timestamp_ns, so they cannot differ. Concretely, at every step here the
 * left side holds strictly lower input indices than the right side (the
 * prefix is input[0..sorted), the run is input[sorted..run_end)), so
 * `<=` on the left preserves input order among equal keys.
 *
 * THE WORST CASE AND THE FALLBACK. A chain merge is O(n) on this data but
 * O(n^2) on a reversed array (n runs of one record, each tail the whole
 * prefix). So the chain phase carries a work budget of `count` tail records;
 * on exceeding it the function hands the array to the old fixed-width
 * mergesort and the whole function stays O(n log n). That hand-off is safe
 * for stability, not just for order: at the break point the prefix holds the
 * stable order of input[0..sorted) and the suffix is untouched input, so for
 * any two equal keys their relative position in the partially merged array
 * is still their input order — and a stable sort of that array is therefore
 * the stable sort of the original.
 *
 * WHO DEPENDS ON THE ORDER. Everything downstream of the loader reads the
 * window as a time-ordered stream: pgwt_tag_events walks it once to attach
 * PLAN/EXEC/CMD marker windows to the events that follow each marker,
 * pgwt_compute_executions opens and closes one row per EXEC_START/CMD_END
 * pair in stream order, the concurrency and transition passes accumulate
 * per-interval state, and the lifecycle/marker consumers rely on ties
 * between a marker and a same-timestamp event keeping their captured order.
 * Reordering equal keys silently re-attributes records between rows, which
 * is why this is a stable sort and why the test asserts the tie order
 * explicitly rather than only asserting "non-decreasing".
 *
 * tests/test_event_sort.c pins all of it, including every way the check
 * could stop being able to see a difference.
 */
#ifndef PGWT_EVENT_SORT_H
#define PGWT_EVENT_SORT_H

#include "pg_wait_tracer.h"

#include <limits.h>
#include <stdlib.h>
#include <string.h>

/* Test seam. Production passes NULL. `alloc_fail` forces the named
 * allocation to fail ("scratch" = the chain merge's overlap buffer,
 * "fallback" = the O(n log n) pass); `fell_back` and `moves` let a test
 * assert WHICH path ran, so it cannot pass by sorting nothing. */
struct pgwt_event_sort_hooks {
    int (*alloc_fail)(const char *point);
    int fell_back;
    long moves;
};

static inline int pgwt_event_sort_fail(const struct pgwt_event_sort_hooks *h,
                                       const char *point)
{
    return h && h->alloc_fail && h->alloc_fail(point);
}

/* The previous implementation, kept verbatim as the worst-case fallback:
 * fixed-width bottom-up stable mergesort on timestamp_ns, left wins ties. */
static inline int pgwt_event_sort_fixed_width(struct pgwt_trace_event *events,
                                              int count,
                                              struct pgwt_event_sort_hooks *h)
{
    if (count < 2)
        return 0;
    if (pgwt_event_sort_fail(h, "fallback"))
        return -1;
    struct pgwt_trace_event *tmp =
        (struct pgwt_trace_event *)malloc((size_t)count * sizeof(*tmp));
    if (!tmp)
        return -1;

    struct pgwt_trace_event *src = events;
    struct pgwt_trace_event *dst = tmp;
    for (size_t width = 1; width < (size_t)count; width *= 2) {
        for (size_t base = 0; base < (size_t)count; base += 2 * width) {
            size_t left = base;
            size_t mid = base + width < (size_t)count
                       ? base + width : (size_t)count;
            size_t right = mid;
            size_t end = base + 2 * width < (size_t)count
                       ? base + 2 * width : (size_t)count;
            size_t out = base;
            while (left < mid && right < end) {
                /* Left wins ties: stable marker/event ordering. */
                if (src[left].timestamp_ns <= src[right].timestamp_ns)
                    dst[out++] = src[left++];
                else
                    dst[out++] = src[right++];
            }
            while (left < mid) dst[out++] = src[left++];
            while (right < end) dst[out++] = src[right++];
        }
        struct pgwt_trace_event *swap = src;
        src = dst;
        dst = swap;
        if (width > (size_t)count / 2)
            break;
    }
    if (src != events)
        memcpy(events, src, (size_t)count * sizeof(*events));
    free(tmp);
    return 0;
}

/* Stable ascending sort on timestamp_ns. Returns 0, or -1 when an
 * allocation failed (the array is then still a permutation of the input,
 * partially merged; callers treat -1 as a hard error and do not render). */
static inline int pgwt_stable_sort_events(struct pgwt_trace_event *events,
                                          int count,
                                          struct pgwt_event_sort_hooks *h)
{
    if (count < 2)
        return 0;

    struct pgwt_trace_event *scratch = NULL;
    int scratch_cap = 0;
    /* Tail records the chain phase may move before giving up; keeps the
     * chain phase O(n) so the fallback bounds the whole function. */
    const long budget = count;
    long tail_total = 0;
    int fell_back = 0;

    /* Length of the already-merged prefix == start of the next input run. */
    int sorted = 1;
    while (sorted < count &&
           events[sorted - 1].timestamp_ns <= events[sorted].timestamp_ns)
        sorted++;

    while (sorted < count) {
        /* The next maximal non-decreasing run, still at its input position.
         * run_end > sorted always, so this loop cannot stall. */
        int run_end = sorted + 1;
        while (run_end < count &&
               events[run_end - 1].timestamp_ns <= events[run_end].timestamp_ns)
            run_end++;

        /* The prefix tail the merge can touch: records whose key is strictly
         * greater than the run's first key. At least one by construction —
         * a run starts precisely at an inversion. Scanned backwards because
         * it is ~1 record deep in practice. */
        uint64_t first = events[sorted].timestamp_ns;
        int lo = sorted;
        while (lo > 0 && events[lo - 1].timestamp_ns > first)
            lo--;
        int tail = sorted - lo;

        tail_total += tail;
        if (tail_total > budget) {
            fell_back = 1;
            break;
        }

        if (tail > scratch_cap) {
            int next = scratch_cap ? scratch_cap : 64;
            while (next < tail) {
                if (next > INT_MAX / 2) { next = tail; break; }
                next *= 2;
            }
            struct pgwt_trace_event *grown =
                pgwt_event_sort_fail(h, "scratch") ? NULL
                : (struct pgwt_trace_event *)
                  realloc(scratch, (size_t)next * sizeof(*scratch));
            if (!grown) {
                free(scratch);
                return -1;
            }
            scratch = grown;
            scratch_cap = next;
        }
        memcpy(scratch, events + lo, (size_t)tail * sizeof(*scratch));

        int a = 0, b = sorted, out = lo;
        while (a < tail && b < run_end) {
            /* Left wins ties: the prefix holds the lower input indices.
             * out == lo + a + (b - sorted) <= b, so a write never clobbers
             * an unread record. */
            if (scratch[a].timestamp_ns <= events[b].timestamp_ns)
                events[out++] = scratch[a++];
            else
                events[out++] = events[b++];
        }
        while (a < tail)
            events[out++] = scratch[a++];
        /* a == tail leaves out == b: the rest of the run is already in
         * place, which is where the O(n) comes from. */
        if (h)
            h->moves += tail + (b - sorted);
        sorted = run_end;
    }

    free(scratch);
    if (fell_back) {
        if (h)
            h->fell_back = 1;
        return pgwt_event_sort_fixed_width(events, count, h);
    }
    return 0;
}

#endif /* PGWT_EVENT_SORT_H */
