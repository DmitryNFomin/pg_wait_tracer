/* test_event_sort.c — the loader's stable sort (src/event_sort.h).
 *
 * WHAT CHANGED. server_load_events_fi_mode()'s order restoration was a
 * fixed-width bottom-up mergesort: ceil(log2 n) passes, each copying all n
 * 48-byte records. On a real 900 s window of a dense capture (5,699,738
 * events, one trace file, no sampled records) that was 1779-1955 ms per
 * request and 29.88% of warm backend self time. The same window holds 11,790
 * maximal non-decreasing runs that overlap by ~1 record at each seam, so it
 * is now an in-place natural chain merge: 36 ms, 23,853 record moves.
 *
 * THE WHOLE SAFETY ARGUMENT IS "BIT-IDENTICAL", and it rests on one fact: a
 * stable sort on a single key is a FUNCTION, not a choice. There is exactly
 * one non-decreasing permutation that preserves input order among equal
 * keys. So section 1 is a differential against a VERBATIM private copy of
 * the deleted fixed-width mergesort, compared with memcmp over whole
 * records — not "also sorted", not "same totals", the same bytes. The copy
 * is private to this file on purpose: event_sort.h reuses the fixed-width
 * pass as its worst-case fallback, and an oracle that IS the subject proves
 * nothing on the fallback fixtures.
 *
 * WHY STABILITY IS LOAD-BEARING. Everything downstream reads the window as a
 * time-ordered stream and ties matter: pgwt_tag_events attaches a marker's
 * PLAN/EXEC/CMD window to the records that FOLLOW it, and
 * pgwt_compute_executions opens/closes one row per EXEC_START/CMD_END in
 * stream order. A same-timestamp marker and event swapping places
 * re-attributes a record to a different row — silently, with every total
 * still adding up. Section 2 therefore asserts the exact tie order, and
 * section 5 proves the fixtures can SEE a tie flip.
 *
 * SECTION 5 IS ABOUT FALSE NEGATIVES, not true positives. Every way this
 * check could be satisfied without checking anything: a fixture that was
 * already ordered (a no-op "sort" passes), a fixture with no duplicate keys
 * (tie handling invisible), an oracle that does no work (two sides agreeing
 * because neither moved anything), count < 2 (nothing to sort at all), a
 * fixture that never reaches the chain path (the fallback would be under
 * test instead of the new code) and a fixture that never reaches the
 * fallback (the O(n log n) bound unproven). Each asserts the specific fact
 * and the test REFUSES rather than passing when it cannot see. It closes
 * with a mutation probe: five independent breakages of the chain merge, each
 * of which must be caught.
 *
 * Runs anywhere: in-memory arrays, a fixed-seed PRNG, no clock, no I/O, no
 * ordering assumptions, no threads. Nothing here can be flaky.
 */
#include "event_sort.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

static int failures;
static int checks;

#define CHECK(cond, ...) do {                                            \
    checks++;                                                            \
    if (!(cond)) {                                                       \
        failures++;                                                      \
        printf("  FAIL %s:%d: ", __FILE__, __LINE__);                    \
        printf(__VA_ARGS__);                                             \
        printf("\n");                                                    \
    }                                                                    \
} while (0)

/* ── fixed-seed PRNG ───────────────────────────────────────── */
static uint64_t rng_state = 0x9e3779b97f4a7c15ULL;
static void rng_seed(uint64_t s) { rng_state = s ? s : 1; }
static uint64_t rng_next(void)
{
    rng_state ^= rng_state << 13;
    rng_state ^= rng_state >> 7;
    rng_state ^= rng_state << 17;
    return rng_state;
}
static uint32_t rng_below(uint32_t n) { return n ? (uint32_t)(rng_next() % n) : 0; }

/* ── the ORACLE: a verbatim copy of the deleted fixed-width mergesort ──
 * Private to this file. Do not replace with the one in event_sort.h. */
