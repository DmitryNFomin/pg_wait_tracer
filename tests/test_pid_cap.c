/* test_pid_cap.c — #275: the two per-PID execution-lifecycle tables that
 * silently dropped every PID past the 512th, and #459 (section 5): the
 * per-PID lifecycle state that could hold only ONE open execution, so a
 * NESTED one hid the outer statement's whole duration.
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

/* ── Section 5: #459 — nested executions ──────────────────────────────────
 *
 * THE DEFECT. pgwt_compute_query_lifecycle kept ONE open execution
 * timestamp and ONE query id per pid:
 *
 *     if (m == EXEC_START) { st->exec_start_ns = ts; st->qid = qid; }
 *     else if (m == EXEC_END && st->exec_start_ns) { ... st->exec_start_ns = 0; }
 *     else if (m == PLAN_START) { st->plan_start_ns = ts; st->qid = qid; }
 *
 * An inner EXEC_START overwrote the outer start; the inner EXEC_END spent
 * it and zeroed the slot; the outer EXEC_END then saw exec_start_ns == 0
 * and contributed NOTHING. The Queries tab therefore reported the INNER
 * statement's duration as the whole query's Avg Exec, and under-counted
 * exec_count by the nesting depth -- while Waterfall
 * (pgwt_compute_executions, same markers, same window, a per-pid STACK
 * since #222) showed the correct outer span right beside it. PLAN_START
 * re-keyed the SAME `qid` the open execution was using, so a re-plan
 * inside an execution credited the execution to the plan's query.
 *
 * WHY THESE FIXTURES. The gate is the brief's: outer A 0-100 ms, inner B
 * 20-30 ms must yield A = 100 ms and B = 10 ms; with one shared query id,
 * count = 2, total = 110 ms, average = 55 ms (inclusive execution
 * durations, NOT additive DB Time -- the inner 10 ms is also inside the
 * outer 100 ms, and this scan's contract is per-execution timing, not a
 * time budget). 100/20/30 are chosen so no two of the four candidate
 * answers coincide: correct {10,100}; pre-fix {10} alone; "outer start
 * paired with inner end" {30}; "inner start paired with outer end" {80}.
 *
 * SECTION 5.6 IS THE BYPASS SUITE and it runs every time: three MUTANT
 * oracles live in this file (the verbatim pre-fix single-slot rule, a
 * stacks-but-shared-query-id rule, and a stacks-but-no-CMD_END-clear
 * rule) and each must DISAGREE with the real code on the fixture that
 * targets it. 5.6 also pins the one fixture that CANNOT see any of it: a
 * FLAT fixture of two disjoint executions (100 ms then 10 ms) produces
 * count = 2, total = 110 -- byte-identical to the nested gate's correct
 * answer -- so a gate written on flat executions is satisfied while every
 * mutation is present. 5.7 covers allocation failure on the new stacks
 * (ABSENT, never short), and 5.8 the cases where the thing being checked
 * is absent or malformed rather than wrong: an END with no START, an END
 * that precedes its START (an unsigned wrap that `ms < 0` cannot catch),
 * query id 0, and a marker-free stream.
 *
 * Synthetic timestamps, in-memory arrays, no clock, no I/O, no threads.
 * Nothing here can be flaky.
 */

#define MS_NS      1000000ULL
#define NEST_PID   51000u
#define QID_BOTH   0xA0A0A0A0A0ULL   /* outer and inner: the same query */
#define QID_OUTER  0xA1A1A1A1A1ULL
#define QID_INNER  0xB2B2B2B2B2ULL
#define QID_PLANQ  0xC3C3C3C3C3ULL

/* outer [0,100] ms with inner [20,30] ms strictly inside it, then the
 * CMD_END a real backend emits when it goes idle (both stacks already
 * empty here -- 5.4 is where CMD_END has something to abandon). */
static struct fixture build_nested(uint64_t outer_qid, uint64_t inner_qid)
{
    struct fixture fx = {0};
    int cap = 0;
    uint64_t t0 = 1000000000ULL;
    fx_push(&fx, &cap, NEST_PID, PGWT_MARKER_EXEC_START, t0, 0, outer_qid);
    fx_push(&fx, &cap, NEST_PID, PGWT_MARKER_EXEC_START,
            t0 + 20 * MS_NS, 0, inner_qid);
    fx_push(&fx, &cap, NEST_PID, PGWT_MARKER_EXEC_END,
            t0 + 30 * MS_NS, 0, inner_qid);
    fx_push(&fx, &cap, NEST_PID, PGWT_MARKER_EXEC_END,
            t0 + 100 * MS_NS, 0, outer_qid);
    fx_push(&fx, &cap, NEST_PID, PGWT_MARKER_CMD_END,
            t0 + 101 * MS_NS, 0, outer_qid);
    return fx;
}

/* Two DISJOINT executions, 100 ms then 10 ms, on one query id. Same
 * count, same total and the same two samples as the nested gate above --
 * which is exactly why a flat fixture proves nothing (5.6). */
static struct fixture build_flat(uint64_t qid)
{
    struct fixture fx = {0};
    int cap = 0;
    uint64_t t0 = 1000000000ULL;
    fx_push(&fx, &cap, NEST_PID, PGWT_MARKER_EXEC_START, t0, 0, qid);
    fx_push(&fx, &cap, NEST_PID, PGWT_MARKER_EXEC_END,
            t0 + 100 * MS_NS, 0, qid);
    fx_push(&fx, &cap, NEST_PID, PGWT_MARKER_EXEC_START,
            t0 + 120 * MS_NS, 0, qid);
    fx_push(&fx, &cap, NEST_PID, PGWT_MARKER_EXEC_END,
            t0 + 130 * MS_NS, 0, qid);
    fx_push(&fx, &cap, NEST_PID, PGWT_MARKER_CMD_END,
            t0 + 131 * MS_NS, 0, qid);
    return fx;
}

