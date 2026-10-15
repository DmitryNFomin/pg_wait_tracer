/* test_top_sessions_backfill.c — #319: top_wait answers a DIFFERENT question
 * on the summary path (longest single wait, per second) than on the raw
 * path (largest per-event TOTAL over the whole window), on BOTH the
 * Sessions and the Top Queries tabs (struct pgwt_summary_session and struct
 * pgwt_summary_query each keep their own per-second "dur > top_wait_ns",
 * same shape, same bug). handle_top_sessions and handle_top_queries now
 * each backfill top_wait from a raw computation over the SAME window
 * already being served, so every other field (db_time_ms, cpu_pct,
 * wait_pct / count, total_ms, the row set) stays the cheap summary answer
 * and only top_wait becomes the bit-exact raw one.
 *
 * Section 1 proves the Sessions backfill: a session (pid 501, query id
 * TARGET_QID) whose summary-only top_wait would read IO:WalSync (one 40 ms
 * wait, the single longest interval any second records) while its raw
 * total is dominated by Lock:relation (10 x 10 ms = 100 ms). Without the
 * backfill, handle_top_sessions reports IO:WalSync — this file pins the GAP
 * by computing the summary-only answer directly off the same files and
 * asserting it disagrees with raw, so a revert of the backfill is caught
 * (demonstrated in the PR body's red-run log).
 *
 * Section 2 proves the Sessions fail-safe: when the raw load the backfill
 * needs would itself overload (PGWT_LOAD_MAX_EVENTS set tiny), the response
 * reports top_wait_unavailable with a reason and top_wait: null — never the
 * (possibly wrong) summary-only name, and never a silently blank field. Run
 * in a forked child so the process-wide load_max_events() cache picked up
 * by the child's deliberately small override never leaks into section 1.
 *
 * Section 3 proves the Top Queries backfill, same fixture (the same
 * TARGET_PID events all carry query_id TARGET_QID): the served row for
 * TARGET_QID must read Lock, not IO. handle_top_queries loads its raw
 * events UNCONDITIONALLY (lifecycle stats need them regardless of route),
 * so its backfill has no analogous overload case to pin here — a raw load
 * that would overload fails the WHOLE request earlier, through the existing
 * reject_overload, unchanged by this fix; only an allocation failure inside
 * the backfill's own compute could produce top_wait_unavailable there, and
 * that shares the same reporting path pinned by section 2.
 */
#define main pgwt_server_main
#include "../src/server.c"
#undef main
#include "idle_rule.h"

#include <sys/wait.h>
#include <unistd.h>

static int tests_run = 0;
static int tests_failed = 0;

