/* test_window_clip.c — windowed aggregates must count only in-window time.
 *
 * THE DEFECT (demo blocker, found by the bucket-weighted AAS cross-tab check
 * on a 35-minute rehearsal): server_load_events_fi selects events by their
 * END timestamp, so a wait already running when the window opened arrives
 * whole. pgwt_compute_aas clipped such a straddler to its in-window overlap;
 * pgwt_compute_time_model / _top_events / _top_sessions / _top_queries summed
 * ev->duration_ns in full. On the rehearsal's 60 s window that was
 *
 *     AAS buckets  140,234.088188 ms
 *     Overview     142,059.216362 ms      (+1.2848%, tolerance 1.0%)
 *
 * and the whole 1,825.128174 ms difference was Timeout (931.110320 ms) plus
 * Lock (894.017854 ms) — the only classes with waits long enough to cross the
 * window's opening edge. The bias is unbounded: one 10 s Lock:relation ending
 * 1 s into the window adds 9 s of time that did not happen in the window.
 *
 * WHAT THIS PINS
 *   1. the straddler itself (section 1) — this is what fails on master;
 *   2. the cross-tab identity Σ(AAS buckets)·bucket_ns == time_model DB ns,
 *      computed by two independent code paths (section 2);
 *   3. that the fix does NOT over-clip: no-window requests, interior events
 *      and latency columns are untouched (sections 3 and 5).
 *
 * WHY SECTION 4 EXISTS (false negatives, not true positives). A gate that
 * cannot see must refuse, never approve. Section 4 walks every way this check
 * could be satisfied without checking anything: an empty event array, a
 * fixture that does not actually straddle, an unbounded window, a degenerate
 * zero-width window, a zero-duration record, an event wholly outside the
 * window, and a corrupt record whose duration exceeds its timestamp. Each
 * asserts the specific value that distinguishes "clipped correctly" from
 * "returned zero because nothing was looked at".
 *
 * Runs anywhere: pure in-memory arrays, no clock, no I/O, no ordering
 * assumptions — nothing here can be flaky.
 */
#include "compute.h"
#include "wait_event.h"
#include "pg_wait_tracer.h"
#include "summary_reader.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <math.h>

static int failures = 0;
static int checks   = 0;

#define CHECK(cond, msg) do {                                        \
    checks++;                                                        \
    if (!(cond)) { failures++;                                       \
        printf("  FAIL: %s (%s:%d)\n", msg, __FILE__, __LINE__); }   \
    else printf("  ok: %s\n", msg);                                  \
} while (0)

/* Dimensionless quantities (AAS) — same tolerance, no unit in the output. */
#define CHECK_NUM(got, want, msg) do {                               \
    checks++;                                                        \
    double _g = (got), _w = (want);                                  \
    if (fabs(_g - _w) > 1e-6) { failures++;                          \
        printf("  FAIL: %s — got %.6f, want %.6f (%s:%d)\n",          \
               msg, _g, _w, __FILE__, __LINE__); }                   \
    else printf("  ok: %s (%.6f)\n", msg, _g);                       \
} while (0)

#define CHECK_MS(got, want, msg) do {                                \
    checks++;                                                        \
    double _g = (got), _w = (want);                                  \
    if (fabs(_g - _w) > 1e-6) { failures++;                          \
        printf("  FAIL: %s — got %.6f ms, want %.6f ms (%s:%d)\n",    \
               msg, _g, _w, __FILE__, __LINE__); }                   \
    else printf("  ok: %s (%.6f ms)\n", msg, _g);                    \
} while (0)

/* Occurrence counts — printed with got/want so a failure names the inflation
 * instead of just saying "false". */
#define CHECK_U64(got, want, msg) do {                               \
    checks++;                                                        \
    unsigned long long _g = (unsigned long long)(got);               \
    unsigned long long _w = (unsigned long long)(want);              \
    if (_g != _w) { failures++;                                      \
        printf("  FAIL: %s — got %llu, want %llu (%s:%d)\n",          \
               msg, _g, _w, __FILE__, __LINE__); }                   \
    else printf("  ok: %s (%llu)\n", msg, _g);                       \
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

/* ── Fixture vocabulary ───────────────────────────────────────────────── */

#define CLASS_LOCK      0x03u
#define CLASS_CLIENT    0x06u
#define CLASS_TIMEOUT   0x09u
#define CLASS_IO        0x0Au
#define EV(cls, n)      (((cls) << 24) | (n))

#define CPU_GAP         0u
#define LOCK_RELATION   EV(CLASS_LOCK, 0)
#define TIMEOUT_PGSLEEP EV(CLASS_TIMEOUT, 2)
#define IO_READ         EV(CLASS_IO, 21)
#define CLIENT_READ     EV(CLASS_CLIENT, 0)

#define S(x)  ((uint64_t)(x) * 1000000000ULL)   /* seconds → ns */

/* A plausible absolute wall clock, like a real trace carries. */
#define T0   1790778045790378496ULL
#define WFROM  T0
#define WTO    (T0 + S(10))

static struct pgwt_trace_event mk(uint32_t pid, uint32_t event,
                                  uint64_t end_ns, uint64_t dur_ns,
                                  uint64_t query_id, uint64_t cpu_ns)
{
    struct pgwt_trace_event ev;
    memset(&ev, 0, sizeof(ev));
    ev.pid          = pid;
    ev.old_event    = event;
    ev.new_event    = CPU_GAP;
    ev.timestamp_ns = end_ns;
    ev.duration_ns  = dur_ns;
    ev.query_id     = query_id;
    ev.cpu_ns       = cpu_ns;
    return ev;
}

/* Σ over every AAS bucket of (Σ class_aas + offcpu_aas) · bucket_ns — the
 * bucket-weighted DB nanoseconds the AAS chart draws. Deliberately NOT built
 * from the same accumulator time_model uses: pgwt_compute_aas walks per-bucket
 * overlaps and never calls the clipping helper, so an agreement between the
 * two is evidence, not an identity. */
static double aas_db_ns(const struct pgwt_aas_result *a)
{
    double total = 0.0;
    for (int i = 0; i < a->num_buckets; i++) {
        double b = a->buckets[i].offcpu_aas;
        for (int c = 0; c < PGWT_NUM_CLASSES; c++)
            b += a->buckets[i].class_aas[c];
        total += b * (double)a->bucket_ns;
    }
    return total;
}

static double tm_class_ms(const struct pgwt_tm_result *tm, const char *name)
{
    for (int i = 0; i < tm->num_rows; i++)
        if (tm->rows[i].indent == 1 && strcmp(tm->rows[i].name, name) == 0)
            return tm->rows[i].time_ms;
    return -1.0;
}

static double ev_total_ms(const struct pgwt_events_result *r, uint32_t id)
{
    for (int i = 0; i < r->num_rows; i++)
        if (r->rows[i].event_id == id) return r->rows[i].total_ms;
    return -1.0;
}

static const struct pgwt_event_row *ev_row(const struct pgwt_events_result *r,
                                           uint32_t id)
{
    for (int i = 0; i < r->num_rows; i++)
        if (r->rows[i].event_id == id) return &r->rows[i];
    return NULL;
}

/* Occurrences of one event id. An ABSENT row reads 0 — which is the honest
 * answer for "how many times did this wait close inside the window", and is
 * what section 6 asserts. The helper is kept from being vacuous by
 * test_right_edge_counted_window(), which requires the SAME helper to return
 * a non-zero count on a fixture that does close in-window. */
static uint64_t ev_count(const struct pgwt_events_result *r, uint32_t id)
{
    for (int i = 0; i < r->num_rows; i++)
        if (r->rows[i].event_id == id) return r->rows[i].count;
    return 0;
}

static uint64_t ev_exact_count(const struct pgwt_events_result *r, uint32_t id)
{
    for (int i = 0; i < r->num_rows; i++)
        if (r->rows[i].event_id == id) return r->rows[i].exact_count;
    return 0;
}

static double sess_ms(const struct pgwt_sessions_result *r, uint32_t pid)
{
    for (int i = 0; i < r->num_rows; i++)
        if (r->rows[i].pid == pid) return r->rows[i].db_time_ms;
    return -1.0;
}

static const struct pgwt_query_row *qrow(const struct pgwt_queries_result *r,
                                         uint64_t qid)
{
    for (int i = 0; i < r->num_rows; i++)
        if (r->rows[i].query_id == qid) return &r->rows[i];
    return NULL;
}

/* ── 1. The straddler: only the in-window part counts ─────────────────── */

/* One Lock:relation wait: starts 3 s BEFORE the window opens, ends 1 s after
 * it opens. Duration 4 s, in-window 1 s. Every DB-time column must read
 * 1000 ms. On master every one of them reads 4000 ms. */
static void test_straddler_leading_edge(void)
{
    printf("--- 1. leading-edge straddler contributes only its overlap ---\n");

    struct pgwt_trace_event evs[] = {
        mk(4242, LOCK_RELATION, WFROM + S(1), S(4), 77, PGWT_CPU_NS_UNKNOWN),
    };
    struct pgwt_filter f;
    memset(&f, 0, sizeof(f));

    /* Self-test of the fixture: if these were equal the whole section could
     * pass without any clipping happening at all. */
    CHECK(evs[0].duration_ns == S(4) && evs[0].timestamp_ns - S(4) < WFROM,
          "fixture really straddles `from` (4 s wait, 1 s inside)");

    struct pgwt_tm_result tm;
    pgwt_compute_time_model(evs, 1, &f, WFROM, WTO, 10000.0, &tm);
    CHECK_MS(tm.db_time_ms, 1000.0, "time_model DB Time = in-window 1 s");
    CHECK_MS(tm_class_ms(&tm, "Lock"), 1000.0, "time_model Lock row = 1 s");
    CHECK_NUM(tm.aas, 0.1, "time_model AAS = 1 s / 10 s window (dimensionless)");
    free(tm.rows);

    struct pgwt_events_result te;
    pgwt_compute_top_events(evs, 1, &f, WFROM, WTO, 10000.0, &te);
    CHECK_MS(te.db_time_ms, 1000.0, "top_events DB Time = 1 s");
    CHECK_MS(ev_total_ms(&te, LOCK_RELATION), 1000.0,
             "top_events Lock:relation total = 1 s");
    free(te.rows);

    struct pgwt_sessions_result ts;
    pgwt_compute_top_sessions(evs, 1, &f, WFROM, WTO, 10000.0, &ts);
    CHECK_MS(sess_ms(&ts, 4242), 1000.0, "top_sessions pid 4242 = 1 s");
    free(ts.rows);

    struct pgwt_queries_result tq;
    pgwt_compute_top_queries(evs, 1, &f, WFROM, WTO, 10000.0, &tq);
    const struct pgwt_query_row *q = qrow(&tq, 77);
    CHECK(q != NULL, "top_queries has the query row");
    if (q) CHECK_MS(q->total_ms, 1000.0, "top_queries query 77 total = 1 s");
    free(tq.rows);
}

