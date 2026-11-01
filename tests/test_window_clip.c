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
#include <errno.h>
#include <poll.h>
#include <signal.h>
#include <unistd.h>
#include <sys/wait.h>

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

/* compute.c's only foreign symbol (summary streaming).
 *
 * It REFUSES by default, which is what keeps every section below on the raw
 * path. Section 12c points it at one synthetic per-second record so the
 * SUMMARY twin of pgwt_compute_time_model can be driven in-process: the
 * >= 120 s unfiltered routing is a different function with its own copy of
 * the CPU-row rule, and a fix applied to only one of the two creates a
 * discontinuity at 120 seconds — the same class of hole #559 hit. */
static const struct pgwt_summary_accum *fake_summary_rec = NULL;

int pgwt_visit_summaries(const char *trace_dir, uint64_t from_wall_ns,
                         uint64_t to_wall_ns, pgwt_summary_visitor visitor,
                         void *ctx)
{
    (void)trace_dir; (void)from_wall_ns; (void)to_wall_ns;
    if (!fake_summary_rec)
        return -1;
    if (visitor(fake_summary_rec, ctx) != 0)
        return -1;
    return 1;
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
    /* The clamp is right, but clamping SILENTLY is not: 15 ms of claimed CPU
     * disappears here. pgwt_compute_time_model counts that excess in
     * cpu_clamped_ms and this response had no equivalent, so the same skew
     * was a reported diagnostic on one response and invisible on the other. */
    CHECK_MS(e.cpu_clamped_ms, 15.0,
             "...and the 15 ms of overshoot is COUNTED, not silently absorbed");
    struct pgwt_tm_result skew_tm;
    pgwt_compute_time_model(&skew, 1, &f, WFROM, WTO, 10000.0, &skew_tm);
    CHECK_MS(e.cpu_clamped_ms, skew_tm.cpu_clamped_ms,
             "...the same number the time model reports under that name");
    free(skew_tm.rows);
    /* Clipped by the window: the measurement is split by the in-window
     * fraction FIRST, so the overshoot is the fraction's overshoot (12.5 ms
     * of CPU over a 5 ms clip), not the whole-record one. A clamp counter
     * written against the unclipped record would read 15.0 here. */
    struct pgwt_events_result e_clip;
    pgwt_compute_top_events(&skew, 1, &f, skew.timestamp_ns - MS_NS(5),
                            skew.timestamp_ns, 5.0, &e_clip);
    CHECK_MS(ev_total_ms(&e_clip, CPU_GAP), 5.0,
             "...half the gap in window clamps to the 5 ms clip");
    CHECK_MS(e_clip.cpu_clamped_ms, 7.5,
             "...and the overshoot is the clipped one (12.5 - 5), not 15.0");
    free(e_clip.rows);
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
     * DB Time, so neither CPU* nor off-CPU may take anything from it.
     *
     * WHAT KEEPS IT OUT is the CPU-CLASS selector, not an idle test. This
     * case used to be labelled as covering a `!pgwt_is_idle_event()` conjunct
     * in compute.c, and a reviewer killed that claim by mutation: deleting
     * the conjunct left every assertion here green, because no idle event is
     * in PGWT_CLASS_CPU, so the conjunct could not change the answer for ANY
     * input. An assertion cannot fix an unfalsifiable claim -- so the dead
     * conjunct is gone from compute.c and the PROPERTY that made it dead is
     * asserted below instead, over the whole idle universe. That one CAN
     * fail: it goes red the day an idle event is put in the CPU class (or
     * event 0 is called idle), which is exactly when the deleted conjunct
     * would have started to matter. */
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

    /* The invariant, swept rather than argued. pgwt_is_idle_event only ever
     * looks at the class byte and (for Timeout) the low event number, so
     * 256 classes x 64 ids covers its whole decision space; the PG18 pacing
     * mask is installed for the sweep so the Timeout pacing ids are in it
     * too (the suite otherwise leaves the mask empty, which would have
     * silently excluded six of the seven visible idle events). */
    uint32_t saved_mask = pgwt_idle_rule_timeout_mask();
    pgwt_idle_rule_set_timeout_mask(PGWT_IDLE_TIMEOUT_MASK_PG18);
    int idle_seen = 0, idle_in_cpu = 0, pacing_seen = 0;
    for (uint32_t cls = 0; cls < 256; cls++) {
        for (uint32_t n = 0; n < 64; n++) {
            uint32_t id = (cls << 24) | n;
            if (!pgwt_is_idle_event(id))
                continue;
            idle_seen++;
            if (cls == CLASS_TIMEOUT)
                pacing_seen++;
            if (pgwt_wait_class_index(id) == PGWT_CLASS_CPU)
                idle_in_cpu++;
        }
    }
    pgwt_idle_rule_set_timeout_mask(saved_mask);
    /* Non-vacuity first: a sweep that matched nothing would report
     * "none of them is in the CPU class" about the empty set. */
    CHECK(idle_seen > 1,
          "the idle sweep found more than one idle event to look at");
    CHECK(pacing_seen == 6,
          "...including all six PG18 Timeout pacing ids (mask 0x29B)");
    CHECK(pgwt_is_idle_event(CLIENT_READ) != 0,
          "...and Client:ClientRead, the one idle event with a visible row");
    CHECK(idle_in_cpu == 0,
          "NO idle event is in PGWT_CLASS_CPU -- which is why the CPU split "
          "needs no idle test of its own");
    CHECK(pgwt_is_idle_event(CPU_GAP) == 0,
          "...and event 0, the ONLY id in that class, is not idle");
    CHECK(pgwt_wait_class_index(CPU_GAP) == PGWT_CLASS_CPU,
          "...so the class the split selects on is reachable at all");

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

    /* 11m. ROUNDING, not truncation. The row accumulator is integer ns and
     * the split is a double, so every straddling record leaves a fractional
     * nanosecond; truncating it biases that remainder into off-CPU on EVERY
     * record. compute.c:1845 rounds for exactly that reason, and a reviewer
     * showed the claim was untested: changing `(uint64_t)(m + 0.5)` to
     * `(uint64_t)m` left the whole suite green, because every earlier fixture
     * splits to a whole nanosecond.
     *
     * The fixture is built so the two differ by HALF a nanosecond per record,
     * and then AMPLIFIED by 2000 records -- not because 1 ns is acceptable,
     * but because CHECK_MS's tolerance IS 1e-6 ms (one nanosecond), so a
     * one-record difference sits exactly on its boundary and would pass. 2000
     * records put the difference at 0.002 ms, two thousand times the
     * tolerance, which is a gate rather than a coin flip.
     *
     * Per record: 2000 ns duration, 1000 ns of it in window (frac 0.5),
     * cpu_ns 501 -> m = 250.5 ns. Rounded 251, truncated 250.
     *   CPU*    = 2000 * 251 ns  = 0.502 ms   (truncation: 0.500 ms)
     *   off-CPU = 2000 * 749 ns  = 1.498 ms   (truncation: 1.500 ms)
     *   DB Time = 2000 * 1000 ns = 2.000 ms   (unchanged either way) */
    static struct pgwt_trace_event frac[2000];
    for (int i = 0; i < 2000; i++)
        frac[i] = mk(1, CPU_GAP, WFROM + 1000, 2000, 44, 501);
    struct pgwt_events_result r;
    pgwt_compute_top_events(frac, 2000, &f, WFROM, WTO, 10000.0, &r);
    CHECK_MS(r.db_time_ms, 2.0,
             "2000 half-in-window gaps: DB Time is the clipped wall, 2.000 ms");
    CHECK_MS(ev_total_ms(&r, CPU_GAP), 0.502,
             "250.5 ns of measured CPU per record ROUNDS up to 251 ns "
             "(truncation would read 0.500 ms)");
    CHECK_MS(r.offcpu_ms, 1.498,
             "...and the half-nanosecond is not biased into off-CPU "
             "(truncation would read 1.500 ms)");
    CHECK(r.has_measured_cpu != 0,
          "...and this is a measurement, not the unsplit wall (which would "
          "also read 2.000 ms on the CPU row)");
    /* No "CPU* + off-CPU == DB Time" line here on purpose: off-CPU is
     * computed AS clip_u - row_ns, so that identity cannot fail (section
     * 10's header says the same about the four-term version). Each number
     * above is asserted against its own hand-computed literal instead. */
    free(r.rows);
}

/* ── Integration note (agent/integration-20261025b) ─────────────────────
 *
 * The sections that follow arrived from agent/laneC-raw-compute, which
 * numbered them 10-13 against the same base that #553's sections above
 * numbered 10-11. BOTH series are kept verbatim rather than renumbered:
 * every "10b", "12a", "13d" label also appears inside ~100 assertion
 * MESSAGE strings, and rewriting those as part of a merge resolution is
 * exactly how a dropped assertion hides inside textual churn. So
 * "section 10" is ambiguous in this file — #553's is above, #461's is
 * below — and main()'s assertion floor is the MEASURED union of both
 * arms (288; see the note on the floor in main()).
 */

