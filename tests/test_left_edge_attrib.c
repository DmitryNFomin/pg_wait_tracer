/* test_left_edge_attrib.c — #586: a record whose id-carrying marker sits LEFT
 * of the loaded window loses its query attribution entirely.
 *
 * THE DEFECT. #128's back-fill (src/query_attr.h, mirrored by
 * pgwt_tag_events in src/compute.c) resolves a foreground record that closed
 * with query_id 0 by looking at the pid's OTHER records and markers:
 *   rule 1 — the first id reported LATER in the same command;
 *   rule 2 — else the last id reported EARLIER in the same command (cmd_qid);
 *   rule 3 — else the explicit UNATTRIBUTED bucket.
 * Rule 2's evidence is `cmd_qid`, and cmd_qid is built by the forward pass
 * over the LOADED ARRAY only. The raw loader admits records by
 * `ts_mono >= from_mono`, so for a command that was already running when the
 * window opened, the EXEC_START/PLAN_START that carries the id is outside the
 * load. The record arrives (its close is inside the window, so it is a real
 * occurrence of this window) with nothing to attribute it to.
 *
 * The user-visible shape, from the capture that filed the issue: one
 * Lock:relation of 2435.069 ms attributed to no query, so summing the 22
 * per-query drill-downs gave 183 records where the unfiltered view showed
 * 184. A per-query filter cannot reach the unattributed bucket
 * (pgwt_filter.query_id == 0 means "no filter"), so the drill-down loses the
 * record outright rather than mis-filing it.
 *
 * THE MIRROR OF #424. #424 added a bounded RIGHT-edge straddler lookahead:
 * records live in blocks after the window, so admission had to look past
 * `to`. This is the same problem on the other side, with one difference that
 * decides the whole design: the left-straddling RECORD already arrives. What
 * is missing is its command's reported ID. So the fix looks back for a
 * MARKER, seeds rule 2's cmd_qid with it, and adds NOTHING to the array.
 *
 * That is why sections 3 and 4 exist and must not be skipped. A lookahead
 * that admitted the left-straddling record a second time would double-count
 * every statistic (count 185, not 184), and one that widened the window
 * would credit the straddler's whole 7 s instead of its in-window 2.5 s and
 * inflate DB Time everywhere at once. Both are pinned as numbers, not as
 * prose.
 *
 * BYPASS SUITE (section 6): every way the seed lookback could stop SEEING
 * rather than stop finding — horizon disabled, horizon shorter than the
 * distance to the marker, the seed load refused by allocation failure, an
 * unparseable horizon, a window with no left straddler at all, and a command
 * that genuinely reported no id anywhere. In each case the loader must either
 * resolve the seed or SAY it could not look far enough
 * (pgwt_load_info.left_edge_truncated). A gate that cannot see must refuse,
 * never approve.
 *
 * Section 5 is the hazard the seed itself creates: a CMD_START before `from`
 * means the command running at the left edge has reported NO id yet, so the
 * previous statement's id must not leak into it. A seed that ignored
 * CMD_START would turn a lost record into a wrongly attributed one, which is
 * worse.
 *
 * WHAT THIS FILE CANNOT REACH: it drives the real writer, the real loader and
 * the real compute in process; it does not go through the WebSocket protocol,
 * and it says nothing about the summary path (every number here is raw).
 */
#define main pgwt_server_main
#include "../src/server.c"
#undef main

#include <sys/wait.h>
#include <unistd.h>

static int tests_run = 0;
static int tests_failed = 0;

