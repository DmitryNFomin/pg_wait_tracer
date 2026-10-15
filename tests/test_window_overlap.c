/* test_window_overlap.c — #424: a wait that is still running at the window's
 * right edge must contribute its IN-WINDOW TIME and NOTHING ELSE.
 *
 * THE DEFECT. A trace record is written when its interval CLOSES, and the raw
 * loader (src/server.c) admitted a non-sample record only when that close
 * landed in [from, to]. Correct selection is OVERLAP — `close >= from &&
 * start <= to`. The left half already worked (a wait straddling `from` closes
 * inside the window, arrives whole and is clipped by event_window_ns); the
 * right half existed nowhere, so a wait still running at `to` contributed 0
 * and a wait spanning the whole window was dropped entirely. The symptom that
 * proves it with no reference path: splitting one window into adjacent
 * sub-windows LOST time (raw `Idle` lost 227,136 ms over a 440 s window at
 * one internal boundary, and more at three and seven).
 *
 * WHY THIS FILE TESTS BOTH HALVES AT ONCE. Widening admission alone is worse
 * than the bug it fixes: top_events increments count / max / the histogram,
 * and pgwt_compute_transitions counts every admitted record, with no test of
 * the close time at all. So a record admitted into THIS window because it
 * overlaps would also be counted here, and counted AGAIN in the window it
 * really closes in — a known undercount of time traded for a new overcount of
 * counts, latencies and histograms. Sections 2 and 3 are the half that must
 * not be skipped: section 2 pins that the time arrives, section 3 pins that
 * the count does not, and section 4 pins "exactly once, in the window where
 * it closes" across a split.
 *
 * SCOPE, STATED SO NOBODY READS MORE INTO IT. Every case here is a HISTORICAL
 * window: the straddler's closing record exists in the trace. A wait that is
 * STILL OPEN RIGHT NOW has no closing record at all, so no admission rule can
 * recover it and nothing in this file claims otherwise. That needs
 * open-interval markers in the trace format.
 *
 * WHAT THIS FILE CANNOT REACH. It drives the loader and the compute layer in
 * process; it does not go through the WebSocket protocol, and it does not
 * exercise the live right edge of a growing capture (see above — there is
 * nothing to exercise).
 *
 * BYPASS SUITE (section 6). Every way the straddler lookahead could stop
 * SEEING instead of stop finding: horizon 0 (disabled), a horizon shorter
 * than the straddler, a one-block decode budget, and a window with no
 * straddler in it at all. In each case the loader must either admit the
 * record or SAY it could not look far enough (right_edge_truncated) — a gate
 * that cannot see must refuse, never approve. Section 6d is the
 * absence-not-wrongness case: it proves section 1's counter is produced by a
 * record and not by the code path, by showing it 0 on a window that holds no
 * straddler.
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
    if (cond) { printf("  ok: " fmt "\n", ##__VA_ARGS__); }            \
    else { tests_failed++;                                             \
           printf("  FAIL(line %d): " fmt "\n", __LINE__, ##__VA_ARGS__); } \
} while (0)

#define MS      1000000ULL
#define ONE_SEC 1000000000ULL

#define EV_LOCK WEI(PG_WAIT_LOCK, 0)
#define EV_IO   WEI(PG_WAIT_IO, 21)

#define FILLER_PID    777
#define STRADDLER_PID 601
#define INSIDE_PID    602

/* The fixture, in MONOTONIC seconds from `base`:
 *
 *   filler CPU tick at s+10ms for s = 0..119            (continuous coverage)
 *   INSIDE_PID  Lock close at 15.5 s, duration 1 s      -> [14.5, 15.5]
 *   STRADDLER   Lock close at 35.5 s, duration 30 s     -> [ 5.5, 35.5]
 *
 * Window A = [10, 20]: the straddler covers it completely (10 s of overlap)
 * and closes 15.5 s past its right edge. Window B = [30, 40] contains the
 * close. The straddler is one record and must be COUNTED once, in B, while
 * its TIME is split 10 s / 5.5 s across the two.
 */
#define FIXTURE_SECS      120
#define STRADDLER_CLOSE   (35 * ONE_SEC + 500 * MS)
#define STRADDLER_DUR     (30 * ONE_SEC)
#define INSIDE_CLOSE      (15 * ONE_SEC + 500 * MS)
#define INSIDE_DUR        (1 * ONE_SEC)

