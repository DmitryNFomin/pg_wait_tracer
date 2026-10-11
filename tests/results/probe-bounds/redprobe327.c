/* redprobe327.c — the RED demonstration for #327.
 *
 * Pre-fix, each of the six probe loops does not fail an assertion: it SPINS
 * FOREVER. So the red evidence cannot be "a failing check"; it has to be
 * "did not terminate". This driver builds the SAME cap+1 fixtures that
 * tests/test_probe_bounds.c uses and calls one site per invocation, so a
 * hang at one site does not hide the others. Run it under `timeout`:
 * exit 124 == the hang == red.
 *
 * It compiles against the PARENT src/compute.c (no `failed` fields), which
 * is why it is a separate driver and not the unit test itself.
 *
 *   usage: redprobe327 sessions|sessions_summary|queries|queries_summary|
 *                      fingerprints|variants
 */
#include "compute.h"
#include "wait_event.h"
#include "pg_wait_tracer.h"
#include "summary_reader.h"
#include "summary_writer.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CAP_SESSIONS 1024
#define CAP_QUERIES  2048
#define CAP_VARIANTS 4096
#define N_SESSIONS (CAP_SESSIONS + 1)
#define N_QUERIES  (CAP_QUERIES + 1)
#define N_VARIANTS (CAP_VARIANTS + 1)
#define ROUNDS     3
#define BASE_PID   40000u
#define BASE_QID   0x7000000000ULL
#define MS         1000000ULL
#define TS_BASE    1000000000000ULL
#define WAIT_A WEI(PG_WAIT_LWLOCK, 1)
#define WAIT_B WEI(PG_WAIT_LWLOCK, 2)
static uint32_t wait_id(int k) { return WEI(PG_WAIT_LWLOCK, 1 + (uint32_t)k); }

static struct pgwt_summary_accum **stub_recs;
static int stub_nrecs;

int pgwt_visit_summaries(const char *d, uint64_t a, uint64_t b,
                         pgwt_summary_visitor visitor, void *ctx)
{
    (void)d; (void)a; (void)b;
    for (int i = 0; i < stub_nrecs; i++)
        if (visitor(stub_recs[i], ctx) != 0) break;
    return stub_nrecs;
}

static struct pgwt_trace_event *ev;
static int n, cap;

static void push(uint32_t pid, uint32_t old_e, uint32_t new_e, uint64_t ts,
                 uint64_t dur, uint64_t qid)
{
    if (n >= cap) {
        cap = cap ? cap * 2 : 4096;
        ev = realloc(ev, (size_t)cap * sizeof(*ev));
        if (!ev) exit(2);
    }
    struct pgwt_trace_event *e = &ev[n++];
    memset(e, 0, sizeof(*e));
    e->pid = pid; e->old_event = old_e; e->new_event = new_e;
    e->timestamp_ns = ts; e->duration_ns = dur; e->query_id = qid;
    e->cpu_ns = PGWT_CPU_NS_UNKNOWN;
}

static void build_sessions(void)
{
    uint64_t ts = TS_BASE;
    for (int r = 0; r < ROUNDS; r++)
        for (int k = 0; k < N_SESSIONS; k++) {
            uint64_t dur = (uint64_t)(k + 1) * MS;
            push(BASE_PID + (uint32_t)k, WAIT_A, WAIT_B, ts, dur, 0);
            ts += dur + MS;
        }
}

static void build_queries(void)
{
    uint64_t ts = TS_BASE;
    for (int r = 0; r < ROUNDS; r++)
        for (int k = 0; k < N_QUERIES; k++) {
            uint64_t dur = (uint64_t)(k + 1) * MS;
            push(BASE_PID + (uint32_t)(k % 64), WAIT_A, WAIT_B, ts, dur,
                 BASE_QID + (uint64_t)k);
            ts += dur + MS;
        }
}

static void build_variants(void)
{
    uint64_t ts = TS_BASE;
    int made = 0;
    for (int ia = 0; ia < 65 && made < N_VARIANTS; ia++)
        for (int ib = 0; ib < 65 && made < N_VARIANTS; ib++) {
            if (ia == ib) continue;
            uint32_t pid = BASE_PID + (uint32_t)(made % 16);
            uint64_t qid = BASE_QID + (uint64_t)(made % 8);
            push(pid, PGWT_MARKER_EXEC_START, PGWT_MARKER_EXEC_START, ts, 0, qid);
            push(pid, wait_id(ia), wait_id(ib), ts + MS, MS, qid);
            push(pid, wait_id(ib), WAIT_A, ts + 2 * MS, MS, qid);
            push(pid, PGWT_MARKER_EXEC_END, PGWT_MARKER_EXEC_END,
                 ts + 3 * MS, 0, qid);
            ts += 10 * MS;
            made++;
        }
}