/* Trailing edge: a wait that starts inside the window and runs past `to`.
 * The raw loader cannot deliver one (it selects on END), but the compute
 * layer is also called from replay paths, so clip both sides. */
static void test_straddler_trailing_edge(void)
{
    printf("--- 1b. trailing-edge straddler is clipped at `to` ---\n");

    struct pgwt_trace_event evs[] = {
        mk(7, TIMEOUT_PGSLEEP, WTO + S(6), S(8), 0, PGWT_CPU_NS_UNKNOWN),
    };
    struct pgwt_filter f;
    memset(&f, 0, sizeof(f));

    struct pgwt_tm_result tm;
    pgwt_compute_time_model(evs, 1, &f, WFROM, WTO, 10000.0, &tm);
    /* starts at WTO-2s, window ends at WTO → 2 s inside */
    CHECK_MS(tm.db_time_ms, 2000.0, "time_model counts 2 s of an 8 s wait");
    free(tm.rows);
}

/* ── 2. Cross-tab: AAS chart and Overview must agree ──────────────────── */

/* The invariant the demo shows on one screen. Built from a mixed workload
 * with straddlers on both edges, interior waits, an idle record, a CPU gap
 * with measured cpu_ns and a CPU gap that straddles — so the CPU* split and
 * the Off-CPU* residual are exercised too, not just wall time. */
static void test_cross_tab_agreement(void)
{
    printf("--- 2. Σ(AAS buckets)·bucket_ns == time_model DB ns ---\n");

    struct pgwt_trace_event evs[] = {
        /* straddles `from`: 4 s wait, 1 s inside */
        mk(1, LOCK_RELATION,   WFROM + S(1), S(4), 11, PGWT_CPU_NS_UNKNOWN),
        /* straddles `from`: 6 s wait, 2 s inside */
        mk(2, TIMEOUT_PGSLEEP, WFROM + S(2), S(6), 12, PGWT_CPU_NS_UNKNOWN),
        /* wholly interior */
        mk(3, IO_READ,         WFROM + S(5), S(2), 13, PGWT_CPU_NS_UNKNOWN),
        /* interior CPU gap with measured CPU: 1 s wall, 400 ms on-CPU */
        mk(4, CPU_GAP,         WFROM + S(6), S(1), 14, 400000000ULL),
        /* CPU gap straddling `from`: 2 s wall (1 s inside), 1 s on-CPU */
        mk(5, CPU_GAP,         WFROM + S(1), S(2), 15, S(1)),
        /* idle, also straddling `from` (3 s, 2 s inside) — excluded from DB
         * Time by both paths, but its own idle_time_ms must clip as well */
        mk(6, CLIENT_READ,     WFROM + S(2), S(3), 0,  PGWT_CPU_NS_UNKNOWN),
        /* straddles `to` */
        mk(7, LOCK_RELATION,   WTO   + S(3), S(5), 16, PGWT_CPU_NS_UNKNOWN),
    };
    int n = (int)(sizeof(evs) / sizeof(evs[0]));
    struct pgwt_filter f;
    memset(&f, 0, sizeof(f));

    /* Hand-computed in-window DB time (idle excluded):
     *   Lock  1 s + Timeout 2 s + IO 2 s + CPU gap 1 s + CPU gap 1 s
     * + Lock  2 s (WTO+3s−5s = WTO−2s .. WTO)                = 9 s      */
    const double expect_db_ms = 9000.0;

    struct pgwt_tm_result tm;
    pgwt_compute_time_model(evs, n, &f, WFROM, WTO, 10000.0, &tm);
    CHECK_MS(tm.db_time_ms, expect_db_ms, "time_model DB Time (hand-computed)");
    CHECK_MS(tm.idle_time_ms, 2000.0,
             "idle time is clipped too (3 s Client:ClientRead, 2 s inside)");
    /* CPU*: 400 ms measured interior + half of the straddling gap's 1 s. */
    CHECK_MS(tm.cpu_ms, 900.0, "CPU* splits the straddling gap by overlap");

    /* The AAS chart, at two different bucket counts. Bucket width must not
     * matter — the rehearsal's 60-bucket chart and a 1-bucket query have to
     * report the same total. */
    int bucket_counts[] = {1, 10, 60, 120};
    for (int k = 0; k < 4; k++) {
        struct pgwt_aas_result aas;
        pgwt_compute_aas(evs, n, &f, WFROM, WTO, bucket_counts[k], 0, 0, &aas);
        char msg[160];
        /* pgwt_compute_aas rounds bucket_ns UP, so its last bucket can end
         * past `to` by (num_buckets · bucket_ns − range) ns and sweep up that
         * much extra from an event straddling `to`. That overhang is derived
         * here, not a tolerance: it is 0 whenever the bucket count divides the
         * range (the rehearsal's 60 s / 60 buckets), 20 ns at 60 buckets over
         * 10 s and 80 ns at 120 — i.e. ≤ 1e-4 ms, six orders of magnitude
         * below the 1.28% defect this file exists for. Asserting a fixed slop
         * instead would let a real regression hide inside it. */
        double overhang_ns =
            (double)((uint64_t)aas.num_buckets * aas.bucket_ns - (WTO - WFROM));
        double diff_ns = aas_db_ns(&aas) - tm.db_time_ms * 1e6;
        snprintf(msg, sizeof(msg),
                 "AAS(%d buckets) == time_model DB Time (diff %.0f ns, "
                 "bucket overhang %.0f ns)",
                 bucket_counts[k], diff_ns, overhang_ns);
        CHECK(diff_ns >= -1e-6 && diff_ns <= overhang_ns + 1e-6, msg);
        if (overhang_ns == 0.0) {
            snprintf(msg, sizeof(msg),
                     "AAS(%d buckets) agrees EXACTLY (bucket width divides "
                     "the range)", bucket_counts[k]);
            CHECK_MS(aas_db_ns(&aas) / 1e6, tm.db_time_ms, msg);
        }
        free(aas.buckets);
        free(aas.event_aas);
    }
    free(tm.rows);
}

/* ── 3. No window given: whole-capture numbers must not move ──────────── */

static void test_unbounded_window_unchanged(void)
{
    printf("--- 3. from=0/to=0 (whole capture) keeps full durations ---\n");

    struct pgwt_trace_event evs[] = {
        mk(1, LOCK_RELATION, WFROM + S(1), S(4), 11, PGWT_CPU_NS_UNKNOWN),
        mk(2, IO_READ,       WFROM + S(5), S(2), 12, PGWT_CPU_NS_UNKNOWN),
        mk(3, CPU_GAP,       WFROM + S(6), S(1), 13, 400000000ULL),
    };
    int n = 3;
    struct pgwt_filter f;
    memset(&f, 0, sizeof(f));

    struct pgwt_tm_result tm;
    pgwt_compute_time_model(evs, n, &f, 0, 0, 10000.0, &tm);
    CHECK_MS(tm.db_time_ms, 7000.0, "unbounded: 4 s + 2 s + 1 s in full");
    CHECK_MS(tm.cpu_ms, 400.0, "unbounded: measured CPU is not scaled");
    free(tm.rows);

    struct pgwt_events_result te;
    pgwt_compute_top_events(evs, n, &f, 0, 0, 10000.0, &te);
    CHECK_MS(ev_total_ms(&te, LOCK_RELATION), 4000.0,
             "unbounded: top_events keeps the full 4 s");
    free(te.rows);

    struct pgwt_sessions_result ts;
    pgwt_compute_top_sessions(evs, n, &f, 0, 0, 10000.0, &ts);
    CHECK_MS(sess_ms(&ts, 1), 4000.0, "unbounded: top_sessions keeps 4 s");
    free(ts.rows);

    struct pgwt_queries_result tq;
    pgwt_compute_top_queries(evs, n, &f, 0, 0, 10000.0, &tq);
    const struct pgwt_query_row *q = qrow(&tq, 11);
    if (q) CHECK_MS(q->total_ms, 4000.0, "unbounded: top_queries keeps 4 s");
    else   CHECK(0, "unbounded: top_queries has the query row");
    free(tq.rows);

    /* Half-bounded windows: only the given side clips. */
    struct pgwt_tm_result lo, hi;
    pgwt_compute_time_model(evs, 1, &f, WFROM, 0, 10000.0, &lo);
    CHECK_MS(lo.db_time_ms, 1000.0, "from set / to unbounded clips the start");
    free(lo.rows);
    pgwt_compute_time_model(evs, 1, &f, 0, WTO, 10000.0, &hi);
    CHECK_MS(hi.db_time_ms, 4000.0, "to set / from unbounded leaves it whole");
    free(hi.rows);
}

/* ── 4. Bypass suite: every way this check could stop checking ────────── */