/* ── 10. #461: pgwt_compute_transitions must TERMINATE on a full table ──
 *
 * `while (ht[h].count > 0 && (from/to mismatch)) h = (h+1) & TRANS_HT_MASK;`
 * had no fullness test. Once 4096 distinct (from, to) pairs occupy the
 * 4096-slot table, the 4097th distinct pair finds no free slot and the loop
 * runs forever: pgwt-server is single-threaded, so the request never
 * answers and every tab stops at once. Same shape as #327.
 *
 * A HANG IS NOT A FAILING TEST, so this section runs the reducer in a
 * forked child behind a watchdog: no answer within the deadline is a FAIL
 * with a named reason, never a stalled suite. The watchdog is itself the
 * thing most likely to make this gate unreachable — one that never fires,
 * or that reports "ok" for a child that produced nothing, would approve the
 * hang it exists to catch — so 10b/10c/10d exercise its three outcomes
 * directly before 10e uses it.
 *
 * Timing: the only clock here is the watchdog deadline. The real fixture is
 * 4097 in-memory records and returns in milliseconds against a 20 s
 * deadline (measured 0.004 s wall for the whole suite); the hang it detects
 * is INFINITE, not slow, so no runner load can turn a pass into a failure
 * or the reverse. The spin control uses a 1.5 s deadline for the same
 * reason — it can never finish.
 */

/* Distinct pairs in the fixture: one more than TRANS_HT_SIZE (4096), which
 * is the first count the old probe loop could not place. */
#define TRANS_FULL_PAIRS 4097
/* Below the table size: the SAME fixture shape, provably admitted by
 * pgwt_transition_record_counts, which is what keeps 10e from passing
 * vacuously on a fixture the filter throws away. */
#define TRANS_SAFE_PAIRS 4000

struct trans_probe {
    int32_t  num_rows;
    int32_t  total_rows;
    uint64_t total_transitions;
};

enum { CHILD_OK = 0, CHILD_TIMEOUT = 1, CHILD_NO_RESULT = 2, CHILD_HARNESS = 3 };

static const char *child_rc_name(int rc)
{
    switch (rc) {
    case CHILD_OK:        return "returned a result";
    case CHILD_TIMEOUT:   return "DID NOT RETURN (watchdog fired)";
    case CHILD_NO_RESULT: return "exited without a result";
    default:              return "harness error (fork/pipe failed)";
    }
}

/* Run `body` in a child with a deadline. CHILD_OK only when the child wrote
 * the whole result: a child that dies, or writes a short record, is
 * CHILD_NO_RESULT, never a silent zero-filled "pass". */
static int run_child(void (*body)(int wfd), int timeout_ms,
                     void *out, size_t len)
{
    int fds[2];
    memset(out, 0xA5, len);     /* poison: an unwritten result cannot read 0 */
    if (pipe(fds) != 0)
        return CHILD_HARNESS;
    fflush(NULL);               /* no duplicated stdio buffer in the child */
    pid_t pid = fork();
    if (pid < 0) { close(fds[0]); close(fds[1]); return CHILD_HARNESS; }
    if (pid == 0) {
        close(fds[0]);
        body(fds[1]);
        _exit(0);
    }
    close(fds[1]);

    struct pollfd pf;
    pf.fd = fds[0];
    pf.events = POLLIN;
    pf.revents = 0;
    int pr;
    do { pr = poll(&pf, 1, timeout_ms); } while (pr < 0 && errno == EINTR);

    int rc;
    if (pr == 0) {
        kill(pid, SIGKILL);
        rc = CHILD_TIMEOUT;
    } else if (pr < 0) {
        kill(pid, SIGKILL);
        rc = CHILD_HARNESS;
    } else {
        size_t got = 0;
        while (got < len) {
            ssize_t k = read(fds[0], (char *)out + got, len - got);
            if (k <= 0) break;
            got += (size_t)k;
        }
        rc = (got == len) ? CHILD_OK : CHILD_NO_RESULT;
    }
    close(fds[0]);
    int st;
    while (waitpid(pid, &st, 0) < 0 && errno == EINTR)
        ;
    return rc;
}

/* The fixture: `pairs` records, each a DISTINCT (from, to). Both ids are
 * ordinary wait events (IO / Lock), so every record passes
 * pgwt_transition_record_counts — asserted, not assumed, by 10e's
 * below-the-bound twin. */
static void trans_pairs_child(int wfd, int pairs)
{
    struct pgwt_trace_event *evs = calloc((size_t)pairs, sizeof(*evs));
    if (!evs)
        _exit(2);               /* CHILD_NO_RESULT, never a zeroed pass */
    for (int i = 0; i < pairs; i++) {
        evs[i].pid          = 4242;
        evs[i].old_event    = EV(CLASS_IO,   (uint32_t)(i >> 7));
        evs[i].new_event    = EV(CLASS_LOCK, (uint32_t)(i & 127));
        evs[i].timestamp_ns = WFROM + (uint64_t)i;
        evs[i].duration_ns  = 1000;
        evs[i].cpu_ns       = PGWT_CPU_NS_UNKNOWN;
    }
    struct pgwt_filter f;
    memset(&f, 0, sizeof(f));
    struct pgwt_transitions_result tr;
    pgwt_compute_transitions(evs, pairs, &f, 50, &tr);

    struct trans_probe r;
    r.num_rows          = tr.num_rows;
    r.total_rows        = tr.total_rows;
    r.total_transitions = tr.total_transitions;
    free(tr.rows);
    free(evs);
    if (write(wfd, &r, sizeof(r)) != (ssize_t)sizeof(r))
        _exit(3);
}

static void child_full_table(int wfd) { trans_pairs_child(wfd, TRANS_FULL_PAIRS); }
static void child_safe_table(int wfd) { trans_pairs_child(wfd, TRANS_SAFE_PAIRS); }

/* Watchdog controls. `volatile` so the spin cannot be optimised away. */
static volatile unsigned long spin_sink;
static void child_spin_forever(int wfd) { (void)wfd; for (;;) spin_sink++; }
static void child_prompt_result(int wfd)
{
    struct trans_probe r = { 11, 22, 33 };
    if (write(wfd, &r, sizeof(r)) != (ssize_t)sizeof(r)) _exit(3);
}
static void child_exit_silently(int wfd) { (void)wfd; _exit(9); }
static void child_short_write(int wfd)
{
    char half[sizeof(struct trans_probe) / 2] = {0};
    if (write(wfd, half, sizeof(half)) != (ssize_t)sizeof(half)) _exit(3);
}

static void test_transitions_table_full(void)
{
    printf("--- 10. #461: transitions probe loop terminates on a full table ---\n");

    struct trans_probe r;

    /* 10b. The watchdog FIRES on a child that never finishes. Without this,
     * a watchdog wired to a deadline it can never reach would report every
     * hang as a pass — the exact false negative this gate exists to avoid. */
    int rc = run_child(child_spin_forever, 1500, &r, sizeof(r));
    CHECK(rc == CHILD_TIMEOUT,
          "10b control: the watchdog FIRES on a child that never returns");

    /* 10c. …and does NOT fire on one that answers promptly. */
    rc = run_child(child_prompt_result, 20000, &r, sizeof(r));
    CHECK(rc == CHILD_OK && r.num_rows == 11 && r.total_rows == 22 &&
          r.total_transitions == 33,
          "10c control: a prompt child is CHILD_OK and its result arrives "
          "intact");

    /* 10d. A child that produces NOTHING is not a pass. Two shapes: a silent
     * exit, and a truncated record. Both must be CHILD_NO_RESULT — a gate
     * that reads an unwritten buffer as zeros approves absence. */
    rc = run_child(child_exit_silently, 20000, &r, sizeof(r));
    CHECK(rc == CHILD_NO_RESULT,
          "10d control: a child that exits without writing is NO_RESULT, not OK");
    rc = run_child(child_short_write, 20000, &r, sizeof(r));
    CHECK(rc == CHILD_NO_RESULT,
          "10d control: a child that writes a PARTIAL result is NO_RESULT");

    /* 10e-pre. The fixture shape is admitted by the transition filter. This
     * runs BELOW the table bound, so it passes on the broken tree too — it
     * is the control that says 10e's failure is about FULLNESS and not
     * about a fixture the reducer throws away. */
    rc = run_child(child_safe_table, 20000, &r, sizeof(r));
    CHECK(rc == CHILD_OK,
          "10e-pre: 4000 distinct pairs (below the 4096 bound) return");
    if (rc == CHILD_OK) {
        CHECK_U64(r.total_rows, TRANS_SAFE_PAIRS,
                  "…and every one of them is a row (the filter admits this "
                  "fixture)");
        CHECK_U64(r.total_transitions, TRANS_SAFE_PAIRS,
                  "…and every record is counted");
    }

    /* 10e. THE DEFECT. 4097 distinct pairs — one past the 4096-slot table. */
    rc = run_child(child_full_table, 20000, &r, sizeof(r));
    if (rc != CHILD_OK)
        printf("  (child outcome: %s)\n", child_rc_name(rc));
    CHECK(rc == CHILD_OK,
          "10e: 4097 distinct (from,to) pairs — one more than the 4096-slot "
          "table — and pgwt_compute_transitions RETURNS");
    if (rc == CHILD_OK) {
        CHECK_U64(r.total_rows, TRANS_FULL_PAIRS,
                  "…with every distinct pair present (no silently dropped "
                  "edge: a graph quietly missing links is the other way this "
                  "could be 'fixed')");
        CHECK_U64(r.total_transitions, TRANS_FULL_PAIRS,
                  "…and every record counted");
        CHECK_U64(r.num_rows, 50,
                  "…and max_rows still caps the returned rows at 50");
    } else {
        /* Keep the assertion count stable whichever way 10e goes, so the
         * floor below cannot be satisfied by a shorter run. */
        CHECK(0, "…total_rows unavailable: the reducer did not return");
        CHECK(0, "…total_transitions unavailable: the reducer did not return");
        CHECK(0, "…num_rows unavailable: the reducer did not return");
    }
}