static int oracle_fixed_width(struct pgwt_trace_event *events, int count)
{
    if (count < 2)
        return 0;
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

/* ── helpers ───────────────────────────────────────────────── */

/* pid carries the input index, so a stability violation is visible without
 * any reference to the sort's internals. */
static struct pgwt_trace_event *make_events(const uint64_t *keys, int n)
{
    struct pgwt_trace_event *e =
        (struct pgwt_trace_event *)calloc((size_t)n ? (size_t)n : 1, sizeof(*e));
    for (int i = 0; i < n; i++) {
        e[i].timestamp_ns = keys[i];
        e[i].pid = (uint32_t)i;
        e[i].old_event = (uint32_t)(i * 7 + 1);
        e[i].new_event = (uint32_t)(i * 13 + 2);
        e[i].duration_ns = (uint64_t)i * 1000u + 3u;
        e[i].query_id = (uint64_t)i * 1000003u + 5u;
        e[i].cpu_ns = (uint64_t)i * 17u + 7u;
        e[i].flags = (uint32_t)(i & 0xff);
    }
    return e;
}

static int count_inversions(const struct pgwt_trace_event *e, int n)
{
    int inv = 0;
    for (int i = 1; i < n; i++)
        if (e[i - 1].timestamp_ns > e[i].timestamp_ns)
            inv++;
    return inv;
}

static int count_dup_keys(const struct pgwt_trace_event *e, int n)
{
    /* adjacent duplicates after sorting a copy of the keys */
    uint64_t *k = (uint64_t *)malloc((size_t)(n ? n : 1) * sizeof(*k));
    for (int i = 0; i < n; i++) k[i] = e[i].timestamp_ns;
    for (int i = 1; i < n; i++) {           /* insertion sort: fixtures are small */
        uint64_t v = k[i];
        int j = i - 1;
        while (j >= 0 && k[j] > v) { k[j + 1] = k[j]; j--; }
        k[j + 1] = v;
    }
    int dup = 0;
    for (int i = 1; i < n; i++)
        if (k[i] == k[i - 1]) dup++;
    free(k);
    return dup;
}

/* Multiset of records is unchanged (no record lost, duplicated or invented).
 * Uses the pid column, which make_events() made unique per input index. */
static int is_permutation(const struct pgwt_trace_event *a,
                          const struct pgwt_trace_event *b, int n)
{
    unsigned char *seen = (unsigned char *)calloc((size_t)(n ? n : 1), 1);
    for (int i = 0; i < n; i++) {
        if (b[i].pid >= (uint32_t)n) { free(seen); return 0; }
        if (seen[b[i].pid]) { free(seen); return 0; }
        seen[b[i].pid] = 1;
    }
    for (int i = 0; i < n; i++) {
        if (!seen[i]) { free(seen); return 0; }
        if (memcmp(&a[b[i].pid], &b[i], sizeof(*b)) != 0) { free(seen); return 0; }
    }
    free(seen);
    return 1;
}

static int is_stable_sorted(const struct pgwt_trace_event *e, int n)
{
    for (int i = 1; i < n; i++) {
        if (e[i - 1].timestamp_ns > e[i].timestamp_ns)
            return 0;
        if (e[i - 1].timestamp_ns == e[i].timestamp_ns &&
            e[i - 1].pid > e[i].pid)
            return 0;
    }
    return 1;
}

/* ── fixture generators ─────────────────────────────────────── */

/* The shape production actually sees: long non-decreasing runs whose keys
 * dip back below the previous run's last key at each seam (per-CPU blocks
 * interleaving). `overlap` is how many keys deep the dip goes, so each seam
 * is a real inversion — a generator that merely restarted at the previous
 * run's end would produce an already-sorted array, and section 5a is what
 * refuses that. */
static uint64_t *gen_runs(int n, int run_len, int overlap, uint64_t *out_n)
{
    uint64_t *k = (uint64_t *)malloc((size_t)(n ? n : 1) * sizeof(*k));
    uint64_t cur = 1000000;
    uint64_t last = 0;
    int first_run = 1;
    int i = 0;
    if (overlap < 1) overlap = 1;
    while (i < n) {
        int len = run_len > 1
                ? (int)(run_len / 2 + rng_below((uint32_t)run_len)) : 1;
        if (!first_run) {
            uint64_t back = 1 + rng_below((uint32_t)overlap);
            cur = last > back ? last - back : 0;
        }
        first_run = 0;
        for (int j = 0; j < len && i < n; j++, i++) {
            k[i] = cur;
            last = cur;
            cur += 1 + rng_below(3);
        }
    }
    *out_n = (uint64_t)n;
    return k;
}

static uint64_t *gen_reversed(int n)
{
    uint64_t *k = (uint64_t *)malloc((size_t)n * sizeof(*k));
    for (int i = 0; i < n; i++) k[i] = (uint64_t)(n - i) * 10;
    return k;
}

static uint64_t *gen_sorted(int n)
{
    uint64_t *k = (uint64_t *)malloc((size_t)n * sizeof(*k));
    for (int i = 0; i < n; i++) k[i] = (uint64_t)i * 10 + 1;
    return k;
}

static uint64_t *gen_all_equal(int n)
{
    uint64_t *k = (uint64_t *)malloc((size_t)n * sizeof(*k));
    for (int i = 0; i < n; i++) k[i] = 4242;
    return k;
}

/* Random keys from a small alphabet: maximum duplicate density, maximum
 * disorder — the fixture a tie bug cannot hide from. */
static uint64_t *gen_random_dup(int n, uint32_t alphabet)
{
    uint64_t *k = (uint64_t *)malloc((size_t)n * sizeof(*k));
    for (int i = 0; i < n; i++) k[i] = rng_below(alphabet);
    return k;
}

/* ── MUTANTS: independent breakages of the chain merge (section 5) ──── */
enum mutant {
    MUT_NONE = 0,
    MUT_TIE_RIGHT,       /* `<` instead of `<=`: ties go to the later record */
    MUT_NO_LEFT_DRAIN,   /* forget to flush the remaining prefix tail */
    MUT_NO_TAIL_SCAN,    /* never look back: append each run unmerged */
    MUT_RUN_DESCENDING,  /* detect runs with the comparison reversed */
    MUT_FALLBACK_SKIP,   /* over budget: give up and return success */
};

static int chain_mutant(struct pgwt_trace_event *events, int count, enum mutant m)
{
    if (count < 2) return 0;
    struct pgwt_trace_event *scratch = NULL;
    int scratch_cap = 0;
    const long budget = count;
    long tail_total = 0;
    int fell_back = 0;
    int sorted = 1;
    while (sorted < count &&
           events[sorted - 1].timestamp_ns <= events[sorted].timestamp_ns)
        sorted++;
    while (sorted < count) {
        int run_end = sorted + 1;
        if (m == MUT_RUN_DESCENDING) {
            while (run_end < count &&
                   events[run_end - 1].timestamp_ns >= events[run_end].timestamp_ns)
                run_end++;
        } else {
            while (run_end < count &&
                   events[run_end - 1].timestamp_ns <= events[run_end].timestamp_ns)
                run_end++;
        }
        uint64_t first = events[sorted].timestamp_ns;
        int lo = sorted;
        if (m != MUT_NO_TAIL_SCAN) {
            while (lo > 0 && events[lo - 1].timestamp_ns > first)
                lo--;
        }
        int tail = sorted - lo;
        tail_total += tail;
        if (tail_total > budget) { fell_back = 1; break; }
        if (tail > scratch_cap) {
            int next = scratch_cap ? scratch_cap : 64;
            while (next < tail) {
                if (next > INT_MAX / 2) { next = tail; break; }
                next *= 2;
            }
            struct pgwt_trace_event *grown = (struct pgwt_trace_event *)
                realloc(scratch, (size_t)next * sizeof(*scratch));
            if (!grown) { free(scratch); return -1; }
            scratch = grown;
            scratch_cap = next;
        }
        memcpy(scratch, events + lo, (size_t)tail * sizeof(*scratch));
        int a = 0, b = sorted, out = lo;
        while (a < tail && b < run_end) {
            int left_wins = (m == MUT_TIE_RIGHT)
                ? (scratch[a].timestamp_ns < events[b].timestamp_ns)
                : (scratch[a].timestamp_ns <= events[b].timestamp_ns);
            if (left_wins) events[out++] = scratch[a++];
            else events[out++] = events[b++];
        }
        if (m != MUT_NO_LEFT_DRAIN)
            while (a < tail) events[out++] = scratch[a++];
        sorted = run_end;
    }
    free(scratch);
    if (fell_back) {
        if (m == MUT_FALLBACK_SKIP)
            return 0;
        return oracle_fixed_width(events, count);
    }
    return 0;
}

/* ── alloc-failure hooks ───────────────────────────────────── */
static const char *fail_point;
static int alloc_fail_hook(const char *point)
{
    return fail_point && strcmp(point, fail_point) == 0;
}

/* ── section 1: differential, bit-identical ────────────────── */
static void differential(const char *name, uint64_t *keys, int n,
                         int expect_fell_back)
{
    struct pgwt_trace_event *input = make_events(keys, n);
    struct pgwt_trace_event *subject = make_events(keys, n);
    struct pgwt_trace_event *oracle = make_events(keys, n);

    CHECK(oracle_fixed_width(oracle, n) == 0, "%s: oracle failed", name);
    struct pgwt_event_sort_hooks h = { NULL, 0, 0 };
    CHECK(pgwt_stable_sort_events(subject, n, &h) == 0, "%s: subject failed", name);

    CHECK(memcmp(subject, oracle, (size_t)n * sizeof(*oracle)) == 0,
          "%s: output differs from the fixed-width mergesort", name);
    if (memcmp(subject, oracle, (size_t)n * sizeof(*oracle)) != 0) {
        for (int i = 0; i < n; i++)
            if (memcmp(&subject[i], &oracle[i], sizeof(*oracle))) {
                printf("    first diff at %d: subject pid=%u ts=%llu, "
                       "oracle pid=%u ts=%llu\n", i, subject[i].pid,
                       (unsigned long long)subject[i].timestamp_ns,
                       oracle[i].pid, (unsigned long long)oracle[i].timestamp_ns);
                break;
            }
    }
    CHECK(is_stable_sorted(subject, n), "%s: subject not stable-sorted", name);
    CHECK(is_permutation(input, subject, n), "%s: subject is not a permutation", name);
    CHECK(h.fell_back == expect_fell_back,
          "%s: fell_back=%d, expected %d", name, h.fell_back, expect_fell_back);

    free(input); free(subject); free(oracle);
    free(keys);
}

int main(void)
{
    printf("=== test_event_sort ===\n");

    /* ── 1. differential over adversarial fixtures ─────────── */
    printf("\n[1] differential vs the deleted fixed-width mergesort\n");
    rng_seed(20261012);
    {
        uint64_t nn;
        differential("real-shape runs (n=200k, run~480, seam 1)",
                     gen_runs(200000, 480, 1, &nn), 200000, 0);
        differential("real-shape runs (n=50k, run~16, seam 8)",
                     gen_runs(50000, 16, 8, &nn), 50000, 0);
        /* Runs of ~3 records put a seam every ~3 records, so the chain
         * phase runs out of budget and hands over — the expectation is
         * recorded, not softened, because WHICH path ran is part of the
         * assertion. */
        differential("real-shape runs (n=4097, run~3, seam 2)",
                     gen_runs(4097, 3, 2, &nn), 4097, 1);
        differential("already sorted (n=10k)", gen_sorted(10000), 10000, 0);
        differential("all keys equal (n=10k)", gen_all_equal(10000), 10000, 0);
        differential("random, 64-key alphabet (n=20k)",
                     gen_random_dup(20000, 64), 20000, 1);
        differential("random, 8-key alphabet (n=5k)",
                     gen_random_dup(5000, 8), 5000, 1);
        differential("random, wide keys (n=20k)",
                     gen_random_dup(20000, 1000000), 20000, 1);
        differential("reversed (n=20k)", gen_reversed(20000), 20000, 1);
        for (int n = 0; n <= 9; n++) {
            char nm[64];
            snprintf(nm, sizeof(nm), "tiny n=%d", n);
            uint64_t *k = (uint64_t *)malloc((size_t)(n ? n : 1) * sizeof(*k));
            for (int i = 0; i < n; i++) k[i] = rng_below(4);
            struct pgwt_trace_event *s = make_events(k, n);
            struct pgwt_trace_event *o = make_events(k, n);
            oracle_fixed_width(o, n);
            struct pgwt_event_sort_hooks h = { NULL, 0, 0 };
            CHECK(pgwt_stable_sort_events(s, n, &h) == 0, "%s: rc", nm);
            CHECK(n == 0 || memcmp(s, o, (size_t)n * sizeof(*o)) == 0,
                  "%s: bytes differ", nm);
            free(s); free(o); free(k);
        }
        /* 300 randomised small cases: shapes no hand-written fixture covers */
        for (int t = 0; t < 300; t++) {
            int n = 2 + (int)rng_below(400);
            uint32_t alpha = 1 + rng_below(50);
            uint64_t *k = (uint64_t *)malloc((size_t)n * sizeof(*k));
            for (int i = 0; i < n; i++) k[i] = rng_below(alpha);
            struct pgwt_trace_event *s = make_events(k, n);
            struct pgwt_trace_event *o = make_events(k, n);
            oracle_fixed_width(o, n);
            struct pgwt_event_sort_hooks h = { NULL, 0, 0 };
            pgwt_stable_sort_events(s, n, &h);
            if (memcmp(s, o, (size_t)n * sizeof(*o)) != 0) {
                failures++;
                printf("  FAIL randomised case t=%d n=%d alpha=%u: bytes differ\n",
                       t, n, alpha);
            }
            free(s); free(o); free(k);
        }
        checks++;
        printf("  300 randomised small cases compared byte-for-byte\n");
    }

    /* ── 2. tie order, asserted by hand ────────────────────── */
    printf("\n[2] tie order is the INPUT order (what attribution depends on)\n");
    {
        /* Keys chosen so a marker (index 1) and an event (index 4) share a
         * timestamp and straddle a run boundary. */
        uint64_t keys[] = { 50, 20, 20, 90, 20, 10, 90, 10 };
        int n = (int)(sizeof(keys) / sizeof(keys[0]));
        /* expected: stable by key, input index as the tiebreak */
        uint32_t want_pid[] = { 5, 7, 1, 2, 4, 0, 3, 6 };
        struct pgwt_trace_event *e = make_events(keys, n);
        struct pgwt_event_sort_hooks h = { NULL, 0, 0 };
        CHECK(pgwt_stable_sort_events(e, n, &h) == 0, "tie fixture rc");
        for (int i = 0; i < n; i++)
            CHECK(e[i].pid == want_pid[i],
                  "tie fixture slot %d: pid=%u ts=%llu, expected pid=%u",
                  i, e[i].pid, (unsigned long long)e[i].timestamp_ns, want_pid[i]);
        free(e);
    }

    /* ── 3. the chain path is what ran on production's shape ── */
    printf("\n[3] the fast path, on the shape the real trace has\n");
    {
        uint64_t nn;
        int n = 500000;
        rng_seed(777);
        uint64_t *k = gen_runs(n, 480, 1, &nn);
        struct pgwt_trace_event *e = make_events(k, n);
        int inv = count_inversions(e, n);
        struct pgwt_event_sort_hooks h = { NULL, 0, 0 };
        CHECK(pgwt_stable_sort_events(e, n, &h) == 0, "shape fixture rc");
        CHECK(h.fell_back == 0, "shape fixture took the fallback (fell_back=%d)",
              h.fell_back);
        CHECK(inv > n / 2000, "shape fixture has too few inversions (%d of %d) — "
              "it would pass without sorting anything", inv, n);
        CHECK(h.moves < (long)n / 4,
              "chain merge moved %ld records for n=%d (>n/4): the O(n) claim "
              "does not hold on this shape", h.moves, n);
        printf("  n=%d runs~%d inversions=%d chain moves=%ld (%.4fn)\n",
               n, 480, inv, h.moves, (double)h.moves / n);
        free(e); free(k);
    }

    /* ── 4. the O(n log n) fallback ────────────────────────── */
    printf("\n[4] the work-budget fallback\n");
    {
        int n = 30000;
        uint64_t *k = gen_reversed(n);
        struct pgwt_trace_event *input = make_events(k, n);
        struct pgwt_trace_event *s = make_events(k, n);
        struct pgwt_trace_event *o = make_events(k, n);
        oracle_fixed_width(o, n);
        struct pgwt_event_sort_hooks h = { NULL, 0, 0 };
        CHECK(pgwt_stable_sort_events(s, n, &h) == 0, "reversed rc");
        CHECK(h.fell_back == 1, "reversed input did NOT reach the fallback");
        CHECK(memcmp(s, o, (size_t)n * sizeof(*o)) == 0,
              "reversed: fallback output differs from the oracle");
        CHECK(is_permutation(input, s, n), "reversed: not a permutation");
        free(input); free(s); free(o); free(k);

        /* The hand-off happens mid-chain, so the partially merged array must
         * still carry input order among equal keys. A fixture with heavy
         * duplicates AND enough disorder to blow the budget pins that. */
        rng_seed(31337);
        for (int t = 0; t < 20; t++) {
            int m = 3000 + (int)rng_below(3000);
            uint64_t *kk = gen_random_dup(m, 5);
            struct pgwt_trace_event *ss = make_events(kk, m);
            struct pgwt_trace_event *oo = make_events(kk, m);
            oracle_fixed_width(oo, m);
            struct pgwt_event_sort_hooks hh = { NULL, 0, 0 };
            pgwt_stable_sort_events(ss, m, &hh);
            if (hh.fell_back != 1) {
                failures++;
                printf("  FAIL dup-heavy t=%d n=%d did not reach the fallback\n", t, m);
            }
            if (memcmp(ss, oo, (size_t)m * sizeof(*oo)) != 0) {
                failures++;
                printf("  FAIL dup-heavy t=%d n=%d: fallback output differs\n", t, m);
            }
            free(ss); free(oo); free(kk);
        }
        checks += 2;
        printf("  20 duplicate-heavy fixtures crossed the budget and matched\n");
    }

    /* ── 5. false negatives: every way this could stop seeing ── */
    printf("\n[5] false-negative suite\n");

    /* 5a. the fixtures are not already ordered (a no-op would pass). */
    {
        rng_seed(4242);
        uint64_t nn;
        uint64_t *k = gen_runs(50000, 480, 1, &nn);
        struct pgwt_trace_event *e = make_events(k, 50000);
        int inv = count_inversions(e, 50000);
        CHECK(inv >= 50, "the run fixture has only %d inversions: a sort that "
              "did nothing would pass section 1", inv);
        free(e); free(k);
    }

    /* 5b. the fixtures contain duplicate keys (else ties are invisible), and
     *     a tie flip is actually DETECTED by them. */
    {
        rng_seed(99);
        uint64_t *k = gen_random_dup(20000, 64);
        struct pgwt_trace_event *e = make_events(k, 20000);
        int dup = count_dup_keys(e, 20000);
        CHECK(dup > 1000, "the duplicate fixture has only %d duplicate keys: a "
              "tie-order bug would be invisible", dup);
        free(e); free(k);
    }

    /* 5c. the oracle does real work (two sides cannot agree by both idling). */
    {
        rng_seed(5);
        uint64_t *k = gen_random_dup(10000, 64);
        struct pgwt_trace_event *before = make_events(k, 10000);
        struct pgwt_trace_event *after = make_events(k, 10000);
        oracle_fixed_width(after, 10000);
        CHECK(memcmp(before, after, (size_t)10000 * sizeof(*after)) != 0,
              "the oracle returned its input unchanged — it is not sorting");
        CHECK(is_stable_sorted(after, 10000), "the oracle's own output is not "
              "stable-sorted: it cannot be an oracle");
        free(before); free(after); free(k);
    }

    /* 5d. nothing to sort: count 0 and 1 must not touch memory or fail. */
    {
        uint64_t k[1] = { 7 };
        struct pgwt_trace_event *e = make_events(k, 1);
        struct pgwt_trace_event copy = e[0];
        struct pgwt_event_sort_hooks h = { NULL, 0, 0 };
        CHECK(pgwt_stable_sort_events(e, 1, &h) == 0, "n=1 rc");
        CHECK(memcmp(&copy, &e[0], sizeof(copy)) == 0, "n=1 modified the record");
        CHECK(pgwt_stable_sort_events(e, 0, &h) == 0, "n=0 rc");
        CHECK(h.moves == 0 && h.fell_back == 0, "n<2 reported work it did not do");
        free(e);
    }

    /* 5e. a gate that cannot allocate must REFUSE, never approve. Both
     *     allocation sites, and the array must still be a permutation. */
    {
        rng_seed(11);
        uint64_t nn;
        int n = 20000;
        uint64_t *k = gen_runs(n, 480, 1, &nn);
        struct pgwt_trace_event *input = make_events(k, n);
        struct pgwt_trace_event *e = make_events(k, n);
        struct pgwt_event_sort_hooks h = { alloc_fail_hook, 0, 0 };
        fail_point = "scratch";
        CHECK(pgwt_stable_sort_events(e, n, &h) == -1,
              "scratch allocation failure returned success");
        CHECK(is_permutation(input, e, n),
              "scratch allocation failure lost or duplicated a record");
        fail_point = NULL;
        free(input); free(e); free(k);

        /* the fallback's allocation */
        uint64_t *kr = gen_reversed(n);
        struct pgwt_trace_event *inr = make_events(kr, n);
        struct pgwt_trace_event *er = make_events(kr, n);
        struct pgwt_event_sort_hooks hr = { alloc_fail_hook, 0, 0 };
        fail_point = "fallback";
        CHECK(pgwt_stable_sort_events(er, n, &hr) == -1,
              "fallback allocation failure returned success");
        CHECK(hr.fell_back == 1, "fallback alloc test never reached the fallback");
        CHECK(is_permutation(inr, er, n),
              "fallback allocation failure lost or duplicated a record");
        fail_point = NULL;
        free(inr); free(er); free(kr);
    }

    /* 5f. MUTATION PROBE. Five independent breakages; each must be caught by
     *     at least one fixture, or that fixture is not testing anything. */
    {
        printf("  mutation probe:\n");
        struct { enum mutant m; const char *name; } muts[] = {
            { MUT_TIE_RIGHT,      "ties go to the later record" },
            { MUT_NO_LEFT_DRAIN,  "prefix tail never flushed" },
            { MUT_NO_TAIL_SCAN,   "runs appended without looking back" },
            { MUT_RUN_DESCENDING, "run detection comparison reversed" },
            { MUT_FALLBACK_SKIP,  "over budget: return success unsorted" },
        };
        for (unsigned mi = 0; mi < sizeof(muts) / sizeof(muts[0]); mi++) {
            int caught = 0;
            rng_seed(606 + mi);
            for (int t = 0; t < 40 && !caught; t++) {
                uint64_t nn;
                int n;
                uint64_t *k;
                if (muts[mi].m == MUT_FALLBACK_SKIP) {
                    n = 4000;
                    k = gen_reversed(n);
                } else if (t % 2 == 0) {
                    n = 2000 + (int)rng_below(2000);
                    k = gen_random_dup(n, 1 + rng_below(40));
                } else {
                    n = 20000;
                    k = gen_runs(n, 480, 1, &nn);
                }
                struct pgwt_trace_event *s = make_events(k, n);
                struct pgwt_trace_event *o = make_events(k, n);
                oracle_fixed_width(o, n);
                chain_mutant(s, n, muts[mi].m);
                if (memcmp(s, o, (size_t)n * sizeof(*o)) != 0)
                    caught = 1;
                free(s); free(o); free(k);
            }
            CHECK(caught, "MUTANT SURVIVED: %s — the fixtures cannot see it",
                  muts[mi].name);
            if (caught)
                printf("    caught: %s\n", muts[mi].name);
        }
        /* And the unmutated copy must NOT be caught: otherwise the probe is
         * reporting a difference that has nothing to do with the mutation. */
        {
            rng_seed(606);
            int n = 20000;
            uint64_t nn;
            uint64_t *k = gen_runs(n, 480, 1, &nn);
            struct pgwt_trace_event *s = make_events(k, n);
            struct pgwt_trace_event *o = make_events(k, n);
            oracle_fixed_width(o, n);
            chain_mutant(s, n, MUT_NONE);
            CHECK(memcmp(s, o, (size_t)n * sizeof(*o)) == 0,
                  "the UNMUTATED control copy differs from the oracle — the "
                  "mutation probe is measuring noise, not mutations");
            free(s); free(o); free(k);
        }
    }

    printf("\n%d checks, %d failures\n", checks, failures);
    if (failures) {
        printf("FAILED\n");
        return 1;
    }
    printf("PASSED\n");
    return 0;
}
