/* test_pid_cap.c — #275: the two per-PID execution-lifecycle tables that
 * silently dropped every PID past the 512th.
 *
 * THE DEFECT. `handle_top_queries` (src/server.c) kept
 * `struct {...} pid_st[512]` with a linear probe and
 * `if (pi < 0) continue;`; `pgwt_compute_variants` (src/compute.c) kept
 * `struct pid_exec_state pids[MAX_PIDS]` with `if (num_pids >= MAX_PIDS)
 * continue;`. Both dropped the 513th distinct pid of the window and every
 * one after it, with no flag in the response, no line in the log and no
 * error. The count of DISTINCT pids in a window is driven by connection
 * CHURN, not by concurrency, so 512 is ordinary; the shortfall grows with
 * churn and always in the direction of looking healthier.
 *
 * Both tables are now unbounded hash tables (src/pid_index.h). The Queries
 * tab's scan moved out of server.c into pgwt_compute_query_lifecycle so
 * this file can call THE REAL CODE instead of a copy of it — a copy would
 * have gone green while server.c stayed broken.
 *
 * WHY THE FIXTURE LOOKS LIKE THIS. 600 distinct pids, interleaved
 * round-robin so the 88 that the old cap dropped are spread through the
 * stream rather than sitting at the end, and each pid gets a DISTINCT
 * execution duration so that dropping any of them moves the p95 and not
 * only the count. Identical durations, or pids arriving in blocks, are
 * both fixtures that can be satisfied while the bug is present —
 * sections 4.1 and 4.2 assert that blindness explicitly so nobody
 * simplifies back into it.
 *
 * SECTION 4 IS ABOUT FALSE NEGATIVES — every way this check could pass
 * without checking anything: a fixture under the cap, a fixture whose
 * assertion shape cannot see a short count, identical durations that hide
 * it from the p95, pid 0 (a legal key the old sentinel-free array handled
 * by accident), consecutive pids colliding in one probe run, a pid whose
 * first event is not a marker, and — the one that matters most — an
 * allocation failure, where the answer must be ABSENT (failed=1) and never
 * short-but-complete-looking. The allocation cases use PGWT_TEST_ALLOC_FAIL
 * (the same shape as src/server.c's test_load_alloc_failure) and also run
 * an unknown injection point, so an injection that silently did nothing
 * cannot be what made them pass.
 *
 * Runs anywhere: in-memory arrays, synthetic timestamps, no clock, no I/O,
 * no threads, no ordering assumptions. Nothing here can be flaky.
 */
#include "compute.h"
#include "percentile.h"
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

#define CHECK_INT(got, want, msg) do {                               \
    checks++;                                                        \
    long _g = (long)(got), _w = (long)(want);                        \
    if (_g != _w) { failures++;                                      \
        printf("  FAIL: %s — got %ld, want %ld (%s:%d)\n", msg,      \
               _g, _w, __FILE__, __LINE__); }                        \
    else printf("  ok: %s (%ld)\n", msg, _g);                        \
} while (0)

#define CHECK_DBL(got, want, tol, msg) do {                          \
    checks++;                                                        \
    double _g = (double)(got), _w = (double)(want);                  \
    if (!(fabs(_g - _w) <= (tol))) { failures++;                     \
        printf("  FAIL: %s — got %.9f, want %.9f (%s:%d)\n", msg,    \
               _g, _w, __FILE__, __LINE__); }                        \
    else printf("  ok: %s (%.9f)\n", msg, _g);                       \
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

/* ── Fixture ──────────────────────────────────────────────────────────────
 * `npids` distinct pids, each running EXECS_PER_PID executions of query id
 * QID. Pid k's executions last (k + 1) microseconds — distinct per pid, so
 * the p95 over the whole window is a closed form and MOVES when any pid is
 * dropped. Markers are emitted round-robin across pids, so a cap on
 * first-sight order drops a scattered subset, not a tail.
 *
 * Each execution is EXEC_START, one ordinary wait event, EXEC_END — the
 * wait event is there so pgwt_compute_variants has a non-empty pattern and
 * so the lifecycle scan has non-marker traffic to step over. */