/* An execution on QID_OUTER spanning [0,100] ms with a PLAN on a
 * DIFFERENT query id, [10,15] ms, nested inside it -- a re-plan on a
 * cache invalidation, or a nested statement's plan. */
static struct fixture build_plan_inside_exec(void)
{
    struct fixture fx = {0};
    int cap = 0;
    uint64_t t0 = 1000000000ULL;
    fx_push(&fx, &cap, NEST_PID, PGWT_MARKER_EXEC_START, t0, 0, QID_OUTER);
    fx_push(&fx, &cap, NEST_PID, PGWT_MARKER_PLAN_START,
            t0 + 10 * MS_NS, 0, QID_PLANQ);
    fx_push(&fx, &cap, NEST_PID, PGWT_MARKER_PLAN_END,
            t0 + 15 * MS_NS, 0, QID_PLANQ);
    fx_push(&fx, &cap, NEST_PID, PGWT_MARKER_EXEC_END,
            t0 + 100 * MS_NS, 0, QID_OUTER);
    return fx;
}

/* An execution that never reaches EXEC_END (an ERROR longjmp, a cancel,
 * statement_timeout, or a dropped marker), its CMD_END, and then a
 * genuine 20 ms execution on the same pid and query. */
static struct fixture build_orphan_then_real(void)
{
    struct fixture fx = {0};
    int cap = 0;
    uint64_t t0 = 1000000000ULL;
    fx_push(&fx, &cap, NEST_PID, PGWT_MARKER_EXEC_START, t0, 0, QID_BOTH);
    fx_push(&fx, &cap, NEST_PID, PGWT_MARKER_CMD_END,
            t0 + 50 * MS_NS, 0, QID_BOTH);
    fx_push(&fx, &cap, NEST_PID, PGWT_MARKER_EXEC_START,
            t0 + 100 * MS_NS, 0, QID_BOTH);
    fx_push(&fx, &cap, NEST_PID, PGWT_MARKER_EXEC_END,
            t0 + 120 * MS_NS, 0, QID_BOTH);
    return fx;
}

/* The orphan case the CMD_END clear is actually load-bearing for: a
 * statement abandoned with no EXEC_END, its CMD_END, and then a LATER
 * statement whose own EXEC_START was lost (a ringbuf drop) but whose
 * EXEC_END arrives. With the stacks cleared at CMD_END the stray END pops
 * nothing and contributes nothing. Without the clear, it pops the DEAD
 * statement's start and credits 120 ms of someone else's command to the
 * abandoned query -- pgwt_tag_events' own CMD_END handling exists for
 * exactly this leak ("an EXEC_END lost to a ringbuf drop must not leak the
 * window across statements", src/compute.c).
 *
 * Note which fixtures are BLIND to this: because the stacks are LIFO, a
 * later statement that DOES have its own EXEC_START pops its own entry and
 * leaves the orphan harmlessly underneath -- so build_orphan_then_real()
 * above answers identically with and without the clear. Only an UNBALANCED
 * end can reach the abandoned entry. */
static struct fixture build_orphan_lost_start(void)
{
    struct fixture fx = {0};
    int cap = 0;
    uint64_t t0 = 1000000000ULL;
    fx_push(&fx, &cap, NEST_PID, PGWT_MARKER_EXEC_START, t0, 0, QID_OUTER);
    fx_push(&fx, &cap, NEST_PID, PGWT_MARKER_CMD_END,
            t0 + 50 * MS_NS, 0, QID_OUTER);
    fx_push(&fx, &cap, NEST_PID, PGWT_MARKER_EXEC_END,
            t0 + 120 * MS_NS, 0, QID_INNER);
    return fx;
}

/* Read one query's exec totals out of the real result. */
static void real_exec(struct pgwt_lifecycle_result *r, uint64_t qid,
                      int *count, double *total)
{
    struct pgwt_qid_lifecycle *lc = pgwt_lifecycle_lookup(r, qid);
    *count = lc ? lc->exec_count : 0;
    *total = lc ? lc->exec_total_ms : 0.0;
}

/* ── The mutant oracles (section 5.6) ─────────────────────────────────────
 * Each is a complete alternative rule over the same event array, so the
 * assertion "the real code disagrees with mutant M on fixture F" is the
 * negative control for M, checked on every run rather than once by hand.
 */
#define ORACLE_PREFIX     0   /* one slot + one shared qid: the pre-#459 code */
#define ORACLE_SHARED_QID 1   /* stacks, but ONE qid slot shared with PLAN */
#define ORACLE_NO_CMDEND  2   /* stacks + per-entry qid, but CMD_END ignored */

#define ORACLE_MAX_QID 64
#define ORACLE_MAX_PID 16
#define ORACLE_DEPTH   256

struct oracle_qid { uint64_t qid; int exec_count, plan_count;
                    double exec_total, plan_total; };
struct oracle_out { struct oracle_qid q[ORACLE_MAX_QID]; int n; };

