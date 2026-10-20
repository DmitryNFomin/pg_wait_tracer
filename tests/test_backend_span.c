/* test_backend_span.c — the #455 gate: BACKEND ACTIVITY, measured from
 * timestamps and from nothing else.
 *
 * THE DEFECT. Queries' top row read `DB Time 720.4 s / 59.8%` beside
 * `Execs 91, Avg Exec 33 us`. The 714 s `Lock:relation` behind that DB Time
 * is taken in parse analysis, BEFORE `query__execute__start`, so no latency
 * column can see it; the only arithmetic the response offered,
 * DB Time / Execs = 7.9 s, divides ONE session's wait by ninety-one OTHER
 * sessions' executions and is a number nobody experienced.
 *
 * THE FIXTURE IS THE DEFECT. Section 1 writes a real trace holding exactly
 * that shape -- 91 completed short executions on one pid, and a DIFFERENT
 * pid blocked for 714 s with NO EXEC_START at all -- drives the REAL
 * `top_queries` / `span_detail` / `execution_detail` handlers in
 * src/server.c over it, and asserts the span's identity, boundaries, status
 * and ZERO contained executions exactly.
 *
 * WHY THE MUTATIONS ARE THE DELIVERABLE (section 4). A green assertion
 * proves nothing about what it would reject. Section 4 takes the SAME oracle
 * that passed in section 1, feeds it a response mutated into each of the
 * five ways this fix can be got wrong, and requires every one to go RED:
 *
 *   1. span duration replaced by DB Time / Execs
 *   2. the span withheld unless an EXEC_START exists
 *   3. the window end substituted for an unobserved CMD_END
 *   4. parallel-worker time added to the leader's wall clock
 *   5. every CMD marker removed  -> `unmeasured`, never 0, never completed
 *
 * WHY THE BYPASS SUITE IS THE OTHER HALF (section 7). Every defect this
 * project keeps finding in review is a FALSE NEGATIVE: a gate that was
 * unreachable, skipped, or satisfied without checking anything. Section 7
 * walks the ways THIS gate's detection can be made blind -- an empty event
 * array, an unordered one, a missing span (absent rather than wrong), an
 * unresolvable identity, an exhausted index, and an injected allocation
 * failure at every allocation point -- and requires a REFUSAL with a reason
 * in each. A pass that cannot see must refuse, never approve. Section 8 is
 * a non-vacuity ledger: a run that executed fewer assertions than the suite
 * contains FAILS, because a section that silently stopped running is the
 * same false negative wearing a green coat.
 *
 * Runs anywhere pgwt-server builds (no BPF, no live PostgreSQL). Writes real
 * traces with the real writer into temp dirs and calls the real handlers.
 * `./test_backend_span bench` additionally measures the reducer against a
 * 900-second capture; that measurement is PRINTED, never asserted -- a
 * wall-clock gate on a shared runner is runner noise dressed up as a check.
 */
#define main pgwt_server_main
#include "../src/server.c"
#undef main

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

#define US      1000ULL
#define MS      1000000ULL
#define ONE_SEC 1000000000ULL

#define EV_LOCK        WEI(PG_WAIT_LOCK, 0)
#define EV_IO          WEI(PG_WAIT_IO, 21)
#define EV_CLIENT_READ WEI(PG_WAIT_CLIENT, 0)

/* ── response capture (same shape as tests/test_semantics_wire.c) ─────── */

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
    char *last = json;
    for (char *p = json; *p; p++)
        if (*p == '\n' && p[1]) last = p + 1;
    cJSON *out = cJSON_Parse(last);
    free(json);
    return out;
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

static int is_json_null(cJSON *o, const char *k)
{
    cJSON *v = cJSON_GetObjectItem(o, k);
    return v != NULL && cJSON_IsNull(v);
}
static double jnum(cJSON *o, const char *k)
{
    cJSON *v = cJSON_GetObjectItem(o, k);
    return (v && cJSON_IsNumber(v)) ? v->valuedouble : -1.0;
}
static const char *jstr(cJSON *o, const char *k)
{
    cJSON *v = cJSON_GetObjectItem(o, k);
    return (v && cJSON_IsString(v)) ? v->valuestring : NULL;
}
static int jtrue(cJSON *o, const char *k)
{
    cJSON *v = cJSON_GetObjectItem(o, k);
    return v && cJSON_IsTrue(v);
}
static int streq(const char *a, const char *b)
{
    return a && b && strcmp(a, b) == 0;
}

/* ── fixture A: THE #455 SHAPE ────────────────────────────────────────────
 *
 *   pid 701 ("fast"): 91 COMPLETE commands. Each is CMD_START, EXEC_START,
 *     one 33 us IO wait, EXEC_END, CMD_END, one millisecond wide, 100 ms
 *     apart, all carrying QID. These are the `Execs 91, Avg Exec 33 us`.
 *   pid 702 ("blocked"): ONE CMD_START at base + 10 s, then a single 714 s
 *     Lock:relation record carrying the SAME QID, and NOTHING else -- no
 *     EXEC_START, no EXEC_END, no CMD_END. This is the backend whose wait
 *     is taken before executor entry, still blocked at the capture's end.
 *
 * Numbers chosen so the forbidden quotient cannot coincide with the truth:
 *   DB Time        = 714 s + 91 x 33 us = 714003.003 ms
 *   DB Time / Execs= 7846.19 ms      <- the number nobody experienced
 *   span observed  = 714000.000 ms   <- the only honest answer
 * A factor of 91 apart, so an assertion on the exact value cannot be
 * satisfied by the quotient and vice versa.
 */
#define PID_FAST     701
#define PID_BLOCKED  702
#define QID          0x5555555555555555ULL
#define N_FAST       91
#define FAST_WAIT_NS (33 * US)
#define BLOCK_WAIT_NS (714 * ONE_SEC)

static uint64_t fx_base;          /* capture origin, MONO (as pushed) */
static uint64_t fx_block_start;   /* pid 702's CMD_START, MONO */
static uint64_t fx_block_end;     /* pid 702's last evidence, MONO */
/* MONO -> WALL. The writer records timestamps in the monotonic domain and
 * the loader re-anchors every record to wall time through the file header's
 * clock offset, so a span's `start_ns` on the wire is NOT the value this
 * fixture pushed. Calibrated once from srv->earliest_wall_ns against the
 * fixture's first pushed event -- and then PINNED by an independent
 * assertion (the fast pid's own first span) so a wrong calibration shows up
 * as a failure here instead of being absorbed into every expectation. */
static uint64_t fx_wall_off;
#define WALL(mono) ((mono) + fx_wall_off)

static void wpush(struct pgwt_event_writer *w, uint64_t ts, uint32_t pid,
                  uint32_t old_ev, uint32_t new_ev, uint64_t dur,
                  uint64_t qid)
{
    struct pgwt_trace_event e;
    memset(&e, 0, sizeof(e));
    e.timestamp_ns = ts;
    e.pid = pid;
    e.old_event = old_ev;
    e.new_event = new_ev;
    e.duration_ns = dur;
    e.query_id = qid;
    e.cpu_ns = PGWT_CPU_NS_UNKNOWN;
    pgwt_writer_push_event(w, &e);
}

static void wmark(struct pgwt_event_writer *w, uint64_t ts, uint32_t pid,
                  uint32_t marker, uint64_t qid)
{
    wpush(w, ts, pid, marker, marker, 0, qid);
}

static void build_fixture_a(const char *dir, uint64_t base)
{
    struct pgwt_event_writer w;
    if (pgwt_writer_init(&w, dir, 180000, 0, NULL) != 0) {
        printf("  FATAL: writer init (a)\n"); exit(2);
    }
    for (int k = 0; k < N_FAST; k++) {
        uint64_t t = base + (uint64_t)k * 100 * MS;
        wmark(&w, t, PID_FAST, PGWT_MARKER_CMD_START, 0);
        wmark(&w, t + 10 * US, PID_FAST, PGWT_MARKER_EXEC_START, QID);
        wpush(&w, t + 10 * US + FAST_WAIT_NS, PID_FAST, EV_IO, 0,
              FAST_WAIT_NS, QID);
        wmark(&w, t + 50 * US, PID_FAST, PGWT_MARKER_EXEC_END, QID);
        wmark(&w, t + 1 * MS, PID_FAST, PGWT_MARKER_CMD_END, QID);
    }
    /* The blocked backend: a command opens and never closes, and the only
     * record inside it is the pre-executor lock wait. */
    fx_block_start = base + 10 * ONE_SEC;
    fx_block_end   = fx_block_start + BLOCK_WAIT_NS;
    wmark(&w, fx_block_start, PID_BLOCKED, PGWT_MARKER_CMD_START, 0);
    wpush(&w, fx_block_end, PID_BLOCKED, EV_LOCK, 0, BLOCK_WAIT_NS, QID);
    pgwt_writer_close(&w);
}

/* ── the ORACLE ───────────────────────────────────────────────────────────
 *
 * ONE function, used twice: on the real response (must report 0 failures)
 * and on each mutated response in section 4 (each must report > 0). A
 * mutation that the oracle still accepts is a hole in the oracle, which is
 * the only thing section 4 can discover and the reason it exists.
 *
 * `ba` is the row's `backend_activity` object, or NULL when the response
 * withheld it (mutation 2). Every check is on an EXACT value or an explicit
 * JSON null -- no tolerances, because every quantity here is an integer
 * number of nanoseconds divided by 1e6.
 */
struct oracle_ctx {
    double   db_time_ms;      /* the row's DB Time */
    double   exec_count;      /* the row's Execs */
    uint32_t want_pid;
    uint64_t want_start_ns;
    double   want_observed_ms;
    uint64_t window_to_ns;
};

#define OFAIL(cond, msg) do { if (!(cond)) {                             \
    nf++; if (verbose) printf("     oracle-fail: %s\n", msg);            \
    else if (!*first) *first = msg; } } while (0)