#define NPIDS         600      /* > 512: the whole point */
#define EXECS_PER_PID 3
#define QID           0x5151515151ULL
#define BASE_PID      40000u
#define WAIT_EVENT    0x01000001u   /* a real non-marker wait_event_info */

struct fixture {
    struct pgwt_trace_event *ev;
    int n;
};

static void fx_push(struct fixture *fx, int *cap, uint32_t pid, uint32_t evid,
                    uint64_t ts, uint64_t dur, uint64_t qid)
{
    if (fx->n >= *cap) {
        *cap = *cap ? *cap * 2 : 1024;
        struct pgwt_trace_event *tmp =
            realloc(fx->ev, (size_t)*cap * sizeof(*fx->ev));
        if (!tmp) { fprintf(stderr, "fixture OOM\n"); exit(2); }
        fx->ev = tmp;
    }
    struct pgwt_trace_event *e = &fx->ev[fx->n++];
    memset(e, 0, sizeof(*e));
    e->pid = pid;
    e->old_event = evid;
    e->new_event = evid;
    e->timestamp_ns = ts;
    e->duration_ns = dur;
    e->query_id = qid;
    e->cpu_ns = PGWT_CPU_NS_UNKNOWN;
}

static struct fixture build_fixture(int npids, int distinct_durations)
{
    struct fixture fx = {0};
    int cap = 0;
    uint64_t ts = 1000000000ULL;   /* 1 s, arbitrary */

    for (int r = 0; r < EXECS_PER_PID; r++) {
        for (int k = 0; k < npids; k++) {
            uint32_t pid = BASE_PID + (uint32_t)k;
            uint64_t dur_ns = distinct_durations
                            ? (uint64_t)(k + 1) * 1000ULL   /* (k+1) us */
                            : 1000ULL;
            fx_push(&fx, &cap, pid, PGWT_MARKER_EXEC_START, ts, 0, QID);
            fx_push(&fx, &cap, pid, WAIT_EVENT, ts + dur_ns / 2,
                    dur_ns / 2, QID);
            fx_push(&fx, &cap, pid, PGWT_MARKER_EXEC_END, ts + dur_ns, 0,
                    QID);
            ts += dur_ns + 1000ULL;
        }
    }
    return fx;
}

/* The exact p95 for the distinct-duration fixture: npids*EXECS_PER_PID
 * samples, pid k contributing EXECS_PER_PID copies of (k+1) us in ms,
 * sorted ascending, picked at index (int)(n * 0.95) — the index expression
 * server.c uses. */
static double expected_p95_ms(int npids)
{
    int n = npids * EXECS_PER_PID;
    int idx = (int)(n * 0.95);
    int k = idx / EXECS_PER_PID;          /* 0-based pid whose value lands */
    return (double)(k + 1) * 1000.0 / 1e6;
}

/* ── Section 1: the Queries tab scan (handle_top_queries → compute.c) ──── */

static void section1_lifecycle(void)
{
    printf("\n1. pgwt_compute_query_lifecycle over %d distinct pids\n", NPIDS);

    struct fixture fx = build_fixture(NPIDS, 1);
    struct pgwt_lifecycle_result r;
    pgwt_compute_query_lifecycle(fx.ev, fx.n, &r);

    CHECK(!r.failed, "scan did not fail");
    struct pgwt_qid_lifecycle *lc = pgwt_lifecycle_lookup(&r, QID);
    CHECK(lc != NULL, "query id present in the lifecycle table");
    if (lc) {
        /* THE assertion: every pid's executions counted, not the first 512.
         * Pre-#275 this was 512 * EXECS_PER_PID = 1536. */
        CHECK_INT(lc->exec_count, NPIDS * EXECS_PER_PID,
                  "exec_count counts every pid's executions");
        CHECK_INT(lc->exec_nsamples, NPIDS * EXECS_PER_PID,
                  "every execution contributed a duration sample");

        /* exec_total_ms: EXECS_PER_PID * sum_{k=1..NPIDS} k us, in ms. */
        double want_total = (double)EXECS_PER_PID *
            ((double)NPIDS * (NPIDS + 1) / 2.0) * 1000.0 / 1e6;
        CHECK_DBL(lc->exec_total_ms, want_total, 1e-6,
                  "exec_total_ms sums every pid's executions");

        /* p95 over the real sample set. The dropped pids held the LARGEST
         * durations, so the capped answer's p95 was lower. */
        pgwt_sort_doubles_asc(lc->exec_times, lc->exec_nsamples);
        double p95 = lc->exec_times[(int)(lc->exec_nsamples * 0.95)];
        CHECK_DBL(p95, expected_p95_ms(NPIDS), 1e-9,
                  "p95_exec_ms is the percentile of the COMPLETE sample set");
    }
    pgwt_lifecycle_free(&r);
    free(fx.ev);
}

