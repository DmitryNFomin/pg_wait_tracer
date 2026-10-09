/* test_probe_bounds.c — #327: six open-addressing tables in src/compute.c
 * whose probe loop had no fullness test, plus the #347 re-keying shape.
 *
 * THE DEFECT. Each site was shaped
 *
 *     while (ht[h].<accumulator> > 0 && ht[h].<key> != k) h = (h + 1) & MASK;
 *
 * over a FIXED power-of-two table. Once the table holds <size> distinct keys
 * every slot is occupied and no slot matches, so that loop NEVER TERMINATES.
 * pgwt-server is single-threaded: the request never answers, the socket never
 * closes, and every tab stops at once. Not a wrong number — a hang.
 *
 *   :1580  top sessions (raw)          1024 slots, pid-keyed
 *   :2517  top sessions (summaries)    1024 slots, pid-keyed  <- 900 s default
 *   :1707  top queries (raw)           2048 slots, query_id
 *   :2676  top queries (summaries)     2048 slots, query_id
 *   :2990  fingerprints                2048 slots, query_id
 *   :4721  variants                    4096 slots, pattern hash
 *
 * THE SUMMARY SITES ARE THE DANGEROUS ONES. SUMMARY_MAX_SESSIONS /
 * SUMMARY_MAX_QUERIES bound ONE per-second record, not the window: distinct
 * keys accumulate ACROSS records. Sections 2 and 4 therefore spread cap+1
 * keys over TWO records of <= cap each — that is the assertion proving the
 * per-second cap does not bound the window. Summary paths also never reach
 * reject_overload, so no knob upstream bounds them either.
 *
 * WHY RED LOOKS LIKE A HANG. Pre-fix each section does not fail an
 * assertion, it spins forever. The red evidence is therefore "did not
 * terminate under a harness timeout", recorded in
 * tests/results/probe-bounds/. That is also the reason main() REFUSES when
 * any fixture is not strictly larger than its cap: a fixture trimmed back
 * under the cap would make this file pass instantly and say nothing.
 *
 * WHAT THE FIXTURES LOOK LIKE AND WHY.
 *  - cap+1 distinct keys, never cap. At exactly cap the pre-fix probe still
 *    terminates, so a cap-sized fixture is satisfied by the broken code
 *    (section 7.1 pins that).
 *  - Keys INTERLEAVED round-robin across rounds, not in blocks, so the
 *    overflow keys are scattered through the stream instead of sitting in a
 *    tail that a prefix-only answer would still get right.
 *  - A DISTINCT per-key duration (k+1 whole milliseconds, so every expected
 *    value is an exactly-representable double and no epsilon is needed), so
 *    a shortfall moves a TOTAL and not merely a count.
 *
 * #347, CORRECTED BY MEASUREMENT. The issue named compute.c:638 (AAS
 * per-event) and proposed guarding the key assignment with
 * `if (total_ns == 0)`. That guard is a NO-OP and the merge it describes
 * does not happen there: the probe can only exit with total_ns > 0 when the
 * key already matches. Section 6.3 asserts the AAS table is immune, so
 * nobody "fixes" it on faith. The SHAPE is real at three other tables that
 * used an accumulator as their occupancy test while also accumulating
 * fields that test does not look at — fingerprints (total_transitions),
 * top sessions (pid 0) and the summary query table — and sections 6.1/6.2
 * are those, measured red on the parent tree before the fix.
 *
 * SECTION 7 IS THE BYPASS SUITE: every way this file's own detection could
 * be made unreachable. A fixture at the cap; assertion shapes that cannot
 * see a shortfall; a summary stub that delivers nothing (the #275-era
 * `return -1` stub) or only ONE of the two records; key 0; a dense
 * consecutive key run; allocation failure at every new growth point, where
 * the answer must be ABSENT (failed == 1) and never short-but-complete;
 * and an unknown injection point, so an injection that silently did nothing
 * cannot be what made the refusals pass.
 *
 * Runs anywhere: in-memory arrays, synthetic timestamps, a stubbed summary
 * stream, no clock, no I/O, no threads. Nothing here can be flaky, and
 * there is no epsilon, retry, sleep or tolerance anywhere — every assertion
 * is an exact integer or an exact flag.
 */
#include "compute.h"
#include "wait_event.h"
#include "pg_wait_tracer.h"
#include "summary_reader.h"
#include "summary_writer.h"

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

#define CHECK_INT(got, want, msg) do {                               \
    checks++;                                                        \
    long long _g = (long long)(got), _w = (long long)(want);         \
    if (_g != _w) { failures++;                                      \
        printf("  FAIL: %s — got %lld, want %lld (%s:%d)\n", msg,    \
               _g, _w, __FILE__, __LINE__); }                        \
    else printf("  ok: %s (%lld)\n", msg, _g);                       \
} while (0)

/* Every expected millisecond value here is a whole number built from whole
 * milliseconds of input, so it is exactly representable and compared with
 * ==. There is deliberately no tolerance parameter in this file. */
#define CHECK_MS_EXACT(got, want, msg) do {                          \
    checks++;                                                        \
    double _g = (double)(got), _w = (double)(want);                  \
    if (_g != _w) { failures++;                                      \
        printf("  FAIL: %s — got %.6f ms, want %.6f ms (%s:%d)\n",   \
               msg, _g, _w, __FILE__, __LINE__); }                   \
    else printf("  ok: %s (%.6f ms)\n", msg, _g);                    \
} while (0)

/* ── Caps, as they were before the fix ─────────────────────────────────────
 * Literals, deliberately: the macros they came from (SESSION_HT_SIZE,
 * QUERY_HT_SIZE, FP_HT_SIZE, VARIANT_HT_SIZE) were DELETED by the fix, and
 * a fixture that tracked a live constant would silently stop exceeding
 * anything if that constant ever moved. These are the historical values,
 * named in src/compute.c's #327 comments at each site. */
#define CAP_SESSIONS 1024       /* was SESSION_HT_SIZE == MAX_BACKENDS */
#define CAP_QUERIES  2048       /* was QUERY_HT_SIZE, and FP_HT_SIZE */
#define CAP_VARIANTS 4096       /* was VARIANT_HT_SIZE */

#define N_SESSIONS (CAP_SESSIONS + 1)
#define N_QUERIES  (CAP_QUERIES + 1)
#define N_VARIANTS (CAP_VARIANTS + 1)