static uint64_t mono_origin(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    uint64_t mono = (uint64_t)ts.tv_sec * ONE_SEC + (uint64_t)ts.tv_nsec;
    return (mono / ONE_SEC) * ONE_SEC;
}

static void push(struct pgwt_event_writer *w, uint64_t ts, uint32_t pid,
                 uint32_t old_ev, uint32_t new_ev, uint64_t dur)
{
    struct pgwt_trace_event e;
    memset(&e, 0, sizeof(e));
    e.timestamp_ns = ts;
    e.pid = pid;
    e.old_event = old_ev;
    e.new_event = new_ev;
    e.duration_ns = dur;
    e.cpu_ns = PGWT_CPU_NS_UNKNOWN;
    pgwt_writer_push_event(w, &e);
}

/* `with_straddler == 0` builds the SAME fixture minus the straddling record:
 * the absence case section 6d needs, so "a straddler was admitted" cannot be
 * satisfied by a fixture that never had one. */
static void build_fixture(const char *dir, uint64_t base, int with_straddler)
{
    struct pgwt_event_writer writer;
    if (pgwt_writer_init(&writer, dir, 180000, 0, NULL) != 0) {
        printf("  FAIL: trace writer init\n"); tests_failed++; return;
    }
    for (int s = 0; s < FIXTURE_SECS; s++) {
        uint64_t sec = base + (uint64_t)s * ONE_SEC;
        push(&writer, sec + 10 * MS, FILLER_PID, 0, EV_IO, 1 * MS);
        if (s == 15)
            push(&writer, base + INSIDE_CLOSE, INSIDE_PID,
                 EV_LOCK, EV_IO, INSIDE_DUR);
        if (s == 35 && with_straddler)
            push(&writer, base + STRADDLER_CLOSE, STRADDLER_PID,
                 EV_LOCK, EV_IO, STRADDLER_DUR);
    }
    pgwt_writer_close(&writer);
}

/* One loaded window: the raw array, its load diagnostics, and the Top-N
 * answer computed from it exactly as a handler would. */
struct win {
    struct pgwt_trace_event *events;
    int count;
    struct pgwt_load_info info;
    struct pgwt_events_result ev;
    struct pgwt_transitions_result tr;
};

static void win_load(struct pgwt_server *srv, struct win *w,
                     uint64_t from_wall, uint64_t to_wall)
{
    memset(w, 0, sizeof(*w));
    w->events = server_load_events_fi(srv, from_wall, to_wall, 0,
                                      &w->count, &w->info);
    struct pgwt_filter f;
    memset(&f, 0, sizeof(f));
    double wall_ms = (double)(to_wall - from_wall) / 1e6;
    pgwt_compute_top_events(w->events, w->count, &f, from_wall, to_wall,
                            wall_ms, &w->ev);
    pgwt_compute_transitions(w->events, w->count, &f, 64, &w->tr);
}

static void win_free(struct win *w)
{
    free(w->ev.rows);
    free(w->tr.rows);
    free(w->events);
    memset(w, 0, sizeof(*w));
}

static const struct pgwt_event_row *row_for(const struct pgwt_events_result *r,
                                            uint32_t event_id)
{
    for (int i = 0; i < r->num_rows; i++)
        if (r->rows[i].event_id == event_id)
            return &r->rows[i];
    return NULL;
}

static uint64_t trans_count(const struct pgwt_transitions_result *r,
                            uint32_t from, uint32_t to)
{
    uint64_t n = 0;
    for (int i = 0; i < r->num_rows; i++)
        if (r->rows[i].from_event == from && r->rows[i].to_event == to)
            n += r->rows[i].count;
    return n;
}

static int flagged_right_open(const struct win *w)
{
    int n = 0;
    for (int i = 0; i < w->count; i++)
        if (w->events[i].flags & PGWT_EVENT_FLAG_RIGHT_OPEN)
            n++;
    return n;
}

/* ms, to the nearest 0.1 — the fixture's numbers are exact nanoseconds, so
 * this is formatting tolerance, never measurement tolerance. */
static int near_ms(double got, double want)
{
    double d = got - want;
    if (d < 0) d = -d;
    return d < 0.1;
}