/* 10f. #461's refusal path. Growing the table removed the hang, but the
 * remaining exit -- a real allocation failure -- must REFUSE the whole
 * graph, never return the edges it managed to place. A transitions graph
 * missing edges reads as "that transition never happened", which is a wrong
 * answer rather than a smaller one.
 *
 * Both allocators are injected separately, because a refusal wired to only
 * one of them approves the other: the accumulator array (accum_grow,
 * "transition_entries") and the pair index (qid_index.h, "qid_index_grow").
 * The clean run on the SAME fixture is the control that says the refusal is
 * caused by the injection and not by the fixture. */
static void trans_refusal_case(const char *point, const char *label)
{
    struct pgwt_trace_event evs[4];
    memset(evs, 0, sizeof(evs));
    for (int i = 0; i < 4; i++) {
        evs[i].pid          = 7;
        evs[i].old_event    = EV(CLASS_IO, (uint32_t)i);
        evs[i].new_event    = EV(CLASS_LOCK, (uint32_t)i);
        evs[i].timestamp_ns = WFROM + (uint64_t)i;
        evs[i].duration_ns  = 1000;
        evs[i].cpu_ns       = PGWT_CPU_NS_UNKNOWN;
    }
    struct pgwt_filter f;
    memset(&f, 0, sizeof(f));

    if (point) setenv("PGWT_TEST_ALLOC_FAIL", point, 1);
    else       unsetenv("PGWT_TEST_ALLOC_FAIL");

    struct pgwt_transitions_result tr;
    pgwt_compute_transitions(evs, 4, &f, 50, &tr);

    if (point) {
        CHECK(tr.failed == 1 && tr.num_rows == 0 && tr.total_rows == 0 &&
              tr.total_transitions == 0 && tr.rows == NULL,
              label);
    } else {
        CHECK(tr.failed == 0 && tr.num_rows == 4 && tr.total_rows == 4 &&
              tr.total_transitions == 4, label);
    }
    free(tr.rows);
    unsetenv("PGWT_TEST_ALLOC_FAIL");
}

static void test_transitions_alloc_refusal(void)
{
    printf("--- 10f. #461: an allocation failure refuses the whole graph ---\n");
    trans_refusal_case(NULL, "10f control: the fixture itself computes four "
                             "pairs cleanly (no injection)");
    trans_refusal_case("transition_entries",
                       "10f: accumulator growth fails -> failed=1 and NOTHING "
                       "is returned (never a partial graph)");
    trans_refusal_case("qid_index_grow",
                       "10f: pair-index growth fails -> failed=1 and NOTHING "
                       "is returned");
    trans_refusal_case(NULL, "10f control: the refusal does not latch -- the "
                             "same fixture computes cleanly again afterwards");
}

/* ── 11. #374: the per-pid TAGGING table must not have a fixed bound ──
 *
 * pgwt_tag_events' `tag_pid_state ht[TAG_HT_SIZE]` was the LAST fixed-size
 * pid table (#327 grew the other five). Past 4096 distinct pids in one
 * loaded window, tag_pid_get() ran out of slots and the pass refused the
 * whole array — correct as a fail-safe (#363), but the array IS the oracle
 * every raw estimator reads, so the refusal becomes a TOTAL OUTAGE: every
 * request on that window answers `code: "allocation_failed"`, "memory
 * allocation failed", hint "retry the request". Nothing was allocated and
 * retrying never helps. Distinct pids per window are driven by connection
 * CHURN, not concurrency, so 4097 is ordinary on a long capture — and
 * `pgwt-server --dump` loads the WHOLE trace, where it is close to certain.
 *
 * Nothing here is timed, ordered against anything, or shared: one in-memory
 * array, one call.
 */

#define TAG_OVER_PIDS  5000     /* > the old 4096-slot table */
#define TAG_UNDER_PIDS 4000     /* < it: the same shape, admitted either way */

/* One IO wait per pid, pids 10000.. ascending. Each pid is declared either
 * BACKGROUND or MAINT by index parity — two DIFFERENT non-zero flags, so the
 * probe below is a positive assertion on both records it reads.
 *
 * That shape is deliberate. An earlier draft marked odd pids with flag 0 and
 * happened to probe an odd pid, so "the record carries its pid's flag" read
 * `(flags & BACKGROUND) == 0` — true for an untagged record too, and true on
 * the broken tree. Expecting ZERO proves nothing about whether the pass ran;
 * the possibility is removed here rather than asserted around. */
static int tag_scale_case(int n_pids, uint32_t *last_flags, uint32_t *prev_flags)
{
    struct pgwt_trace_event *evs = calloc((size_t)n_pids, sizeof(*evs));
    struct pgwt_pid_cat *cats = calloc((size_t)n_pids, sizeof(*cats));
    if (!evs || !cats) { free(evs); free(cats); return -1; }

    for (int i = 0; i < n_pids; i++) {
        uint32_t pid = 10000u + (uint32_t)i;
        evs[i].pid          = pid;
        evs[i].old_event    = IO_READ;
        evs[i].new_event    = CPU_GAP;
        evs[i].timestamp_ns = WFROM + S(1) + (uint64_t)i;
        evs[i].duration_ns  = 1000;
        evs[i].cpu_ns       = PGWT_CPU_NS_UNKNOWN;
        /* pid_cat_lookup binary-searches, so cats must be pid-ascending. */
        cats[i].pid  = pid;
        cats[i].flag = (i % 2 == 0) ? PGWT_EVENT_FLAG_BACKGROUND
                                    : PGWT_EVENT_FLAG_MAINT;
    }

    int ok = pgwt_tag_events(evs, n_pids, cats, n_pids);
    /* The last two records: past the old bound, and the ones a pass that
     * stopped early would never reach. Their expected flags DIFFER, so a
     * blanket stamp fails one of them. */
    *prev_flags = evs[n_pids - 2].flags;
    *last_flags = evs[n_pids - 1].flags;
    free(evs);
    free(cats);
    return ok;
}

/* n_pids is even in both cases below, so index n-2 is a BACKGROUND pid and
 * index n-1 a MAINT one. Both expectations are non-zero. */
static int tag_probe_ok(uint32_t last_flags, uint32_t prev_flags)
{
    return (prev_flags & PGWT_EVENT_FLAG_BACKGROUND) != 0 &&
           (prev_flags & PGWT_EVENT_FLAG_MAINT) == 0 &&
           (last_flags & PGWT_EVENT_FLAG_MAINT) != 0 &&
           (last_flags & PGWT_EVENT_FLAG_BACKGROUND) == 0;
}

