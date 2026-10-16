/* test_semantics_wire.c — the four semantics contracts that live in the
 * RESPONSE rather than in a compute result, driven through the real handlers
 * in src/server.c over a real trace file.
 *
 *   1. #435  Overview must serialize the Idle rows' %DB as JSON null
 *            ("not applicable"), never as a numeric 0 ("measured zero").
 *            Events has emitted null for the same events since FID-3; the
 *            Overview did not, so the same idle time read "0.0%" on one tab
 *            and "—" on the other.
 *   2. #441a Queries' latency average must be suppressed where no
 *            full-duration evidence exists -- a sampled-only query, and the
 *            per-second summary path, which carries no full durations at all
 *            and used to publish in-window DB Time / interval count as a
 *            latency.
 *   3. #441c Queries' exec/plan statistics are WHOLE-QUERY (every pid that
 *            ran the query) because the marker scan is not pid-scoped. Under
 *            a class/event filter that is correct and is now said out loud;
 *            under a PID DRILL it is a wrong scope and must be SUPPRESSED.
 *   4. #436  The transition graph's NODE population must equal its EDGE
 *            population. The node pass ran through
 *            pgwt_block_agg_node_counts(), which takes no filter, so under a
 *            class drill the picture carried node bars for records whose
 *            edges the same response had excluded.
 *
 * WHY THIS FILE EXISTS SEPARATELY FROM test_window_clip.c: every contract
 * above is a property of the JSON the client receives. A compute-level
 * assertion cannot see the difference between `0` and `null`, between an
 * absent key and a suppressed one, or between a node table built from one
 * population and one built from another -- which is exactly how #435 and
 * #441 survived: their compute values were defensible and their
 * SERIALIZATION was not.
 *
 * BYPASS SUITE (section 5). Every assertion above is satisfiable by a
 * response that contains nothing: "no numeric idle pct" is trivially true of
 * an empty row list, "avg_us is null" of a row that does not exist, "no IO
 * node" of a fixture with no IO records. Section 5 walks those, and every
 * section above additionally asserts its own POSITIVE control -- the value
 * that proves the handler looked.
 *
 * Runs anywhere pgwt-server builds (no BPF, no live PostgreSQL): writes a
 * real trace (and a real summary file) into a temp dir and calls the
 * handlers directly.
 */
#define main pgwt_server_main
#include "../src/server.c"
#undef main
#include "idle_rule.h"

#include <unistd.h>
#include <math.h>

static int checks = 0;
static int failures = 0;

