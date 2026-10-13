/* test_io_worker_summary.c — io_workers must be OUTSIDE the summary model too
 * (issues #315 and #376, summary format v6).
 *
 * THE BUG. docs/AAS_SEMANTICS_DECISION.md records that io_worker time is
 * excluded from DB Time AND from Idle, and surfaced instead as an io_worker
 * busy %: the requesting backend already shows AioIoCompletion for the same
 * logical I/O, so counting the worker double-counts it. src/compute.c
 * implements that on every raw path. src/summary_writer.c did not, so the
 * same window answered differently depending on which path served it --
 * +400% on DB Time on a synthetic probe (#315) and, on the real PG18 demo
 * capture, +23.36% on the whole-capture Idle total / +308.16% on a window
 * containing the capture end (measured, tests/results/issue376-measurement).
 *
 * DIVISION OF LABOUR WITH tests/test_io_worker_parity.c, which landed first
 * as the EXPECTED-RED gate for the same two issues. That file owns the
 * raw-vs-summary comparison itself: three windows, an acceptance tier over
 * the real PG18 capture, handle_heatmap's two routes cell by cell, and the
 * refusals that keep an unresolvable base from approving itself. Read it for
 * the verdict on the numbers.
 *
 * THIS file owns the three things a comparison of compute outputs cannot see:
 *   (1) the RECORDS -- that io_worker time is absent from each of class_ns,
 *       idle_ns, events[] (with its histograms), sessions[] and queries[],
 *       and that the writer COUNTED what it dropped. A read-side subtraction
 *       or a half-exclusion can produce the same totals for one window and a
 *       wrong per-event breakdown for the next;
 *   (2) the PRODUCERS -- that src/event_stream.c and src/escalation.c
 *       actually resolve a category before pushing (section 4). Every
 *       writer-level test, including the parity gate, resolves the category
 *       ITSELF and hands it to the writer, so a daemon producer passing 0
 *       leaves all of them green. That is the gap the fix for #376 was
 *       largely about: the resolve used to sit 65 lines BELOW the push;
 *   (3) a synthetic CAPTURE-END FLUSH record, so the shape that carries the
 *       bulk of the divergence is covered without the 26 MB capture.
 * Section 2 keeps a small raw-vs-summary comparison of its own, as the
 * non-vacuity anchor for (1) and (3) -- the same run shows agreement with a
 * resolved category and divergence with cat_flag 0, which is what makes the
 * record-level assertions above mean something. It is NOT the authority on
 * parity; test_io_worker_parity is.
 *
 * WHY NEITHER OF THESE IS tests/test_agg_raw_crosscheck.c, WHICH ALSO
 * COMPARES THE TWO PATHS: that gate's fixture is deliberately io_worker-FREE
 * (its hole N7), precisely so that nobody closes #315 by pre-flagging an
 * event:
 * `evt->flags` carries no category bit on any path that feeds the writer, so
 * a `flags & PGWT_EVENT_FLAG_IO_WORKER` check inside the writer is DEAD IN
 * THE DAEMON and a pre-flagged input greens it while testing an input shape
 * production never produces. So this file:
 *
 *   - pushes UNFLAGGED events and lets the PRODUCER's resolved category be
 *     the only thing that can cause the exclusion (asserted: section 0
 *     refuses any fixture whose events carry a category bit);
 *   - covers BUSY and IDLE io_worker records, because the raw path treats
 *     them differently (busy time goes to cat_ms[io_worker], idle time is
 *     dropped) and a half-exclusion would pass a test that only had one;
 *   - covers the CAPTURE-END FLUSH shape (one long interval closing in the
 *     last second), which is where the divergence peaks and which
 *     src/escalation.c produces by a path that never touches
 *     src/event_stream.c;
 *   - compares time_model AND the heatmap, since events[] carries the
 *     histograms the summary heatmap is built from;
 *   - checks that BOTH producers actually resolve a category, by reading
 *     their source (section 4) -- the daemon itself needs BPF and cannot be
 *     linked here, and the one thing a writer-level test cannot see is a
 *     producer that passes 0;
 *   - and carries a bypass suite (section 5) for every way these checks can
 *     be satisfied without checking anything.
 *
 * WHAT IS DELIBERATELY NOT CLAIMED. The writer buckets a long interval WHOLE
 * into the second in which it closes, while the raw path clips it to the
 * requested window (event_window_ns). That straddler mechanism is general --
 * a long Client:ClientRead behaves identically -- and it is NOT fixed here.
 * So no assertion in this file says "summary equals raw on any window": the
 * equality cases are built from intervals that do not straddle a second
 * boundary, and the capture-end case asserts what was EXCLUDED rather than a
 * global equality. On the real capture, the io_worker component of the
 * capture-end divergence went from +5,768,521.72 ms to 0 while the
 * straddler component (+215,388.03 ms, non-io_worker idle closing at capture
 * end) stayed -- measured, not assumed.
 *
 * TIMING. Every duration and spacing is a literal; the only clock read is the
 * monotonic origin (same convention as tests/test_idle_summary.c, and for the
 * same reason: records must land inside their own file's wall hour or the
 * reader's file filter misses them and every comparison passes on 0 == 0).
 * No assertion depends on its value, and the compared window is derived from
 * the records that were actually written.
 */
#include "compute.h"
#include "summary_writer.h"
#include "summary_reader.h"
#include "wait_event.h"
#include "idle_rule.h"
#include "pg_wait_tracer.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <math.h>
#include <unistd.h>
#include <sys/stat.h>
#include <time.h>

static int tests_run, tests_failed;
/* Section 5 runs the real checks against crafted bad inputs and asserts they
 * go red. Their red output is EXPECTED, so it is labelled: a log full of
 * unexplained FAIL lines that still exits 0 is its own kind of lie. */
static int expect_fail;

