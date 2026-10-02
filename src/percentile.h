/* percentile.h — ascending sort + percentile pick for the Queries tab.
 *
 * WHY THIS EXISTS (#265). handle_top_queries used to sort each row's
 * exec_times/plan_times with a hand-written exchange sort purely to read
 * p95/p99 out of the sorted array:
 *
 *     for (a = 0; a < n-1; a++)
 *         for (b = a+1; b < n; b++)
 *             if (v[a] > v[b]) swap(v[a], v[b]);
 *
 * The sample arrays are capped at 10,000, and the demo workload puts every
 * pgbench statement at that cap, so that is ~5e7 branchy iterations per
 * array, two arrays per row, ~7 rows at the cap. Measured: top_queries
 * 2338 ms vs top_events 42 ms on the same capture/window/process.
 *
 * The replacement is qsort over plain doubles. The output must be
 * BIT-IDENTICAL: the arrays hold no satellite data, so order among equal
 * elements is unobservable, and the producer (handle_top_queries' marker
 * scan) admits a sample only when `ms >= 0`, which excludes NaN -- so the
 * ascending permutation of the multiset is unique down to the bit pattern.
 *
 * THE COMPARATOR TRAP: `return (int)(a - b)` truncates toward zero, so every
 * pair closer together than 1.0 compares equal. These arrays are execution
 * times in milliseconds and are routinely sub-millisecond, so that mistake
 * would silently leave them nearly unsorted and report a wrong percentile.
 * tests/test_top_queries_percentile.c pins exactly that case.
 */
#ifndef PGWT_PERCENTILE_H
#define PGWT_PERCENTILE_H

#include <stdint.h>
#include <stdlib.h>

/* Ascending order over doubles. Explicit -1/0/1 from sign comparisons, never
 * a truncated subtraction (see THE COMPARATOR TRAP above). */
static inline int pgwt_cmp_double_asc(const void *pa, const void *pb)
{
    double a = *(const double *)pa;
    double b = *(const double *)pb;
    if (a < b) return -1;
    if (a > b) return 1;
    return 0;
}

/* Sort v[0..n) ascending in place. n < 2 is a no-op. */
static inline void pgwt_sort_doubles_asc(double *v, int n)
{
    if (!v || n < 2)
        return;
    qsort(v, (size_t)n, sizeof(double), pgwt_cmp_double_asc);
}

/* Read the percentile element out of an already-sorted array.
 *
 * The index expression is UNCHANGED from the pre-qsort code, deliberately,
 * including the small-n guard: below min_n samples the caller reported the
 * maximum (index n-1) rather than a percentile. Callers pass
 * (pct=0.95, min_n=20) and (pct=0.99, min_n=100). Requires n >= 1; the
 * caller's own `n > 1` guard is what keeps the percentile fields off a row
 * that has no samples to speak of. */
static inline double pgwt_percentile_at(const double *sorted, int n,
                                        double pct, int min_n)
{
    return sorted[n > min_n ? (int)((n - 1) * pct) : n - 1];
}

/* ── uint64_t variants (#269, pgwt_compute_variants) ──────────────────────
 *
 * pgwt_compute_variants keeps per-variant execution times as uint64_t
 * nanoseconds and had the same O(n^2) exchange sort. The double comparator
 * above is NOT reusable for them: passing a uint64_t array through
 * pgwt_cmp_double_asc reinterprets the bits as IEEE doubles, which orders
 * them wrongly (and turns some values into NaN, for which every comparison
 * is false).
 *
 * THE COMPARATOR TRAP, UNSIGNED EDITION. `return (int)(a - b)` is worse here
 * than it is for doubles: a - b is computed in uint64_t, so 1 - 2 is
 * 18446744073709551615, and the cast to int keeps only the low 32 bits
 * (-1 here, but +1 for 1 - 4294967298). Nanosecond execution times routinely
 * differ by more than 2^31, so a subtraction comparator mis-orders real
 * data. Always the explicit -1/0/1 below.
 */
static inline int pgwt_cmp_u64_asc(const void *pa, const void *pb)
{
    uint64_t a = *(const uint64_t *)pa;
    uint64_t b = *(const uint64_t *)pb;
    if (a < b) return -1;
    if (a > b) return 1;
    return 0;
}

/* Sort v[0..n) ascending in place. n < 2 is a no-op. */
static inline void pgwt_sort_u64_asc(uint64_t *v, int n)
{
    if (!v || n < 2)
        return;
    qsort(v, (size_t)n, sizeof(uint64_t), pgwt_cmp_u64_asc);
}

#endif /* PGWT_PERCENTILE_H */