/* ── 1. ADMISSION: the straddler reaches the window at all ─────────────── */
static void test_admission(struct pgwt_server *srv, uint64_t w0)
{
    printf("--- 1. a wait open at the right edge is ADMITTED (overlap) ---\n");
    struct win a;
    win_load(srv, &a, w0 + 10 * ONE_SEC, w0 + 20 * ONE_SEC);

    CHECK(!a.info.overloaded && !a.info.allocation_failed,
          "the window loaded (overloaded=%d alloc_failed=%d)",
          a.info.overloaded, a.info.allocation_failed);
    CHECK(a.info.straddlers_admitted == 1,
          "exactly one record was admitted by OVERLAP (got %d) -- before "
          "#424 this was 0 and the whole window's Lock time was invisible",
          a.info.straddlers_admitted);
    CHECK(flagged_right_open(&a) == 1,
          "exactly one loaded record carries PGWT_EVENT_FLAG_RIGHT_OPEN "
          "(got %d) -- the bit the selection rule reads",
          flagged_right_open(&a));
    CHECK(!a.info.right_edge_truncated,
          "the lookahead reached the close, so the answer is NOT labelled "
          "truncated");
    win_free(&a);
}

/* ── 2. INTERVAL half: the clipped time arrives ────────────────────────── */
static void test_interval_time(struct pgwt_server *srv, uint64_t w0)
{
    printf("--- 2. INTERVAL-selected: the in-window time is contributed ---\n");
    struct win a;
    win_load(srv, &a, w0 + 10 * ONE_SEC, w0 + 20 * ONE_SEC);

    const struct pgwt_event_row *lock = row_for(&a.ev, EV_LOCK);
    CHECK(lock != NULL, "Lock appears in Top Events for a window whose only "
                        "Lock time belongs to a still-running wait");
    if (lock) {
        /* 10 s of straddler overlap + the 1 s wait that closed inside. */
        CHECK(near_ms(lock->total_ms, 11000.0),
              "Lock total_ms = %.1f, expected 11000.0 (10 s of the "
              "[5.5, 35.5] straddler clipped to [10, 20], plus the 1 s "
              "wait that closed at 15.5)", lock->total_ms);
    }
    win_free(&a);
}

/* ── 3. INSTANT half: the occurrence does NOT ──────────────────────────── */
static void test_instant_excluded(struct pgwt_server *srv, uint64_t w0)
{
    printf("--- 3. INSTANT-selected: count/latency/transitions exclude it ---\n");
    struct win a;
    win_load(srv, &a, w0 + 10 * ONE_SEC, w0 + 20 * ONE_SEC);

    const struct pgwt_event_row *lock = row_for(&a.ev, EV_LOCK);
    CHECK(lock != NULL, "the Lock row exists (its time is in it)");
    if (lock) {
        CHECK(lock->count == 1,
              "Lock count = %llu, expected 1 -- only the wait that CLOSED "
              "inside the window is an observation here; admitting the "
              "straddler as a second one is the overcount this gate exists "
              "to refuse", (unsigned long long)lock->count);
        CHECK(lock->exact_count == 1,
              "Lock exact_count = %llu, expected 1 (the latency population "
              "is the closed observation only)",
              (unsigned long long)lock->exact_count);
        /* max is the 1 s wait, NOT the 30 s straddler. */
        CHECK(near_ms(lock->max_us / 1000.0, 1000.0),
              "Lock max = %.1f ms, expected 1000.0 -- the 30 s straddler "
              "must not set a max no completed wait had in this window",
              lock->max_us / 1000.0);
    }
    CHECK(trans_count(&a.tr, EV_LOCK, EV_IO) == 1,
          "Lock->IO transitions = %llu, expected 1 -- a transition IS a "
          "close; the straddler has not transitioned yet",
          (unsigned long long)trans_count(&a.tr, EV_LOCK, EV_IO));
    win_free(&a);
}