static int span_oracle(cJSON *ba, const struct oracle_ctx *c, int verbose,
                       const char **first)
{
    int nf = 0;
    const char *dummy = NULL;
    if (!first) first = &dummy;

    /* 0. The span must EXIST. A backend with zero executions is exactly
     *    what #455 is about; withholding it is mutation 2. */
    OFAIL(ba != NULL, "no backend_activity object on the row");
    if (!ba)
        return nf;

    OFAIL(streq(jstr(ba, "source"), "in_flight"),
          "source is not in_flight");
    cJSON *s = cJSON_GetObjectItem(ba, "span");
    OFAIL(s != NULL && cJSON_IsObject(s), "no span object");
    if (!s || !cJSON_IsObject(s))
        return nf;

    /* 1. Identity: backend identity + the OBSERVED CMD_START timestamp. */
    char want_ident[48];
    snprintf(want_ident, sizeof(want_ident), "%u:%llu",
             c->want_pid, (unsigned long long)c->want_start_ns);
    OFAIL(streq(jstr(s, "identity"), want_ident), "span identity differs");
    OFAIL((uint32_t)jnum(s, "pid") == c->want_pid, "span pid differs");

    /* 2. Boundaries: the start observed, the end NOT. */
    char want_start[32];
    snprintf(want_start, sizeof(want_start), "%llu",
             (unsigned long long)c->want_start_ns);
    OFAIL(streq(jstr(s, "start_ns"), want_start), "start_ns differs");
    OFAIL(is_json_null(s, "end_ns"),
          "end_ns is not null: an unobserved end was given a value");

    /* 3. Status: two axes, neither collapsed. */
    OFAIL(streq(jstr(s, "kind"), "In flight"), "kind is not 'In flight'");
    OFAIL(streq(jstr(s, "completion"), "in_flight"),
          "completion is not in_flight");
    OFAIL(streq(jstr(s, "measurement"), "partial"),
          "measurement is not partial");
    OFAIL(streq(jstr(s, "reason"),
                "end not observed: the backend was still active at the end "
                "of the processed stream"),
          "reason does not name the unobserved end");

    /* 4. Duration. The observed elapsed is EXACT and comes from
     *    timestamps; the completed duration DOES NOT EXIST. */
    OFAIL(jnum(s, "observed_elapsed_ms") == c->want_observed_ms,
          "observed_elapsed_ms is not the observed timestamp difference");
    OFAIL(is_json_null(s, "completed_duration_ms"),
          "completed_duration_ms is not null for an unobserved end");
    OFAIL(streq(jstr(s, "duration_source"), "timestamps"),
          "duration_source is not 'timestamps'");
    OFAIL(!jtrue(s, "duration_additive"),
          "the span is declared additive across rows");

    /* 5. THE FORBIDDEN QUOTIENT. Separated by construction (a factor of 91
     *    in this fixture), so the gate rejects it on the number itself and
     *    not merely on a label. */
    double forbidden = c->exec_count > 0 ? c->db_time_ms / c->exec_count : -1;
    OFAIL(forbidden > 0 &&
          fabs(jnum(s, "observed_elapsed_ms") - forbidden) > 1000.0,
          "observed_elapsed_ms is within 1 s of DB Time / Execs");
    OFAIL(!is_json_null(s, "completed_duration_ms") ||
          fabs(forbidden - c->want_observed_ms) > 1000.0,
          "the fixture does not separate the honest duration from the "
          "forbidden quotient: the gate could not tell them apart");

    /* 6. The window end was never substituted for anything. */
    char window_str[32];
    snprintf(window_str, sizeof(window_str), "%llu",
             (unsigned long long)c->window_to_ns);
    OFAIL(!streq(jstr(s, "end_ns"), window_str),
          "end_ns is the window end");

    /* 7. ZERO contained executions -- a number, not an absence. */
    OFAIL(jnum(s, "exec_starts") == 0, "exec_starts is not 0");
    OFAIL(jnum(s, "exec_ends") == 0, "exec_ends is not 0");
    OFAIL(jnum(s, "records") == 1, "records is not 1");

    /* 8. The query association came from the WAIT RECORD, not a marker. */
    cJSON *qs = cJSON_GetObjectItem(s, "queries");
    OFAIL(qs && cJSON_IsArray(qs) && cJSON_GetArraySize(qs) == 1,
          "the span is not associated with exactly one query");
    if (qs && cJSON_GetArraySize(qs) == 1) {
        cJSON *q0 = cJSON_GetArrayItem(qs, 0);
        OFAIL(jnum(q0, "records") == 1,
              "the association did not come from a wait record");
        OFAIL(jnum(q0, "exec_starts") == 0,
              "the association claims an EXEC_START that does not exist");
    }
    return nf;
}

/* ── 1. the #455 gate ─────────────────────────────────────────────────── */

static struct oracle_ctx ctx_a;
static cJSON *resp_a_row;      /* kept for section 4's mutations */

static void test_455_gate(struct pgwt_server *srv)
{
    printf("--- 1. #455: Queries exposes the blocked backend's span ---\n");

    struct pgwt_request req = {0};
    req.id = 1;
    struct pgwt_wfid wf = {0};
    CHECK(!should_use_summaries(srv, &req, &wf),
          "the RAW route answers this capture (no summary files exist), so "
          "this section tests the path it claims to");

    /* Calibrate the mono->wall offset, then PIN it: the capture's earliest
     * wall timestamp must be the fixture's first pushed event, and the fast
     * pid's first span must carry exactly that identity. If the anchor were
     * wrong, this is where it fails -- not silently inside every other
     * expectation. */
    fx_wall_off = srv->earliest_wall_ns - fx_base;

    cJSON *resp = call_handler(handle_top_queries, srv, &req);
    if (!resp) { printf("  FATAL: no top_queries response\n"); exit(2); }
    cJSON *rows = cJSON_GetObjectItem(resp, "rows");
    cJSON *row = qrow_by_id(rows, QID);
    CHECK(row != NULL, "the Queries response has a row for the query");
    if (!row) { cJSON_Delete(resp); return; }

    /* The defect's own numbers, re-measured from the fixture, so the gate
     * is anchored to the shape it claims to reproduce. */
    double db_time_ms = jnum(row, "total_ms");
    double execs = jnum(row, "exec_count");
    CHECK(fabs(db_time_ms - 714003.003) < 0.5,
          "DB Time is %.3f ms (the 714 s pre-executor lock plus 91 short "
          "executions)", db_time_ms);
    CHECK(execs == (double)N_FAST,
          "Execs is %.0f -- the completed EXEC pairs, all on the OTHER pid",
          execs);

    ctx_a.db_time_ms = db_time_ms;
    ctx_a.exec_count = execs;
    ctx_a.want_pid = PID_BLOCKED;
    ctx_a.want_start_ns = WALL(fx_block_start);
    ctx_a.want_observed_ms = (double)BLOCK_WAIT_NS / 1e6;
    ctx_a.window_to_ns = srv->latest_wall_ns;

    CHECK(jtrue(resp, "backend_activity_available"),
          "the response says the backend span pass answered");
    CHECK(jnum(resp, "backend_activity_inflight") == 1,
          "exactly one span is in flight at the end of the capture");
    CHECK(streq(jstr(resp, "backend_activity_scope"), "whole-query"),
          "the scope of the span summary is stated on the wire");

    cJSON *ba = cJSON_GetObjectItem(row, "backend_activity");
    const char *first = NULL;
    int nf = span_oracle(ba, &ctx_a, 1, &first);
    CHECK(nf == 0, "the span oracle accepts the real response "
          "(%d violations)", nf);

    /* A positive control for the oracle's own reach: the fast pid's 91
     * spans exist, are fully measured and are NOT the one chosen for the
     * row -- so "the oracle passed" cannot mean "there was only one span". */
    CHECK(jnum(resp, "backend_activity_spans") == (double)(N_FAST + 1),
          "the pass produced %d spans (91 complete commands + the blocked "
          "one), so the row's choice was a CHOICE", N_FAST + 1);
    CHECK(jnum(resp, "backend_activity_unmeasured") == 0,
          "no span in this capture is unmeasured: every one has markers");

    resp_a_row = cJSON_Duplicate(row, 1);
    cJSON_Delete(resp);
}

/* ── 2. the drill, with ZERO EXEC_START markers ───────────────────────── */

static void test_drill(struct pgwt_server *srv)
{
    printf("--- 2. span_detail opens with zero EXEC_START markers ---\n");

    struct pgwt_request req = {0};
    req.id = 2;
    req.filter.pid = PID_BLOCKED;
    cJSON *resp = call_handler(handle_span_detail, srv, &req);
    if (!resp) { printf("  FATAL: no span_detail response\n"); exit(2); }
    CHECK(jtrue(resp, "available"), "the pid drill answered");
    cJSON *spans = cJSON_GetObjectItem(resp, "spans");
    CHECK(spans && cJSON_GetArraySize(spans) == 1,
          "the pid drill returns exactly one span for the blocked backend");
    if (spans && cJSON_GetArraySize(spans) == 1) {
        cJSON *s = cJSON_GetObjectItem(cJSON_GetArrayItem(spans, 0), "span");
        char want[48];
        snprintf(want, sizeof(want), "%u:%llu", PID_BLOCKED,
                 (unsigned long long)WALL(fx_block_start));
        CHECK(streq(jstr(s, "identity"), want),
              "the drill's span carries the SAME identity as the row's");
        CHECK(jnum(s, "exec_starts") == 0,
              "the drill opened on a span with zero executions");
        CHECK(jnum(s, "observed_elapsed_ms") == (double)BLOCK_WAIT_NS / 1e6,
              "the drill's observed elapsed is the same 714000 ms");
    }
    CHECK(is_json_null(resp, "empty_state"),
          "a drill that found something has no empty state");
    CHECK(jnum(resp, "in_flight") == 1, "the drill counts one in-flight span");
    cJSON_Delete(resp);

    /* The FAST pid, as the closed/measured positive control -- and as the
     * independent PIN on the mono->wall calibration: the capture's earliest
     * wall timestamp must be this pid's first CMD_START. A wrong anchor
     * fails HERE rather than being absorbed into every other expectation. */
    memset(&req, 0, sizeof(req));
    req.id = 12;
    req.filter.pid = PID_FAST;
    req.limit = 100;
    resp = call_handler(handle_span_detail, srv, &req);
    spans = resp ? cJSON_GetObjectItem(resp, "spans") : NULL;
    CHECK(resp && jnum(resp, "scoped_spans") == (double)N_FAST,
          "the fast pid has exactly %d spans -- one per completed command",
          N_FAST);
    CHECK(resp && jnum(resp, "closed") == (double)N_FAST &&
          jnum(resp, "in_flight") == 0 && jnum(resp, "partial") == 0,
          "all %d are CLOSED and fully measured", N_FAST);
    if (spans && cJSON_GetArraySize(spans) > 0) {
        /* The selection is ordered by observed elapsed descending, and
         * every fast span is identical, so the tiebreak (pid, identity
         * ascending) puts the FIRST command first. */
        cJSON *s0 = cJSON_GetObjectItem(cJSON_GetArrayItem(spans, 0), "span");
        char want[48];
        snprintf(want, sizeof(want), "%u:%llu", PID_FAST,
                 (unsigned long long)WALL(fx_base));
        CHECK(streq(jstr(s0, "identity"), want),
              "and the first one's identity is the capture's earliest wall "
              "timestamp (%s) -- the calibration pin", jstr(s0, "identity"));
        CHECK(jnum(s0, "completed_duration_ms") == 1.0,
              "with a completed duration of exactly 1 ms, from its two CMD "
              "markers");
        CHECK(streq(jstr(s0, "measurement"), "measured") &&
              streq(jstr(s0, "kind"), "Closed"),
              "measured AND closed -- the contrast with the blocked span");
        CHECK(jnum(s0, "exec_starts") == 1,
              "and it does contain its one execution, so 'exec_starts == 0' "
              "above is a measurement and not a constant");
    }
    cJSON_Delete(resp);

    /* Query scope: the same span, selected by its association. */
    memset(&req, 0, sizeof(req));
    req.id = 3;
    req.filter.query_id = QID;
    resp = call_handler(handle_span_detail, srv, &req);
    spans = resp ? cJSON_GetObjectItem(resp, "spans") : NULL;
    int found = 0;
    for (int i = 0; spans && i < cJSON_GetArraySize(spans); i++) {
        cJSON *s = cJSON_GetObjectItem(cJSON_GetArrayItem(spans, i), "span");
        if ((uint32_t)jnum(s, "pid") == PID_BLOCKED) found = 1;
    }
    CHECK(found, "the query-scoped drill selects the blocked backend's span");
    CHECK(spans && cJSON_GetArraySize(spans) > 1,
          "and the fast pid's spans too -- the query filter selects spans, "
          "it does not reduce the answer to one");
    cJSON_Delete(resp);

    /* Unattributed scope: this capture has no unattributed activity, and
     * the EMPTY STATE must say so rather than return a bare empty list. */
    memset(&req, 0, sizeof(req));
    req.id = 4;
    req.unattributed = 1;
    resp = call_handler(handle_span_detail, srv, &req);
    CHECK(resp && jtrue(resp, "available"),
          "the unattributed disclosure answers");
    spans = resp ? cJSON_GetObjectItem(resp, "spans") : NULL;
    CHECK(spans && cJSON_GetArraySize(spans) == 0,
          "no span in this capture lacks an association");
    CHECK(jstr(resp, "empty_state") != NULL,
          "the empty state is a MESSAGE, not an empty list left to speak "
          "for itself");
    cJSON_Delete(resp);
}

