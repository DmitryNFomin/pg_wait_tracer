/* test_top_queries_percentile.c — the Queries tab's p95/p99 sort (#265).
 *
 * WHAT CHANGED. handle_top_queries sorted each row's exec_times/plan_times
 * with a hand-written O(n^2) exchange sort purely to index out p95/p99. The
 * sample arrays cap at 10,000 and the demo workload sits at the cap, so that
 * was ~5e7 branchy iterations per array, two arrays per row, ~7 rows at the
 * cap: top_queries measured 2338 ms against top_events' 42 ms on the same
 * capture and window. It is now qsort (src/percentile.h).
 *
 * THE WHOLE SAFETY ARGUMENT IS "BIT-IDENTICAL". Sorting is sorting, so this
 * may not move a single reported number. Section 1 is therefore a
 * differential against a VERBATIM copy of the exchange sort that was
 * deleted, over adversarially generated arrays, compared with memcmp -- not
 * "close", not "same percentile", the same bytes.
 *
 * THE COMPARATOR TRAP. `return (int)(a - b)` truncates toward zero, so every
 * pair closer than 1.0 compares equal. These arrays hold execution times in
 * milliseconds and are routinely sub-millisecond, so that mistake leaves
 * them nearly unsorted while every coarse-grained fixture still passes.
 * Section 3 keeps a working copy of that wrong comparator and asserts the
 * fixture DISTINGUISHES it -- the fixture's power to see the bug is itself
 * under test, so it cannot rot into a fixture of 1.0-apart integers.
 *
 * SECTION 4 IS ABOUT FALSE NEGATIVES, not true positives. Every way this
 * check could be satisfied without checking anything: an oracle that does
 * not itself sort (two sides agreeing because neither did any work), a
 * fixture that was already ascending (a no-op "sort" passes), a fixture with
 * no sub-millisecond gaps (the comparator trap is invisible), a fixture with
 * no duplicates (comparator-returns-0 paths never taken), a fixture that
 * never reaches the 10,000 cap (the only size that matters in production),
 * and the n < 2 / NULL inputs where there is nothing to sort at all. Each
 * one asserts the specific fact, and the test REFUSES rather than passing
 * when it cannot see.
 *
 * Runs anywhere: pure in-memory arrays, a fixed-seed PRNG, no clock, no I/O,
 * no ordering assumptions. Nothing here can be flaky.
 */
#include "percentile.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

static int failures;

#define CHECK(cond, ...) do {                                           \
    if (!(cond)) {                                                      \
        failures++;                                                     \
        printf("  FAIL %s:%d: ", __FILE__, __LINE__);                   \
        printf(__VA_ARGS__);                                            \
        printf("\n");                                                   \
    }                                                                   \
} while (0)

/* ---------------------------------------------------------------------
 * The oracle: a VERBATIM copy of the exchange sort removed from
 * src/server.c handle_top_queries by #265. Do not "clean this up" -- its
 * only job is to be the code that used to run.
 * --------------------------------------------------------------------- */
static void reference_exchange_sort(double *v, int n)
{
    for (int a = 0; a < n - 1; a++)
        for (int b = a + 1; b < n; b++)
            if (v[a] > v[b]) {
                double tmp = v[a];
                v[a] = v[b];
                v[b] = tmp;
            }
}

/* The wrong comparator the contract warns about, kept so section 3 can
 * prove the fixture detects it. Never used by production code. */
static int cmp_double_truncating(const void *pa, const void *pb)
{
    double a = *(const double *)pa;
    double b = *(const double *)pb;
    return (int)(a - b);
}

/* Deterministic PRNG (xorshift64*). Fixed seed: this test must give the
 * same verdict on every machine and every run. */
static uint64_t rng_state;
static void rng_seed(uint64_t s) { rng_state = s ? s : 0x9e3779b97f4a7c15ULL; }
static uint64_t rng_next(void)
{
    uint64_t x = rng_state;
    x ^= x >> 12; x ^= x << 25; x ^= x >> 27;
    rng_state = x;
    return x * 0x2545f4914f6cdd1dULL;
}

static int is_ascending(const double *v, int n)
{
    for (int i = 1; i < n; i++)
        if (v[i - 1] > v[i]) return 0;
    return 1;
}