/* ── Section 2: pgwt_compute_variants ─────────────────────────────────── */

static void section2_variants(void)
{
    printf("\n2. pgwt_compute_variants over %d distinct pids\n", NPIDS);

    struct fixture fx = build_fixture(NPIDS, 1);
    struct pgwt_variants_result res;
    pgwt_compute_variants(fx.ev, fx.n, NULL, 50, PGWT_PHASE_EXEC, &res);

    CHECK(!res.failed, "variants did not fail");
    /* Pre-#275 this was 512 * EXECS_PER_PID = 1536. */
    CHECK_INT(res.total_executions, NPIDS * EXECS_PER_PID,
              "total_executions counts every pid's executions");

    int summed = 0;
    for (int i = 0; i < res.num_variants; i++)
        summed += res.variants[i].exec_count;
    CHECK_INT(summed, NPIDS * EXECS_PER_PID,
              "the variant rows account for every execution");
    free(res.variants);
    free(fx.ev);
}

/* ── Section 3: the query-id table has no bound either ────────────────── */

static void section3_many_query_ids(void)
{
    printf("\n3. 4000 distinct query ids (pre-#275: an unterminated probe)\n");

    /* The pre-#275 query-id table was a FIXED 1024 slots probed with
     * `while (used && query_id != qid) h = (h+1) & MASK;`. At 1024 distinct
     * ids every slot is used and that loop never terminates: the response
     * never arrives at all. 4000 ids is comfortably past it. Not a widening
     * of #275 — it is the same table, in the code #275 moves, and leaving a
     * reachable infinite loop inside a function being rewritten is not an
     * option. */
    struct fixture fx = {0};
    int cap = 0;
    uint64_t ts = 1000000000ULL;
    const int NQ = 4000;
    for (int q = 0; q < NQ; q++) {
        uint32_t pid = BASE_PID + (uint32_t)(q % 64);
        uint64_t qid = 0x1000ULL + (uint64_t)q;
        fx_push(&fx, &cap, pid, PGWT_MARKER_EXEC_START, ts, 0, qid);
        fx_push(&fx, &cap, pid, PGWT_MARKER_EXEC_END, ts + 2000, 0, qid);
        ts += 10000;
    }

    struct pgwt_lifecycle_result r;
    pgwt_compute_query_lifecycle(fx.ev, fx.n, &r);
    CHECK(!r.failed, "scan over 4000 query ids completed");
    CHECK_INT(r.n, NQ, "every distinct query id got a slot");
    int found = 0;
    for (int q = 0; q < NQ; q++) {
        const struct pgwt_qid_lifecycle *lc =
            pgwt_lifecycle_lookup(&r, 0x1000ULL + (uint64_t)q);
        if (lc && lc->exec_count == 1) found++;
    }
    CHECK_INT(found, NQ, "every query id is retrievable with its count");
    pgwt_lifecycle_free(&r);
    free(fx.ev);
}

/* ── Section 4: false negatives ───────────────────────────────────────── */