static void test_bypass_paths(void)
{
    printf("--- 4. false-negative suite: the gate must refuse, not approve ---\n");

    struct pgwt_filter f;
    memset(&f, 0, sizeof(f));

    /* 4a. Empty input. A "gap == 0" style check is trivially satisfied by an
     * empty event set; pin that the answer is a real zero and that the
     * straddler oracle above is NOT zero, so "everything is 0" cannot pass
     * section 1. */
    struct pgwt_tm_result tm0;
    pgwt_compute_time_model(NULL, 0, &f, WFROM, WTO, 10000.0, &tm0);
    CHECK_MS(tm0.db_time_ms, 0.0, "empty input yields 0 ms, not garbage");
    CHECK(tm0.num_rows >= 1, "empty input still emits the DB Time row");
    free(tm0.rows);

    struct pgwt_aas_result aas0;
    pgwt_compute_aas(NULL, 0, &f, WFROM, WTO, 10, 0, 0, &aas0);
    CHECK(aas0.num_buckets == 10, "empty input still lays out its buckets");
    CHECK_MS(aas_db_ns(&aas0) / 1e6, 0.0, "empty input: AAS total 0");
    free(aas0.buckets);
    free(aas0.event_aas);

    /* 4b. The straddler is ABSENT rather than wrong: an event wholly inside
     * the window must be untouched. If the fix clipped indiscriminately this
     * would go red — the "over-clip" false positive. */
    struct pgwt_trace_event interior =
        mk(1, LOCK_RELATION, WFROM + S(5), S(2), 11, PGWT_CPU_NS_UNKNOWN);
    struct pgwt_tm_result tmi;
    pgwt_compute_time_model(&interior, 1, &f, WFROM, WTO, 10000.0, &tmi);
    CHECK_MS(tmi.db_time_ms, 2000.0, "interior event is not clipped");
    free(tmi.rows);

    /* 4c. Event wholly BEFORE the window (end <= from). Contributes nothing —
     * and must not produce a wrapped uint64 difference. */
    struct pgwt_trace_event before =
        mk(1, LOCK_RELATION, WFROM - S(1), S(2), 11, PGWT_CPU_NS_UNKNOWN);
    struct pgwt_tm_result tmb;
    pgwt_compute_time_model(&before, 1, &f, WFROM, WTO, 10000.0, &tmb);
    CHECK_MS(tmb.db_time_ms, 0.0, "event ending before `from` contributes 0");
    free(tmb.rows);

    /* 4d. Event wholly AFTER the window (start >= to). */
    struct pgwt_trace_event after =
        mk(1, LOCK_RELATION, WTO + S(3), S(1), 11, PGWT_CPU_NS_UNKNOWN);
    struct pgwt_tm_result tma;
    pgwt_compute_time_model(&after, 1, &f, WFROM, WTO, 10000.0, &tma);
    CHECK_MS(tma.db_time_ms, 0.0, "event starting after `to` contributes 0");
    free(tma.rows);

    /* 4e. Zero-width window (from == to): nothing is inside it, and the
     * cpu_ns fraction must not divide by a zero duration. */
    struct pgwt_trace_event mixed[] = {
        mk(1, LOCK_RELATION, WFROM + S(1), S(4), 11, PGWT_CPU_NS_UNKNOWN),
        mk(2, CPU_GAP,       WFROM + S(1), S(2), 12, S(1)),
    };
    struct pgwt_tm_result tmz;
    pgwt_compute_time_model(mixed, 2, &f, WFROM, WFROM, 0.0, &tmz);
    CHECK_MS(tmz.db_time_ms, 0.0, "zero-width window yields 0 ms");
    CHECK_MS(tmz.cpu_ms, 0.0, "zero-width window yields 0 CPU*");
    free(tmz.rows);

    /* 4f. Zero-duration record (a SAMPLES point observation): no time, no
     * NaN, no divide-by-zero in the in-window fraction. */
    struct pgwt_trace_event zero =
        mk(1, CPU_GAP, WFROM + S(2), 0, 11, 0);
    struct pgwt_tm_result tmzd;
    pgwt_compute_time_model(&zero, 1, &f, WFROM, WTO, 10000.0, &tmzd);
    CHECK_MS(tmzd.db_time_ms, 0.0, "zero-duration record adds 0 ms");
    CHECK(!isnan(tmzd.cpu_ms) && tmzd.cpu_ms == 0.0,
          "zero-duration record leaves CPU* a real 0, not NaN");
    free(tmzd.rows);

    /* 4g. Impossible record: duration longer than its own absolute END
     * timestamp (needs a wait longer than the epoch). It is REFUSED, not
     * repaired — see pgwt_filter_matches. The two plausible repairs disagree
     * with each other, and that disagreement is the bug: clamping the start
     * to 0 makes the summers count ~58 years, while pgwt_compute_aas derives
     * ev_start by subtraction, wraps, and drops the record — so Overview and
     * the AAS chart would answer differently on the same bytes, which is the
     * exact cross-tab defect this file exists for.
     *
     * Pinned on all three axes: windowed, UNBOUNDED (a clamp-to-0 repair
     * returns timestamp_ns here instead of duration_ns, the one exception to
     * "whole-capture is bit-identical"), and the AAS-vs-Overview identity on
     * a mixed array, which is what proves the two paths agree on garbage. */
    struct pgwt_trace_event corrupt =
        mk(1, LOCK_RELATION, WFROM + S(1), WFROM + S(1) + S(5), 11,
           PGWT_CPU_NS_UNKNOWN);
    struct pgwt_tm_result tmc;
    pgwt_compute_time_model(&corrupt, 1, &f, WFROM, WTO, 10000.0, &tmc);
    CHECK_MS(tmc.db_time_ms, 0.0,
             "duration > timestamp is refused, not clamped to a 1 s window");
    free(tmc.rows);

    struct pgwt_tm_result tmcu;
    pgwt_compute_time_model(&corrupt, 1, &f, 0, 0, 10000.0, &tmcu);
    CHECK_MS(tmcu.db_time_ms, 0.0,
             "...refused on the UNBOUNDED path too, not counted as timestamp_ns");
    free(tmcu.rows);

    /* The identity, on an array holding one good event and one impossible
     * one: both paths must drop the same record and keep the same one. If
     * either path repaired the corrupt record instead, these two numbers
     * would differ — and a suite that only checked "corrupt == 0" in
     * isolation would never notice. */
    struct pgwt_trace_event mixed_corrupt[] = {
        mk(1, LOCK_RELATION, WFROM + S(5), S(2), 11, PGWT_CPU_NS_UNKNOWN),
        corrupt,
    };
    struct pgwt_tm_result tmmc;
    pgwt_compute_time_model(mixed_corrupt, 2, &f, WFROM, WTO, 10000.0, &tmmc);
    CHECK_MS(tmmc.db_time_ms, 2000.0,
             "an impossible record does not disturb the valid one beside it");
    struct pgwt_aas_result aasc;
    pgwt_compute_aas(mixed_corrupt, 2, &f, WFROM, WTO, 10, 0, 0, &aasc);
    CHECK_MS(aas_db_ns(&aasc) / 1e6, tmmc.db_time_ms,
             "AAS and time_model agree on an array containing garbage");
    free(aasc.buckets);
    free(aasc.event_aas);
    free(tmmc.rows);

    /* 4h. A filter that matches nothing must not be mistaken for agreement:
     * both sides go to zero, so section 2's identity would hold vacuously.
     * Pin that the filter really excluded everything. */
    struct pgwt_filter fp;
    memset(&fp, 0, sizeof(fp));
    fp.pid = 999999;
    struct pgwt_tm_result tmf;
    pgwt_compute_time_model(mixed, 2, &fp, WFROM, WTO, 10000.0, &tmf);
    CHECK_MS(tmf.db_time_ms, 0.0, "non-matching filter yields 0, vacuously");
    free(tmf.rows);
    struct pgwt_tm_result tmn;
    pgwt_compute_time_model(mixed, 2, &f, WFROM, WTO, 10000.0, &tmn);
    CHECK(tmn.db_time_ms > 0.0,
          "...and the SAME events without the filter are non-zero");
    free(tmn.rows);
}

/* ── 5. Latency columns are a property of the wait, not of the window ─── */

static void test_latency_columns_keep_full_duration(void)
{
    printf("--- 5. avg/max/percentiles keep the full duration ---\n");

    struct pgwt_trace_event evs[] = {
        mk(1, LOCK_RELATION, WFROM + S(1), S(4), 11, PGWT_CPU_NS_UNKNOWN),
    };
    struct pgwt_filter f;
    memset(&f, 0, sizeof(f));

    struct pgwt_events_result te;
    pgwt_compute_top_events(evs, 1, &f, WFROM, WTO, 10000.0, &te);
    const struct pgwt_event_row *r = ev_row(&te, LOCK_RELATION);
    CHECK(r != NULL, "top_events has the Lock:relation row");
    if (r) {
        CHECK_MS(r->total_ms, 1000.0, "…its DB-time column is clipped to 1 s");
        CHECK(r->count == 1, "…counted once");
        CHECK(fabs(r->avg_us - 4000000.0) < 1e-3,
              "…but avg latency is the real 4 s wait");
        CHECK(fabs(r->max_us - 4000000.0) < 1e-3,
              "…and so is max latency");
    }
    free(te.rows);

    struct pgwt_queries_result tq;
    pgwt_compute_top_queries(evs, 1, &f, WFROM, WTO, 10000.0, &tq);
    const struct pgwt_query_row *q = qrow(&tq, 11);
    CHECK(q != NULL, "top_queries has the query row");
    if (q) {
        CHECK_MS(q->total_ms, 1000.0, "…query DB-time column is clipped");
        CHECK(fabs(q->avg_us - 4000000.0) < 1e-3,
              "…query avg keeps the full 4 s");
    }
    free(tq.rows);
}

/* ── 6. The RIGHT edge: INSTANT vs INTERVAL (#424) ────────────────────
 *
 * #424 is the mirror of section 1, one layer down. The raw loader
 * (server_load_events_fi_mode / load_file_range_mono, src/server.c) admits a
 * non-sample record only when its CLOSE timestamp falls in [from, to]. The
 * correct admission rule is OVERLAP: close >= from AND start <= to. The right
 * half is implemented nowhere, so a wait still open at the window's right edge
 * contributes 0, and a wait spanning the whole window is dropped entirely.
 *
 * This section does NOT test the loader (it lives in server.c and needs a
 * trace dir — tests/test_window_additivity.py does that end to end). It pins
 * the semantics the loader fix has to deliver records INTO, and that is a
 * separate defect: compute.c clips TIME on both sides already
 * (event_window_ns), but every PER-OCCURRENCE quantity — count, exact_count,
 * max_ns, the latency histogram — is incremented for whatever record it is
 * handed, with no test on the record's own timestamp (src/compute.c:1587-1596,
 * :1883-1886). Today that is latent because admission never delivers a
 * right-open record. The moment admission is widened it stops being latent and
 * becomes inflation: counts, max latency and percentiles that include waits
 * which had not finished yet. A fix that corrects admission without this
 * distinction replaces an old wrong number with a new one.
 *
 * THE RULE, in one line:
 *   TIME        is selected by OVERLAP and CLIPPED to the window;
 *   OCCURRENCES (count, exact_count, max, histogram) are selected by the
 *               record's OWN close timestamp and must EXCLUDE right-open ones.
 *
 * Which of these are red on 304a772 is stated per assertion below: the time
 * legs are green here (they are red one layer down, through the loader) and
 * are the over-clip guard; the occurrence legs are red.
 */

#define RT        (T0 + S(100))          /* "T": the right-edge close time */
#define RA_FROM   (RT - S(20))
#define RA_TO     (RT - S(2))            /* 18 s window, closing BEFORE T */

/* 6a. A 10 s wait closing at T, with an interior 3 s wait of the same event
 * beside it so the row exists on both sides of the fix and max/count have a
 * baseline that is NOT the straddler's. */