static int count_duplicates(const double *sorted, int n)
{
    int d = 0;
    for (int i = 1; i < n; i++)
        if (sorted[i - 1] == sorted[i]) d++;
    return d;
}

/* Adjacent pairs in the SORTED array that differ by less than 1.0 but are
 * not equal -- exactly the pairs a truncating comparator cannot order. */
static int count_submilli_gaps(const double *sorted, int n)
{
    int c = 0;
    for (int i = 1; i < n; i++) {
        double d = sorted[i] - sorted[i - 1];
        if (d > 0.0 && d < 1.0) c++;
    }
    return c;
}

/* Fixture shapes. `spread` is the modulus in microseconds, so small values
 * give heavy duplication and sub-millisecond neighbours -- i.e. real
 * execution times, not 1.0-apart integers. */
enum shape { SHAPE_RANDOM, SHAPE_REVERSE, SHAPE_SORTED, SHAPE_ALL_EQUAL,
             SHAPE_TINY_SPREAD };

static void fill(double *v, int n, enum shape s)
{
    switch (s) {
    case SHAPE_RANDOM:
        for (int i = 0; i < n; i++)
            v[i] = (double)(rng_next() % 2000000ULL) / 1e6;   /* 0..2 ms, us res */
        break;
    case SHAPE_REVERSE:
        for (int i = 0; i < n; i++)
            v[i] = (double)(n - i) / 1024.0;                  /* strictly descending, sub-ms steps */
        break;
    case SHAPE_SORTED:
        for (int i = 0; i < n; i++)
            v[i] = (double)i / 1024.0;
        break;
    case SHAPE_ALL_EQUAL:
        for (int i = 0; i < n; i++)
            v[i] = 0.375;
        break;
    case SHAPE_TINY_SPREAD:
        for (int i = 0; i < n; i++)
            v[i] = (double)(rng_next() % 97ULL) / 1000.0;     /* 0..0.096 ms */
        break;
    }
}

static const char *shape_name(enum shape s)
{
    switch (s) {
    case SHAPE_RANDOM:      return "random";
    case SHAPE_REVERSE:     return "reverse";
    case SHAPE_SORTED:      return "sorted";
    case SHAPE_ALL_EQUAL:   return "all-equal";
    case SHAPE_TINY_SPREAD: return "tiny-spread";
    }
    return "?";
}

/* Observed properties of the whole section-1 corpus, asserted in section 4.
 * If the corpus loses any of these, the differential still passes but has
 * stopped testing anything -- so the test must fail instead. */
static long corpus_arrays;
static long corpus_elements;
static long corpus_unsorted_inputs;   /* inputs not already ascending */
static long corpus_submilli_gaps;
static long corpus_duplicates;
static int  corpus_max_n;

/* =====================================================================
 * SECTION 1 — bit-identical against the deleted exchange sort
 * ===================================================================== */
static void section1_differential(void)
{
    static const int sizes[] = { 0, 1, 2, 3, 19, 20, 21, 64, 99, 100, 101,
                                 999, 1000, 4096, 10000 };
    static const enum shape shapes[] = { SHAPE_RANDOM, SHAPE_REVERSE,
                                         SHAPE_SORTED, SHAPE_ALL_EQUAL,
                                         SHAPE_TINY_SPREAD };
    printf("=== 1. bit-identical vs the removed exchange sort ===\n");
    rng_seed(0x5265616479ULL);

    for (size_t si = 0; si < sizeof(sizes) / sizeof(sizes[0]); si++) {
        int n = sizes[si];
        for (size_t hi = 0; hi < sizeof(shapes) / sizeof(shapes[0]); hi++) {
            enum shape sh = shapes[hi];
            /* The 10,000 case is the production size; repeat the cheap
             * sizes more often to widen the corpus. */
            int reps = (n > 4096) ? 1 : (n > 999 ? 4 : 32);
            for (int rep = 0; rep < reps; rep++) {
                size_t bytes = (size_t)(n ? n : 1) * sizeof(double);
                double *a = malloc(bytes), *b = malloc(bytes);
                if (!a || !b) { printf("  FAIL oom\n"); failures++; free(a); free(b); return; }
                fill(a, n, sh);
                memcpy(b, a, (size_t)n * sizeof(double));

                corpus_arrays++;
                corpus_elements += n;
                if (n > corpus_max_n) corpus_max_n = n;
                if (!is_ascending(a, n)) corpus_unsorted_inputs++;

                reference_exchange_sort(a, n);     /* what used to run */
                pgwt_sort_doubles_asc(b, n);       /* what runs now */

                /* The oracle must itself have sorted. Two sides that both
                 * did nothing would agree -- that is the false negative
                 * this guards. */
                CHECK(is_ascending(a, n),
                      "oracle did not sort: n=%d shape=%s", n, shape_name(sh));
                CHECK(memcmp(a, b, (size_t)n * sizeof(double)) == 0,
                      "NOT bit-identical: n=%d shape=%s rep=%d", n, shape_name(sh), rep);

                corpus_submilli_gaps += count_submilli_gaps(b, n);
                corpus_duplicates    += count_duplicates(b, n);
                free(a); free(b);
            }
        }
    }
    printf("  %ld arrays, %ld elements, max n=%d\n",
           corpus_arrays, corpus_elements, corpus_max_n);
}

