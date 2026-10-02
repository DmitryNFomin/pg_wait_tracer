/* test_variants_p95.c — pgwt_compute_variants' p95 sample set and sort.
 *
 * TWO DEFECTS, THE SAME FOUR LINES.
 *
 * #271 (correctness). Execution times were sampled only while
 * `exec_count <= 10000`, but the array was grown by doubling to a capacity of
 * 16384 with realloc(), which does not zero what it adds. The read count was
 * then `min(exec_count, exec_times_cap)` — 12332 for a variant with 12332
 * executions — so the sort and the p95 pick ran over 2332 slots that were
 * never written. Observed on a real capture as a p95 of ~9.1 years on the
 * Transitions tab. When the tail happens to hold zeros the value is not
 * garbage but still wrong: the zeros sort to the front and the pick lands on
 * a lower percentile of the real data. Both are pinned below.
 *
 * #269 (cost). The same lines used an O(n^2) exchange sort. It is now qsort
 * via src/percentile.h; the safety argument is that the output is
 * BIT-IDENTICAL, so section 3 is a differential against a verbatim copy of
 * the deleted sort.
 *
 * WHY NOT THE FIXTURE THE ISSUE ASKED FOR. "12000 identical durations,
 * assert p95 == that duration" CANNOT SEE #271 on a fresh heap: 10000 copies
 * of D plus 2000 zeros, picked at index 11400, is still D. Section 4.1
 * asserts that blindness explicitly, so nobody re-simplifies the fixture back
 * into one. What this file uses instead is 10000 DISTINCT durations in
 * scrambled arrival order, where the pick index shifts by exactly the number
 * of unwritten slots and the expected value is a closed form.
 *
 * SECTION 4 IS ABOUT FALSE NEGATIVES. Every way this check could be
 * satisfied without checking anything: a fixture blind to the bug, no
 * variants produced at all (empty input), a fixture that never reaches the
 * cap, an arrival order that is already sorted (a no-op "sort" passes), an
 * oracle that does not itself sort, the comparator trap, a mutation probe
 * proving the differential can go red, a transient allocation failure that
 * drops one sample (the case `min(exec_count, cap, 10000)` cannot represent),
 * and a total allocation failure where the samples are ABSENT rather than
 * wrong. The allocation cases use --wrap=realloc and REFUSE (fail) if the
 * interposer never fired, because an injection that silently does nothing is
 * a gate that cannot see.
 *
 * Runs anywhere: in-memory arrays, deterministic durations, no clock, no I/O,
 * no threads, no ordering assumptions. Nothing here can be flaky.
 */
#include "compute.h"
#include "wait_event.h"
#include "pg_wait_tracer.h"
#include "summary_reader.h"
#include "percentile.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

static int failures = 0;
static int checks   = 0;

#define CHECK(cond, msg) do {                                        \
    checks++;                                                        \
    if (!(cond)) { failures++;                                       \
        printf("  FAIL: %s (%s:%d)\n", msg, __FILE__, __LINE__); }   \
    else printf("  ok: %s\n", msg);                                  \
} while (0)

#define CHECK_U64(got, want, msg) do {                               \
    checks++;                                                        \
    uint64_t _g = (uint64_t)(got), _w = (uint64_t)(want);            \
    if (_g != _w) { failures++;                                      \
        printf("  FAIL: %s — got %llu, want %llu (%s:%d)\n", msg,    \
               (unsigned long long)_g, (unsigned long long)_w,       \
               __FILE__, __LINE__); }                                \
    else printf("  ok: %s (%llu)\n", msg, (unsigned long long)_g);   \
} while (0)

/* compute.c's only foreign symbol (summary streaming) — unused here. */
int pgwt_visit_summaries(const char *trace_dir, uint64_t from_wall_ns,
                         uint64_t to_wall_ns, pgwt_summary_visitor visitor,
                         void *ctx)
{
    (void)trace_dir; (void)from_wall_ns; (void)to_wall_ns;
    (void)visitor; (void)ctx;
    return -1;
}