static void test_tag_table_scale(void)
{
    printf("--- 11. #374: the tagging pid table is not fixed at 4096 ---\n");

    uint32_t last = 0, prev = 0;

    /* 11a control, below the old bound: passes on the broken tree too. It is
     * what says 11b's failure is about the BOUND and not about a fixture
     * pgwt_tag_events would reject for some other reason. */
    int ok = tag_scale_case(TAG_UNDER_PIDS, &last, &prev);
    CHECK(ok == 1, "11a control: 4000 distinct pids tag successfully");
    CHECK(tag_probe_ok(last, prev),
          "11a control: …and the last two records carry their OWN pids' "
          "distinct category flags (the pass is neither a no-op that returns "
          "1 nor a blanket stamp)");

    /* 11b. THE DEFECT: one more distinct pid than the fixed table held is a
     * total refusal of every raw request on the window. */
    last = prev = 0;
    ok = tag_scale_case(TAG_OVER_PIDS, &last, &prev);
    CHECK(ok == 1,
          "11b: 5000 distinct pids (past the old 4096-slot table) still tag "
          "successfully — connection churn is not an allocation failure");
    CHECK(tag_probe_ok(last, prev),
          "11b: …and the records BEYOND the old bound carry their own flags, "
          "so the pass grew the table rather than quietly skipping the "
          "surplus pids");

    /* 11c. The refusal that must SURVIVE: a real allocation failure still
     * refuses the whole array (#363). Three injection points — the historic
     * one and both allocators the growable table uses — because a refusal
     * wired to only one of them approves the others. */
    struct pgwt_trace_event one[1];
    struct pgwt_pid_cat cat1[1];
    const char *pts[3] = { "tag_ht", "tag_pid_entries", "pid_index_grow" };
    const char *envs[3] = { "PGWT_TEST_LOAD_ALLOC_FAIL",
                            "PGWT_TEST_ALLOC_FAIL", "PGWT_TEST_ALLOC_FAIL" };
    for (int k = 0; k < 3; k++) {
        memset(one, 0, sizeof(one));
        one[0].pid = 777; one[0].old_event = IO_READ;
        one[0].timestamp_ns = WFROM + S(1); one[0].duration_ns = 1000;
        one[0].cpu_ns = PGWT_CPU_NS_UNKNOWN;
        cat1[0].pid = 777; cat1[0].flag = PGWT_EVENT_FLAG_BACKGROUND;
        setenv(envs[k], pts[k], 1);
        int r = pgwt_tag_events(one, 1, cat1, 1);
        unsetenv(envs[k]);
        char msg[160];
        snprintf(msg, sizeof(msg),
                 "11c: a REAL allocation failure at \"%s\" still refuses the "
                 "whole array", pts[k]);
        CHECK(r == 0, msg);
    }

    /* …and the refusal does not latch: the same call succeeds once the
     * injection is gone. Without this, 11c would also pass if the function
     * had simply started refusing everything. */
    memset(one, 0, sizeof(one));
    one[0].pid = 777; one[0].old_event = IO_READ;
    one[0].timestamp_ns = WFROM + S(1); one[0].duration_ns = 1000;
    one[0].cpu_ns = PGWT_CPU_NS_UNKNOWN;
    cat1[0].pid = 777; cat1[0].flag = PGWT_EVENT_FLAG_BACKGROUND;
    int r = pgwt_tag_events(one, 1, cat1, 1);
    CHECK(r == 1 && (one[0].flags & PGWT_EVENT_FLAG_BACKGROUND) != 0,
          "11c control: with no injection the same call succeeds and tags");

    /* An empty array is a complete success, not a refusal (compute.h). */
    CHECK(pgwt_tag_events(NULL, 0, NULL, 0) == 1,
          "11d: an empty window is a complete tagging result, not a failure");
}

/* ── 12. #575: time_model must not hide a non-zero off-CPU sibling ──
 *
 * Both CPU rows sat INSIDE the `classes[c].total_ns <= 0 -> continue` guard.
 * An all-runqueue gap — a backend that was ready and never scheduled, which
 * is exactly the condition this tool exists to show — measures cpu_ns == 0
 * over a non-zero wall, so the CPU class total is 0, the guard skips the
 * class, and BOTH CPU rows disappear. The time is still inside DB Time, so
 * the Time Model shows a DB Time it does not account for and the reader
 * concludes there was no CPU activity.
 *
 * TWO DEFECTS, not one, and 12b is the second:
 *   (1) the row guard above, which is in the raw path AND in its summary
 *       twin (pgwt_compute_time_model_from_summaries), so fixing one alone
 *       would make the Time Model disagree with itself across the 120 s
 *       summary-routing threshold. 12a and 12c pin both.
 *   (2) has_measured_cpu was also raised by MEASURED SPIN INSIDE A WAIT
 *       (the else branch's wait_gap_cpu_ns), which is a different question
 *       from "is this response's CPU row a measurement". With an UNKNOWN CPU
 *       gap next to such a wait, time_model labelled an UNMEASURED wall
 *       "CPU (running)" and printed a 0.0 ms runqueue sibling beside it.
 *       Different cause, different line, so: a second bug. The summary
 *       twin's flag was already the narrow one (cpu_measured_records > 0),
 *       which is also the evidence for which of the two predicates is
 *       intended.
 *
 * Nothing here is timed or ordered: in-memory arrays and, for 12c, one
 * synthetic summary record fed through the stub above.
 */

static double tm_row_ms(const struct pgwt_tm_result *tm, const char *name)
{
    return tm_class_ms(tm, name);      /* -1.0 == the row is ABSENT */
}

static int tm_row_count(const struct pgwt_tm_result *tm, const char *name)
{
    int n = 0;
    for (int i = 0; i < tm->num_rows; i++)
        if (tm->rows[i].indent == 1 && strcmp(tm->rows[i].name, name) == 0)
            n++;
    return n;
}

static void test_time_model_offcpu_sibling(void)
{
    printf("--- 12. #575: an all-runqueue gap must still report both CPU rows ---\n");

    struct pgwt_filter f;
    memset(&f, 0, sizeof(f));

    /* 12a. One 10 ms on-CPU gap, MEASURED as 0 ns on a core. */
    {
        struct pgwt_trace_event evs[1];
        evs[0] = mk(900, CPU_GAP, WFROM + S(1), 10000000ULL, 0, 0 /* measured 0 */);
        struct pgwt_tm_result tm;
        pgwt_compute_time_model(evs, 1, &f, WFROM, WTO, 10000.0, &tm);

        CHECK_MS(tm.db_time_ms, 10.0, "12a: the gap is 10 ms of DB Time");
        CHECK(tm.has_measured_cpu == 1,
              "12a: the window HAS a measured CPU row (a measured zero is a "
              "signal, not an absence)");
        CHECK_MS(tm.offcpu_ms, 10.0,
                 "12a: …and the scalar off-CPU total is the whole gap");
        CHECK_MS(tm_row_ms(&tm, "CPU (running)"), 0.0,
                 "12a: the 'CPU (running)' ROW is present and reads 0.0 ms — "
                 "nothing ran, measured");
        CHECK_MS(tm_row_ms(&tm, "CPU (waiting for a core)"), 10.0,
                 "12a: and its sibling ROW reports the whole 10 ms "
                 "(this is the defect: both rows used to vanish)");
        CHECK(tm_row_count(&tm, "CPU*") == 0,
              "12a: …and the un-split 'CPU*' label is NOT used, because this "
              "window can separate running from waiting");
        free(tm.rows);
    }

    /* 12b. An UNKNOWN gap plus a wait carrying measured spin. The gap's wall
     * is NOT a measurement of on-CPU time and must not be labelled one. */
    {
        struct pgwt_trace_event evs[2];
        evs[0] = mk(901, CPU_GAP, WFROM + S(1), 10000000ULL, 0,
                    PGWT_CPU_NS_UNKNOWN);
        evs[1] = mk(901, LOCK_RELATION, WFROM + S(2), 5000000ULL, 0,
                    1000000ULL /* 1 ms of spin inside the wait */);
        struct pgwt_tm_result tm;
        pgwt_compute_time_model(evs, 2, &f, WFROM, WTO, 10000.0, &tm);

        CHECK_MS(tm.db_time_ms, 15.0, "12b: DB Time is 10 + 5 ms");
        CHECK(tm.has_measured_cpu == 0,
              "12b: has_measured_cpu is 0 — spin measured INSIDE A WAIT is "
              "not a measurement of THIS response's CPU row");
        CHECK_MS(tm_row_ms(&tm, "CPU*"), 10.0,
                 "12b: the gap is the un-split 'CPU*' row by gap-inference");
        CHECK(tm_row_ms(&tm, "CPU (running)") < 0.0,
              "12b: …NOT labelled 'CPU (running)', which claimed an "
              "unmeasured wall was measured on-CPU time");
        CHECK(tm_row_ms(&tm, "CPU (waiting for a core)") < 0.0,
              "12b: …and there is NO 0.0 ms runqueue sibling: off-CPU time "
              "here is unavailable, which is not the same as zero");
        CHECK_MS(tm.wait_gap_cpu_ms, 1.0,
                 "12b: the wait's spin is still reported as the observability "
                 "sum it always was — narrowing the flag loses no number");
        free(tm.rows);
    }

    /* 12c. The SUMMARY twin, same all-runqueue second, through the stub. The
     * routing threshold is 120 s, so these two functions answer the same
     * question on either side of it and must not disagree. */
    {
        struct pgwt_summary_accum rec;
        memset(&rec, 0, sizeof(rec));
        rec.second_wall_ns = WFROM + S(1);
        rec.total_events = 1;
        rec.class_ns[PGWT_CLASS_CPU] = 10000000ULL;   /* 10 ms of DB Time */
        rec.cpu_measured_ns = 0;                      /* measured: none on a core */
        rec.cpu_unknown_ns = 0;
        rec.cpu_measured_records = 1;                 /* …but it WAS measured */
        fake_summary_rec = &rec;
        struct pgwt_tm_result tm;
        pgwt_compute_time_model_from_summaries("/nonexistent", WFROM, WTO,
                                               &f, 10000.0, &tm);
        fake_summary_rec = NULL;

        CHECK_MS(tm.db_time_ms, 10.0, "12c: the summary twin sees 10 ms of DB Time");
        CHECK(tm.has_measured_cpu == 1, "12c: …and a measured CPU row");
        CHECK_MS(tm_row_ms(&tm, "CPU (running)"), 0.0,
                 "12c: the summary twin ALSO emits 'CPU (running)' 0.0 ms");
        CHECK_MS(tm_row_ms(&tm, "CPU (waiting for a core)"), 10.0,
                 "12c: …and its 10 ms sibling — identical to 12a, so the "
                 "Time Model does not change answer at the 120 s threshold");
        free(tm.rows);
    }

    /* 12d controls. Each removes a way the fix could be "always print the
     * CPU rows", which would be a different wrong answer. */
    {
        /* A MEASURED, non-zero gap: 10 ms wall, 4 ms on a core. Unchanged. */
        struct pgwt_trace_event evs[1];
        evs[0] = mk(902, CPU_GAP, WFROM + S(1), 10000000ULL, 0, 4000000ULL);
        struct pgwt_tm_result tm;
        pgwt_compute_time_model(evs, 1, &f, WFROM, WTO, 10000.0, &tm);
        CHECK_MS(tm_row_ms(&tm, "CPU (running)"), 4.0,
                 "12d control: a measured non-zero gap still reports the "
                 "MEASURED 4 ms on the running row");
        CHECK_MS(tm_row_ms(&tm, "CPU (waiting for a core)"), 6.0,
                 "12d control: …and 6 ms on the runqueue row");
        free(tm.rows);
    }
    {
        /* UNKNOWN only, no wait spin anywhere: one combined row, no sibling. */
        struct pgwt_trace_event evs[1];
        evs[0] = mk(903, CPU_GAP, WFROM + S(1), 10000000ULL, 0,
                    PGWT_CPU_NS_UNKNOWN);
        struct pgwt_tm_result tm;
        pgwt_compute_time_model(evs, 1, &f, WFROM, WTO, 10000.0, &tm);
        CHECK_MS(tm_row_ms(&tm, "CPU*"), 10.0,
                 "12d control: an UNKNOWN-only window keeps ONE combined "
                 "'CPU*' row");
        CHECK(tm_row_ms(&tm, "CPU (waiting for a core)") < 0.0,
              "12d control: …with no runqueue sibling (unavailable is not 0)");
        free(tm.rows);
    }
    {
        /* NO CPU record at all: neither CPU row may be invented. */
        struct pgwt_trace_event evs[1];
        evs[0] = mk(904, IO_READ, WFROM + S(1), 10000000ULL, 0,
                    PGWT_CPU_NS_UNKNOWN);
        struct pgwt_tm_result tm;
        pgwt_compute_time_model(evs, 1, &f, WFROM, WTO, 10000.0, &tm);
        CHECK(tm_row_ms(&tm, "CPU (running)") < 0.0 &&
              tm_row_ms(&tm, "CPU*") < 0.0 &&
              tm_row_ms(&tm, "CPU (waiting for a core)") < 0.0,
              "12d control: a window with NO on-CPU gap reports NO CPU row "
              "of any kind — the fix must not conjure a 0.0/0.0 pair");
        free(tm.rows);
    }
}