#define ROUNDS     3            /* interleaved passes over every key */
#define BASE_PID   40000u
#define BASE_QID   0x7000000000ULL
#define MS         1000000ULL   /* ns per ms */
#define TS_BASE    1000000000000ULL   /* 1000 s, so ts >= duration always */

/* LWLOCK class: non-idle, non-marker, and not event 0 (which compute.c
 * routes to cpu_ns rather than to a wait). */
#define WAIT_A WEI(PG_WAIT_LWLOCK, 1)
#define WAIT_B WEI(PG_WAIT_LWLOCK, 2)
static uint32_t wait_id(int k) { return WEI(PG_WAIT_LWLOCK, 1 + (uint32_t)k); }

/* ── Synthetic summary stream ─────────────────────────────────────────────
 * compute.c reaches the per-second records only through
 * pgwt_visit_summaries. This file provides that symbol, so the summary
 * sections drive THE REAL VISITORS over records built in memory: no files,
 * no writer, no clock, and an exactly known record set.
 *
 * `stub_mode` exists for the bypass suite: STUB_ERROR reproduces the
 * `return -1` stub that test_pid_cap and test_concurrency_bounds use, which
 * delivers NOTHING — the way the summary sections could silently become
 * vacuous. stub_visited is the positive control: every summary section
 * asserts how many records the visitor actually saw. */
enum { STUB_RECORDS, STUB_ERROR };
static int stub_mode = STUB_RECORDS;
static struct pgwt_summary_accum **stub_recs;
static int stub_nrecs;
static int stub_visited;        /* records handed to the visitor */

int pgwt_visit_summaries(const char *trace_dir, uint64_t from_wall_ns,
                         uint64_t to_wall_ns, pgwt_summary_visitor visitor,
                         void *ctx)
{
    (void)trace_dir; (void)from_wall_ns; (void)to_wall_ns;
    stub_visited = 0;
    if (stub_mode == STUB_ERROR)
        return -1;
    for (int i = 0; i < stub_nrecs; i++) {
        stub_visited++;
        if (visitor(stub_recs[i], ctx) != 0)
            break;              /* non-zero means stop, as the reader does */
    }
    return stub_visited;
}

static struct pgwt_summary_accum *rec_new(uint64_t second)
{
    struct pgwt_summary_accum *r = calloc(1, sizeof(*r));
    if (!r) { fprintf(stderr, "fixture OOM\n"); exit(2); }
    r->second_wall_ns = second;
    r->second_mono_ns = second;
    return r;
}

static void stub_set(struct pgwt_summary_accum **recs, int n)
{
    stub_mode = STUB_RECORDS;
    stub_recs = recs;
    stub_nrecs = n;
}

/* ── Raw event fixture ────────────────────────────────────────────────── */

struct fixture {
    struct pgwt_trace_event *ev;
    int n;
    int cap;
};

static void fx_push(struct fixture *fx, uint32_t pid, uint32_t old_e,
                    uint32_t new_e, uint64_t ts, uint64_t dur, uint64_t qid)
{
    if (fx->n >= fx->cap) {
        fx->cap = fx->cap ? fx->cap * 2 : 4096;
        struct pgwt_trace_event *tmp =
            realloc(fx->ev, (size_t)fx->cap * sizeof(*fx->ev));
        if (!tmp) { fprintf(stderr, "fixture OOM\n"); exit(2); }
        fx->ev = tmp;
    }
    struct pgwt_trace_event *e = &fx->ev[fx->n++];
    memset(e, 0, sizeof(*e));
    e->pid = pid;
    e->old_event = old_e;
    e->new_event = new_e;
    e->timestamp_ns = ts;
    e->duration_ns = dur;
    e->query_id = qid;
    e->cpu_ns = PGWT_CPU_NS_UNKNOWN;
}

static const struct pgwt_session_row *sess_row(
        const struct pgwt_sessions_result *r, uint32_t pid)
{
    for (int i = 0; i < r->num_rows; i++)
        if (r->rows[i].pid == pid) return &r->rows[i];
    return NULL;
}

static const struct pgwt_query_row *qry_row(
        const struct pgwt_queries_result *r, uint64_t qid)
{
    for (int i = 0; i < r->num_rows; i++)
        if (r->rows[i].query_id == qid) return &r->rows[i];
    return NULL;
}

static const struct pgwt_fingerprint_row *fp_row(
        const struct pgwt_fingerprint_result *r, uint64_t qid)
{
    for (int i = 0; i < r->num_rows; i++)
        if (r->rows[i].query_id == qid) return &r->rows[i];
    return NULL;
}

/* Sum of 1..n, the exact total-ms closed form for a (k+1)-ms fixture. */
static double tri(int n) { return (double)n * (n + 1) / 2.0; }

/* ── 1. Top sessions, raw (compute.c:1580) ────────────────────────────── */

/* `npids` distinct pids, ROUND-ROBIN across ROUNDS passes, pid k waiting
 * (k+1) ms each pass. Every expected number is a whole ms. */
static struct fixture build_sessions(int npids, int distinct_durations)
{
    struct fixture fx = {0};
    uint64_t ts = TS_BASE;
    for (int r = 0; r < ROUNDS; r++) {
        for (int k = 0; k < npids; k++) {
            uint64_t dur = (distinct_durations ? (uint64_t)(k + 1) : 1) * MS;
            fx_push(&fx, BASE_PID + (uint32_t)k, WAIT_A, WAIT_B, ts, dur, 0);
            ts += dur + MS;
        }
    }
    return fx;
}

static void section1_sessions_raw(void)
{
    printf("\n1. pgwt_compute_top_sessions: %d distinct pids (cap was %d)\n",
           N_SESSIONS, CAP_SESSIONS);

    struct fixture fx = build_sessions(N_SESSIONS, 1);
    struct pgwt_filter f;
    memset(&f, 0, sizeof(f));
    struct pgwt_sessions_result r;
    pgwt_compute_top_sessions(fx.ev, fx.n, &f, 0, 0, 0, &r);

    CHECK(!r.failed, "top sessions did not fail");
    CHECK_INT(r.num_rows, N_SESSIONS, "every distinct pid got a row");

    /* THE total assertion: a dropped key moves this, a dropped key in the
     * tail does not move any individual surviving row. */
    double total = 0;
    for (int i = 0; i < r.num_rows; i++)
        total += r.rows[i].db_time_ms;
    CHECK_MS_EXACT(total, ROUNDS * tri(N_SESSIONS),
                   "summed DB time accounts for every pid's waits");

    /* And the overflow key specifically: pid BASE_PID+1024 is the 1025th. */
    const struct pgwt_session_row *ov = sess_row(&r, BASE_PID + N_SESSIONS - 1);
    CHECK(ov != NULL, "the 1025th distinct pid is present");
    if (ov)
        CHECK_MS_EXACT(ov->db_time_ms, (double)ROUNDS * N_SESSIONS,
                       "...with its own exact DB time");

    free(r.rows);
    free(fx.ev);
}

