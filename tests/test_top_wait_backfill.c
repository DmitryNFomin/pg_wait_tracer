/* test_top_wait_backfill.c — Sessions retain #319's raw top_wait backfill;
 * Queries now compute their entire answer directly from raw events.
 * Summary-only top_wait (longest single wait per second) disagrees with
 * raw's largest per-event TOTAL over the window in the shared fixture:
 * IO:WalSync has one 40 ms wait; Lock:relation has ten 10 ms waits.
 *
 * Sections 1 and 2 pin the Sessions backfill and overload degradation.
 * Section 2b pins the existing Events/Queries overload hints.
 * Section 3 pins Queries' raw top_wait against the summary-only IO answer.
 * Section 3b requires an unfiltered >=120 s request that would qualify for
 * summaries to match direct raw computation, including clipped row totals
 * and percentages. Its straddling interval forces a numeric disagreement
 * with summaries: a fixture that cannot distinguish the routes must fail.
 * Sections 4 and 5 retain summary-predicate and query-filtered Events gates.
 * Sections 6 and 7 pin raw unattributed/backfilled measurements, empty
 * controls with and without summaries, and terminal raw compute failure.
 */
#define main pgwt_server_main
#include "../src/server.c"
#undef main
#include "idle_rule.h"

#include <sys/wait.h>
#include <unistd.h>
#include <math.h>

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
#define UNATTRIB_PID 901
#define STRADDLE_PID 903
#define STRADDLE_MS  2500
#define STRADDLE_IN_WINDOW_MS 1500
#define BACKFILL_PID 902
#define BACKFILL_QID 0x5252525252525252ULL

enum fixture_kind {
    FIXTURE_MIXED,
    FIXTURE_NO_UNATTRIBUTED,
    FIXTURE_NO_QUERIES,
    FIXTURE_STRADDLE
};

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
 * 100 ms under the raw definition.
 *
 * FIXTURE_STRADDLE adds one Lock interval on a third pid, with TARGET_QID,
 * from s=125.5 to s=128.0. Section 5 ends its window at s=127.0. Open notes
 * make events[] carry 500/1000/1000 ms in seconds 125/126/127, while
 * queries[] carries the full 2500 ms only at close in second 128. Keep this
 * variant separate: even a different pid with TARGET_QID would change the
 * per-query summary-only winner in section 3 from IO to Lock. */