/* ── realloc interposer (sections 4.7 and 4.8) ────────────────────────────
 * Linked with -Wl,--wrap=realloc, so every realloc in this binary — including
 * the ones inside the compute.c under test — arrives here. Disarmed by
 * default; both counters are asserted, so a build where --wrap silently did
 * not take effect FAILS instead of quietly passing. */
extern void *__real_realloc(void *ptr, size_t size);

static size_t fail_realloc_size   = 0;  /* fail the next realloc of this size */
static int    fail_all_reallocs   = 0;
static int    realloc_calls       = 0;
static int    realloc_failures    = 0;

void *__wrap_realloc(void *ptr, size_t size)
{
    realloc_calls++;
    if (fail_all_reallocs ||
        (fail_realloc_size != 0 && size == fail_realloc_size)) {
        if (!fail_all_reallocs)
            fail_realloc_size = 0;      /* transient: one failure only */
        realloc_failures++;
        return NULL;
    }
    return __real_realloc(ptr, size);
}

/* ── The oracle: a VERBATIM copy of the exchange sort deleted from
 * pgwt_compute_variants by #269. Do not "clean this up" — its only job is to
 * be the code that used to run. ── */
static void reference_exchange_sort(uint64_t *v, int n)
{
    for (int a = 0; a < n - 1; a++)
        for (int b = a + 1; b < n; b++)
            if (v[a] > v[b]) {
                uint64_t tmp = v[a];
                v[a] = v[b];
                v[b] = tmp;
            }
}

/* The unsigned comparator trap src/percentile.h warns about. Never used by
 * production code; section 4.5 proves the fixture distinguishes it. */
static int cmp_u64_truncating(const void *pa, const void *pb)
{
    uint64_t a = *(const uint64_t *)pa;
    uint64_t b = *(const uint64_t *)pb;
    return (int)(a - b);
}

/* A deliberately broken sort for the mutation probe (section 4.6): sorts
 * everything but the last element. */
static void mutated_sort(uint64_t *v, int n)
{
    if (n > 1)
        qsort(v, (size_t)n - 1, sizeof(uint64_t), pgwt_cmp_u64_asc);
}

/* ── Fixture vocabulary ─────────────────────────────────────────────────── */

#define CLASS_IO        0x0Au
#define EV(cls, n)      (((cls) << 24) | (n))
#define IO_READ         EV(CLASS_IO, 21)        /* non-idle, so it is sampled */
#define MARK_START      PGWT_MARKER_EXEC_START
#define MARK_END        PGWT_MARKER_EXEC_END

#define T0              1790778045790378496ULL
#define FIX_PID         4242u
#define FIX_QID         0x5EEDULL

/* The cap in src/compute.c (PGWT_VARIANT_MAX_SAMPLES). Kept as a literal on
 * purpose: if the production constant moves, these expectations must be
 * re-derived by hand, not silently follow it. */
#define CAP             10000

/* Duration of execution i, in ns.
 *
 * For i < CAP: ((i * 9967) mod CAP + 1) * 1000. 9967 is prime and coprime
 * with 10000, so over i in [0, CAP) this is a BIJECTION onto
 * {1000, 2000, ..., 10000000} — 10000 distinct values in scrambled arrival
 * order (section 4.4 asserts both the bijection and that the order is not
 * already ascending, so neither duplicates nor a no-op "sort" can pass).
 * 9967 specifically, out of the multipliers that scramble, because it puts
 * execution 8192 — the one section 4.7 makes the allocator drop — at rank
 * 9665, above the p95 index, so dropping it moves the p95 VALUE and not
 * only the sample count.
 *
 * For i >= CAP: (20000 + i) * 1000, strictly greater than every sampled
 * value, so an implementation that sampled past the cap would pick a visibly
 * larger p95. */
static uint64_t fixture_dur(int i)
{
    if (i >= CAP)
        return (uint64_t)(20000 + i) * 1000ULL;
    return (uint64_t)(((long)i * 9967L) % CAP + 1) * 1000ULL;
}