/* ── 2. Top sessions, from summaries (compute.c:2517) ─────────────────── */

static void section2_sessions_summary(void)
{
    printf("\n2. pgwt_compute_top_sessions_from_summaries: %d pids over 2 "
           "records of <= %d (the per-second cap is NOT a window bound)\n",
           N_SESSIONS, CAP_SESSIONS);

    /* Record A: pids 0..1023. Record B: pid 1024, plus pid 0 again so the
     * cross-record accumulation itself is asserted. Each record is within
     * SUMMARY_MAX_SESSIONS; the WINDOW is not. */
    struct pgwt_summary_accum *a = rec_new(1000000000ULL);
    struct pgwt_summary_accum *b = rec_new(2000000000ULL);
    for (int k = 0; k < CAP_SESSIONS; k++) {
        a->sessions[k].pid = BASE_PID + (uint32_t)k;
        a->sessions[k].db_time_ns = (uint64_t)(k + 1) * MS;
        a->sessions[k].cpu_ns = 0;
        a->sessions[k].top_wait_id = WAIT_A;
        a->sessions[k].top_wait_ns = (uint64_t)(k + 1) * MS;
    }
    a->num_sessions = CAP_SESSIONS;
    b->sessions[0].pid = BASE_PID + (uint32_t)(N_SESSIONS - 1);
    b->sessions[0].db_time_ns = (uint64_t)N_SESSIONS * MS;
    b->sessions[0].top_wait_id = WAIT_A;
    b->sessions[0].top_wait_ns = (uint64_t)N_SESSIONS * MS;
    b->sessions[1].pid = BASE_PID;                   /* second helping */
    b->sessions[1].db_time_ns = 7 * MS;
    b->sessions[1].top_wait_id = WAIT_A;
    b->sessions[1].top_wait_ns = 7 * MS;
    b->num_sessions = 2;

    struct pgwt_summary_accum *recs[2] = { a, b };
    stub_set(recs, 2);

    struct pgwt_filter f;
    memset(&f, 0, sizeof(f));
    struct pgwt_sessions_result r;
    pgwt_compute_top_sessions_from_summaries("ignored", 0, 0, &f, 0, &r);

    CHECK_INT(stub_visited, 2, "the visitor saw BOTH records "
                               "(positive control: the stub delivered)");
    CHECK(!r.failed, "summary top sessions did not fail");
    CHECK_INT(r.num_rows, N_SESSIONS,
              "every pid across the two records got a row");

    double total = 0;
    for (int i = 0; i < r.num_rows; i++)
        total += r.rows[i].db_time_ms;
    CHECK_MS_EXACT(total, tri(N_SESSIONS) + 7.0,
                   "summed DB time accounts for both records");

    const struct pgwt_session_row *ov = sess_row(&r, BASE_PID + N_SESSIONS - 1);
    CHECK(ov != NULL, "the 1025th pid — which arrived in the SECOND record — "
                      "is present");
    if (ov)
        CHECK_MS_EXACT(ov->db_time_ms, (double)N_SESSIONS,
                       "...with its own exact DB time");
    const struct pgwt_session_row *first = sess_row(&r, BASE_PID);
    CHECK(first != NULL, "pid seen in both records is present once");
    if (first)
        CHECK_MS_EXACT(first->db_time_ms, 1.0 + 7.0,
                       "...with its two records SUMMED, not replaced");

    free(r.rows);
    free(a);
    free(b);
}

/* ── 3. Top queries, raw (compute.c:1707) ─────────────────────────────── */

static struct fixture build_queries(int nqids)
{
    struct fixture fx = {0};
    uint64_t ts = TS_BASE;
    for (int r = 0; r < ROUNDS; r++) {
        for (int k = 0; k < nqids; k++) {
            uint64_t dur = (uint64_t)(k + 1) * MS;
            fx_push(&fx, BASE_PID + (uint32_t)(k % 64), WAIT_A, WAIT_B, ts,
                    dur, BASE_QID + (uint64_t)k);
            ts += dur + MS;
        }
    }
    return fx;
}

static void section3_queries_raw(void)
{
    printf("\n3. pgwt_compute_top_queries: %d distinct query ids (cap was "
           "%d)\n", N_QUERIES, CAP_QUERIES);

    struct fixture fx = build_queries(N_QUERIES);
    struct pgwt_filter f;
    memset(&f, 0, sizeof(f));
    struct pgwt_queries_result r;
    pgwt_compute_top_queries(fx.ev, fx.n, &f, 0, 0, 0, &r);

    CHECK(!r.failed, "top queries did not fail");
    CHECK_INT(r.num_rows, N_QUERIES, "every distinct query id got a row");
    CHECK_MS_EXACT(r.db_time_ms, ROUNDS * tri(N_QUERIES),
                   "db_time_ms accounts for every query id");

    const struct pgwt_query_row *ov = qry_row(&r, BASE_QID + N_QUERIES - 1);
    CHECK(ov != NULL, "the 2049th distinct query id is present");
    if (ov) {
        CHECK_INT(ov->count, ROUNDS, "...with its exact observation count");
        CHECK_MS_EXACT(ov->total_ms, (double)ROUNDS * N_QUERIES,
                       "...and its exact total");
    }

    free(r.rows);
    free(fx.ev);
}

/* ── 4. Top queries, from summaries (compute.c:2676) ──────────────────── */