static struct oracle_qid *oracle_slot(struct oracle_out *o, uint64_t qid)
{
    for (int i = 0; i < o->n; i++)
        if (o->q[i].qid == qid) return &o->q[i];
    if (o->n >= ORACLE_MAX_QID) { fprintf(stderr, "oracle qid overflow\n");
                                  exit(2); }
    struct oracle_qid *s = &o->q[o->n++];
    memset(s, 0, sizeof(*s));
    s->qid = qid;
    return s;
}

static void oracle_run(const struct pgwt_trace_event *ev, int n, int mode,
                       struct oracle_out *o)
{
    struct {
        uint32_t pid; int used;
        uint64_t exec_one, plan_one, shared_qid;          /* ORACLE_PREFIX */
        uint64_t es[ORACLE_DEPTH], eq[ORACLE_DEPTH]; int ne;
        uint64_t ps[ORACLE_DEPTH], pq[ORACLE_DEPTH]; int np;
    } st[ORACLE_MAX_PID];
    memset(st, 0, sizeof(st));
    memset(o, 0, sizeof(*o));

    for (int i = 0; i < n; i++) {
        uint32_t m = ev[i].old_event;
        if (!PGWT_IS_MARKER(m)) continue;
        int pi = -1;
        for (int k = 0; k < ORACLE_MAX_PID; k++) {
            if (st[k].used && st[k].pid == ev[i].pid) { pi = k; break; }
            if (!st[k].used) { st[k].used = 1; st[k].pid = ev[i].pid;
                               pi = k; break; }
        }
        if (pi < 0) { fprintf(stderr, "oracle pid overflow\n"); exit(2); }

        if (m == PGWT_MARKER_EXEC_START) {
            if (mode == ORACLE_PREFIX) {
                st[pi].exec_one = ev[i].timestamp_ns;
                st[pi].shared_qid = ev[i].query_id;
            } else {
                if (st[pi].ne >= ORACLE_DEPTH) { fprintf(stderr,
                        "oracle depth\n"); exit(2); }
                st[pi].es[st[pi].ne] = ev[i].timestamp_ns;
                st[pi].eq[st[pi].ne] = ev[i].query_id;
                st[pi].ne++;
                if (mode == ORACLE_SHARED_QID)
                    st[pi].shared_qid = ev[i].query_id;
            }
        } else if (m == PGWT_MARKER_EXEC_END) {
            uint64_t start, qid;
            if (mode == ORACLE_PREFIX) {
                if (!st[pi].exec_one) continue;
                start = st[pi].exec_one;
                qid = st[pi].shared_qid;
                st[pi].exec_one = 0;
            } else {
                if (st[pi].ne <= 0) continue;
                st[pi].ne--;
                start = st[pi].es[st[pi].ne];
                qid = mode == ORACLE_SHARED_QID ? st[pi].shared_qid
                                                : st[pi].eq[st[pi].ne];
            }
            if (ev[i].timestamp_ns < start || qid == 0) continue;
            struct oracle_qid *s = oracle_slot(o, qid);
            s->exec_count++;
            s->exec_total += (double)(ev[i].timestamp_ns - start) / 1e6;
        } else if (m == PGWT_MARKER_PLAN_START) {
            if (mode == ORACLE_PREFIX) {
                st[pi].plan_one = ev[i].timestamp_ns;
                st[pi].shared_qid = ev[i].query_id;
            } else {
                if (st[pi].np >= ORACLE_DEPTH) { fprintf(stderr,
                        "oracle depth\n"); exit(2); }
                st[pi].ps[st[pi].np] = ev[i].timestamp_ns;
                st[pi].pq[st[pi].np] = ev[i].query_id;
                st[pi].np++;
                if (mode == ORACLE_SHARED_QID)
                    st[pi].shared_qid = ev[i].query_id;
            }
        } else if (m == PGWT_MARKER_PLAN_END) {
            uint64_t start, qid;
            if (mode == ORACLE_PREFIX) {
                if (!st[pi].plan_one) continue;
                start = st[pi].plan_one;
                qid = st[pi].shared_qid;
                st[pi].plan_one = 0;
            } else {
                if (st[pi].np <= 0) continue;
                st[pi].np--;
                start = st[pi].ps[st[pi].np];
                qid = mode == ORACLE_SHARED_QID ? st[pi].shared_qid
                                                : st[pi].pq[st[pi].np];
            }
            if (ev[i].timestamp_ns < start || qid == 0) continue;
            struct oracle_qid *s = oracle_slot(o, qid);
            s->plan_count++;
            s->plan_total += (double)(ev[i].timestamp_ns - start) / 1e6;
        } else if (m == PGWT_MARKER_CMD_END && mode != ORACLE_NO_CMDEND) {
            /* ORACLE_PREFIX never looked at CMD_END either, but leaving its
             * single slot set would make it a DIFFERENT mutant than the one
             * that shipped; clear it so 5.6's pre-fix numbers are the ones
             * the old code really produced. */
            st[pi].exec_one = 0;
            st[pi].plan_one = 0;
            st[pi].ne = 0;
            st[pi].np = 0;
        }
    }
}

static void oracle_exec(const struct oracle_out *o, uint64_t qid,
                        int *count, double *total)
{
    *count = 0; *total = 0.0;
    for (int i = 0; i < o->n; i++)
        if (o->q[i].qid == qid) { *count = o->q[i].exec_count;
                                  *total = o->q[i].exec_total; return; }
}

/* ── 5.1 the gate ─────────────────────────────────────────────────────── */

