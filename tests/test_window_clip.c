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

int main(void)
{
    printf("=== test_window_clip ===\n");

    test_straddler_leading_edge();
    test_straddler_trailing_edge();
    test_cross_tab_agreement();
    test_unbounded_window_unchanged();
    test_bypass_paths();
    test_latency_columns_keep_full_duration();

    /* A suite that ran no assertions is a suite that proved nothing. */
    if (checks < 40) {
        printf("FAIL: only %d assertions ran — the suite did not execute\n",
               checks);
        return 1;
    }

    printf("\n%d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