/* ── 3. the prefix: retained index and legacy decode agree exactly ────── */

static char *span_json_for_subwindow(struct pgwt_server *srv, const char *mode)
{
    setenv("PGWT_EXEC_PREFIX_INDEX", mode, 1);
    /* A fresh server: the retained-marker index is built during coverage
     * refresh, so reusing one that already refreshed under the other mode
     * would compare a path against itself. */
    struct pgwt_server *s2 = calloc(1, sizeof(*s2));
    if (!s2 || server_init(s2, srv->trace_dir) != 0) {
        printf("  FATAL: server_init (prefix %s)\n", mode); exit(2);
    }
    struct pgwt_request req = {0};
    req.id = 5;
    req.filter.pid = PID_BLOCKED;
    /* Starts 300 s AFTER the blocked command opened: without the prefix the
     * CMD_START is not in the window at all. */
    req.from_ns = WALL(fx_block_start) + 300 * ONE_SEC;
    req.to_ns = srv->latest_wall_ns;
    cJSON *resp = call_handler(handle_span_detail, s2, &req);
    char *out = NULL;
    cJSON *spans = resp ? cJSON_GetObjectItem(resp, "spans") : NULL;
    if (spans && cJSON_GetArraySize(spans) > 0)
        out = cJSON_PrintUnformatted(spans);
    cJSON_Delete(resp);
    server_destroy(s2);
    free(s2);
    unsetenv("PGWT_EXEC_PREFIX_INDEX");
    return out;
}

static void test_prefix_identity(struct pgwt_server *srv)
{
    printf("--- 3. pre-window CMD context: index == legacy, bit for bit ---\n");
    char *idx = span_json_for_subwindow(srv, "1");
    char *leg = span_json_for_subwindow(srv, "0");
    CHECK(idx != NULL && leg != NULL,
          "both prefix paths produced a span for a window that does not "
          "contain the CMD_START (the prefix loader found it)");
    CHECK(idx && leg && strcmp(idx, leg) == 0,
          "the retained-marker index and the exact full decode produce "
          "IDENTICAL spans");
    if (idx) {
        cJSON *arr = cJSON_Parse(idx);
        cJSON *s = arr && cJSON_GetArraySize(arr) > 0
                 ? cJSON_GetObjectItem(cJSON_GetArrayItem(arr, 0), "span")
                 : NULL;
        char want[48];
        snprintf(want, sizeof(want), "%u:%llu", PID_BLOCKED,
                 (unsigned long long)WALL(fx_block_start));
        CHECK(s && streq(jstr(s, "identity"), want),
              "the recovered span's identity is the PRE-WINDOW CMD_START, "
              "not the window's left edge");
        CHECK(s && streq(jstr(s, "measurement"), "partial") &&
              is_json_null(s, "completed_duration_ms"),
              "and it is still partial with no completed duration: "
              "recovering a start does not invent an end");
        cJSON_Delete(arr);
    }
    free(idx);
    free(leg);
}

/* ── 4. the mutations: the SAME oracle must go red for each ───────────── */

static void mutate_set_num(cJSON *s, const char *k, double v)
{
    cJSON_DeleteItemFromObject(s, k);
    cJSON_AddNumberToObject(s, k, v);
}
static void mutate_set_str(cJSON *s, const char *k, const char *v)
{
    cJSON_DeleteItemFromObject(s, k);
    cJSON_AddStringToObject(s, k, v);
}

static void run_mutation(const char *name, cJSON *mutated_ba)
{
    const char *first = NULL;
    int nf = span_oracle(mutated_ba, &ctx_a, 0, &first);
    CHECK(nf > 0, "mutation '%s' is REJECTED (%d oracle violations, first: "
          "%s)", name, nf, first ? first : "-");
    if (mutated_ba) cJSON_Delete(mutated_ba);
}

static void test_mutations(void)
{
    printf("--- 4. mutations: each must FAIL the section-1 oracle ---\n");
    if (!resp_a_row) {
        printf("  FAIL: section 1 produced no row to mutate\n");
        failures++; checks++;
        return;
    }
    /* Sanity: the UNMUTATED copy still passes, so a mutation's red cannot
     * be the copy being broken. */
    cJSON *base = cJSON_Duplicate(
        cJSON_GetObjectItem(resp_a_row, "backend_activity"), 1);
    const char *first = NULL;
    CHECK(span_oracle(base, &ctx_a, 0, &first) == 0,
          "the unmutated copy still passes, so every red below is the "
          "MUTATION and not the copy");
    cJSON_Delete(base);

    /* 1. span duration replaced by DB Time / Execs. */
    cJSON *m = cJSON_Duplicate(
        cJSON_GetObjectItem(resp_a_row, "backend_activity"), 1);
    mutate_set_num(cJSON_GetObjectItem(m, "span"), "observed_elapsed_ms",
                   ctx_a.db_time_ms / ctx_a.exec_count);
    run_mutation("1: duration = DB Time / Execs", m);

    /* 2. the span withheld because no EXEC_START exists. */
    run_mutation("2: span withheld without an EXEC_START", NULL);

    /* 3. the window end substituted for the unobserved CMD_END. */
    m = cJSON_Duplicate(cJSON_GetObjectItem(resp_a_row, "backend_activity"), 1);
    cJSON *s = cJSON_GetObjectItem(m, "span");
    char wstr[32];
    snprintf(wstr, sizeof(wstr), "%llu",
             (unsigned long long)ctx_a.window_to_ns);
    mutate_set_str(s, "end_ns", wstr);
    mutate_set_str(s, "completion", "closed");
    mutate_set_str(s, "kind", "Closed");
    mutate_set_num(s, "completed_duration_ms",
                   (double)(ctx_a.window_to_ns - ctx_a.want_start_ns) / 1e6);
    run_mutation("3: window end substituted for CMD_END", m);

    /* 4. parallel-worker time added to the leader's wall clock. The worker
     *    fixture drives this one too (section 6f); here the mutation is on
     *    the duration itself, which is the thing that must never grow. */
    m = cJSON_Duplicate(cJSON_GetObjectItem(resp_a_row, "backend_activity"), 1);
    mutate_set_num(cJSON_GetObjectItem(m, "span"), "observed_elapsed_ms",
                   ctx_a.want_observed_ms + 5000.0);
    run_mutation("4: worker time added to leader wall duration", m);

    /* And the two label mutations that would hide the measurement axes. */
    m = cJSON_Duplicate(cJSON_GetObjectItem(resp_a_row, "backend_activity"), 1);
    mutate_set_str(cJSON_GetObjectItem(m, "span"), "measurement", "measured");
    run_mutation("4b: an unobserved end reported as fully measured", m);

    m = cJSON_Duplicate(cJSON_GetObjectItem(resp_a_row, "backend_activity"), 1);
    mutate_set_num(cJSON_GetObjectItem(m, "span"), "exec_starts", 1);
    run_mutation("4c: a fabricated contained execution", m);
}

/* ── 5. mutation 5: EVERY CMD marker removed ──────────────────────────── */

static void build_fixture_nocmd(const char *dir, uint64_t base)
{
    /* The same blocked backend, with NO CMD markers at all -- the sampled
     * tier, or a capture that started mid-command and never saw either
     * boundary. */
    struct pgwt_event_writer w;
    if (pgwt_writer_init(&w, dir, 180000, 0, NULL) != 0) {
        printf("  FATAL: writer init (nocmd)\n"); exit(2);
    }
    wpush(&w, base + 10 * MS, PID_BLOCKED, EV_LOCK, 0, 10 * MS, QID);
    wpush(&w, base + 714 * ONE_SEC, PID_BLOCKED, EV_LOCK, 0,
          714 * ONE_SEC - 10 * MS, QID);
    pgwt_writer_close(&w);
}

static void test_no_cmd_markers(const char *dir)
{
    printf("--- 5. mutation 5: no CMD markers -> UNMEASURED, never zero ---\n");
    struct pgwt_server *srv = calloc(1, sizeof(*srv));
    if (!srv || server_init(srv, dir) != 0) {
        printf("  FATAL: server_init (nocmd)\n"); exit(2);
    }
    struct pgwt_request req = {0};
    req.id = 6;
    cJSON *resp = call_handler(handle_top_queries, srv, &req);
    cJSON *row = resp ? qrow_by_id(cJSON_GetObjectItem(resp, "rows"), QID)
                      : NULL;
    CHECK(row != NULL, "the query still has a Queries row");
    cJSON *ba = row ? cJSON_GetObjectItem(row, "backend_activity") : NULL;
    CHECK(ba && cJSON_IsObject(ba),
          "backend activity is still REPORTED with no CMD markers: absence "
          "of markers is not absence of activity");
    if (ba && cJSON_IsObject(ba)) {
        cJSON *s = cJSON_GetObjectItem(ba, "span");
        CHECK(streq(jstr(ba, "source"), "not_measured"),
              "the row says which span it chose: not_measured");
        CHECK(streq(jstr(s, "measurement"), "unmeasured"),
              "measurement is 'unmeasured'");
        CHECK(streq(jstr(s, "kind"), "Not measured"),
              "the one-word kind is 'Not measured' -- not a blank");
        CHECK(streq(jstr(s, "completion"), "unknown"),
              "completion is 'unknown': with no markers we cannot say the "
              "backend finished");
        CHECK(is_json_null(s, "completed_duration_ms"),
              "there is NO completed duration");
        CHECK(is_json_null(s, "start_ns") && is_json_null(s, "end_ns"),
              "neither boundary is given a value");
        /* NEVER ZERO: the observed elapsed still exists, from the records'
         * own timestamps, and is the thing an operator can act on. */
        CHECK(jnum(s, "observed_elapsed_ms") == 714000.0,
              "the observed elapsed is still 714000 ms, from record "
              "timestamps -- unmeasured boundaries are not a zero duration");
        CHECK(jstr(s, "reason") != NULL &&
              strstr(jstr(s, "reason"), "not measured") != NULL,
              "and the reason says why");
    }
    cJSON_Delete(resp);
    server_destroy(srv);
    free(srv);
}

/* ── 6. the mandated edge cases ───────────────────────────────────────── */

#define PID_EXT     711   /* extended protocol: 2 executions + client pause */
#define PID_MULTI   712   /* multi-statement command: 2 query ids */
#define PID_LEFT    713   /* left window edge: CMD_END with no CMD_START */
#define PID_NEST    714   /* nested execution */
#define PID_LEADER  715   /* parallel leader */
#define PID_WORKER  716   /* its worker */
#define PID_WONLY   717   /* a worker whose leader produced no span */
#define QID_EXT     0x1001ULL
#define QID_M1      0x2001ULL
#define QID_M2      0x2002ULL
#define QID_LEFT    0x3001ULL
#define QID_NEST    0x4001ULL
#define QID_PAR     0x5001ULL
#define QID_WONLY   0x6001ULL

static uint64_t fx_e_base;