static void section4_queries_summary(void)
{
    printf("\n4. pgwt_compute_top_queries_from_summaries: %d query ids over "
           "2 records of <= %d\n", N_QUERIES, CAP_QUERIES);

    struct pgwt_summary_accum *a = rec_new(1000000000ULL);
    struct pgwt_summary_accum *b = rec_new(2000000000ULL);
    for (int k = 0; k < CAP_QUERIES; k++) {
        a->queries[k].query_id = BASE_QID + (uint64_t)k;
        a->queries[k].count = 1;
        a->queries[k].total_ns = (uint64_t)(k + 1) * MS;
        a->queries[k].top_wait_id = WAIT_A;
        a->queries[k].top_wait_ns = (uint64_t)(k + 1) * MS;
    }
    a->num_queries = CAP_QUERIES;
    b->queries[0].query_id = BASE_QID + (uint64_t)(N_QUERIES - 1);
    b->queries[0].count = 1;
    b->queries[0].total_ns = (uint64_t)N_QUERIES * MS;
    b->queries[0].top_wait_id = WAIT_A;
    b->queries[0].top_wait_ns = (uint64_t)N_QUERIES * MS;
    b->num_queries = 1;

    struct pgwt_summary_accum *recs[2] = { a, b };
    stub_set(recs, 2);

    struct pgwt_filter f;
    memset(&f, 0, sizeof(f));
    struct pgwt_queries_result r;
    pgwt_compute_top_queries_from_summaries("ignored", 0, 0, &f, 0, &r);

    CHECK_INT(stub_visited, 2, "the visitor saw BOTH records "
                               "(positive control: the stub delivered)");
    CHECK(!r.failed, "summary top queries did not fail");
    CHECK_INT(r.num_rows, N_QUERIES,
              "every query id across the two records got a row");
    CHECK_MS_EXACT(r.db_time_ms, tri(N_QUERIES),
                   "db_time_ms accounts for both records");

    const struct pgwt_query_row *ov = qry_row(&r, BASE_QID + N_QUERIES - 1);
    CHECK(ov != NULL, "the 2049th query id — in the SECOND record — is "
                      "present");
    if (ov)
        CHECK_MS_EXACT(ov->total_ms, (double)N_QUERIES,
                       "...with its exact total");

    free(r.rows);
    free(a);
    free(b);
}

/* ── 5. Fingerprints (compute.c:2990) and variants (compute.c:4721) ───── */

static void section5a_fingerprints(void)
{
    printf("\n5a. pgwt_compute_fingerprints: %d distinct query ids (cap was "
           "%d)\n", N_QUERIES, CAP_QUERIES);

    struct fixture fx = build_queries(N_QUERIES);
    struct pgwt_filter f;
    memset(&f, 0, sizeof(f));
    struct pgwt_fingerprint_result r;
    pgwt_compute_fingerprints(fx.ev, fx.n, &f, &r);

    CHECK(!r.failed, "fingerprints did not fail");
    CHECK_INT(r.num_rows, N_QUERIES, "every distinct query id got a row");

    uint64_t total_tr = 0;
    for (int i = 0; i < r.num_rows; i++)
        total_tr += r.rows[i].total_transitions;
    CHECK_INT(total_tr, (uint64_t)N_QUERIES * ROUNDS,
              "every transition is accounted for exactly once");

    const struct pgwt_fingerprint_row *ov =
        fp_row(&r, BASE_QID + N_QUERIES - 1);
    CHECK(ov != NULL, "the 2049th query id is present");
    if (ov)
        CHECK_INT(ov->total_transitions, ROUNDS,
                  "...with its own transition count");

    free(r.rows);
    free(fx.ev);
}

/* `nvariants` distinct two-step flow patterns (ev_a, ev_b) with a != b,
 * drawn from 65 synthetic LWLOCK ids: 65*64 = 4160 ordered pairs, well
 * past the 4096 cap, each two steps long (far inside the 32-step bound) and
 * distinct by construction. */
static struct fixture build_variants(int nvariants)
{
    struct fixture fx = {0};
    uint64_t ts = TS_BASE;
    int made = 0;
    for (int ia = 0; ia < 65 && made < nvariants; ia++) {
        for (int ib = 0; ib < 65 && made < nvariants; ib++) {
            if (ia == ib) continue;
            uint32_t pid = BASE_PID + (uint32_t)(made % 16);
            uint64_t qid = BASE_QID + (uint64_t)(made % 8);
            fx_push(&fx, pid, PGWT_MARKER_EXEC_START, PGWT_MARKER_EXEC_START,
                    ts, 0, qid);
            fx_push(&fx, pid, wait_id(ia), wait_id(ib), ts + MS, MS, qid);
            fx_push(&fx, pid, wait_id(ib), WAIT_A, ts + 2 * MS, MS, qid);
            fx_push(&fx, pid, PGWT_MARKER_EXEC_END, PGWT_MARKER_EXEC_END,
                    ts + 3 * MS, 0, qid);
            ts += 10 * MS;
            made++;
        }
    }
    return fx;
}

static void section5b_variants(void)
{
    printf("\n5b. pgwt_compute_variants: %d distinct flow patterns (cap was "
           "%d)\n", N_VARIANTS, CAP_VARIANTS);

    struct fixture fx = build_variants(N_VARIANTS);
    struct pgwt_variants_result r;
    pgwt_compute_variants(fx.ev, fx.n, NULL, 100000, PGWT_PHASE_EXEC, &r);

    CHECK(!r.failed, "variants did not fail");
    CHECK_INT(r.total_executions, N_VARIANTS,
              "every execution was walked");
    CHECK_INT(r.num_variants, N_VARIANTS,
              "every distinct pattern got its own variant row");

    int summed = 0;
    for (int i = 0; i < r.num_variants; i++)
        summed += r.variants[i].exec_count;
    CHECK_INT(summed, N_VARIANTS,
              "the variant rows account for every execution");

    free(r.variants);
    free(fx.ev);
}

/* ── 6. #347: an accumulator is not an occupancy test ─────────────────── */

/* compute.c's hash64, copied so the fixture can pick colliding query ids.
 * A copy is correct here: the point is to land two keys in ONE slot of the
 * table as the PRE-FIX code sized and hashed it (2048 slots, this mixer). */
static int hash64_copy(uint64_t key, int mask)
{
    key = (key ^ (key >> 30)) * 0xbf58476d1ce4e5b9ULL;
    key = (key ^ (key >> 27)) * 0x94d049bb133111ebULL;
    key = key ^ (key >> 31);
    return (int)(key & mask);
}