/* =====================================================================
 * SECTION 2 — percentile index arithmetic at the guard boundaries
 *
 * Values are 0,1,2,... after sorting, so the returned value IS the index.
 * The expected numbers are the pre-#265 expressions evaluated by hand,
 * including their double-rounding quirks: n=21 gives (int)(20*0.95) = 19
 * (not 20), n=100 gives (int)(99*0.95) = 94, n=101 gives exactly 95 and 99.
 * These are pinned, not derived, so a change to the arithmetic shows up.
 * ===================================================================== */
static void section2_percentile_indices(void)
{
    struct { int n; int i95; int i99; } cases[] = {
        {     2,    1,    1 },   /* below both guards: max, max        */
        {     3,    2,    2 },
        {    20,   19,   19 },   /* n > 20 is false at exactly 20      */
        {    21,   19,   20 },   /* p95 engages; p99 still the max     */
        {   100,   94,   99 },   /* n > 100 is false at exactly 100    */
        {   101,   95,   99 },   /* both engage                        */
        {  1000,  949,  989 },
        { 10000, 9499, 9899 },   /* the production cap                 */
    };
    printf("=== 2. percentile index at the guard boundaries ===\n");
    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        int n = cases[i].n;
        double *v = malloc((size_t)n * sizeof(double));
        if (!v) { printf("  FAIL oom\n"); failures++; return; }
        /* Descending on input, so a missing sort cannot pass. */
        for (int k = 0; k < n; k++) v[k] = (double)(n - 1 - k);
        pgwt_sort_doubles_asc(v, n);

        double p95 = pgwt_percentile_at(v, n, 0.95, 20);
        double p99 = pgwt_percentile_at(v, n, 0.99, 100);
        CHECK(p95 == (double)cases[i].i95,
              "n=%d p95: got %.1f want %d", n, p95, cases[i].i95);
        CHECK(p99 == (double)cases[i].i99,
              "n=%d p99: got %.1f want %d", n, p99, cases[i].i99);
        /* p99 >= p95 always, and both inside the array. */
        CHECK(p99 >= p95, "n=%d p99 %.1f < p95 %.1f", n, p99, p95);
        CHECK(p95 >= 0.0 && p95 <= (double)(n - 1), "n=%d p95 out of range", n);
        free(v);
    }
    printf("  8 boundary sizes pinned\n");
}

/* =====================================================================
 * SECTION 3 — the comparator trap, and proof the fixture can see it
 * ===================================================================== */