static void s4_1_blind_assertion_shapes(void)
{
    printf("\n4.1 assertion shapes that CANNOT see the bug (pinned as blind)\n");

    /* With the cap in place the first 512 pids still produce executions, so
     * `exec_count > 0`, `num_variants > 0` and "the query id is present"
     * are all satisfied while the answer is 15% short. Simulate the capped
     * answer by running the fixture at exactly 512 pids and show those
     * shapes do not distinguish it from the 600-pid answer. */
    struct fixture capped = build_fixture(512, 1);
    struct pgwt_lifecycle_result rc;
    pgwt_compute_query_lifecycle(capped.ev, capped.n, &rc);
    const struct pgwt_qid_lifecycle *lcc = pgwt_lifecycle_lookup(&rc, QID);

    struct fixture full = build_fixture(NPIDS, 1);
    struct pgwt_lifecycle_result rf;
    pgwt_compute_query_lifecycle(full.ev, full.n, &rf);
    const struct pgwt_qid_lifecycle *lcf = pgwt_lifecycle_lookup(&rf, QID);

    CHECK(lcc && lcf, "both fixtures produce a lifecycle entry");
    if (lcc && lcf) {
        CHECK(lcc->exec_count > 0 && lcf->exec_count > 0,
              "`exec_count > 0` holds for BOTH — a blind assertion shape");
        CHECK(lcc->exec_count != lcf->exec_count,
              "the exact-count assertion this file uses DOES separate them");
    }
    pgwt_lifecycle_free(&rc);
    pgwt_lifecycle_free(&rf);
    free(capped.ev);
    free(full.ev);
}

static void s4_2_identical_durations_blind_to_p95(void)
{
    printf("\n4.2 identical durations: the p95 cannot see a dropped pid\n");

    struct fixture a = build_fixture(512, 0);
    struct fixture b = build_fixture(NPIDS, 0);
    struct pgwt_lifecycle_result ra, rb;
    pgwt_compute_query_lifecycle(a.ev, a.n, &ra);
    pgwt_compute_query_lifecycle(b.ev, b.n, &rb);
    struct pgwt_qid_lifecycle *la = pgwt_lifecycle_lookup(&ra, QID);
    struct pgwt_qid_lifecycle *lb = pgwt_lifecycle_lookup(&rb, QID);
    CHECK(la && lb, "both identical-duration fixtures produce an entry");
    if (la && lb) {
        pgwt_sort_doubles_asc(la->exec_times, la->exec_nsamples);
        pgwt_sort_doubles_asc(lb->exec_times, lb->exec_nsamples);
        double pa = la->exec_times[(int)(la->exec_nsamples * 0.95)];
        double pb = lb->exec_times[(int)(lb->exec_nsamples * 0.95)];
        CHECK_DBL(pa, pb, 1e-12,
                  "identical durations give the SAME p95 with and without "
                  "the dropped pids — why section 1 uses distinct ones");
    }
    pgwt_lifecycle_free(&ra);
    pgwt_lifecycle_free(&rb);
    free(a.ev);
    free(b.ev);
}

static void s4_3_under_the_cap_is_unchanged(void)
{
    printf("\n4.3 a 512-pid window is unchanged by the fix\n");

    /* The fix must not move any number for windows the old code already
     * handled — otherwise a green suite says nothing about whether the
     * change was safe. 512 is the largest window the old code got right. */
    struct fixture fx = build_fixture(512, 1);
    struct pgwt_lifecycle_result r;
    pgwt_compute_query_lifecycle(fx.ev, fx.n, &r);
    struct pgwt_qid_lifecycle *lc = pgwt_lifecycle_lookup(&r, QID);
    CHECK(lc != NULL, "512-pid window produces an entry");
    if (lc) {
        CHECK_INT(lc->exec_count, 512 * EXECS_PER_PID,
                  "512 pids: exec_count unchanged by the fix");
        pgwt_sort_doubles_asc(lc->exec_times, lc->exec_nsamples);
        CHECK_DBL(lc->exec_times[(int)(lc->exec_nsamples * 0.95)],
                  expected_p95_ms(512), 1e-9,
                  "512 pids: p95 unchanged by the fix");
    }
    pgwt_lifecycle_free(&r);
    free(fx.ev);
}