#define CHECK(cond, fmt, ...) do {                                     \
    tests_run++;                                                       \
    if (cond) { printf("  ok: " fmt "\n", ##__VA_ARGS__); }            \
    else { tests_failed++;                                             \
           printf("  FAIL(line %d): " fmt "\n", __LINE__, ##__VA_ARGS__); } \
} while (0)

#define MS      1000000ULL
#define ONE_SEC 1000000000ULL

#define EV_LOCK WEI(PG_WAIT_LOCK, 0)
#define EV_IO   WEI(PG_WAIT_IO, 21)

#define FILLER_PID    777
#define STRADDLE_PID  601
#define INSIDE_PID    602
#define STALE_PID     603

#define QID_F 0x5000000000000001ULL   /* the filler's statement */
#define QID_A 0x5000000000000002ULL   /* the left-straddling command */
#define QID_B 0x5000000000000003ULL   /* a command wholly inside the window */
#define QID_C 0x5000000000000004ULL   /* the PREVIOUS command (section 5) */

#define FIXTURE_SECS 60

/* The window every section uses, as offsets from the fixture base. */
#define WIN_FROM (10 * ONE_SEC)
#define WIN_TO   (20 * ONE_SEC)

/* The straddler: Lock [5.5, 12.5] — 7 s long, closing 2.5 s inside the
 * window. Its EXEC_START is at 4.1 s, 5.9 s left of `from`. */
#define STRADDLE_CLOSE (12 * ONE_SEC + 500 * MS)
#define STRADDLE_DUR   (7 * ONE_SEC)
#define STRADDLE_IN_MS 2500.0          /* (12.5 - 10) s, clipped */
#define INSIDE_CLOSE   (15 * ONE_SEC)
#define INSIDE_DUR     (500 * MS)
#define INSIDE_IN_MS   500.0

static void push_q(struct pgwt_event_writer *w, uint64_t ts, uint32_t pid,
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
    if (pgwt_writer_push_event(w, &e) != 0) {
        printf("  FAIL: writer refused a record\n");
        tests_failed++;
    }
}

static void mark(struct pgwt_event_writer *w, uint64_t ts, uint32_t pid,
                 uint32_t marker, uint64_t qid)
{
    push_q(w, ts, pid, marker, marker, 0, qid);
}

/* Continuous filler coverage, every record ATTRIBUTED (query_id QID_F), so
 * the per-query sum can equal the unfiltered count at all. An unattributed
 * filler would make the headline identity unreachable for reasons that have
 * nothing to do with #586. */
static void push_filler(struct pgwt_event_writer *w, uint64_t base)
{
    for (int s = 0; s < FIXTURE_SECS; s++)
        push_q(w, base + (uint64_t)s * ONE_SEC + 10 * MS, FILLER_PID,
               0, EV_IO, 1 * MS, QID_F);
}

/* `variant` selects which property of the fixture is under test:
 *   0 MAIN      — the defect: EXEC_START at 4.1 s, Lock [5.5, 12.5]
 *   1 NOSTRADDLE— the same command, entirely INSIDE the window (absence case)
 *   2 NOID      — a left-straddling command that never reported an id
 */
enum fixture_variant { FX_MAIN = 0, FX_NOSTRADDLE = 1, FX_NOID = 2 };

static void build_fixture(const char *dir, uint64_t base, int variant)
{
    struct pgwt_event_writer writer;
    if (pgwt_writer_init(&writer, dir, 180000, 0, NULL) != 0) {
        printf("  FAIL: trace writer init (%s)\n", dir);
        tests_failed++;
        return;
    }
    push_filler(&writer, base);

    if (variant == FX_NOSTRADDLE) {
        /* Same command, same record, both wholly inside [10, 20]: the
         * marker is IN the load, so rule 2 has its evidence without any
         * lookback. Nothing for the seed scan to do. */
        mark(&writer, base + 11 * ONE_SEC, STRADDLE_PID,
             PGWT_MARKER_CMD_START, 0);
        mark(&writer, base + 11 * ONE_SEC + 100 * MS, STRADDLE_PID,
             PGWT_MARKER_EXEC_START, QID_A);
        push_q(&writer, base + STRADDLE_CLOSE, STRADDLE_PID,
               EV_LOCK, EV_IO, 1 * ONE_SEC, 0);
        mark(&writer, base + STRADDLE_CLOSE + 100 * MS, STRADDLE_PID,
             PGWT_MARKER_EXEC_END, 0);
        mark(&writer, base + STRADDLE_CLOSE + 200 * MS, STRADDLE_PID,
             PGWT_MARKER_CMD_END, 0);
    } else {
        mark(&writer, base + 4 * ONE_SEC, STRADDLE_PID,
             PGWT_MARKER_CMD_START, 0);
        if (variant != FX_NOID)
            mark(&writer, base + 4 * ONE_SEC + 100 * MS, STRADDLE_PID,
                 PGWT_MARKER_EXEC_START, QID_A);
        push_q(&writer, base + STRADDLE_CLOSE, STRADDLE_PID,
               EV_LOCK, EV_IO, STRADDLE_DUR, 0);
        mark(&writer, base + STRADDLE_CLOSE + 100 * MS, STRADDLE_PID,
             PGWT_MARKER_EXEC_END, 0);
        mark(&writer, base + STRADDLE_CLOSE + 200 * MS, STRADDLE_PID,
             PGWT_MARKER_CMD_END, 0);
    }

    /* The control: a command that opens, reports its id and closes entirely
     * inside the window. Rule 2 has always worked for it. */
    mark(&writer, base + 13 * ONE_SEC, INSIDE_PID, PGWT_MARKER_CMD_START, 0);
    mark(&writer, base + 13 * ONE_SEC + 100 * MS, INSIDE_PID,
         PGWT_MARKER_EXEC_START, QID_B);
    push_q(&writer, base + INSIDE_CLOSE, INSIDE_PID,
           EV_LOCK, EV_IO, INSIDE_DUR, 0);
    mark(&writer, base + INSIDE_CLOSE + 100 * MS, INSIDE_PID,
         PGWT_MARKER_EXEC_END, 0);
    mark(&writer, base + INSIDE_CLOSE + 200 * MS, INSIDE_PID,
         PGWT_MARKER_CMD_END, 0);

    pgwt_writer_close(&writer);
}

/* Section 5's fixture: the pid runs ONE command that reports QID_C and
 * closes, then opens a SECOND command at 9 s — before `from` — which has
 * reported no id by the time its Lock [8, 12] closes inside the window.
 * (The second CMD_START at 9 s is after the Lock's start at 8 s on purpose:
 * the stale id is reachable in the lookback either way, and this is the
 * ordering that makes "the record started before its own command" explicit.)
 */
static void build_stale_fixture(const char *dir, uint64_t base)
{
    struct pgwt_event_writer writer;
    if (pgwt_writer_init(&writer, dir, 180000, 0, NULL) != 0) {
        printf("  FAIL: trace writer init (stale)\n");
        tests_failed++;
        return;
    }
    push_filler(&writer, base);
    mark(&writer, base + 3 * ONE_SEC, STALE_PID, PGWT_MARKER_CMD_START, 0);
    mark(&writer, base + 3 * ONE_SEC + 100 * MS, STALE_PID,
         PGWT_MARKER_EXEC_START, QID_C);
    mark(&writer, base + 3 * ONE_SEC + 500 * MS, STALE_PID,
         PGWT_MARKER_EXEC_END, 0);
    mark(&writer, base + 3 * ONE_SEC + 600 * MS, STALE_PID,
         PGWT_MARKER_CMD_END, 0);
    /* The new command: no id reported before the record closes. */
    mark(&writer, base + 9 * ONE_SEC, STALE_PID, PGWT_MARKER_CMD_START, 0);
    push_q(&writer, base + 12 * ONE_SEC, STALE_PID,
           EV_LOCK, EV_IO, 4 * ONE_SEC, 0);
    mark(&writer, base + 16 * ONE_SEC, STALE_PID, PGWT_MARKER_CMD_END, 0);
    pgwt_writer_close(&writer);
}

/* ── one loaded window, and the two counts whose equality is the issue ──── */

struct win {
    struct pgwt_trace_event *events;
    int count;
    struct pgwt_load_info info;
    uint64_t from, to;
};

static void win_load(struct pgwt_server *srv, struct win *w,
                     uint64_t from_wall, uint64_t to_wall)
{
    memset(w, 0, sizeof(*w));
    w->from = from_wall;
    w->to = to_wall;
    w->events = server_load_events_fi(srv, from_wall, to_wall, 0,
                                      &w->count, &w->info);
}

static void win_free(struct win *w)
{
    free(w->events);
    memset(w, 0, sizeof(*w));
}

/* Total occurrences the window reports, summed over every event row — the
 * "unfiltered 184". `qid != 0` restricts it to one query's drill-down. */
static uint64_t occurrences(const struct win *w, uint64_t qid, uint32_t event)
{
    struct pgwt_filter f;
    memset(&f, 0, sizeof(f));
    f.query_id = qid;
    struct pgwt_events_result r;
    memset(&r, 0, sizeof(r));
    pgwt_compute_top_events(w->events, w->count, &f, w->from, w->to,
                            (double)(w->to - w->from) / 1e6, &r);
    uint64_t n = 0;
    for (int i = 0; i < r.num_rows; i++)
        if (event == 0xFFFFFFFFU || r.rows[i].event_id == event)
            n += r.rows[i].count;
    free(r.rows);
    return n;
}

#define ALL_EVENTS 0xFFFFFFFFU

static double event_ms(const struct win *w, uint32_t event)
{
    struct pgwt_filter f;
    memset(&f, 0, sizeof(f));
    struct pgwt_events_result r;
    memset(&r, 0, sizeof(r));
    pgwt_compute_top_events(w->events, w->count, &f, w->from, w->to,
                            (double)(w->to - w->from) / 1e6, &r);
    double ms = 0;
    for (int i = 0; i < r.num_rows; i++)
        if (r.rows[i].event_id == event)
            ms += r.rows[i].total_ms;
    free(r.rows);
    return ms;
}

static int near_ms(double got, double want)
{
    double d = got - want;
    if (d < 0) d = -d;
    return d < 0.1;
}

/* The per-query sum: every query id this fixture contains. The unattributed
 * bucket is deliberately NOT in this list — a per-query filter cannot select
 * it (query_id 0 means "no filter"), which is exactly why the drill-down
 * loses the record instead of mis-filing it. */
static uint64_t per_query_sum(const struct win *w, uint32_t event)
{
    static const uint64_t ids[] = { QID_F, QID_A, QID_B, QID_C };
    uint64_t n = 0;
    for (size_t i = 0; i < sizeof(ids) / sizeof(ids[0]); i++)
        n += occurrences(w, ids[i], event);
    return n;
}

/* ── 1. The premises: the fixture really does hold the defect ──────────── */
static void test_premises(struct pgwt_server *srv, uint64_t w0)
{
    printf("--- 1. the fixture holds a LEFT straddler whose id marker is "
           "outside the load ---\n");
    struct win w;
    win_load(srv, &w, w0 + WIN_FROM, w0 + WIN_TO);

    int left_straddlers = 0, id_markers_for_it = 0, unattributed = 0;
    for (int i = 0; i < w.count; i++) {
        const struct pgwt_trace_event *ev = &w.events[i];
        if (PGWT_IS_MARKER(ev->old_event)) {
            if (ev->pid == STRADDLE_PID && ev->query_id != 0)
                id_markers_for_it++;
            continue;
        }
        if (ev->duration_ns > 0 && ev->timestamp_ns >= ev->duration_ns &&
            ev->timestamp_ns - ev->duration_ns < w.from)
            left_straddlers++;
    }
    CHECK(left_straddlers == 1,
          "1a. exactly one record starts before `from` (%d). A fixture with "
          "none would pass every assertion below without the fix and prove "
          "nothing -- that is the vacuity trap this section closes",
          left_straddlers);
    CHECK(id_markers_for_it == 0,
          "1b. and the loaded array holds NO id-carrying marker for pid %d "
          "(%d): its EXEC_START is 5.9 s left of `from`, so rule 2's "
          "evidence is outside the window by construction",
          STRADDLE_PID, id_markers_for_it);

    for (int i = 0; i < w.count; i++)
        if (w.events[i].flags & PGWT_EVENT_FLAG_QUERY_UNATTRIB)
            unattributed++;
    CHECK(unattributed == 0,
          "1c. nothing in the window is left UNATTRIBUTED (%d). Before the "
          "seed lookback the straddler was, and the 2500.0 ms it carries "
          "reached no per-query drill-down at all",
          unattributed);
    win_free(&w);
}

/* ── 2. THE ISSUE: the per-query sum equals the unfiltered count ────────── */
static void test_sum_equals_unfiltered(struct pgwt_server *srv, uint64_t w0)
{
    printf("--- 2. per-query sum == unfiltered count (#586's 183 vs 184) "
           "---\n");
    struct win w;
    win_load(srv, &w, w0 + WIN_FROM, w0 + WIN_TO);

    uint64_t unf = occurrences(&w, 0, ALL_EVENTS);
    uint64_t sum = per_query_sum(&w, ALL_EVENTS);
    CHECK(unf > 0, "2a. the unfiltered window is non-empty (%llu "
          "occurrences) -- a zero would satisfy 2b vacuously",
          (unsigned long long)unf);
    CHECK(sum == unf,
          "2b. summing the per-query drill-downs gives %llu against %llu "
          "unfiltered. Before the seed lookback it was %llu vs %llu: the "
          "left straddler was in the unfiltered view and in no query's",
          (unsigned long long)sum, (unsigned long long)unf,
          (unsigned long long)(unf - 1), (unsigned long long)unf);

    uint64_t lock_unf = occurrences(&w, 0, EV_LOCK);
    uint64_t lock_sum = per_query_sum(&w, EV_LOCK);
    CHECK(lock_unf == 2 && lock_sum == 2,
          "2c. and on the Lock row specifically -- the shape #586 reports -- "
          "unfiltered %llu, per-query sum %llu (both 2: the straddler and "
          "the in-window control)",
          (unsigned long long)lock_unf, (unsigned long long)lock_sum);
    CHECK(occurrences(&w, QID_A, EV_LOCK) == 1,
          "2d. the straddler lands under QID_A specifically (%llu), the id "
          "its command reported before the window opened -- not merely "
          "somewhere",
          (unsigned long long)occurrences(&w, QID_A, EV_LOCK));
    win_free(&w);
}

/* ── 3. NO DOUBLE ADMISSION ─────────────────────────────────────────────
 *
 * The seed lookback must add nothing to the array. If it admitted the
 * left-straddling record (or its markers) a second time, every count would
 * rise -- 185 where 184 is right -- and that is a worse answer than the
 * missing attribution it set out to fix. */
static void test_no_double_admission(struct pgwt_server *srv, uint64_t w0)
{
    printf("--- 3. the lookback admits NOTHING: no record counted twice "
           "---\n");
    struct win w;
    win_load(srv, &w, w0 + WIN_FROM, w0 + WIN_TO);

    int straddler_records = 0, pre_window = 0;
    for (int i = 0; i < w.count; i++) {
        if (w.events[i].pid == STRADDLE_PID &&
            !PGWT_IS_MARKER(w.events[i].old_event))
            straddler_records++;
        if (w.events[i].timestamp_ns < w.from)
            pre_window++;
    }
    CHECK(straddler_records == 1,
          "3a. pid %d contributes exactly one record (%d), not two",
          STRADDLE_PID, straddler_records);
    CHECK(pre_window == 0,
          "3b. nothing in the array closes before `from` (%d): the seed "
          "lookback reads markers for their id and does not append them",
          pre_window);
    CHECK(occurrences(&w, 0, EV_LOCK) == 2,
          "3c. the Lock row still counts 2 occurrences (%llu). 3 would be "
          "the double-admission failure wearing the right attribution",
          (unsigned long long)occurrences(&w, 0, EV_LOCK));
    CHECK(occurrences(&w, QID_A, EV_LOCK) == 1,
          "3d. and QID_A's drill-down counts it ONCE (%llu)",
          (unsigned long long)occurrences(&w, QID_A, EV_LOCK));
    win_free(&w);
}

/* ── 4. NO WINDOW WIDENING ──────────────────────────────────────────────
 *
 * The straddler is 7 s long and 2.5 s of it is inside [10, 20]. Attribution
 * must not change which 2.5 s this window owns. #328/#427 settled the edges
 * as left-closed/right-open [from, to), so a record closing exactly at
 * `from` belongs to the earlier window; this section pins the clipped total
 * either way, because that total is what every view's DB Time is built on. */
static void test_no_widening(struct pgwt_server *srv, uint64_t w0)
{
    printf("--- 4. the window's total time is unchanged ---\n");
    struct win w;
    win_load(srv, &w, w0 + WIN_FROM, w0 + WIN_TO);

    double lock = event_ms(&w, EV_LOCK);
    CHECK(near_ms(lock, STRADDLE_IN_MS + INSIDE_IN_MS),
          "4a. Lock totals %.1f ms, i.e. the straddler's clipped 2500.0 plus "
          "the control's 500.0. Its full 7000.0 ms would be the widening "
          "failure -- time moved into a window that did not contain it",
          lock);

    /* The same number, through the per-query path: attribution moves time
     * BETWEEN queries, never into or out of the window. */
    struct pgwt_filter f;
    memset(&f, 0, sizeof(f));
    f.query_id = QID_A;
    struct pgwt_events_result r;
    memset(&r, 0, sizeof(r));
    pgwt_compute_top_events(w.events, w.count, &f, w.from, w.to,
                            (double)(w.to - w.from) / 1e6, &r);
    double qa_lock = 0;
    for (int i = 0; i < r.num_rows; i++)
        if (r.rows[i].event_id == EV_LOCK)
            qa_lock += r.rows[i].total_ms;
    free(r.rows);
    CHECK(near_ms(qa_lock, STRADDLE_IN_MS),
          "4b. and QID_A's own Lock total is %.1f ms -- the CLIPPED share, "
          "not the record's full duration",
          qa_lock);
    win_free(&w);
}

/* ── 5. A new command must not inherit the previous statement's id ─────── */
static void test_stale_id_not_inherited(const char *dir)
{
    printf("--- 5. a CMD_START before `from` resets the seed ---\n");
    struct pgwt_server *srv = calloc(1, sizeof(*srv));
    if (!srv || server_init(srv, dir) != 0) {
        CHECK(0, "5. server_init on the stale-id fixture");
        free(srv);
        return;
    }
    uint64_t w0 = srv->earliest_wall_ns - 10 * MS;
    struct win w;
    win_load(srv, &w, w0 + WIN_FROM, w0 + WIN_TO);

    CHECK(occurrences(&w, QID_C, EV_LOCK) == 0,
          "5a. the record of the SECOND command is not filed under QID_C "
          "(%llu), the id the FIRST command reported 7 s earlier. A seed "
          "that ignored the intervening CMD_START would turn a lost record "
          "into a wrongly attributed one",
          (unsigned long long)occurrences(&w, QID_C, EV_LOCK));

    int unattrib = 0;
    for (int i = 0; i < w.count; i++)
        if (w.events[i].pid == STALE_PID &&
            (w.events[i].flags & PGWT_EVENT_FLAG_QUERY_UNATTRIB))
            unattrib++;
    CHECK(unattrib == 1,
          "5b. it stays in the UNATTRIBUTED bucket (%d) -- visible and "
          "counted, which is rule 3 and the honest answer when the command "
          "reported no id",
          unattrib);
    win_free(&w);
    server_destroy(srv);
    free(srv);
}

/* ── 6. BYPASS SUITE: every way the seed lookback can stop SEEING ───────
 *
 * Each case runs in a forked child so one process's getenv() cannot leak
 * into the next, and reports two numbers back: the per-query Lock sum and
 * whether the load SAID its left edge was cut. */
struct bypass_out {
    uint64_t lock_sum;
    uint64_t lock_unfiltered;
    int      left_truncated;
    int      seeds_resolved;
    int      trace_unreadable;
};

static int run_bypass(const char *dir, const char *env, const char *val,
                      struct bypass_out *out)
{
    int fds[2];
    if (pipe(fds) != 0)
        return -1;
    pid_t pid = fork();
    if (pid < 0) {
        close(fds[0]); close(fds[1]);
        return -1;
    }
    if (pid == 0) {
        close(fds[0]);
        if (env) {
            if (val) setenv(env, val, 1);
            else     unsetenv(env);
        }
        struct bypass_out r;
        memset(&r, 0, sizeof(r));
        struct pgwt_server *srv = calloc(1, sizeof(*srv));
        if (srv && server_init(srv, dir) == 0) {
            uint64_t w0 = srv->earliest_wall_ns - 10 * MS;
            struct win w;
            win_load(srv, &w, w0 + WIN_FROM, w0 + WIN_TO);
            r.lock_sum = per_query_sum(&w, EV_LOCK);
            r.lock_unfiltered = occurrences(&w, 0, EV_LOCK);
            r.left_truncated = w.info.left_edge_truncated;
            r.seeds_resolved = w.info.left_seeds_resolved;
            r.trace_unreadable = w.info.trace_unreadable ||
                                 w.info.allocation_failed ||
                                 w.info.overloaded;
            win_free(&w);
            server_destroy(srv);
        }
        free(srv);
        ssize_t n = write(fds[1], &r, sizeof(r));
        (void)n;
        close(fds[1]);
        _exit(0);
    }
    close(fds[1]);
    memset(out, 0, sizeof(*out));
    ssize_t got = read(fds[0], out, sizeof(*out));
    close(fds[0]);
    int st = 0;
    waitpid(pid, &st, 0);
    return got == (ssize_t)sizeof(*out) ? 0 : -1;
}

static void test_bypass(const char *dir_main, const char *dir_nostraddle,
                        const char *dir_noid)
{
    printf("--- 6. bypass suite: a seed scan that cannot see must REFUSE "
           "---\n");
    struct bypass_out base, b;

    if (run_bypass(dir_main, NULL, NULL, &base) != 0) {
        CHECK(0, "6. the bypass harness could not run the control");
        return;
    }
    CHECK(base.lock_sum == 2 && base.left_truncated == 0 &&
          base.seeds_resolved == 1,
          "6a. control, default horizon: per-query Lock sum %llu, "
          "truncated=%d, seeds=%d. Every case below is read against these",
          (unsigned long long)base.lock_sum, base.left_truncated,
          base.seeds_resolved);

    /* 6b. horizon 0 = the lookback disabled. The pre-#586 answer, and the
     * loader must SAY the left edge was not scanned rather than present the
     * short answer as complete. */
    if (run_bypass(dir_main, "PGWT_LEFT_SEED_LOOKBACK_NS", "0", &b) == 0)
        CHECK(b.lock_sum == 1 && b.left_truncated == 1,
              "6b. horizon 0: the sum drops to %llu (the defect) AND the "
              "load reports truncated=%d. A silent 1 would be the false "
              "negative this suite exists for",
              (unsigned long long)b.lock_sum, b.left_truncated);
    else
        CHECK(0, "6b. harness failure");

    /* 6c. a horizon shorter than the distance to the marker: 1 s back from
     * `from` reaches 9 s, and the EXEC_START is at 4.1 s. */
    if (run_bypass(dir_main, "PGWT_LEFT_SEED_LOOKBACK_NS", "1000000000",
                   &b) == 0)
        CHECK(b.lock_sum == 1 && b.left_truncated == 1 &&
              b.seeds_resolved == 0,
              "6c. a 1 s horizon cannot reach a marker 5.9 s back: sum "
              "%llu, seeds=%d, truncated=%d -- the bound stopped the scan "
              "and the answer says so",
              (unsigned long long)b.lock_sum, b.seeds_resolved,
              b.left_truncated);
    else
        CHECK(0, "6c. harness failure");

    /* 6d. an UNPARSEABLE horizon must not be read as 0. "garbage" silently
     * meaning "disabled" is a gate switched off by a typo. */
    if (run_bypass(dir_main, "PGWT_LEFT_SEED_LOOKBACK_NS", "30s", &b) == 0)
        CHECK(b.lock_sum == 2 && b.left_truncated == 0,
              "6d. an unparseable horizon falls back to the default, it does "
              "not disable the scan: sum %llu, truncated=%d",
              (unsigned long long)b.lock_sum, b.left_truncated);
    else
        CHECK(0, "6d. harness failure");

    /* 6e. the seed load itself refused (allocation failure at the one
     * injection point the scan owns). Refuse + say so; never approve. */
    if (run_bypass(dir_main, "PGWT_TEST_LOAD_ALLOC_FAIL", "left_seeds",
                   &b) == 0)
        CHECK(b.lock_sum == 1 && b.left_truncated == 1,
              "6e. the seed scan's allocation refused: sum %llu, "
              "truncated=%d. It degrades to the pre-#586 answer and flags "
              "it, instead of carrying on with a partial seed table",
              (unsigned long long)b.lock_sum, b.left_truncated);
    else
        CHECK(0, "6e. harness failure");

    /* 6h. the NEEDY-PID TABLE could not be built, so WHICH pids have a
     * rule-2 hole is unknown. Unknown is never "none". (The table grows on
     * its first insert, so this point is reached on every window -- a
     * refusal path that could not be reached would be indistinguishable
     * from one that approves.) */
    if (run_bypass(dir_main, "PGWT_TEST_LOAD_ALLOC_FAIL", "left_needy_grow",
                   &b) == 0)
        CHECK(b.lock_sum == 1 && b.left_truncated == 1 &&
              b.seeds_resolved == 0,
              "6h. the needy-pid table refused: sum %llu, seeds=%d, "
              "truncated=%d -- not a silent 'no pid needs a seed'",
              (unsigned long long)b.lock_sum, b.seeds_resolved,
              b.left_truncated);
    else
        CHECK(0, "6h. harness failure");

    /* 6i. the seeds were RESOLVED and then the array holding them could not
     * be allocated. The one case where the scan saw everything and still
     * cannot deliver: it must drop the seeds it found rather than pass a
     * short array, and still flag the edge. */
    if (run_bypass(dir_main, "PGWT_TEST_LOAD_ALLOC_FAIL", "left_seed_array",
                   &b) == 0)
        CHECK(b.lock_sum == 1 && b.left_truncated == 1 &&
              b.seeds_resolved == 0,
              "6i. the resolved seeds could not be handed over: sum %llu, "
              "seeds=%d, truncated=%d. A partial seed table would attribute "
              "some of a window's left-edge records and not others",
              (unsigned long long)b.lock_sum, b.seeds_resolved,
              b.left_truncated);
    else
        CHECK(0, "6i. harness failure");

    /* 6f. ABSENCE, not wrongness: a window with no left straddler at all.
     * Proves 6a's counters are produced by a record and not by the code
     * path -- the fixture that would have made this whole file vacuous. */
    if (run_bypass(dir_nostraddle, NULL, NULL, &b) == 0)
        CHECK(b.lock_sum == 2 && b.lock_unfiltered == 2 &&
              b.left_truncated == 0 && b.seeds_resolved == 0,
              "6f. no left straddler: sum %llu == unfiltered %llu with "
              "seeds=%d and truncated=%d. The scan did nothing because "
              "there was nothing to do -- so 6a's seeds=1 is a fact about "
              "the data, not about the build",
              (unsigned long long)b.lock_sum,
              (unsigned long long)b.lock_unfiltered, b.seeds_resolved,
              b.left_truncated);
    else
        CHECK(0, "6f. harness failure");

    /* 6g. the thing looked for is ABSENT rather than out of reach: the
     * command genuinely never reported an id. The scan reaches trace start,
     * so nothing was missed; UNATTRIBUTED is the right answer and
     * truncated must be 0 -- a flag raised here would be a false alarm on
     * every capture that starts mid-command. */
    if (run_bypass(dir_noid, NULL, NULL, &b) == 0)
        CHECK(b.lock_sum == 1 && b.lock_unfiltered == 2 &&
              b.left_truncated == 0 && b.seeds_resolved == 0,
              "6g. the command reported no id anywhere: sum %llu vs "
              "unfiltered %llu, seeds=%d, truncated=%d. No id is invented "
              "and no truncation is claimed -- the scan saw everything "
              "there was",
              (unsigned long long)b.lock_sum,
              (unsigned long long)b.lock_unfiltered, b.seeds_resolved,
              b.left_truncated);
    else
        CHECK(0, "6g. harness failure");
}

/* ── 7. The unbounded path is untouched ─────────────────────────────────
 *
 * A whole-capture request has no left edge to straddle, so the scan must not
 * run at all and the answer must be the pre-#586 one. */
static void test_unbounded_untouched(struct pgwt_server *srv)
{
    printf("--- 7. a whole-capture request runs no seed scan ---\n");
    struct win w;
    memset(&w, 0, sizeof(w));
    w.events = server_load_events_fi(srv, 0, 0, 0, &w.count, &w.info);
    w.from = srv->earliest_wall_ns;
    w.to = srv->latest_wall_ns;
    CHECK(w.info.left_seeds_resolved == 0 && w.info.left_edge_truncated == 0,
          "7a. seeds=%d truncated=%d on an unbounded load: there is no "
          "window edge, so there is nothing to seed and nothing to flag",
          w.info.left_seeds_resolved, w.info.left_edge_truncated);
    CHECK(occurrences(&w, QID_A, EV_LOCK) == 1,
          "7b. and the straddler is attributed anyway (%llu) -- its "
          "EXEC_START is inside a whole-capture load, so rule 2 has always "
          "worked there",
          (unsigned long long)occurrences(&w, QID_A, EV_LOCK));
    win_free(&w);
}

int main(void)
{
    char dir_main[256], dir_nostraddle[256], dir_noid[256], dir_stale[256];
    const char *tmp = getenv("TMPDIR");
    if (!tmp || !*tmp) tmp = "/tmp";
    snprintf(dir_main, sizeof(dir_main), "%s/pgwt_le_main_%d", tmp,
             (int)getpid());
    snprintf(dir_nostraddle, sizeof(dir_nostraddle), "%s/pgwt_le_nos_%d", tmp,
             (int)getpid());
    snprintf(dir_noid, sizeof(dir_noid), "%s/pgwt_le_noid_%d", tmp,
             (int)getpid());
    snprintf(dir_stale, sizeof(dir_stale), "%s/pgwt_le_stale_%d", tmp,
             (int)getpid());

    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    uint64_t base = (((uint64_t)ts.tv_sec * ONE_SEC +
                      (uint64_t)ts.tv_nsec) / ONE_SEC) * ONE_SEC;

    build_fixture(dir_main, base, FX_MAIN);
    build_fixture(dir_nostraddle, base, FX_NOSTRADDLE);
    build_fixture(dir_noid, base, FX_NOID);
    build_stale_fixture(dir_stale, base);

    struct pgwt_server *srv = calloc(1, sizeof(*srv));
    if (!srv || server_init(srv, dir_main) != 0) {
        printf("FAIL: server_init(%s)\n", dir_main);
        return 1;
    }
    uint64_t w0 = srv->earliest_wall_ns - 10 * MS;

    test_premises(srv, w0);
    test_sum_equals_unfiltered(srv, w0);
    test_no_double_admission(srv, w0);
    test_no_widening(srv, w0);
    test_stale_id_not_inherited(dir_stale);
    test_bypass(dir_main, dir_nostraddle, dir_noid);
    test_unbounded_untouched(srv);

    server_destroy(srv);
    free(srv);

    printf("\n%d checks, %d failed\n", tests_run, tests_failed);
    return tests_failed ? 1 : 0;
}