/* p95 of the full bijection: sorted[k] == (k+1)*1000, index (int)(10000*0.95)
 * == 9500, so the value is 9501 * 1000. */
#define EXPECT_P95_FULL  9501000ULL

/* What the pre-fix code reported for the 12000-execution fixture under the
 * kindest possible heap (unwritten tail reading as zeros): 2000 zeros sort to
 * the front, the pick at index 11400 lands on prefix[9400]. */
#define BUGGY_P95_12000  9401000ULL

/* Build nexec executions of ONE pattern: EXEC_START, one IO wait of
 * fixture_dur(i), EXEC_END. Caller frees. */
static struct pgwt_trace_event *build_events(int nexec, int *out_count)
{
    int n = nexec * 3;
    struct pgwt_trace_event *ev = calloc((size_t)n, sizeof(*ev));
    if (!ev) { printf("  FAIL: fixture calloc\n"); exit(2); }
    uint64_t ts = T0;
    int k = 0;
    for (int i = 0; i < nexec; i++) {
        uint64_t d = fixture_dur(i);
        ev[k].pid = FIX_PID; ev[k].timestamp_ns = ts;
        ev[k].old_event = MARK_START; ev[k].new_event = MARK_START;
        ev[k].query_id = FIX_QID; k++;
        ts += d;
        ev[k].pid = FIX_PID; ev[k].timestamp_ns = ts; ev[k].duration_ns = d;
        ev[k].old_event = IO_READ; ev[k].new_event = 0;
        ev[k].query_id = FIX_QID; k++;
        ts += 1000;
        ev[k].pid = FIX_PID; ev[k].timestamp_ns = ts;
        ev[k].old_event = MARK_END; ev[k].new_event = MARK_END;
        ev[k].query_id = FIX_QID; k++;
        ts += 1000;
    }
    *out_count = n;
    return ev;
}

/* Run the subject over nexec executions of the single fixture pattern. */
static void run_fixture(int nexec, struct pgwt_variants_result *out)
{
    int count = 0;
    struct pgwt_trace_event *ev = build_events(nexec, &count);
    pgwt_compute_variants(ev, count, NULL, 64, PGWT_PHASE_EXEC, out);
    free(ev);
}

/* ── Section 1: the #271 red — the sample set is what was written ───────── */

static void section1_truncation(void)
{
    printf("\n[1] p95 comes from the executions actually sampled\n");

    struct pgwt_variants_result r;
    run_fixture(12000, &r);

    /* Refuse rather than approve if the fixture produced nothing to look at:
     * every assertion below would otherwise be vacuous. */
    CHECK(r.num_variants == 1, "fixture yields exactly one variant");
    if (r.num_variants != 1) { free(r.variants); return; }

    struct pgwt_variant *v = &r.variants[0];
    CHECK_U64(v->exec_count, 12000, "exec_count counts every execution");
    CHECK_U64(v->p95_sample_n, CAP, "p95_sample_n is the cap, not exec_count");
    CHECK_U64(v->p95_ns, EXPECT_P95_FULL,
              "p95 == p95 of the first 10000 executions");

    /* Asserting the right answer differs from the buggy one is what makes
     * section 1 a detector and not a tautology. */
    CHECK(EXPECT_P95_FULL != BUGGY_P95_12000,
          "the buggy zero-tail answer (9401000) differs from the right one");
    free(r.variants);
}

/* ── Section 2: the boundary where the pick leaves the written region ───── */