static void build_fixture_edges(const char *dir, uint64_t base)
{
    struct pgwt_event_writer w;
    if (pgwt_writer_init(&w, dir, 180000, 0, NULL) != 0) {
        printf("  FATAL: writer init (edges)\n"); exit(2);
    }
    /* The LEFT edge first, in stream order: a CMD_END with no CMD_START
     * before it anywhere in the capture. */
    wpush(&w, base + 100 * MS, PID_LEFT, EV_LOCK, 0, 50 * MS, QID_LEFT);
    wmark(&w, base + 200 * MS, PID_LEFT, PGWT_MARKER_CMD_END, QID_LEFT);

    /* Extended protocol: ONE command, two executions, a 2 s ClientRead
     * between them. */
    uint64_t t = base + 1 * ONE_SEC;
    wmark(&w, t, PID_EXT, PGWT_MARKER_CMD_START, 0);
    wmark(&w, t + 10 * MS, PID_EXT, PGWT_MARKER_EXEC_START, QID_EXT);
    wpush(&w, t + 20 * MS, PID_EXT, EV_IO, 0, 5 * MS, QID_EXT);
    wmark(&w, t + 30 * MS, PID_EXT, PGWT_MARKER_EXEC_END, QID_EXT);
    wpush(&w, t + 2030 * MS, PID_EXT, EV_CLIENT_READ, 0, 2 * ONE_SEC,
          QID_EXT);
    wmark(&w, t + 2040 * MS, PID_EXT, PGWT_MARKER_EXEC_START, QID_EXT);
    wpush(&w, t + 2050 * MS, PID_EXT, EV_IO, 0, 5 * MS, QID_EXT);
    wmark(&w, t + 2060 * MS, PID_EXT, PGWT_MARKER_EXEC_END, QID_EXT);
    wmark(&w, t + 3 * ONE_SEC, PID_EXT, PGWT_MARKER_CMD_END, QID_EXT);

    /* Multi-statement command: one span, two query ids. */
    t = base + 5 * ONE_SEC;
    wmark(&w, t, PID_MULTI, PGWT_MARKER_CMD_START, 0);
    wmark(&w, t + 10 * MS, PID_MULTI, PGWT_MARKER_EXEC_START, QID_M1);
    wpush(&w, t + 20 * MS, PID_MULTI, EV_IO, 0, 5 * MS, QID_M1);
    wmark(&w, t + 30 * MS, PID_MULTI, PGWT_MARKER_EXEC_END, QID_M1);
    wmark(&w, t + 40 * MS, PID_MULTI, PGWT_MARKER_EXEC_START, QID_M2);
    wpush(&w, t + 50 * MS, PID_MULTI, EV_IO, 0, 5 * MS, QID_M2);
    wmark(&w, t + 60 * MS, PID_MULTI, PGWT_MARKER_EXEC_END, QID_M2);
    wmark(&w, t + 1 * ONE_SEC, PID_MULTI, PGWT_MARKER_CMD_END, QID_M2);

    /* Nested execution: EXEC_START, EXEC_START, EXEC_END, EXEC_END. */
    t = base + 8 * ONE_SEC;
    wmark(&w, t, PID_NEST, PGWT_MARKER_CMD_START, 0);
    wmark(&w, t + 10 * MS, PID_NEST, PGWT_MARKER_EXEC_START, QID_NEST);
    wmark(&w, t + 20 * MS, PID_NEST, PGWT_MARKER_EXEC_START, QID_NEST);
    wpush(&w, t + 30 * MS, PID_NEST, EV_IO, 0, 5 * MS, QID_NEST);
    wmark(&w, t + 40 * MS, PID_NEST, PGWT_MARKER_EXEC_END, QID_NEST);
    wmark(&w, t + 50 * MS, PID_NEST, PGWT_MARKER_EXEC_END, QID_NEST);
    wmark(&w, t + 500 * MS, PID_NEST, PGWT_MARKER_CMD_END, QID_NEST);

    /* Parallel query: a leader span of 1 s and a worker span of 4 s. The
     * worker is DELIBERATELY longer than the leader so that adding it
     * cannot be mistaken for noise. */
    t = base + 10 * ONE_SEC;
    wmark(&w, t, PID_LEADER, PGWT_MARKER_CMD_START, 0);
    wmark(&w, t + 10 * MS, PID_LEADER, PGWT_MARKER_EXEC_START, QID_PAR);
    wpush(&w, t + 20 * MS, PID_LEADER, EV_IO, 0, 5 * MS, QID_PAR);
    wmark(&w, t + 1 * ONE_SEC, PID_LEADER, PGWT_MARKER_CMD_END, QID_PAR);
    wmark(&w, t, PID_WORKER, PGWT_MARKER_CMD_START, 0);
    wpush(&w, t + 1 * MS, PID_WORKER, EV_IO, 0, 1 * MS, QID_PAR);
    wmark(&w, t + 4 * ONE_SEC, PID_WORKER, PGWT_MARKER_CMD_END, QID_PAR);

    /* A worker whose LEADER left no evidence at all. */
    t = base + 20 * ONE_SEC;
    wmark(&w, t, PID_WONLY, PGWT_MARKER_CMD_START, 0);
    wpush(&w, t + 1 * MS, PID_WONLY, EV_IO, 0, 1 * MS, QID_WONLY);
    wmark(&w, t + 2 * ONE_SEC, PID_WONLY, PGWT_MARKER_CMD_END, QID_WONLY);

    pgwt_writer_close(&w);

    /* backends.jsonl is how the server learns leader/worker links. */
    char path[1024];
    snprintf(path, sizeof(path), "%s/backends.jsonl", dir);
    FILE *f = fopen(path, "w");
    if (!f) { printf("  FATAL: backends.jsonl\n"); exit(2); }
    fprintf(f, "{\"pid\":%d,\"type\":\"client backend\"}\n", PID_LEADER);
    fprintf(f, "{\"pid\":%d,\"type\":\"parallel worker\",\"leader_pid\":%d}\n",
            PID_WORKER, PID_LEADER);
    fprintf(f, "{\"pid\":%d,\"type\":\"parallel worker\",\"leader_pid\":%d}\n",
            PID_WONLY, 999999);
    fclose(f);
}

static cJSON *edges_resp;   /* top_queries over the edges fixture */

static cJSON *edge_span(uint64_t qid)
{
    cJSON *row = qrow_by_id(cJSON_GetObjectItem(edges_resp, "rows"), qid);
    cJSON *ba = row ? cJSON_GetObjectItem(row, "backend_activity") : NULL;
    return (ba && cJSON_IsObject(ba)) ? ba : NULL;
}

static void test_edges(struct pgwt_server *srv)
{
    printf("--- 6. the mandated edge cases ---\n");
    struct pgwt_request req = {0};
    req.id = 7;
    edges_resp = call_handler(handle_top_queries, srv, &req);
    if (!edges_resp) { printf("  FATAL: edges response\n"); exit(2); }

    /* 6a. Extended protocol: one span, kept whole, disclosed. */
    cJSON *ba = edge_span(QID_EXT);
    CHECK(ba != NULL, "6a: the extended-protocol query has a span");
    if (ba) {
        cJSON *s = cJSON_GetObjectItem(ba, "span");
        CHECK(jnum(s, "exec_starts") == 2,
              "6a: ONE span holds BOTH executions");
        CHECK(jtrue(s, "multi_exec"), "6a: and says so");
        CHECK(jnum(s, "client_pause_ms") == 2000.0,
              "6a: the 2 s client pause inside it is reported, not removed");
        CHECK(jnum(s, "completed_duration_ms") == 3000.0,
              "6a: the span's duration is the whole 3 s between its CMD "
              "markers -- the client pause is not subtracted and the two "
              "executions are not summed");
        CHECK(jstr(s, "scope_note") != NULL,
              "6a: the disclosure names multiple statements/executions and "
              "client pauses");
    }

    /* 6b. Multi-statement command: one span, two rows, SAME identity,
     *     duration NOT additive. */
    cJSON *b1 = edge_span(QID_M1), *b2 = edge_span(QID_M2);
    CHECK(b1 && b2, "6b: both query ids have a span");
    if (b1 && b2) {
        cJSON *s1 = cJSON_GetObjectItem(b1, "span");
        cJSON *s2 = cJSON_GetObjectItem(b2, "span");
        CHECK(streq(jstr(s1, "identity"), jstr(s2, "identity")),
              "6b: the same span appears under both rows with the SAME "
              "identity (%s)", jstr(s1, "identity"));
        CHECK(jnum(s1, "completed_duration_ms") ==
              jnum(s2, "completed_duration_ms"),
              "6b: and the SAME duration -- it is not split between them");
        CHECK(jnum(s1, "completed_duration_ms") == 1000.0,
              "6b: which is the one span's 1000 ms, not 2 x 1000 ms");
        CHECK(!jtrue(s1, "duration_additive"),
              "6b: the wire says the duration is not additive across rows");
        CHECK(jtrue(s1, "multi_query"), "6b: and that it spans two queries");
    }

    /* 6c. Left window edge: CMD_END with no CMD_START. */
    ba = edge_span(QID_LEFT);
    CHECK(ba != NULL, "6c: the left-edge query has a span");
    if (ba) {
        cJSON *s = cJSON_GetObjectItem(ba, "span");
        CHECK(is_json_null(s, "start_ns"),
              "6c: start_ns is null -- the start was not captured");
        CHECK(streq(jstr(s, "completion"), "closed") &&
              streq(jstr(s, "measurement"), "partial"),
              "6c: closed AND partial -- the two axes do not collapse");
        CHECK(is_json_null(s, "completed_duration_ms"),
              "6c: and there is NO completed duration without both "
              "boundaries");
        CHECK(jnum(s, "observed_elapsed_ms") == 150.0,
              "6c: the observed portion only (the record's own 150 ms)");
        CHECK(jstr(s, "reason") &&
              strstr(jstr(s, "reason"), "not captured"),
              "6c: and the reason says 'start not captured'");
        CHECK(strstr(jstr(s, "identity"), "~") != NULL,
              "6c: the identity marks itself as evidence-derived (%s)",
              jstr(s, "identity"));
    }

    /* 6e. Nested execution: kept whole, duration still from CMD markers. */
    ba = edge_span(QID_NEST);
    CHECK(ba != NULL, "6e: the nested-execution query has a span");
    if (ba) {
        cJSON *s = cJSON_GetObjectItem(ba, "span");
        CHECK(jnum(s, "exec_starts") == 2 && jnum(s, "exec_ends") == 2,
              "6e: both EXEC_START markers are counted, nesting and all");
        CHECK(jnum(s, "completed_duration_ms") == 500.0,
              "6e: the span is still the 500 ms between its CMD markers -- "
              "nesting does not partition it");
        CHECK(streq(jstr(s, "measurement"), "measured"),
              "6e: and it is fully measured");
    }

    /* 6f. Parallel workers: the LEADER's span, workers SEPARATE. */
    ba = edge_span(QID_PAR);
    CHECK(ba != NULL, "6f: the parallel query has a leader span");
    if (ba) {
        cJSON *s = cJSON_GetObjectItem(ba, "span");
        CHECK((uint32_t)jnum(s, "pid") == PID_LEADER,
              "6f: the row shows the LEADER's span");
        CHECK(jnum(s, "completed_duration_ms") == 1000.0,
              "6f: the leader's own 1000 ms -- the worker's 4000 ms is NOT "
              "added");
        cJSON *wk = cJSON_GetObjectItem(ba, "workers");
        CHECK(wk && cJSON_IsObject(wk),
              "6f: worker activity is reported separately");
        if (wk && cJSON_IsObject(wk)) {
            CHECK(jnum(wk, "spans") == 1, "6f: one worker span");
            CHECK(jnum(wk, "observed_max_ms") == 4000.0,
                  "6f: with its own 4000 ms, as its own number");
            CHECK(!jtrue(wk, "added_to_leader"),
                  "6f: and the wire states it is not added to the leader");
        }
        /* MUTATION 4, driven here: adding the worker's time to the leader
         * must break the leader-duration assertion. */
        cJSON *mut = cJSON_Duplicate(s, 1);
        mutate_set_num(mut, "completed_duration_ms",
                       1000.0 + jnum(wk, "observed_max_ms"));
        CHECK(jnum(mut, "completed_duration_ms") != 1000.0,
              "6f/mutation 4: adding worker time to the leader's wall "
              "duration is REJECTED by the 6f assertion (%.0f != 1000)",
              jnum(mut, "completed_duration_ms"));
        cJSON_Delete(mut);
    }

    /* 6f-2. A worker with no leader evidence: explicit unavailable. */
    cJSON *row = qrow_by_id(cJSON_GetObjectItem(edges_resp, "rows"),
                            QID_WONLY);
    CHECK(row != NULL, "6f: the worker-only query has a Queries row");
    if (row) {
        CHECK(is_json_null(row, "backend_activity"),
              "6f: with NO span -- a worker's time is never shown as the "
              "leader's");
        const char *why = jstr(row, "backend_activity_unavailable_reason");
        CHECK(why && strstr(why, "leader evidence is unavailable"),
              "6f: and the reason says leader evidence is unavailable");
    }
}