static void s5_1_nested_same_query(void)
{
    printf("\n5.1 #459 gate: outer [0,100] ms, inner [20,30] ms, one query\n");

    struct fixture fx = build_nested(QID_BOTH, QID_BOTH);
    struct pgwt_lifecycle_result r;
    pgwt_compute_query_lifecycle(fx.ev, fx.n, &r);
    CHECK(!r.failed, "nested scan did not fail");

    struct pgwt_qid_lifecycle *lc = pgwt_lifecycle_lookup(&r, QID_BOTH);
    CHECK(lc != NULL, "the nested query is present");
    if (lc) {
        CHECK_INT(lc->exec_count, 2,
                  "count = 2: the OUTER end contributes too (pre-fix: 1)");
        CHECK_DBL(lc->exec_total_ms, 110.0, 1e-9,
                  "total = 110 ms = 100 (outer) + 10 (inner)");
        CHECK_DBL(lc->exec_total_ms / lc->exec_count, 55.0, 1e-9,
                  "average = 55 ms (server.c divides total by count)");
        CHECK_INT(lc->exec_nsamples, 2, "both durations sampled");
        if (lc->exec_nsamples == 2) {
            pgwt_sort_doubles_asc(lc->exec_times, lc->exec_nsamples);
            CHECK_DBL(lc->exec_times[0], 10.0, 1e-9,
                      "inner sample = 10 ms exactly");
            CHECK_DBL(lc->exec_times[1], 100.0, 1e-9,
                      "outer sample = 100 ms exactly (not 80, not 30)");
        }
        /* The three wrong pairings this fixture is shaped to separate. */
        int saw_80 = 0, saw_30 = 0;
        for (int i = 0; i < lc->exec_nsamples; i++) {
            if (fabs(lc->exec_times[i] - 80.0) < 1e-9) saw_80 = 1;
            if (fabs(lc->exec_times[i] - 30.0) < 1e-9) saw_30 = 1;
        }
        CHECK(!saw_80, "no 80 ms sample (inner start paired with outer end)");
        CHECK(!saw_30, "no 30 ms sample (outer start paired with inner end)");
    }
    pgwt_lifecycle_free(&r);
    free(fx.ev);
}

/* ── 5.2 outer and inner are DIFFERENT queries ────────────────────────── */

static void s5_2_nested_distinct_queries(void)
{
    printf("\n5.2 nesting across two query ids: each keeps its own span\n");

    struct fixture fx = build_nested(QID_OUTER, QID_INNER);
    struct pgwt_lifecycle_result r;
    pgwt_compute_query_lifecycle(fx.ev, fx.n, &r);

    int oc, ic; double ot, it;
    real_exec(&r, QID_OUTER, &oc, &ot);
    real_exec(&r, QID_INNER, &ic, &it);
    CHECK_INT(oc, 1, "outer query: one execution (pre-fix: zero, ABSENT)");
    CHECK_DBL(ot, 100.0, 1e-9, "outer query: 100 ms");
    CHECK_INT(ic, 1, "inner query: one execution");
    CHECK_DBL(it, 10.0, 1e-9, "inner query: 10 ms");
    pgwt_lifecycle_free(&r);
    free(fx.ev);
}

/* ── 5.3 a PLAN inside an execution must not re-key it ────────────────── */

static void s5_3_plan_does_not_rekey_exec(void)
{
    printf("\n5.3 PLAN_START inside an open execution: separate identities\n");

    struct fixture fx = build_plan_inside_exec();
    struct pgwt_lifecycle_result r;
    pgwt_compute_query_lifecycle(fx.ev, fx.n, &r);

    struct pgwt_qid_lifecycle *outer = pgwt_lifecycle_lookup(&r, QID_OUTER);
    struct pgwt_qid_lifecycle *planq = pgwt_lifecycle_lookup(&r, QID_PLANQ);
    CHECK(outer != NULL,
          "the EXECUTING query is present (pre-fix: credited to the PLAN's "
          "query id, so this lookup was NULL)");
    if (outer) {
        CHECK_INT(outer->exec_count, 1, "executing query: one execution");
        CHECK_DBL(outer->exec_total_ms, 100.0, 1e-9,
                  "executing query: the full 100 ms");
        CHECK_INT(outer->plan_count, 0,
                  "executing query: no planning charged to it");
    }
    CHECK(planq != NULL, "the planned query is present");
    if (planq) {
        CHECK_INT(planq->plan_count, 1, "planned query: one plan");
        CHECK_DBL(planq->plan_total_ms, 5.0, 1e-9, "planned query: 5 ms");
        CHECK_INT(planq->exec_count, 0,
                  "planned query: no EXECUTION charged to it (pre-fix: 1 "
                  "execution of 100 ms)");
    }
    pgwt_lifecycle_free(&r);
    free(fx.ev);
}

/* ── 5.4 CMD_END abandons, and does NOT count ─────────────────────────── */