static void test_right_edge_open_wait(void)
{
    printf("--- 6a. wait open at `to`: time clipped in, occurrence out ---\n");

    struct pgwt_trace_event evs[] = {
        /* right-open: [T-10, T]; overlap with [T-20, T-2] is 8 s */
        mk(5001, LOCK_RELATION, RT,        S(10), 88, PGWT_CPU_NS_UNKNOWN),
        /* interior:   [T-8, T-5] — wholly inside the window */
        mk(5001, LOCK_RELATION, RT - S(5), S(3),  88, PGWT_CPU_NS_UNKNOWN),
    };
    struct pgwt_filter f;
    memset(&f, 0, sizeof(f));

    /* Fixture self-test: without this, 6a could pass by looking at a record
     * that does not actually cross `to`. */
    CHECK(evs[0].timestamp_ns > RA_TO &&
          evs[0].timestamp_ns - evs[0].duration_ns < RA_TO,
          "fixture really crosses `to` (closes 2 s after the window ends)");

    struct pgwt_tm_result tm;
    pgwt_compute_time_model(evs, 2, &f, RA_FROM, RA_TO, 18000.0, &tm);
    /* TIME legs — green on 304a772 at this layer, red through the loader. */
    CHECK_MS(tm.db_time_ms, 11000.0,
             "time_model DB Time = 8 s clipped straddler + 3 s interior");
    CHECK_MS(tm_class_ms(&tm, "Lock"), 11000.0, "time_model Lock row = 11 s");
    free(tm.rows);

    struct pgwt_events_result te;
    pgwt_compute_top_events(evs, 2, &f, RA_FROM, RA_TO, 18000.0, &te);
    CHECK_MS(ev_total_ms(&te, LOCK_RELATION), 11000.0,
             "top_events Lock:relation time = 11 s");
    /* OCCURRENCE legs — RED on 304a772: count 2, exact_count 2, max 10 s. */
    CHECK_U64(ev_count(&te, LOCK_RELATION), 1,
              "top_events counts ONLY the wait that closed in-window");
    CHECK_U64(ev_exact_count(&te, LOCK_RELATION), 1,
              "…exact_count likewise excludes the right-open wait");
    const struct pgwt_event_row *r = ev_row(&te, LOCK_RELATION);
    CHECK(r != NULL, "top_events has the Lock:relation row");
    if (r) {
        CHECK_NUM(r->max_us, 3000000.0,
                  "…max latency (us) is the 3 s wait that finished, not the "
                  "open 10 s");
        /* The histogram is only observable through the latency columns the
         * server derives from it; a 10 s observation in it would move both
         * of the above. has_latency_dist must still be set — one real
         * observation remains. */
        CHECK(r->has_latency_dist == 1,
              "…the row still HAS a distribution (the 3 s observation)");
    }
    free(te.rows);

    struct pgwt_sessions_result ts;
    pgwt_compute_top_sessions(evs, 2, &f, RA_FROM, RA_TO, 18000.0, &ts);
    CHECK_MS(sess_ms(&ts, 5001), 11000.0, "top_sessions pid 5001 = 11 s");
    free(ts.rows);

    struct pgwt_queries_result tq;
    pgwt_compute_top_queries(evs, 2, &f, RA_FROM, RA_TO, 18000.0, &tq);
    const struct pgwt_query_row *q = qrow(&tq, 88);
    CHECK(q != NULL, "top_queries has the query row");
    if (q) {
        CHECK_MS(q->total_ms, 11000.0, "top_queries query 88 time = 11 s");
        CHECK_U64(q->count, 1,
                  "…and counts only the wait that closed in-window");
    }
    free(tq.rows);
}

/* 6b. The SAME fixture with the window extended past T. Now both waits close
 * inside, so both are occurrences and the full 10 s is in. This is the
 * over-correction guard: a fix that drops right-open records from TIME as
 * well, or that stops counting by position rather than by close timestamp,
 * goes red here. Green on 304a772 and must stay green. */
static void test_right_edge_counted_window(void)
{
    printf("--- 6b. window extended past T: the wait is counted in full ---\n");

    struct pgwt_trace_event evs[] = {
        mk(5001, LOCK_RELATION, RT,        S(10), 88, PGWT_CPU_NS_UNKNOWN),
        mk(5001, LOCK_RELATION, RT - S(5), S(3),  88, PGWT_CPU_NS_UNKNOWN),
    };
    struct pgwt_filter f;
    memset(&f, 0, sizeof(f));

    struct pgwt_events_result te;
    pgwt_compute_top_events(evs, 2, &f, RA_FROM, RT + S(1), 21000.0, &te);
    CHECK_MS(ev_total_ms(&te, LOCK_RELATION), 13000.0,
             "both waits whole: 10 s + 3 s");
    CHECK_U64(ev_count(&te, LOCK_RELATION), 2,
              "both waits counted — ev_count() is not stuck at 0");
    const struct pgwt_event_row *r = ev_row(&te, LOCK_RELATION);
    if (r) CHECK_NUM(r->max_us, 10000000.0,
                     "…max latency (us) is now the 10 s wait, which closed "
                     "in-window");
    free(te.rows);
}

/* 6c. A wait spanning the ENTIRE window: starts before `from`, closes after
 * `to`. The window sees nothing but this wait, so the answer is the full
 * window width with zero occurrences. Through the loader this row is 0 today
 * (#424's worst case); at this layer the time is already right and the
 * occurrence is not.
 *
 * NOTE for the fix: pgwt_compute_top_events uses count == 0 as its hash
 * table's empty-slot marker and skips count == 0 rows when collecting
 * results, so "time but no occurrence" needs a real occupancy flag. That is
 * why the time here is asserted on db_time_ms (an accumulator independent of
 * the row) as well as on the row. */
static void test_right_edge_whole_window_spanner(void)
{
    printf("--- 6c. a wait spanning the whole window ---\n");

    struct pgwt_trace_event evs[] = {
        /* [T-50, T+50] — the window [T-20, T-2] is strictly inside it */
        mk(5002, TIMEOUT_PGSLEEP, RT + S(50), S(100), 0, PGWT_CPU_NS_UNKNOWN),
    };
    struct pgwt_filter f;
    memset(&f, 0, sizeof(f));

    CHECK(evs[0].timestamp_ns - evs[0].duration_ns < RA_FROM &&
          evs[0].timestamp_ns > RA_TO,
          "fixture really spans the whole window on both sides");

    struct pgwt_tm_result tm;
    pgwt_compute_time_model(evs, 1, &f, RA_FROM, RA_TO, 18000.0, &tm);
    CHECK_MS(tm.db_time_ms, 18000.0, "spanner fills the window: 18 s");
    CHECK_MS(tm_class_ms(&tm, "Timeout"), 18000.0, "…on the Timeout row");
    free(tm.rows);

    struct pgwt_events_result te;
    pgwt_compute_top_events(evs, 1, &f, RA_FROM, RA_TO, 18000.0, &te);
    CHECK_MS(te.db_time_ms, 18000.0,
             "top_events still REPORTS the 18 s (independent accumulator)");
    CHECK_U64(ev_count(&te, TIMEOUT_PGSLEEP), 0,
              "…with zero occurrences: the wait closed outside the window");
    free(te.rows);

    struct pgwt_sessions_result ts;
    pgwt_compute_top_sessions(evs, 1, &f, RA_FROM, RA_TO, 18000.0, &ts);
    CHECK_MS(sess_ms(&ts, 5002), 18000.0, "top_sessions pid 5002 = 18 s");
    free(ts.rows);
}

/* 6d. Adjacent-window sweep over one 10 s wait: Σ time == the duration (no
 * part lost, none double-counted) and Σ count == 1 (the occurrence belongs to
 * exactly ONE window — the one its close falls in). The second is the
 * double-count hazard in one number: on 304a772 the record is counted in
 * every window of the sweep, including the two it does not overlap at all. */
static void test_right_edge_adjacent_sweep(void)
{
    printf("--- 6d. adjacent-window sweep: Σ time == duration, Σ count == 1 ---\n");

    struct pgwt_trace_event evs[] = {
        mk(5001, LOCK_RELATION, RT, S(10), 88, PGWT_CPU_NS_UNKNOWN),
    };
    struct pgwt_filter f;
    memset(&f, 0, sizeof(f));

    /* Edges chosen so T falls STRICTLY inside one window (never on an edge),
     * which keeps "which window owns the occurrence" unambiguous. */
    const uint64_t edges[] = {
        RT - S(14), RT - S(10), RT - S(6), RT - S(2), RT + S(2), RT + S(6),
    };
    const int nwin = (int)(sizeof(edges) / sizeof(edges[0])) - 1;

    double  sum_ms    = 0.0;
    uint64_t sum_count = 0;
    int owning_window = -1;
    for (int i = 0; i < nwin; i++) {
        struct pgwt_events_result te;
        pgwt_compute_top_events(evs, 1, &f, edges[i], edges[i + 1],
                                (double)(edges[i + 1] - edges[i]) / 1e6, &te);
        double  ms = ev_total_ms(&te, LOCK_RELATION);
        uint64_t c = ev_count(&te, LOCK_RELATION);
        if (ms > 0) sum_ms += ms;
        sum_count += c;
        if (c > 0 && owning_window < 0) owning_window = i;
        free(te.rows);
    }
    CHECK_MS(sum_ms, 10000.0, "Σ clipped time over the sweep == the 10 s wait");
    CHECK_U64(sum_count, 1, "Σ count over the sweep == 1 occurrence");
    CHECK_U64(owning_window, 3,
              "…and it belongs to window 3, [T-2, T+2], the one T falls in");
}

/* 6e. False-negative legs for section 6. Each is a way the right-edge rule
 * could be "satisfied" without looking at anything. */