#define CHECK(cond, fmt, ...) do {                                        \
    checks++;                                                             \
    if (cond) printf("  ok: " fmt "\n", ##__VA_ARGS__);                   \
    else { failures++;                                                    \
           printf("  FAIL(%s:%d): " fmt "\n", __FILE__, __LINE__,         \
                  ##__VA_ARGS__); }                                       \
} while (0)

#define MS      1000000ULL
#define ONE_SEC 1000000000ULL

/* PG18 ids. ClientRead is idle-but-visible; Lock/IO never are. */
#define EV_CLIENT_READ WEI(PG_WAIT_CLIENT, 0)
#define EV_LOCK        WEI(PG_WAIT_LOCK, 0)
#define EV_IO          WEI(PG_WAIT_IO, 21)

/* ── response capture ─────────────────────────────────────────────────── */

typedef void (*handler_fn)(struct pgwt_server *, struct pgwt_request *);

static cJSON *call_handler(handler_fn fn, struct pgwt_server *srv,
                           struct pgwt_request *req)
{
    FILE *capture = tmpfile();
    if (!capture) { printf("  FATAL: tmpfile\n"); exit(2); }
    fflush(stdout);
    int saved = dup(STDOUT_FILENO);
    dup2(fileno(capture), STDOUT_FILENO);
    fn(srv, req);
    fflush(stdout);
    dup2(saved, STDOUT_FILENO);
    close(saved);
    fseek(capture, 0, SEEK_END);
    long size = ftell(capture);
    rewind(capture);
    char *json = calloc((size_t)size + 1, 1);
    if (!json) { printf("  FATAL: oom\n"); exit(2); }
    size_t rd = fread(json, 1, (size_t)size, capture);
    (void)rd;
    fclose(capture);
    /* The handlers emit exactly one JSON object per request, but some also
     * print diagnostics to stdout first (the current-trace cache line). Parse
     * from the LAST line, which is the response. */
    char *last = json;
    for (char *p = json; *p; p++)
        if (*p == '\n' && p[1]) last = p + 1;
    cJSON *out = cJSON_Parse(last);
    free(json);
    return out;
}

static cJSON *row_by_name(cJSON *rows, const char *name)
{
    cJSON *r;
    cJSON_ArrayForEach(r, rows) {
        cJSON *n = cJSON_GetObjectItem(r, "name");
        if (n && cJSON_IsString(n) && strcmp(n->valuestring, name) == 0)
            return r;
    }
    return NULL;
}

static cJSON *qrow_by_id(cJSON *rows, uint64_t qid)
{
    char want[32];
    snprintf(want, sizeof(want), "%llu", (unsigned long long)qid);
    cJSON *r;
    cJSON_ArrayForEach(r, rows) {
        cJSON *q = cJSON_GetObjectItem(r, "query_id");
        if (q && cJSON_IsString(q) && strcmp(q->valuestring, want) == 0)
            return r;
    }
    return NULL;
}

/* "Present AND JSON null" -- the distinction the whole file is about. An
 * ABSENT key is reported separately, because "the server stopped sending the
 * field" and "the server says it is not applicable" are different answers and
 * only one of them is the contract. */
static int is_json_null(cJSON *obj, const char *key)
{
    cJSON *v = cJSON_GetObjectItem(obj, key);
    return v != NULL && cJSON_IsNull(v);
}

static int is_json_number(cJSON *obj, const char *key)
{
    cJSON *v = cJSON_GetObjectItem(obj, key);
    return v != NULL && cJSON_IsNumber(v);
}

static double json_num(cJSON *obj, const char *key)
{
    cJSON *v = cJSON_GetObjectItem(obj, key);
    return (v && cJSON_IsNumber(v)) ? v->valuedouble : -1.0;
}

/* ── fixture A: idle + exact + sampled, over a long window ────────────
 *
 * WINDOW_SECS is above should_use_summaries' 120 s floor so section 2 can
 * exercise the SUMMARY route; sections 1 and 3 request a narrow sub-window
 * and take the raw route. Every second carries:
 *   - pid 601: one 10 ms Client:ClientRead  (idle-but-visible)
 *   - pid 601: one 10 ms Lock:relation, query QID_EXACT   (exact)
 *   - pid 602: one 10 ms IO:DataFileRead, query QID_SAMPLED, SAMPLE-flagged
 *   - pid 601: an EXEC_START/EXEC_END marker pair for QID_EXACT
 *   - pid 603: an EXEC_START/EXEC_END marker pair for QID_EXACT
 * Two pids running the same query is what makes section 3's pid drill
 * meaningful: a pid-scoped execution count would be half the whole-query one.
 */
#define WINDOW_SECS  130
#define PID_EXACT    601
#define PID_SAMPLED  602
#define PID_OTHER    603
#define QID_EXACT    0x1111111111111111ULL
#define QID_SAMPLED  0x2222222222222222ULL

static uint64_t fixture_base;

static void push(struct pgwt_event_writer *w, struct pgwt_summary_writer *sw,
                 const struct pgwt_trace_event *ev)
{
    pgwt_writer_push_event(w, ev);
    if (sw) pgwt_summary_push_event(sw, ev, 0, ev->old_event);
}

static void build_fixture_a(const char *dir, uint64_t base)
{
    struct pgwt_event_writer w;
    if (pgwt_writer_init(&w, dir, 180000, 0, NULL) != 0) {
        printf("  FATAL: writer init\n"); exit(2);
    }
    struct pgwt_summary_writer *sw = calloc(1, sizeof(*sw));
    if (!sw || pgwt_summary_writer_init(sw, dir, 24, NULL) != 0) {
        printf("  FATAL: summary writer init\n"); exit(2);
    }

    for (int s = 0; s < WINDOW_SECS; s++) {
        uint64_t sec = base + (uint64_t)s * ONE_SEC;
        struct pgwt_trace_event e;

        /* idle-but-visible */
        memset(&e, 0, sizeof(e));
        e.timestamp_ns = sec + 100 * MS;
        e.pid = PID_EXACT;
        e.old_event = EV_CLIENT_READ;
        e.new_event = EV_LOCK;
        e.duration_ns = 10 * MS;
        e.cpu_ns = PGWT_CPU_NS_UNKNOWN;
        push(&w, sw, &e);

        /* exact, attributed */
        memset(&e, 0, sizeof(e));
        e.timestamp_ns = sec + 200 * MS;
        e.pid = PID_EXACT;
        e.old_event = EV_LOCK;
        e.new_event = EV_IO;
        e.duration_ns = 10 * MS;
        e.query_id = QID_EXACT;
        e.cpu_ns = PGWT_CPU_NS_UNKNOWN;
        push(&w, sw, &e);

        /* Sampled, attributed to a DIFFERENT query, written as a REAL
         * SAMPLES block via pgwt_writer_push_samples -- not as a
         * push_event() with the flag set by hand. The on-disk TRANSITIONS
         * layout carries no flags field, so a hand-set FLAG_SAMPLE does not
         * survive the round trip; the reader derives it from the BLOCK TYPE
         * (src/event_reader.c, SAMPLES layout: wait id in new_event,
         * old_event and duration cleared, FLAG_SAMPLE set). The first draft
         * of this fixture set the flag by hand and the sampled row came back
         * with a latency average -- the test was green on a response that
         * contained the defect, because the fixture had quietly produced an
         * exact record. Deliberately NOT pushed into the summary writer: the
         * sampled tier has no per-second query record here, which is what
         * section 2b asserts. */
        memset(&e, 0, sizeof(e));
        e.timestamp_ns = sec + 300 * MS;
        e.pid = PID_SAMPLED;
        e.new_event = EV_IO;          /* a sample is a POINT observation */
        e.query_id = QID_SAMPLED;
        e.cpu_ns = PGWT_CPU_NS_UNKNOWN;
        pgwt_writer_push_samples(&w, &e, 1, 10 * MS);

        /* two execution marker pairs for QID_EXACT, on two different pids */
        const uint32_t pids[2] = { PID_EXACT, PID_OTHER };
        for (int k = 0; k < 2; k++) {
            memset(&e, 0, sizeof(e));
            e.timestamp_ns = sec + (uint64_t)(400 + k * 100) * MS;
            e.pid = pids[k];
            e.old_event = e.new_event = PGWT_MARKER_EXEC_START;
            e.query_id = QID_EXACT;
            e.cpu_ns = PGWT_CPU_NS_UNKNOWN;
            push(&w, sw, &e);

            memset(&e, 0, sizeof(e));
            e.timestamp_ns = sec + (uint64_t)(450 + k * 100) * MS;
            e.pid = pids[k];
            e.old_event = e.new_event = PGWT_MARKER_EXEC_END;
            e.query_id = QID_EXACT;
            e.cpu_ns = PGWT_CPU_NS_UNKNOWN;
            push(&w, sw, &e);
        }
    }

    pgwt_writer_close(&w);
    pgwt_summary_flush(sw);
    pgwt_summary_close(sw);
    pgwt_summary_destroy(sw);
    free(sw);
}

/* ── fixture C: EXACT ONLY, with summaries, for the summary route ──────
 *
 * should_use_summaries refuses any window that is not wholly EXACT (a mixed
 * window would be answered from per-second records that do not describe the
 * sampled part of it), so fixture A -- which contains real SAMPLES blocks on
 * purpose -- can never take the summary route. Section 2b therefore needs its
 * own exact-only capture. This separation was found by section 2b's own route
 * assertion going red, which is why that assertion exists: without it 2b
 * would have silently tested the raw path a second time and reported the
 * summary contract as verified.
 */
static void build_fixture_c(const char *dir, uint64_t base)
{
    struct pgwt_event_writer w;
    if (pgwt_writer_init(&w, dir, 180000, 0, NULL) != 0) {
        printf("  FATAL: writer init (c)\n"); exit(2);
    }
    struct pgwt_summary_writer *sw = calloc(1, sizeof(*sw));
    if (!sw || pgwt_summary_writer_init(sw, dir, 24, NULL) != 0) {
        printf("  FATAL: summary writer init (c)\n"); exit(2);
    }
    for (int s = 0; s < WINDOW_SECS; s++) {
        uint64_t sec = base + (uint64_t)s * ONE_SEC;
        struct pgwt_trace_event e;

        memset(&e, 0, sizeof(e));
        e.timestamp_ns = sec + 100 * MS;
        e.pid = PID_EXACT;
        e.old_event = EV_CLIENT_READ;
        e.new_event = EV_LOCK;
        e.duration_ns = 10 * MS;
        e.cpu_ns = PGWT_CPU_NS_UNKNOWN;
        push(&w, sw, &e);

        memset(&e, 0, sizeof(e));
        e.timestamp_ns = sec + 200 * MS;
        e.pid = PID_EXACT;
        e.old_event = EV_LOCK;
        e.new_event = EV_IO;
        e.duration_ns = 10 * MS;
        e.query_id = QID_EXACT;
        e.cpu_ns = PGWT_CPU_NS_UNKNOWN;
        push(&w, sw, &e);
    }
    pgwt_writer_close(&w);
    pgwt_summary_flush(sw);
    pgwt_summary_close(sw);
    pgwt_summary_destroy(sw);
    free(sw);
}

/* ── 1. #435: Overview's idle share is null, not 0 ────────────────────── */
static void test_overview_idle_share_is_null(struct pgwt_server *srv)
{
    printf("--- 1. #435: Overview idle %%DB is JSON null, never numeric 0 ---\n");

    struct pgwt_request req = {0};
    req.from_ns = srv->earliest_wall_ns;
    req.to_ns   = srv->earliest_wall_ns + 30 * ONE_SEC;   /* raw route */
    struct pgwt_wfid wf = {0};
    CHECK(!should_use_summaries(srv, &req, &wf),
          "the RAW route answers this 30 s window (the summary route is "
          "asserted separately in section 2; a section that does not know "
          "which path answered is not testing a path)");

    cJSON *out = call_handler(handle_time_model, srv, &req);
    CHECK(out != NULL, "handle_time_model produced parseable JSON");
    if (!out) return;
    CHECK(cJSON_GetObjectItem(out, "error") == NULL, "no error in response");

    cJSON *rows = cJSON_GetObjectItem(out, "rows");
    CHECK(cJSON_IsArray(rows) && cJSON_GetArraySize(rows) > 0,
          "rows is a non-empty array (%d rows)",
          cJSON_IsArray(rows) ? cJSON_GetArraySize(rows) : -1);

    /* POSITIVE CONTROL first: a non-idle row still carries a NUMBER. Without
     * this, "every idle pct is null" would also be satisfied by a server
     * that nulled the whole column. */
    cJSON *db = row_by_name(rows, "DB Time");
    CHECK(db != NULL, "the DB Time row is present");
    CHECK(db && is_json_number(db, "pct"),
          "…and its pct is a NUMBER (%.1f) -- the column is not blanket-null",
          db ? json_num(db, "pct") : -1.0);

    cJSON *idle = row_by_name(rows, "Idle");
    CHECK(idle != NULL, "the Idle row is present");
    if (idle) {
        CHECK(is_json_null(idle, "pct"),
              "…its pct is present AND JSON null (#435: it was the number 0, "
              "which the UI draws as a 0.0%% bar)");
        CHECK(cJSON_GetObjectItem(idle, "pct") != NULL,
              "…the key is still SENT (an absent key is a different answer "
              "from an explicit not-applicable)");
        /* The row still carries its time and its rate: #435 removes a
         * meaningless percentage, not the row. 130 ClientRead waits of 10 ms
         * each is 1300 ms, of which this 30 s window holds 30. */
        CHECK(json_num(idle, "ms") > 0.0,
              "…and it still reports its TIME (%.1f ms) -- the fix removes a "
              "percentage, not the row", json_num(idle, "ms"));
        CHECK(json_num(idle, "aas") > 0.0,
              "…and its AAS rate (%.4f), which IS defined for idle time",
              json_num(idle, "aas"));
    }

    /* Every named idle child, not just the parent. */
    cJSON *cr = row_by_name(rows, "Client:ClientRead");
    CHECK(cr != NULL, "the named Client:ClientRead child row is present");
    if (cr)
        CHECK(is_json_null(cr, "pct"),
              "…and its pct is null too (the children were 0 as well)");

    cJSON_Delete(out);
}

/* ── 2. #441a: the Queries latency average needs evidence ─────────────── */
static void test_queries_avg_gated(struct pgwt_server *srv)
{
    printf("--- 2. #441: Queries avg_us is null without full durations ---\n");

    /* 2a. RAW route: the sampled-only query. */
    {
        struct pgwt_request req = {0};
        req.from_ns = srv->earliest_wall_ns;
        req.to_ns   = srv->earliest_wall_ns + 30 * ONE_SEC;
        struct pgwt_wfid wf = {0};
        CHECK(!should_use_summaries(srv, &req, &wf),
              "2a. the RAW route answers this 30 s window");

        cJSON *out = call_handler(handle_top_queries, srv, &req);
        CHECK(out != NULL, "2a. parseable JSON");
        if (!out) return;
        CHECK(cJSON_GetObjectItem(out, "error") == NULL, "2a. no error");
        cJSON *rows = cJSON_GetObjectItem(out, "rows");

        cJSON *qe = qrow_by_id(rows, QID_EXACT);
        CHECK(qe != NULL, "2a. the exact query has a row");
        /* POSITIVE CONTROL: the exact query DOES get a number, so "null" is
         * a judgement about evidence and not a blanket suppression. */
        CHECK(qe && is_json_number(qe, "avg_us"),
              "2a. …and its avg_us is a number (%.1f us)",
              qe ? json_num(qe, "avg_us") : -1.0);

        cJSON *qs = qrow_by_id(rows, QID_SAMPLED);
        CHECK(qs != NULL, "2a. the sampled-only query has a row (its count "
                          "and DB Time are valid ASH math)");
        if (qs) {
            CHECK(json_num(qs, "total_ms") > 0.0,
                  "2a. …carrying real DB Time (%.1f ms)",
                  json_num(qs, "total_ms"));
            CHECK(json_num(qs, "count") > 0.0,
                  "2a. …and real observations (%.0f)", json_num(qs, "count"));
            CHECK(is_json_null(qs, "avg_us"),
                  "2a. …but avg_us is JSON null: a sample period is not a "
                  "measured latency");
        }
        cJSON_Delete(out);
    }

}

/* ── 2b. #441: the summary route has no latency evidence of its own ─────
 *
 * pgwt_compute_top_queries_from_summaries now declares avg_us UNAVAILABLE --
 * the per-second records carry no full-duration sum, so the quantity it used
 * to publish (in-window DB Time / interval count) was not a latency at all.
 * handle_top_queries backfills the column from the raw pass it ALREADY runs
 * for the lifecycle statistics, so the served number is the raw definition
 * computed in ONE place, rather than two formulas that happen to agree on
 * fixtures where nothing is clipped.
 *
 * Needs its own exact-only fixture: see build_fixture_c.
 */
static void test_queries_avg_summary_route(struct pgwt_server *srv)
{
    printf("--- 2b. #441: the summary route has no latency evidence ---\n");

    /* The raw answer for the SAME window, as the comparison target. A pid
     * filter is the cheapest way to force the raw route here and nothing
     * else about it matters: the fixture has exactly one busy pid, so the
     * pid-filtered and unfiltered answers describe the same records. */
    struct pgwt_request raw_req = {0};
    raw_req.from_ns = srv->earliest_wall_ns;
    raw_req.to_ns   = srv->latest_wall_ns;
    raw_req.filter.pid = PID_EXACT;
    struct pgwt_wfid rwf = {0};
    CHECK(!should_use_summaries(srv, &raw_req, &rwf),
          "2b. the pid-filtered request takes the RAW route (the comparison "
          "target must not be the thing being compared)");
    cJSON *raw_out = call_handler(handle_top_queries, srv, &raw_req);
    double raw_avg = raw_out ? json_num(qrow_by_id(
        cJSON_GetObjectItem(raw_out, "rows"), QID_EXACT), "avg_us") : -1.0;
    CHECK(raw_avg > 0.0, "2b. …and it reports a real avg_us (%.1f us)", raw_avg);

    struct pgwt_request req = {0};
    req.from_ns = srv->earliest_wall_ns;
    req.to_ns   = srv->latest_wall_ns;
    struct pgwt_wfid wf = {0};
    CHECK(should_use_summaries(srv, &req, &wf),
          "2b. the SUMMARY route answers the whole %d s window -- otherwise "
          "this section tests the raw path twice and proves nothing about "
          "the summary one", WINDOW_SECS);

    cJSON *out = call_handler(handle_top_queries, srv, &req);
    CHECK(out != NULL, "2b. parseable JSON");
    if (out) {
        cJSON *rows = cJSON_GetObjectItem(out, "rows");
        cJSON *qe = qrow_by_id(rows, QID_EXACT);
        CHECK(qe != NULL, "2b. the exact query has a summary row");
        CHECK(qe && json_num(qe, "total_ms") > 0.0,
              "2b. …with real summary DB Time (%.1f ms), so the row is not "
              "empty", qe ? json_num(qe, "total_ms") : -1.0);
        CHECK(qe && is_json_number(qe, "avg_us"),
              "2b. …and its avg_us is backfilled from raw (%.1f us)",
              qe ? json_num(qe, "avg_us") : -1.0);
        CHECK(raw_avg > 0.0 && qe &&
              fabs(json_num(qe, "avg_us") - raw_avg) < 1e-6,
              "2b. …bit-identical to the raw route's %.1f us", raw_avg);
        cJSON_Delete(out);
    }

    /* AND the compute layer underneath it, directly. The backfill above is
     * the SERVER's doing; if the summary compute path started inventing a
     * number again, every assertion above would still pass, because the
     * backfill overwrites whatever it produced. This is the assertion that
     * can see that. */
    struct pgwt_queries_result sum;
    struct pgwt_filter none = {0};
    pgwt_compute_top_queries_from_summaries(srv->trace_dir,
                                            srv->earliest_wall_ns,
                                            srv->latest_wall_ns,
                                            &none, 0.0, &sum);
    CHECK(!sum.failed && sum.num_rows > 0,
          "2b. the summary compute returned %d row(s) of its own", sum.num_rows);
    int any_claim = 0;
    for (int i = 0; i < sum.num_rows; i++)
        if (sum.rows[i].exact_count != 0 || sum.rows[i].avg_us != 0.0)
            any_claim = 1;
    CHECK(!any_claim,
          "2b. …and not one of them claims full-duration evidence "
          "(exact_count 0, avg_us 0 throughout): the per-second records have "
          "none to claim");
    free(sum.rows);
    if (raw_out) cJSON_Delete(raw_out);
}

/* ── 3. #441c: execution statistics under a PID drill ─────────────────── */
static void test_exec_stats_scope(struct pgwt_server *srv)
{
    printf("--- 3. #441: exec/plan stats are whole-query, or suppressed ---\n");

    /* 3a. POSITIVE CONTROL, and the scope declaration. */
    struct pgwt_request req = {0};
    req.from_ns = srv->earliest_wall_ns;
    req.to_ns   = srv->earliest_wall_ns + 30 * ONE_SEC;
    cJSON *out = call_handler(handle_top_queries, srv, &req);
    CHECK(out != NULL, "3a. parseable JSON");
    if (!out) return;
    cJSON *scope = cJSON_GetObjectItem(out, "exec_stats_scope");
    CHECK(scope && cJSON_IsString(scope) &&
          strcmp(scope->valuestring, "whole-query") == 0,
          "3a. unfiltered: exec_stats_scope is \"whole-query\" (got %s)",
          scope && cJSON_IsString(scope) ? scope->valuestring : "(absent)");
    cJSON *qe = qrow_by_id(cJSON_GetObjectItem(out, "rows"), QID_EXACT);
    CHECK(qe != NULL, "3a. the exact query has a row");
    double whole_execs = qe ? json_num(qe, "exec_count") : -1.0;
    CHECK(qe && is_json_number(qe, "exec_count") && whole_execs > 0,
          "3a. …with a real exec_count (%.0f) -- the markers ARE there, so "
          "section 3b's absence cannot be an empty fixture", whole_execs);
    cJSON_Delete(out);

    /* 3b. A PID DRILL. The scan is not pid-scoped, so the statistics are
     * suppressed rather than answered at the wrong scope. The fixture runs
     * QID_EXACT on TWO pids, so a wrong-scope answer is not merely
     * mislabelled -- it is off by a factor of two. */
    struct pgwt_request preq = {0};
    preq.from_ns = srv->earliest_wall_ns;
    preq.to_ns   = srv->earliest_wall_ns + 30 * ONE_SEC;
    preq.filter.pid = PID_EXACT;
    cJSON *pout = call_handler(handle_top_queries, srv, &preq);
    CHECK(pout != NULL, "3b. parseable JSON under a pid drill");
    if (!pout) return;
    cJSON *pscope = cJSON_GetObjectItem(pout, "exec_stats_scope");
    CHECK(pscope && cJSON_IsString(pscope) &&
          strcmp(pscope->valuestring, "unavailable") == 0,
          "3b. pid drill: exec_stats_scope is \"unavailable\" (got %s)",
          pscope && cJSON_IsString(pscope) ? pscope->valuestring : "(absent)");
    CHECK(cJSON_GetObjectItem(pout, "exec_stats_unavailable_reason") != NULL,
          "3b. …with a reason, so the UI can say WHY rather than just \"—\"");
    cJSON *pqe = qrow_by_id(cJSON_GetObjectItem(pout, "rows"), QID_EXACT);
    CHECK(pqe != NULL, "3b. the query still has a row (its wait profile IS "
                       "pid-scoped and stays)");
    if (pqe) {
        CHECK(json_num(pqe, "total_ms") > 0.0,
              "3b. …with real pid-scoped DB Time (%.1f ms)",
              json_num(pqe, "total_ms"));
        CHECK(is_json_null(pqe, "exec_count"),
              "3b. …but exec_count is JSON null, not the whole-query %.0f",
              whole_execs);
        CHECK(is_json_null(pqe, "avg_exec_ms"),
              "3b. …and avg_exec_ms is null (an average over the wrong "
              "population is not a smaller error than a count over it)");
        CHECK(is_json_null(pqe, "plan_count"), "3b. …and plan_count is null");
    }
    cJSON_Delete(pout);

    /* 3c. A CLASS filter must NOT suppress them: narrowing the wait profile
     * to Lock does not change how long the query took to execute, and
     * rescoping execution timing to "executions containing a Lock wait"
     * would answer a different question under the same heading. */
    struct pgwt_request creq = {0};
    creq.from_ns = srv->earliest_wall_ns;
    creq.to_ns   = srv->earliest_wall_ns + 30 * ONE_SEC;
    snprintf(creq.filter.class_name, sizeof(creq.filter.class_name), "Lock");
    cJSON *cout = call_handler(handle_top_queries, srv, &creq);
    CHECK(cout != NULL, "3c. parseable JSON under a class filter");
    if (!cout) return;
    cJSON *cscope = cJSON_GetObjectItem(cout, "exec_stats_scope");
    CHECK(cscope && cJSON_IsString(cscope) &&
          strcmp(cscope->valuestring, "whole-query") == 0,
          "3c. class filter: exec_stats_scope stays \"whole-query\" (got %s)",
          cscope && cJSON_IsString(cscope) ? cscope->valuestring : "(absent)");
    cJSON *cqe = qrow_by_id(cJSON_GetObjectItem(cout, "rows"), QID_EXACT);
    CHECK(cqe && is_json_number(cqe, "exec_count") &&
          fabs(json_num(cqe, "exec_count") - whole_execs) < 1e-9,
          "3c. …and exec_count is the SAME whole-query %.0f, deliberately "
          "unaffected by the class filter (got %.0f)",
          whole_execs, cqe ? json_num(cqe, "exec_count") : -1.0);
    cJSON_Delete(cout);
}

/* ── fixture B + section 4: #436 node population == edge population ───── */

/* pid 701 alternates Lock:relation -> IO:DataFileRead (10 ms) and
 * IO:DataFileRead -> Lock:relation (20 ms), 10 of each. So:
 *   edge Lock->IO : count 10, dwell 100 ms   (source state = Lock)
 *   edge IO->Lock : count 10, dwell 200 ms   (source state = IO)
 * Under filter class=Lock only the first survives, so the node table must
 * hold Lock:relation ALONE at 100 ms. On the parent tree the node pass saw
 * every record, so it also published an IO:DataFileRead node of 200 ms --
 * twice the mass of the only edge in the picture, attached to nothing. */
#define PID_GRAPH 701

static void build_fixture_b(const char *dir, uint64_t base)
{
    struct pgwt_event_writer w;
    if (pgwt_writer_init(&w, dir, 180000, 0, NULL) != 0) {
        printf("  FATAL: writer init (b)\n"); exit(2);
    }
    for (int i = 0; i < 10; i++) {
        uint64_t sec = base + (uint64_t)i * ONE_SEC;
        struct pgwt_trace_event e;

        memset(&e, 0, sizeof(e));
        e.timestamp_ns = sec + 100 * MS;
        e.pid = PID_GRAPH;
        e.old_event = EV_LOCK;
        e.new_event = EV_IO;
        e.duration_ns = 10 * MS;
        e.cpu_ns = PGWT_CPU_NS_UNKNOWN;
        pgwt_writer_push_event(&w, &e);

        memset(&e, 0, sizeof(e));
        e.timestamp_ns = sec + 300 * MS;
        e.pid = PID_GRAPH;
        e.old_event = EV_IO;
        e.new_event = EV_LOCK;
        e.duration_ns = 20 * MS;
        e.cpu_ns = PGWT_CPU_NS_UNKNOWN;
        pgwt_writer_push_event(&w, &e);
    }
    pgwt_writer_close(&w);
}

struct graph_sums { double node_ms; double link_count; int n_nodes; int n_links; };

static struct graph_sums graph_totals(cJSON *out)
{
    struct graph_sums g = {0.0, 0.0, 0, 0};
    cJSON *nodes = cJSON_GetObjectItem(out, "nodes");
    cJSON *n;
    cJSON_ArrayForEach(n, nodes) {
        g.node_ms += json_num(n, "total_ms");
        g.n_nodes++;
    }
    cJSON *links = cJSON_GetObjectItem(out, "links");
    cJSON *l;
    cJSON_ArrayForEach(l, links) {
        g.link_count += json_num(l, "value");
        g.n_links++;
    }
    return g;
}

static int has_node(cJSON *out, const char *name)
{
    return row_by_name(cJSON_GetObjectItem(out, "nodes"), name) != NULL;
}

static double node_ms(cJSON *out, const char *name)
{
    cJSON *n = row_by_name(cJSON_GetObjectItem(out, "nodes"), name);
    return n ? json_num(n, "total_ms") : -1.0;
}

static void test_graph_node_population(const char *dir, uint64_t base)
{
    printf("--- 4. #436: graph nodes and edges share one population ---\n");

    struct pgwt_server *srv = calloc(1, sizeof(*srv));
    if (!srv || server_init(srv, dir) != 0) {
        printf("  FATAL: server_init (b)\n"); exit(2);
    }
    (void)base;

    /* 4a. UNFILTERED: both nodes exist, and the counts conserve. */
    struct pgwt_request req = {0};
    req.from_ns = srv->earliest_wall_ns;
    req.to_ns   = srv->latest_wall_ns + 1;
    req.num_buckets = 200;          /* max_rows: nothing may be truncated */
    cJSON *out = call_handler(handle_transitions, srv, &req);
    CHECK(out != NULL, "4a. parseable JSON");
    if (!out) return;
    CHECK(cJSON_GetObjectItem(out, "error") == NULL, "4a. no error");
    cJSON *trunc = cJSON_GetObjectItem(out, "truncated");
    CHECK(trunc && cJSON_IsFalse(trunc),
          "4a. nothing truncated -- a truncated edge list would break the "
          "conservation check below for a legitimate reason");
    struct graph_sums g = graph_totals(out);
    CHECK(g.n_nodes == 2, "4a. two nodes (got %d)", g.n_nodes);
    CHECK(g.n_links == 2, "4a. two edges (got %d)", g.n_links);
    /* POSITIVE CONTROL for 4b: the IO node is REALLY in the unfiltered
     * answer, so 4b's "no IO node" cannot be a fixture with no IO records. */
    CHECK(has_node(out, "IO:DataFileRead"),
          "4a. the IO node IS present unfiltered (200 ms of dwell)");
    CHECK(fabs(node_ms(out, "IO:DataFileRead") - 200.0) < 1e-6,
          "4a. …at its full 200 ms (got %.3f)",
          node_ms(out, "IO:DataFileRead"));
    CHECK(fabs(node_ms(out, "Lock:relation") - 100.0) < 1e-6,
          "4a. and the Lock node at 100 ms (got %.3f)",
          node_ms(out, "Lock:relation"));
    /* CONSERVATION. Each counted record contributes its dwell to exactly one
     * node (its old_event) and to exactly one edge (old->new), so the two
     * totals are equal iff the two populations are. The two sides are
     * computed by different code over different accumulators -- block_agg's
     * node table and compute.c's pair hash -- so this is a comparison, not
     * one sum against itself. */
    CHECK(fabs(g.node_ms - 300.0) < 1e-6,
          "4a. Σ node dwell == the fixture's 300 ms (got %.3f)", g.node_ms);
    CHECK(fabs(g.link_count - 20.0) < 1e-6,
          "4a. Σ edge count == the fixture's 20 records (got %.0f)",
          g.link_count);
    cJSON_Delete(out);

    /* 4b. CLASS FILTER: the node population must narrow with the edges. */
    struct pgwt_request freq = {0};
    freq.from_ns = srv->earliest_wall_ns;
    freq.to_ns   = srv->latest_wall_ns + 1;
    freq.num_buckets = 200;
    snprintf(freq.filter.class_name, sizeof(freq.filter.class_name), "Lock");
    cJSON *fout = call_handler(handle_transitions, srv, &freq);
    CHECK(fout != NULL, "4b. parseable JSON under a class filter");
    if (!fout) return;
    CHECK(cJSON_GetObjectItem(fout, "error") == NULL, "4b. no error");
    struct graph_sums fg = graph_totals(fout);
    CHECK(fg.n_links == 1, "4b. one edge survives the Lock filter (got %d)",
          fg.n_links);
    CHECK(fabs(fg.link_count - 10.0) < 1e-6,
          "4b. …carrying the 10 Lock->IO records (got %.0f)", fg.link_count);
    CHECK(fg.n_nodes == 1,
          "4b. and exactly ONE node (got %d) -- the parent tree published 2, "
          "the second attached to no edge in the response", fg.n_nodes);
    CHECK(has_node(fout, "Lock:relation"),
          "4b. the surviving node is Lock:relation (the filtered class)");
    CHECK(!has_node(fout, "IO:DataFileRead"),
          "4b. and IO:DataFileRead is GONE -- its 200 ms of dwell belonged to "
          "records this response excluded");
    CHECK(fabs(fg.node_ms - 100.0) < 1e-6,
          "4b. Σ node dwell is the filtered 100 ms, not the unfiltered 300 "
          "(got %.3f)", fg.node_ms);
    cJSON_Delete(fout);

    /* 4c. A filter that matches NOTHING. Absence must be an empty graph, not
     * a full one: this is the shape in which an unfiltered node pass is most
     * visible, because every node in the answer is provably wrong. */
    struct pgwt_request nreq = {0};
    nreq.from_ns = srv->earliest_wall_ns;
    nreq.to_ns   = srv->latest_wall_ns + 1;
    nreq.num_buckets = 200;
    snprintf(nreq.filter.class_name, sizeof(nreq.filter.class_name), "LWLock");
    cJSON *nout = call_handler(handle_transitions, srv, &nreq);
    CHECK(nout != NULL, "4c. parseable JSON for a filter matching nothing");
    if (nout) {
        struct graph_sums ng = graph_totals(nout);
        CHECK(ng.n_links == 0, "4c. no edges (got %d)", ng.n_links);
        CHECK(ng.n_nodes == 0,
              "4c. and no nodes (got %d) -- 300 ms of dwell under a filter "
              "that matched nothing is the clearest form of the defect",
              ng.n_nodes);
        cJSON_Delete(nout);
    }

    free(srv);
}

/* ── 5. bypass suite: the ways these sections could stop seeing ───────── */
static void test_bypass(const char *dir_b)
{
    printf("--- 5. bypass suite ---\n");

    /* 5a. An EMPTY trace directory. Every section above is satisfiable by a
     * response with no rows at all, so the one thing this suite must never
     * do is treat emptiness as agreement. A server over an empty dir must
     * not produce a graph. */
    char empty[] = "/tmp/pgwt_semwire_empty_XXXXXX";
    CHECK(mkdtemp(empty) != NULL, "5a. made an empty trace dir");
    struct pgwt_server *es = calloc(1, sizeof(*es));
    int erc = es ? server_init(es, empty) : -1;
    if (erc == 0) {
        struct pgwt_request req = {0};
        req.num_buckets = 200;
        cJSON *out = call_handler(handle_transitions, es, &req);
        /* Either a structured unavailable/error, or an empty graph. What it
         * must NOT be is a populated one. */
        int populated = 0;
        if (out) {
            struct graph_sums g = graph_totals(out);
            populated = (g.n_nodes > 0 || g.n_links > 0);
            cJSON_Delete(out);
        }
        CHECK(!populated,
              "5a. an empty trace dir yields no graph (a populated one would "
              "mean section 4 could pass on fabricated data)");
    } else {
        CHECK(1, "5a. server_init REFUSED an empty trace dir (also correct: "
                 "absence is never an answer)");
    }
    free(es);
    rmdir(empty);

    /* 5b. A WINDOW THAT MISSES EVERY RECORD. Section 4's assertions about
     * what is absent must not be satisfiable by looking at the wrong time. */
    struct pgwt_server *srv = calloc(1, sizeof(*srv));
    if (!srv || server_init(srv, dir_b) != 0) {
        printf("  FATAL: server_init (bypass)\n"); exit(2);
    }
    struct pgwt_request miss = {0};
    miss.from_ns = srv->latest_wall_ns + 3600 * ONE_SEC;
    miss.to_ns   = miss.from_ns + 60 * ONE_SEC;
    miss.num_buckets = 200;
    cJSON *mout = call_handler(handle_transitions, srv, &miss);
    if (mout) {
        struct graph_sums mg = graph_totals(mout);
        CHECK(mg.n_nodes == 0 && mg.n_links == 0,
              "5b. a window an hour past the capture yields nothing "
              "(%d nodes, %d edges)", mg.n_nodes, mg.n_links);
        cJSON_Delete(mout);
    } else {
        CHECK(1, "5b. a window past the capture was REFUSED outright");
    }

    /* 5c. THE THING BEING CHECKED IS ABSENT RATHER THAN WRONG. Fixture B has
     * no idle records and no query ids, so handle_time_model over it has no
     * Idle row at all. Section 1 must be reading a fixture that HAS one --
     * asserted there by requiring the row to exist before testing its pct.
     * Pinned here from the other side: on a fixture with no idle time the row
     * is absent, which is NOT the same as a null pct, and a section that
     * confused the two would pass on fixture B. */
    struct pgwt_request tm = {0};
    tm.from_ns = srv->earliest_wall_ns;
    tm.to_ns   = srv->latest_wall_ns + 1;
    cJSON *tout = call_handler(handle_time_model, srv, &tm);
    CHECK(tout != NULL, "5c. parseable time model over the idle-free fixture");
    if (tout) {
        cJSON *rows = cJSON_GetObjectItem(tout, "rows");
        CHECK(cJSON_IsArray(rows) && cJSON_GetArraySize(rows) > 0,
              "5c. …which is NOT empty (%d rows): this fixture has DB Time, "
              "just no idle time", cJSON_GetArraySize(rows));
        CHECK(row_by_name(rows, "Idle") == NULL,
              "5c. …and carries no Idle row at all -- absent, not null");
        cJSON_Delete(tout);
    }
    free(srv);
}

int main(void)
{
    printf("=== test_semantics_wire ===\n");

    /* A wall-clock-derived base, like a real trace: server_init discovers
     * coverage from the file header, and should_use_summaries compares a
     * window WIDTH, so nothing here depends on the current time beyond the
     * fixture being internally consistent. */
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    fixture_base = ((uint64_t)ts.tv_sec * ONE_SEC + (uint64_t)ts.tv_nsec);
    fixture_base = (fixture_base / ONE_SEC) * ONE_SEC;

    char dir_a[] = "/tmp/pgwt_semwire_a_XXXXXX";
    if (!mkdtemp(dir_a)) { printf("FATAL: mkdtemp a\n"); return 2; }
    build_fixture_a(dir_a, fixture_base);

    struct pgwt_server *srv = calloc(1, sizeof(*srv));
    if (!srv || server_init(srv, dir_a) != 0) {
        printf("FATAL: server_init a\n"); return 2;
    }

    test_overview_idle_share_is_null(srv);
    test_queries_avg_gated(srv);
    test_exec_stats_scope(srv);
    free(srv);

    char dir_c[] = "/tmp/pgwt_semwire_c_XXXXXX";
    if (!mkdtemp(dir_c)) { printf("FATAL: mkdtemp c\n"); return 2; }
    build_fixture_c(dir_c, fixture_base);
    struct pgwt_server *srv_c = calloc(1, sizeof(*srv_c));
    if (!srv_c || server_init(srv_c, dir_c) != 0) {
        printf("FATAL: server_init c\n"); return 2;
    }
    test_queries_avg_summary_route(srv_c);
    free(srv_c);

    char dir_b[] = "/tmp/pgwt_semwire_b_XXXXXX";
    if (!mkdtemp(dir_b)) { printf("FATAL: mkdtemp b\n"); return 2; }
    build_fixture_b(dir_b, fixture_base);
    test_graph_node_population(dir_b, fixture_base);
    test_bypass(dir_b);

    /* A suite that ran no assertions proved nothing, and a FLOOR that does
     * not move with the suite lets a whole section stop running unnoticed.
     * 74 assertions today. */
    if (checks < 70) {
        printf("FAIL: only %d assertions ran -- the suite did not execute\n",
               checks);
        return 1;
    }

    printf("\n%d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