static struct pgwt_summary_accum *rec_new(uint64_t s)
{
    struct pgwt_summary_accum *r = calloc(1, sizeof(*r));
    if (!r) exit(2);
    r->second_wall_ns = s; r->second_mono_ns = s;
    return r;
}

int main(int argc, char **argv)
{
    if (argc < 2) { fprintf(stderr, "usage: redprobe327 <site>\n"); return 2; }
    const char *site = argv[1];
    struct pgwt_filter f;
    memset(&f, 0, sizeof(f));

    if (strcmp(site, "sessions") == 0) {
        build_sessions();
        printf("calling pgwt_compute_top_sessions with %d distinct pids "
               "(cap %d)...\n", N_SESSIONS, CAP_SESSIONS);
        fflush(stdout);
        struct pgwt_sessions_result r;
        pgwt_compute_top_sessions(ev, n, &f, 0, 0, 0, &r);
        printf("RETURNED num_rows=%d\n", r.num_rows);
        return 0;
    }
    if (strcmp(site, "queries") == 0) {
        build_queries();
        printf("calling pgwt_compute_top_queries with %d distinct query ids "
               "(cap %d)...\n", N_QUERIES, CAP_QUERIES);
        fflush(stdout);
        struct pgwt_queries_result r;
        pgwt_compute_top_queries(ev, n, &f, 0, 0, 0, &r);
        printf("RETURNED num_rows=%d\n", r.num_rows);
        return 0;
    }
    if (strcmp(site, "fingerprints") == 0) {
        build_queries();
        printf("calling pgwt_compute_fingerprints with %d distinct query ids "
               "(cap %d)...\n", N_QUERIES, CAP_QUERIES);
        fflush(stdout);
        struct pgwt_fingerprint_result r;
        pgwt_compute_fingerprints(ev, n, &f, &r);
        printf("RETURNED num_rows=%d\n", r.num_rows);
        return 0;
    }
    if (strcmp(site, "variants") == 0) {
        build_variants();
        printf("calling pgwt_compute_variants with %d distinct patterns "
               "(cap %d)...\n", N_VARIANTS, CAP_VARIANTS);
        fflush(stdout);
        struct pgwt_variants_result r;
        pgwt_compute_variants(ev, n, NULL, 100000, PGWT_PHASE_EXEC, &r);
        printf("RETURNED num_variants=%d total_executions=%d\n",
               r.num_variants, r.total_executions);
        return 0;
    }
    if (strcmp(site, "sessions_summary") == 0) {
        struct pgwt_summary_accum *a = rec_new(1000000000ULL);
        struct pgwt_summary_accum *b = rec_new(2000000000ULL);
        for (int k = 0; k < CAP_SESSIONS; k++) {
            a->sessions[k].pid = BASE_PID + (uint32_t)k;
            a->sessions[k].db_time_ns = (uint64_t)(k + 1) * MS;
            a->sessions[k].top_wait_id = WAIT_A;
            a->sessions[k].top_wait_ns = (uint64_t)(k + 1) * MS;
        }
        a->num_sessions = CAP_SESSIONS;
        b->sessions[0].pid = BASE_PID + (uint32_t)(N_SESSIONS - 1);
        b->sessions[0].db_time_ns = (uint64_t)N_SESSIONS * MS;
        b->sessions[0].top_wait_id = WAIT_A;
        b->sessions[0].top_wait_ns = (uint64_t)N_SESSIONS * MS;
        b->num_sessions = 1;
        static struct pgwt_summary_accum *recs[2];
        recs[0] = a; recs[1] = b;
        stub_recs = recs; stub_nrecs = 2;
        printf("calling pgwt_compute_top_sessions_from_summaries: %d pids "
               "across 2 records of <= %d (cap %d)...\n",
               N_SESSIONS, CAP_SESSIONS, CAP_SESSIONS);
        fflush(stdout);
        struct pgwt_sessions_result r;
        pgwt_compute_top_sessions_from_summaries("x", 0, 0, &f, 0, &r);
        printf("RETURNED num_rows=%d\n", r.num_rows);
        return 0;
    }
    if (strcmp(site, "queries_summary") == 0) {
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
        static struct pgwt_summary_accum *recs[2];
        recs[0] = a; recs[1] = b;
        stub_recs = recs; stub_nrecs = 2;
        printf("calling pgwt_compute_top_queries_from_summaries: %d query ids "
               "across 2 records of <= %d (cap %d)...\n",
               N_QUERIES, CAP_QUERIES, CAP_QUERIES);
        fflush(stdout);
        struct pgwt_queries_result r;
        pgwt_compute_top_queries_from_summaries("x", 0, 0, &f, 0, &r);
        printf("RETURNED num_rows=%d\n", r.num_rows);
        return 0;
    }
    fprintf(stderr, "unknown site %s\n", site);
    return 2;
}