static void test_right_edge_bypass(void)
{
    printf("--- 6e. right-edge false-negative suite ---\n");

    struct pgwt_filter f;
    memset(&f, 0, sizeof(f));

    struct pgwt_trace_event evs[] = {
        mk(5001, LOCK_RELATION,   RT,        S(10), 88, PGWT_CPU_NS_UNKNOWN),
        mk(5001, LOCK_RELATION,   RT - S(5), S(3),  88, PGWT_CPU_NS_UNKNOWN),
        mk(5002, TIMEOUT_PGSLEEP, RT + S(50), S(100), 0, PGWT_CPU_NS_UNKNOWN),
    };

    /* 6e-1. UNBOUNDED (to_ns == 0) must be BIT-IDENTICAL to today: no
     * admission rule applies, every record is whole, every record counts. */
    struct pgwt_events_result tu;
    pgwt_compute_top_events(evs, 3, &f, 0, 0, 170000.0, &tu);
    CHECK_MS(ev_total_ms(&tu, LOCK_RELATION), 13000.0,
             "unbounded: Lock keeps 10 s + 3 s");
    CHECK_U64(ev_count(&tu, LOCK_RELATION), 2,
              "unbounded: both Lock waits count");
    CHECK_MS(ev_total_ms(&tu, TIMEOUT_PGSLEEP), 100000.0,
             "unbounded: the 100 s spanner keeps its full duration");
    CHECK_U64(ev_count(&tu, TIMEOUT_PGSLEEP), 1,
              "unbounded: the spanner counts");
    free(tu.rows);

    /* 6e-2. A fixture that does NOT straddle must produce a NON-ZERO value,
     * so none of section 6 can pass by looking at nothing. */
    struct pgwt_events_result tn;
    pgwt_compute_top_events(&evs[1], 1, &f, RA_FROM, RA_TO, 18000.0, &tn);
    CHECK_MS(ev_total_ms(&tn, LOCK_RELATION), 3000.0,
             "non-straddling fixture is non-zero (3 s), so 0 is a real answer");
    CHECK_U64(ev_count(&tn, LOCK_RELATION), 1,
              "…and counts its one occurrence");
    free(tn.rows);

    /* 6e-3. Zero-width window at a point no record closes in: no time, no
     * occurrence — and no divide-by-zero in the in-window fraction. */
    struct pgwt_events_result tz;
    pgwt_compute_top_events(evs, 3, &f, RT - S(3), RT - S(3), 0.0, &tz);
    CHECK_MS(tz.db_time_ms, 0.0, "zero-width window: 0 ms");
    CHECK_U64(ev_count(&tz, LOCK_RELATION), 0,
              "zero-width window: 0 occurrences");
    free(tz.rows);

    /* 6e-4. A zero-duration record closing exactly at `to`: no time, but it
     * IS an occurrence — "instant" is the whole point of the rule. */
    struct pgwt_trace_event instant = mk(5003, LOCK_RELATION, RA_TO, 0, 88, 0);
    struct pgwt_events_result ti;
    pgwt_compute_top_events(&instant, 1, &f, RA_FROM, RA_TO, 18000.0, &ti);
    CHECK_MS(ev_total_ms(&ti, LOCK_RELATION), 0.0,
             "zero-duration record at `to`: 0 ms of time");
    CHECK_U64(ev_count(&ti, LOCK_RELATION), 1,
              "…but one occurrence: it closed inside the window");
    free(ti.rows);

    /* 6e-5. A record WHOLLY past `to` (starts after the window too): neither
     * time nor occurrence. The distinction from 6a is the point — "closes
     * after `to`" is not by itself a reason to keep a record. */
    struct pgwt_trace_event past =
        mk(5004, LOCK_RELATION, RT + S(30), S(2), 88, PGWT_CPU_NS_UNKNOWN);
    struct pgwt_events_result tp;
    pgwt_compute_top_events(&past, 1, &f, RA_FROM, RA_TO, 18000.0, &tp);
    CHECK_MS(tp.db_time_ms, 0.0, "record wholly past `to`: 0 ms");
    CHECK_U64(ev_count(&tp, LOCK_RELATION), 0,
              "record wholly past `to`: 0 occurrences");
    free(tp.rows);

    /* 6e-6. A record wholly BEFORE `from`: same, on the other side. */
    struct pgwt_trace_event before =
        mk(5005, LOCK_RELATION, RA_FROM - S(1), S(2), 88, PGWT_CPU_NS_UNKNOWN);
    struct pgwt_events_result tb;
    pgwt_compute_top_events(&before, 1, &f, RA_FROM, RA_TO, 18000.0, &tb);
    CHECK_MS(tb.db_time_ms, 0.0, "record wholly before `from`: 0 ms");
    CHECK_U64(ev_count(&tb, LOCK_RELATION), 0,
              "record wholly before `from`: 0 occurrences");
    free(tb.rows);

    /* 6e-7. Empty input: a real zero, not an unexamined one. Paired with
     * 6e-2 above, which proves the same call is non-zero on real data. */
    struct pgwt_events_result t0;
    pgwt_compute_top_events(NULL, 0, &f, RA_FROM, RA_TO, 18000.0, &t0);
    CHECK_MS(t0.db_time_ms, 0.0, "empty input: 0 ms");
    CHECK_U64(t0.num_rows, 0, "empty input: no rows");
    free(t0.rows);
}


/* ── 7. #426: AAS top-N MEMBERSHIP is selected on contribution ──────────
 *
 * `rank-reversing-boundary`. pgwt_compute_aas plots per-bucket OVERLAP but
 * used to rank candidates for the top-N cut by FULL duration
 * (evt_ht[].total_ns += ev->duration_ns). Those two orders are the same for
 * every interior event, which is why this went unnoticed: the only inputs
 * that separate them are boundary straddlers.
 *
 * A: Lock:relation, 10 s long, ending 1 s after the window opens
 *      -> full duration 10 s, CONTRIBUTION 1 s
 * B: IO:DataFileRead, 2 s long, wholly inside
 *      -> full duration 2 s, CONTRIBUTION 2 s
 *
 * With max_series = 1 the two selectors disagree by construction: by full
 * duration A wins (10 > 2) and draws 1 s of area while B draws none; by
 * contribution B wins (2 > 1), which is what the chart's own axis means.
 *
 * The fixture self-test below asserts BOTH inequalities, so the membership
 * assertion cannot be satisfied by a no-op: this input is only green under
 * contribution selection and only red under full-duration selection. The
 * latency half of the #426 contract is asserted in the same breath -- A's
 * Events-tab latency must still read the whole 10 s, because clipping is a
 * rule about TIME IN WINDOW and never about how long a wait lasted.
 */
static void test_rank_reversing_boundary(void)
{
    printf("--- 7. #426: top-N membership follows contribution, not duration ---\n");

    struct pgwt_trace_event evs[] = {
        /* A: ends 1 s into the window, 10 s long  -> contribution 1 s */
        mk(9001, LOCK_RELATION, WFROM + S(1), S(10), 0, PGWT_CPU_NS_UNKNOWN),
        /* B: [WFROM+3, WFROM+5), 2 s long         -> contribution 2 s */
        mk(9002, IO_READ,       WFROM + S(5), S(2),  0, PGWT_CPU_NS_UNKNOWN),
    };

    /* FIXTURE SELF-TEST: the two selectors must provably disagree here, or
     * the assertions below prove nothing. */
    uint64_t a_full = evs[0].duration_ns, b_full = evs[1].duration_ns;
    uint64_t a_contrib = evs[0].timestamp_ns - WFROM;  /* A opens before `from` */
    uint64_t b_contrib = evs[1].duration_ns;           /* wholly interior */
    CHECK(a_full > b_full,
          "fixture: by FULL DURATION A outranks B (10 s vs 2 s)");
    CHECK(a_contrib < b_contrib,
          "fixture: by CONTRIBUTION B outranks A (1 s vs 2 s) -- the two "
          "selectors disagree, so this input can tell them apart");

    struct pgwt_filter f;
    memset(&f, 0, sizeof(f));

    struct pgwt_aas_result aas;
    pgwt_compute_aas(evs, 2, &f, WFROM, WTO, 10, 1 /*detail*/, 1 /*max_series*/,
                     &aas);
    CHECK_U64(aas.num_event_series, 1,
              "exactly one series survives the top-N=1 cut");
    if (aas.num_event_series == 1) {
        CHECK(aas.event_series[0].event_id == IO_READ,
              "the SURVIVOR is B (IO:DataFileRead, 2 s in window), not A "
              "(Lock:relation, 10 s long but 1 s in window)");
        /* The ranking quantity itself, not just its argmax: compute.h
         * documents total_aas as "sum across all buckets", which under
         * full-duration accumulation was false for any straddler. */
        CHECK_NUM(aas.event_series[0].total_aas, 2.0 / 10.0,
                  "…and its total_aas is its in-window 2 s / 10 s window");
    }
    free(aas.buckets);
    free(aas.event_aas);

    /* With room for both, A's own ranking quantity must be its 1 s of
     * contribution (0.1 AAS), not its 10 s of duration (1.0 AAS). This is
     * the assertion that goes red on the full-duration site even if a future
     * max_series default made membership moot. */
    struct pgwt_aas_result aas2;
    pgwt_compute_aas(evs, 2, &f, WFROM, WTO, 10, 1, 8, &aas2);
    CHECK_U64(aas2.num_event_series, 2, "both events are series when N=8");
    double a_aas = -1.0, b_aas = -1.0;
    for (int i = 0; i < aas2.num_event_series; i++) {
        if (aas2.event_series[i].event_id == LOCK_RELATION)
            a_aas = aas2.event_series[i].total_aas;
        if (aas2.event_series[i].event_id == IO_READ)
            b_aas = aas2.event_series[i].total_aas;
    }
    CHECK_NUM(a_aas, 1.0 / 10.0,
              "A's total_aas is 1 s / 10 s window (full duration reads 1.0)");
    CHECK_NUM(b_aas, 2.0 / 10.0, "B's total_aas is 2 s / 10 s window");
    CHECK(b_aas > a_aas,
          "…so B outranks A on the quantity the chart actually plots");
    CHECK(aas2.event_series[0].event_id == IO_READ,
          "and the sorted array puts B first");
    free(aas2.buckets);
    free(aas2.event_aas);

    /* THE OTHER HALF OF THE CONTRACT: A's latency is untouched. */
    struct pgwt_events_result te;
    pgwt_compute_top_events(evs, 2, &f, WFROM, WTO, 10000.0, &te);
    const struct pgwt_event_row *ra = ev_row(&te, LOCK_RELATION);
    CHECK(ra != NULL, "Events has A's row");
    if (ra) {
        CHECK_MS(ra->total_ms, 1000.0, "…A's Total is its 1 s in window");
        CHECK(fabs(ra->avg_us - 10000000.0) < 1e-3,
              "…and A's latency still reads the whole 10 s");
    }
    free(te.rows);
}

/* ── 8. #440: equal values come back in IDENTITY order, reproducibly ────
 *
 * cmp_event_row_desc / cmp_session_row_desc / cmp_query_row_desc ordered by
 * value alone, which is not a total order over a set with ties, and qsort is
 * not stable -- so the row order of a tie group was a function of the hash
 * table's probe sequence and the partition, i.e. of nothing a caller can
 * see. Under a max_rows cut that decides WHICH ROWS are returned, not just
 * their order.
 *
 * Three events, three pids and three query ids, every one carrying exactly
 * 1 s wholly inside the window: a perfect three-way tie on all three tables
 * at once. The same multiset is fed in two different array orders, and both
 * must come back in ascending identity order. Asserting the ORDER (rather
 * than only that the two runs agree) is what makes this a gate: two runs
 * could agree by accident on one input, but neither can land on ascending
 * identity by accident on both permutations.
 *
 * Neither permutation is already in identity order as written, so a
 * "preserve input order" implementation fails both.
 */