static void s5_4_cmd_end_abandons(void)
{
    printf("\n5.4 CMD_END drops abandoned entries without counting them\n");

    struct fixture fx = build_orphan_then_real();
    struct pgwt_lifecycle_result r;
    pgwt_compute_query_lifecycle(fx.ev, fx.n, &r);
    struct pgwt_qid_lifecycle *lc = pgwt_lifecycle_lookup(&r, QID_BOTH);
    CHECK(lc != NULL, "the query that DID complete once is present");
    if (lc) {
        CHECK_INT(lc->exec_count, 1,
                  "exactly one completed pair: the orphan is not counted");
        CHECK_DBL(lc->exec_total_ms, 20.0, 1e-9,
                  "20 ms, the measured pair -- not 120 ms (the real END "
                  "popping the orphan's start) and not 50 ms (an inferred "
                  "end at CMD_END)");
    }
    pgwt_lifecycle_free(&r);
    free(fx.ev);

    /* Nested, where only the INNER statement finished before the command
     * died: the inner pair counts, the outer is abandoned and absent. */
    struct fixture fx2 = {0};
    int cap = 0;
    uint64_t t0 = 1000000000ULL;
    fx_push(&fx2, &cap, NEST_PID, PGWT_MARKER_EXEC_START, t0, 0, QID_OUTER);
    fx_push(&fx2, &cap, NEST_PID, PGWT_MARKER_EXEC_START,
            t0 + 20 * MS_NS, 0, QID_INNER);
    fx_push(&fx2, &cap, NEST_PID, PGWT_MARKER_EXEC_END,
            t0 + 30 * MS_NS, 0, QID_INNER);
    fx_push(&fx2, &cap, NEST_PID, PGWT_MARKER_CMD_END,
            t0 + 40 * MS_NS, 0, QID_OUTER);
    pgwt_compute_query_lifecycle(fx2.ev, fx2.n, &r);
    int oc, ic; double ot, it;
    real_exec(&r, QID_OUTER, &oc, &ot);
    real_exec(&r, QID_INNER, &ic, &it);
    CHECK(pgwt_lifecycle_lookup(&r, QID_OUTER) == NULL,
          "the cancelled OUTER statement is absent, not an inferred 40 ms");
    CHECK_INT(oc, 0, "cancelled outer: no execution counted");
    CHECK_INT(ic, 1, "the inner statement that did finish still counts");
    CHECK_DBL(it, 10.0, 1e-9, "inner: 10 ms");
    pgwt_lifecycle_free(&r);
    free(fx2.ev);

    /* The leak the clear prevents: an abandoned statement, its CMD_END,
     * then a stray EXEC_END from the NEXT command whose start was dropped.
     * Nothing may be charged to either query. */
    struct fixture fx3 = build_orphan_lost_start();
    pgwt_compute_query_lifecycle(fx3.ev, fx3.n, &r);
    CHECK(pgwt_lifecycle_lookup(&r, QID_OUTER) == NULL,
          "a stray END after CMD_END charges the abandoned query NOTHING "
          "(without the clear: 120 ms of another command's time)");
    CHECK(pgwt_lifecycle_lookup(&r, QID_INNER) == NULL,
          "...and nothing to the query the stray END named either");
    CHECK_INT(r.n, 0, "the table is empty, not a zero row");
    pgwt_lifecycle_free(&r);
    free(fx3.ev);
}

/* ── 5.5 depth past the stack's initial capacity ──────────────────────── */

#define DEEP 64

static void s5_5_deep_nesting(void)
{
    printf("\n5.5 nesting %d deep (the stack starts at 8 and must grow)\n",
           DEEP);

    /* Execution k spans [k, 200-k] ms, so k nests strictly inside k-1 and
     * every duration (200 - 2k ms) is distinct: a dropped level moves the
     * total AND the sample set, never only the count. */
    struct fixture fx = {0};
    int cap = 0;
    uint64_t t0 = 1000000000ULL;
    for (int k = 0; k < DEEP; k++)
        fx_push(&fx, &cap, NEST_PID, PGWT_MARKER_EXEC_START,
                t0 + (uint64_t)k * MS_NS, 0, QID_BOTH);
    for (int k = DEEP - 1; k >= 0; k--)
        fx_push(&fx, &cap, NEST_PID, PGWT_MARKER_EXEC_END,
                t0 + (uint64_t)(200 - k) * MS_NS, 0, QID_BOTH);

    struct pgwt_lifecycle_result r;
    pgwt_compute_query_lifecycle(fx.ev, fx.n, &r);
    CHECK(!r.failed, "deep scan did not fail");
    struct pgwt_qid_lifecycle *lc = pgwt_lifecycle_lookup(&r, QID_BOTH);
    CHECK(lc != NULL, "deep fixture produces an entry");
    if (lc) {
        CHECK_INT(lc->exec_count, DEEP, "every nesting level counted");
        double want = 0;
        for (int k = 0; k < DEEP; k++) want += (double)(200 - 2 * k);
        CHECK_DBL(lc->exec_total_ms, want, 1e-9,
                  "total is the sum of all 64 inclusive spans");
        CHECK_INT(lc->exec_nsamples, DEEP, "every level sampled");
        if (lc->exec_nsamples == DEEP) {
            pgwt_sort_doubles_asc(lc->exec_times, lc->exec_nsamples);
            CHECK_DBL(lc->exec_times[0], (double)(200 - 2 * (DEEP - 1)),
                      1e-9, "innermost span is the smallest sample");
            CHECK_DBL(lc->exec_times[DEEP - 1], 200.0, 1e-9,
                      "outermost span is 200 ms");
        }
    }
    pgwt_lifecycle_free(&r);
    free(fx.ev);
}

/* ── 5.6 bypass suite: the mutants, and the fixture blind to all of them ─ */