static void section6_1_fingerprint_rekey(void)
{
    printf("\n6.1 #347's shape where it is REAL: a zero-duration event used "
           "to donate its transitions to the next query id\n");

    /* Two query ids that collide in the pre-fix 2048-slot table. */
    uint64_t qa = 1, qb = 0;
    for (uint64_t k = 2; k < 1000000 && !qb; k++)
        if (hash64_copy(k, CAP_QUERIES - 1) == hash64_copy(qa, CAP_QUERIES - 1))
            qb = k;
    CHECK(qb != 0, "found two query ids colliding in the pre-fix table");
    if (!qb) return;
    printf("      qa=%llu qb=%llu both hash to slot %d\n",
           (unsigned long long)qa, (unsigned long long)qb,
           hash64_copy(qa, CAP_QUERIES - 1));

    struct fixture fx = {0};
    /* qa: ONE transition of duration 0. Pre-fix that left total_ns == 0, so
     * the slot still read as free while total_transitions was already 1. */
    fx_push(&fx, 100, WAIT_A, WAIT_B, TS_BASE, 0, qa);
    /* qb: two real transitions, probing to the same slot. */
    fx_push(&fx, 101, WAIT_A, WAIT_B, TS_BASE + MS, MS, qb);
    fx_push(&fx, 101, WAIT_B, WAIT_A, TS_BASE + 2 * MS, MS, qb);

    struct pgwt_filter f;
    memset(&f, 0, sizeof(f));
    struct pgwt_fingerprint_result r;
    pgwt_compute_fingerprints(fx.ev, fx.n, &f, &r);

    CHECK(!r.failed, "fingerprints did not fail");
    /* Measured on the parent tree: ONE row, qb, transitions = 3. */
    CHECK_INT(r.num_rows, 2, "both query ids have their own row");
    const struct pgwt_fingerprint_row *ra = fp_row(&r, qa);
    const struct pgwt_fingerprint_row *rb = fp_row(&r, qb);
    CHECK(ra != NULL, "the zero-duration query id is present, not absorbed");
    CHECK(rb != NULL, "the colliding query id is present");
    if (ra)
        CHECK_INT(ra->total_transitions, 1,
                  "zero-duration id keeps its OWN single transition");
    if (rb)
        CHECK_INT(rb->total_transitions, 2,
                  "colliding id reports 2, not the inherited 3");

    free(r.rows);
    free(fx.ev);
}

static void section6_2_session_pid_zero_rekey(void)
{
    printf("\n6.2 #347's shape in top sessions: pid 0 used to donate its DB "
           "time to the next pid that probed to slot 0\n");

    struct fixture fx = {0};
    /* pid 0 waits 5 ms. Pre-fix the slot still read as free (`pid != 0` was
     * the occupancy test), so pid 1024 — whose home slot is also 0 under
     * `pid % 1024` — claimed it AND inherited the 5 ms. Measured on the
     * parent tree: one row, pid 1024, db_time_ms = 6.000. */
    fx_push(&fx, 0, WAIT_A, WAIT_B, TS_BASE, 5 * MS, 0);
    fx_push(&fx, CAP_SESSIONS, WAIT_A, WAIT_B, TS_BASE + 10 * MS, MS, 0);

    struct pgwt_filter f;
    memset(&f, 0, sizeof(f));
    struct pgwt_sessions_result r;
    pgwt_compute_top_sessions(fx.ev, fx.n, &f, 0, 0, 0, &r);

    CHECK(!r.failed, "top sessions did not fail");
    CHECK_INT(r.num_rows, 2, "pid 0 and pid 1024 each have a row");
    const struct pgwt_session_row *r0 = sess_row(&r, 0);
    const struct pgwt_session_row *r1 = sess_row(&r, CAP_SESSIONS);
    CHECK(r0 != NULL, "pid 0 is a key like any other, not a free sentinel");
    if (r0)
        CHECK_MS_EXACT(r0->db_time_ms, 5.0, "pid 0 keeps its own 5 ms");
    CHECK(r1 != NULL, "pid 1024 is present");
    if (r1)
        CHECK_MS_EXACT(r1->db_time_ms, 1.0,
                       "pid 1024 reports 1 ms, not the inherited 6 ms");

    free(r.rows);
    free(fx.ev);
}

static void section6_3_aas_is_immune(void)
{
    printf("\n6.3 #347's NAMED site (AAS per-event, compute.c:638) is immune "
           "— the proposed one-line guard is a no-op\n");

    /* Two ids whose home slot is identical under `id & 511`, the first one
     * zero-duration. Pre-fix AND post-fix this is already correct: the
     * probe can only exit with total_ns > 0 when the key matches, so the
     * unconditional assignment never re-keys a slot that holds time. This
     * check is why no guard was added there: a fix whose red case cannot be
     * constructed is not a fix. */
    uint32_t ida = WEI(PG_WAIT_LWLOCK, 0x000001);
    uint32_t idb = WEI(PG_WAIT_LWLOCK, 0x000201);
    CHECK_INT(ida & 511u, idb & 511u, "the two ids share a home slot");

    struct fixture fx = {0};
    fx_push(&fx, 100, ida, idb, TS_BASE, 0, 0);                 /* 0 ns */
    fx_push(&fx, 100, idb, ida, TS_BASE + MS, MS, 0);           /* 1 ms */
    fx_push(&fx, 100, ida, idb, TS_BASE + 3 * MS, 2 * MS, 0);   /* 2 ms */

    struct pgwt_filter f;
    memset(&f, 0, sizeof(f));
    struct pgwt_aas_result aas;
    /* A 4 s window so each series' total_aas is time/4 s. */
    pgwt_compute_aas(fx.ev, fx.n, &f, TS_BASE - MS, TS_BASE + 3999 * MS,
                     4, 1, 10, &aas);

    CHECK_INT(aas.num_event_series, 2, "two distinct event series");
    double a_aas = -1, b_aas = -1;
    for (int i = 0; i < aas.num_event_series; i++) {
        if (aas.event_series[i].event_id == ida) a_aas = aas.event_series[i].total_aas;
        if (aas.event_series[i].event_id == idb) b_aas = aas.event_series[i].total_aas;
    }
    CHECK(a_aas >= 0 && b_aas >= 0, "both ids kept their own series");
    /* 2 ms / 4000 ms and 1 ms / 4000 ms — exact binary fractions. */
    CHECK(a_aas == 2.0 / 4000.0,
          "the zero-duration id's series holds exactly its own 2 ms");
    CHECK(b_aas == 1.0 / 4000.0,
          "the colliding id's series holds exactly its own 1 ms, not 3 ms");

    free(aas.event_aas);
    free(fx.ev);
}