static void test_tie_break_is_identity(void)
{
    printf("--- 8. #440: ties resolve on identity, in both input orders ---\n");

    /* The three event ids are chosen so that IDENTITY order and the
     * accumulator's own COLLECTION order are exactly reversed, which is what
     * makes the Events assertion a gate rather than a coincidence.
     * pgwt_compute_top_events hashes `event_id & EVENT_HT_MASK`, and the
     * class lives in the high byte, so an event's slot is its NUMBER:
     *
     *   TIE_LWLOCK 0x0100001E  slot 30   id rank 1
     *   TIE_LOCK   0x03000014  slot 20   id rank 2
     *   TIE_IO     0x0A00000A  slot 10   id rank 3
     *
     * so collecting the table in slot order yields IO, LOCK, LWLOCK -- the
     * reverse of the required answer. The first draft of this section used
     * Lock/Timeout/IO with slots 0/2/21, where slot order HAPPENED to equal
     * id order, and the value-only comparator passed it. None of the three
     * is idle or hidden (only Activity-class, Client:ClientRead and the six
     * named Timeout pacing events are), so all three really do appear.
     * Pids and qids ascend with their numbers. */
    #define TIE_LWLOCK  EV(0x01u, 30)
    #define TIE_LOCK    EV(CLASS_LOCK, 20)
    #define TIE_IO      EV(CLASS_IO, 10)
    struct pgwt_trace_event fwd[] = {
        mk(7002, TIE_LOCK,   WFROM + S(2), S(1), 502, PGWT_CPU_NS_UNKNOWN),
        mk(7003, TIE_IO,     WFROM + S(4), S(1), 503, PGWT_CPU_NS_UNKNOWN),
        mk(7001, TIE_LWLOCK, WFROM + S(6), S(1), 501, PGWT_CPU_NS_UNKNOWN),
    };
    struct pgwt_trace_event rev[3];
    for (int i = 0; i < 3; i++) rev[i] = fwd[2 - i];

    struct pgwt_filter f;
    memset(&f, 0, sizeof(f));

    const uint32_t want_ev[3]  = { TIE_LWLOCK, TIE_LOCK, TIE_IO };
    const uint32_t want_pid[3] = { 7001, 7002, 7003 };
    const uint64_t want_qid[3] = { 501, 502, 503 };

    for (int perm = 0; perm < 2; perm++) {
        const struct pgwt_trace_event *in = perm ? rev : fwd;

        struct pgwt_events_result te;
        pgwt_compute_top_events(in, 3, &f, WFROM, WTO, 10000.0, &te);
        /* FIXTURE SELF-TEST: a tie this section cannot see is a tie it
         * cannot test. All three rows must be present AND equal. */
        int tie_rows = 0;
        double first = -1.0;
        int all_equal = 1;
        for (int i = 0; i < te.num_rows; i++) {
            for (int k = 0; k < 3; k++) {
                if (te.rows[i].event_id != want_ev[k]) continue;
                tie_rows++;
                if (first < 0) first = te.rows[i].total_ms;
                else if (fabs(te.rows[i].total_ms - first) > 1e-9)
                    all_equal = 0;
            }
        }
        CHECK_U64(tie_rows, 3, "all three tied event rows are present");
        CHECK(all_equal && fabs(first - 1000.0) < 1e-6,
              "…and they really TIE at 1000 ms (an untied fixture would be "
              "ordered correctly by the old comparator too)");
        /* The order of the tie group, extracted in served order. */
        int seen = 0, order_ok = 1;
        for (int i = 0; i < te.num_rows && seen < 3; i++) {
            for (int k = 0; k < 3; k++) {
                if (te.rows[i].event_id != want_ev[k]) continue;
                if (te.rows[i].event_id != want_ev[seen]) order_ok = 0;
                seen++;
            }
        }
        CHECK(order_ok && seen == 3,
              perm ? "Events tie group is event_id-ascending (reversed input)"
                   : "Events tie group is event_id-ascending (as-written input)");
        free(te.rows);

        struct pgwt_sessions_result ts;
        pgwt_compute_top_sessions(in, 3, &f, WFROM, WTO, 10000.0, &ts);
        CHECK_U64(ts.num_rows, 3, "three tied session rows");
        int s_ok = (ts.num_rows == 3);
        for (int i = 0; i < ts.num_rows && i < 3; i++) {
            if (ts.rows[i].pid != want_pid[i]) s_ok = 0;
            if (fabs(ts.rows[i].db_time_ms - 1000.0) > 1e-6) s_ok = 0;
        }
        CHECK(s_ok, perm ? "Sessions ties are pid-ascending (reversed input)"
                         : "Sessions ties are pid-ascending (as-written input)");
        free(ts.rows);

        struct pgwt_queries_result tq;
        pgwt_compute_top_queries(in, 3, &f, WFROM, WTO, 10000.0, &tq);
        CHECK_U64(tq.num_rows, 3, "three tied query rows");
        int q_ok = (tq.num_rows == 3);
        for (int i = 0; i < tq.num_rows && i < 3; i++) {
            if (tq.rows[i].query_id != want_qid[i]) q_ok = 0;
            if (fabs(tq.rows[i].total_ms - 1000.0) > 1e-6) q_ok = 0;
        }
        CHECK(q_ok, perm ? "Queries ties are query_id-ascending (reversed input)"
                         : "Queries ties are query_id-ascending (as-written input)");
        free(tq.rows);
    }
}

/* ── 9. #441: the Queries latency average needs full-duration evidence ──
 *
 * `count` on a Queries row counts non-idle wait AND CPU intervals, exact and
 * sampled alike (ASH math makes the count and the time valid). `avg_us` is a
 * LATENCY and needs real durations, which a sampled record does not have --
 * its duration_ns is a normalized sample period, so averaging it reports a
 * wait the capture never measured. pgwt_compute_top_events has refused that
 * since FID-3 via its own exact_count; pgwt_compute_top_queries did not.
 *
 * Fixture: query 801 is exact-only (two 10 ms intervals -> 10 ms average);
 * query 802 is sampled-only (two 10 ms sample periods -> exact_count 0, so
 * the average is UNAVAILABLE); query 803 is mixed, and its average must be
 * taken over the exact observations ALONE (one 30 ms interval -> 30 ms),
 * never over all of them. 803 is the row that distinguishes "gated the whole
 * row" from "gated the right denominator": a fix that only skipped
 * sampled-only rows would report 803 as (30+10)/2 = 20 ms.
 */
static void test_query_avg_needs_exact_evidence(void)
{
    printf("--- 9. #441: Queries avg is exact-only, or unavailable ---\n");

    struct pgwt_trace_event evs[6];
    int n = 0;
    /* 801: exact, two 10 ms IO waits. */
    evs[n++] = mk(8001, IO_READ, WFROM + S(1), 10000000ULL, 801,
                  PGWT_CPU_NS_UNKNOWN);
    evs[n++] = mk(8001, IO_READ, WFROM + S(2), 10000000ULL, 801,
                  PGWT_CPU_NS_UNKNOWN);
    /* 802: sampled, two 10 ms sample periods. */
    evs[n] = mk(8002, IO_READ, WFROM + S(3), 10000000ULL, 802,
                PGWT_CPU_NS_UNKNOWN);
    evs[n].flags |= PGWT_EVENT_FLAG_SAMPLE;
    n++;
    evs[n] = mk(8002, IO_READ, WFROM + S(4), 10000000ULL, 802,
                PGWT_CPU_NS_UNKNOWN);
    evs[n].flags |= PGWT_EVENT_FLAG_SAMPLE;
    n++;
    /* 803: mixed -- one 30 ms exact wait and one 10 ms sample. */
    evs[n++] = mk(8003, IO_READ, WFROM + S(5), 30000000ULL, 803,
                  PGWT_CPU_NS_UNKNOWN);
    evs[n] = mk(8003, IO_READ, WFROM + S(6), 10000000ULL, 803,
                PGWT_CPU_NS_UNKNOWN);
    evs[n].flags |= PGWT_EVENT_FLAG_SAMPLE;
    n++;

    struct pgwt_filter f;
    memset(&f, 0, sizeof(f));
    struct pgwt_queries_result tq;
    pgwt_compute_top_queries(evs, n, &f, WFROM, WTO, 10000.0, &tq);

    const struct pgwt_query_row *q1 = qrow(&tq, 801);
    CHECK(q1 != NULL, "exact-only query 801 has a row");
    if (q1) {
        CHECK_U64(q1->count, 2, "…two intervals counted");
        CHECK_U64(q1->exact_count, 2, "…both are exact, so both are evidence");
        CHECK(fabs(q1->avg_us - 10000.0) < 1e-3,
              "…average interval is the real 10 ms");
    }

    const struct pgwt_query_row *q2 = qrow(&tq, 802);
    CHECK(q2 != NULL, "sampled-only query 802 still HAS a row (ASH math: its "
                      "count and DB Time are valid)");
    if (q2) {
        CHECK_U64(q2->count, 2, "…its two sampled observations are counted");
        CHECK_MS(q2->total_ms, 20.0, "…and its DB Time is real");
        CHECK_U64(q2->exact_count, 0,
                  "…but it carries NO full-duration evidence, so the latency "
                  "average is unavailable (server.c emits null)");
    }

    const struct pgwt_query_row *q3 = qrow(&tq, 803);
    CHECK(q3 != NULL, "mixed query 803 has a row");
    if (q3) {
        CHECK_U64(q3->count, 2, "…both observations counted");
        CHECK_U64(q3->exact_count, 1, "…one of them is exact");
        CHECK(fabs(q3->avg_us - 30000.0) < 1e-3,
              "…and the average is over the EXACT one alone (30 ms), not over "
              "both observations (which would read 20 ms)");
    }
    free(tq.rows);
}

