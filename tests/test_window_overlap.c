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
#include "output.h"   /* pgwt_row_is_empty: the row-suppression rule both
                        surfaces share (section 12) */

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

/* WINDOW ORIGIN CONVENTION -- READ THIS BEFORE ADDING A FIXTURE HERE.
 *
 * Every window in this file is measured from `srv->earliest_wall_ns - 10 ms`,
 * NOT from the base timestamp the fixture writer was given. `window_lo` and
 * `window_hi` are whole seconds from that origin. The origin is therefore
 * decided by the EARLIEST RECORD the fixture happens to write, which means a
 * fixture whose first record sits well after its own `base` shifts every
 * window in the section by that distance.
 *
 * That is not hypothetical and it is not a crash -- it is a SILENT MISS. The
 * tiered-budget section (the SAMPLES-blocks-charged-against-the-straddler-
 * budget defect) first passed 63/63 against the UNFIXED tree: its fixture
 * started at base + 10 s, so every window slid ~10 s and landed on the
 * SAMPLES blocks, where they sit inside the sample edge, are never charged
 * to the budget, and the defect is unreachable. The section was green
 * because it was pointed somewhere else. One anchor record at base + 10 ms
 * pinned the origin, and only then did it go red.
 *
 * So: either write an anchor record at the base, or compute your window
 * bounds from the same earliest record the loader will. And whichever you
 * do, run the new section against the PARENT tree and confirm it fails --
 * a section that has never been red has not been shown to be looking at
 * anything.
 */
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

/* ── Handler plumbing for sections 8-10 (the response, not the compute) ── */
typedef void (*handler_fn)(struct pgwt_server *, struct pgwt_request *);