/* ── 7. Bypass suite: how this file's detection could be made blind ───── */

static void s7_1_cap_sized_fixture_is_blind(void)
{
    printf("\n7.1 a fixture AT the cap cannot see the defect (pinned blind)\n");

    /* At exactly 1024 distinct pids the pre-fix probe still terminates, so
     * `!failed`, `num_rows > 0` and any individual row's DB time are all
     * satisfied by the BROKEN code. Only cap+1 reaches the hang, and only
     * the exact count and the summed total separate the two answers. */
    struct fixture capped = build_sessions(CAP_SESSIONS, 1);
    struct fixture over   = build_sessions(N_SESSIONS, 1);
    struct pgwt_filter f;
    memset(&f, 0, sizeof(f));
    struct pgwt_sessions_result rc, ro;
    pgwt_compute_top_sessions(capped.ev, capped.n, &f, 0, 0, 0, &rc);
    pgwt_compute_top_sessions(over.ev, over.n, &f, 0, 0, 0, &ro);

    CHECK(rc.num_rows > 0 && ro.num_rows > 0,
          "`num_rows > 0` holds for BOTH — a blind assertion shape");
    const struct pgwt_session_row *c0 = sess_row(&rc, BASE_PID);
    const struct pgwt_session_row *o0 = sess_row(&ro, BASE_PID);
    CHECK(c0 && o0 && c0->db_time_ms == o0->db_time_ms,
          "a spot-checked row is IDENTICAL in both — also blind");
    CHECK_INT(rc.num_rows, CAP_SESSIONS,
              "the cap-sized answer is unchanged by the fix");
    CHECK(rc.num_rows != ro.num_rows,
          "the exact-count assertion this file uses DOES separate them");

    double tc = 0, to = 0;
    for (int i = 0; i < rc.num_rows; i++) tc += rc.rows[i].db_time_ms;
    for (int i = 0; i < ro.num_rows; i++) to += ro.rows[i].db_time_ms;
    CHECK(tc != to, "...and so does the summed total");
    CHECK_MS_EXACT(tc, ROUNDS * tri(CAP_SESSIONS),
                   "the cap-sized total is exactly the pre-fix value");

    free(rc.rows); free(ro.rows);
    free(capped.ev); free(over.ev);
}

static void s7_2_identical_durations_are_blind_per_row(void)
{
    printf("\n7.2 identical durations: no per-row assertion can see a "
           "dropped key\n");

    struct fixture same = build_sessions(N_SESSIONS, 0);
    struct pgwt_filter f;
    memset(&f, 0, sizeof(f));
    struct pgwt_sessions_result r;
    pgwt_compute_top_sessions(same.ev, same.n, &f, 0, 0, 0, &r);

    int all_equal = 1;
    for (int i = 0; i < r.num_rows; i++)
        if (r.rows[i].db_time_ms != (double)ROUNDS) all_equal = 0;
    CHECK(all_equal,
          "with equal durations EVERY row is 3 ms — a per-row check passes "
          "whatever got dropped, which is why sections 1-5 use distinct ones");
    CHECK_INT(r.num_rows, N_SESSIONS,
              "the count assertion still separates them");

    free(r.rows);
    free(same.ev);
}

static void s7_3_summary_stub_delivers_nothing(void)
{
    printf("\n7.3 a summary stream that delivers NOTHING must read as empty, "
           "never as success\n");

    /* This is the `return -1` stub test_pid_cap and test_concurrency_bounds
     * use. If section 2 or 4 ran against it they would see zero records and
     * zero rows — and `!failed` would still hold. The positive control in
     * those sections (stub_visited) is what makes that impossible to miss. */
    stub_mode = STUB_ERROR;
    struct pgwt_filter f;
    memset(&f, 0, sizeof(f));
    struct pgwt_sessions_result rs;
    pgwt_compute_top_sessions_from_summaries("ignored", 0, 0, &f, 0, &rs);
    CHECK(!rs.failed && rs.num_rows == 0,
          "no records: empty, not failed, and NOT mistakable for cap+1 rows");
    CHECK_INT(stub_visited, 0, "...and the control reports 0 records seen");
    free(rs.rows);

    struct pgwt_queries_result rq;
    pgwt_compute_top_queries_from_summaries("ignored", 0, 0, &f, 0, &rq);
    CHECK(!rq.failed && rq.num_rows == 0,
          "same for top queries from summaries");
    free(rq.rows);
    stub_mode = STUB_RECORDS;
}

static void s7_4_partial_record_range(void)
{
    printf("\n7.4 a PARTIAL record range must not reach cap+1 — the overflow "
           "assertion depends on both records arriving\n");

    /* Only record A (1024 pids). If the stub, the window or the reader ever
     * dropped the second record, section 2 would be asserting 1024 == 1025
     * and going red rather than passing vacuously. */
    struct pgwt_summary_accum *a = rec_new(1000000000ULL);
    for (int k = 0; k < CAP_SESSIONS; k++) {
        a->sessions[k].pid = BASE_PID + (uint32_t)k;
        a->sessions[k].db_time_ns = (uint64_t)(k + 1) * MS;
        a->sessions[k].top_wait_id = WAIT_A;
        a->sessions[k].top_wait_ns = (uint64_t)(k + 1) * MS;
    }
    a->num_sessions = CAP_SESSIONS;
    struct pgwt_summary_accum *recs[1] = { a };
    stub_set(recs, 1);

    struct pgwt_filter f;
    memset(&f, 0, sizeof(f));
    struct pgwt_sessions_result r;
    pgwt_compute_top_sessions_from_summaries("ignored", 0, 0, &f, 0, &r);
    CHECK_INT(stub_visited, 1, "one record delivered");
    CHECK_INT(r.num_rows, CAP_SESSIONS,
              "one record yields exactly the cap, never cap+1");
    free(r.rows);
    free(a);
}

