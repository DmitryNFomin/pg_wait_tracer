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
    test_transitions_table_full();
    test_transitions_alloc_refusal();
    test_tag_table_scale();
    test_time_model_offcpu_sibling();

    /* A suite that ran no assertions is a suite that proved nothing. The
     * floor moves WITH the suite: sections 7-9 (#426, #440, #441) add 44
     * assertions, and a floor left at 80 would have let any one of the three
     * silently stop running. Section 10 (#461) adds 11 -- and its 10e branch
     * asserts the SAME number of times whichever way the child goes, so a
     * hang cannot satisfy the floor by running fewer checks. 10f adds 4
     * more (the two allocators' refusal paths and their two controls).
     * Section 11 (#374) adds 9, section 12 (#575) 21. */
    if (checks < 176) {
        printf("FAIL: only %d assertions ran — the suite did not execute\n",
               checks);
        return 1;
    }

    printf("\n%d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