/* ── 6g/6h. sampled interior and a real capture gap ───────────────────── */

#define PID_GAP   721
#define QID_GAP   0x7001ULL

static void build_fixture_gap(const char *dir, uint64_t base)
{
    /* A TIERED capture. Exact coverage in this codebase is delimited by the
     * ESCALATE_START/ESCALATE_END marks (src/server.c gen_cov_build, rules
     * 1-3); between two escalation windows the tier is sampled, which is a
     * real HOLE IN EXACT COVERAGE. A full-mode file claims whole-file exact
     * coverage by construction (rule 4) and therefore has no holes at all --
     * the first draft of this fixture was full-mode and the gap assertion
     * went green on a span with `gap_inside: false`, which is how a gate
     * that cannot see what it checks looks from the outside.
     *
     * One command spans the hole: CMD_START inside the first window,
     * CMD_END inside the second. BOTH boundaries are observed, so
     * `measured` is exactly the answer a bridging implementation would
     * give. It must still read PARTIAL. */
    struct pgwt_event_writer w;
    if (pgwt_writer_init(&w, dir, 180000, 0, NULL) != 0) {
        printf("  FATAL: writer init (gap)\n"); exit(2);
    }
    wmark(&w, base + 500 * MS, 0, PGWT_MARKER_ESCALATE_START, 0);
    wmark(&w, base + 1 * ONE_SEC, PID_GAP, PGWT_MARKER_CMD_START, 0);
    wpush(&w, base + 2 * ONE_SEC, PID_GAP, EV_LOCK, 0, 500 * MS, QID_GAP);
    wmark(&w, base + 3 * ONE_SEC, 0, PGWT_MARKER_ESCALATE_END, 0);

    /* The hole: sampled-only, 598 s wide. A REAL SAMPLES block, because the
     * reader derives FLAG_SAMPLE from the block type and coverage needs
     * has_samples to route through the marker state machine at all. */
    struct pgwt_trace_event smp;
    memset(&smp, 0, sizeof(smp));
    smp.timestamp_ns = base + 300 * ONE_SEC;
    smp.pid = PID_GAP;
    smp.new_event = EV_LOCK;
    smp.query_id = QID_GAP;
    smp.cpu_ns = PGWT_CPU_NS_UNKNOWN;
    pgwt_writer_push_samples(&w, &smp, 1, ONE_SEC);

    wmark(&w, base + 601 * ONE_SEC, 0, PGWT_MARKER_ESCALATE_START, 0);
    wpush(&w, base + 602 * ONE_SEC, PID_GAP, EV_LOCK, 0, 500 * MS, QID_GAP);
    wmark(&w, base + 603 * ONE_SEC, PID_GAP, PGWT_MARKER_CMD_END, QID_GAP);
    wmark(&w, base + 604 * ONE_SEC, 0, PGWT_MARKER_ESCALATE_END, 0);
    pgwt_writer_close(&w);
}

static void test_gap(const char *dir)
{
    printf("--- 6h. a known capture gap is never bridged ---\n");
    struct pgwt_server *srv = calloc(1, sizeof(*srv));
    if (!srv || server_init(srv, dir) != 0) {
        printf("  FATAL: server_init (gap)\n"); exit(2);
    }
    struct pgwt_request req = {0};
    req.id = 8;
    cJSON *resp = call_handler(handle_top_queries, srv, &req);
    cJSON *row = resp ? qrow_by_id(cJSON_GetObjectItem(resp, "rows"), QID_GAP)
                      : NULL;
    cJSON *ba = row ? cJSON_GetObjectItem(row, "backend_activity") : NULL;
    CHECK(ba && cJSON_IsObject(ba), "6h: the span is reported");
    if (ba && cJSON_IsObject(ba)) {
        cJSON *s = cJSON_GetObjectItem(ba, "span");
        CHECK(!is_json_null(s, "start_ns") && !is_json_null(s, "end_ns"),
              "6h: BOTH CMD boundaries were observed -- so 'measured' is "
              "the answer a bridging implementation would give");
        CHECK(streq(jstr(s, "measurement"), "partial"),
              "6h: and it is still PARTIAL, because a coverage hole falls "
              "inside it");
        CHECK(jtrue(s, "gap_inside"), "6h: the gap is named on the wire");
        CHECK(jstr(s, "reason") &&
              strstr(jstr(s, "reason"), "not bridged"),
              "6h: with its reason");
        /* The completed duration still exists -- both boundaries were
         * observed -- but it is labelled partial, which is the honest
         * combination. Asserted so a future change cannot quietly drop
         * either half. */
        CHECK(jnum(s, "completed_duration_ms") == 602000.0,
              "6h: the duration is still the observed 602000 ms, labelled "
              "partial -- absence of coverage is not absence of elapsed "
              "time");
        /* NON-VACUITY for this section: the server must actually have found
         * a hole. A fixture whose coverage has no hole would satisfy
         * "partial" through a different reason entirely (the sampled
         * interior), and the section would report a gap guard that never
         * saw a gap. */
        CHECK(jtrue(s, "gap_inside") && !jtrue(s, "malformed"),
              "6h: and the PARTIAL verdict is the coverage hole, not some "
              "other reason");
        CHECK(jnum(s, "records") == 3,
              "6h: three records fell inside the span (two exact, one "
              "sample), so the span really did straddle the hole");
    }
    cJSON_Delete(resp);
    server_destroy(srv);
    free(srv);
}

/* ── 7. BYPASS SUITE: every way this gate can be made blind ───────────── */

static struct pgwt_trace_event *mk_events(int *n)
{
    static struct pgwt_trace_event e[4];
    memset(e, 0, sizeof(e));
    e[0].timestamp_ns = 1000;
    e[0].pid = 42;
    e[0].old_event = e[0].new_event = PGWT_MARKER_CMD_START;
    e[1].timestamp_ns = 2000;
    e[1].pid = 42;
    e[1].old_event = EV_LOCK;
    e[1].duration_ns = 1000;
    e[1].query_id = 7;
    e[2].timestamp_ns = 3000;
    e[2].pid = 42;
    e[2].old_event = e[2].new_event = PGWT_MARKER_CMD_END;
    *n = 3;
    return e;
}