static void s5_6_negative_controls(void)
{
    printf("\n5.6 negative controls: each mutation must change the answer\n");

    struct fixture nested = build_nested(QID_BOTH, QID_BOTH);
    struct fixture flat   = build_flat(QID_BOTH);
    struct fixture planfx = build_plan_inside_exec();
    struct fixture orphan = build_orphan_lost_start();

    struct pgwt_lifecycle_result rn, rf, rp, ro;
    pgwt_compute_query_lifecycle(nested.ev, nested.n, &rn);
    pgwt_compute_query_lifecycle(flat.ev,   flat.n,   &rf);
    pgwt_compute_query_lifecycle(planfx.ev, planfx.n, &rp);
    pgwt_compute_query_lifecycle(orphan.ev, orphan.n, &ro);

    struct oracle_out o;
    int rc, oc; double rt, ot;

    /* Control 1 — restore the single execution timestamp. */
    oracle_run(nested.ev, nested.n, ORACLE_PREFIX, &o);
    real_exec(&rn, QID_BOTH, &rc, &rt);
    oracle_exec(&o, QID_BOTH, &oc, &ot);
    CHECK_INT(oc, 1, "single-slot mutant counts ONE execution on the "
                     "nested fixture (the shipped defect)");
    CHECK_DBL(ot, 10.0, 1e-9, "...and reports only the inner 10 ms");
    CHECK(rc != oc && fabs(rt - ot) > 1e-9,
          "CONTROL 1: the real code disagrees with the single-slot mutant "
          "-- restoring one timestamp makes 5.1 fail");

    /* Control 2 — restore the shared plan/exec query-id slot. */
    oracle_run(planfx.ev, planfx.n, ORACLE_SHARED_QID, &o);
    real_exec(&rp, QID_OUTER, &rc, &rt);
    oracle_exec(&o, QID_OUTER, &oc, &ot);
    CHECK_INT(oc, 0, "shared-qid mutant loses the executing query entirely");
    CHECK_INT(rc, 1, "the real code keeps it");
    CHECK(rc != oc,
          "CONTROL 2: the real code disagrees with the shared-qid mutant "
          "-- restoring one qid slot makes 5.3 fail");
    oracle_exec(&o, QID_PLANQ, &oc, &ot);
    CHECK(oc == 1 && fabs(ot - 100.0) < 1e-9,
          "...and charges the outer 100 ms execution to the PLANNED query");

    /* Control 3 — stop clearing abandoned entries at CMD_END. Needs an
     * UNBALANCED end (see build_orphan_lost_start): with LIFO stacks a
     * balanced later statement pops its own entry and cannot see the
     * orphan, so the fixture has to be the stray-END one. */
    oracle_run(orphan.ev, orphan.n, ORACLE_NO_CMDEND, &o);
    real_exec(&ro, QID_OUTER, &rc, &rt);
    oracle_exec(&o, QID_OUTER, &oc, &ot);
    CHECK(oc == 1 && fabs(ot - 120.0) < 1e-9,
          "no-CMD_END mutant pairs the stray END with the ORPHAN's start "
          "and charges the abandoned query 120 ms");
    CHECK_INT(rc, 0, "the real code charges it nothing");
    CHECK(rc != oc && fabs(rt - ot) > 1e-9,
          "CONTROL 3: the real code disagrees -- dropping the CMD_END clear "
          "makes 5.4 fail");
    /* And the clear must not be a blanket "drop everything": the same
     * mutant AGREES on the balanced fixture, which is why 5.4 keeps both. */
    struct fixture balanced = build_orphan_then_real();
    struct pgwt_lifecycle_result rb;
    pgwt_compute_query_lifecycle(balanced.ev, balanced.n, &rb);
    oracle_run(balanced.ev, balanced.n, ORACLE_NO_CMDEND, &o);
    real_exec(&rb, QID_BOTH, &rc, &rt);
    oracle_exec(&o, QID_BOTH, &oc, &ot);
    CHECK(rc == oc && fabs(rt - ot) < 1e-9,
          "balanced orphan fixture: the no-CMD_END mutant AGREES (LIFO "
          "already protects it) -- pinned so it is not mistaken for the "
          "control");
    pgwt_lifecycle_free(&rb);
    free(balanced.ev);

    /* THE BLIND FIXTURE. Two disjoint executions give count = 2 and
     * total = 110 under the real code AND under every mutant above, so a
     * gate built on flat executions is satisfied with all three defects
     * present. Pinned here so nobody "simplifies" 5.1 into it. */
    real_exec(&rf, QID_BOTH, &rc, &rt);
    CHECK(rc == 2 && fabs(rt - 110.0) < 1e-9,
          "flat fixture: count 2, total 110 -- identical to the nested "
          "gate's CORRECT answer");
    for (int mode = ORACLE_PREFIX; mode <= ORACLE_NO_CMDEND; mode++) {
        oracle_run(flat.ev, flat.n, mode, &o);
        oracle_exec(&o, QID_BOTH, &oc, &ot);
        char msg[160];
        snprintf(msg, sizeof(msg),
                 "flat fixture: mutant %d agrees with the real code "
                 "(count %d, total %.1f) -- a flat gate sees NOTHING",
                 mode, oc, ot);
        CHECK(oc == rc && fabs(ot - rt) < 1e-9, msg);
    }

    pgwt_lifecycle_free(&rn);
    pgwt_lifecycle_free(&rf);
    pgwt_lifecycle_free(&rp);
    pgwt_lifecycle_free(&ro);
    free(nested.ev); free(flat.ev); free(planfx.ev); free(orphan.ev);
}

/* ── 5.7 the new stacks' allocation failures refuse ───────────────────── */