/* ── 13. #562: Top Sessions' CPU column is MEASURED on-CPU time ──
 *
 * `if (ev->old_event == 0) ht[h].cpu_ns += clip_u;` added the window-clipped
 * WALL of each on-CPU gap where the MEASURED cpu_ns belongs — the sibling of
 * #553 (top_events, PR #559) and #531 (CLI replay, PR #554), on the function
 * #553's issue text actually quoted. For a WAIT row wall IS the wait; the gap
 * between two waits is "on a core" only for the part sum_exec_runtime
 * actually advanced, and the rest is runqueue time.
 *
 * AN AGREEMENT OR SUMS-TO-100% CHECK CANNOT SEE THIS, and that is not a
 * guess: on the broken tree `wait_pct = 100 - cpu_pct` with cpu_pct taken
 * from the gap wall is ALGEBRAICALLY equal to Σwaits/db, because db is
 * exactly gaps + waits. So the two columns summed to a flat 100.0% while the
 * CPU column overstated on-CPU time by 150% (10.0 ms where 4.0 was right).
 * Every assertion below is therefore against its OWN literal, and the
 * conservation check names each of its three terms separately — the sum
 * alone is vacuous, since off-CPU is wall − measured by construction.
 *
 * The contract is #559's, field for field, not a parallel one: measured
 * on-CPU ns on the row, an offcpu_ms remainder beside it, a response-level
 * has_measured_cpu saying which quantity the CPU column IS, and
 * cpu_clamped_ms as the skew self-check. wait_pct additionally moves to the
 * live semantics Σwaits/db, which is forced BY the CPU fix: once cpu_pct is
 * the measurement, 100 − cpu_pct is no longer the wait share.
 *
 * 13d pins the v9 SUMMARY twin on the same 15 ms fixture as 13a: a 10 ms
 * gap measured at 4 ms on a core, plus a 5 ms wait. CPU and off-CPU must each
 * match raw, independently, while wait_pct still uses the full gap wall.
 * 13e is its UNKNOWN-only control: without an observation the summary must
 * keep the gap wall unsplit and refuse to label it a measurement.
 *
 * Nothing here is timed: in-memory arrays, and synthetic summary records
 * through the stub at the top of this file.
 */

static const struct pgwt_session_row *sess_row(const struct pgwt_sessions_result *r,
                                               uint32_t pid)
{
    for (int i = 0; i < r->num_rows; i++)
        if (r->rows[i].pid == pid) return &r->rows[i];
    return NULL;
}