static void test_bypass(const char *dir_a)
{
    printf("--- 7. bypass suite: a pass that cannot see must REFUSE ---\n");

    /* 7a. EMPTY input. Not a failure -- there is nothing to reduce -- but
     *     it must be DISTINGUISHABLE from a measured answer: no spans, no
     *     slots, and every accessor saying "no value" rather than 0. */
    struct pgwt_span_result r;
    pgwt_span_reduce(NULL, 0, NULL, &r);
    CHECK(!r.failed && r.total_spans == 0 && r.num_selected == 0 &&
          r.num_qid_slots == 0,
          "7a: an empty array yields an empty, non-failed result");
    CHECK(pgwt_span_qid_lookup(&r, 7) == NULL &&
          pgwt_span_row_pick(NULL) == NULL,
          "7a: and no lookup invents a slot -- the row reads 'unavailable', "
          "never '0 s'");
    pgwt_span_free(&r);

    /* 7b. UNORDERED input: span boundaries would be invented. REFUSE. */
    int n;
    struct pgwt_trace_event *e = mk_events(&n);
    uint64_t save = e[2].timestamp_ns;
    e[2].timestamp_ns = 500;            /* CMD_END before the record */
    pgwt_span_reduce(e, n, NULL, &r);
    CHECK(r.failed && r.refusal_reason &&
          strstr(r.refusal_reason, "not ordered"),
          "7b: an unordered array is REFUSED with its reason, not reduced");
    CHECK(r.num_selected == 0 && r.qid_slots == NULL,
          "7b: and the refused result carries nothing readable");
    pgwt_span_free(&r);
    e[2].timestamp_ns = save;

    /* 7b-positive: the SAME array in order must produce a span, so 7b's red
     *              is the ordering and not the fixture. */
    pgwt_span_reduce(e, n, NULL, &r);
    CHECK(!r.failed && r.total_spans == 1 && r.n_closed == 1,
          "7b: the same array IN ORDER reduces to one closed span");
    pgwt_span_free(&r);

    /* 7c. ALLOCATION FAILURE at every injection point: refuse, never a
     *     short answer. The point names come from src/backend_span.c. */
    static const char *points[] = { "span_sel", "span_pid_ht", "span_qid_ht",
                                    "span_qid_dense", "span_esc" };
    for (size_t i = 0; i < sizeof(points) / sizeof(points[0]); i++) {
        /* span_esc only fires with an escalation marker present. */
        struct pgwt_trace_event ev[4];
        memcpy(ev, e, sizeof(struct pgwt_trace_event) * (size_t)n);
        int cnt = n;
        if (strcmp(points[i], "span_esc") == 0) {
            memset(&ev[3], 0, sizeof(ev[3]));
            ev[3].timestamp_ns = 4000;
            ev[3].pid = 0;
            ev[3].old_event = ev[3].new_event = PGWT_MARKER_ESCALATE_START;
            cnt = 4;
        }
        setenv("PGWT_TEST_LOAD_ALLOC_FAIL", points[i], 1);
        pgwt_span_reduce(ev, cnt, NULL, &r);
        CHECK(r.failed && r.refusal_reason != NULL,
              "7c: injected allocation failure at '%s' REFUSES with a "
              "reason", points[i]);
        CHECK(r.selected == NULL && r.qid_slots == NULL,
              "7c: and leaves nothing readable behind at '%s'", points[i]);
        pgwt_span_free(&r);
        unsetenv("PGWT_TEST_LOAD_ALLOC_FAIL");
    }
    /* 7c-control: an UNKNOWN injection point must fail nothing, so the
     *             five reds above are the named points and not the env var. */
    setenv("PGWT_TEST_LOAD_ALLOC_FAIL", "span_not_a_point", 1);
    pgwt_span_reduce(e, n, NULL, &r);
    CHECK(!r.failed && r.total_spans == 1,
          "7c: an unknown injection point fails nothing");
    pgwt_span_free(&r);
    unsetenv("PGWT_TEST_LOAD_ALLOC_FAIL");

    /* 7d. THE THING CHECKED IS ABSENT rather than wrong: a response with no
     *     row for the query. The row lookup must return NULL and the oracle
     *     must FAIL on it -- otherwise section 1 would pass on a fixture
     *     that produced nothing at all. */
    const char *first = NULL;
    CHECK(span_oracle(NULL, &ctx_a, 0, &first) > 0,
          "7d: the oracle REJECTS an absent span (it cannot be satisfied "
          "by a response that contains nothing)");
    cJSON *empty = cJSON_CreateObject();
    CHECK(span_oracle(empty, &ctx_a, 0, &first) > 0,
          "7d: and rejects an EMPTY backend_activity object");
    cJSON_Delete(empty);

    /* 7e. UNPARSEABLE / UNRESOLVABLE identity: a span with neither an
     *     observed start nor any evidence has no extent, so containment
     *     must return NULL rather than the nearest span. */
    struct pgwt_span_result fake;
    memset(&fake, 0, sizeof(fake));
    struct pgwt_backend_span one;
    memset(&one, 0, sizeof(one));
    one.pid = 42;
    fake.selected = &one;
    fake.num_selected = 1;
    CHECK(pgwt_span_containing(&fake, 42, 2000) == NULL,
          "7e: a span with no observed extent contains nothing");
    char ident[48];
    CHECK(streq(pgwt_span_identity(&one, ident, sizeof(ident)), "42:?"),
          "7e: and its identity says so ('%s') instead of claiming a "
          "timestamp it never saw", ident);
    uint64_t out = 12345;
    CHECK(pgwt_span_observed_elapsed_ns(&one, &out) == 0 && out == 0,
          "7e: its observed elapsed is reported as ABSENT, and the written "
          "value is 0 only because the caller must not use it");
    CHECK(pgwt_span_completed_duration_ns(&one, &out) == 0,
          "7e: and it has no completed duration");
    fake.failed = 1;
    CHECK(pgwt_span_containing(&fake, 42, 2000) == NULL &&
          pgwt_span_qid_lookup(&fake, 7) == NULL,
          "7e: a FAILED result answers nothing at all");

    /* 7i. THE UNATTRIBUTED SELECTION'S POSITIVE CONTROL. Section 2 asserts
     *     the unattributed drill's EMPTY STATE, and an empty-state assertion
     *     is satisfied by a selector that can never select anything -- the
     *     exact false negative this suite exists for. Here one span holds a
     *     record with NO query id and another holds an attributed one, and
     *     the unattributed scope must return the first and ONLY the first.
     *     A wait with no attributable query id stays reachable; no
     *     association is invented for it. */
    {
        static struct pgwt_trace_event two[6];
        memset(two, 0, sizeof(two));
        /* pid 801: attributed. */
        two[0].timestamp_ns = 1000; two[0].pid = 801;
        two[0].old_event = two[0].new_event = PGWT_MARKER_CMD_START;
        two[1].timestamp_ns = 1100; two[1].pid = 801;
        two[1].old_event = EV_LOCK; two[1].duration_ns = 100;
        two[1].query_id = 4242;
        two[2].timestamp_ns = 1200; two[2].pid = 801;
        two[2].old_event = two[2].new_event = PGWT_MARKER_CMD_END;
        /* pid 802: a wait with NO attributable query id. */
        two[3].timestamp_ns = 2000; two[3].pid = 802;
        two[3].old_event = two[3].new_event = PGWT_MARKER_CMD_START;
        two[4].timestamp_ns = 2500; two[4].pid = 802;
        two[4].old_event = EV_LOCK; two[4].duration_ns = 500;
        two[4].query_id = 0;
        two[4].flags = PGWT_EVENT_FLAG_QUERY_UNATTRIB;
        two[5].timestamp_ns = 2600; two[5].pid = 802;
        two[5].old_event = two[5].new_event = PGWT_MARKER_CMD_END;

        struct pgwt_span_params up = {0};
        up.unattributed_only = 1;
        pgwt_span_reduce(two, 6, &up, &r);
        CHECK(!r.failed && r.total_spans == 2,
              "7i: both spans were produced");
        CHECK(r.scoped_spans == 1 && r.num_selected == 1,
              "7i: the unattributed scope selects exactly ONE of them "
              "(scoped=%d) -- the selector does select", r.scoped_spans);
        CHECK(r.num_selected == 1 && r.selected[0].pid == 802 &&
              r.selected[0].n_qids == 0,
              "7i: and it is the pid whose wait had no attributable query "
              "id, with no association invented for it");
        CHECK(r.num_selected == 1 &&
              r.selected[0].end_ns - r.selected[0].start_ns == 600,
              "7i: carrying its own measured 600 ns duration, so the "
              "unattributed drill is not an empty shell");
        pgwt_span_free(&r);

        /* And the contrast: with no scope, BOTH are selected. */
        pgwt_span_reduce(two, 6, NULL, &r);
        CHECK(!r.failed && r.scoped_spans == 2 && r.num_selected == 2,
              "7i: without the unattributed scope BOTH spans are selected "
              "-- so 7i's '1' is the FILTER and not the fixture");
        pgwt_span_free(&r);
    }

    /* 7h. The row index is searched by BISECTION (pgwt_span_qid_lookup),
     *     which is correct only on an ascending array -- and a miss reads
     *     exactly like "this query has no span", which is the #455 defect
     *     itself. Assert the ORDER and that EVERY slot is findable, so a
     *     future change to the compaction cannot make the row summary
     *     silently disappear for some query ids. */
    {
        enum { NQ = 64 };
        static struct pgwt_trace_event many[NQ * 3];
        memset(many, 0, sizeof(many));
        /* Query ids deliberately NOT in ascending order of appearance, and
         * not in hash order either. */
        for (int i = 0; i < NQ; i++) {
            uint64_t qid = 1 + (uint64_t)((i * 37) % NQ) * 1000ULL;
            uint64_t t = 1000 + (uint64_t)i * 1000;
            many[i * 3 + 0].timestamp_ns = t;
            many[i * 3 + 0].pid = 900 + (uint32_t)i;
            many[i * 3 + 0].old_event = many[i * 3 + 0].new_event =
                PGWT_MARKER_CMD_START;
            many[i * 3 + 1].timestamp_ns = t + 100;
            many[i * 3 + 1].pid = 900 + (uint32_t)i;
            many[i * 3 + 1].old_event = EV_LOCK;
            many[i * 3 + 1].duration_ns = 100;
            many[i * 3 + 1].query_id = qid;
            many[i * 3 + 2].timestamp_ns = t + 200;
            many[i * 3 + 2].pid = 900 + (uint32_t)i;
            many[i * 3 + 2].old_event = many[i * 3 + 2].new_event =
                PGWT_MARKER_CMD_END;
        }
        pgwt_span_reduce(many, NQ * 3, NULL, &r);
        CHECK(!r.failed && r.num_qid_slots == NQ,
              "7h: %d distinct query ids produce %d index slots",
              (int)NQ, r.num_qid_slots);
        int sorted = 1;
        for (int i = 1; i < r.num_qid_slots; i++)
            if (r.qid_slots[i - 1].query_id >= r.qid_slots[i].query_id)
                sorted = 0;
        CHECK(sorted, "7h: the index is strictly ascending by query id -- "
              "the precondition the bisection depends on");
        int findable = 0;
        for (int i = 0; i < NQ; i++) {
            uint64_t qid = 1 + (uint64_t)((i * 37) % NQ) * 1000ULL;
            const struct pgwt_span_qid_slot *sl =
                pgwt_span_qid_lookup(&r, qid);
            if (sl && sl->query_id == qid && pgwt_span_row_pick(sl))
                findable++;
        }
        CHECK(findable == NQ,
              "7h: all %d are findable by bisection (%d found) -- a miss "
              "would read as 'this query has no span', which IS the defect",
              (int)NQ, findable);
        CHECK(pgwt_span_qid_lookup(&r, 999999999ULL) == NULL,
              "7h: and a query id the window never carried is NOT found -- "
              "the lookup does not return a neighbour");
        pgwt_span_free(&r);
    }

    /* 7f. A REFUSAL REACHES THE WIRE. The handler must not quietly serve a
     *     row with no backend activity when the pass refused -- the reason
     *     has to be there, or a blind gate reads as a clean one. */
    struct pgwt_server *srv = calloc(1, sizeof(*srv));
    if (!srv || server_init(srv, dir_a) != 0) {
        printf("  FATAL: server_init (bypass)\n"); exit(2);
    }
    setenv("PGWT_TEST_LOAD_ALLOC_FAIL", "span_sel", 1);
    struct pgwt_request req = {0};
    req.id = 9;
    cJSON *resp = call_handler(handle_top_queries, srv, &req);
    unsetenv("PGWT_TEST_LOAD_ALLOC_FAIL");
    CHECK(resp && !jtrue(resp, "backend_activity_available"),
          "7f: top_queries reports the span pass UNAVAILABLE");
    CHECK(resp && jstr(resp, "backend_activity_unavailable_reason") != NULL,
          "7f: with the refusal's reason at the top level");
    cJSON *row = resp ? qrow_by_id(cJSON_GetObjectItem(resp, "rows"), QID)
                      : NULL;
    CHECK(row && is_json_null(row, "backend_activity") &&
          jstr(row, "backend_activity_unavailable_reason") != NULL,
          "7f: and EVERY ROW says unavailable-with-a-reason rather than "
          "silently omitting the column");
    CHECK(row && jnum(row, "total_ms") > 0,
          "7f: while DB Time and the rest of the Queries answer survive -- "
          "the span pass is additive, so its refusal is not a blank tab");
    cJSON_Delete(resp);

    setenv("PGWT_TEST_LOAD_ALLOC_FAIL", "span_sel", 1);
    memset(&req, 0, sizeof(req));
    req.id = 10;
    req.filter.pid = PID_BLOCKED;
    resp = call_handler(handle_span_detail, srv, &req);
    unsetenv("PGWT_TEST_LOAD_ALLOC_FAIL");
    CHECK(resp && !jtrue(resp, "available") &&
          jstr(resp, "unavailable_reason") != NULL &&
          is_json_null(resp, "spans"),
          "7f: span_detail refuses with a reason and a NULL span list -- "
          "never an empty list that reads as 'nothing was happening'");
    cJSON_Delete(resp);

    /* 7g. execution_detail must carry the observed containing span. A
     *     refusal or an error response cannot skip these assertions. */
    memset(&req, 0, sizeof(req));
    req.id = 11;
    req.filter.pid = PID_FAST;
    req.start_ns = WALL(fx_base) + 10 * US;
    req.end_ns = WALL(fx_base) + 50 * US;
    resp = call_handler(handle_execution_detail, srv, &req);
    CHECK(resp != NULL, "7g: execution_detail answers");
    cJSON *s7 = resp ? cJSON_GetObjectItem(resp, "backend_activity_span") : NULL;
    int have_span = (s7 != NULL && cJSON_IsObject(s7));
    CHECK(have_span,
          "7g: the containing span is a NON-NULL object -- a JSON null with a "
          "reason, or an error response with no such key at all, is exactly what "
          "#479 produced on every live execution, and this assertion used to "
          "accept both");
    if (resp && !(s7 && cJSON_IsObject(s7)))
        printf("     (reason: %s)\n",
               jstr(resp, "backend_activity_unavailable_reason")
                   ? jstr(resp, "backend_activity_unavailable_reason")
                   : "(none)");
    CHECK(have_span && (uint32_t)jnum(s7, "pid") == PID_FAST,
          "7g: the containing span is the LEADER's own");
    CHECK(have_span && jnum(s7, "completed_duration_ms") == 1.0,
          "7g: and its duration is the 1 ms command, measured from "
          "the CMD markers around this execution -- BOTH of which "
          "lie outside the clicked execution's own window, so the "
          "pre- and post-window marker passes both had to work");
    CHECK(have_span && streq(jstr(s7, "measurement"), "measured") &&
          streq(jstr(s7, "completion"), "closed"),
          "7g: measured and closed -- not 'open at the end of the "
          "processed stream', which is what a request-window-only "
          "reconstruction reports for every completed statement");
    CHECK(jstr(resp, "backend_activity_span_scope") != NULL,
          "7g: and the response states how the span was scoped");
    cJSON_Delete(resp);
    server_destroy(srv);
    free(srv);
}