/* ── 4. The two halves together: once, and only once ──────────────────── */
static void test_split_conservation(struct pgwt_server *srv, uint64_t w0)
{
    printf("--- 4. split windows: time is additive, the count lands once ---\n");
    struct win whole, a, b, c;
    win_load(srv, &whole, w0 + 10 * ONE_SEC, w0 + 40 * ONE_SEC);
    win_load(srv, &a,     w0 + 10 * ONE_SEC, w0 + 20 * ONE_SEC);
    win_load(srv, &b,     w0 + 20 * ONE_SEC, w0 + 30 * ONE_SEC);
    win_load(srv, &c,     w0 + 30 * ONE_SEC, w0 + 40 * ONE_SEC);

    const struct pgwt_event_row *rw = row_for(&whole.ev, EV_LOCK);
    const struct pgwt_event_row *ra = row_for(&a.ev, EV_LOCK);
    const struct pgwt_event_row *rb = row_for(&b.ev, EV_LOCK);
    const struct pgwt_event_row *rc = row_for(&c.ev, EV_LOCK);
    CHECK(rw && ra && rb && rc,
          "every sub-window has a Lock row (whole=%d a=%d b=%d c=%d)",
          rw != NULL, ra != NULL, rb != NULL, rc != NULL);
    if (!(rw && ra && rb && rc)) goto out;

    /* ADDITIVITY — the property that proved the defect with no reference
     * path. [10,40] holds the 1 s closed wait plus 25.5 s of straddler. */
    double sum = ra->total_ms + rb->total_ms + rc->total_ms;
    CHECK(near_ms(sum, rw->total_ms),
          "Lock time is additive across the split: %.1f ms (10-20) + %.1f "
          "(20-30) + %.1f (30-40) = %.1f vs %.1f for 10-40",
          ra->total_ms, rb->total_ms, rc->total_ms, sum, rw->total_ms);
    CHECK(near_ms(rw->total_ms, 26500.0),
          "and the whole-window figure is the arithmetic truth, 26500.0 ms "
          "(25.5 s of the straddler + 1 s closed), not %.1f", rw->total_ms);

    /* CONSERVATION OF OCCURRENCES. Two Lock records exist; across three
     * disjoint windows each must be counted exactly once, and the
     * straddler's one count must land in the window holding its close. */
    uint64_t counts = ra->count + rb->count + rc->count;
    CHECK(counts == 2,
          "2 Lock records produce 2 counts across the three windows (got "
          "%llu: %llu + %llu + %llu) -- 3 would be the double-count an "
          "admission-only change creates",
          (unsigned long long)counts, (unsigned long long)ra->count,
          (unsigned long long)rb->count, (unsigned long long)rc->count);
    CHECK(rc->count == 1 && rb->count == 0,
          "the straddler's count lands in [30,40] where it CLOSES (%llu), "
          "not in [20,30] which it merely crosses (%llu)",
          (unsigned long long)rc->count, (unsigned long long)rb->count);
    CHECK(trans_count(&a.tr, EV_LOCK, EV_IO) +
          trans_count(&b.tr, EV_LOCK, EV_IO) +
          trans_count(&c.tr, EV_LOCK, EV_IO) == 2,
          "and the transition matrix counts the same two closes once each "
          "(%llu) -- it is keyed by close, so a straddler counted here "
          "would be counted twice",
          (unsigned long long)(trans_count(&a.tr, EV_LOCK, EV_IO) +
                               trans_count(&b.tr, EV_LOCK, EV_IO) +
                               trans_count(&c.tr, EV_LOCK, EV_IO)));
out:
    win_free(&whole); win_free(&a); win_free(&b); win_free(&c);
}

/* ── 5. The unbounded path is untouched ───────────────────────────────── */
static void test_unbounded_identical(struct pgwt_server *srv)
{
    printf("--- 5. a whole-capture request stamps no right-open record ---\n");
    struct win w;
    win_load(srv, &w, 0, 0);
    CHECK(w.info.straddlers_admitted == 0 && flagged_right_open(&w) == 0,
          "to_ns == 0 admits no straddler and flags nothing (%d/%d): there "
          "is no right edge to be open at", w.info.straddlers_admitted,
          flagged_right_open(&w));
    CHECK(!w.info.right_edge_truncated,
          "and it is not labelled truncated -- nothing was cut");
    win_free(&w);
}

/* ── 6. BYPASS SUITE: every way the lookahead could stop SEEING ────────
 *
 * Each case runs in a forked child: straddle_lookahead_ns() and
 * straddle_max_blocks() cache their env lookup process-wide, exactly as
 * load_max_events() does, so an override must not leak into the sections
 * above. The child's exit status is the assertion. */