static void s4_4_pid_zero_and_collisions(void)
{
    printf("\n4.4 pid 0 and a dense consecutive pid run\n");

    /* pid 0 is a legal key here: the input is a trace file, not a promise.
     * The map decides occupancy with a `used` byte precisely so 0 is not a
     * sentinel. Consecutive pids are the realistic case and must not all
     * land in one probe run (hence the Fibonacci scramble). */
    struct fixture fx = {0};
    int cap = 0;
    uint64_t ts = 1000000000ULL;
    const int N = 1500;
    for (int k = 0; k < N; k++) {
        uint32_t pid = (uint32_t)k;          /* includes pid 0 */
        fx_push(&fx, &cap, pid, PGWT_MARKER_EXEC_START, ts, 0, QID);
        fx_push(&fx, &cap, pid, PGWT_MARKER_EXEC_END, ts + 5000, 0, QID);
        ts += 10000;
    }
    struct pgwt_lifecycle_result r;
    pgwt_compute_query_lifecycle(fx.ev, fx.n, &r);
    const struct pgwt_qid_lifecycle *lc = pgwt_lifecycle_lookup(&r, QID);
    CHECK(lc != NULL, "dense pid run produces an entry");
    if (lc)
        CHECK_INT(lc->exec_count, N,
                  "pid 0 and 1499 consecutive pids all counted");
    pgwt_lifecycle_free(&r);
    free(fx.ev);
}

static void s4_5_variants_pid_first_seen_on_a_wait(void)
{
    printf("\n4.5 variants: a pid whose first event is not a marker\n");

    /* pgwt_compute_variants now creates per-pid state only on marker_start.
     * That is behaviour-identical only if a pid first seen on an ordinary
     * event still works when its EXEC_START arrives later. If the narrowing
     * were wrong, this execution would vanish. */
    struct fixture fx = {0};
    int cap = 0;
    uint64_t ts = 1000000000ULL;
    fx_push(&fx, &cap, 7777, WAIT_EVENT, ts, 500, QID);         /* no state */
    fx_push(&fx, &cap, 7777, PGWT_MARKER_EXEC_START, ts + 1000, 0, QID);
    fx_push(&fx, &cap, 7777, WAIT_EVENT, ts + 2000, 1000, QID);
    fx_push(&fx, &cap, 7777, PGWT_MARKER_EXEC_END, ts + 3000, 0, QID);

    struct pgwt_variants_result res;
    pgwt_compute_variants(fx.ev, fx.n, NULL, 10, PGWT_PHASE_EXEC, &res);
    CHECK(!res.failed, "variants did not fail");
    CHECK_INT(res.total_executions, 1,
              "the execution is found even though the pid was first seen "
              "on a wait event");
    /* And the pre-EXEC_START wait must NOT be in the pattern: the old code
     * did not count it either (ps->active was 0). */
    if (res.num_variants > 0) {
        CHECK_INT(res.variants[0].num_steps, 1,
                  "exactly one step: the pre-EXEC_START wait is excluded");
        CHECK_INT(res.variants[0].steps[0].event_id, WAIT_EVENT,
                  "the in-execution wait is the pattern's only step");
    }
    free(res.variants);
    free(fx.ev);
}

/* Allocation-failure injection. Each case asserts the result is a REFUSAL,
 * and an unknown injection point is run too, so an injection that silently
 * did nothing cannot be what made them pass. */