/* ── 10. #553: the CPU* row is MEASURED on-CPU time, not the gap's wall ──
 *
 * THE DEFECT. pgwt_compute_top_events added the on-CPU interval's window-
 * clipped WALL to the CPU* row. Every WAIT row is right to carry wall -- the
 * wall IS the wait -- but the gap between two waits is "on a core" only for
 * the part sum_exec_runtime actually advanced; the rest is runqueue time,
 * which pgwt_compute_time_model reports as Off-CPU*. So one invocation
 * answered the same question twice: this table's CPU* exceeded that
 * invocation's time_model CPU* by exactly the off-CPU amount. This is the
 * compute path behind pgwt-server's top_events response -- what the browser
 * draws -- and #531 is the same defect on the CLI replay path
 * (src/event_reader.c).
 *
 * WHY THE FIXTURE IS ABSOLUTE, not an agreement check. "top_events agrees
 * with time_model" is satisfied by both carrying wall, which is exactly how
 * #531 survived until someone wrote a 10 ms-wall / 4 ms-measured fixture. So
 * the numbers below are hand-computed from the fixture and asserted as
 * values; the cross-tab agreement is checked too, but only AFTER each side
 * has been pinned to its own literal.
 *
 * Likewise the conservation identity DB == CPU* + Off-CPU* + Sigma waits is
 * NOT the gate: off-CPU is the on-CPU gaps' wall minus their measurement, so
 * that sum is true by construction and cannot fail. Each of the four terms is
 * asserted against its own literal instead, and the identity is then stated.
 */
#define MS_NS(x)  ((uint64_t)(x) * 1000000ULL)

static void test_cpu_row_is_measured(void)
{
    printf("--- 10. #553: CPU* carries measured cpu_ns, waits carry wall ---\n");

    struct pgwt_filter f;
    memset(&f, 0, sizeof(f));

    /* 10a. The #531 shape, wholly inside the window:
     *        one on-CPU gap  10 ms wall, 4 ms measured on-CPU
     *        one Lock:relation wait 5 ms
     *      DB Time 15 ms = CPU* 4 + Off-CPU* 6 + Lock 5. */
    struct pgwt_trace_event evs[] = {
        mk(1, CPU_GAP,       WFROM + S(1), MS_NS(10), 21, MS_NS(4)),
        mk(1, LOCK_RELATION, WFROM + S(2), MS_NS(5),  21, PGWT_CPU_NS_UNKNOWN),
    };
    struct pgwt_events_result te;
    pgwt_compute_top_events(evs, 2, &f, WFROM, WTO, 10000.0, &te);

    /* The row must EXIST. ev_total_ms() answers -1.0 for an absent row, which
     * would satisfy any "CPU* is below the wall" style check without the
     * product computing anything -- section 11a pins that -1.0. */
    const struct pgwt_event_row *cpu = ev_row(&te, CPU_GAP);
    CHECK(cpu != NULL, "the CPU* row is present");
    CHECK_MS(ev_total_ms(&te, CPU_GAP), 4.0,
             "CPU* reports the MEASURED 4 ms (wall would read 10.0)");
    CHECK_MS(ev_total_ms(&te, LOCK_RELATION), 5.0,
             "the Lock wait keeps its FULL 5 ms wall -- only the CPU row moved");
    CHECK_MS(te.db_time_ms, 15.0,
             "DB Time is still wall: 10 ms gap + 5 ms wait");
    CHECK(te.has_measured_cpu != 0,
          "the result declares the CPU* row a measurement");
    CHECK_MS(te.offcpu_ms, 6.0,
             "the 6 ms the CPU row no longer claims is REPORTED as off-CPU, "
             "not dropped");

    /* Conservation, stated. Each term above was pinned to its own literal
     * first; this line only records that they close on DB Time. */
    CHECK_MS(ev_total_ms(&te, CPU_GAP) + te.offcpu_ms +
             ev_total_ms(&te, LOCK_RELATION), te.db_time_ms,
             "DB Time == CPU* + Off-CPU* + Sigma waits (4 + 6 + 5 == 15)");

    /* The %DB consequence, pinned rather than left for a reader to discover:
     * this table has no Off-CPU* ROW, so its %DB column now sums to 100%
     * minus the off-CPU share instead of to a flat 100% that counted runqueue
     * time as CPU. */
    if (cpu)
        CHECK_NUM(cpu->pct_db, 4.0 / 15.0 * 100.0,
                  "CPU* %DB is the measured share (26.67%), not 66.67%");
    double pct_sum = 0.0;
    for (int i = 0; i < te.num_rows; i++)
        pct_sum += te.rows[i].pct_db;
    CHECK_NUM(pct_sum, 60.0,
              "the %DB column sums to 60% -- the missing 40% is offcpu_ms, "
              "which this view has no row for");

    /* The LATENCY columns are a property of the interval, not of this fix: a
     * 10 ms on-CPU gap still lasted 10 ms. If a future change "fixed" the
     * time column by rewriting duration_ns, these go red. */
    if (cpu) {
        CHECK_U64(cpu->count, 1, "the gap is still ONE observation");
        CHECK(fabs(cpu->avg_us - 10000.0) < 1e-3,
              "CPU* avg is the gap's 10 ms wall (latency is not clipped and "
              "not replaced by the measurement)");
        CHECK(fabs(cpu->max_us - 10000.0) < 1e-3,
              "CPU* max is the gap's 10 ms wall");
    }

    /* RANKING follows the corrected number: with CPU* at 4 ms the 5 ms Lock
     * is now the bigger row. Every assertion in this section looks rows up by
     * EVENT ID (ev_row/ev_total_ms), never by index, so this reordering
     * cannot change what any of them read -- it is asserted here as the
     * user-visible consequence, not relied on anywhere. */
    CHECK(te.num_rows == 2, "both rows are present");
    if (te.num_rows == 2) {
        CHECK(te.rows[0].event_id == LOCK_RELATION &&
              te.rows[1].event_id == CPU_GAP,
              "the 5 ms Lock now outranks the 4 ms CPU* (wall ranked CPU* "
              "first with 10 ms)");
    }

    /* Cross-tab: time_model over the SAME events, same window. Checked only
     * after both sides were pinned to literals above -- agreement alone is
     * satisfiable by both carrying wall. */
    struct pgwt_tm_result tm;
    pgwt_compute_time_model(evs, 2, &f, WFROM, WTO, 10000.0, &tm);
    CHECK_MS(tm.cpu_ms, 4.0, "time_model CPU* is the same measured 4 ms");
    CHECK_MS(tm.offcpu_ms, 6.0, "time_model Off-CPU* is the same 6 ms");
    CHECK_MS(ev_total_ms(&te, CPU_GAP), tm.cpu_ms,
             "and the two tabs agree within one invocation");
    CHECK_MS(te.offcpu_ms, tm.offcpu_ms,
             "...on the off-CPU remainder too");
    free(tm.rows);
    free(te.rows);

    /* 10b. A STRADDLING gap, which separates the three candidate
     * implementations. 20 ms wall ending 10 ms into the window (so 10 ms
     * inside), 8 ms measured on-CPU over the whole 20 ms:
     *     wall            -> 10.0 ms  (the defect)
     *     cpu_ns taken whole ->  8.0 ms  (measurement not clipped)
     *     measured * overlap ->  4.0 ms  (correct: 8 * 10/20)
     * Off-CPU* is the rest of the in-window wall: 10 - 4 = 6. */
    struct pgwt_trace_event strad =
        mk(2, CPU_GAP, WFROM + MS_NS(10), MS_NS(20), 22, MS_NS(8));
    struct pgwt_events_result ts;
    pgwt_compute_top_events(&strad, 1, &f, WFROM, WTO, 10000.0, &ts);
    CHECK_MS(ev_total_ms(&ts, CPU_GAP), 4.0,
             "a straddling gap contributes measured * overlap (4 ms), not the "
             "whole measurement (8) and not the clipped wall (10)");
    CHECK_MS(ts.db_time_ms, 10.0, "its DB Time is the clipped wall, 10 ms");
    CHECK_MS(ts.offcpu_ms, 6.0, "off-CPU is the in-window remainder, 6 ms");
    CHECK(ts.has_measured_cpu != 0, "and it is still a measurement");
    free(ts.rows);
}

/* ── 11. #553 bypass suite: the measured-CPU gate must refuse, not approve ──
 *
 * Section 10 proves the TRUE-POSITIVE path (it goes red on the parent tree).
 * This section walks the FALSE NEGATIVES: every way the CPU* row could stop
 * being checked, or could be "right" without anything being measured. A gate
 * that cannot see must refuse, never approve -- so where no measurement
 * exists, has_measured_cpu must read 0 and offcpu_ms 0, which is a refusal to
 * split, NOT a claim that off-CPU time was zero.
 */