static void s7_5_dense_consecutive_keys(void)
{
    printf("\n7.5 key 0 and a dense consecutive key run (probe clustering)\n");

    /* Consecutive pids and query ids are the realistic case and must not all
     * land in one probe run. Includes pid 0 and query id 1. */
    struct fixture fx = {0};
    uint64_t ts = TS_BASE;
    const int N = 1500;
    for (int k = 0; k < N; k++) {
        fx_push(&fx, (uint32_t)k, WAIT_A, WAIT_B, ts, MS,
                1 + (uint64_t)k);
        ts += 2 * MS;
    }
    struct pgwt_filter f;
    memset(&f, 0, sizeof(f));
    struct pgwt_sessions_result rs;
    pgwt_compute_top_sessions(fx.ev, fx.n, &f, 0, 0, 0, &rs);
    CHECK_INT(rs.num_rows, N, "1500 consecutive pids including 0 all present");
    free(rs.rows);

    struct pgwt_queries_result rq;
    pgwt_compute_top_queries(fx.ev, fx.n, &f, 0, 0, 0, &rq);
    CHECK_INT(rq.num_rows, N, "1500 consecutive query ids all present");
    free(rq.rows);

    struct pgwt_fingerprint_result rf;
    pgwt_compute_fingerprints(fx.ev, fx.n, &f, &rf);
    CHECK_INT(rf.num_rows, N, "1500 consecutive query ids: fingerprint rows");
    free(rf.rows);

    free(fx.ev);
}

static void s7_6_alloc_failure_refuses(void)
{
    printf("\n7.6 allocation failure at every new growth point: ABSENT, "
           "never short-but-complete\n");

    struct pgwt_filter f;
    memset(&f, 0, sizeof(f));

    /* --- raw top sessions --- */
    struct fixture sfx = build_sessions(N_SESSIONS, 1);
    static const char *sess_points[] = { "sessions_entries", "pid_index_grow" };
    for (unsigned c = 0; c < 2; c++) {
        setenv("PGWT_TEST_ALLOC_FAIL", sess_points[c], 1);
        struct pgwt_sessions_result r;
        pgwt_compute_top_sessions(sfx.ev, sfx.n, &f, 0, 0, 0, &r);
        unsetenv("PGWT_TEST_ALLOC_FAIL");
        char msg[200];
        snprintf(msg, sizeof(msg),
                 "sessions: %s failure sets failed=1 and yields no rows",
                 sess_points[c]);
        CHECK(r.failed == 1 && r.num_rows == 0 && r.rows == NULL, msg);
        free(r.rows);
    }

    /* --- raw top queries --- */
    struct fixture qfx = build_queries(N_QUERIES);
    static const char *qry_points[] = { "queries_entries", "qid_index_grow" };
    for (unsigned c = 0; c < 2; c++) {
        setenv("PGWT_TEST_ALLOC_FAIL", qry_points[c], 1);
        struct pgwt_queries_result r;
        pgwt_compute_top_queries(qfx.ev, qfx.n, &f, 0, 0, 0, &r);
        unsetenv("PGWT_TEST_ALLOC_FAIL");
        char msg[200];
        snprintf(msg, sizeof(msg),
                 "queries: %s failure sets failed=1 and yields no rows",
                 qry_points[c]);
        CHECK(r.failed == 1 && r.num_rows == 0 && r.rows == NULL, msg);
        free(r.rows);
    }

    /* --- fingerprints --- */
    setenv("PGWT_TEST_ALLOC_FAIL", "fingerprint_entries", 1);
    struct pgwt_fingerprint_result rf;
    pgwt_compute_fingerprints(qfx.ev, qfx.n, &f, &rf);
    unsetenv("PGWT_TEST_ALLOC_FAIL");
    CHECK(rf.failed == 1 && rf.num_rows == 0 && rf.rows == NULL,
          "fingerprints: fingerprint_entries failure sets failed=1, no rows");
    free(rf.rows);

    /* --- variants --- */
    struct fixture vfx = build_variants(N_VARIANTS);
    setenv("PGWT_TEST_ALLOC_FAIL", "variant_entries", 1);
    struct pgwt_variants_result rv;
    pgwt_compute_variants(vfx.ev, vfx.n, NULL, 100000, PGWT_PHASE_EXEC, &rv);
    unsetenv("PGWT_TEST_ALLOC_FAIL");
    CHECK(rv.failed == 1 && rv.num_variants == 0 && rv.variants == NULL,
          "variants: variant_entries failure sets failed=1, no variant list");
    free(rv.variants);

    /* --- the two summary paths --- */
    struct pgwt_summary_accum *a = rec_new(1000000000ULL);
    for (int k = 0; k < CAP_SESSIONS; k++) {
        a->sessions[k].pid = BASE_PID + (uint32_t)k;
        a->sessions[k].db_time_ns = (uint64_t)(k + 1) * MS;
        a->sessions[k].top_wait_id = WAIT_A;
        a->sessions[k].top_wait_ns = (uint64_t)(k + 1) * MS;
    }
    for (int k = 0; k < CAP_QUERIES; k++) {
        a->queries[k].query_id = BASE_QID + (uint64_t)k;
        a->queries[k].count = 1;
        a->queries[k].total_ns = (uint64_t)(k + 1) * MS;
        a->queries[k].top_wait_id = WAIT_A;
        a->queries[k].top_wait_ns = (uint64_t)(k + 1) * MS;
    }
    a->num_sessions = CAP_SESSIONS;
    a->num_queries = CAP_QUERIES;
    struct pgwt_summary_accum *recs[1] = { a };
    stub_set(recs, 1);

    setenv("PGWT_TEST_ALLOC_FAIL", "ts_summary_entries", 1);
    struct pgwt_sessions_result rss;
    pgwt_compute_top_sessions_from_summaries("ignored", 0, 0, &f, 0, &rss);
    unsetenv("PGWT_TEST_ALLOC_FAIL");
    CHECK(rss.failed == 1 && rss.num_rows == 0 && rss.rows == NULL,
          "summary sessions: ts_summary_entries failure sets failed=1, "
          "no rows");
    free(rss.rows);

    setenv("PGWT_TEST_ALLOC_FAIL", "tq_summary_entries", 1);
    struct pgwt_queries_result rqs;
    pgwt_compute_top_queries_from_summaries("ignored", 0, 0, &f, 0, &rqs);
    unsetenv("PGWT_TEST_ALLOC_FAIL");
    CHECK(rqs.failed == 1 && rqs.num_rows == 0 && rqs.rows == NULL,
          "summary queries: tq_summary_entries failure sets failed=1, no rows");
    free(rqs.rows);

    /* --- the control: an injection that does nothing must change nothing,
     *     so the eight refusals above cannot be passing for some other
     *     reason (a missing env var, a typo, a compiled-out hook). --- */
    setenv("PGWT_TEST_ALLOC_FAIL", "a_point_that_does_not_exist", 1);
    struct pgwt_sessions_result ok1;
    pgwt_compute_top_sessions(sfx.ev, sfx.n, &f, 0, 0, 0, &ok1);
    struct pgwt_queries_result ok2;
    pgwt_compute_top_queries(qfx.ev, qfx.n, &f, 0, 0, 0, &ok2);
    struct pgwt_fingerprint_result ok3;
    pgwt_compute_fingerprints(qfx.ev, qfx.n, &f, &ok3);
    struct pgwt_variants_result ok4;
    pgwt_compute_variants(vfx.ev, vfx.n, NULL, 100000, PGWT_PHASE_EXEC, &ok4);
    struct pgwt_sessions_result ok5;
    pgwt_compute_top_sessions_from_summaries("ignored", 0, 0, &f, 0, &ok5);
    struct pgwt_queries_result ok6;
    pgwt_compute_top_queries_from_summaries("ignored", 0, 0, &f, 0, &ok6);
    unsetenv("PGWT_TEST_ALLOC_FAIL");
    CHECK(!ok1.failed && ok1.num_rows == N_SESSIONS,
          "unknown injection point: sessions still complete");
    CHECK(!ok2.failed && ok2.num_rows == N_QUERIES,
          "unknown injection point: queries still complete");
    CHECK(!ok3.failed && ok3.num_rows == N_QUERIES,
          "unknown injection point: fingerprints still complete");
    CHECK(!ok4.failed && ok4.num_variants == N_VARIANTS,
          "unknown injection point: variants still complete");
    CHECK(!ok5.failed && ok5.num_rows == CAP_SESSIONS,
          "unknown injection point: summary sessions still complete");
    CHECK(!ok6.failed && ok6.num_rows == CAP_QUERIES,
          "unknown injection point: summary queries still complete");
    free(ok1.rows); free(ok2.rows); free(ok3.rows); free(ok4.variants);
    free(ok5.rows); free(ok6.rows);

    free(a);
    free(sfx.ev); free(qfx.ev); free(vfx.ev);
}