struct bypass_result {
    int admitted;
    int truncated;
    int lock_row_present;
    double lock_total_ms;
    uint64_t lock_count;
};

static int run_bypass(const char *dir, uint64_t w0_unused,
                      const char *env_name, const char *env_value,
                      struct bypass_result *out, int window_lo, int window_hi)
{
    (void)w0_unused;
    int fds[2];
    if (pipe(fds) != 0) return -1;
    pid_t pid = fork();
    if (pid < 0) { close(fds[0]); close(fds[1]); return -1; }
    if (pid == 0) {
        close(fds[0]);
        if (env_name) setenv(env_name, env_value, 1);
        struct pgwt_server *srv = calloc(1, sizeof(*srv));
        struct bypass_result r;
        memset(&r, 0, sizeof(r));
        if (srv && server_init(srv, dir) == 0) {
            uint64_t w0 = srv->earliest_wall_ns - 10 * MS;
            struct win w;
            win_load(srv, &w, w0 + (uint64_t)window_lo * ONE_SEC,
                     w0 + (uint64_t)window_hi * ONE_SEC);
            const struct pgwt_event_row *lock = row_for(&w.ev, EV_LOCK);
            r.admitted = w.info.straddlers_admitted;
            r.truncated = w.info.right_edge_truncated;
            r.lock_row_present = lock != NULL;
            r.lock_total_ms = lock ? lock->total_ms : 0.0;
            r.lock_count = lock ? lock->count : 0;
            win_free(&w);
        }
        ssize_t n = write(fds[1], &r, sizeof(r));
        (void)n;
        close(fds[1]);
        _exit(0);
    }
    close(fds[1]);
    ssize_t got = read(fds[0], out, sizeof(*out));
    close(fds[0]);
    int status = 0;
    waitpid(pid, &status, 0);
    return got == (ssize_t)sizeof(*out) ? 0 : -1;
}

static void test_bypass(const char *dir, const char *dir_nostraddler)
{
    printf("--- 6. bypass suite: a lookahead that cannot see must REFUSE ---\n");
    struct bypass_result r;

    /* 6a. Lookahead switched off entirely. The pre-#424 answer is allowed —
     * what is NOT allowed is producing it while claiming completeness. */
    if (run_bypass(dir, 0, "PGWT_STRADDLE_LOOKAHEAD_NS", "0", &r, 10, 20) == 0) {
        CHECK(r.admitted == 0,
              "6a. horizon 0 admits no straddler (%d) -- the pre-#424 "
              "selection, reproduced on demand", r.admitted);
        CHECK(r.truncated == 1,
              "6a. and SAYS SO: right_edge_truncated=%d. A disabled "
              "lookahead that reported a clean answer would be the silent "
              "undercount #424 is about", r.truncated);
    } else { CHECK(0, "6a. bypass child failed"); }

    /* 6b. A horizon shorter than the straddler: the close is 15.5 s past
     * the right edge, the horizon is 1 s. */
    if (run_bypass(dir, 0, "PGWT_STRADDLE_LOOKAHEAD_NS", "1000000000",
                   &r, 10, 20) == 0) {
        CHECK(r.admitted == 0 && r.truncated == 1,
              "6b. a 1 s horizon cannot reach a close 15.5 s away: "
              "admitted=%d truncated=%d -- partial, and labelled partial",
              r.admitted, r.truncated);
    } else { CHECK(0, "6b. bypass child failed"); }

    /* 6c. A one-block decode budget. The straddler's close is many blocks
     * past the edge in this fixture, so the budget, not the horizon, is
     * what cuts — and it must be just as audible. */
    if (run_bypass(dir, 0, "PGWT_STRADDLE_MAX_BLOCKS", "1", &r, 10, 20) == 0) {
        CHECK(r.truncated == 1 || r.admitted == 1,
              "6c. a 1-block budget either still reaches the close "
              "(admitted=%d) or reports the cut (truncated=%d) -- never "
              "a quiet partial", r.admitted, r.truncated);
    } else { CHECK(0, "6c. bypass child failed"); }

    /* 6d. ABSENCE, not wrongness. The same window over a fixture with no
     * straddling record: section 1's counter must be 0, which is what makes
     * its `== 1` a statement about a record rather than about the code
     * path having run. */
    if (run_bypass(dir_nostraddler, 0, NULL, NULL, &r, 10, 20) == 0) {
        CHECK(r.admitted == 0 && !r.truncated,
              "6d. a window with no straddler admits none (%d) and is not "
              "truncated (%d) -- section 1 cannot be satisfied by absence",
              r.admitted, r.truncated);
        CHECK(r.lock_row_present && near_ms(r.lock_total_ms, 1000.0),
              "6d. and the ordinary closed wait is still there, 1000.0 ms "
              "(got %.1f, present=%d) -- the fixture differs by the "
              "straddler alone", r.lock_total_ms, r.lock_row_present);
    } else { CHECK(0, "6d. bypass child failed"); }
}