#define CHECK(cond, fmt, ...) do {                                     \
    tests_run++;                                                       \
    if (cond) { }                                                      \
    else { tests_failed++;                                             \
           printf("  FAIL(line %d): " fmt "\n", __LINE__, ##__VA_ARGS__); } \
} while (0)

#define MS      1000000ULL
#define ONE_SEC 1000000000ULL

/* PG18 ids, same convention tests/test_idle_summary.c pins by name. */
#define EV_LOCK WEI(PG_WAIT_LOCK, 0)       /* raw winner: 10 x 10ms = 100ms */
#define EV_IO   WEI(PG_WAIT_IO, 21)        /* summary winner: one 40ms wait */

static uint64_t mono_origin(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    uint64_t mono = (uint64_t)ts.tv_sec * ONE_SEC + (uint64_t)ts.tv_nsec;
    return (mono / ONE_SEC) * ONE_SEC;
}

#define WINDOW_SECS 130      /* > should_use_summaries' 120s floor */
#define TARGET_PID  501
#define FILLER_PID  777
#define TARGET_QID  0x5151515151515151ULL

/* Build the fixture directory: a real trace file AND a real summary file
 * over the same events, so should_use_summaries sees EXACT fidelity and
 * pgwt_compute_top_sessions_from_summaries has real records to read.
 *
 * Every second carries one FILLER_PID CPU tick (keeps raw coverage
 * continuous for the whole window). Ten of the seconds (0..9) additionally
 * carry one TARGET_PID Lock:relation wait of 10 ms. One further second (100)
 * carries a single TARGET_PID IO:WalSync wait of 40 ms — longer than any
 * individual Lock interval, so it is the per-second "longest single wait"
 * in every record that matters, but its 40 ms total loses to Lock's summed
 * 100 ms under the raw definition. */
static void build_fixture(const char *dir, uint64_t base_mono, int with_summaries)
{
    struct pgwt_event_writer writer;
    if (pgwt_writer_init(&writer, dir, 180000, 0, NULL) != 0) {
        printf("  FAIL: trace writer init\n"); tests_failed++; return;
    }
    struct pgwt_summary_writer *sw = NULL;
    if (with_summaries) {
        sw = calloc(1, sizeof(*sw));
        if (!sw || pgwt_summary_writer_init(sw, dir, 24, NULL) != 0) {
            printf("  FAIL: summary writer init\n"); tests_failed++; return;
        }
    }

    for (int s = 0; s < WINDOW_SECS; s++) {
        uint64_t sec = base_mono + (uint64_t)s * ONE_SEC;

        struct pgwt_trace_event filler;
        memset(&filler, 0, sizeof(filler));
        filler.timestamp_ns = sec + 10 * MS;
        filler.pid = FILLER_PID;
        filler.old_event = 0;              /* CPU */
        filler.new_event = EV_LOCK;
        filler.duration_ns = 1 * MS;
        filler.cpu_ns = PGWT_CPU_NS_UNKNOWN;
        pgwt_writer_push_event(&writer, &filler);
        if (sw) pgwt_summary_push_event(sw, &filler, 0, filler.old_event);

        if (s < 10) {
            struct pgwt_trace_event e;
            memset(&e, 0, sizeof(e));
            e.timestamp_ns = sec + 50 * MS;
            e.pid = TARGET_PID;
            e.old_event = EV_LOCK;
            e.new_event = EV_IO;
            e.duration_ns = 10 * MS;
            e.query_id = TARGET_QID;
            e.cpu_ns = PGWT_CPU_NS_UNKNOWN;
            pgwt_writer_push_event(&writer, &e);
            if (sw) pgwt_summary_push_event(sw, &e, 0, e.old_event);
        }
        if (s == 100) {
            struct pgwt_trace_event e;
            memset(&e, 0, sizeof(e));
            e.timestamp_ns = sec + 50 * MS;
            e.pid = TARGET_PID;
            e.old_event = EV_IO;
            e.new_event = EV_LOCK;
            e.duration_ns = 40 * MS;
            e.query_id = TARGET_QID;
            e.cpu_ns = PGWT_CPU_NS_UNKNOWN;
            pgwt_writer_push_event(&writer, &e);
            if (sw) pgwt_summary_push_event(sw, &e, 0, e.old_event);
        }
    }

    pgwt_writer_close(&writer);
    if (sw) {
        pgwt_summary_flush(sw);
        pgwt_summary_close(sw);
        pgwt_summary_destroy(sw);
        free(sw);
    }
}

/* ROUTE ASSERTION (bypass-suite plug). Sections 1 and 3 assert the SERVED
 * top_wait is the raw winner -- but the raw path produces that answer too,
 * for free. So if should_use_summaries ever stops returning true for this
 * fixture (a missing/old-version summary file, a window that slips under
 * the 120 s floor, a future routing change), the handler would serve raw,
 * both sections would stay GREEN, and the backfill they exist to pin would
 * never execute. This is the exact false negative the gate must refuse
 * rather than approve: every section that claims to test the backfill
 * first proves the backfill's route was taken. Section 4 proves this
 * predicate is not vacuously true by showing it FALSE on a fixture with no
 * summary file. */
static int route_is_summaries(struct pgwt_server *srv, struct pgwt_request *req)
{
    struct pgwt_wfid wf = {0};
    return should_use_summaries(srv, req, &wf);
}

/* Call a handler, capture its one line of JSON from stdout. */
typedef void (*handler_fn)(struct pgwt_server *, struct pgwt_request *);

static cJSON *call_handler(handler_fn fn, struct pgwt_server *srv,
                           struct pgwt_request *req)
{
    FILE *capture = tmpfile();
    if (!capture) { printf("  FAIL: tmpfile\n"); tests_failed++; return NULL; }
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
    size_t rd = fread(json, 1, (size_t)size, capture);
    (void)rd;
    fclose(capture);
    cJSON *out = cJSON_Parse(json);
    free(json);
    return out;
}

static cJSON *call_top_sessions(struct pgwt_server *srv, struct pgwt_request *req)
{
    return call_handler(handle_top_sessions, srv, req);
}

static cJSON *call_top_queries(struct pgwt_server *srv, struct pgwt_request *req)
{
    return call_handler(handle_top_queries, srv, req);
}

static cJSON *find_row(cJSON *rows, uint32_t pid)
{
    cJSON *row;
    cJSON_ArrayForEach(row, rows) {
        cJSON *p = cJSON_GetObjectItem(row, "pid");
        if (p && (uint32_t)p->valuedouble == pid)
            return row;
    }
    return NULL;
}

static cJSON *find_query_row(cJSON *rows, uint64_t query_id)
{
    char want[32];
    snprintf(want, sizeof(want), "%llu", (unsigned long long)query_id);
    cJSON *row;
    cJSON_ArrayForEach(row, rows) {
        cJSON *q = cJSON_GetObjectItem(row, "query_id");
        if (q && cJSON_IsString(q) && strcmp(q->valuestring, want) == 0)
            return row;
    }
    return NULL;
}

/* ── Section 1: the backfill overrides the summary-only answer ─────────── */
static void test_backfill_matches_raw(const char *dir, uint64_t base_mono)
{
    printf("--- 1. backfill matches raw, not the summary-only longest wait ---\n");

    struct pgwt_server *srv = calloc(1, sizeof(*srv));
    CHECK(srv != NULL && server_init(srv, dir) == 0, "server_init");
    if (!srv) return;

    (void)base_mono;
    struct pgwt_request req = {0};
    req.from_ns = 0;
    req.to_ns = 0; /* use server's own discovered coverage */

    CHECK(route_is_summaries(srv, &req),
          "the SUMMARY route is taken for this request -- otherwise the raw "
          "path answers it, the backfill never runs, and every assertion "
          "below passes without testing anything");

    cJSON *out = call_top_sessions(srv, &req);
    CHECK(out != NULL, "handle_top_sessions produced parseable JSON");
    if (!out) return;
    CHECK(cJSON_GetObjectItem(out, "error") == NULL, "no error in response");
    CHECK(cJSON_GetObjectItem(out, "top_wait_unavailable") == NULL,
          "top_wait is available for an ordinary window");

    cJSON *fidelity = cJSON_GetObjectItem(out, "fidelity");
    CHECK(fidelity && cJSON_IsString(fidelity) &&
          strcmp(fidelity->valuestring, "exact") == 0,
          "served exact fidelity (got %s)",
          fidelity && cJSON_IsString(fidelity) ? fidelity->valuestring : "?");

    cJSON *rows = cJSON_GetObjectItem(out, "rows");
    CHECK(cJSON_IsArray(rows), "rows is an array");
    cJSON *row = find_row(rows, TARGET_PID);
    CHECK(row != NULL, "pid %d present in the served rows", TARGET_PID);
    if (row) {
        cJSON *tw = cJSON_GetObjectItem(row, "top_wait");
        CHECK(tw && cJSON_IsString(tw) &&
              strstr(tw->valuestring, "Lock") != NULL,
              "served top_wait is Lock (raw winner, 100ms total), got %s",
              tw && cJSON_IsString(tw) ? tw->valuestring : "(null)");
        CHECK(tw && cJSON_IsString(tw) &&
              strstr(tw->valuestring, "WalSync") == NULL,
              "served top_wait is NOT IO:WalSync (the summary-only, "
              "single-longest-wait answer this backfill replaces), got %s",
              tw && cJSON_IsString(tw) ? tw->valuestring : "(null)");
    }

    /* THE GAP THIS CLOSES: the summary path's OWN per-pid answer, read
     * directly off the same files with no backfill in the loop, is IO — so
     * removing the backfill in handle_top_sessions would make the served
     * row disagree with raw again. This is what makes section 1 RED against
     * an unpatched handle_top_sessions (see the PR body's revert log). */
    struct pgwt_sessions_result summary_only;
    struct pgwt_filter empty_filter = {0};
    pgwt_compute_top_sessions_from_summaries(dir, srv->earliest_wall_ns,
                                             srv->latest_wall_ns,
                                             &empty_filter, 0.0, &summary_only);
    CHECK(!summary_only.failed, "summary-only compute did not fail");
    int found_summary_only = 0;
    for (int i = 0; i < summary_only.num_rows; i++) {
        if (summary_only.rows[i].pid != TARGET_PID) continue;
        found_summary_only = 1;
        CHECK(strstr(summary_only.rows[i].top_wait, "IO") != NULL,
              "fixture is well-formed: the UNPATCHED summary-only answer "
              "for pid %d is IO (single longest wait), got %s -- otherwise "
              "this test cannot tell a working backfill from a no-op one",
              TARGET_PID, summary_only.rows[i].top_wait);
    }
    CHECK(found_summary_only, "pid %d present in the summary-only compute too",
          TARGET_PID);
    free(summary_only.rows);

    cJSON_Delete(out);
    free(srv);
}

/* ── Section 2: raw backfill overload reports absence, not a wrong name ── */
static int child_overload_absence(const char *dir)
{
    setenv("PGWT_LOAD_MAX_EVENTS", "3", 1);   /* far below this fixture's events */

    struct pgwt_server *srv = calloc(1, sizeof(*srv));
    if (!srv || server_init(srv, dir) != 0) {
        fprintf(stderr, "child: server_init failed\n");
        return 1;
    }
    struct pgwt_request req = {0};
    cJSON *out = call_top_sessions(srv, &req);
    if (!out) { fprintf(stderr, "child: no JSON\n"); return 1; }

    int ok = 1;
    cJSON *err = cJSON_GetObjectItem(out, "error");
    if (err != NULL) {
        fprintf(stderr, "child: got a hard error, not a degraded response: %s\n",
                cJSON_IsString(err) ? err->valuestring : "?");
        ok = 0;
    }
    cJSON *unavail = cJSON_GetObjectItem(out, "top_wait_unavailable");
    if (!(unavail && cJSON_IsTrue(unavail))) {
        fprintf(stderr, "child: top_wait_unavailable not set true\n");
        ok = 0;
    }
    cJSON *reason = cJSON_GetObjectItem(out, "top_wait_unavailable_reason");
    if (!(reason && cJSON_IsString(reason) && reason->valuestring[0] != '\0')) {
        fprintf(stderr, "child: top_wait_unavailable_reason missing/empty\n");
        ok = 0;
    }
    cJSON *rows = cJSON_GetObjectItem(out, "rows");
    if (!cJSON_IsArray(rows) || cJSON_GetArraySize(rows) == 0) {
        fprintf(stderr, "child: rows missing -- a degraded top_wait must not "
                "take db_time_ms etc. down with it\n");
        ok = 0;
    } else {
        cJSON *row = find_row(rows, TARGET_PID);
        if (!row) {
            fprintf(stderr, "child: pid %d missing from degraded rows\n",
                    TARGET_PID);
            ok = 0;
        } else {
            cJSON *tw = cJSON_GetObjectItem(row, "top_wait");
            if (!(tw && cJSON_IsNull(tw))) {
                fprintf(stderr, "child: row top_wait is not null (%s) -- "
                        "the row must not carry the summary-only name\n",
                        tw && cJSON_IsString(tw) ? tw->valuestring
                                                  : "(not a string either)");
                ok = 0;
            }
            cJSON *dbt = cJSON_GetObjectItem(row, "db_time_ms");
            if (!(dbt && cJSON_IsNumber(dbt) && dbt->valuedouble > 0)) {
                fprintf(stderr, "child: db_time_ms missing/zero -- the cheap "
                        "summary fields must survive a top_wait degrade\n");
                ok = 0;
            }
        }
    }
    cJSON_Delete(out);
    free(srv);
    return ok ? 0 : 1;
}

static void test_overload_reports_absence(const char *dir)
{
    printf("--- 2. raw-backfill overload: absence with a reason, never a "
           "wrong name or a silent blank ---\n");
    fflush(stdout);
    pid_t pid = fork();
    if (pid < 0) {
        tests_run++; tests_failed++;
        printf("  FAIL: fork() failed, cannot isolate the env-var override\n");
        return;
    }
    if (pid == 0)
        _exit(child_overload_absence(dir));
    int status = 0;
    waitpid(pid, &status, 0);
    CHECK(WIFEXITED(status) && WEXITSTATUS(status) == 0,
          "child reported absence-with-reason correctly (exit %d, signalled=%d)",
          WIFEXITED(status) ? WEXITSTATUS(status) : -1, !WIFEXITED(status));
}

/* ── Section 3: the Top Queries backfill ────────────────────────────────── */
static void test_queries_backfill_matches_raw(const char *dir)
{
    printf("--- 3. queries backfill matches raw, not the summary-only "
           "longest wait ---\n");

    struct pgwt_server *srv = calloc(1, sizeof(*srv));
    CHECK(srv != NULL && server_init(srv, dir) == 0, "server_init");
    if (!srv) return;

    struct pgwt_request req = {0};
    req.from_ns = 0;
    req.to_ns = 0;

    CHECK(route_is_summaries(srv, &req),
          "the SUMMARY route is taken for this request -- see the comment on "
          "route_is_summaries");

    cJSON *out = call_top_queries(srv, &req);
    CHECK(out != NULL, "handle_top_queries produced parseable JSON");
    if (!out) { free(srv); return; }
    CHECK(cJSON_GetObjectItem(out, "error") == NULL, "no error in response");
    CHECK(cJSON_GetObjectItem(out, "top_wait_unavailable") == NULL,
          "top_wait is available for an ordinary window");

    cJSON *fidelity = cJSON_GetObjectItem(out, "fidelity");
    CHECK(fidelity && cJSON_IsString(fidelity) &&
          strcmp(fidelity->valuestring, "exact") == 0,
          "served exact fidelity (got %s)",
          fidelity && cJSON_IsString(fidelity) ? fidelity->valuestring : "?");

    cJSON *rows = cJSON_GetObjectItem(out, "rows");
    CHECK(cJSON_IsArray(rows), "rows is an array");
    cJSON *row = find_query_row(rows, TARGET_QID);
    CHECK(row != NULL, "query_id %llu present in the served rows",
          (unsigned long long)TARGET_QID);
    if (row) {
        cJSON *tw = cJSON_GetObjectItem(row, "top_wait");
        CHECK(tw && cJSON_IsString(tw) &&
              strstr(tw->valuestring, "Lock") != NULL,
              "served top_wait is Lock (raw winner, 100ms total), got %s",
              tw && cJSON_IsString(tw) ? tw->valuestring : "(null)");
        CHECK(tw && cJSON_IsString(tw) &&
              strstr(tw->valuestring, "WalSync") == NULL,
              "served top_wait is NOT IO:WalSync (the summary-only, "
              "single-longest-wait answer this backfill replaces), got %s",
              tw && cJSON_IsString(tw) ? tw->valuestring : "(null)");
    }

    /* THE GAP THIS CLOSES, same shape as section 1: the summary path's OWN
     * per-query answer, read directly off the same files with no backfill
     * in the loop, is IO -- so removing the backfill in handle_top_queries
     * would make the served row disagree with raw again. */
    struct pgwt_queries_result summary_only;
    struct pgwt_filter empty_filter = {0};
    pgwt_compute_top_queries_from_summaries(dir, srv->earliest_wall_ns,
                                            srv->latest_wall_ns,
                                            &empty_filter, 0.0, &summary_only);
    CHECK(!summary_only.failed, "summary-only compute did not fail");
    int found_summary_only = 0;
    for (int i = 0; i < summary_only.num_rows; i++) {
        if (summary_only.rows[i].query_id != TARGET_QID) continue;
        found_summary_only = 1;
        CHECK(strstr(summary_only.rows[i].top_wait, "IO") != NULL,
              "fixture is well-formed: the UNPATCHED summary-only answer "
              "for query %llu is IO (single longest wait), got %s -- "
              "otherwise this test cannot tell a working backfill from a "
              "no-op one", (unsigned long long)TARGET_QID,
              summary_only.rows[i].top_wait);
    }
    CHECK(found_summary_only, "query_id %llu present in the summary-only "
          "compute too", (unsigned long long)TARGET_QID);
    free(summary_only.rows);

    cJSON_Delete(out);
    free(srv);
}

/* -- Section 4: the route assertion can actually be FALSE ---------------- */
/* Without this, route_is_summaries could be a constant-true predicate (a
 * typo'd field, a short-circuit, a refactor that makes should_use_summaries
 * always take the summary branch) and sections 1 and 3 would still read as
 * if they had proved the route. Same fixture events, same 130 s window,
 * same 0/0 request -- ONLY the summary file is absent. If this comes back
 * true, the predicate is not discriminating and sections 1 and 3 are
 * unproven. */
static void test_route_predicate_discriminates(const char *dir_no_summaries)
{
    printf("--- 4. the route predicate is not vacuously true: no summary "
           "file => not the summary route ---\n");

    struct pgwt_server *srv = calloc(1, sizeof(*srv));
    CHECK(srv != NULL && server_init(srv, dir_no_summaries) == 0,
          "server_init on the summary-less fixture");
    if (!srv) return;

    struct pgwt_request req = {0};
    req.from_ns = 0;
    req.to_ns = 0;
    CHECK(srv->latest_wall_ns > srv->earliest_wall_ns &&
          srv->latest_wall_ns - srv->earliest_wall_ns >=
              120ULL * 1000000000ULL,
          "the summary-less fixture still spans >= 120 s, so a FALSE route "
          "here is the missing summary file and not a too-short window "
          "(span %.1f s)",
          (double)(srv->latest_wall_ns - srv->earliest_wall_ns) / 1e9);
    CHECK(!route_is_summaries(srv, &req),
          "route_is_summaries is FALSE with no summary file -- so the TRUE "
          "it returns in sections 1 and 3 is a real observation, not a "
          "constant");

    /* And the served answer is still the right one, from raw. A route the
     * backfill does not cover must not be a route that returns a wrong
     * name either. */
    cJSON *out = call_top_sessions(srv, &req);
    CHECK(out != NULL, "handle_top_sessions answered the raw route");
    if (out) {
        cJSON *row = find_row(cJSON_GetObjectItem(out, "rows"), TARGET_PID);
        cJSON *tw = row ? cJSON_GetObjectItem(row, "top_wait") : NULL;
        CHECK(tw && cJSON_IsString(tw) && strstr(tw->valuestring, "Lock"),
              "raw route still serves Lock for pid %d, got %s", TARGET_PID,
              tw && cJSON_IsString(tw) ? tw->valuestring : "(null)");
        cJSON_Delete(out);
    }
    free(srv);
}

int main(void)
{
    char dir[] = "/tmp/pgwt_top_wait_backfill_XXXXXX";
    char dir_nosum[] = "/tmp/pgwt_top_wait_backfill_nosum_XXXXXX";
    if (!mkdtemp(dir)) { printf("FAIL: mkdtemp\n"); return 1; }
    if (!mkdtemp(dir_nosum)) { printf("FAIL: mkdtemp\n"); return 1; }
    uint64_t base_mono = mono_origin();
    build_fixture(dir, base_mono, 1);
    build_fixture(dir_nosum, base_mono, 0);

    /* Section 2 forks BEFORE this process ever calls load_max_events()
     * (section 1 does, via server_load_events_fi inside the backfill), so
     * the child's PGWT_LOAD_MAX_EVENTS=3 override lands in a process whose
     * cache is still unset, and never leaks into section 1's budget. */
    test_overload_reports_absence(dir);
    test_backfill_matches_raw(dir, base_mono);
    test_queries_backfill_matches_raw(dir);
    test_route_predicate_discriminates(dir_nosum);

    /* A run where nothing ran at all is a PASS under "0 failed". Refuse. */
    if (tests_run < 20) {
        printf("FAIL: only %d assertions executed -- a truncated run must "
               "not read as a pass\n", tests_run);
        tests_failed++;
    }

    printf("\n%d tests, %d failed\n", tests_run, tests_failed);
    return tests_failed ? 1 : 0;
}