static void section2_boundaries(void)
{
    printf("\n[2] boundaries around the %d-sample cap\n", CAP);

    /* 10526 is where (int)(n*0.95) first exceeds 9999 on the buggy read
     * count; 10001 is the first truncated size at all. Every one of these
     * must give the same answer: the p95 of the same 10000 samples. */
    const int sizes[] = { CAP, CAP + 1, 10525, 10526, 10600, 12000 };
    for (size_t s = 0; s < sizeof(sizes) / sizeof(sizes[0]); s++) {
        struct pgwt_variants_result r;
        char msg[128];
        run_fixture(sizes[s], &r);
        if (r.num_variants != 1) {
            snprintf(msg, sizeof(msg), "n=%d produced one variant", sizes[s]);
            CHECK(0, msg);
            free(r.variants);
            continue;
        }
        struct pgwt_variant *v = &r.variants[0];
        snprintf(msg, sizeof(msg), "n=%d: exec_count", sizes[s]);
        CHECK_U64(v->exec_count, sizes[s], msg);
        snprintf(msg, sizeof(msg), "n=%d: p95_sample_n", sizes[s]);
        CHECK_U64(v->p95_sample_n, CAP, msg);
        snprintf(msg, sizeof(msg), "n=%d: p95_ns", sizes[s]);
        CHECK_U64(v->p95_ns, EXPECT_P95_FULL, msg);
        free(r.variants);
    }
}

/* ── Section 3: qsort is bit-identical to the deleted exchange sort ─────── */

static uint64_t lcg_state = 0x2545F4914F6CDD1DULL;
static uint64_t lcg_next(void)
{
    lcg_state = lcg_state * 6364136223846793005ULL + 1442695040888963407ULL;
    return lcg_state >> 11;
}

static void fill_case(uint64_t *v, int n, int kind)
{
    for (int i = 0; i < n; i++) {
        switch (kind) {
        case 0: v[i] = lcg_next(); break;                  /* full range */
        case 1: v[i] = (uint64_t)(n - i) * 1000ULL; break; /* reverse sorted */
        case 2: v[i] = (uint64_t)(i + 1) * 1000ULL; break; /* already sorted */
        case 3: v[i] = 7ULL; break;                        /* all equal */
        case 4: v[i] = (uint64_t)(i % 3) * 4294967296ULL; break; /* 2^32 apart */
        case 5: v[i] = lcg_next() % 5; break;              /* many duplicates */
        default: v[i] = lcg_next() % 1000; break;
        }
    }
}

static void section3_differential(void)
{
    printf("\n[3] qsort output is byte-for-byte the exchange sort's\n");

    const int sizes[] = { 0, 1, 2, 3, 17, 999, CAP };
    int cases = 0, mismatches = 0, pick_mismatches = 0;
    for (size_t si = 0; si < sizeof(sizes) / sizeof(sizes[0]); si++) {
        int n = sizes[si];
        for (int kind = 0; kind <= 6; kind++) {
            uint64_t *a = calloc((size_t)(n ? n : 1), sizeof(uint64_t));
            uint64_t *b = calloc((size_t)(n ? n : 1), sizeof(uint64_t));
            fill_case(a, n, kind);
            memcpy(b, a, (size_t)n * sizeof(uint64_t));
            reference_exchange_sort(a, n);
            pgwt_sort_u64_asc(b, n);
            cases++;
            if (memcmp(a, b, (size_t)n * sizeof(uint64_t)) != 0) {
                mismatches++;
                printf("    n=%d kind=%d: sorted arrays differ\n", n, kind);
            }
            if (n > 0 && a[(int)(n * 0.95)] != b[(int)(n * 0.95)])
                pick_mismatches++;
            free(a); free(b);
        }
    }
    CHECK(cases == 49, "49 size x shape differentials were actually run");
    CHECK(mismatches == 0, "every differential is byte-identical");
    CHECK(pick_mismatches == 0, "every picked p95 element is identical");

    pgwt_sort_u64_asc(NULL, 10);     /* must not crash */
    pgwt_sort_u64_asc(NULL, 0);
    CHECK(1, "NULL / n<2 inputs are a no-op, not a crash");
}

/* ── Section 4: false negatives — every way this could pass blind ───────── */