static void test_sessions_measured_cpu(void)
{
    printf("--- 13. #562: Top Sessions' CPU column is measured, not wall ---\n");

    struct pgwt_filter f;
    memset(&f, 0, sizeof(f));

    /* 13a. pid 100: a 10 ms gap MEASURED at 4 ms on a core, plus a 5 ms Lock
     * wait. db = 15 ms; CPU 4 ms (26.666…%), waits 5 ms (33.333…%), off-CPU
     * 6 ms (the remainder). On the broken tree: CPU 10 ms / 66.667%. */
    {
        struct pgwt_trace_event evs[2];
        evs[0] = mk(100, CPU_GAP, WFROM + S(1), 10000000ULL, 0, 4000000ULL);
        evs[1] = mk(100, LOCK_RELATION, WFROM + S(2), 5000000ULL, 0,
                    PGWT_CPU_NS_UNKNOWN);
        struct pgwt_sessions_result ts;
        pgwt_compute_top_sessions(evs, 2, &f, WFROM, WTO, 10000.0, &ts);

        const struct pgwt_session_row *r = sess_row(&ts, 100);
        CHECK(r != NULL, "13a: pid 100 has a row");
        CHECK(ts.has_measured_cpu == 1,
              "13a: the response says its CPU column IS a measurement");
        if (r) {
            CHECK_MS(r->db_time_ms, 15.0,
                     "13a: DB Time is the full 15 ms of wall (unchanged)");
            CHECK_NUM(r->cpu_pct, 4.0 / 15.0 * 100.0,
                      "13a: cpu_pct is the MEASURED 4 ms over DB Time "
                      "(26.667%), not the gap's 10 ms wall (66.667%)");
            CHECK_NUM(r->wait_pct, 5.0 / 15.0 * 100.0,
                      "13a: wait_pct is Σwaits/db — the live semantics — "
                      "not 100 − cpu_pct, which would now read 73.333%");
            CHECK_MS(r->offcpu_ms, 6.0,
                     "13a: and the 6 ms the CPU row no longer claims is "
                     "REPORTED as the off-CPU remainder, not dropped");
            /* Conservation, each term against its own literal. */
            double cpu_ms = r->cpu_pct / 100.0 * r->db_time_ms;
            double wait_ms = r->wait_pct / 100.0 * r->db_time_ms;
            CHECK_MS(cpu_ms, 4.0, "13a conservation: the CPU term is 4 ms");
            CHECK_MS(wait_ms, 5.0, "13a conservation: the wait term is 5 ms");
            CHECK_MS(r->offcpu_ms, 6.0,
                     "13a conservation: the off-CPU term is 6 ms");
            CHECK_MS(cpu_ms + wait_ms + r->offcpu_ms, 15.0,
                     "13a conservation: …and the three add up to DB Time "
                     "(the sum ALONE would be vacuous — off-CPU is wall "
                     "minus measured by construction)");
        }
        CHECK_MS(ts.cpu_clamped_ms, 0.0,
                 "13a: nothing was clamped (the measurement fits its wall)");
        free(ts.rows);
    }

    /* 13b. UNKNOWN only (sampled / v2 / no CPU capability): gap-inference is
     * kept — the whole gap wall is the CPU column — and the response says so
     * rather than claiming a measured zero remainder. */
    {
        struct pgwt_trace_event evs[2];
        evs[0] = mk(200, CPU_GAP, WFROM + S(1), 10000000ULL, 0,
                    PGWT_CPU_NS_UNKNOWN);
        evs[1] = mk(200, LOCK_RELATION, WFROM + S(2), 10000000ULL, 0,
                    PGWT_CPU_NS_UNKNOWN);
        struct pgwt_sessions_result ts;
        pgwt_compute_top_sessions(evs, 2, &f, WFROM, WTO, 10000.0, &ts);
        const struct pgwt_session_row *r = sess_row(&ts, 200);
        CHECK(ts.has_measured_cpu == 0,
              "13b: an UNKNOWN-only window does NOT claim a measurement");
        CHECK(r != NULL, "13b: pid 200 has a row");
        if (r) {
            CHECK_NUM(r->cpu_pct, 50.0,
                      "13b: the CPU column is the unsplit gap wall (50%)");
            CHECK_NUM(r->wait_pct, 50.0, "13b: and Σwaits/db is the other half");
            CHECK_MS(r->offcpu_ms, 0.0,
                     "13b: off-CPU is 0 as a REFUSAL to split, which "
                     "has_measured_cpu == 0 is what distinguishes from a "
                     "measured zero");
        }
        free(ts.rows);
    }

    /* 13c. A gap STRADDLING the window edge contributes only the CPU that
     * happened inside it: 10 ms wall / 4 ms measured, half in window ->
     * 5 ms of DB Time, 2 ms measured, 3 ms off-CPU. The same fraction split
     * the time model and #559 use, record for record.
     * Plus the clamp: pid 400's measurement exceeds its own wall. */
    {
        struct pgwt_trace_event evs[1];
        evs[0] = mk(300, CPU_GAP, WFROM + S(1), 10000000ULL, 0, 4000000ULL);
        /* Window opens 5 ms before the record closes: half of it is inside. */
        uint64_t wfrom = WFROM + S(1) - 5000000ULL;
        struct pgwt_sessions_result ts;
        pgwt_compute_top_sessions(evs, 1, &f, wfrom, WTO, 10000.0, &ts);
        const struct pgwt_session_row *r = sess_row(&ts, 300);
        CHECK(r != NULL, "13c: the straddling session has a row");
        if (r) {
            CHECK_MS(r->db_time_ms, 5.0, "13c: only the in-window 5 ms of wall");
            CHECK_NUM(r->cpu_pct, 2.0 / 5.0 * 100.0,
                      "13c: and only the in-window HALF of the measurement "
                      "(2 ms of 5 ms = 40%), never the whole 4 ms");
            CHECK_MS(r->offcpu_ms, 3.0, "13c: with 3 ms of off-CPU remainder");
        }
        free(ts.rows);
    }
    {
        struct pgwt_trace_event evs[1];
        evs[0] = mk(400, CPU_GAP, WFROM + S(1), 10000000ULL, 0, 12000000ULL);
        struct pgwt_sessions_result ts;
        pgwt_compute_top_sessions(evs, 1, &f, WFROM, WTO, 10000.0, &ts);
        const struct pgwt_session_row *r = sess_row(&ts, 400);
        CHECK(r != NULL, "13c: the over-measured session has a row");
        if (r) {
            CHECK_NUM(r->cpu_pct, 100.0,
                      "13c: a measurement past its own wall is CLAMPED to "
                      "the wall (sum_exec_runtime is only current at a tick)");
            CHECK_MS(r->offcpu_ms, 0.0, "13c: leaving no off-CPU remainder");
        }
        CHECK_MS(ts.cpu_clamped_ms, 2.0,
                 "13c: …and the 2 ms excess is COUNTED, not silently "
                 "absorbed — the same self-check the time model reports");
        free(ts.rows);
    }

    /* 13d. The v9 SUMMARY twin: measured CPU and off-CPU match 13a per term.
     * One synthetic per-second record describing the SAME session as 13a —
     * 15 ms of DB Time, a 10 ms gap wall, and 4 ms measured on a core. */
    {
        struct pgwt_summary_accum rec;
        memset(&rec, 0, sizeof(rec));
        rec.second_wall_ns = WFROM + S(1);
        rec.sessions[0].pid = 100;
        rec.sessions[0].db_time_ns = 15000000ULL;
        rec.sessions[0].cpu_ns = 10000000ULL;          /* total gap WALL */
        rec.sessions[0].cpu_offcpu_ns = 6000000ULL;    /* wall - measured */
        rec.sessions[0].cpu_measured_records = 1;
        rec.sessions[0].top_wait_id = LOCK_RELATION;
        rec.sessions[0].top_wait_ns = 5000000ULL;
        rec.num_sessions = 1;
        fake_summary_rec = &rec;
        struct pgwt_sessions_result sum;
        pgwt_compute_top_sessions_from_summaries("/nonexistent", WFROM, WTO,
                                                 &f, 10000.0, &sum);
        fake_summary_rec = NULL;

        const struct pgwt_session_row *sr = sess_row(&sum, 100);
        CHECK(sum.has_measured_cpu == 1,
              "13d: the summary routing DECLARES a measurement");
        CHECK(sr != NULL, "13d: the summary row exists");
        if (sr) {
            CHECK_NUM(sr->cpu_pct, 4.0 / 15.0 * 100.0,
                      "13d: CPU is the measured 4 ms over 15 ms DB Time");
            CHECK_MS(sr->offcpu_ms, 6.0,
                     "13d: the 6 ms runqueue remainder is reported");
            /* wait_pct is the SAME quantity on both paths, and must be the
             * same floating-point EXPRESSION too: it used to read
             * `100 - cpu_pct` here, which is algebraically Σwaits/db on this
             * path but differs from it in the last bit, and
             * tests/test_agg_raw_crosscheck.c compares the two paths with
             * exact equality (`!(a == b)`). Subtract the FULL gap wall,
             * not measured CPU: 5 ms of 15. */
            CHECK_NUM(sr->wait_pct, 5.0 / 15.0 * 100.0,
                      "13d: the summary wait_pct is Σwaits/db, computed as "
                      "the integer (db − cpu) over db, exactly as raw "
                      "divides");
            /* Each term must agree independently on this fixture.
             * Rebuilt from the raw path rather than from 13a's locals, so
             * this compares two computations and not one. */
            struct pgwt_trace_event evs[2];
            evs[0] = mk(100, CPU_GAP, WFROM + S(1), 10000000ULL, 0, 4000000ULL);
            evs[1] = mk(100, LOCK_RELATION, WFROM + S(2), 5000000ULL, 0,
                        PGWT_CPU_NS_UNKNOWN);
            struct pgwt_sessions_result raw;
            pgwt_compute_top_sessions(evs, 2, &f, WFROM, WTO, 10000.0, &raw);
            const struct pgwt_session_row *rr = sess_row(&raw, 100);
            CHECK(rr != NULL, "13d: the raw row exists for the same session");
            if (rr) {
                double raw_cpu_ms = rr->cpu_pct / 100.0 * rr->db_time_ms;
                double sum_cpu_ms = sr->cpu_pct / 100.0 * sr->db_time_ms;
                CHECK_MS(raw_cpu_ms, sum_cpu_ms,
                         "13d identity: raw CPU ms == summary CPU ms");
                CHECK_MS(rr->offcpu_ms, sr->offcpu_ms,
                         "13d identity: raw off-CPU == summary off-CPU");
                CHECK_MS(rr->db_time_ms, sr->db_time_ms,
                         "13d: raw DB Time == summary DB Time");
                CHECK(sum_cpu_ms != 10.0,
                      "13d non-vacuity: measured CPU differs from the "
                      "10 ms gap wall");
            }
            free(raw.rows);
        }
        free(sum.rows);
    }

    /* 13e. An UNKNOWN-only window must STILL refuse to split. */
    {
        struct pgwt_summary_accum rec;
        memset(&rec, 0, sizeof(rec));
        rec.second_wall_ns = WFROM + S(1);
        rec.sessions[0].pid = 100;
        rec.sessions[0].db_time_ns = 15000000ULL;
        rec.sessions[0].cpu_ns = 10000000ULL;  /* full UNKNOWN gap wall */
        rec.sessions[0].cpu_offcpu_ns = 0;
        rec.sessions[0].cpu_measured_records = 0;
        rec.sessions[0].top_wait_id = LOCK_RELATION;
        rec.sessions[0].top_wait_ns = 5000000ULL;
        rec.num_sessions = 1;
        fake_summary_rec = &rec;
        struct pgwt_sessions_result sum;
        pgwt_compute_top_sessions_from_summaries("/nonexistent", WFROM, WTO,
                                                 &f, 10000.0, &sum);
        fake_summary_rec = NULL;

        const struct pgwt_session_row *sr = sess_row(&sum, 100);
        /* Otherwise inferred wall would print as "CPU (running)" beside a
         * 0.0 ms runqueue sibling, which the compute contract forbids. */
        CHECK(sum.has_measured_cpu == 0,
              "13e: an UNKNOWN-only summary does NOT claim a measurement");
        CHECK(sr != NULL, "13e: the UNKNOWN-only summary row exists");
        if (sr) {
            CHECK_NUM(sr->cpu_pct, 10.0 / 15.0 * 100.0,
                      "13e: CPU is the unsplit gap wall, by gap-inference");
            CHECK_MS(sr->offcpu_ms, 0.0,
                     "13e: off-CPU is 0 as a REFUSAL to split, which "
                     "has_measured_cpu == 0 is what distinguishes from a "
                     "measured zero");
        }
        free(sum.rows);
    }
}