#define CHECK(cond, fmt, ...) do {                                        \
    tests_run++;                                                          \
    if (!(cond)) { tests_failed++;                                        \
        printf("  %s(line %d): " fmt "\n",                                \
               expect_fail ? "red-as-designed" : "FAIL",                  \
               __LINE__, ##__VA_ARGS__); }                                \
} while (0)

/* Same derivation as tests/test_idle_summary.c: both paths sum the same
 * integer nanoseconds in a different order, so 0.01 ms over a 150 s window
 * (7e-8 relative) is float noise, not a budget. A single misaccounted 1 ms
 * interval still fails. */
#define TOL_MS 0.01
#define NEAR_MS(a, b) (fabs((double)(a) - (double)(b)) <= TOL_MS)

#define MS      1000000ULL
#define ONE_SEC 1000000000ULL

#define EV_LOCK        WEI(PG_WAIT_LOCK, 0)
#define EV_CLIENTREAD  WEI(PG_WAIT_CLIENT, 0)     /* idle, visible */
#define EV_IOWORKERMAIN WEI(PG_WAIT_ACTIVITY, 6)  /* io_worker's own idle wait */
#define EV_DATAFILEREAD WEI(PG_WAIT_IO, 21)       /* io_worker BUSY time */
#define EV_BGWRITERMAIN WEI(PG_WAIT_ACTIVITY, 3)  /* a BACKGROUND pid's idle */

#define PID_CLIENT    101
#define PID_IO_WORKER 102
#define PID_BG        104         /* bgwriter: BACKGROUND, NOT excluded */
#define QID_A 0x1111111111111111ULL

#define SECS 150        /* > the 120 s floor should_use_summaries applies */

static char g_base[300];

static const char *fresh_dir(const char *name)
{
    static char buf[600];
    snprintf(buf, sizeof(buf), "%s/%s", g_base, name);
    char cmd[700];
    snprintf(cmd, sizeof(cmd), "rm -rf '%s'", buf);
    if (system(cmd) != 0) { /* ignore */ }
    mkdir(buf, 0755);
    return buf;
}

static uint64_t mono_origin(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    uint64_t mono = (uint64_t)ts.tv_sec * ONE_SEC + (uint64_t)ts.tv_nsec;
    return (mono / ONE_SEC) * ONE_SEC;
}

/* The pid → category table, exactly the shape src/server.c builds from
 * backends.jsonl and the daemon's pgwt_live_pid_cat_flag returns. */
static const struct pgwt_pid_cat CATS[] = {
    { PID_CLIENT,    0 },
    { PID_IO_WORKER, PGWT_EVENT_FLAG_IO_WORKER },
    /* THE OVER-BROAD-EXCLUSION CONTROL. src/compute.c special-cases ONLY
     * PGWT_EVENT_FLAG_IO_WORKER: a checkpointer/walwriter/bgwriter/autovacuum
     * record keeps its time in DB Time and in Idle on every raw path. So a
     * writer that excluded on `cat_flag != 0` instead of on the io_worker bit
     * would silently delete all maintenance and background time from every
     * window served from summaries -- the demo's default 900 s Overview --
     * while raw kept it. With no such pid in the fixture that mutation is
     * invisible (measured: 58/0 green). This pid is here so it is not. */
    { PID_BG,        PGWT_EVENT_FLAG_BACKGROUND },
};
#define N_CATS ((int)(sizeof(CATS)/sizeof(CATS[0])))

static uint32_t cat_of(uint32_t pid)
{
    for (int i = 0; i < N_CATS; i++)
        if (CATS[i].pid == pid) return CATS[i].flag;
    return 0;
}

/* ── the stream ────────────────────────────────────────────────────────────
 * Per second, each interval ending 100+ ms into its own second so nothing
 * straddles a boundary (a straddler would make raw and summary legitimately
 * differ and turn an equality into a tolerance argument):
 *
 *   pid 101 client    Lock            20 ms   DB Time
 *   pid 101 client    ClientRead      25 ms   Idle
 *   pid 104 bgwriter  Lock            12 ms   DB Time  -> MUST SURVIVE
 *   pid 104 bgwriter  BgwriterMain    30 ms   Idle     -> MUST SURVIVE
 *   pid 102 io_worker IoWorkerMain    40 ms   io_worker IDLE  -> excluded
 *   pid 102 io_worker IO:DataFileRead 10 ms   io_worker BUSY  -> excluded
 *
 * per second: DB Time 32 ms, Idle 55 ms, io_worker 50 ms
 * over 150 s: DB Time 4800 ms, Idle 8250 ms, io_worker 7500 ms
 *
 * The background pid is the control for an OVER-BROAD exclusion (see CATS):
 * its two records carry one busy and one idle event through the same five
 * accumulators the io_worker records are removed from, and every assertion
 * below checks them PRESENT in the same breath as it checks the io_worker's
 * absent. "Excluded" and "excluded too much" are different bugs and this
 * fixture can tell them apart.
 */
struct spec { uint32_t pid; uint32_t ev; uint64_t ms; uint64_t qid; };
static const struct spec SLICE[] = {
    { PID_CLIENT,    EV_LOCK,         20, QID_A },
    { PID_CLIENT,    EV_CLIENTREAD,   25, QID_A },
    { PID_BG,        EV_LOCK,         12, 0     },
    { PID_BG,        EV_BGWRITERMAIN, 30, 0     },
    { PID_IO_WORKER, EV_IOWORKERMAIN, 40, 0     },
    { PID_IO_WORKER, EV_DATAFILEREAD, 10, 0     },
};
#define SLICE_N ((int)(sizeof(SLICE)/sizeof(SLICE[0])))
#define MAX_EV  (SECS * SLICE_N + 8)

/* Kept records per second (everything but the two io_worker ones) and the
 * distinct sessions they come from. */
#define KEPT_PER_SEC 4
#define SESSIONS_PER_SEC 2

#define EXP_LOCK_MS (SECS * (20.0 + 12.0))   /* client + background */
#define EXP_DB_MS   (SECS * (20.0 + 12.0))
#define EXP_IDLE_MS (SECS * (25.0 + 30.0))   /* ClientRead + BgwriterMain */
#define EXP_BG_IDLE_MS (SECS * 30.0)
#define EXP_IW_MS   (SECS * 50.0)

static int build_stream(struct pgwt_trace_event *out, uint64_t base)
{
    int n = 0;
    for (int s = 0; s < SECS; s++) {
        uint64_t sec = base + (uint64_t)s * ONE_SEC;
        for (int i = 0; i < SLICE_N; i++) {
            struct pgwt_trace_event e;
            memset(&e, 0, sizeof(e));
            e.timestamp_ns = sec + 100 * MS + (uint64_t)(i + 1) * MS;
            e.pid          = SLICE[i].pid;
            e.old_event    = SLICE[i].ev;
            e.new_event    = EV_LOCK;        /* never EXIT, never a marker */
            e.duration_ns  = SLICE[i].ms * MS;
            e.query_id     = SLICE[i].qid;
            e.cpu_ns       = PGWT_CPU_NS_UNKNOWN;
            e.flags        = 0;              /* NO category bit: see section 0 */
            out[n++] = e;
        }
    }
    return n;
}

/* ── write through the REAL writer ─────────────────────────────────────── */

/* `resolve`: 1 = the producer resolves each pid's category (what the daemon
 * now does), 0 = every push carries 0, which is exactly the pre-v6 writer
 * (it had no category input at all). The 0 variant is this file's mutation
 * probe: every exclusion assertion below is also run against it, and must
 * see the io_worker time present. */
static int write_stream(const char *dir, const struct pgwt_trace_event *ev,
                        int n, int resolve, uint64_t *excl_ns,
                        uint64_t *excl_events)
{
    struct pgwt_summary_writer *w = calloc(1, sizeof(*w));
    if (!w || pgwt_summary_writer_init(w, dir, 24, NULL) != 0) {
        free(w);
        return -1;
    }
    int rejected = 0;
    for (int i = 0; i < n; i++)
        if (pgwt_summary_push_event(w, &ev[i], resolve ? cat_of(ev[i].pid) : 0)
            != 0)
            rejected++;
    pgwt_summary_flush(w);
    pgwt_summary_close(w);
    if (excl_ns)     *excl_ns = w->io_worker_ns_excluded_total;
    if (excl_events) *excl_events = w->io_worker_events_excluded_total;
    int recs = (int)w->total_records_written;
    pgwt_summary_destroy(w);
    free(w);
    return rejected ? -1 : recs;
}

/* ── read the records back ─────────────────────────────────────────────── */

struct rec_ctx {
    int records;
    uint64_t min_sec, max_sec;
    uint64_t class_ns[PGWT_NUM_CLASSES];
    uint64_t idle_ns;
    uint64_t ev_ns[8];            /* parallel to EV_PROBE below */
    int      saw_io_worker_session;
    int      saw_bg_session;      /* the over-broad-exclusion control */
    int      sessions;
    uint64_t hist_cells;          /* Σ events[].histogram -- the heatmap source */
};

/* 0..3 are the events whose presence/absence the exclusion decides; index 4
 * is the BACKGROUND pid's idle event, which must ALWAYS be present. */
static const uint32_t EV_PROBE[8] = {
    EV_LOCK, EV_CLIENTREAD, EV_IOWORKERMAIN, EV_DATAFILEREAD,
    EV_BGWRITERMAIN, 0, 0, 0
};
#define EV_PROBE_N 5

static int rec_visitor(const struct pgwt_summary_accum *rec, void *vctx)
{
    struct rec_ctx *c = vctx;
    if (c->records == 0 || rec->second_wall_ns < c->min_sec)
        c->min_sec = rec->second_wall_ns;
    if (rec->second_wall_ns > c->max_sec)
        c->max_sec = rec->second_wall_ns;
    c->records++;
    for (int i = 0; i < PGWT_NUM_CLASSES; i++)
        c->class_ns[i] += rec->class_ns[i];
    c->idle_ns += rec->idle_ns;
    for (int e = 0; e < SUMMARY_MAX_EVENTS; e++) {
        const struct pgwt_summary_event *se = &rec->events[e];
        if (se->count == 0) continue;
        for (int k = 0; k < EV_PROBE_N; k++)
            if (se->event_id == EV_PROBE[k]) c->ev_ns[k] += se->total_ns;
        for (int b = 0; b < HISTOGRAM_BUCKETS; b++)
            c->hist_cells += se->histogram[b];
    }
    for (int s = 0; s < SUMMARY_MAX_SESSIONS; s++) {
        if (rec->sessions[s].pid == 0) continue;
        c->sessions++;
        if (rec->sessions[s].pid == PID_IO_WORKER)
            c->saw_io_worker_session = 1;
        if (rec->sessions[s].pid == PID_BG)
            c->saw_bg_session = 1;
    }
    return 0;
}

static int read_records(const char *dir, struct rec_ctx *c)
{
    memset(c, 0, sizeof(*c));
    pgwt_visit_summaries(dir, 0, 0, rec_visitor, c);
    return c->records;
}

/* ── 0. THE FIXTURE'S OWN PREMISE ──────────────────────────────────────── */

static void test_fixture_is_unflagged(const struct pgwt_trace_event *ev, int n)
{
    printf("--- 0. the input carries NO category bit (the N7 trap) ---\n");
    int flagged = 0, iw_recs = 0, iw_busy = 0, iw_idle = 0;
    int bg_busy = 0, bg_idle = 0;
    for (int i = 0; i < n; i++) {
        if (ev[i].flags & (PGWT_EVENT_FLAG_IO_WORKER | PGWT_EVENT_FLAG_MAINT
                           | PGWT_EVENT_FLAG_BACKGROUND))
            flagged++;
        uint32_t cat = cat_of(ev[i].pid);
        if (cat == PGWT_EVENT_FLAG_BACKGROUND) {
            if (pgwt_is_idle_event(ev[i].old_event)) bg_idle++; else bg_busy++;
        }
        if (cat != PGWT_EVENT_FLAG_IO_WORKER) continue;
        iw_recs++;
        if (pgwt_is_idle_event(ev[i].old_event)) iw_idle++; else iw_busy++;
    }
    CHECK(flagged == 0,
          "no fixture event pre-sets a category flag (%d did) -- a pre-flagged "
          "event greens a writer check that is DEAD in the daemon, which is "
          "how this bug was 'fixed' once before (hole N7)", flagged);
    CHECK(iw_recs > 0, "the fixture HAS io_worker records (%d)", iw_recs);
    CHECK(iw_busy > 0 && iw_idle > 0,
          "...both BUSY (%d) and IDLE (%d): the raw path treats them "
          "differently (busy -> cat_ms[io_worker], idle -> dropped), so one "
          "alone would pass a half-exclusion", iw_busy, iw_idle);
    CHECK(bg_busy > 0 && bg_idle > 0,
          "and it has a NON-io_worker background pid with both busy (%d) and "
          "idle (%d) records -- the control for an OVER-BROAD exclusion. "
          "src/compute.c special-cases only the io_worker bit, so background "
          "and maintenance time stays in raw DB Time and raw Idle; a writer "
          "excluding on `cat_flag != 0` would delete it from every >= 120 s "
          "window and no fixture without such a pid could tell",
          bg_busy, bg_idle);
}

/* ── 1. THE WRITER DROPS THEM FROM ALL FIVE ACCUMULATORS ───────────────── */

static void test_writer_exclusion(const struct pgwt_trace_event *ev, int n)
{
    printf("--- 1. writer: class_ns, idle_ns, events[], sessions[], queries[] ---\n");
    const char *dir = fresh_dir("excl");
    uint64_t excl_ns = 0, excl_ev = 0;
    int recs = write_stream(dir, ev, n, 1, &excl_ns, &excl_ev);
    CHECK(recs == SECS, "the writer committed %d records (expected %d)",
          recs, SECS);
    struct rec_ctx c;
    CHECK(read_records(dir, &c) == SECS, "the reader read %d records back",
          c.records);
    if (c.records == 0) return;        /* nothing below could mean anything */

    CHECK(excl_ev == (uint64_t)(SECS * 2),
          "the writer COUNTED the drops: %llu events (expected %d)",
          (unsigned long long)excl_ev, SECS * 2);
    CHECK(NEAR_MS((double)excl_ns / 1e6, EXP_IW_MS),
          "...and their ns: %.3f ms (expected %.3f)",
          (double)excl_ns / 1e6, EXP_IW_MS);

    /* class_ns: the io_worker's BUSY time was an IO-class event, and it is
     * the only IO-class event in the fixture, so the IO class total is the
     * whole test. Idle events never enter class_ns (v3), so IoWorkerMain
     * could only have shown up in idle_ns. */
    CHECK(c.class_ns[PGWT_CLASS_IO] == 0,
          "class_ns[IO] == 0 (got %.3f ms): the io_worker's busy time is the "
          "only IO-class event here", (double)c.class_ns[PGWT_CLASS_IO] / 1e6);
    /* THE OVER-BROAD HALF of the same assertion: the Lock class carries the
     * client's 20 ms AND the background pid's 12 ms per second. A writer
     * excluding every non-zero cat_flag would be 12 ms/s short here while
     * every io_worker check above still passed. */
    CHECK(NEAR_MS((double)c.class_ns[PGWT_CLASS_LOCK] / 1e6, EXP_LOCK_MS),
          "...while the client's AND the background pid's Lock time are "
          "untouched: %.3f ms (expected %.3f = %d x (20 + 12) ms) -- the "
          "exclusion is the io_worker bit, not any non-zero category",
          (double)c.class_ns[PGWT_CLASS_LOCK] / 1e6, EXP_LOCK_MS, SECS);
    CHECK(NEAR_MS((double)c.idle_ns / 1e6, EXP_IDLE_MS),
          "idle_ns carries ClientRead AND the background pid's BgwriterMain: "
          "%.3f ms (expected %.3f = %d x (25 + 30) ms), i.e. the io_worker's "
          "%.3f ms of IoWorkerMain is NOT in it and the background pid's "
          "%.3f ms IS",
          (double)c.idle_ns / 1e6, EXP_IDLE_MS, SECS, (double)(SECS * 40),
          EXP_BG_IDLE_MS);
    CHECK(c.ev_ns[2] == 0 && c.ev_ns[3] == 0,
          "events[] has no IoWorkerMain (%.3f ms) and no io_worker "
          "DataFileRead (%.3f ms) -- events[] carries the HISTOGRAMS, so this "
          "is also what governs the summary heatmap",
          (double)c.ev_ns[2] / 1e6, (double)c.ev_ns[3] / 1e6);
    CHECK(c.ev_ns[0] > 0 && c.ev_ns[1] > 0
          && NEAR_MS((double)c.ev_ns[4] / 1e6, EXP_BG_IDLE_MS),
          "...while the client's two events (%.3f, %.3f ms) and the "
          "BACKGROUND pid's idle event (%.3f ms, expected %.3f) are all still "
          "there -- IoWorkerMain and BgwriterMain are both Activity-class idle "
          "events, so only the pid's category can tell them apart",
          (double)c.ev_ns[0] / 1e6, (double)c.ev_ns[1] / 1e6,
          (double)c.ev_ns[4] / 1e6, EXP_BG_IDLE_MS);
    CHECK(!c.saw_io_worker_session,
          "sessions[] has no row for pid %d", PID_IO_WORKER);
    CHECK(c.saw_bg_session,
          "...but it DOES have one for the background pid %d: an over-broad "
          "exclusion would drop that row too", PID_BG);
    CHECK(c.sessions == SECS * SESSIONS_PER_SEC,
          "...and exactly %d session rows per second (%d): the client's and "
          "the background pid's", SESSIONS_PER_SEC, c.sessions);
    CHECK(c.hist_cells == (uint64_t)(SECS * KEPT_PER_SEC),
          "Σ events[].histogram counts the %d non-io_worker intervals per "
          "second (%llu, expected %d) -- the io_worker's 2 are gone from the "
          "grid and nothing else is", KEPT_PER_SEC,
          (unsigned long long)c.hist_cells, SECS * KEPT_PER_SEC);

    /* MUTATION PROBE: the same stream, same assertions, with the producer
     * passing 0 -- the pre-v6 writer. Everything above must flip, or the
     * assertions are not reading what they claim to read. */
    const char *dir0 = fresh_dir("excl-cat0");
    uint64_t z_ns = 1, z_ev = 1;
    CHECK(write_stream(dir0, ev, n, 0, &z_ns, &z_ev) == SECS,
          "cat_flag-0 run writes the same %d records", SECS);
    struct rec_ctx c0;
    read_records(dir0, &c0);
    CHECK(z_ev == 0 && z_ns == 0,
          "cat_flag 0: nothing is excluded (%llu events, %llu ns)",
          (unsigned long long)z_ev, (unsigned long long)z_ns);
    CHECK(NEAR_MS((double)c0.class_ns[PGWT_CLASS_IO] / 1e6, SECS * 10.0),
          "cat_flag 0: class_ns[IO] carries the io_worker busy time "
          "(%.3f ms) -- so the check above can see a difference",
          (double)c0.class_ns[PGWT_CLASS_IO] / 1e6);
    CHECK(NEAR_MS((double)c0.idle_ns / 1e6, EXP_IDLE_MS + SECS * 40.0),
          "cat_flag 0: idle_ns is inflated by IoWorkerMain (%.3f ms vs "
          "%.3f) -- #376's +23%% on the real capture, in miniature",
          (double)c0.idle_ns / 1e6, EXP_IDLE_MS);
    CHECK(c0.saw_io_worker_session && c0.ev_ns[2] > 0 && c0.ev_ns[3] > 0,
          "cat_flag 0: the io_worker has a sessions[] row and both events[] "
          "entries (#315's Top Sessions / Top Events rows)");
    CHECK(c0.hist_cells == (uint64_t)(SECS * SLICE_N),
          "cat_flag 0: the histogram grid carries all %d intervals per second "
          "(%llu) -- the heatmap half of the divergence", SLICE_N,
          (unsigned long long)c0.hist_cells);

    /* THE OTHER DIRECTION, and the one a fixture without a background pid
     * cannot probe at all: an OVER-BROAD exclusion. Driven by handing the
     * writer PGWT_EVENT_FLAG_IO_WORKER for every pid, which is what
     * `cat_flag != 0` in accum_event would effectively do to this stream --
     * the background pid's records vanish from class_ns, idle_ns, events[]
     * and sessions[] while every io_worker assertion above stays green.
     * Pinned here so the difference between "excluded" and "excluded too
     * much" is observable inside one run. */
    const char *dirx = fresh_dir("excl-overbroad");
    {
        struct pgwt_summary_writer *w = calloc(1, sizeof(*w));
        if (w && pgwt_summary_writer_init(w, dirx, 24, NULL) == 0) {
            for (int i = 0; i < n; i++)
                pgwt_summary_push_event(w, &ev[i],
                                        PGWT_EVENT_FLAG_IO_WORKER);
            pgwt_summary_flush(w);
            pgwt_summary_close(w);
            pgwt_summary_destroy(w);
        }
        free(w);
    }
    struct rec_ctx cx;
    read_records(dirx, &cx);
    CHECK(cx.records > 0
          && !NEAR_MS((double)cx.class_ns[PGWT_CLASS_LOCK] / 1e6, EXP_LOCK_MS)
          && !NEAR_MS((double)cx.idle_ns / 1e6, EXP_IDLE_MS)
          && !cx.saw_bg_session,
          "an over-broad exclusion is VISIBLE: Lock %.3f ms (correct %.3f), "
          "idle_ns %.3f ms (correct %.3f), background session row %s -- so "
          "the assertions above distinguish the io_worker bit from any "
          "non-zero category",
          (double)cx.class_ns[PGWT_CLASS_LOCK] / 1e6, EXP_LOCK_MS,
          (double)cx.idle_ns / 1e6, EXP_IDLE_MS,
          cx.saw_bg_session ? "present" : "gone");
}

/* ── 2. RAW vs SUMMARY: time model AND heatmap ─────────────────────────── */

/* Returns 1 if the window was usable; the deltas come back through the
 * pointers. REFUSES (returns 0) on any shape that would make a comparison
 * pass on nothing. */
static int compare_paths(const char *dir, const struct pgwt_trace_event *ev,
                         int n, uint64_t mono_base,
                         double *d_db, double *d_idle, long long *d_cells,
                         double *raw_db, double *raw_idle)
{
    struct rec_ctx c;
    if (read_records(dir, &c) == 0) {
        printf("  REFUSE: no summary records in %s -- every delta would be "
               "computed against an empty side\n", dir);
        tests_failed++;
        return 0;
    }
    uint64_t from = c.min_sec, to = c.max_sec + ONE_SEC;
    int considered = -1, unusable = -1;
    if (pgwt_summaries_window_current(dir, from, to, &considered,
                                      &unusable) != 1 || considered <= 0) {
        printf("  REFUSE: the version preflight does not accept the window "
               "just written (considered=%d unusable=%d)\n",
               considered, unusable);
        tests_failed++;
        return 0;
    }
    double wall_ms = (double)(to - from) / 1e6;

    /* The raw side sees the SAME wall window: shift by the writer's own
     * mono->wall offset, recovered from the records rather than from a clock,
     * then tag exactly as pgwt-server's raw loader does. */
    uint64_t shift = from - mono_base;
    struct pgwt_trace_event *tag = malloc((size_t)n * sizeof(*tag));
    for (int i = 0; i < n; i++) {
        tag[i] = ev[i];
        tag[i].timestamp_ns = ev[i].timestamp_ns + shift;
    }
    /* #363: a REFUSED tagging pass leaves this array -- the raw side of the
     * comparison below -- partly rewritten, with the records it never reached
     * keeping old_event 0 instead of PGWT_WEI_NONCMD_CPU. Comparing against
     * a half-built oracle would produce a confident number from nothing, so
     * refuse. */
    if (!pgwt_tag_events(tag, n, CATS, N_CATS)) {
        printf("  REFUSE: pgwt_tag_events REFUSED (#363) -- the raw oracle is "
               "partly rewritten and must be discarded, not compared\n");
        tests_failed++;
        free(tag);
        return 0;
    }
    int tagged_iw = 0;
    for (int i = 0; i < n; i++)
        if (tag[i].flags & PGWT_EVENT_FLAG_IO_WORKER) tagged_iw++;
    if (tagged_iw == 0) {
        printf("  REFUSE: pgwt_tag_events flagged no io_worker record, so the "
               "RAW side has nothing to exclude and both sides would agree "
               "for the wrong reason\n");
        tests_failed++;
        free(tag);
        return 0;
    }

    struct pgwt_filter f;
    memset(&f, 0, sizeof(f));
    struct pgwt_tm_result rtm, stm;
    pgwt_compute_time_model(tag, n, &f, from, to, wall_ms, &rtm);
    pgwt_compute_time_model_from_summaries(dir, from, to, &f, wall_ms, &stm);
    struct pgwt_heatmap_result rhm, shm;
    pgwt_compute_heatmap(tag, n, &f, from, to, SECS, &rhm);
    pgwt_compute_heatmap_from_summaries(dir, from, to, &f, SECS, &shm);

    *d_db    = stm.db_time_ms   - rtm.db_time_ms;
    *d_idle  = stm.idle_time_ms - rtm.idle_time_ms;
    *d_cells = (long long)shm.total_events - (long long)rhm.total_events;
    *raw_db   = rtm.db_time_ms;
    *raw_idle = rtm.idle_time_ms;
    int ok = 1;
    if (rtm.db_time_ms <= 0 || rtm.idle_time_ms <= 0
        || rhm.total_events == 0) {
        printf("  REFUSE: the raw side is empty (db %.3f idle %.3f cells "
               "%llu)\n", rtm.db_time_ms, rtm.idle_time_ms,
               (unsigned long long)rhm.total_events);
        tests_failed++;
        ok = 0;
    }
    free(rtm.rows); free(stm.rows);
    free(rhm.grid); free(rhm.times);
    free(shm.grid); free(shm.times);
    free(tag);
    return ok;
}

static void test_raw_vs_summary(const struct pgwt_trace_event *ev, int n,
                                uint64_t base)
{
    printf("--- 2. raw vs summary: time model AND heatmap ---\n");
    const char *dir = fresh_dir("agree");
    CHECK(write_stream(dir, ev, n, 1, NULL, NULL) == SECS, "records written");
    double d_db = 1, d_idle = 1, raw_db = 0, raw_idle = 0;
    long long d_cells = 1;
    if (compare_paths(dir, ev, n, base, &d_db, &d_idle, &d_cells,
                      &raw_db, &raw_idle)) {
        CHECK(NEAR_MS(raw_db, EXP_DB_MS) && NEAR_MS(raw_idle, EXP_IDLE_MS),
              "the raw path is the reference and is itself right: DB Time "
              "%.3f (expected %.3f), Idle %.3f (expected %.3f)",
              raw_db, EXP_DB_MS, raw_idle, EXP_IDLE_MS);
        CHECK(fabs(d_db) <= TOL_MS, "DB Time agrees (delta %+.3f ms)", d_db);
        CHECK(fabs(d_idle) <= TOL_MS, "Idle agrees (delta %+.3f ms)", d_idle);
        CHECK(d_cells == 0,
              "the heatmap agrees cell-for-cell (delta %+lld records) -- both "
              "handle_heatmap routes answer the same question", d_cells);
    }

    /* And with the producer passing 0: all three must DISAGREE. This is the
     * gate's teeth, measured in the same run that claims agreement. */
    const char *dir0 = fresh_dir("agree-cat0");
    CHECK(write_stream(dir0, ev, n, 0, NULL, NULL) == SECS, "cat0 written");
    double z_db = 0, z_idle = 0, z_rdb = 0, z_ridle = 0;
    long long z_cells = 0;
    if (compare_paths(dir0, ev, n, base, &z_db, &z_idle, &z_cells,
                      &z_rdb, &z_ridle)) {
        CHECK(z_db > TOL_MS && z_idle > TOL_MS && z_cells > 0,
              "cat_flag 0 diverges on all three (DB %+.3f ms, Idle %+.3f ms, "
              "heatmap %+lld records) -- a comparison that cannot go red is "
              "not evidence", z_db, z_idle, z_cells);
        CHECK(NEAR_MS(z_db, SECS * 10.0) && NEAR_MS(z_idle, SECS * 40.0),
              "...by exactly the io_worker busy (%.3f, expected %.3f) and "
              "idle (%.3f, expected %.3f) totals", z_db, SECS * 10.0,
              z_idle, SECS * 40.0);
    }
}

/* ── 3. THE CAPTURE-END FLUSH SHAPE ────────────────────────────────────── */

/* src/escalation.c's flush_open_intervals synthesizes one record per open
 * interval when the capture ends -- and an io_worker spends the WHOLE capture
 * in a single Activity:IoWorkerMain wait, so that record is its only one and
 * carries the entire span. It reaches the writer by a path that never passes
 * through src/event_stream.c, which is why a fix wired only there misses the
 * main symptom: measured +308.16% on summary Idle for a window containing
 * that second, against +2.43% for the same window one second earlier. */
static void test_capture_end_flush(uint64_t base)
{
    printf("--- 3. the capture-end flush record (escalation.c's producer) ---\n");
    struct pgwt_trace_event *ev = malloc(sizeof(*ev) * MAX_EV);
    int n = build_stream(ev, base);

    /* The flush record: closes in the LAST second, duration = the whole
     * 150 s span, as the real one does. */
    struct pgwt_trace_event flush;
    memset(&flush, 0, sizeof(flush));
    flush.timestamp_ns = base + (uint64_t)(SECS - 1) * ONE_SEC + 900 * MS;
    flush.pid          = PID_IO_WORKER;
    flush.old_event    = EV_IOWORKERMAIN;
    flush.new_event    = EV_IOWORKERMAIN;
    flush.duration_ns  = (uint64_t)SECS * ONE_SEC;
    flush.cpu_ns       = PGWT_CPU_NS_UNKNOWN;
    flush.flags        = 0;
    ev[n++] = flush;

    const char *dir = fresh_dir("flush");
    uint64_t excl_ns = 0, excl_ev = 0;
    CHECK(write_stream(dir, ev, n, 1, &excl_ns, &excl_ev) == SECS,
          "records written with the flush record included");
    struct rec_ctx c;
    read_records(dir, &c);
    CHECK(excl_ev == (uint64_t)(SECS * 2 + 1),
          "the flush record is excluded too (%llu drops, expected %d)",
          (unsigned long long)excl_ev, SECS * 2 + 1);
    CHECK(NEAR_MS((double)c.idle_ns / 1e6, EXP_IDLE_MS),
          "idle_ns is still the client's %.3f ms (got %.3f) -- the %d 000 ms "
          "flush record did not land in the closing second",
          EXP_IDLE_MS, (double)c.idle_ns / 1e6, SECS);

    const char *dir0 = fresh_dir("flush-cat0");
    CHECK(write_stream(dir0, ev, n, 0, NULL, NULL) == SECS, "cat0 written");
    struct rec_ctx c0;
    read_records(dir0, &c0);
    CHECK((double)c0.idle_ns / 1e6 > EXP_IDLE_MS + SECS * 1000.0 - 1.0,
          "cat_flag 0: the SAME record inflates idle_ns to %.3f ms (the whole "
          "%d s span bucketed into one second) -- the shape #376 measured",
          (double)c0.idle_ns / 1e6, SECS);
    free(ev);
}

/* ── 4. BOTH PRODUCERS MUST RESOLVE A CATEGORY ─────────────────────────── */

/* The writer can only be as right as what it is told, and the one thing the
 * sections above CANNOT see is a producer that passes 0. The daemon's two
 * producers (src/event_stream.c, src/escalation.c) need libbpf and a BPF
 * skeleton, so they cannot be linked into this test; their wiring is checked
 * by reading the source, the same idiom tests/test_agg_raw_crosscheck.c uses
 * to detect a new aggregate path in ../src/compute.h.
 *
 * It is a REFUSING check: an unreadable file, a renamed symbol or a push it
 * cannot classify all FAIL. A gate that cannot see must never approve. */

static char *slurp(const char *path, long *len)
{
    FILE *fp = fopen(path, "rb");
    if (!fp) return NULL;
    fseek(fp, 0, SEEK_END);
    long sz = ftell(fp);
    fseek(fp, 0, SEEK_SET);
    if (sz <= 0) { fclose(fp); return NULL; }
    char *buf = malloc((size_t)sz + 1);
    size_t got = fread(buf, 1, (size_t)sz, fp);
    fclose(fp);
    buf[got] = 0;
    if (len) *len = (long)got;
    return buf;
}

/* How many times `needle` occurs in `hay`. */
static int count_occurrences(const char *hay, const char *needle)
{
    int n = 0;
    for (const char *p = hay; (p = strstr(p, needle)); p += strlen(needle))
        n++;
    return n;
}

#define PUSH "pgwt_summary_push_event("
#define RESOLVE "pgwt_live_pid_cat_flag("

/* Returns 0 on success, non-zero with a printed reason otherwise. Used by
 * section 4 for the real files and by section 5's bypass suite for crafted
 * ones, so the refusal paths are exercised, not merely asserted to exist. */
static int check_event_stream_src(const char *path, char *why, size_t wsz)
{
    long len = 0;
    char *src = slurp(path, &len);
    if (!src) { snprintf(why, wsz, "cannot read %s", path); return 1; }
    const char *push = strstr(src, PUSH);
    const char *res  = strstr(src, RESOLVE);
    int rc = 0;
    if (!push)      { snprintf(why, wsz, "no %s call found", PUSH); rc = 2; }
    else if (!res)  { snprintf(why, wsz, "no %s call found", RESOLVE); rc = 3; }
    else if (res > push) {
        snprintf(why, wsz, "the category is resolved %ld bytes AFTER the "
                 "summary push -- the writer is told nothing",
                 (long)(res - push));
        rc = 4;
    } else {
        /* The push must carry a third argument that is not a literal 0. */
        const char *close = strchr(push, ';');
        if (!close) { snprintf(why, wsz, "unparseable push call"); rc = 5; }
        else {
            size_t span = (size_t)(close - push);
            char call[512];
            if (span >= sizeof(call)) span = sizeof(call) - 1;
            memcpy(call, push, span);
            call[span] = 0;
            if (!strstr(call, "cat_flag")) {
                snprintf(why, wsz, "the push does not pass cat_flag: %s", call);
                rc = 6;
            }
        }
    }
    free(src);
    return rc;
}

static int check_escalation_src(const char *path, char *why, size_t wsz)
{
    long len = 0;
    char *src = slurp(path, &len);
    if (!src) { snprintf(why, wsz, "cannot read %s", path); return 1; }
    int pushes = count_occurrences(src, PUSH);
    int rc = 0;
    if (pushes == 0) {
        snprintf(why, wsz, "no %s call found", PUSH);
        rc = 2;
    } else if (pushes != 3) {
        /* Deliberately exact: three producers live here (two marker writers
         * and the capture-end flush). A fourth means a new producer that has
         * to decide what category it passes, and it should land in this gate
         * rather than beside it. */
        snprintf(why, wsz, "%d summary pushes, expected 3 -- a new producer "
                 "must declare its category", pushes);
        rc = 3;
    } else {
        /* The LAST push is flush_open_intervals' (file order), and it is the
         * one that must resolve: markers carry no time. */
        const char *p = src, *last = NULL;
        while ((p = strstr(p, PUSH))) { last = p; p += strlen(PUSH); }
        const char *end = strchr(last, ';');
        if (!end) { snprintf(why, wsz, "unparseable push call"); rc = 4; }
        else {
            size_t span = (size_t)(end - last);
            char call[512];
            if (span >= sizeof(call)) span = sizeof(call) - 1;
            memcpy(call, last, span);
            call[span] = 0;
            if (!strstr(call, RESOLVE)) {
                snprintf(why, wsz, "the capture-end flush push does not "
                         "resolve a category: %s", call);
                rc = 5;
            }
        }
    }
    free(src);
    return rc;
}

static void test_producers_are_wired(void)
{
    printf("--- 4. both daemon producers resolve a category ---\n");
    char why[600] = "";
    int rc = check_event_stream_src("../src/event_stream.c", why, sizeof(why));
    CHECK(rc == 0, "src/event_stream.c hoists the resolve above the summary "
          "push and passes it: %s", rc ? why : "ok");
    why[0] = 0;
    rc = check_escalation_src("../src/escalation.c", why, sizeof(why));
    CHECK(rc == 0, "src/escalation.c resolves in the capture-end flush: %s",
          rc ? why : "ok");
}

/* ── 5. BYPASS SUITE: every way these checks can stop checking ─────────── */

static void write_text(const char *path, const char *text)
{
    FILE *fp = fopen(path, "w");
    if (!fp) return;
    fputs(text, fp);
    fclose(fp);
}

static void test_bypass(const struct pgwt_trace_event *ev, int n,
                        uint64_t base)
{
    printf("--- 5. bypass suite (a gate that cannot see must refuse) ---\n");
/* Run a real check against a crafted bad input: its red output is expected,
 * so it is labelled, and the counter is restored afterwards by the caller. */
#define PROVOKE(stmt) do { expect_fail = 1; stmt; expect_fail = 0; } while (0)

    /* 5a. No summary directory at all: the comparison must refuse, not read
     * 0 == 0 as agreement. Checked by calling the real helper and counting
     * that it bumped tests_failed. */
    {
        int before = tests_failed;
        double a, b, rd, ri; long long cells;
        char missing[600];
        snprintf(missing, sizeof(missing), "%s/no-such-dir", g_base);
        PROVOKE(compare_paths(missing, ev, n, base, &a, &b, &cells, &rd, &ri));
        CHECK(tests_failed == before + 1,
              "a missing summary dir REFUSES (failures %d -> %d)",
              before, tests_failed);
        tests_failed = before;        /* that failure was the expected one */
    }

    /* 5b. A writer that wrote nothing (no events pushed). */
    {
        const char *dir = fresh_dir("empty-writer");
        struct pgwt_summary_writer *w = calloc(1, sizeof(*w));
        pgwt_summary_writer_init(w, dir, 24, NULL);
        pgwt_summary_flush(w);
        pgwt_summary_close(w);
        pgwt_summary_destroy(w);
        free(w);
        int before = tests_failed;
        double a, b, rd, ri; long long cells;
        PROVOKE(compare_paths(dir, ev, n, base, &a, &b, &cells, &rd, &ri));
        CHECK(tests_failed == before + 1,
              "an empty summary window REFUSES (failures %d -> %d)",
              before, tests_failed);
        tests_failed = before;
    }

    /* 5c. The thing being checked is ABSENT rather than wrong: a stream with
     * no io_worker record at all. The comparison would pass trivially, so the
     * fixture premise check (section 0) must reject it. */
    {
        struct pgwt_trace_event *plain = malloc(sizeof(*plain) * MAX_EV);
        int m = 0;
        for (int s = 0; s < SECS; s++) {
            struct pgwt_trace_event e;
            memset(&e, 0, sizeof(e));
            e.timestamp_ns = base + (uint64_t)s * ONE_SEC + 110 * MS;
            e.pid = PID_CLIENT; e.old_event = EV_LOCK; e.new_event = EV_LOCK;
            e.duration_ns = 20 * MS; e.cpu_ns = PGWT_CPU_NS_UNKNOWN;
            plain[m++] = e;
        }
        int before = tests_failed;
        PROVOKE(test_fixture_is_unflagged(plain, m));
        CHECK(tests_failed > before,
              "an io_worker-FREE fixture is REJECTED by the premise check "
              "(failures %d -> %d) -- otherwise 'the two paths agree' says "
              "nothing about io_workers", before, tests_failed);
        tests_failed = before;
        free(plain);
    }

    /* 5d. A PRE-FLAGGED fixture: the trap. The premise check must reject it
     * even though the writer would then exclude the records. */
    {
        struct pgwt_trace_event *pre = malloc(sizeof(*pre) * MAX_EV);
        int m = build_stream(pre, base);
        for (int i = 0; i < m; i++)
            if (pre[i].pid == PID_IO_WORKER)
                pre[i].flags |= PGWT_EVENT_FLAG_IO_WORKER;
        int before = tests_failed;
        PROVOKE(test_fixture_is_unflagged(pre, m));
        CHECK(tests_failed > before,
              "a PRE-FLAGGED fixture is REJECTED (failures %d -> %d)",
              before, tests_failed);
        tests_failed = before;
        /* ...and the product does NOT honour the flag, so pre-flagging cannot
         * green the exclusion either: the writer's only input is cat_flag. */
        const char *dir = fresh_dir("preflag");
        uint64_t ex_ns = 1, ex_ev = 1;
        write_stream(dir, pre, m, 0, &ex_ns, &ex_ev);
        CHECK(ex_ev == 0 && ex_ns == 0,
              "a pre-flagged event with cat_flag 0 is NOT excluded (%llu "
              "drops): the writer reads the producer's verdict, never "
              "evt->flags, so no fixture can certify a dead guard",
              (unsigned long long)ex_ev);
        free(pre);
    }

    /* 5e. The source-reading producer gate: missing file, absent symbols,
     * a push with a literal 0, a resolve placed after the push, and an
     * unexpected producer count. Each must REFUSE, with its own reason. */
    {
        char why[600];
        char p[600];
        snprintf(p, sizeof(p), "%s/nofile.c", g_base);
        CHECK(check_event_stream_src(p, why, sizeof(why)) == 1,
              "producer gate: a missing file refuses (%s)", why);
        CHECK(check_escalation_src(p, why, sizeof(why)) == 1,
              "producer gate: a missing escalation file refuses (%s)", why);

        snprintf(p, sizeof(p), "%s/empty.c", g_base);
        write_text(p, "int main(void) { return 0; }\n");
        CHECK(check_event_stream_src(p, why, sizeof(why)) == 2,
              "producer gate: a file with no push at all refuses (%s)", why);
        CHECK(check_escalation_src(p, why, sizeof(why)) == 2,
              "producer gate: likewise for escalation (%s)", why);

        snprintf(p, sizeof(p), "%s/nores.c", g_base);
        write_text(p, "void f(void) { pgwt_summary_push_event(w, evt, 0); }\n");
        CHECK(check_event_stream_src(p, why, sizeof(why)) == 3,
              "producer gate: a push with no resolver anywhere refuses (%s)",
              why);

        snprintf(p, sizeof(p), "%s/after.c", g_base);
        write_text(p, "void f(void) {\n"
                      "  pgwt_summary_push_event(w, evt, cat_flag);\n"
                      "  uint32_t cat_flag = pgwt_live_pid_cat_flag(d, pa, pid);\n"
                      "}\n");
        CHECK(check_event_stream_src(p, why, sizeof(why)) == 4,
              "producer gate: a resolve AFTER the push refuses -- the exact "
              "pre-fix layout (%s)", why);

        snprintf(p, sizeof(p), "%s/zero.c", g_base);
        write_text(p, "void f(void) {\n"
                      "  uint32_t x = pgwt_live_pid_cat_flag(d, pa, pid);\n"
                      "  pgwt_summary_push_event(w, evt, 0);\n"
                      "}\n");
        CHECK(check_event_stream_src(p, why, sizeof(why)) == 6,
              "producer gate: resolving and then passing a literal 0 refuses "
              "(%s)", why);

        snprintf(p, sizeof(p), "%s/four.c", g_base);
        write_text(p, "void a(void){pgwt_summary_push_event(w,&ev,0);}\n"
                      "void b(void){pgwt_summary_push_event(w,&ev,0);}\n"
                      "void c(void){pgwt_summary_push_event(w,&ev,0);}\n"
                      "void d(void){pgwt_summary_push_event(w,&ev,0);}\n");
        CHECK(check_escalation_src(p, why, sizeof(why)) == 3,
              "producer gate: a FOURTH producer in escalation.c refuses -- a "
              "new one must declare its category (%s)", why);

        snprintf(p, sizeof(p), "%s/three0.c", g_base);
        write_text(p, "void a(void){pgwt_summary_push_event(w,&ev,0);}\n"
                      "void b(void){pgwt_summary_push_event(w,&ev,0);}\n"
                      "void c(void){pgwt_summary_push_event(w,&ev,0);}\n");
        CHECK(check_escalation_src(p, why, sizeof(why)) == 5,
              "producer gate: three pushes where the flush one passes 0 "
              "refuses (%s)", why);
    }

    /* 5f. A v5 file in the window: the preflight must refuse it, so
     * compare_paths refuses rather than comparing against a partial read.
     * (The full version-preflight suite lives in tests/test_idle_summary.c;
     * this pins that MY comparison inherits the refusal instead of reading
     * an empty window as agreement.) */
    {
        const char *dir = fresh_dir("v5");
        write_stream(dir, ev, n, 1, NULL, NULL);
        char cmd[800];
        /* Rewrite the version byte of every summary file to 5. The header is
         * magic(4) + version(4) little-endian at offset 4. */
        snprintf(cmd, sizeof(cmd),
                 "for f in '%s'/*.summary '%s'/current.summary; do "
                 "[ -f \"$f\" ] && printf '\\005\\000\\000\\000' | "
                 "dd of=\"$f\" bs=1 seek=4 conv=notrunc status=none; done",
                 dir, dir);
        if (system(cmd) != 0) { /* ignore */ }
        int considered = -1, unusable = -1;
        struct rec_ctx c;
        read_records(dir, &c);
        CHECK(c.records == 0,
              "a v5 file reads back as 0 records under v%d (%d) -- refused, "
              "not mis-parsed", PGWT_SUMMARY_VERSION, c.records);
        CHECK(pgwt_summaries_window_current(dir, 1, (uint64_t)-1, &considered,
                                            &unusable) == 0 && unusable > 0,
              "...and the preflight refuses the window (unusable=%d), so the "
              "server recomputes from RAW instead of answering from nothing",
              unusable);
    }
}

int main(void)
{
    printf("=== io_worker exclusion on the SUMMARY path (#315, #376) ===\n");
    snprintf(g_base, sizeof(g_base), "/tmp/pgwt_iw_%d", (int)getpid());
    char cmd[400];
    snprintf(cmd, sizeof(cmd), "rm -rf '%s'; mkdir -p '%s'", g_base, g_base);
    if (system(cmd) != 0) { /* ignore */ }

    uint64_t base = mono_origin();
    struct pgwt_trace_event *ev = malloc(sizeof(*ev) * MAX_EV);
    int n = build_stream(ev, base);

    test_fixture_is_unflagged(ev, n);
    test_writer_exclusion(ev, n);
    test_raw_vs_summary(ev, n, base);
    test_capture_end_flush(base);
    test_producers_are_wired();
    test_bypass(ev, n, base);

    free(ev);
    snprintf(cmd, sizeof(cmd), "rm -rf '%s'", g_base);
    if (system(cmd) != 0) { /* ignore */ }

    printf("\n%d checks, %d failed\n", tests_run, tests_failed);
    return tests_failed ? 1 : 0;
}