static cJSON *call_handler(handler_fn fn, struct pgwt_server *srv,
                           struct pgwt_request *req)
{
    FILE *capture = tmpfile();
    if (!capture) return NULL;
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

/* ── 8. An incomplete scan is VISIBLE IN THE RESPONSE ─────────────────
 *
 * The budget can miss a straddler longer than the horizon. Storing that fact
 * in a struct nobody serialises would reproduce, one layer up, the very
 * defect #424 exists to remove: a quietly short answer. The caller must be
 * able to tell a complete answer from a bounded one. */
static void test_truncation_surfaced(const char *dir)
{
    printf("--- 8. a budget-limited scan is reported in the RESPONSE ---\n");
    int fds[2];
    if (pipe(fds) != 0) { CHECK(0, "8. pipe"); return; }
    pid_t pid = fork();
    if (pid < 0) { CHECK(0, "8. fork"); return; }
    if (pid == 0) {
        close(fds[0]);
        /* A 1 s horizon cannot reach a close 15.5 s past the edge. */
        setenv("PGWT_STRADDLE_LOOKAHEAD_NS", "1000000000", 1);
        struct pgwt_server *srv = calloc(1, sizeof(*srv));
        int r[3] = {0, 0, 0};
        if (srv && server_init(srv, dir) == 0) {
            uint64_t w0 = srv->earliest_wall_ns - 10 * MS;
            struct pgwt_request req;
            memset(&req, 0, sizeof(req));
            req.id = 1;
            req.from_ns = w0 + 10 * ONE_SEC;
            req.to_ns   = w0 + 20 * ONE_SEC;
            cJSON *out = call_handler(handle_top_events, srv, &req);
            cJSON *t = out ? cJSON_GetObjectItem(out, "right_edge_truncated")
                           : NULL;
            cJSON *s = out ? cJSON_GetObjectItem(out, "right_edge_scan_ns")
                           : NULL;
            r[0] = t && cJSON_IsTrue(t);
            r[1] = s != NULL;
            cJSON_Delete(out);

            /* And the complete answer must NOT carry the field, so a client
             * sees "bounded" only when it really is. */
            unsetenv("PGWT_STRADDLE_LOOKAHEAD_NS");
            cJSON *ok = call_handler(handle_top_events, srv, &req);
            r[2] = ok && cJSON_GetObjectItem(ok, "right_edge_truncated") == NULL;
            cJSON_Delete(ok);
        }
        ssize_t n = write(fds[1], r, sizeof(r));
        (void)n;
        close(fds[1]);
        _exit(0);
    }
    close(fds[1]);
    int r[3] = {-1, -1, -1};
    ssize_t got = read(fds[0], r, sizeof(r));
    close(fds[0]);
    int status = 0;
    waitpid(pid, &status, 0);
    if (got != (ssize_t)sizeof(r)) { CHECK(0, "8. child failed"); return; }
    CHECK(r[0] == 1,
          "8a. the response carries right_edge_truncated=true (%d) -- the "
          "flag reaches the CALLER, not just pgwt_load_info", r[0]);
    CHECK(r[1] == 1,
          "8b. and right_edge_scan_ns, so \"incomplete\" arrives with the "
          "bound that made it incomplete (%d)", r[1]);
    CHECK(r[2] == 1,
          "8c. a COMPLETE answer omits the field entirely (%d) -- otherwise "
          "the signal is noise and clients learn to ignore it", r[2]);
}

/* ── 8d. The skipped-FILE bypass ──────────────────────────────────────
 *
 * A file whose records all close past the horizon is rejected by the
 * per-file gate BEFORE IT IS OPENED, so the record-level truncation flag
 * inside load_file_range_mono can never fire for it. That is a gate that
 * cannot see: in a rotated capture the straddler's close lives in the next
 * hourly file, out past the horizon, and without this the window returns
 * short with nothing said. straddle_file_unseen() is the predicate the
 * per-file loop now calls.
 *
 * Tested as a predicate rather than through a two-file fixture, and the
 * reason is stated rather than hidden: a second trace file only exists
 * after the daemon's HOURLY rotation writes an LZ4-compressed
 * `YYYY-MM-DD_HH.trace.lz4`, which is keyed to wall-clock time and cannot
 * be provoked deterministically from a unit test. What is gated here is the
 * decision; its call site is one line at the per-file `continue`. This is
 * NOT end-to-end coverage of a rotated capture.
 */
static void test_skipped_file_bypass(void)
{
    printf("--- 8d. a file skipped past the horizon must REFUSE ---\n");
    uint64_t from = 10 * ONE_SEC, scan_to = 320 * ONE_SEC;

    CHECK(straddle_file_unseen(1, 7200 * ONE_SEC, 7260 * ONE_SEC,
                               from, scan_to) == 1,
          "8d. the next hourly file starts 2 h past a 320 s scan edge: "
          "UNSEEN, so the load is marked truncated -- the one case where "
          "the record-level flag is structurally unreachable");
    CHECK(straddle_file_unseen(1, 0, 400 * ONE_SEC, from, scan_to) == 0,
          "8d. a file the scan actually covers is not flagged -- otherwise "
          "every ordinary window would claim to be incomplete and the "
          "signal would be noise");
    CHECK(straddle_file_unseen(1, 0, 5 * ONE_SEC, from, scan_to) == 0,
          "8d. a file entirely BEFORE the window is not flagged: nothing "
          "that closed before `from` can overlap it");
    CHECK(straddle_file_unseen(0, 7200 * ONE_SEC, 7260 * ONE_SEC,
                               from, scan_to) == 0,
          "8d. and an unbounded request flags nothing -- there is no right "
          "edge for a wait to be open at");
}


/* ── 9. Concurrency is an INTERVAL quantity ───────────────────────────── */
static void test_concurrency_overlap(struct pgwt_server *srv, uint64_t w0)
{
    printf("--- 9. concurrent occupancy counts a session still waiting ---\n");
    int count;
    struct pgwt_load_info linfo = {0};
    uint64_t from = w0 + 10 * ONE_SEC, to = w0 + 20 * ONE_SEC;
    struct pgwt_trace_event *events =
        server_load_events_fi(srv, from, to, 0, &count, &linfo);
    struct pgwt_filter f;
    memset(&f, 0, sizeof(f));
    struct pgwt_concurrency_result res;
    pgwt_compute_concurrency(events, count, &f, from, to, 10,
                             ONE_SEC, 2, &res);
    int lock_buckets = 0;
    for (int b = 0; b < res.num_buckets; b++)
        if (res.peak_event[b] == EV_LOCK && res.peak_sessions[b] >= 1)
            lock_buckets++;
    CHECK(lock_buckets >= 9,
          "a session that is STILL waiting at `to` occupies the buckets it "
          "covers: %d of %d buckets show a Lock waiter. Selecting occupancy "
          "by the wait's CLOSE dropped exactly the longest waits from the "
          "peak -- the opposite of what the burst view is for",
          lock_buckets, res.num_buckets);
    free(res.peak_sessions);
    free(res.peak_event);
    free(res.bursts);
    free(events);
}

/* ── 10. Transition NODE totals must match the edges ──────────────────── */
static void test_node_totals(struct pgwt_server *srv, uint64_t w0)
{
    printf("--- 10. node bars exclude the straddler, as its edges do ---\n");
    struct pgwt_request req;
    memset(&req, 0, sizeof(req));
    req.id = 2;
    req.from_ns = w0 + 10 * ONE_SEC;
    req.to_ns   = w0 + 20 * ONE_SEC;
    req.limit = 50;
    cJSON *out = call_handler(handle_transitions, srv, &req);
    CHECK(out != NULL, "handle_transitions produced parseable JSON");
    if (!out) return;
    double lock_ms = -1.0;
    cJSON *nodes = cJSON_GetObjectItem(out, "nodes");
    cJSON *n;
    cJSON_ArrayForEach(n, nodes) {
        cJSON *id = cJSON_GetObjectItem(n, "event_id");
        if (id && (uint32_t)id->valuedouble == EV_LOCK) {
            cJSON *ms = cJSON_GetObjectItem(n, "total_ms");
            lock_ms = ms ? ms->valuedouble : -1.0;
        }
    }
    CHECK(near_ms(lock_ms, 1000.0),
          "the Lock node bar is %.1f ms, expected 1000.0 -- node totals are "
          "FULL durations keyed by close (block_agg C6/C7), so feeding the "
          "accumulator unbounded credited the straddler's whole 30 s to a "
          "node whose EDGE the same response correctly refused", lock_ms);
    cJSON_Delete(out);
}

/* ── 11. The clipped majority is a DB TIME fix, not a label fix ────────
 *
 * pid 701 runs one command that ENDS 1 s into a 30 s on-CPU stretch which
 * closes long after the window. Judged on the full duration the stretch is
 * 1/30 in-command -> relabelled PGWT_WEI_NONCMD_CPU -> dropped from DB Time.
 * Judged on its in-window portion ([t0, to] = 2 s) it is 1/2 in-command ->
 * stays CPU -> counted. The window's DB Time differs by the clipped 2 s,
 * which is a headline number, not a tag. */
#define CMD_PID 701
static void build_cmd_fixture(const char *dir, uint64_t base)
{
    struct pgwt_event_writer writer;
    if (pgwt_writer_init(&writer, dir, 180000, 0, NULL) != 0) {
        printf("  FAIL: trace writer init (cmd)\n"); tests_failed++; return;
    }
    for (int s = 0; s < 60; s++)
        push(&writer, base + (uint64_t)s * ONE_SEC + 10 * MS,
             FILLER_PID, 0, EV_IO, 1 * MS);
    /* Command opens at 10.2 s and closes at 11.2 s — BOTH inside the
     * window, so the loader sees the whole command run and the majority
     * rule has something to divide. (A CMD_START before `from` is not
     * admitted at all, and then neither rule fires: that version of this
     * fixture tested nothing, which is why the numbers are spelled out.) */
    push(&writer, base + 10 * ONE_SEC + 200 * MS, CMD_PID,
         PGWT_MARKER_CMD_START, PGWT_MARKER_CMD_START, 0);
    push(&writer, base + 11 * ONE_SEC + 200 * MS, CMD_PID,
         PGWT_MARKER_CMD_END, PGWT_MARKER_CMD_END, 0);
    /* On-CPU stretch [10, 40]: closes at 40 s, 30 s long. Window is
     * [10, 12], so its in-window part is 2 s of which 1 s is in-command. */
    push(&writer, base + 40 * ONE_SEC, CMD_PID, 0, EV_IO, 30 * ONE_SEC);
    pgwt_writer_close(&writer);
}

static void test_clipped_majority_moves_db_time(const char *dir)
{
    printf("--- 11. a straddler's in-command majority is judged on the "
           "in-window portion ---\n");
    struct pgwt_server *srv = calloc(1, sizeof(*srv));
    if (!srv || server_init(srv, dir) != 0) {
        CHECK(0, "11. server_init on the command fixture");
        return;
    }
    uint64_t w0 = srv->earliest_wall_ns - 10 * MS;
    int count;
    struct pgwt_load_info linfo = {0};
    uint64_t from = w0 + 10 * ONE_SEC, to = w0 + 12 * ONE_SEC;
    struct pgwt_trace_event *events =
        server_load_events_fi(srv, from, to, 0, &count, &linfo);
    CHECK(linfo.straddlers_admitted == 1,
          "11a. the on-CPU stretch is admitted by overlap (%d) -- without "
          "that there is no majority to decide and this section is vacuous",
          linfo.straddlers_admitted);

    int relabelled = 0, kept_cpu = 0;
    for (int i = 0; i < count; i++) {
        if (events[i].pid != CMD_PID) continue;
        if (!(events[i].flags & PGWT_EVENT_FLAG_RIGHT_OPEN)) continue;
        if (events[i].old_event == PGWT_WEI_NONCMD_CPU) relabelled++;
        if (events[i].old_event == 0) kept_cpu++;
    }
    CHECK(kept_cpu == 1 && relabelled == 0,
          "11b. the straddler keeps its CPU label (kept=%d relabelled=%d): "
          "1 s of its in-window 2 s was in-command. The full-duration rule "
          "saw 1 s of 30 s, relabelled it PGWT_WEI_NONCMD_CPU and took its "
          "time OUT of DB Time -- a wrong headline, not a wrong tag",
          kept_cpu, relabelled);

    struct pgwt_filter f;
    memset(&f, 0, sizeof(f));
    struct pgwt_tm_result tm;
    pgwt_compute_time_model(events, count, &f, from, to, 2000.0, &tm);
    CHECK(tm.db_time_ms > 1900.0,
          "11c. and DB Time for the window is %.1f ms, i.e. it contains the "
          "straddler's clipped 2 s. The relabel would have left ~0",
          tm.db_time_ms);
    free(tm.rows);
    free(events);
    server_destroy(srv);
    free(srv);
}

/* ── 12. A time-positive, count-zero row must be VISIBLE ──────────────
 *
 * The side effect the repair itself creates. Once admission is overlap, an
 * event whose whole in-window time belongs to a wait still running at `to`
 * has milliseconds and zero completed occurrences. Every table in
 * src/output.c suppressed rows on `count == 0`, so that time would land in
 * DB Time and the Time Model with no row to explain it — a total that moves
 * with a breakdown that cannot account for it.
 *
 * 12a gates the shared predicate both surfaces now use. 12b gates the
 * SERVER response end to end (what the web UI consumes): the row is
 * emitted, with its time and with count 0. The TUI print functions take a
 * fully built `struct pgwt_daemon` and live in the daemon link set, not
 * this test's, so their seven call sites are gated through the predicate
 * rather than through rendered output — stated plainly, not implied. */
static void test_count_zero_row_visible(struct pgwt_server *srv, uint64_t w0)
{
    printf("--- 12. a row with time and no completed occurrence is shown "
           "---\n");

    CHECK(pgwt_row_is_empty(0, 0) == 1,
          "12a. a row with neither time nor occurrences is still suppressed "
          "-- the fix is 'empty in BOTH senses', not 'show everything'");
    CHECK(pgwt_row_is_empty(0, 5 * ONE_SEC) == 0,
          "12a. a row with 5 s of right-open time and count 0 is SHOWN -- "
          "its milliseconds are already in DB Time, so suppressing it makes "
          "the breakdown unable to explain the total");
    CHECK(pgwt_row_is_empty(3, 0) == 0,
          "12a. and a row with occurrences but no time is shown too (the "
          "zero-duration transition case that #347 was about)");

    /* [20,30] of the main fixture: the straddler covers it end to end and
     * closes at 35.5, so Lock has 10 s and no completed wait at all. */
    struct pgwt_request req;
    memset(&req, 0, sizeof(req));
    req.id = 3;
    req.from_ns = w0 + 20 * ONE_SEC;
    req.to_ns   = w0 + 30 * ONE_SEC;
    req.limit = 20;
    cJSON *out = call_handler(handle_top_events, srv, &req);
    CHECK(out != NULL, "12b. handle_top_events produced parseable JSON");
    if (!out) return;
    cJSON *rows = cJSON_GetObjectItem(out, "rows");
    cJSON *row, *found = NULL;
    cJSON_ArrayForEach(row, rows) {
        cJSON *id = cJSON_GetObjectItem(row, "event_id");
        if (id && (uint32_t)id->valuedouble == EV_LOCK)
            found = row;
    }
    CHECK(found != NULL,
          "12b. the served response HAS a Lock row for a window whose only "
          "Lock time is a wait still running at both edges -- without it "
          "the UI shows DB Time it cannot break down");
    if (found) {
        cJSON *c = cJSON_GetObjectItem(found, "count");
        cJSON *ms = cJSON_GetObjectItem(found, "total_ms");
        CHECK(c && c->valuedouble == 0,
              "12b. and its count is 0, PRESENT not omitted (%.0f) -- the "
              "occurrence belongs to the window where it closes",
              c ? c->valuedouble : -1.0);
        CHECK(ms && near_ms(ms->valuedouble, 10000.0),
              "12b. and its time is the clipped 10000.0 ms (got %.1f)",
              ms ? ms->valuedouble : -1.0);
    }
    cJSON_Delete(out);
}

/* ── 13. The block LOOKUP, where the truncation flag was bypassed (#429)
 *
 * `block_index[]` is appended one entry per flushed block, in COMMIT order,
 * and nothing sorts it or checks that it is sorted. The loader
 * binary-searched it for a scan start and then iterated forward until a
 * block began after the right edge. Both halves are unsound: the search has
 * no precondition on commit-ordered data, and `lo - 1` assumes blocks do not
 * overlap in time — a SAMPLES block batches ~1 s of ticks across several
 * TRANSITIONS blocks, so a block two or more positions back can still be
 * inside the window.
 *
 * This is in #424's scope rather than a follow-up because it is a FALSE
 * NEGATIVE for right_edge_truncated: a straddler in a block the scan never
 * visits is missing from the answer AND unflagged, so the window is short
 * while reporting itself complete. "Must say when it could not" is only as
 * sound as the lookup underneath it.
 *
 * REACH, not overstated: the inversion needs two streams in one file plus a
 * drain lag longer than the ~1 s sample batch (tiered/escalating, or full
 * mode with the sampler, under load). It has NOT been observed on a live
 * capture — only in a writer-API reproduction, which is what this fixture
 * is. Severity when it does occur is total: an empty window with no flag.
 *
 * THE FIXTURE ASSERTS ITS OWN PROPERTY FIRST (§13a). A fixture where commit
 * order and timestamp order coincide passes while the code is broken, and if
 * the writer is ever changed to sort, §13a goes RED and says so instead of
 * the gate quietly losing its teeth. §13e is the sorted control: the same
 * assertions on the ordinary fixture, where the property is absent.
 */
#define IL_SAMPLE_TICKS 12
#define IL_PERIOD       (100 * MS)
#define IL_STRAD_PID    801

/* Transitions lag the sampler, which is what a loaded capture does: the
 * writer flushes whichever pending buffer holds the OLDER first timestamp,
 * so each round commits [TRANSITIONS at ~base+10 s, SAMPLES at base+60 s+],
 * and the next round's TRANSITIONS block commits after a SAMPLES block whose
 * first timestamp is 50 s in its future. */
static void build_interleaved_fixture(const char *dir, uint64_t base)
{
    struct pgwt_event_writer writer;
    if (pgwt_writer_init(&writer, dir, 180000, 0, NULL) != 0) {
        printf("  FAIL: trace writer init (interleaved)\n");
        tests_failed++;
        return;
    }
    for (int round = 0; round < 3; round++) {
        if (round == 0) {
            for (int i = 0; i < 20; i++)
                push(&writer, base + 10 * ONE_SEC + (uint64_t)i * MS,
                     FILLER_PID, 0, EV_IO, 1 * MS);
        } else if (round == 1) {
            for (int i = 0; i < 20; i++)
                push(&writer, base + 11 * ONE_SEC + (uint64_t)i * MS,
                     FILLER_PID, 0, EV_IO, 1 * MS);
        } else {
            /* The straddler: closes at 13 s after running 2 s, so it is
             * inside a window ending at 12 s — and it lives in a block
             * committed after two far-future SAMPLES blocks. */
            push(&writer, base + 13 * ONE_SEC, IL_STRAD_PID,
                 EV_LOCK, EV_IO, 2 * ONE_SEC);
        }
        struct pgwt_trace_event smp[IL_SAMPLE_TICKS];
        for (int i = 0; i < IL_SAMPLE_TICKS; i++) {
            memset(&smp[i], 0, sizeof(smp[i]));
            smp[i].timestamp_ns = base + 60 * ONE_SEC
                                + (uint64_t)round * 2 * ONE_SEC
                                + (uint64_t)i * IL_PERIOD;
            smp[i].pid = FILLER_PID;
            smp[i].old_event = 0;
            smp[i].new_event = EV_IO;
            smp[i].cpu_ns = PGWT_CPU_NS_UNKNOWN;
        }
        if (pgwt_writer_push_samples(&writer, smp, IL_SAMPLE_TICKS,
                                     IL_PERIOD) != 0) {
            printf("  FAIL: push_samples (interleaved)\n");
            tests_failed++;
            break;
        }
    }
    pgwt_writer_close(&writer);
}

/* The fixture's own property: is block_index[] ascending in first
 * timestamp? Returns the first inversion index, or -1 when sorted. */
static int first_index_inversion(const char *dir, int *n_blocks)
{
    struct pgwt_trace_file_entry files[16];
    int nf = pgwt_scan_trace_files(dir, files, 16);
    if (nf <= 0) return -2;
    struct pgwt_event_reader r;
    if (pgwt_reader_open(&r, files[0].path) != 0) return -2;
    if (n_blocks) *n_blocks = r.num_blocks;
    int inv = -1;
    for (int b = 1; b < r.num_blocks; b++)
        if (r.block_index[b].timestamp_ns < r.block_index[b - 1].timestamp_ns) {
            inv = b;
            break;
        }
    pgwt_reader_close(&r);
    return inv;
}

static void test_block_lookup(const char *dir_il, const char *dir_sorted)
{
    printf("--- 13. the block lookup must not skip a block (#429) ---\n");

    /* 13a. PRECONDITION. Without this the rest of the section could pass on
     * a fixture whose two orders agree, i.e. while the defect is live. */
    int nb = 0;
    int inv = first_index_inversion(dir_il, &nb);
    CHECK(inv >= 1,
          "13a. the fixture's block index is NOT ascending: %d blocks, "
          "first inversion at block %d. Commit order and timestamp order "
          "genuinely disagree -- if the writer is ever changed to sort, "
          "THIS check goes red rather than the gate going quiet",
          nb, inv);
    int inv_sorted = first_index_inversion(dir_sorted, NULL);
    CHECK(inv_sorted == -1,
          "13a. and the ordinary fixture IS ascending (inversion=%d), so "
          "the property above is a property of the fixture, not of the "
          "reader", inv_sorted);
    if (inv < 1) return;

    struct pgwt_server *srv = calloc(1, sizeof(*srv));
    if (!srv || server_init(srv, dir_il) != 0) {
        CHECK(0, "13. server_init on the interleaved fixture");
        return;
    }
    uint64_t w0 = srv->earliest_wall_ns - 10 * ONE_SEC;

    /* 13b. The sampled region lives in ONE block, which sits BEFORE the
     * block a binary search over this index lands on. */
    struct win s;
    win_load(srv, &s, w0 + 60 * ONE_SEC, w0 + 61 * ONE_SEC);
    int n_sampled = 0;
    for (int i = 0; i < s.count; i++)
        if (s.events[i].flags & PGWT_EVENT_FLAG_SAMPLE)
            n_sampled++;
    CHECK(n_sampled > 0,
          "13b. a window over the sampled region returns its ticks (%d of "
          "%d records) -- the binary search landed past this block and the "
          "old scan answered the window with NOTHING",
          n_sampled, s.count);
    /* The pair is the point. On the old scan this window came back EMPTY
     * with right_edge_truncated == 0: short, and SILENT about it. Here the
     * answer is complete, so the same 0 means "nothing was cut". Read the
     * two checks together in the red log — that is the false negative. */
    CHECK(!s.info.right_edge_truncated,
          "13b. and it is not flagged truncated (%d) -- which on the old "
          "scan was a LIE about an empty window, and here is the truth "
          "about a complete one", s.info.right_edge_truncated);
    win_free(&s);

    /* 13c. The straddler sits in a block committed AFTER a SAMPLES block
     * whose first timestamp is far in the future. With a 1 s horizon the
     * scan edge is below that SAMPLES block, so the old loop BROKE there and
     * threw away the in-window blocks behind it. */
    int fds[2];
    if (pipe(fds) != 0) { CHECK(0, "13c. pipe"); goto out; }
    pid_t pid = fork();
    if (pid < 0) { CHECK(0, "13c. fork"); goto out; }
    if (pid == 0) {
        close(fds[0]);
        setenv("PGWT_STRADDLE_LOOKAHEAD_NS", "2000000000", 1);  /* 2 s */
        struct pgwt_server *c = calloc(1, sizeof(*c));
        int r[2] = {0, 0};
        if (c && server_init(c, dir_il) == 0) {
            uint64_t cw0 = c->earliest_wall_ns - 10 * ONE_SEC;
            struct win w;
            win_load(c, &w, cw0 + 11 * ONE_SEC, cw0 + 12 * ONE_SEC);
            r[0] = w.info.straddlers_admitted;
            r[1] = w.info.right_edge_truncated;
            win_free(&w);
        }
        ssize_t n = write(fds[1], r, sizeof(r));
        (void)n;
        close(fds[1]);
        _exit(0);
    }
    close(fds[1]);
    int r[2] = {-1, -1};
    ssize_t got = read(fds[0], r, sizeof(r));
    close(fds[0]);
    int status = 0;
    waitpid(pid, &status, 0);
    if (got == (ssize_t)sizeof(r)) {
        CHECK(r[0] == 1,
              "13c. the straddler in a late-committed block IS found "
              "(admitted=%d) even though a far-future SAMPLES block sits "
              "between it and the window -- a block that cannot matter says "
              "nothing about the blocks after it", r[0]);
    } else { CHECK(0, "13c. child failed"); }

    /* 13d. And when the straddler genuinely cannot be reached, the flag
     * fires -- the bypass this section exists to close. A 1 ms horizon
     * cannot reach a close 1 s past the edge. */
    int fds2[2];
    if (pipe(fds2) != 0) { CHECK(0, "13d. pipe"); goto out; }
    pid_t pid2 = fork();
    if (pid2 < 0) { CHECK(0, "13d. fork"); goto out; }
    if (pid2 == 0) {
        close(fds2[0]);
        setenv("PGWT_STRADDLE_LOOKAHEAD_NS", "1000000", 1);     /* 1 ms */
        struct pgwt_server *c = calloc(1, sizeof(*c));
        int r2[2] = {0, 0};
        if (c && server_init(c, dir_il) == 0) {
            uint64_t cw0 = c->earliest_wall_ns - 10 * ONE_SEC;
            struct win w;
            win_load(c, &w, cw0 + 11 * ONE_SEC, cw0 + 12 * ONE_SEC);
            r2[0] = w.info.straddlers_admitted;
            r2[1] = w.info.right_edge_truncated;
            win_free(&w);
        }
        ssize_t n = write(fds2[1], r2, sizeof(r2));
        (void)n;
        close(fds2[1]);
        _exit(0);
    }
    close(fds2[1]);
    int r2[2] = {-1, -1};
    ssize_t got2 = read(fds2[0], r2, sizeof(r2));
    close(fds2[0]);
    waitpid(pid2, &status, 0);
    if (got2 == (ssize_t)sizeof(r2)) {
        CHECK(r2[0] == 0 && r2[1] == 1,
              "13d. with a 1 ms horizon the straddler is out of reach "
              "(admitted=%d) and the load SAYS SO (truncated=%d) -- on the "
              "old scan this came back short with 0 flags, which is the "
              "false negative #429 is about", r2[0], r2[1]);
    } else { CHECK(0, "13d. child failed"); }

out:
    server_destroy(srv);
    free(srv);
}

/* ── 14. INSTANT selection has TWO bounds ─────────────────────────────
 *
 * The first cut of the per-occurrence gate implemented only the upper half
 * (`!event_right_open(...)`), so a record whose close lies BEFORE `from`
 * still passed it and was counted. The rule had always been stated as
 * `timestamp_ns in [from, to]`; only half of it was code.
 *
 * Every section above went through the loader, which rejects `ts < from_m`
 * before compute ever sees the array — so the server path covered for the
 * missing bound and this suite could not see it. tests/test_window_clip.c
 * section 6 calls pgwt_compute_top_events() DIRECTLY with hand-built
 * arrays, and failed three ways on the Linux box (a sweep counting one
 * record twice, a zero-width window reporting one occurrence, and a record
 * wholly before `from` reporting one). This section builds its arrays the
 * same way, so the two suites now assert the same contract instead of one
 * covering for the other.
 *
 * 14d is the counterweight: INCLUSIVE at `to` must survive the fix. Making
 * selection half-open at the end is the deferred #427 migration, and it
 * must not arrive as a side effect of adding a lower bound. */
static void test_instant_two_bounds(void)
{
    printf("--- 14. per-occurrence selection is [from, to], both ends ---\n");
    struct pgwt_filter f;
    memset(&f, 0, sizeof(f));

    /* One record, closing at 15 s after a 1 s wait. No RIGHT_OPEN flag and
     * no loader anywhere: compute must gate on its own. */
    struct pgwt_trace_event ev;
    memset(&ev, 0, sizeof(ev));
    ev.timestamp_ns = 15 * ONE_SEC;
    ev.pid = INSIDE_PID;
    ev.old_event = EV_LOCK;
    ev.new_event = EV_IO;
    ev.duration_ns = 1 * ONE_SEC;
    ev.query_id = 0x4242;
    ev.cpu_ns = PGWT_CPU_NS_UNKNOWN;

    struct pgwt_events_result res;
    const struct pgwt_event_row *row;

    /* 14a. A window that OPENS AFTER the record closed. */
    pgwt_compute_top_events(&ev, 1, &f, 20 * ONE_SEC, 30 * ONE_SEC,
                            10000.0, &res);
    row = row_for(&res, EV_LOCK);
    CHECK(row == NULL || row->count == 0,
          "14a. a record closing 5 s BEFORE `from` is not an occurrence of "
          "this window (count=%llu) -- the upper-bound-only gate counted it",
          (unsigned long long)(row ? row->count : 0));
    free(res.rows);

    /* 14b. The sweep: two adjacent windows, one record, one occurrence
     * TOTAL. The record is strictly interior to the first window, so this
     * is not a shared-boundary question. */
    uint64_t total = 0;
    pgwt_compute_top_events(&ev, 1, &f, 10 * ONE_SEC, 16 * ONE_SEC,
                            6000.0, &res);
    row = row_for(&res, EV_LOCK);
    total += row ? row->count : 0;
    free(res.rows);
    pgwt_compute_top_events(&ev, 1, &f, 16 * ONE_SEC, 22 * ONE_SEC,
                            6000.0, &res);
    row = row_for(&res, EV_LOCK);
    total += row ? row->count : 0;
    free(res.rows);
    CHECK(total == 1,
          "14b. one record swept by two adjacent windows is counted ONCE "
          "(got %llu) -- 2 is the same defect seen as a double count",
          (unsigned long long)total);

    /* 14c. A zero-width window after the close. */
    pgwt_compute_top_events(&ev, 1, &f, 20 * ONE_SEC, 20 * ONE_SEC,
                            0.0, &res);
    row = row_for(&res, EV_LOCK);
    CHECK(row == NULL || row->count == 0,
          "14c. a zero-width window at 20 s holds no occurrence of a record "
          "that closed at 15 s (count=%llu)",
          (unsigned long long)(row ? row->count : 0));
    free(res.rows);

    /* 14d. INCLUSIVE at `to`, unchanged. A record closing exactly at the
     * right edge lies wholly inside the window and is ONE occurrence. */
    pgwt_compute_top_events(&ev, 1, &f, 10 * ONE_SEC, 15 * ONE_SEC,
                            5000.0, &res);
    row = row_for(&res, EV_LOCK);
    CHECK(row && row->count == 1,
          "14d. a record closing exactly AT `to` is still one occurrence "
          "(count=%llu) -- adding the lower bound must not quietly make "
          "selection half-open at the end (that is #427, deferred)",
          (unsigned long long)(row ? row->count : 0));
    free(res.rows);

    /* 14e. And exactly AT `from` too — the bound is >=, not >. */
    pgwt_compute_top_events(&ev, 1, &f, 15 * ONE_SEC, 25 * ONE_SEC,
                            10000.0, &res);
    row = row_for(&res, EV_LOCK);
    CHECK(row && row->count == 1,
          "14e. a record closing exactly AT `from` is one occurrence "
          "(count=%llu) -- the new bound is inclusive, not an off-by-one "
          "that drops the boundary record",
          (unsigned long long)(row ? row->count : 0));
    free(res.rows);

    /* 14f. The SECOND gated site: top_queries keeps its own count and
     * full_ns, so it needs its own leg rather than inheriting trust. */
    struct pgwt_queries_result qres;
    pgwt_compute_top_queries(&ev, 1, &f, 20 * ONE_SEC, 30 * ONE_SEC,
                             10000.0, &qres);
    uint64_t qcount = 0;
    for (int i = 0; i < qres.num_rows; i++)
        if (qres.rows[i].query_id == 0x4242)
            qcount = qres.rows[i].count;
    CHECK(qcount == 0,
          "14f. top_queries applies the same lower bound (count=%llu for a "
          "query whose only record closed before `from`)",
          (unsigned long long)qcount);
    free(qres.rows);
}

/* ── 15. A TIERED capture: the straddler budget is for TRANSITIONS ────
 *
 * `only_straddlers` was `first_ts > sample_to_mono` with no block-type
 * test, so a SAMPLES block anywhere in the straddler horizon (up to 300 s
 * past `to`) was charged against PGWT_STRADDLE_MAX_BLOCKS — even though the
 * record loop discards every record in it, because the sample lookahead has
 * its own, narrower edge.
 *
 * On a tiered capture, where SAMPLES blocks dominate, that is not a
 * slowdown: the budget is spent on blocks that CANNOT hold a straddler,
 * right_edge_truncated fires falsely, and the TRANSITIONS blocks that do
 * hold straddlers are skipped. The answer is short AND the honest-truncation
 * signal is consumed by blocks that were never candidates — #424 defeated
 * while reporting itself as a clean refusal.
 *
 * THE FIXTURE MUST BE TIERED OR IT PROVES NOTHING. Three SAMPLES blocks sit
 * between the window and the TRANSITIONS block holding the straddler, and
 * the budget is set to 2 — fewer than the SAMPLES blocks in the way. A
 * fixture with no SAMPLES blocks in the horizon passes while the defect is
 * live, so §15a asserts the fixture really is tiered before anything else.
 */
#define TIER_PERIOD   (100 * MS)
#define TIER_TICKS    12
#define TIER_STRAD_PID 901

static void build_tiered_fixture(const char *dir, uint64_t base)
{
    struct pgwt_event_writer writer;
    if (pgwt_writer_init(&writer, dir, 180000, 0, NULL) != 0) {
        printf("  FAIL: trace writer init (tiered)\n");
        tests_failed++;
        return;
    }
    /* ANCHOR, and it is load-bearing: run_bypass() derives the window
     * origin as `earliest_wall_ns - 10 ms`, so a fixture whose first record
     * is anywhere else silently shifts every window this section asks for.
     * The first version of this fixture started at base + 10 s, which moved
     * the window on top of the SAMPLES blocks — where they are inside the
     * sample edge, are never charged to the straddler budget, and the
     * section passed on the UNFIXED tree. A gate that cannot reach the
     * defect is worse than no gate; this one line is what reaches it. */
    push(&writer, base + 10 * MS, FILLER_PID, 0, EV_IO, 1 * MS);

    /* Transitions inside the window. */
    for (int i = 0; i < 20; i++)
        push(&writer, base + 10 * ONE_SEC + (uint64_t)i * MS,
             FILLER_PID, 0, EV_IO, 1 * MS);

    /* Three SAMPLES blocks at 20 s, 22 s, 24 s -- inside the straddler
     * horizon, past the sample edge, holding nothing this window can use.
     * The first push also flushes the pending TRANSITIONS buffer (older
     * first timestamp), which is what commits the block above. */
    for (int round = 0; round < 3; round++) {
        struct pgwt_trace_event smp[TIER_TICKS];
        for (int i = 0; i < TIER_TICKS; i++) {
            memset(&smp[i], 0, sizeof(smp[i]));
            smp[i].timestamp_ns = base + 20 * ONE_SEC
                                + (uint64_t)round * 2 * ONE_SEC
                                + (uint64_t)i * TIER_PERIOD;
            smp[i].pid = FILLER_PID;
            smp[i].old_event = 0;
            smp[i].new_event = EV_IO;
            smp[i].cpu_ns = PGWT_CPU_NS_UNKNOWN;
        }
        if (pgwt_writer_push_samples(&writer, smp, TIER_TICKS,
                                     TIER_PERIOD) != 0) {
            printf("  FAIL: push_samples (tiered)\n");
            tests_failed++;
            break;
        }
    }
    /* The straddler, BEHIND all three SAMPLES blocks: closes at 30 s after
     * running 20 s, so it covers the window at [10.5, 12]. */
    push(&writer, base + 30 * ONE_SEC, TIER_STRAD_PID,
         EV_LOCK, EV_IO, 20 * ONE_SEC);
    pgwt_writer_close(&writer);
}

/* How many SAMPLES blocks sit strictly between two mono timestamps. */
static int count_sample_blocks_between(const char *dir, uint64_t lo,
                                       uint64_t hi, int *n_blocks)
{
    struct pgwt_trace_file_entry files[16];
    int nf = pgwt_scan_trace_files(dir, files, 16);
    if (nf <= 0) return -1;
    struct pgwt_event_reader r;
    if (pgwt_reader_open(&r, files[0].path) != 0) return -1;
    if (n_blocks) *n_blocks = r.num_blocks;
    int n = 0;
    for (int b = 0; b < r.num_blocks; b++) {
        struct pgwt_block_info bi;
        if (pgwt_reader_block_info(&r, b, &bi) != 0) continue;
        if (bi.block_type == PGWT_BLOCK_SAMPLES &&
            bi.first_timestamp_ns > lo && bi.first_timestamp_ns < hi)
            n++;
    }
    pgwt_reader_close(&r);
    return n;
}

static void test_tiered_budget(const char *dir_t, uint64_t base)
{
    printf("--- 15. a tiered capture: SAMPLES blocks must not spend the "
           "straddler budget ---\n");

    /* 15a. PRECONDITION: the fixture really is tiered, with more SAMPLES
     * blocks in the way than the budget allows. */
    int nb = 0;
    int ns_blocks = count_sample_blocks_between(dir_t, base + 15 * ONE_SEC,
                                                base + 29 * ONE_SEC, &nb);
    CHECK(ns_blocks >= 3,
          "15a. %d SAMPLES blocks sit between the window and the "
          "straddler's block (%d blocks total), more than the 2-block "
          "budget below -- without this the section would pass on a "
          "capture that never exercises the defect", ns_blocks, nb);
    if (ns_blocks < 3) return;

    struct bypass_result r;
    /* Budget 2, i.e. fewer than the SAMPLES blocks in the way. Before the
     * fix they ate it; now only TRANSITIONS blocks can. */
    if (run_bypass(dir_t, 0, "PGWT_STRADDLE_MAX_BLOCKS", "2",
                   &r, 10, 12) == 0) {
        CHECK(r.admitted == 1,
              "15b. the straddler BEHIND three SAMPLES blocks is still "
              "found with a 2-block budget (admitted=%d) -- the budget is "
              "for blocks that could hold one", r.admitted);
        CHECK(r.truncated == 0,
              "15c. and nothing is reported truncated (%d): skipping "
              "SAMPLES blocks costs this window nothing, so claiming a cut "
              "right edge would be a false alarm on every tiered capture",
              r.truncated);
        CHECK(r.lock_row_present && r.lock_total_ms > 1000.0,
              "15d. its time arrives too (%.1f ms of Lock) -- admitted "
              "without contributing would be the same loss with a "
              "different counter", r.lock_total_ms);
    } else { CHECK(0, "15. bypass child failed"); }
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
    test_truncation_surfaced(dir);

    char dirc[] = "/tmp/pgwt_wovl_cmd_XXXXXX";
    if (!mkdtemp(dirc)) {
        printf("FAIL: mkdtemp (section 11)\n");
        return 1;
    }
    build_cmd_fixture(dirc, base);
    test_skipped_file_bypass();
    test_concurrency_overlap(srv, w0);
    test_node_totals(srv, w0);
    test_clipped_majority_moves_db_time(dirc);
    test_count_zero_row_visible(srv, w0);

    char dir_il[] = "/tmp/pgwt_wovl_il_XXXXXX";
    if (!mkdtemp(dir_il)) {
        printf("FAIL: mkdtemp (section 13)\n");
        return 1;
    }
    build_interleaved_fixture(dir_il, base);
    test_block_lookup(dir_il, dir);
    test_instant_two_bounds();

    char dir_t[] = "/tmp/pgwt_wovl_tier_XXXXXX";
    if (!mkdtemp(dir_t)) {
        printf("FAIL: mkdtemp (section 15)\n");
        return 1;
    }
    build_tiered_fixture(dir_t, base);
    test_tiered_budget(dir_t, base);

    printf("\n%d checks, %d failed\n", tests_run, tests_failed);
    return tests_failed ? 1 : 0;
}