/* ── fixture S: #479's window/post seam ───────────────────────────────── */

#define PID_SORD   801          /* the clicked execution's leader */
#define PID_SIDLE  802          /* holds a ClientRead across the right edge */
#define QID_SORD   0x7777777777777777ULL

static uint64_t fx_s_base;      /* fixture S origin, MONO */
static uint64_t fx_s_wall_off;  /* MONO -> WALL, calibrated like fx_wall_off */
#define SWALL(mono) ((mono) + fx_s_wall_off)

static void build_fixture_straddle(const char *dir)
{
    struct pgwt_event_writer w;
    if (pgwt_writer_init(&w, dir, 180000, 0, NULL) != 0) {
        printf("  FATAL: writer init (s)\n"); exit(2);
    }
    uint64_t t0 = fx_s_base;
    wmark(&w, t0, PID_SORD, PGWT_MARKER_CMD_START, 0);
    wmark(&w, t0 + 1 * MS, PID_SORD, PGWT_MARKER_EXEC_START, QID_SORD);
    wpush(&w, t0 + 5 * MS, PID_SORD, EV_IO, 0, 2 * MS, QID_SORD);
    wmark(&w, t0 + 9 * MS, PID_SORD, PGWT_MARKER_EXEC_END, QID_SORD);
    wmark(&w, t0 + 10 * MS, PID_SORD, PGWT_MARKER_CMD_END, QID_SORD);
    /* The straddler: a ClientRead on ANOTHER pid that opens one second
     * before this command and closes thirty seconds after it -- the exact
     * shape an idle session has in every real capture. */
    wpush(&w, t0 + 30 * ONE_SEC, PID_SIDLE, EV_CLIENT_READ, 0,
          31 * ONE_SEC, 0);
    pgwt_writer_close(&w);
}

/* Reproduce the handler's three loads and concatenate WITHOUT sorting, so
 * the refusal and its bypass exercise the same seam as the real request. */
static struct pgwt_trace_event *straddle_concat(struct pgwt_server *srv,
                                                struct pgwt_request *req,
                                                int counts[3], int *count)
{
    struct pgwt_load_info info[3] = {{0}};
    uint64_t context_from = req->start_ns > EXEC_PLAN_LOOKBACK_NS
                          ? req->start_ns - EXEC_PLAN_LOOKBACK_NS : 0;
    struct pgwt_trace_event *chunks[3];
    chunks[0] = server_load_markers_fi(srv, context_from, req->start_ns - 1,
                                      PID_SORD, &counts[0], &info[0]);
    chunks[1] = server_load_events_fi(srv, req->start_ns, req->end_ns, 0,
                                     &counts[1], &info[1]);
    chunks[2] = server_load_markers_fi(srv, req->end_ns + 1,
                                      req->end_ns + EXEC_PLAN_LOOKBACK_NS,
                                      PID_SORD, &counts[2], &info[2]);
    *count = counts[0] + counts[1] + counts[2];
    struct pgwt_trace_event *events =
        malloc((size_t)(*count > 0 ? *count : 1) * sizeof(*events));
    if (!events) { printf("  FATAL: concat allocation (s)\n"); exit(2); }
    int at = 0;
    for (int i = 0; i < 3; i++) {
        if (counts[i] > 0)
            memcpy(events + at, chunks[i], (size_t)counts[i] * sizeof(*events));
        at += counts[i];
        free(chunks[i]);
    }
    return events;
}

static int first_inversion(const struct pgwt_trace_event *events, int count)
{
    for (int i = 1; i < count; i++)
        if (events[i - 1].timestamp_ns > events[i].timestamp_ns)
            return i;
    return -1;
}

static void test_straddler_order(struct pgwt_server *srv_s)
{
    printf("--- 9. #479: straddlers must not kill the containing span ---\n");
    fx_s_wall_off = srv_s->earliest_wall_ns - fx_s_base;
    uint64_t t0 = fx_s_base;
    struct pgwt_request req = {0};
    req.id = 479;
    req.filter.pid = PID_SORD;
    req.start_ns = SWALL(t0 + 1 * MS);
    req.end_ns = SWALL(t0 + 9 * MS);

    /* 9a. The hazard is PRESENT, not assumed. The real loader retains the
     * straddler's own late closing timestamp in the execution window. */
    int wn = 0;
    struct pgwt_load_info wi = {0};
    struct pgwt_trace_event *we = server_load_events_fi(
        srv_s, req.start_ns, req.end_ns, 0, &wn, &wi);
    CHECK(we != NULL && wn > 0,
          "9a: the real window load has records -- an empty fixture would "
          "exercise no ordering hazard");
    CHECK(wi.straddlers_admitted >= 1,
          "9a: the window load ADMITS a right-edge straddler, so the condition "
          "#479 is about is present in this fixture rather than assumed; "
          "with the lookahead off (9d) this is 0 and section 9 would be "
          "testing nothing");
    int late = 0;
    uint64_t latest = req.end_ns;
    for (int i = 0; we && i < wn; i++) {
        if (we[i].timestamp_ns > req.end_ns) {
            late++;
            if (we[i].timestamp_ns > latest) latest = we[i].timestamp_ns;
        }
    }
    CHECK(late >= 1,
          "9a: the window carries a closing timestamp past its end -- "
          "without that late timestamp the seam cannot invert");
    printf("     (%d records past the window; latest %.2f ms past its end)\n",
           late, (double)(latest - req.end_ns) / 1e6);
    free(we);

    /* 9b. The unsorted concatenation must refuse for EXACTLY #479's reason,
     * before pid selection can hide the other backend's late timestamp. */
    int counts[3] = {0};
    int count = 0;
    struct pgwt_trace_event *events = straddle_concat(srv_s, &req, counts, &count);
    CHECK(counts[0] > 0 && counts[1] > 0 && counts[2] > 0,
          "9b: all three chunks contributed: a zero-length post chunk would "
          "make the seam monotone and this section would prove nothing");
    int inversion = first_inversion(events, count);
    CHECK(inversion >= 0,
          "9b: the concatenation really is out of order at the window/post "
          "seam -- a monotone fixture would never reproduce #479");
    if (inversion >= 0)
        printf("     (first inversion at %d: %llu > %llu)\n", inversion,
               (unsigned long long)events[inversion - 1].timestamp_ns,
               (unsigned long long)events[inversion].timestamp_ns);
    struct pgwt_span_params sp = {0};
    sp.window_from_ns = req.start_ns;
    sp.window_to_ns = req.end_ns;
    sp.pid = req.filter.pid;
    sp.max_selected = 16;
    struct pgwt_span_result res;
    pgwt_span_reduce(events, count, &sp, &res);
    CHECK(res.failed && streq(res.refusal_reason,
          "the event array is not ordered by timestamp: span boundaries "
          "would be invented rather than observed"),
          "9b: the unsorted concatenation refuses for EXACTLY #479's "
          "ordering reason -- a different refusal would not pin this bug");
    pgwt_span_free(&res);
    free(events);

    /* 9c. Every assertion runs even on an error or a null span. Observed
     * command markers, outside the execution window, supply both bounds. */
    cJSON *resp = call_handler(handle_execution_detail, srv_s, &req);
    CHECK(resp != NULL && cJSON_GetObjectItem(resp, "error") == NULL,
          "9c: execution_detail answers without an error -- an error response "
          "cannot stand in for the containing span");
    if (resp && cJSON_GetObjectItem(resp, "error"))
        printf("     (error: %s)\n", jstr(resp, "error"));
    cJSON *s = resp ? cJSON_GetObjectItem(resp, "backend_activity_span") : NULL;
    int have_span9 = (s != NULL && cJSON_IsObject(s));
    CHECK(have_span9,
          "9c: this is the assertion #479 makes red: before the fix the "
          "handler fed pgwt_span_reduce the unordered array from 9b and "
          "every live execution came back null-with-a-reason");
    if (resp && !(s && cJSON_IsObject(s)))
        printf("     (reason: %s)\n",
               jstr(resp, "backend_activity_unavailable_reason")
                   ? jstr(resp, "backend_activity_unavailable_reason") : "(none)");
    CHECK(have_span9 && (uint32_t)jnum(s, "pid") == PID_SORD,
          "9c: the containing span belongs to the clicked leader -- the "
          "other pid's straddler cannot supply its identity");
    char want_start[32], want_end[32], window_end[32];
    snprintf(want_start, sizeof(want_start), "%llu",
             (unsigned long long)SWALL(t0));
    snprintf(want_end, sizeof(want_end), "%llu",
             (unsigned long long)SWALL(t0 + 10 * MS));
    snprintf(window_end, sizeof(window_end), "%llu",
             (unsigned long long)req.end_ns);
    CHECK(have_span9 && streq(jstr(s, "start_ns"), want_start),
          "9c: start_ns is the observed CMD_START -- this also pins the "
          "mono-to-wall calibration, so a wrong anchor cannot pass");
    CHECK(have_span9 && streq(jstr(s, "end_ns"), want_end),
          "9c: end_ns is the observed CMD_END -- the other pid's straddler "
          "must never become this command's boundary");
    CHECK(have_span9 && jnum(s, "completed_duration_ms") == 10.0 &&
          streq(jstr(s, "duration_source"), "timestamps"),
          "9c: the command lasts exactly 10 ms from timestamps -- the "
          "execution window or a late straddler cannot supply its duration");
    CHECK(have_span9 && streq(jstr(s, "measurement"), "measured") &&
          streq(jstr(s, "completion"), "closed"),
          "9c: the containing span is measured and closed -- losing either "
          "context marker would misreport this completed command");
    CHECK(have_span9 && jstr(s, "end_ns") != NULL &&
          !streq(jstr(s, "end_ns"), window_end),
          "9c: end_ns is not the execution window end -- a request edge "
          "must never substitute for an observed command boundary");
    CHECK(cJSON_IsObject(cJSON_GetObjectItem(resp, "leader")) &&
          jnum(resp, "kept_count") > 0,
          "9c: the leader lane and kept records remain present -- the span "
          "is additive and must not collapse the rest of the answer");
    cJSON_Delete(resp);

    /* 9d. Removing only the straddler makes the original concatenation
     * valid. Restore and prove the lookahead so later work cannot go blind. */
    setenv("PGWT_STRADDLE_LOOKAHEAD_NS", "0", 1);
    wn = 0;
    memset(&wi, 0, sizeof(wi));
    we = server_load_events_fi(srv_s, req.start_ns, req.end_ns, 0, &wn, &wi);
    late = 0;
    for (int i = 0; we && i < wn; i++)
        if (we[i].timestamp_ns > req.end_ns) late++;
    CHECK(we != NULL && wn > 0 && wi.straddlers_admitted == 0 && late == 0,
          "9d: disabling lookahead removes both admission and late records "
          "from a nonempty window -- otherwise the bypass removes no hazard");
    free(we);
    events = straddle_concat(srv_s, &req, counts, &count);
    pgwt_span_reduce(events, count, &sp, &res);
    CHECK(counts[0] > 0 && counts[1] > 0 && counts[2] > 0 &&
          first_inversion(events, count) < 0 && !res.failed,
          "9d: all three chunks are ascending and reduce without failure, "
          "so 9b's refusal is caused by the admitted straddler and by "
          "nothing else in this fixture");
    pgwt_span_free(&res);
    free(events);
    resp = call_handler(handle_execution_detail, srv_s, &req);
    s = resp ? cJSON_GetObjectItem(resp, "backend_activity_span") : NULL;
    CHECK(cJSON_IsObject(s) && (uint32_t)jnum(s, "pid") == PID_SORD &&
          streq(jstr(s, "start_ns"), want_start) &&
          streq(jstr(s, "end_ns"), want_end) &&
          jnum(s, "completed_duration_ms") == 10.0,
          "9d: without the straddler the handler still returns the same "
          "non-null 10 ms span -- the fix must preserve pre-#479 behaviour");
    cJSON_Delete(resp);
    unsetenv("PGWT_STRADDLE_LOOKAHEAD_NS");
    wn = 0;
    memset(&wi, 0, sizeof(wi));
    we = server_load_events_fi(srv_s, req.start_ns, req.end_ns, 0, &wn, &wi);
    CHECK(wi.straddlers_admitted >= 1,
          "9d: restoring lookahead admits the straddler again -- a leaked "
          "environment variable must not silently disable the later hazard");
    free(we);

    /* 9e. This injection belongs to the span sort, not the loader's sorts:
     * a loader refusal would blank the tab and test the wrong call site. */
    setenv("PGWT_TEST_LOAD_ALLOC_FAIL", "span_sort", 1);
    resp = call_handler(handle_execution_detail, srv_s, &req);
    unsetenv("PGWT_TEST_LOAD_ALLOC_FAIL");
    CHECK(is_json_null(resp, "backend_activity_span") &&
          streq(jstr(resp, "backend_activity_unavailable_reason"),
                "the span context could not be sorted into timestamp order"),
          "9e: the ordering failure returns a null span with its EXACT "
          "span-sort reason -- a loader refusal or some other reason "
          "would test the wrong failure");
    CHECK(resp != NULL && cJSON_GetObjectItem(resp, "error") == NULL &&
          cJSON_IsObject(cJSON_GetObjectItem(resp, "leader")) &&
          jnum(resp, "kept_count") > 0,
          "9e: refusing an unordered span keeps the leader and records "
          "without an error -- an additive refusal must not blank the tab");
    cJSON_Delete(resp);

    /* 9f. Three ascending runs at scale, with 64 late window records
     * outranking the post markers. Allocate once, sort once, never gate on
     * a shared runner's clock. Hooks identify the path that actually ran. */
    const int pre = 64, window = 1000000, post = 64;
    int total = pre + window + post;
    events = calloc((size_t)total, sizeof(*events));
    if (!events) {
        printf("  skip SORT COST: cannot allocate %d events\n", total);
        return;
    }
    for (int i = 0; i < pre; i++) {
        events[i].timestamp_ns = (uint64_t)i * US;
        events[i].pid = PID_SORD;
        events[i].old_event = events[i].new_event = PGWT_MARKER_CMD_START;
    }
    for (int i = 0; i < window; i++) {
        struct pgwt_trace_event *e = &events[pre + i];
        e->timestamp_ns = (uint64_t)(pre + i) * US;
        e->pid = i >= window - 64 ? PID_SIDLE : PID_SORD;
        e->old_event = EV_IO;
        if (i >= window - 64) e->timestamp_ns += 30 * ONE_SEC;
    }
    for (int i = 0; i < post; i++) {
        struct pgwt_trace_event *e = &events[pre + window + i];
        e->timestamp_ns = (uint64_t)(pre + window + i) * US;
        e->pid = PID_SORD;
        e->old_event = e->new_event = PGWT_MARKER_CMD_END;
    }
    struct pgwt_event_sort_hooks hooks = {0};
    struct timespec a, b;
    clock_gettime(CLOCK_MONOTONIC, &a);
    int sort_rc = pgwt_stable_sort_events(events, total, &hooks);
    clock_gettime(CLOCK_MONOTONIC, &b);
    double ms = (double)(b.tv_sec - a.tv_sec) * 1e3 +
                (double)(b.tv_nsec - a.tv_nsec) / 1e6;
    printf("  SORT COST: %d events, %.2f ms, moves=%ld, fell_back=%d\n",
           total, ms, hooks.moves, hooks.fell_back);
    printf("  (printed, never asserted: a wall-clock gate on a shared runner is "
           "runner noise)\n");
    if (sort_rc != 0) printf("  skip SORT COST result: sort allocation failed\n");
    free(events);
}