static void s5_7_stack_alloc_failure_refuses(void)
{
    printf("\n5.7 the per-pid phase stacks: allocation failure is a REFUSAL\n");

    /* A dropped push is worse than a missing one: the next END would pop
     * someone else's start. So the push has no "skip it" path at all --
     * only a refusal. Both stacks allocate on their FIRST push (cap 0), so
     * the injection is reached by the first marker of its kind. */
    struct fixture fx = build_plan_inside_exec();

    static const struct { const char *point; const char *what; } cases[] = {
        { "lifecycle_exec_stack", "execution stack growth" },
        { "lifecycle_plan_stack", "planning stack growth" },
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

    /* The injection must be able to do nothing, or the two refusals above
     * could be coming from somewhere else entirely. */
    setenv("PGWT_TEST_ALLOC_FAIL", "lifecycle_no_such_stack", 1);
    struct pgwt_lifecycle_result ok;
    pgwt_compute_query_lifecycle(fx.ev, fx.n, &ok);
    unsetenv("PGWT_TEST_ALLOC_FAIL");
    CHECK(!ok.failed, "an unknown injection point changes nothing");
    struct pgwt_qid_lifecycle *lc = pgwt_lifecycle_lookup(&ok, QID_OUTER);
    CHECK(lc && lc->exec_count == 1 && fabs(lc->exec_total_ms - 100.0) < 1e-9,
          "...and the uninjected answer is still the complete one");
    pgwt_lifecycle_free(&ok);
    free(fx.ev);
}

/* ── 5.8 absent and malformed, never wrong ────────────────────────────── */

static void s5_8_absent_and_malformed(void)
{
    printf("\n5.8 absent / malformed marker streams\n");

    uint64_t t0 = 1000000000ULL;

    /* An EXEC_END with nothing open (a window that opens mid-execution, or
     * a dropped EXEC_START): nothing, and certainly not a sample measured
     * from zero. */
    struct fixture a = {0}; int ca = 0;
    fx_push(&a, &ca, NEST_PID, PGWT_MARKER_EXEC_END, t0, 0, QID_BOTH);
    fx_push(&a, &ca, NEST_PID, PGWT_MARKER_EXEC_END, t0 + MS_NS, 0, QID_BOTH);
    struct pgwt_lifecycle_result r;
    pgwt_compute_query_lifecycle(a.ev, a.n, &r);
    CHECK(!r.failed && pgwt_lifecycle_lookup(&r, QID_BOTH) == NULL,
          "two ENDs with no START: absent, not a 1-second execution");
    pgwt_lifecycle_free(&r);
    free(a.ev);

    /* An END that PRECEDES its start. The subtraction is unsigned, so the
     * `ms < 0` filter in lifecycle_sample cannot see this -- it wraps to
     * ~1.8e13 ms and would sit in the p99 forever. */
    struct fixture b = {0}; int cb = 0;
    fx_push(&b, &cb, NEST_PID, PGWT_MARKER_EXEC_START,
            t0 + 50 * MS_NS, 0, QID_BOTH);
    fx_push(&b, &cb, NEST_PID, PGWT_MARKER_EXEC_END,
            t0 + 10 * MS_NS, 0, QID_BOTH);
    pgwt_compute_query_lifecycle(b.ev, b.n, &r);
    CHECK(pgwt_lifecycle_lookup(&r, QID_BOTH) == NULL,
          "an END before its START is dropped, not wrapped into a "
          "1.8e13 ms sample");
    pgwt_lifecycle_free(&r);
    free(b.ev);

    /* Query id 0 keeps the pre-existing filtering policy: that pair is
     * dropped, and dropping it must not disturb the one beside it. */
    struct fixture c = build_nested(0, QID_INNER);
    pgwt_compute_query_lifecycle(c.ev, c.n, &r);
    int ic; double it;
    real_exec(&r, QID_INNER, &ic, &it);
    CHECK(pgwt_lifecycle_lookup(&r, 0) == NULL,
          "query id 0 is still filtered out (policy unchanged)");
    CHECK(ic == 1 && fabs(it - 10.0) < 1e-9,
          "the qid-0 outer pair does not disturb the inner one");
    pgwt_lifecycle_free(&r);
    free(c.ev);

    /* No markers at all: the lifecycle answer is ABSENT. */
    struct fixture d = {0}; int cd = 0;
    fx_push(&d, &cd, NEST_PID, WAIT_EVENT, t0, 1000, QID_BOTH);
    pgwt_compute_query_lifecycle(d.ev, d.n, &r);
    CHECK(!r.failed && r.n == 0,
          "a marker-free window produces an EMPTY table, not a zero row");
    pgwt_lifecycle_free(&r);
    free(d.ev);
}

/* ── 5.9 the same markers through Waterfall must agree ────────────────── */

static void s5_9_waterfall_crosscheck(void)
{
    printf("\n5.9 valid completed pairs agree with pgwt_compute_executions\n");

    /* Three pids: each runs a nested pair (outer containing inner) and one
     * flat execution, and pid 3 additionally abandons a statement that its
     * CMD_END closes. Durations are distinct per pid so a swapped pairing
     * on either side moves a total.
     *
     * The two computations are INDEPENDENT code paths over one array:
     * pgwt_compute_query_lifecycle accumulates per query id, and
     * pgwt_compute_executions materialises one row per execution. They are
     * allowed to disagree about an INFERRED end (Waterfall shows such a row
     * with end_inferred=1; the lifecycle scan drops it) -- so this compares
     * MEASURED pairs only, and asserts below that an inferred row really
     * existed to be excluded. */
    struct fixture fx = {0};
    int cap = 0;
    uint64_t t0 = 1000000000ULL;
    const uint64_t qid[3] = { 0x111ULL, 0x222ULL, 0x333ULL };
    for (int p = 0; p < 3; p++) {
        uint32_t pid = NEST_PID + (uint32_t)p;
        uint64_t base = t0 + (uint64_t)p * 1000 * MS_NS;
        uint64_t outer = (uint64_t)(40 + 10 * p);      /* ms */
        uint64_t inner = (uint64_t)(5 + p);            /* ms */
        fx_push(&fx, &cap, pid, PGWT_MARKER_EXEC_START, base, 0, qid[p]);
        fx_push(&fx, &cap, pid, PGWT_MARKER_EXEC_START,
                base + 10 * MS_NS, 0, qid[p]);
        fx_push(&fx, &cap, pid, PGWT_MARKER_EXEC_END,
                base + (10 + inner) * MS_NS, 0, qid[p]);
        fx_push(&fx, &cap, pid, PGWT_MARKER_EXEC_END,
                base + outer * MS_NS, 0, qid[p]);
        fx_push(&fx, &cap, pid, PGWT_MARKER_CMD_END,
                base + (outer + 1) * MS_NS, 0, qid[p]);
        /* flat execution, 7+p ms */
        fx_push(&fx, &cap, pid, PGWT_MARKER_EXEC_START,
                base + 100 * MS_NS, 0, qid[p]);
        fx_push(&fx, &cap, pid, PGWT_MARKER_EXEC_END,
                base + (uint64_t)(107 + p) * MS_NS, 0, qid[p]);
        fx_push(&fx, &cap, pid, PGWT_MARKER_CMD_END,
                base + 110 * MS_NS, 0, qid[p]);
        if (p == 2) {
            /* abandoned statement: no EXEC_END, closed by CMD_END */
            fx_push(&fx, &cap, pid, PGWT_MARKER_EXEC_START,
                    base + 200 * MS_NS, 0, qid[p]);
            fx_push(&fx, &cap, pid, PGWT_MARKER_CMD_END,
                    base + 260 * MS_NS, 0, qid[p]);
        }
    }

    struct pgwt_lifecycle_result r;
    pgwt_compute_query_lifecycle(fx.ev, fx.n, &r);
    CHECK(!r.failed, "lifecycle scan did not fail");

    struct pgwt_executions_result xr;
    pgwt_compute_executions(fx.ev, fx.n, t0 - 1000 * MS_NS,
                            t0 + 10000 * MS_NS, NULL, NULL, 0, &xr);
    CHECK(!xr.failed, "Waterfall extraction did not fail");

    /* PROVENANCE: the comparison is only worth anything if Waterfall
     * actually produced nested rows and an inferred one. */
    int measured = 0, inferred = 0, nested = 0, in_progress = 0;
    for (int i = 0; i < xr.num_rows; i++) {
        if (xr.rows[i].in_progress) { in_progress++; continue; }
        if (xr.rows[i].end_inferred) { inferred++; continue; }
        measured++;
        for (int j = 0; j < xr.num_rows; j++) {
            if (j == i || xr.rows[j].pid != xr.rows[i].pid) continue;
            if (xr.rows[j].in_progress) continue;
            if (xr.rows[j].start_ns < xr.rows[i].start_ns &&
                xr.rows[i].end_ns < xr.rows[j].end_ns) { nested++; break; }
        }
    }
    CHECK_INT(xr.num_rows, 10, "Waterfall found every execution");
    CHECK_INT(measured, 9, "9 measured pairs (3 pids x 3, minus the orphan)");
    CHECK_INT(nested, 3, "3 of them are strictly nested inside another -- "
                         "the comparison is NOT over a flat fixture");
    CHECK_INT(inferred, 1, "1 CMD_END-closed row exists to be excluded");
    CHECK_INT(in_progress, 0, "no row left open");

    /* Per query id: the lifecycle totals must equal the sum of Waterfall's
     * measured rows, and the counts must match. */
    for (int p = 0; p < 3; p++) {
        int wc = 0; double wt = 0;
        for (int i = 0; i < xr.num_rows; i++) {
            if (xr.rows[i].in_progress || xr.rows[i].end_inferred) continue;
            if (xr.rows[i].query_id != qid[p]) continue;
            wc++;
            wt += (double)(xr.rows[i].end_ns - xr.rows[i].start_ns) / 1e6;
        }
        int lcc; double lct;
        real_exec(&r, qid[p], &lcc, &lct);
        char msg[180];
        snprintf(msg, sizeof(msg),
                 "qid 0x%llx: lifecycle count == Waterfall measured count",
                 (unsigned long long)qid[p]);
        CHECK_INT(lcc, wc, msg);
        snprintf(msg, sizeof(msg),
                 "qid 0x%llx: lifecycle total == sum of Waterfall spans",
                 (unsigned long long)qid[p]);
        CHECK_DBL(lct, wt, 1e-9, msg);
        CHECK(wc == 3, "...and that count is 3, not 0 (non-vacuous)");
    }

    free(xr.rows);
    pgwt_lifecycle_free(&r);
    free(fx.ev);
}

int main(void)
{
    printf("=== test_pid_cap (#275, #459) ===\n");
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
    s5_1_nested_same_query();
    s5_2_nested_distinct_queries();
    s5_3_plan_does_not_rekey_exec();
    s5_4_cmd_end_abandons();
    s5_5_deep_nesting();
    s5_6_negative_controls();
    s5_7_stack_alloc_failure_refuses();
    s5_8_absent_and_malformed();
    s5_9_waterfall_crosscheck();

    printf("\n=== %d checks, %d failures ===\n", checks, failures);
    return failures ? 1 : 0;
}