/* ── 14. #529: the execution-detail tooltip's wall time ───────────────────
 *
 * THE DEFECT. #496 made `pgwt_exec_event.duration_ns` the slice INSIDE the
 * execution window, which is right for the column that gets SUMMED (before
 * #496 the per-event durations added up past the execution's own duration).
 * But the clipped value became the ONLY one there is, and the Waterfall
 * tooltip reads it as the event's wall time: a 4 ms wait that opened 2 ms
 * before EXEC_START displays as 2 ms. The record's own duration was on no
 * wire field at all, so no client could say what the wait actually was.
 *
 * So the struct carries BOTH: `duration_ns` (clipped, summable) and
 * `raw_duration_ns` (the record's own wall time). src/server.c serialises
 * them as `dur_ns` and `raw_dur_ns`.
 *
 * WHY THE ASSERTIONS ARE ABSOLUTE LITERALS AND NOT AN AGREEMENT. Lane C's
 * #562 trap in miniature: "the two fields differ on a straddler" is
 * satisfied by any pair of different numbers, and "they agree on an interior
 * event" is satisfied by emitting one value twice. Each arm below names the
 * nanosecond count it requires, and the two arms disagree about which field
 * moves, so no single wrong implementation satisfies both:
 *
 *   straddler  dur_ns = 2 ms   raw_dur_ns = 4 ms   (#529's measured pair)
 *   interior   dur_ns = 5 ms   raw_dur_ns = 5 ms
 *
 * - emit the raw value into dur_ns (i.e. revert #496): straddler dur_ns
 *   reads 4 ms, fails.
 * - emit the clipped value into raw_dur_ns (the no-op field): straddler
 *   raw_dur_ns reads 2 ms, fails.
 * - leave raw_duration_ns unset: reads 0, fails.
 * - raw_dur_ns = 2 * dur_ns (which the straddler alone cannot tell apart):
 *   the interior event reads 10 ms, fails.
 *
 * Nothing here touches a clock, a socket or a subprocess: two in-memory
 * arrays and absolute ns literals. There is no ordering to get wrong either
 * — events are located by event id, never by index.
 */

#define EXEC_PID        100u
#define EXEC_QID        4242ULL
#define EXEC_START_NS   (T0 + S(1))
#define EXEC_END_NS     (T0 + S(3))
#define MS(x)           ((uint64_t)(x) * 1000000ULL)

/* Locate one lane entry by event id. Returns NULL when the lane does not
 * carry it — and section 14b REQUIRES that NULL for an id the fixture never
 * produced, so "the helper found nothing" can never read as a pass. */
static const struct pgwt_exec_event *
lane_event(const struct pgwt_exec_lane *lane, uint32_t event_id)
{
    for (int i = 0; i < lane->num_events; i++)
        if (lane->events[i].event_id == event_id)
            return &lane->events[i];
    return NULL;
}

/* The fixture. `straddle` chooses whether the Lock:relation record crosses
 * EXEC_START (the #529 case) or sits wholly inside the execution (14b's
 * control, where clipping must be a no-op). Everything else is identical, so
 * the control differs from the case in exactly one property. */
static int build_exec_fixture(struct pgwt_trace_event *ev, int straddle)
{
    int n = 0;
    /* EXEC_START must be the first record of the pid the scan sees: the lane
     * only accepts events while inside_exec. */
    ev[n++] = mk(EXEC_PID, PGWT_MARKER_EXEC_START, EXEC_START_NS, 0,
                 EXEC_QID, 0);
    /* The straddler: 4 ms long, closing 2 ms after EXEC_START, so 2 ms of it
     * is inside the execution. Non-straddling control: the same 4 ms record
     * closing 6 ms after EXEC_START, wholly inside. */
    ev[n++] = mk(EXEC_PID, LOCK_RELATION,
                 straddle ? EXEC_START_NS + MS(2) : EXEC_START_NS + MS(6),
                 MS(4), EXEC_QID, PGWT_CPU_NS_UNKNOWN);
    /* The interior event, 5 ms, never clipped on either fixture. */
    ev[n++] = mk(EXEC_PID, IO_READ, EXEC_START_NS + S(1), MS(5),
                 EXEC_QID, PGWT_CPU_NS_UNKNOWN);
    ev[n++] = mk(EXEC_PID, PGWT_MARKER_EXEC_END, EXEC_END_NS, 0, EXEC_QID, 0);
    return n;
}

static void test_exec_detail_raw_duration(void)
{
    printf("\n14. #529: a clipped slice is not the event's wall time\n");

    struct pgwt_trace_event ev[8];
    int n = build_exec_fixture(ev, 1);

    struct pgwt_execution_detail_result res;
    pgwt_compute_execution_detail(ev, n, EXEC_PID, EXEC_START_NS, EXEC_END_NS,
                                  NULL, NULL, 0, 16, &res);

    CHECK(!res.failed, "14a: the detail pass completed");
    CHECK(res.found_execution == 1,
          "14a: the requested EXEC_START matched — the lane is populated by "
          "the scan and not by an empty-result shortcut");
    CHECK_U64(res.leader.num_events, 2,
              "14a: both records entered the leader lane");

    const struct pgwt_exec_event *str = lane_event(&res.leader, LOCK_RELATION);
    const struct pgwt_exec_event *in  = lane_event(&res.leader, IO_READ);
    CHECK(str != NULL, "14a: the straddling Lock:relation record is present");
    CHECK(in != NULL, "14a: the interior IO:DataFileRead record is present");

    if (str) {
        /* The #496 contract, unchanged: what clients SUM is the slice. */
        CHECK_U64(str->duration_ns, MS(2),
                  "14a: duration_ns is the 2 ms slice inside the execution "
                  "(#496) — a client summing the lane still cannot exceed "
                  "the execution's own duration");
        /* The #529 contract: what a tooltip DISPLAYS is the wait. */
        CHECK_U64(str->raw_duration_ns, MS(4),
                  "14a: raw_duration_ns is the record's own 4 ms wall time "
                  "— the number #529 measured displaying as 2 ms");
        CHECK(str->raw_duration_ns > str->duration_ns,
              "14a: and on a straddler the two are not the same number");
        CHECK_U64(str->start_ns, EXEC_START_NS,
                  "14a: start_ns stays clamped to the execution start, so the "
                  "slice is (start_ns, start_ns+duration_ns) and still lies "
                  "inside the execution");
    }
    if (in) {
        CHECK_U64(in->duration_ns, MS(5),
                  "14a: the interior record's slice is its whole 5 ms");
        CHECK_U64(in->raw_duration_ns, MS(5),
                  "14a: and its raw wall time is the same 5 ms — which is "
                  "what kills raw_dur_ns = 2 * dur_ns, a rule the straddler "
                  "alone cannot distinguish");
    }
    pgwt_free_execution_detail(&res);
}

/* 14b. FALSE NEGATIVES. Every way section 14a could be satisfied without
 * checking anything. A gate that cannot see must refuse, never approve. */