/* ── 7. The compute-side rule holds without the loader's bit ──────────── */
static void test_derived_without_flag(void)
{
    printf("--- 7. compute re-derives openness, so a missing bit cannot "
           "overcount ---\n");
    /* A hand-built array, as a future loader (or a test harness) that admits
     * straddlers but forgets to stamp the flag would produce it. The
     * per-occurrence statistics must still exclude the record. */
    struct pgwt_trace_event ev[2];
    memset(ev, 0, sizeof(ev));
    ev[0].timestamp_ns = 15 * ONE_SEC;  /* closes inside  */
    ev[0].pid = INSIDE_PID;
    ev[0].old_event = EV_LOCK;
    ev[0].new_event = EV_IO;
    ev[0].duration_ns = 1 * ONE_SEC;
    ev[0].cpu_ns = PGWT_CPU_NS_UNKNOWN;
    ev[1].timestamp_ns = 35 * ONE_SEC;  /* closes after `to`, NO flag */
    ev[1].pid = STRADDLER_PID;
    ev[1].old_event = EV_LOCK;
    ev[1].new_event = EV_IO;
    ev[1].duration_ns = 30 * ONE_SEC;
    ev[1].cpu_ns = PGWT_CPU_NS_UNKNOWN;

    struct pgwt_filter f;
    memset(&f, 0, sizeof(f));
    struct pgwt_events_result res;
    pgwt_compute_top_events(ev, 2, &f, 10 * ONE_SEC, 20 * ONE_SEC,
                            10000.0, &res);
    const struct pgwt_event_row *lock = row_for(&res, EV_LOCK);
    CHECK(lock && lock->count == 1,
          "count = %llu with the flag absent -- event_right_open() also "
          "re-derives `timestamp_ns > to_ns`, so the rule does not depend "
          "on one producer remembering to stamp a bit",
          (unsigned long long)(lock ? lock->count : 0));
    CHECK(lock && near_ms(lock->total_ms, 11000.0),
          "and its time is still the clipped 11000.0 ms (got %.1f)",
          lock ? lock->total_ms : 0.0);
    free(res.rows);
}

int main(void)
{
    printf("=== test_window_overlap (#424) ===\n");

    char dir[] = "/tmp/pgwt_wovl_XXXXXX";
    char dir2[] = "/tmp/pgwt_wovl_ns_XXXXXX";
    if (!mkdtemp(dir) || !mkdtemp(dir2)) {
        printf("FAIL: mkdtemp\n");
        return 1;
    }
    uint64_t base = mono_origin();
    build_fixture(dir, base, 1);
    build_fixture(dir2, base, 0);

    struct pgwt_server *srv = calloc(1, sizeof(*srv));
    if (!srv || server_init(srv, dir) != 0) {
        printf("FAIL: server_init\n");
        return 1;
    }
    /* Wall value of the fixture's `base`: the earliest record sits 10 ms
     * into it. Windows below are stated in fixture seconds. */
    uint64_t w0 = srv->earliest_wall_ns - 10 * MS;

    test_admission(srv, w0);
    test_interval_time(srv, w0);
    test_instant_excluded(srv, w0);
    test_split_conservation(srv, w0);
    test_unbounded_identical(srv);
    test_bypass(dir, dir2);
    test_derived_without_flag();

    printf("\n%d checks, %d failed\n", tests_run, tests_failed);
    return tests_failed ? 1 : 0;
}