/* ── bench: the reducer against a 900-second capture ──────────────────── */

static void bench_900s(void)
{
    printf("--- bench: reducer over a synthetic 900 s capture ---\n");
    /* 900 s at 1000 commands/s, 5 records per command: 4.5M records. The
     * same shape handle_top_queries walks. Allocated once, in memory --
     * this measures THE REDUCER, which is the thing this branch adds to
     * the paint path. */
    int per_cmd = 5;
    int n_cmd = 900 * 1000;
    long total = (long)n_cmd * per_cmd;
    struct pgwt_trace_event *ev = calloc((size_t)total, sizeof(*ev));
    if (!ev) { printf("  skip: cannot allocate %ld events\n", total); return; }
    long k = 0;
    for (int i = 0; i < n_cmd; i++) {
        uint64_t t = (uint64_t)i * ONE_SEC / 1000;
        uint32_t pid = 1000 + (uint32_t)(i % 64);
        uint64_t qid = 1 + (uint64_t)(i % 40);
        ev[k].timestamp_ns = t;       ev[k].pid = pid;
        ev[k].old_event = ev[k].new_event = PGWT_MARKER_CMD_START; k++;
        ev[k].timestamp_ns = t + 1000; ev[k].pid = pid;
        ev[k].old_event = ev[k].new_event = PGWT_MARKER_EXEC_START;
        ev[k].query_id = qid; k++;
        ev[k].timestamp_ns = t + 20000; ev[k].pid = pid;
        ev[k].old_event = EV_IO; ev[k].duration_ns = 19000;
        ev[k].query_id = qid; k++;
        ev[k].timestamp_ns = t + 30000; ev[k].pid = pid;
        ev[k].old_event = ev[k].new_event = PGWT_MARKER_EXEC_END;
        ev[k].query_id = qid; k++;
        ev[k].timestamp_ns = t + 40000; ev[k].pid = pid;
        ev[k].old_event = ev[k].new_event = PGWT_MARKER_CMD_END;
        ev[k].query_id = qid; k++;
    }
    struct timespec a, b;
    struct pgwt_span_result r;
    clock_gettime(CLOCK_MONOTONIC, &a);
    pgwt_span_reduce(ev, (int)k, NULL, &r);
    clock_gettime(CLOCK_MONOTONIC, &b);
    double ms = (double)(b.tv_sec - a.tv_sec) * 1e3 +
                (double)(b.tv_nsec - a.tv_nsec) / 1e6;
    printf("  BENCH span_reduce: %ld events, %d spans, %d qid slots, "
           "%.1f ms (%.3f us/event)\n",
           k, r.total_spans, r.num_qid_slots, ms, ms * 1000.0 / (double)k);
    printf("  (printed, never asserted: a wall-clock gate on a shared "
           "runner is runner noise)\n");
    pgwt_span_free(&r);
    free(ev);
}

/* ── main ─────────────────────────────────────────────────────────────── */

int main(int argc, char **argv)
{
    printf("=== test_backend_span ===\n");

    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    fx_base = ((uint64_t)ts.tv_sec * ONE_SEC + (uint64_t)ts.tv_nsec);
    fx_base = (fx_base / ONE_SEC) * ONE_SEC;

    if (argc > 1 && strcmp(argv[1], "bench") == 0) {
        bench_900s();
        return 0;
    }

    char dir_a[] = "/tmp/pgwt_span_a_XXXXXX";
    if (!mkdtemp(dir_a)) { printf("FATAL: mkdtemp a\n"); return 2; }
    build_fixture_a(dir_a, fx_base);
    struct pgwt_server *srv = calloc(1, sizeof(*srv));
    if (!srv || server_init(srv, dir_a) != 0) {
        printf("FATAL: server_init a\n"); return 2;
    }
    test_455_gate(srv);
    test_drill(srv);
    test_prefix_identity(srv);
    test_mutations();
    server_destroy(srv);
    free(srv);

    char dir_n[] = "/tmp/pgwt_span_n_XXXXXX";
    if (!mkdtemp(dir_n)) { printf("FATAL: mkdtemp n\n"); return 2; }
    build_fixture_nocmd(dir_n, fx_base);
    test_no_cmd_markers(dir_n);

    char dir_e[] = "/tmp/pgwt_span_e_XXXXXX";
    if (!mkdtemp(dir_e)) { printf("FATAL: mkdtemp e\n"); return 2; }
    fx_e_base = fx_base;
    build_fixture_edges(dir_e, fx_e_base);
    struct pgwt_server *srv_e = calloc(1, sizeof(*srv_e));
    if (!srv_e || server_init(srv_e, dir_e) != 0) {
        printf("FATAL: server_init e\n"); return 2;
    }
    test_edges(srv_e);
    cJSON_Delete(edges_resp);
    server_destroy(srv_e);
    free(srv_e);

    char dir_g[] = "/tmp/pgwt_span_g_XXXXXX";
    if (!mkdtemp(dir_g)) { printf("FATAL: mkdtemp g\n"); return 2; }
    build_fixture_gap(dir_g, fx_base);
    test_gap(dir_g);

    test_bypass(dir_a);

    char dir_s[] = "/tmp/pgwt_span_s_XXXXXX";
    if (!mkdtemp(dir_s)) { printf("FATAL: mkdtemp s\n"); return 2; }
    fx_s_base = fx_base + 2 * ONE_SEC;
    build_fixture_straddle(dir_s);
    struct pgwt_server *srv_s = calloc(1, sizeof(*srv_s));
    if (!srv_s || server_init(srv_s, dir_s) != 0) {
        printf("FATAL: server_init s\n"); return 2;
    }
    test_straddler_order(srv_s);
    server_destroy(srv_s);
    free(srv_s);

    if (resp_a_row) cJSON_Delete(resp_a_row);

    /* NON-VACUITY LEDGER. The floor moves with the suite's measured check
     * count, with no slack. A run that executed fewer assertions did not
     * test what it claims: a whole section stopping unnoticed is the same
     * false negative as a gate that cannot fail. */
    if (checks < 155) {
        printf("FAIL: only %d assertions ran -- a section did not execute\n",
               checks);
        return 1;
    }
    printf("\n%d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