static void section4_bypass(void)
{
    printf("\n[4] bypass suite: ways this check could see nothing\n");

    /* 4.1 The fixture the issue specified is BLIND. 10000 copies of D plus
     * 2000 zeros, picked at index 11400, is still D — so "12000 identical
     * durations, assert p95 == D" passes on the broken code. */
    {
        int n = 12000;
        uint64_t *old = calloc((size_t)n, sizeof(uint64_t));
        for (int i = 0; i < CAP; i++) old[i] = 777000ULL;   /* written */
        /* slots CAP..n-1 left as the zeros a fresh mapping supplies */
        pgwt_sort_u64_asc(old, n);
        CHECK_U64(old[(int)(n * 0.95)], 777000ULL,
                  "identical-duration fixture cannot see #271 (hence 10000 "
                  "distinct durations)");
        free(old);
    }

    /* 4.2 The same simulation with THIS file's fixture does see it: the
     * buggy read count picks a different value. */
    {
        int n = 12000;
        uint64_t *old = calloc((size_t)n, sizeof(uint64_t));
        for (int i = 0; i < CAP; i++) old[i] = fixture_dur(i);
        pgwt_sort_u64_asc(old, n);
        uint64_t buggy = old[(int)(n * 0.95)];
        CHECK_U64(buggy, BUGGY_P95_12000, "buggy read count picks prefix[9400]");
        CHECK(buggy != EXPECT_P95_FULL,
              "this fixture distinguishes the buggy pick from the right one");
        free(old);
    }

    /* 4.3 No variants at all: an empty stream must yield zero, and section 1
     * asserted the real fixture yields one — so a harness stuck at "nothing"
     * fails there. */
    {
        struct pgwt_variants_result r;
        struct pgwt_trace_event none;
        memset(&none, 0, sizeof(none));
        pgwt_compute_variants(&none, 0, NULL, 64, PGWT_PHASE_EXEC, &r);
        CHECK(r.num_variants == 0 && r.total_executions == 0,
              "empty stream yields no variants (not a stale/odd value)");
        free(r.variants);
    }

    /* 4.4 Below the cap there must be NO truncation, and the arrival order
     * must not already be ascending — otherwise a no-op "sort" would pass. */
    {
        struct pgwt_variants_result r;
        run_fixture(100, &r);
        CHECK(r.num_variants == 1, "100-execution fixture yields one variant");
        if (r.num_variants == 1) {
            uint64_t want[100];
            CHECK_U64(r.variants[0].p95_sample_n, 100,
                      "p95_sample_n == exec_count below the cap");
            /* durations are fixture_dur(0..99), a subset of the bijection;
             * the expectation comes from the independent oracle. */
            for (int i = 0; i < 100; i++) want[i] = fixture_dur(i);
            reference_exchange_sort(want, 100);
            CHECK_U64(r.variants[0].p95_ns, want[95],
                      "untruncated p95 matches the oracle");
        }
        free(r.variants);

        /* Duplicates would blunt every index-shift assertion above, so the
         * fixture's distinctness is itself under test. */
        char *seen = calloc(CAP + 1, 1);
        int dups = 0;
        for (int i = 0; i < CAP; i++) {
            uint64_t rank = fixture_dur(i) / 1000ULL;
            if (rank < 1 || rank > CAP || seen[rank]) dups++;
            else seen[rank] = 1;
        }
        free(seen);
        CHECK(dups == 0, "the 10000 sampled durations are all distinct");

        int descents = 0;
        for (int i = 1; i < CAP; i++)
            if (fixture_dur(i) < fixture_dur(i - 1)) descents++;
        CHECK(descents > 1000, "arrival order is scrambled, so a no-op sort "
                               "cannot pass");
        CHECK(fixture_dur(9500) != EXPECT_P95_FULL,
              "picking without sorting gives a different value");
    }

    /* 4.5 The oracle does real work, and the comparator trap is visible. */
    {
        uint64_t a[64], b[64], c[64];
        fill_case(a, 64, 1);                 /* reverse sorted */
        memcpy(b, a, sizeof(a));
        reference_exchange_sort(a, 64);
        CHECK(memcmp(a, b, sizeof(a)) != 0,
              "the oracle actually reorders (not two idle sides agreeing)");

        fill_case(a, 64, 4);                 /* values 2^32 apart */
        memcpy(b, a, sizeof(a)); memcpy(c, a, sizeof(a));
        reference_exchange_sort(a, 64);
        qsort(c, 64, sizeof(uint64_t), cmp_u64_truncating);
        CHECK(memcmp(a, c, sizeof(a)) != 0,
              "fixture exposes the (int)(a-b) comparator trap");
        pgwt_sort_u64_asc(b, 64);
        CHECK(memcmp(a, b, sizeof(a)) == 0,
              "the real comparator gets that same fixture right");
    }

    /* 4.6 Mutation probe: the section-3 differential can go red. */
    {
        uint64_t a[256], b[256];
        fill_case(a, 256, 0);
        memcpy(b, a, sizeof(a));
        reference_exchange_sort(a, 256);
        mutated_sort(b, 256);
        CHECK(memcmp(a, b, sizeof(a)) != 0,
              "a broken sort is caught by the same comparison");
    }

    /* 4.7 A TRANSIENT allocation failure drops one sample. This is the case
     * `min(exec_count, cap, 10000)` cannot represent: it would still claim
     * 10000 samples and read one slot that was never written. */
    {
        struct pgwt_variants_result r;
        realloc_calls = 0; realloc_failures = 0;
        fail_realloc_size = (size_t)CAP * sizeof(uint64_t);  /* the last grow */
        run_fixture(CAP, &r);
        fail_realloc_size = 0;

        CHECK(realloc_calls > 0,
              "--wrap=realloc is on the path (else this section sees nothing)");
        CHECK_U64(realloc_failures, 1, "exactly one allocation failure injected");
        CHECK(r.num_variants == 1, "fixture still yields one variant");
        if (realloc_failures == 1 && r.num_variants == 1) {
            uint64_t *want = calloc(CAP - 1, sizeof(uint64_t));
            int w = 0;
            CHECK_U64(r.variants[0].exec_count, CAP,
                      "every execution still counted");
            CHECK_U64(r.variants[0].p95_sample_n, CAP - 1,
                      "one sample dropped, and the count says so");
            /* Expectation from the oracle: the bijection minus the duration
             * of the execution in flight when realloc failed (the 8193rd,
             * where the array had to grow from 8192 to the cap). That
             * duration is at rank 9665, above the p95 index, so the answer
             * must MOVE — a p95 still equal to the untruncated one would
             * mean the dropped sample was silently counted. */
            for (int i = 0; i < CAP; i++)
                if (i != 8192) want[w++] = fixture_dur(i);
            reference_exchange_sort(want, CAP - 1);
            CHECK(want[(int)((CAP - 1) * 0.95)] != EXPECT_P95_FULL,
                  "dropping that sample is observable in the p95 value");
            CHECK_U64(r.variants[0].p95_ns, want[(int)((CAP - 1) * 0.95)],
                      "p95 is over the 9999 contiguous samples, no hole");
            free(want);
        }
        free(r.variants);
    }

    /* 4.8 The samples ABSENT rather than wrong: every allocation fails, so
     * there is no array at all. The answer must be an honest zero sample
     * count, not a read of nothing. */
    {
        struct pgwt_variants_result r;
        realloc_calls = 0; realloc_failures = 0;
        fail_all_reallocs = 1;
        run_fixture(100, &r);
        fail_all_reallocs = 0;

        CHECK(realloc_failures > 0, "allocation failures were injected");
        CHECK(r.num_variants == 1, "variant still reported without samples");
        if (r.num_variants == 1) {
            CHECK_U64(r.variants[0].exec_count, 100, "exec_count unaffected");
            CHECK_U64(r.variants[0].p95_sample_n, 0, "p95_sample_n == 0");
            CHECK_U64(r.variants[0].p95_ns, 0, "p95 is 0, not indeterminate");
        }
        free(r.variants);
    }
}

int main(void)
{
    printf("=== test_variants_p95 (#271 sample set, #269 sort) ===\n");
    section1_truncation();
    section2_boundaries();
    section3_differential();
    section4_bypass();
    printf("\n%d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