static void section3_comparator(void)
{
    printf("=== 3. comparator: sign, not truncated subtraction ===\n");

    /* Sub-millisecond pairs must order strictly. (int)(a-b) returns 0 for
     * every one of these. */
    struct { double a, b; int want; } pairs[] = {
        { 0.4,       0.9,       -1 },
        { 0.9,       0.4,        1 },
        { 0.000001,  0.000002,  -1 },
        { 1.5,       1.5,        0 },
        { 0.0,       0.0,        0 },
        { 0.0,       1e-300,    -1 },
        { 123.75,    0.25,       1 },
    };
    for (size_t i = 0; i < sizeof(pairs) / sizeof(pairs[0]); i++) {
        int got = pgwt_cmp_double_asc(&pairs[i].a, &pairs[i].b);
        CHECK(got == pairs[i].want, "cmp(%g,%g) = %d, want %d",
              pairs[i].a, pairs[i].b, got, pairs[i].want);
        /* Antisymmetry, which qsort relies on. */
        int rev = pgwt_cmp_double_asc(&pairs[i].b, &pairs[i].a);
        CHECK(rev == -pairs[i].want, "cmp not antisymmetric at (%g,%g)",
              pairs[i].a, pairs[i].b);
    }

    /* Now prove the FIXTURE discriminates: the same sub-millisecond array
     * sorted with the truncating comparator must come out WRONG. If this
     * assertion ever fails it means the fixture has drifted to values far
     * enough apart that the trap is invisible -- the test would then be
     * green for the wrong reason, so it must go red here instead. */
    const int n = 512;
    double *good = malloc((size_t)n * sizeof(double));
    double *bad  = malloc((size_t)n * sizeof(double));
    if (!good || !bad) { printf("  FAIL oom\n"); failures++; free(good); free(bad); return; }
    rng_seed(0x747275636BULL);
    fill(good, n, SHAPE_TINY_SPREAD);
    memcpy(bad, good, (size_t)n * sizeof(double));

    pgwt_sort_doubles_asc(good, n);
    qsort(bad, (size_t)n, sizeof(double), cmp_double_truncating);

    CHECK(is_ascending(good, n), "correct comparator did not sort");
    CHECK(!is_ascending(bad, n),
          "FIXTURE BLIND: the truncating comparator sorted it correctly, so "
          "this test can no longer detect the (int)(a-b) bug");
    CHECK(memcmp(good, bad, (size_t)n * sizeof(double)) != 0,
          "FIXTURE BLIND: truncating and correct comparators agree bit-for-bit");

    /* And it is observable through the reported percentile, not merely in
     * the array -- a wrong order that never reaches p95 would not matter. */
    double p95_good = pgwt_percentile_at(good, n, 0.95, 20);
    double p95_bad  = pgwt_percentile_at(bad,  n, 0.95, 20);
    CHECK(p95_good != p95_bad,
          "FIXTURE BLIND: p95 identical (%.9f) under the broken comparator",
          p95_good);
    printf("  trap visible: p95 correct=%.6f truncating=%.6f\n", p95_good, p95_bad);
    free(good); free(bad);
}

/* =====================================================================
 * SECTION 4 — false negatives: every way this could pass blind
 * ===================================================================== */