static void build_fixture(const char *dir, uint64_t base_mono, int with_summaries,
                          enum fixture_kind kind)
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

        if (sw && kind == FIXTURE_STRADDLE && s >= 126 && s <= 128)
            pgwt_summary_note_open(sw, STRADDLE_PID, EV_LOCK,
                                   base_mono + 125 * ONE_SEC + 500 * MS,
                                   0, sec);

        struct pgwt_trace_event filler;
        memset(&filler, 0, sizeof(filler));
        filler.timestamp_ns = sec + 10 * MS;
        filler.pid = FILLER_PID;
        filler.old_event = 0;              /* CPU */
        filler.new_event = EV_LOCK;
        filler.duration_ns = 1 * MS;
        /* The zero-unattributed control must also avoid the loader tagging
         * these query-less CPU ticks as unattributed foreground time. */
        if (kind == FIXTURE_NO_UNATTRIBUTED)
            filler.query_id = BACKFILL_QID;
        filler.cpu_ns = PGWT_CPU_NS_UNKNOWN;
        pgwt_writer_push_event(&writer, &filler);
        if (sw) {
            /* Use the trace's clock conversion so window edges agree to
             * the nanosecond, independent of the two writers' init timing. */
            if (s == 0)
                pgwt_summary_pin_clock(sw, writer.file_start_mono_ns,
                                       writer.file_start_wall_ns);
            pgwt_summary_push_event(sw, &filler, 0, filler.old_event);
            if (kind == FIXTURE_STRADDLE && s == 125)
                pgwt_summary_note_open(sw, STRADDLE_PID, EV_LOCK,
                                       sec + 500 * MS, 0, sec + 500 * MS);
        }
        if (kind == FIXTURE_STRADDLE && s == 128) {
            struct pgwt_trace_event e = {0};
            e.timestamp_ns = sec;
            e.pid = STRADDLE_PID;
            e.old_event = EV_LOCK;
            e.new_event = EV_IO;
            e.duration_ns = STRADDLE_MS * MS;
            e.query_id = TARGET_QID;
            e.cpu_ns = PGWT_CPU_NS_UNKNOWN;
            pgwt_writer_push_event(&writer, &e);
            if (sw) pgwt_summary_push_event(sw, &e, 0, e.old_event);
        }

        if (s < 10 && kind != FIXTURE_NO_QUERIES) {
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
        if (s == 100 && kind != FIXTURE_NO_QUERIES) {
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
        /* #598: separate pids keep the target's 100 ms Lock / 40 ms IO
         * profile intact, and keep the loader from backfilling the orphan
         * from the other record's query id. Both reach the summary writer. */
        if ((kind == FIXTURE_MIXED || kind == FIXTURE_STRADDLE) &&
            (s == 30 || s == 40)) {
            struct pgwt_trace_event e = {0};
            e.timestamp_ns = sec + 50 * MS;
            e.pid = s == 30 ? UNATTRIB_PID : BACKFILL_PID;
            e.old_event = EV_IO;
            e.new_event = EV_LOCK;
            e.duration_ns = 7 * MS;
            e.cpu_ns = PGWT_CPU_NS_UNKNOWN;
            e.query_id = 0;
            e.flags = s == 30 ? PGWT_EVENT_FLAG_QUERY_UNATTRIB
                              : PGWT_EVENT_FLAG_QUERY_BACKFILL;
            pgwt_writer_push_event(&writer, &e);
            if (sw) pgwt_summary_push_event(sw, &e, 0, e.old_event);
            if (s == 40) {
                /* Reader annotations are not persisted. Report an id later
                 * on this same pid so the real loader reconstructs BACKFILL
                 * on the preceding interval; a pre-set flag alone vanishes
                 * in the trace file and would leave backfilled_ms at zero. */
                e.timestamp_ns += 10 * MS;
                e.duration_ns = 1 * MS;
                e.query_id = BACKFILL_QID;
                e.flags = 0;
                pgwt_writer_push_event(&writer, &e);
                if (sw) pgwt_summary_push_event(sw, &e, 0, e.old_event);
            }
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

/* Sessions still route on this predicate. Queries deliberately override a
 * true result, so their gates must pair it with direct raw expectations and
 * a summary-only answer that differs. Events also has a query-id guard.
 * Summary-less controls prove the predicate is not vacuously true. */
static int summaries_would_be_used(struct pgwt_server *srv, struct pgwt_request *req)
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

static cJSON *call_top_events(struct pgwt_server *srv, struct pgwt_request *req)
{
    return call_handler(handle_top_events, srv, req);
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

    CHECK(summaries_would_be_used(srv, &req),
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

/* ── Section 2b: query-filter overload hints describe the actual route ── */
/* Like section 2, fork BEFORE the parent's first load_max_events() call so
 * each child's tiny budget reaches an unset cache. Positively require a
 * window_too_large refusal BEFORE inspecting the hint: an absent refusal
 * must fail, never make this gate pass vacuously. Events forces raw for a
 * query filter; Queries loads raw regardless, so keeps the original hint. */
static int child_query_filter_overload_hint(const char *dir, int queries)
{
    setenv("PGWT_LOAD_MAX_EVENTS", "3", 1);
    struct pgwt_server *srv = calloc(1, sizeof(*srv));
    if (!srv || server_init(srv, dir) != 0) return 1;
    struct pgwt_request req = {0};
    snprintf(req.cmd, sizeof(req.cmd), "%s",
             queries ? "top_queries" : "top_events");
    req.filter.query_id = TARGET_QID;
    cJSON *out = queries ? call_top_queries(srv, &req)
                         : call_top_events(srv, &req);
    cJSON *code = cJSON_GetObjectItem(out, "code");
    if (!cJSON_IsString(code) ||
        strcmp(code->valuestring, "window_too_large") != 0) {
        fprintf(stderr, "child: %s did not reach window_too_large\n", req.cmd);
        cJSON_Delete(out);
        free(srv);
        return 2;
    }
    cJSON *hint = cJSON_GetObjectItem(out, "hint");
    int ok = cJSON_IsString(hint) && (queries
        ? strcmp(hint->valuestring,
                 "narrow the time range or add a pid/query filter") == 0 &&
          strstr(hint->valuestring, "forces the exact path") == NULL
        : strstr(hint->valuestring, "forces the exact path") != NULL);
    if (!ok)
        fprintf(stderr, "child: %s has the wrong overload hint: %s\n",
                req.cmd, cJSON_IsString(hint) ? hint->valuestring : "(missing)");
    cJSON_Delete(out);
    free(srv);
    return ok ? 0 : 3;
}

static void test_query_filter_overload_hints(const char *dir)
{
    printf("--- 2b. query-filter overload hints match Events and Queries "
           "routing ---\n");
    for (int queries = 0; queries <= 1; queries++) {
        fflush(stdout);
        pid_t pid = fork();
        if (pid == 0) _exit(child_query_filter_overload_hint(dir, queries));
        int status = 0;
        int waited = pid > 0 ? waitpid(pid, &status, 0) : -1;
        CHECK(waited == pid && pid > 0 && WIFEXITED(status) &&
              WEXITSTATUS(status) == 0,
              "%s child must reach window_too_large and report the %s "
              "hint (exit %d)", queries ? "top_queries" : "top_events",
              queries ? "original" : "query-forces-raw",
              WIFEXITED(status) ? WEXITSTATUS(status) : -1);
    }
}

static void compute_raw_queries(struct pgwt_server *srv,
                                 struct pgwt_request *req,
                                 struct pgwt_queries_result *raw)
{
    int count = 0;
    struct pgwt_load_info linfo = {0};
    struct pgwt_trace_event *events =
        server_load_events_fi(srv, req->from_ns, req->to_ns, 0, &count, &linfo);
    CHECK(events != NULL && count > 0, "raw query expectation has events");
    uint64_t from = req->from_ns ? req->from_ns : srv->earliest_wall_ns;
    uint64_t to = req->to_ns ? req->to_ns : srv->latest_wall_ns;
    pgwt_compute_top_queries(events, count, &req->filter,
                             req->from_ns, req->to_ns,
                             (double)(to - from) / 1e6, raw);
    CHECK(!raw->failed, "direct raw query computation succeeds");
    free(events);
}

static int json_number_equals(cJSON *value, double expected)
{
    return cJSON_IsNumber(value) && fabs(value->valuedouble - expected) < 1e-6;
}

/* ── Section 3: Top Queries answers top_wait directly from raw ────────── */
static void test_queries_top_wait_matches_raw(const char *dir)
{
    printf("--- 3. Queries serves raw top_wait instead of summary-only "
           "longest wait ---\n");

    struct pgwt_server *srv = calloc(1, sizeof(*srv));
    CHECK(srv != NULL && server_init(srv, dir) == 0, "server_init");
    if (!srv) return;

    struct pgwt_request req = {0};
    req.from_ns = 0;
    req.to_ns = 0;

    CHECK(summaries_would_be_used(srv, &req),
          "should_use_summaries WOULD accept this window; Queries forces raw");

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
              "single-longest-wait answer raw replaces), got %s",
              tw && cJSON_IsString(tw) ? tw->valuestring : "(null)");
    }

    /* The summary-only answer remains IO. Queries now serves the raw Lock
     * winner directly, rather than overwriting summary rows in a backfill. */
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
              "otherwise this fixture cannot distinguish raw from summaries", (unsigned long long)TARGET_QID,
              summary_only.rows[i].top_wait);
    }
    CHECK(found_summary_only, "query_id %llu present in the summary-only "
          "compute too", (unsigned long long)TARGET_QID);
    free(summary_only.rows);

    cJSON_Delete(out);
    free(srv);
}

/* 3b: summary eligibility must never select the Queries compute anymore.
 * The window cuts a 2500 ms wait at 1500 ms, before its summary close.
 * Require a real numeric gap, then compare every compute-derived field of
 * every served row with the direct raw answer over these same events. */
static void test_queries_force_raw(const char *dir)
{
    printf("--- 3b. unfiltered >=120 s Queries forces raw despite summary "
           "eligibility ---\n");
    struct pgwt_server *srv = calloc(1, sizeof(*srv));
    CHECK(srv != NULL && server_init(srv, dir) == 0, "server_init");
    if (!srv) return;
    struct pgwt_request req = {0};
    req.from_ns = srv->earliest_wall_ns;
    req.to_ns = srv->earliest_wall_ns - 10 * MS + 127 * ONE_SEC;
    CHECK(req.to_ns - req.from_ns >= 120 * ONE_SEC,
          "unfiltered Queries window spans >=120 s");
    CHECK(summaries_would_be_used(srv, &req),
          "should_use_summaries is TRUE; handler must override its answer");

    struct pgwt_queries_result raw, summary_only;
    compute_raw_queries(srv, &req, &raw);
    pgwt_compute_top_queries_from_summaries(dir, req.from_ns, req.to_ns,
                                           &req.filter,
                                           (double)(req.to_ns - req.from_ns) / 1e6,
                                           &summary_only);
    CHECK(!summary_only.failed, "direct summary query computation succeeds");
    CHECK(fabs(raw.db_time_ms - summary_only.db_time_ms) > 1e-6,
          "fixture MUST distinguish raw DB Time from summaries "
          "(raw %.9f, summary %.9f)", raw.db_time_ms, summary_only.db_time_ms);
    const struct pgwt_query_row *raw_target = NULL, *summary_target = NULL;
    for (int i = 0; i < raw.num_rows; i++)
        if (raw.rows[i].query_id == TARGET_QID) raw_target = &raw.rows[i];
    for (int i = 0; i < summary_only.num_rows; i++)
        if (summary_only.rows[i].query_id == TARGET_QID)
            summary_target = &summary_only.rows[i];
    CHECK(raw_target && fabs(raw_target->total_ms -
                            (140 + STRADDLE_IN_WINDOW_MS)) < 1e-6,
          "raw query total includes exactly 1500 ms of the straddling wait");
    CHECK(raw_target && summary_target &&
          fabs(raw_target->total_ms - summary_target->total_ms) > 1e-6,
          "fixture MUST distinguish raw query-row time from summaries");

    cJSON *out = call_top_queries(srv, &req);
    CHECK(out && cJSON_GetObjectItem(out, "error") == NULL,
          "unfiltered long-window Queries request succeeds");
    cJSON *db_time = cJSON_GetObjectItem(out, "db_time_ms");
    CHECK(json_number_equals(db_time, raw.db_time_ms),
          "served DB Time equals direct raw computation");
    CHECK(cJSON_IsNumber(db_time) &&
          fabs(db_time->valuedouble - summary_only.db_time_ms) > 1e-6,
          "served DB Time MUST differ from summary-only computation");
    CHECK(cJSON_IsTrue(cJSON_GetObjectItem(out, "unattributed_available")),
          "raw measurements are available");
    CHECK(json_number_equals(cJSON_GetObjectItem(out, "unattributed_ms"),
                             raw.unattributed_ms) &&
          json_number_equals(cJSON_GetObjectItem(out, "unattributed_count"),
                             (double)raw.unattributed_count) &&
          json_number_equals(cJSON_GetObjectItem(out, "backfilled_ms"),
                             raw.backfilled_ms),
          "all three measurement scalars equal direct raw computation");
    CHECK(cJSON_GetObjectItem(out, "top_wait_unavailable") == NULL &&
          cJSON_GetObjectItem(out, "top_wait_unavailable_reason") == NULL,
          "raw response has no summary-backfill unavailability fields");
    cJSON *rows = cJSON_GetObjectItem(out, "rows");
    CHECK(cJSON_IsArray(rows) && cJSON_GetArraySize(rows) == raw.num_rows,
          "served row count equals direct raw computation");
    for (int i = 0; i < raw.num_rows; i++) {
        const struct pgwt_query_row *expected = &raw.rows[i];
        cJSON *row = find_query_row(rows, expected->query_id);
        CHECK(row != NULL, "raw query %llu is served",
              (unsigned long long)expected->query_id);
        CHECK(json_number_equals(cJSON_GetObjectItem(row, "count"),
                                 (double)expected->count),
              "served observation count equals raw");
        CHECK(json_number_equals(cJSON_GetObjectItem(row, "total_ms"),
                                 expected->total_ms),
              "served query time equals clipped raw time");
        CHECK(json_number_equals(cJSON_GetObjectItem(row, "pct"),
                                 expected->pct_db),
              "served percentage uses the same raw numerator and denominator");
        cJSON *avg = cJSON_GetObjectItem(row, "avg_us");
        CHECK(expected->exact_count ? json_number_equals(avg, expected->avg_us)
                                    : cJSON_IsNull(avg),
              "served latency equals raw full-duration latency or null");
        cJSON *top_wait = cJSON_GetObjectItem(row, "top_wait");
        CHECK(cJSON_IsString(top_wait) &&
              strcmp(top_wait->valuestring, expected->top_wait) == 0 &&
              json_number_equals(cJSON_GetObjectItem(row, "top_wait_id"),
                                 expected->top_wait_id),
              "served top_wait name and id equal raw");
        cJSON *classes = cJSON_GetObjectItem(row, "classes");
        int classes_match = cJSON_IsArray(classes) &&
                            cJSON_GetArraySize(classes) == PGWT_NUM_CLASSES;
        for (int c = 0; c < PGWT_NUM_CLASSES; c++)
            classes_match &= json_number_equals(cJSON_GetArrayItem(classes, c),
                                                 expected->class_ms[c]);
        CHECK(classes_match, "every served class total equals raw");
        cJSON *events = cJSON_GetObjectItem(row, "events");
        int events_match = expected->num_events > 0
            ? cJSON_IsArray(events) &&
              cJSON_GetArraySize(events) == expected->num_events
            : events == NULL;
        for (int e = 0; e < expected->num_events; e++) {
            cJSON *event = cJSON_GetArrayItem(events, e);
            events_match &= json_number_equals(cJSON_GetObjectItem(event, "id"),
                                                expected->event_ids[e]) &&
                            json_number_equals(cJSON_GetObjectItem(event, "ms"),
                                                expected->event_ms[e]);
        }
        CHECK(events_match, "every served per-event total and id equals raw");
    }
    cJSON *served_target = find_query_row(rows, TARGET_QID);
    cJSON *served_total = cJSON_GetObjectItem(served_target, "total_ms");
    CHECK(summary_target && cJSON_IsNumber(served_total) &&
          fabs(served_total->valuedouble - summary_target->total_ms) > 1e-6,
          "served query row MUST differ from the summary-only total");
    printf("  Queries ms: summary %.9f, raw %.9f, served %.9f\n",
           summary_target ? summary_target->total_ms : -1.0,
           raw_target ? raw_target->total_ms : -1.0,
           cJSON_IsNumber(served_total) ? served_total->valuedouble : -1.0);
    cJSON_Delete(out);
    free(raw.rows);
    free(summary_only.rows);
    free(srv);
}

/* -- Section 4: the route assertion can actually be FALSE ---------------- */
/* Without this, summaries_would_be_used could be a constant-true predicate (a
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
    CHECK(!summaries_would_be_used(srv, &req),
          "summaries_would_be_used is FALSE with no summary file -- so the TRUE "
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

/* -- Section 5: #526 query-filtered Events must bypass summaries --------- */
/* Merely seeing a Lock row would miss the regression: both routes have one.
 * Prove should_use_summaries WOULD accept this request, then compare the
 * served number with a direct raw computation AND pin a different summary-
 * only number. If that gap disappears, this fixture must FAIL, not skip.
 * handle_top_events exposes fidelity but no route/source field: exact
 * fidelity describes both routes, so only the numeric gap proves the guard.
 *
 * Measured by printing ALL persisted query/Lock records and raw intervals:
 * all ten 10 ms waits are on disk, but the s=0 summary record starts 10 ms
 * BEFORE earliest_wall_ns (the first CPU tick at s=0.010). The summary
 * visitor excludes that second; raw's 0/0 bounds include its Lock interval
 * [0.040, 0.050]. Thus summary=90.000000000 ms, raw/served=100.000000000 ms.
 * Raw receives from/to=0/0 (unbounded); summaries receive the discovered
 * earliest/latest bounds. Both receive wall_ms=129000.000000000. The 10 ms
 * gap is this lower-window-edge exclusion, not a lost final flush or a
 * dur/tns asymmetry in these ten single-second waits.
 *
 * The straddle variant ALSO exercises #526's actual mechanism: the window
 * ends at s=127, before the 2500 ms wait closes at s=128. queries[] files
 * all 2500 ms at close, OUTSIDE the window; events[] files 500 ms at s=125,
 * 1000 ms at s=126, and 1000 ms at s=127. Raw clips to 1500 ms in-window.
 * Measured filtered summary=90, unfiltered summary=1590, raw/served=1600 ms:
 * the 1510 ms gap is 1500 ms from the straddle plus the same 10 ms s=0
 * exclusion. In particular the gate cannot pass on that 10 ms alone. */
static void test_events_query_filter_uses_raw(const char *dir, int straddle)
{
    printf("--- 5. query-filtered Events bypass summaries (#526), %s ---\n",
           straddle ? "straddling wait" : "original boundary control");
    struct pgwt_server *srv = calloc(1, sizeof(*srv));
    CHECK(srv != NULL && server_init(srv, dir) == 0, "server_init");
    if (!srv) return;
    struct pgwt_request req = {0};
    req.filter.query_id = TARGET_QID;
    if (straddle)
        req.to_ns = srv->earliest_wall_ns - 10 * MS + 127 * ONE_SEC;
    CHECK(summaries_would_be_used(srv, &req),
          "should_use_summaries WOULD accept the query-filtered window; "
          "the handler's query_id conjunct is the guard under test");

    int count = 0;
    struct pgwt_load_info linfo = {0};
    struct pgwt_trace_event *events =
        server_load_events_fi(srv, req.from_ns, req.to_ns, 0, &count, &linfo);
    CHECK(events != NULL && count > 0, "loaded all raw events for expectation");
    uint64_t from = req.from_ns ? req.from_ns : srv->earliest_wall_ns;
    uint64_t to = req.to_ns ? req.to_ns : srv->latest_wall_ns;
    double wall_ms = (double)(to - from) / 1e6;
    struct pgwt_events_result raw, summary_only;
    pgwt_compute_top_events(events, count, &req.filter, req.from_ns, req.to_ns,
                            wall_ms, &raw);
    pgwt_compute_top_events_from_summaries(dir, from, to, &req.filter,
                                           wall_ms, &summary_only);
    const struct pgwt_event_row *raw_lock = NULL, *summary_lock = NULL;
    for (int i = 0; i < raw.num_rows; i++)
        if (raw.rows[i].event_id == EV_LOCK) raw_lock = &raw.rows[i];
    for (int i = 0; i < summary_only.num_rows; i++)
        if (summary_only.rows[i].event_id == EV_LOCK)
            summary_lock = &summary_only.rows[i];
    CHECK(raw_lock != NULL && raw_lock->total_ms > 0,
          "raw expectation contains real Lock:relation time");
    CHECK(summary_lock != NULL, "summary-only filtered answer has Lock:relation");

    cJSON *out = call_top_events(srv, &req);
    CHECK(out != NULL && cJSON_GetObjectItem(out, "error") == NULL,
          "query-filtered Events request succeeds");
    cJSON *served_ms = NULL, *row;
    cJSON_ArrayForEach(row, cJSON_GetObjectItem(out, "rows")) {
        cJSON *id = cJSON_GetObjectItem(row, "event_id");
        if (cJSON_IsNumber(id) && (uint32_t)id->valuedouble == EV_LOCK)
            served_ms = cJSON_GetObjectItem(row, "total_ms");
    }
    CHECK(cJSON_IsNumber(served_ms), "served Lock:relation total_ms is numeric");
    CHECK(raw_lock && cJSON_IsNumber(served_ms) &&
          fabs(served_ms->valuedouble - raw_lock->total_ms) < 1e-6,
          "served filtered Lock time equals the direct RAW answer");
    CHECK(summary_lock && cJSON_IsNumber(served_ms) &&
          fabs(summary_lock->total_ms - served_ms->valuedouble) > 1e-6,
          "THE GAP: summary-only filtered Lock time must differ from served "
          "raw time, or this fixture cannot detect a missing #526 guard "
          "(summary %.9f, served %.9f)",
          summary_lock ? summary_lock->total_ms : -1.0,
          cJSON_IsNumber(served_ms) ? served_ms->valuedouble : -1.0);
    double straddle_ms = straddle ? STRADDLE_IN_WINDOW_MS : 0;
    CHECK(raw_lock && fabs(raw_lock->total_ms - (100 + straddle_ms)) < 1e-6,
          "raw Lock time is 100 ms plus the straddle's in-window contribution");
    CHECK(summary_lock && fabs(summary_lock->total_ms - 90) < 1e-6,
          "summary-only Lock time excludes s=0 and the straddle's close");
    CHECK(raw_lock && summary_lock &&
          raw_lock->total_ms - summary_lock->total_ms >= straddle_ms &&
          fabs(raw_lock->total_ms - summary_lock->total_ms -
               (10 + straddle_ms)) < 1e-6,
          "gap is the straddle's %.9f ms contribution plus the 10 ms s=0 exclusion",
          straddle_ms);
    printf("  Lock ms: summary %.9f, raw %.9f, served %.9f; "
           "straddle contribution %.9f\n",
           summary_lock ? summary_lock->total_ms : -1.0,
           raw_lock ? raw_lock->total_ms : -1.0,
           cJSON_IsNumber(served_ms) ? served_ms->valuedouble : -1.0,
           raw_lock ? raw_lock->total_ms - 100 : -1.0);
    if (straddle) {
        struct pgwt_filter unfiltered = {0};
        struct pgwt_events_result all;
        pgwt_compute_top_events_from_summaries(dir, from, to, &unfiltered,
                                               wall_ms, &all);
        const struct pgwt_event_row *all_lock = NULL;
        for (int i = 0; i < all.num_rows; i++)
            if (all.rows[i].event_id == EV_LOCK) all_lock = &all.rows[i];
        CHECK(all_lock && summary_lock &&
              fabs(all_lock->total_ms - summary_lock->total_ms -
                   STRADDLE_IN_WINDOW_MS) < 1e-6,
              "events[] includes 1500 ms of open slices absent from queries[] "
              "(unfiltered %.9f, filtered %.9f)",
              all_lock ? all_lock->total_ms : -1.0,
              summary_lock ? summary_lock->total_ms : -1.0);
        printf("  Unfiltered summary Lock ms: %.9f; "
               "events[] minus queries[]: %.9f\n",
               all_lock ? all_lock->total_ms : -1.0,
               all_lock && summary_lock
                   ? all_lock->total_ms - summary_lock->total_ms : -1.0);
        free(all.rows);
    }
    free(events);
    free(raw.rows);
    free(summary_only.rows);
    cJSON_Delete(out);
    free(srv);
}

/* -- Section 6: #598 preserves all three measured raw scalars ------------ */
/* PRESENCE alone passes against the old hardwired zeros. Require real
 * unattributed AND backfilled time, and compare every scalar to a direct
 * raw computation over the handler's exact events/window/filter. Prove the
 * predicate would have accepted summaries, while Queries still uses raw. */
static void test_queries_unattributed_survives(const char *dir)
{
    printf("--- 6. Queries retain measured raw unattributed/backfilled "
           "time (#598) ---\n");
    struct pgwt_server *srv = calloc(1, sizeof(*srv));
    CHECK(srv != NULL && server_init(srv, dir) == 0, "server_init");
    if (!srv) return;
    struct pgwt_request req = {0};
    CHECK(summaries_would_be_used(srv, &req),
          "should_use_summaries WOULD accept this unfiltered Queries window");
    struct pgwt_queries_result raw;
    compute_raw_queries(srv, &req, &raw);
    CHECK(raw.unattributed_ms > 0 && raw.unattributed_count > 0,
          "fixture really contains unattributed time and observations");
    CHECK(raw.backfilled_ms > 0, "fixture really contains backfilled time");
    cJSON *out = call_top_queries(srv, &req);
    CHECK(out != NULL && cJSON_GetObjectItem(out, "error") == NULL,
          "raw Queries request succeeds");
    CHECK(cJSON_IsTrue(cJSON_GetObjectItem(out, "unattributed_available")),
          "Queries reports that it measured unattributed time");
    cJSON *ms = cJSON_GetObjectItem(out, "unattributed_ms");
    cJSON *n = cJSON_GetObjectItem(out, "unattributed_count");
    cJSON *backfill = cJSON_GetObjectItem(out, "backfilled_ms");
    CHECK(cJSON_IsNumber(ms) && ms->valuedouble > 0 &&
          fabs(ms->valuedouble - raw.unattributed_ms) < 1e-6,
          "nonzero unattributed_ms equals the direct raw answer");
    CHECK(cJSON_IsNumber(n) && n->valuedouble > 0 &&
          n->valuedouble == (double)raw.unattributed_count,
          "nonzero unattributed_count equals the direct raw answer");
    CHECK(cJSON_IsNumber(backfill) && backfill->valuedouble > 0 &&
          fabs(backfill->valuedouble - raw.backfilled_ms) < 1e-6,
          "nonzero backfilled_ms equals the direct raw answer");
    free(raw.rows);
    cJSON_Delete(out);
    free(srv);
}

/* -- Section 7: bypass suite for the new gates --------------------------- */
/* 7a: a zero measurement is available, not unmeasured. A second control has
 * NO query rows but DOES have unattributed time: testing only nonempty
 * results would miss the old res.num_rows > 0 gate around the raw pass. */
static void test_queries_empty_controls(const char *dir, int no_query_rows,
                                         int with_summaries)
{
    printf("--- 7a. %s, %s summaries ---\n", no_query_rows
           ? "zero query rows still measure real unattributed time"
           : "no unattributed records is a measured zero",
           with_summaries ? "with" : "without");
    struct pgwt_server *srv = calloc(1, sizeof(*srv));
    CHECK(srv != NULL && server_init(srv, dir) == 0, "server_init");
    if (!srv) return;
    struct pgwt_request req = {0};
    if (with_summaries)
        CHECK(summaries_would_be_used(srv, &req),
              "should_use_summaries WOULD accept the empty-input control");
    struct pgwt_queries_result raw;
    compute_raw_queries(srv, &req, &raw);
    CHECK(no_query_rows ? (raw.num_rows == 0 && raw.unattributed_ms > 0)
                        : (raw.unattributed_ms == 0 && raw.unattributed_count == 0),
          "raw control has the intended empty-input shape");
    cJSON *out = call_top_queries(srv, &req);
    CHECK(out != NULL && cJSON_GetObjectItem(out, "error") == NULL,
          "empty-input raw Queries request succeeds");
    CHECK(cJSON_IsTrue(cJSON_GetObjectItem(out, "unattributed_available")),
          "successful raw pass is available even with an empty result");
    cJSON *ms = cJSON_GetObjectItem(out, "unattributed_ms");
    cJSON *n = cJSON_GetObjectItem(out, "unattributed_count");
    CHECK(cJSON_IsNumber(ms) &&
          fabs(ms->valuedouble - raw.unattributed_ms) < 1e-6,
          "empty-input unattributed_ms matches measured raw (%.9f ms)",
          raw.unattributed_ms);
    CHECK(cJSON_IsNumber(n) && n->valuedouble == (double)raw.unattributed_count,
          "empty-input unattributed_count matches raw");
    cJSON *backfill = cJSON_GetObjectItem(out, "backfilled_ms");
    CHECK(cJSON_IsNumber(backfill) && backfill->valuedouble == 0,
          "empty-input control has measured zero backfilled time");
    cJSON *rows = cJSON_GetObjectItem(out, "rows");
    CHECK(cJSON_IsArray(rows) && (no_query_rows
              ? cJSON_GetArraySize(rows) == 0 : cJSON_GetArraySize(rows) > 0),
          "served control has the intended query row count");
    free(raw.rows);
    cJSON_Delete(out);
    free(srv);
}

/* 7b: queries_entries fails the sole raw computation, which is TERMINAL.
 * Fork to isolate the allocation hook and positively require compute_failed
 * plus absence of all measurement fields; no degraded response exists. */
static int child_queries_failure(const char *dir)
{
    setenv("PGWT_TEST_ALLOC_FAIL", "queries_entries", 1);
    struct pgwt_server *srv = calloc(1, sizeof(*srv));
    if (!srv || server_init(srv, dir) != 0) return 1;
    struct pgwt_request req = {0};
    if (!summaries_would_be_used(srv, &req)) return 2;
    cJSON *out = call_top_queries(srv, &req);
    if (!out) return 3;
    cJSON *code = cJSON_GetObjectItem(out, "code");
    cJSON *fidelity = cJSON_GetObjectItem(out, "fidelity");
    int ok = cJSON_IsString(code) &&
             strcmp(code->valuestring, "compute_failed") == 0 &&
             cJSON_IsString(cJSON_GetObjectItem(out, "error")) &&
             cJSON_IsString(fidelity) &&
             strcmp(fidelity->valuestring, "exact") == 0 &&
             cJSON_GetObjectItem(out, "unattributed_available") == NULL &&
             cJSON_GetObjectItem(out, "unattributed_ms") == NULL &&
             cJSON_GetObjectItem(out, "unattributed_count") == NULL &&
             cJSON_GetObjectItem(out, "backfilled_ms") == NULL;
    cJSON_Delete(out);
    free(srv);
    return ok ? 0 : 4;
}

static void test_queries_failures(const char *dir)
{
    printf("--- 7b. raw Queries compute failure is terminal ---\n");
    fflush(stdout);
    pid_t pid = fork();
    if (pid == 0) _exit(child_queries_failure(dir));
    int status = 0;
    int waited = pid > 0 ? waitpid(pid, &status, 0) : -1;
    CHECK(waited == pid && pid > 0 && WIFEXITED(status) &&
          WEXITSTATUS(status) == 0,
          "raw failure child must report compute_failed with fidelity and "
          "no measurement fields (exit %d)",
          WIFEXITED(status) ? WEXITSTATUS(status) : -1);
}

/* 7c: retain the Events route discriminator. Queries always uses raw;
 * its summary-less empty-input twins are exercised directly in 7a. */
static void test_events_routes_discriminate(const char *dir_nosum)
{
    printf("--- 7c. query-filtered Events can route RAW without summaries ---\n");
    struct pgwt_server *srv = calloc(1, sizeof(*srv));
    CHECK(srv != NULL && server_init(srv, dir_nosum) == 0,
          "server_init on summary-less mixed fixture");
    if (!srv) return;
    CHECK(srv->latest_wall_ns > srv->earliest_wall_ns &&
          srv->latest_wall_ns - srv->earliest_wall_ns >= 120 * ONE_SEC,
          "summary-less mixed fixture still spans >= 120 s (span %.1f s)",
          (double)(srv->latest_wall_ns - srv->earliest_wall_ns) / 1e9);
    struct pgwt_request req = {0};
    req.filter.query_id = TARGET_QID;
    CHECK(!summaries_would_be_used(srv, &req),
          "section 5 query-filtered Events request routes RAW without summaries");
    req.to_ns = srv->earliest_wall_ns - 10 * MS + 127 * ONE_SEC;
    CHECK(req.to_ns - srv->earliest_wall_ns >= 120 * ONE_SEC,
          "section 5 straddle window also spans >= 120 s");
    CHECK(!summaries_would_be_used(srv, &req),
          "section 5 bounded query-filtered Events request routes RAW "
          "without summaries");
    free(srv);
}

int main(void)
{
    char dir[] = "/tmp/pgwt_top_wait_backfill_XXXXXX";
    char dir_nosum[] = "/tmp/pgwt_top_wait_backfill_nosum_XXXXXX";
    char dir_noattrib[] = "/tmp/pgwt_top_wait_backfill_noattrib_XXXXXX";
    char dir_noqueries[] = "/tmp/pgwt_top_wait_backfill_noqueries_XXXXXX";
    char dir_noattrib_nosum[] = "/tmp/pgwt_top_wait_backfill_noattrib_nosum_XXXXXX";
    char dir_noqueries_nosum[] = "/tmp/pgwt_top_wait_backfill_noqueries_nosum_XXXXXX";
    char dir_straddle[] = "/tmp/pgwt_top_wait_backfill_straddle_XXXXXX";
    if (!mkdtemp(dir)) { printf("FAIL: mkdtemp\n"); return 1; }
    if (!mkdtemp(dir_nosum)) { printf("FAIL: mkdtemp\n"); return 1; }
    if (!mkdtemp(dir_noattrib)) { printf("FAIL: mkdtemp\n"); return 1; }
    if (!mkdtemp(dir_noqueries)) { printf("FAIL: mkdtemp\n"); return 1; }
    if (!mkdtemp(dir_noattrib_nosum)) { printf("FAIL: mkdtemp\n"); return 1; }
    if (!mkdtemp(dir_noqueries_nosum)) { printf("FAIL: mkdtemp\n"); return 1; }
    if (!mkdtemp(dir_straddle)) { printf("FAIL: mkdtemp\n"); return 1; }
    uint64_t base_mono = mono_origin();
    build_fixture(dir, base_mono, 1, FIXTURE_MIXED);
    build_fixture(dir_nosum, base_mono, 0, FIXTURE_MIXED);
    build_fixture(dir_noattrib, base_mono, 1, FIXTURE_NO_UNATTRIBUTED);
    build_fixture(dir_noqueries, base_mono, 1, FIXTURE_NO_QUERIES);
    build_fixture(dir_noattrib_nosum, base_mono, 0, FIXTURE_NO_UNATTRIBUTED);
    build_fixture(dir_noqueries_nosum, base_mono, 0, FIXTURE_NO_QUERIES);
    build_fixture(dir_straddle, base_mono, 1, FIXTURE_STRADDLE);

    /* Sections 2 and 2b fork BEFORE this process ever calls load_max_events()
     * (section 1 does, via server_load_events_fi inside the backfill), so
     * the child's PGWT_LOAD_MAX_EVENTS=3 override lands in a process whose
     * cache is still unset, and never leaks into section 1's budget. */
    test_overload_reports_absence(dir);
    test_query_filter_overload_hints(dir);
    test_backfill_matches_raw(dir, base_mono);
    test_queries_top_wait_matches_raw(dir);
    test_queries_force_raw(dir_straddle);
    test_route_predicate_discriminates(dir_nosum);
    test_events_query_filter_uses_raw(dir, 0);
    test_events_query_filter_uses_raw(dir_straddle, 1);
    test_queries_unattributed_survives(dir);
    test_queries_empty_controls(dir_noattrib, 0, 1);
    test_queries_empty_controls(dir_noqueries, 1, 1);
    test_queries_empty_controls(dir_noattrib_nosum, 0, 0);
    test_queries_empty_controls(dir_noqueries_nosum, 1, 0);
    test_queries_failures(dir);
    test_events_routes_discriminate(dir_nosum);

    /* Require the observed assertion total: a truncated run must fail
     * even when all assertions it did execute passed. */
    if (tests_run < 151) {
        printf("FAIL: only %d assertions executed -- a truncated run must "
               "not read as a pass\n", tests_run);
        tests_failed++;
    }

    printf("\n%d tests, %d failed\n", tests_run, tests_failed);
    return tests_failed ? 1 : 0;
}