static void s4_6_alloc_failure_refuses(void)
{
    printf("\n4.6 allocation failure: ABSENT, never short-but-complete\n");

    struct fixture fx = build_fixture(NPIDS, 1);

    static const struct { const char *point; const char *what; } cases[] = {
        { "pid_index_grow",      "pid hash growth" },
        { "lifecycle_pid_state", "per-pid state array growth" },
        { "lifecycle_qid_grow",  "query-id table growth" },
    };

    for (unsigned c = 0; c < sizeof(cases) / sizeof(cases[0]); c++) {
        setenv("PGWT_TEST_ALLOC_FAIL", cases[c].point, 1);
        struct pgwt_lifecycle_result r;
        pgwt_compute_query_lifecycle(fx.ev, fx.n, &r);
        unsetenv("PGWT_TEST_ALLOC_FAIL");

        char msg[200];
        snprintf(msg, sizeof(msg), "%s failure sets failed=1", cases[c].what);
        CHECK(r.failed == 1, msg);
        snprintf(msg, sizeof(msg), "%s failure yields NO partial table",
                 cases[c].what);
        CHECK(r.slots == NULL && r.n == 0, msg);
        pgwt_lifecycle_free(&r);
    }

    /* The injection must be able to do nothing too — otherwise the three
     * cases above could be passing for an unrelated reason. */
    setenv("PGWT_TEST_ALLOC_FAIL", "a_point_that_does_not_exist", 1);
    struct pgwt_lifecycle_result ok;
    pgwt_compute_query_lifecycle(fx.ev, fx.n, &ok);
    unsetenv("PGWT_TEST_ALLOC_FAIL");
    CHECK(!ok.failed, "an unknown injection point changes nothing "
                      "(so the failures above came from the named points)");
    const struct pgwt_qid_lifecycle *lc = pgwt_lifecycle_lookup(&ok, QID);
    CHECK(lc && lc->exec_count == NPIDS * EXECS_PER_PID,
          "...and the uninjected answer is still complete");
    pgwt_lifecycle_free(&ok);

    /* Same for variants. */
    setenv("PGWT_TEST_ALLOC_FAIL", "variants_pid_state", 1);
    struct pgwt_variants_result vres;
    pgwt_compute_variants(fx.ev, fx.n, NULL, 50, PGWT_PHASE_EXEC, &vres);
    unsetenv("PGWT_TEST_ALLOC_FAIL");
    CHECK(vres.failed == 1, "variants: state-array failure sets failed=1");
    CHECK(vres.num_variants == 0 && vres.variants == NULL,
          "variants: no partial variant list on allocation failure");
    free(vres.variants);

    setenv("PGWT_TEST_ALLOC_FAIL", "pid_index_grow", 1);
    struct pgwt_variants_result vres2;
    pgwt_compute_variants(fx.ev, fx.n, NULL, 50, PGWT_PHASE_EXEC, &vres2);
    unsetenv("PGWT_TEST_ALLOC_FAIL");
    CHECK(vres2.failed == 1, "variants: pid hash failure sets failed=1");
    free(vres2.variants);

    free(fx.ev);
}

static void s4_7_empty_and_absent_inputs(void)
{
    printf("\n4.7 the thing being checked is ABSENT, not wrong\n");

    struct pgwt_lifecycle_result r;
    pgwt_compute_query_lifecycle(NULL, 0, &r);
    CHECK(!r.failed && r.n == 0 && pgwt_lifecycle_lookup(&r, QID) == NULL,
          "no events: empty table, no failure, lookup returns NULL");
    pgwt_lifecycle_free(&r);

    /* Markers for a query id that never completes an execution: the id must
     * NOT appear, so the Queries tab omits exec_count rather than reporting
     * 0 executions as if measured. */
    struct fixture fx = {0};
    int cap = 0;
    fx_push(&fx, &cap, 4242, PGWT_MARKER_EXEC_START, 1000000000ULL, 0, QID);
    pgwt_compute_query_lifecycle(fx.ev, fx.n, &r);
    CHECK(pgwt_lifecycle_lookup(&r, QID) == NULL,
          "an unfinished execution creates no entry (absent, not zero)");
    pgwt_lifecycle_free(&r);
    free(fx.ev);

    struct pgwt_variants_result v;
    pgwt_compute_variants(NULL, 0, NULL, 10, PGWT_PHASE_EXEC, &v);
    CHECK(!v.failed && v.num_variants == 0,
          "variants over no events: empty, not failed");
    free(v.variants);
}

int main(void)
{
    printf("=== test_pid_cap (#275) ===\n");
    printf("NPIDS=%d (the pre-fix cap was 512)\n", NPIDS);
    if (NPIDS <= 512) {
        printf("REFUSE: the fixture does not exceed the old 512 cap, so it "
               "cannot see the defect.\n");
        return 1;
    }

    section1_lifecycle();
    section2_variants();
    section3_many_query_ids();
    s4_1_blind_assertion_shapes();
    s4_2_identical_durations_blind_to_p95();
    s4_3_under_the_cap_is_unchanged();
    s4_4_pid_zero_and_collisions();
    s4_5_variants_pid_first_seen_on_a_wait();
    s4_6_alloc_failure_refuses();
    s4_7_empty_and_absent_inputs();

    printf("\n=== %d checks, %d failures ===\n", checks, failures);
    return failures ? 1 : 0;
}