static void test_cpu_row_measured_bypass(void)
{
    printf("--- 11. #553 false-negative suite ---\n");

    struct pgwt_filter f;
    memset(&f, 0, sizeof(f));

    /* 11a. The thing being checked is ABSENT rather than wrong: no CPU gap in
     * the window at all. The row must be missing (ev_total_ms answers -1.0,
     * which is what keeps section 10's existence check non-vacuous), and the
     * result must not report a measured split it never made. */
    struct pgwt_trace_event wait_only =
        mk(1, LOCK_RELATION, WFROM + S(1), MS_NS(5), 31, PGWT_CPU_NS_UNKNOWN);
    struct pgwt_events_result a;
    pgwt_compute_top_events(&wait_only, 1, &f, WFROM, WTO, 10000.0, &a);
    CHECK(ev_row(&a, CPU_GAP) == NULL, "no CPU gap => no CPU* row");
    CHECK_MS(ev_total_ms(&a, CPU_GAP), -1.0,
             "...and the lookup answers -1.0 for it, so an absent row cannot "
             "satisfy a 'CPU* <= wall' style check");
    CHECK(a.has_measured_cpu == 0, "...has_measured_cpu refuses");
    CHECK_MS(a.offcpu_ms, 0.0, "...and no off-CPU is invented");
    CHECK_MS(ev_total_ms(&a, LOCK_RELATION), 5.0,
             "...while the wait row is untouched");
    free(a.rows);

    /* 11b. The MEASUREMENT is absent (sampled / v2 / no CPU capability):
     * cpu_ns == PGWT_CPU_NS_UNKNOWN. Gap-inference keeps the whole gap on
     * CPU*, and the result says the row is NOT a measurement. */
    struct pgwt_trace_event unk =
        mk(1, CPU_GAP, WFROM + S(1), MS_NS(10), 32, PGWT_CPU_NS_UNKNOWN);
    struct pgwt_events_result b;
    pgwt_compute_top_events(&unk, 1, &f, WFROM, WTO, 10000.0, &b);
    CHECK_MS(ev_total_ms(&b, CPU_GAP), 10.0,
             "UNKNOWN cpu_ns: CPU* is the whole 10 ms gap (gap-inference)");
    CHECK(b.has_measured_cpu == 0,
          "...and is declared NOT a measurement, so nobody reads it as one");
    CHECK_MS(b.offcpu_ms, 0.0,
             "...no off-CPU is split out of an unmeasured gap");
    free(b.rows);

    /* 11c. Measured == wall. This is the input that makes the BROKEN code
     * look right, so it cannot be the only fixture: 10 ms wall, 10 ms on-CPU
     * reads 10.0 either way. What distinguishes them is the two flags. */
    struct pgwt_trace_event full =
        mk(1, CPU_GAP, WFROM + S(1), MS_NS(10), 33, MS_NS(10));
    struct pgwt_events_result c;
    pgwt_compute_top_events(&full, 1, &f, WFROM, WTO, 10000.0, &c);
    CHECK_MS(ev_total_ms(&c, CPU_GAP), 10.0,
             "a gap entirely on-CPU reports 10 ms -- same as wall, by "
             "coincidence of the input");
    CHECK(c.has_measured_cpu != 0,
          "...but it IS measured (this is how the fixture tells 'no off-CPU' "
          "from 'cannot measure')");
    CHECK_MS(c.offcpu_ms, 0.0, "...and the remainder is a real zero");
    free(c.rows);

    /* 11d. A measured ZERO is a signal, not absence: the gap was all
     * runqueue. Reading 0 as UNKNOWN would put the whole 10 ms back on CPU*. */
    struct pgwt_trace_event zero =
        mk(1, CPU_GAP, WFROM + S(1), MS_NS(10), 34, 0);
    struct pgwt_events_result d;
    pgwt_compute_top_events(&zero, 1, &f, WFROM, WTO, 10000.0, &d);
    CHECK_MS(ev_total_ms(&d, CPU_GAP), 0.0,
             "cpu_ns == 0: CPU* is 0 ms, the gap was never on a core");
    CHECK(d.has_measured_cpu != 0, "...a measured zero still counts as "
                                   "measured");
    CHECK_MS(d.offcpu_ms, 10.0, "...and all 10 ms is off-CPU");
    CHECK_MS(d.db_time_ms, 10.0, "...DB Time is unchanged at the 10 ms wall");
    free(d.rows);

    /* 11e. cpu_ns EXCEEDS its own wall (sum_exec_runtime is only current at a
     * tick or a context switch, so a leaked sub-ms burst can). The row must
     * clamp to the time that actually elapsed, never exceed it. */
    struct pgwt_trace_event skew =
        mk(1, CPU_GAP, WFROM + S(1), MS_NS(10), 35, MS_NS(25));
    struct pgwt_events_result e;
    pgwt_compute_top_events(&skew, 1, &f, WFROM, WTO, 10000.0, &e);
    CHECK_MS(ev_total_ms(&e, CPU_GAP), 10.0,
             "25 ms measured over a 10 ms gap clamps to 10 ms");
    CHECK_MS(e.offcpu_ms, 0.0, "...and the remainder floors at 0, never "
                               "negative");
    CHECK(e.has_measured_cpu != 0, "...still a measurement");
    free(e.rows);

    /* 11f. EMPTY input: nothing to look at. Every number must be a real zero
     * and no measurement may be claimed. */
    struct pgwt_events_result g;
    pgwt_compute_top_events(NULL, 0, &f, WFROM, WTO, 10000.0, &g);
    CHECK(g.num_rows == 0, "empty input: no rows");
    CHECK_MS(g.db_time_ms, 0.0, "empty input: DB Time 0");
    CHECK(g.has_measured_cpu == 0, "empty input claims no measurement");
    CHECK_MS(g.offcpu_ms, 0.0, "empty input invents no off-CPU");
    free(g.rows);

    /* 11g. ZERO-WIDTH window over a measured gap: zero overlap is ABSENCE,
     * not a measured zero -- and the in-window fraction must not divide by a
     * zero duration. */
    struct pgwt_events_result h;
    pgwt_compute_top_events(&full, 1, &f, WFROM, WFROM, 0.0, &h);
    CHECK_MS(ev_total_ms(&h, CPU_GAP), 0.0, "zero-width window: CPU* 0 ms");
    CHECK_MS(h.db_time_ms, 0.0, "zero-width window: DB Time 0");
    CHECK(h.has_measured_cpu == 0,
          "zero OVERLAP is absence: no measurement is claimed for a window "
          "the gap does not intersect");
    CHECK_MS(h.offcpu_ms, 0.0, "zero-width window: no off-CPU");
    free(h.rows);

    /* 11h. ZERO-DURATION record carrying a measurement: frac is 0, so nothing
     * is attributed and nothing divides by zero. */
    struct pgwt_trace_event zdur =
        mk(1, CPU_GAP, WFROM + S(1), 0, 36, MS_NS(4));
    struct pgwt_events_result i2;
    pgwt_compute_top_events(&zdur, 1, &f, WFROM, WTO, 10000.0, &i2);
    CHECK_MS(ev_total_ms(&i2, CPU_GAP), 0.0,
             "a zero-duration record contributes 0 ms, not its cpu_ns");
    CHECK(i2.has_measured_cpu == 0,
          "...and claims no measurement (no wall to measure over)");
    CHECK_MS(i2.offcpu_ms, 0.0, "...and no off-CPU");
    free(i2.rows);

    /* 11i. A WAIT that carries measured cpu_ns (a backend spinning on-CPU
     * inside an LWLock, S3). Its wall stays on the WAIT row with its label --
     * the fix must not move wait time onto CPU*, nor count the spin as
     * off-CPU. This is the over-reach direction: a change that keyed on
     * "cpu_ns is present" instead of "the record is a CPU gap" goes red
     * here. */
    struct pgwt_trace_event spin =
        mk(1, LOCK_RELATION, WFROM + S(1), MS_NS(5), 37, MS_NS(3));
    struct pgwt_events_result j;
    pgwt_compute_top_events(&spin, 1, &f, WFROM, WTO, 10000.0, &j);
    CHECK_MS(ev_total_ms(&j, LOCK_RELATION), 5.0,
             "a wait with measured spin keeps its full 5 ms wall and label");
    CHECK(ev_row(&j, CPU_GAP) == NULL,
          "...no CPU* row is conjured from a wait's spin");
    CHECK_MS(j.offcpu_ms, 0.0, "...and a wait's spin is not off-CPU time");
    CHECK(j.has_measured_cpu == 0,
          "...the CPU* row is what the flag is about; there is none here");
    free(j.rows);

    /* 11j. An IDLE interval carrying a measurement (Client:ClientRead). The
     * time model skips such an interval WHOLE, including its cpu_ns
     * (tests/test_idle_accounting.c); this table lists it at wall but outside
     * DB Time, so neither CPU* nor off-CPU may take anything from it. */
    struct pgwt_trace_event idle =
        mk(1, CLIENT_READ, WFROM + S(1), MS_NS(8), 38, MS_NS(2));
    struct pgwt_events_result k;
    pgwt_compute_top_events(&idle, 1, &f, WFROM, WTO, 10000.0, &k);
    CHECK_MS(ev_total_ms(&k, CLIENT_READ), 8.0,
             "an idle interval keeps its 8 ms wall on its own row");
    CHECK_MS(k.db_time_ms, 0.0, "...and stays out of DB Time");
    CHECK(k.has_measured_cpu == 0,
          "...its cpu_ns does not make this a measured-CPU window");
    CHECK_MS(k.offcpu_ms, 0.0, "...and yields no off-CPU");
    free(k.rows);

    /* 11k. MIXED tiers in one window: one measured gap (10 ms wall / 4 ms
     * on-CPU) and one UNKNOWN gap (6 ms). CPU* is measured + unknown-wall =
     * 10 ms; off-CPU comes ONLY from the measured gap, so the unknown gap's
     * wall is not counted twice. */
    struct pgwt_trace_event mix[] = {
        mk(1, CPU_GAP, WFROM + S(1), MS_NS(10), 39, MS_NS(4)),
        mk(2, CPU_GAP, WFROM + S(2), MS_NS(6),  40, PGWT_CPU_NS_UNKNOWN),
    };
    struct pgwt_events_result l;
    pgwt_compute_top_events(mix, 2, &f, WFROM, WTO, 10000.0, &l);
    CHECK_MS(ev_total_ms(&l, CPU_GAP), 10.0,
             "mixed tiers: CPU* is 4 ms measured + 6 ms inferred");
    CHECK_MS(l.offcpu_ms, 6.0,
             "...off-CPU is the measured gap's remainder only (6 ms), not the "
             "unknown gap's wall as well");
    CHECK_MS(l.db_time_ms, 16.0, "...DB Time is the two walls, 16 ms");
    CHECK(l.has_measured_cpu != 0,
          "...and the window reports that it CAN split (one gap was measured)");
    free(l.rows);

    /* 11l. A FILTER removes the measured record. The flag must come back
     * down: the response describes the rows it returned, not the trace. */
    struct pgwt_filter pf;
    memset(&pf, 0, sizeof(pf));
    pf.pid = 2;                      /* keeps only the UNKNOWN gap above */
    struct pgwt_events_result m;
    pgwt_compute_top_events(mix, 2, &pf, WFROM, WTO, 10000.0, &m);
    CHECK_MS(ev_total_ms(&m, CPU_GAP), 6.0,
             "filtered to pid 2: CPU* is that pid's 6 ms unknown gap");
    CHECK(m.has_measured_cpu == 0,
          "...and the measured flag goes back DOWN -- the filtered-out "
          "measurement is not described by this response");
    CHECK_MS(m.offcpu_ms, 0.0, "...with no off-CPU carried over");
    free(m.rows);
}

int main(void)
{
    printf("=== test_window_clip ===\n");

    test_straddler_leading_edge();
    test_straddler_trailing_edge();
    test_cross_tab_agreement();
    test_unbounded_window_unchanged();
    test_bypass_paths();
    test_latency_columns_keep_full_duration();
    test_right_edge_open_wait();
    test_right_edge_counted_window();
    test_right_edge_whole_window_spanner();
    test_right_edge_adjacent_sweep();
    test_right_edge_bypass();
    test_rank_reversing_boundary();
    test_tie_break_is_identity();
    test_query_avg_needs_exact_evidence();
    test_cpu_row_is_measured();
    test_cpu_row_measured_bypass();

    /* A suite that ran no assertions is a suite that proved nothing. The
     * floor moves WITH the suite: sections 7-9 (#426, #440, #441) add 44
     * assertions, and a floor left at 80 would have let any one of the three
     * silently stop running. Sections 10-11 (#553) add 66 more (5 of them
     * inside guards that are only skipped when the guarding assertion has
     * already failed), so the floor rises with them -- a dropped call in
     * main() is exactly the false negative this floor exists to catch. */
    if (checks < 192) {
        printf("FAIL: only %d assertions ran — the suite did not execute\n",
               checks);
        return 1;
    }

    printf("\n%d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