static void section4_bypass(void)
{
    printf("=== 4. bypass suite (a gate that cannot see must refuse) ===\n");

    /* 4a. Empty and degenerate inputs: nothing to sort. These must be safe
     * AND must not be the only thing section 1 exercised. */
    pgwt_sort_doubles_asc(NULL, 0);
    pgwt_sort_doubles_asc(NULL, 10);          /* NULL with a positive n */
    double one[1] = { 7.25 };
    pgwt_sort_doubles_asc(one, 1);
    CHECK(one[0] == 7.25, "n=1 modified the element: %g", one[0]);
    pgwt_sort_doubles_asc(one, 0);
    CHECK(one[0] == 7.25, "n=0 modified the element: %g", one[0]);
    double two[2] = { 2.5, 1.5 };
    pgwt_sort_doubles_asc(two, 2);
    CHECK(two[0] == 1.5 && two[1] == 2.5, "n=2 not sorted: %g %g", two[0], two[1]);
    /* n=1 is below the caller's `n > 1` guard, so no percentile is reported
     * for it; n=2 is the first size that is. Pin that first size's value
     * rather than assuming the guard. */
    CHECK(pgwt_percentile_at(two, 2, 0.95, 20) == 2.5, "n=2 p95 is not the max");

    /* 4b. The corpus must actually have been unsorted on input. If every
     * fixture arrived ascending, a sort that does nothing passes. */
    CHECK(corpus_unsorted_inputs > 0,
          "CANNOT SEE: no section-1 input was out of order, so a no-op sort "
          "would pass (unsorted=%ld of %ld arrays)",
          corpus_unsorted_inputs, corpus_arrays);

    /* 4c. The corpus must contain sub-millisecond neighbours, or the
     * truncating-comparator bug is invisible to section 1. */
    CHECK(corpus_submilli_gaps > 0,
          "CANNOT SEE: no sub-millisecond adjacent pairs in the corpus, so "
          "(int)(a-b) would be bit-identical to the correct comparator");

    /* 4d. The corpus must contain equal elements, or the comparator's
     * return-0 path and qsort's equal-element handling are never taken --
     * and "order among equals is unobservable" is exactly the claim the
     * bit-identical argument rests on. */
    CHECK(corpus_duplicates > 0,
          "CANNOT SEE: no duplicate values in the corpus, so the equal-element "
          "path that the bit-identical argument depends on was never run");

    /* 4e. The corpus must reach the production cap. 10,000 is the only size
     * that motivated the change; a suite of 20-element arrays proves
     * nothing about it. */
    CHECK(corpus_max_n >= 10000,
          "CANNOT SEE: largest array was %d, never reached the 10,000 sample cap",
          corpus_max_n);

    /* 4f. Section 1 must have run at all. A differential over zero arrays
     * reports success without comparing anything. */
    CHECK(corpus_arrays >= 100,
          "CANNOT SEE: section 1 compared only %ld arrays", corpus_arrays);
    CHECK(corpus_elements > 100000,
          "CANNOT SEE: section 1 compared only %ld elements", corpus_elements);

    /* 4g. The oracle must be a real sort, independently of the subject.
     * Checked inline in section 1 per array; re-assert standalone so a
     * section-1 that skipped its loop cannot hide it. */
    double probe[5] = { 5.0, 1.0, 4.0, 1.0, 3.0 };
    reference_exchange_sort(probe, 5);
    CHECK(probe[0] == 1.0 && probe[1] == 1.0 && probe[2] == 3.0 &&
          probe[3] == 4.0 && probe[4] == 5.0,
          "the oracle itself does not sort -- section 1 compares two no-ops");

    /* 4h. memcmp is only a bit-identity check if doubles have no padding. */
    CHECK(sizeof(double) == 8, "double is not 8 bytes; memcmp is not a "
                               "bit-identity check on this platform");

    /* 4i. PROVE SECTION 1 CAN GO RED. The differential is the whole safety
     * argument, so "it passed" is worth nothing unless the same comparison,
     * on the same corpus, fails when the subject is wrong. Re-run section
     * 1's exact shapes with the truncating comparator substituted for the
     * subject and require a bit-level disagreement with the oracle. If this
     * cannot be made to fail, neither can section 1. */
    {
        static const int sizes[] = { 2, 3, 19, 21, 101, 1000, 10000 };
        long mismatches = 0, compared = 0;
        rng_seed(0x6D7574617465ULL);
        for (size_t i = 0; i < sizeof(sizes) / sizeof(sizes[0]); i++) {
            int n = sizes[i];
            double *a = malloc((size_t)n * sizeof(double));
            double *b = malloc((size_t)n * sizeof(double));
            if (!a || !b) { failures++; printf("  FAIL oom\n"); free(a); free(b); break; }
            fill(a, n, SHAPE_TINY_SPREAD);
            memcpy(b, a, (size_t)n * sizeof(double));
            reference_exchange_sort(a, n);
            qsort(b, (size_t)n, sizeof(double), cmp_double_truncating);
            compared++;
            if (memcmp(a, b, (size_t)n * sizeof(double)) != 0) mismatches++;
            free(a); free(b);
        }
        CHECK(compared == (long)(sizeof(sizes) / sizeof(sizes[0])),
              "mutation probe did not run on every size (%ld)", compared);
        CHECK(mismatches == compared,
              "SECTION 1 CANNOT GO RED: a deliberately broken sort matched "
              "the oracle bit-for-bit on %ld of %ld sizes",
              compared - mismatches, compared);
        printf("  mutation probe: broken sort caught on %ld/%ld sizes\n",
               mismatches, compared);
    }

    printf("  corpus: %ld arrays / %ld elements, %ld unsorted inputs, "
           "%ld sub-ms gaps, %ld duplicate pairs, max n=%d\n",
           corpus_arrays, corpus_elements, corpus_unsorted_inputs,
           corpus_submilli_gaps, corpus_duplicates, corpus_max_n);
}

int main(void)
{
    printf("test_top_queries_percentile — #265 qsort for the Queries tab\n\n");
    section1_differential();
    section2_percentile_indices();
    section3_comparator();
    section4_bypass();
    printf("\n%s (%d failure%s)\n", failures ? "FAILED" : "PASSED",
           failures, failures == 1 ? "" : "s");
    return failures ? 1 : 0;
}