static void test_exec_detail_raw_duration_bypass(void)
{
    printf("\n14b. #529 bypass: the ways this gate could stop seeing\n");

    struct pgwt_trace_event ev[8];
    int n = build_exec_fixture(ev, 1);
    struct pgwt_execution_detail_result res;

    /* (1) THE THING IS ABSENT RATHER THAN WRONG. An id the fixture never
     * produced must come back NULL, so 14a's `if (str)` guards sit behind a
     * CHECK that the record exists — not behind a silent skip. */
    pgwt_compute_execution_detail(ev, n, EXEC_PID, EXEC_START_NS, EXEC_END_NS,
                                  NULL, NULL, 0, 16, &res);
    CHECK(lane_event(&res.leader, CLIENT_READ) == NULL,
          "14b1: an event id the fixture never emitted is NOT found — the "
          "lookup cannot manufacture a row to assert on");
    CHECK(lane_event(&res.leader, LOCK_RELATION) != NULL,
          "14b1 control: the same lookup DOES find the id the fixture emits, "
          "so 14b1 is not passing because the helper always returns NULL");
    pgwt_free_execution_detail(&res);

    /* (2) EMPTY INPUT. No events at all: the lane is empty and the execution
     * was not found. 14a's found_execution and num_events assertions are the
     * ones that separate this from a real answer. */
    pgwt_compute_execution_detail(NULL, 0, EXEC_PID, EXEC_START_NS,
                                  EXEC_END_NS, NULL, NULL, 0, 16, &res);
    CHECK_U64(res.leader.num_events, 0, "14b2: empty input -> empty lane");
    CHECK(res.found_execution == 0,
          "14b2: and found_execution is 0, which is what 14a asserts against "
          "— an empty answer cannot pose as a populated one");
    pgwt_free_execution_detail(&res);

    /* (3) THE LANE CAP MAKES THE PASS UNREACHABLE. max_events_per_lane 0
     * returns before the scan runs at all. This is the readiness-gate
     * failure mode: it must look like nothing happened, not like a pass. */
    pgwt_compute_execution_detail(ev, n, EXEC_PID, EXEC_START_NS, EXEC_END_NS,
                                  NULL, NULL, 0, 0, &res);
    CHECK_U64(res.leader.num_events, 0,
              "14b3: max_events_per_lane 0 -> no lane entries");
    CHECK(res.found_execution == 0,
          "14b3: and the scan never ran, so found_execution stays 0");
    pgwt_free_execution_detail(&res);

    /* (4) NO MATCHING EXECUTION. start_ns that no EXEC_START carries: the
     * records are all outside an open execution, so nothing is admitted.
     * A fixture that quietly stopped matching would otherwise assert over an
     * empty lane and pass. */
    pgwt_compute_execution_detail(ev, n, EXEC_PID, EXEC_START_NS + 1,
                                  EXEC_END_NS, NULL, NULL, 0, 16, &res);
    CHECK(res.found_execution == 0,
          "14b4: an unresolvable execution start is not found");
    CHECK_U64(res.leader.num_events, 0,
              "14b4: and admits nothing, rather than admitting unclipped "
              "records under a window it could not resolve");
    pgwt_free_execution_detail(&res);

    /* (5) THE FIXTURE DOES NOT ACTUALLY STRADDLE. The same record 4 ms long,
     * moved wholly inside the execution. Now clipping is a no-op and the two
     * fields MUST agree — which is what proves 14a's inequality comes from
     * the straddle and not from the field being unconditionally different. */
    n = build_exec_fixture(ev, 0);
    pgwt_compute_execution_detail(ev, n, EXEC_PID, EXEC_START_NS, EXEC_END_NS,
                                  NULL, NULL, 0, 16, &res);
    {
        const struct pgwt_exec_event *e = lane_event(&res.leader,
                                                     LOCK_RELATION);
        CHECK(e != NULL, "14b5: the non-straddling record is present");
        if (e) {
            CHECK_U64(e->duration_ns, MS(4),
                      "14b5: a record wholly inside the execution is not "
                      "clipped");
            CHECK_U64(e->raw_duration_ns, MS(4),
                      "14b5: and its raw wall time is the same 4 ms — the two "
                      "fields differ ONLY where the boundary cuts");
        }
    }
    pgwt_free_execution_detail(&res);

    /* (6) A RIGHT-OPEN RECORD. #424 flags a wait still running at the window
     * edge; the detail scan drops it, so it contributes NO lane entry. The
     * hazard this guards is the opposite of 14a's: a dropped record must not
     * reappear carrying a raw duration the execution never contained. */
    n = build_exec_fixture(ev, 1);
    ev[1].flags |= PGWT_EVENT_FLAG_RIGHT_OPEN;
    pgwt_compute_execution_detail(ev, n, EXEC_PID, EXEC_START_NS, EXEC_END_NS,
                                  NULL, NULL, 0, 16, &res);
    CHECK(lane_event(&res.leader, LOCK_RELATION) == NULL,
          "14b6: a RIGHT_OPEN straddler is excluded from the lane entirely "
          "— neither field is emitted for it");
    CHECK_U64(res.leader.num_events, 1,
              "14b6: and the interior record still arrives, so 14b6 is not "
              "passing because the whole lane went empty");
    pgwt_free_execution_detail(&res);

    /* (7) A CORRUPT RECORD whose duration exceeds its own timestamp. The
     * hazard would be a wrapped subtraction producing a vast raw figure.
     * It is UNREACHABLE, and this block asserts the DEADNESS rather than
     * asserting against it. The first draft did assert the admitted
     * record's two fields and went red: the record never arrives at all.
     *
     * pgwt_filter_matches refuses `duration_ns > timestamp_ns`
     * (src/compute.c:375) before pgwt_compute_execution_detail's selection
     * reaches detail_lane_add, so no caller can hand that function a record
     * whose raw_start underflows. Both arms are needed: on its own, "the
     * lane does not carry it" is also satisfied by a fixture that stopped
     * producing records for an unrelated reason. */
    n = 0;
    ev[n++] = mk(EXEC_PID, PGWT_MARKER_EXEC_START, EXEC_START_NS, 0,
                 EXEC_QID, 0);
    ev[n++] = mk(EXEC_PID, LOCK_RELATION, EXEC_START_NS + MS(2),
                 EXEC_START_NS + MS(2) + 1, EXEC_QID, PGWT_CPU_NS_UNKNOWN);
    ev[n++] = mk(EXEC_PID, PGWT_MARKER_EXEC_END, EXEC_END_NS, 0, EXEC_QID, 0);
    pgwt_compute_execution_detail(ev, n, EXEC_PID, EXEC_START_NS, EXEC_END_NS,
                                  NULL, NULL, 0, 16, &res);
    CHECK(res.found_execution == 1,
          "14b7: the execution itself still resolves, so the emptiness below "
          "is the record being refused and not the scan failing to run");
    CHECK(lane_event(&res.leader, LOCK_RELATION) == NULL,
          "14b7: a record whose duration exceeds its own timestamp never "
          "reaches detail_lane_add -- raw_start cannot underflow because the "
          "input cannot exist, not because a clamp catches it");
    pgwt_free_execution_detail(&res);

    /* 14b7 CONTROL: the same record at the same timestamp with a duration
     * that can be true. It IS admitted and carries both fields, which is
     * what makes the refusal above attributable to the impossible
     * duration. */
    ev[1].duration_ns = MS(4);
    pgwt_compute_execution_detail(ev, n, EXEC_PID, EXEC_START_NS, EXEC_END_NS,
                                  NULL, NULL, 0, 16, &res);
    {
        const struct pgwt_exec_event *e = lane_event(&res.leader,
                                                     LOCK_RELATION);
        CHECK(e != NULL,
              "14b7 control: a possible duration at the same timestamp IS "
              "admitted");
        if (e) {
            CHECK_U64(e->raw_duration_ns, MS(4),
                      "14b7 control: and carries its own 4 ms wall time");
            CHECK_U64(e->duration_ns, MS(2),
                      "14b7 control: clipped to the 2 ms inside the "
                      "execution");
        }
    }
    pgwt_free_execution_detail(&res);
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
    test_transitions_table_full();
    test_transitions_alloc_refusal();
    test_tag_table_scale();
    test_time_model_offcpu_sibling();
    test_sessions_measured_cpu();
    test_exec_detail_raw_duration();
    test_exec_detail_raw_duration_bypass();

    /* A suite that ran no assertions is a suite that proved nothing. The
     * floor moves WITH the suite: sections 7-9 (#426, #440, #441) add 44
     * assertions, and a floor left at 80 would have let any one of the three
     * silently stop running.
     *
     * Both arms of the integration raise it. #553's sections 10-11 add 66,
     * then 11e's clamp counter, 11j's idle-class sweep and 11m's rounding
     * gate add 14 (floor 211 on that branch). #461's section 10 adds 11 --
     * and its 10e branch asserts the SAME number of times whichever way the
     * child goes, so a hang cannot satisfy the floor by running fewer
     * checks -- 10f adds 4 more, #374's section 11 adds 9, #575's section 12
     * adds 21 and #562's section 13 adds 32 (floor 208 on that branch).
     * Sections behind `if (r)` guards would run FEWER checks rather than
     * failing each one, which is exactly what this floor is for.
     *
     * The union is MEASURED, not added up from the two branch floors: the
     * base suite runs 131 against a floor of 130, so naively summing the
     * two deltas off the FLOOR gives 289 and the test fails. Built and run
     * on the merged tree it reports 288 (131 base + 80 from the #553 arm
     * + 77 from the #461 arm), and 288 is what is pinned here. A dropped
     * call in main() is the false negative this floor exists to catch, so
     * it moves with the union rather than staying where a deleted section
     * would still clear it.
     *
     * #529's section 14 adds 11 and its 14b bypass suite 18 (floor 237 on
     * agent/laneC2-raw-compute). 14a's field assertions sit behind
     * `if (str)` / `if (in)` for the same reason section 13's do -- the
     * record-present CHECK is what fails, and the shortfall in this count
     * is what catches a section that stopped running its body at all.
     * Measured on the merged tree: 317. */
    if (checks < 317) {
        printf("FAIL: only %d assertions ran — the suite did not execute\n",
               checks);
        return 1;
    }

    printf("\n%d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