static void s7_7_absent_not_wrong(void)
{
    printf("\n7.7 the thing being checked is ABSENT rather than wrong\n");

    struct pgwt_filter f;
    memset(&f, 0, sizeof(f));

    struct pgwt_sessions_result rs;
    pgwt_compute_top_sessions(NULL, 0, &f, 0, 0, 0, &rs);
    CHECK(!rs.failed && rs.num_rows == 0,
          "no events: sessions empty, not failed");
    free(rs.rows);

    struct pgwt_queries_result rq;
    pgwt_compute_top_queries(NULL, 0, &f, 0, 0, 0, &rq);
    CHECK(!rq.failed && rq.num_rows == 0,
          "no events: queries empty, not failed");
    free(rq.rows);

    struct pgwt_fingerprint_result rf;
    pgwt_compute_fingerprints(NULL, 0, &f, &rf);
    CHECK(!rf.failed && rf.num_rows == 0,
          "no events: fingerprints empty, not failed");
    free(rf.rows);

    struct pgwt_variants_result rv;
    pgwt_compute_variants(NULL, 0, NULL, 100, PGWT_PHASE_EXEC, &rv);
    CHECK(!rv.failed && rv.num_variants == 0,
          "no events: variants empty, not failed");
    free(rv.variants);

    /* A query id present only on records the visitor refuses (a class filter
     * the per-second data cannot honour) must come back as NO ROWS, not as
     * unfiltered numbers under a filtered label — the #318 rule, re-checked
     * here because this commit rewrote that visitor's table. */
    struct pgwt_summary_accum *a = rec_new(1000000000ULL);
    a->sessions[0].pid = BASE_PID;
    a->sessions[0].db_time_ns = 5 * MS;
    a->sessions[0].top_wait_id = WAIT_A;
    a->sessions[0].top_wait_ns = 5 * MS;
    a->num_sessions = 1;
    struct pgwt_summary_accum *recs[1] = { a };
    stub_set(recs, 1);

    struct pgwt_filter fc;
    memset(&fc, 0, sizeof(fc));
    snprintf(fc.class_name, sizeof(fc.class_name), "Lock");
    struct pgwt_sessions_result rsf;
    pgwt_compute_top_sessions_from_summaries("ignored", 0, 0, &fc, 0, &rsf);
    CHECK(!rsf.failed && rsf.num_rows == 0,
          "a class filter the summary records cannot honour yields NO rows "
          "(#318 refusal still intact)");
    free(rsf.rows);
    free(a);
}

int main(void)
{
    printf("=== test_probe_bounds (#327, #347) ===\n");
    printf("fixtures: sessions %d (cap %d), queries %d (cap %d), "
           "variants %d (cap %d)\n",
           N_SESSIONS, CAP_SESSIONS, N_QUERIES, CAP_QUERIES,
           N_VARIANTS, CAP_VARIANTS);

    /* A fixture trimmed back to the cap makes every section below pass
     * instantly while saying nothing about the defect — at the cap the
     * pre-fix probe still terminates. Refuse rather than approve. */
    if (N_SESSIONS <= CAP_SESSIONS || N_QUERIES <= CAP_QUERIES ||
        N_VARIANTS <= CAP_VARIANTS) {
        printf("REFUSE: a fixture does not exceed its pre-fix cap, so it "
               "cannot see the defect.\n");
        return 1;
    }
    /* Likewise if the two-step pattern generator cannot actually produce
     * cap+1 distinct patterns: 65 ids give 65*64 = 4160 ordered pairs. */
    if (65 * 64 < N_VARIANTS) {
        printf("REFUSE: the pattern generator cannot reach %d distinct "
               "patterns.\n", N_VARIANTS);
        return 1;
    }

    section1_sessions_raw();
    section2_sessions_summary();
    section3_queries_raw();
    section4_queries_summary();
    section5a_fingerprints();
    section5b_variants();
    section6_1_fingerprint_rekey();
    section6_2_session_pid_zero_rekey();
    section6_3_aas_is_immune();
    s7_1_cap_sized_fixture_is_blind();
    s7_2_identical_durations_are_blind_per_row();
    s7_3_summary_stub_delivers_nothing();
    s7_4_partial_record_range();
    s7_5_dense_consecutive_keys();
    s7_6_alloc_failure_refuses();
    s7_7_absent_not_wrong();

    printf("\n=== %d checks, %d failures ===\n", checks, failures);
    return failures ? 1 : 0;
}
