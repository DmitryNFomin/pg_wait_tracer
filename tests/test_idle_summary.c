/* test_idle_summary.c — the SUMMARY fast path must answer the same DB Time as
 * the raw path, and must refuse a window it cannot answer at all (2026-10-06).
 *
 * WHY THIS FILE IS THE IMPORTANT ONE. The timer-sleep change starts with a
 * predicate (pgwt_is_idle_event), but pgwt-server does not consult that
 * predicate for a wide window: `should_use_summaries()` routes any request
 * wider than 120 s — the demo's whole 900 s window — through
 * pgwt_compute_*_from_summaries, which reads PRECOMPUTED per-second totals.
 * Those totals used to be LUMPED: src/summary_writer.c accumulated every event
 * into `class_ns` with no idle check, and the read paths subtracted
 * Client:ClientRead back out by hand, by hunting WEI(PG_WAIT_CLIENT, 0) in the
 * event table. So changing the predicate alone would have left the demo window
 * reporting the OLD DB Time — green unit tests, wrong number on screen.
 *
 * v3 fixes it at the WRITER: idle events never enter `class_ns`, the per-query
 * totals or the top-wait selection, and `events[]` still carries every event so
 * the idle total and the named Idle sub-rows come from there. The three
 * hardcoded copies of the rule in compute.c are gone.
 *
 * WHAT EACH SECTION PINS
 *   1. a >= 120 s stream, written through the REAL writer and read back
 *      through the REAL reader: time model, AAS, visible event rows, sessions
 *      and queries must match the raw path on the same events. Unfiltered,
 *      query-filtered and Timeout-class-filtered, because the three take
 *      different branches in tm_summary_visitor / aas_summary_visitor.
 *   2. the writer-side exclusion, read off the records directly: class_ns
 *      carries no idle time, events[] carries all of it, and neither top-wait
 *      selection can pick an idle event.
 *   3. the Idle row and its named children on the SUMMARY path too.
 *   4. the VERSION PREFLIGHT. Refusing v1/v2 in the reader is not enough:
 *      pgwt_visit_summaries SKIPS a file it cannot open, so a mixed window
 *      would come back as a plausible PARTIAL answer. The preflight must
 *      refuse, and refusing must mean "recompute from raw", not "empty".
 *   5. FALSE NEGATIVES. Section 1's agreement check is satisfied by 0 == 0,
 *      so every way the comparison can be starved gets its own case: no
 *      summary directory, a writer that wrote nothing, an unreadable file, a
 *      truncated header, a window with no overlap, and the case where the
 *      pacing event is absent rather than mis-accounted.
 *
 * TIMING. Every timestamp is a literal and every flush takes an explicit
 * now_mono_ns, so nothing here depends on wall time. The only real clock read
 * is the writer's own wall/mono offset, captured when it opens its file; no
 * assertion depends on its value, only on the window being derived from the
 * records that were actually written (see window_from_records).
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
#include <dirent.h>
#include <time.h>

static int tests_run = 0;
static int tests_failed = 0;

#define CHECK(cond, fmt, ...) do {                                     \
    tests_run++;                                                       \
    if (cond) { }                                                      \
    else { tests_failed++;                                             \
           printf("  FAIL(line %d): " fmt "\n", __LINE__, ##__VA_ARGS__); } \
} while (0)

/* Summary records are whole seconds and the reader converts mono->wall, so a
 * raw-vs-summary total can differ in the last bit of a double. 0.01 ms over a
 * 150 s window is 7e-8 relative: tight enough that a single misaccounted 1 ms
 * interval fails, loose enough not to chase float noise. NOT a knob — if a
 * comparison here needs more than this, the two paths disagree. */
#define TOL_MS 0.01
#define NEAR_MS(a, b) (fabs((double)(a) - (double)(b)) <= TOL_MS)

#define MS      1000000ULL
#define ONE_SEC 1000000000ULL

/* PG18 Timeout ids; tests/test_wait_event.c pins them by name. */
#define EV_CHECKPOINT_DELAY WEI(PG_WAIT_TIMEOUT, 1)   /* pacing */
#define EV_PGSLEEP          WEI(PG_WAIT_TIMEOUT, 2)   /* DB Time */
#define EV_VACUUM_DELAY     WEI(PG_WAIT_TIMEOUT, 7)   /* pacing */
#define EV_CLIENTREAD       WEI(PG_WAIT_CLIENT, 0)    /* idle, visible */
#define EV_LOCK             WEI(PG_WAIT_LOCK, 0)
#define EV_IO               WEI(PG_WAIT_IO, 21)
#define EV_ACTIVITY         WEI(PG_WAIT_ACTIVITY, 4)  /* idle AND hidden */

#define QID_A 0x1111111111111111ULL
#define QID_B 0x2222222222222222ULL

/* ── scratch dirs ──────────────────────────────────────────────────────── */

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

/* ── the stream ────────────────────────────────────────────────────────────
 * 150 one-second slices (> the 120 s floor should_use_summaries applies, so
 * this is the shape of request that actually takes the summary path). Each
 * second carries the same mix, so every expected total is exact:
 *
 *   pid 101, QID_A : Lock      20 ms   load
 *   pid 101, QID_A : IO        10 ms   load
 *   pid 102, QID_B : PgSleep   30 ms   load  (Timeout, but DB Time)
 *   pid 103, (none): CheckpointWriteDelay 40 ms   IDLE pacing
 *   pid 103, (none): VacuumDelay          15 ms   IDLE pacing
 *   pid 101, QID_A : ClientRead           25 ms   IDLE
 *   pid 104, (none): Activity             50 ms   IDLE and hidden
 *
 *   per second: DB Time 60 ms, Idle 130 ms
 *   over 150 s: DB Time 9000 ms, Idle 19500 ms
 */
#define SLICES          150
#define PER_SEC_DB_MS    60.0
#define PER_SEC_IDLE_MS 130.0

/* THE MONOTONIC ORIGIN, and why it is a clock read rather than a literal.
 *
 * The writer converts each second's MONOTONIC timestamp to WALL using the
 * (CLOCK_REALTIME, CLOCK_MONOTONIC) pair it captured when it opened its file,
 * and stamps that same realtime value into the file header as start_time_ns.
 * The reader's file-level window filter then treats a file as covering
 * [start_time_ns, start_time_ns + 1 h] -- which is sound in production, where
 * every record is written at or after the moment the file was opened.
 *
 * A stream stamped with small literal monotonic values breaks that invariant:
 * its records convert to a wall time one machine-uptime in the PAST while the
 * header still says "now", so every window query misses the file, the summary
 * path answers 0.000 ms, and an agreement test reads 0 == 0 as success. (That
 * is exactly what the first run of this file did; section 5c now pins the
 * 0-records shape so it cannot come back silently.)
 *
 * So the origin is the current monotonic second -- the same convention
 * tests/test_summary_tick_flush.c uses -- which puts the records at or after
 * the file's own start, as in production. This is a clock READ, not a timing
 * dependency: no assertion in this file depends on its value, every duration
 * and spacing below is a literal, and the window each section queries is
 * derived from the records that were actually written (window_from_records),
 * never from a clock. */
static uint64_t mono_origin(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    uint64_t mono = (uint64_t)ts.tv_sec * ONE_SEC + (uint64_t)ts.tv_nsec;
    return (mono / ONE_SEC) * ONE_SEC;
}

struct ev_spec { uint32_t pid; uint32_t ev; uint64_t ms; uint64_t qid; };

static const struct ev_spec slice[] = {
    { 101, EV_LOCK,             20, QID_A },
    { 101, EV_IO,               10, QID_A },
    { 102, EV_PGSLEEP,          30, QID_B },
    /* qid 0: a checkpointer never reports a query id. Giving it one made the
     * #128 deferred per-query resolver file this pacing time under QID_B,
     * which is a fixture artefact rather than anything the product does with
     * a real checkpointer. */
    { 103, EV_CHECKPOINT_DELAY, 40, 0     },
    { 103, EV_VACUUM_DELAY,     15, 0     },
    { 101, EV_CLIENTREAD,       25, QID_A },
    { 104, EV_ACTIVITY,         50, 0     },
};
#define SLICE_N ((int)(sizeof(slice)/sizeof(slice[0])))

#define MAX_EV (SLICES * SLICE_N)

/* Build the event array. `base_mono` is the monotonic origin; events are spread
 * inside each second so no event straddles a boundary (a straddler would make
 * the raw and summary totals legitimately differ and turn this into a tolerance
 * argument instead of an equality). */
static uint64_t g_origin;        /* the base_mono of the last build_stream */

static int build_stream(struct pgwt_trace_event *out, uint64_t base_mono)
{
    g_origin = base_mono;
    int n = 0;
    for (int s = 0; s < SLICES; s++) {
        uint64_t sec = base_mono + (uint64_t)s * ONE_SEC;
        for (int i = 0; i < SLICE_N; i++) {
            struct pgwt_trace_event e;
            memset(&e, 0, sizeof(e));
            /* Each interval ENDS at its timestamp and is duration_ns long, so
             * the timestamps start 100 ms into the second: the longest interval
             * here is 50 ms, so no interval begins before its own second. That
             * matters twice over -- a straddler would be split between two
             * summary seconds while the raw path keeps it whole, and the first
             * second's intervals would otherwise begin BEFORE the window the
             * raw path is asked for and be clipped (54 ms of the 9000, which is
             * how the first version of this file read a 0.6% disagreement). */
            e.timestamp_ns = sec + 100 * MS + (uint64_t)(i + 1) * MS;
            e.pid          = slice[i].pid;
            e.old_event    = slice[i].ev;
            e.new_event    = EV_IO;            /* never EXIT, never a marker */
            e.duration_ns  = slice[i].ms * MS;
            e.query_id     = slice[i].qid;
            e.cpu_ns       = PGWT_CPU_NS_UNKNOWN;
            out[n++] = e;
        }
    }
    return n;
}

/* Write the stream through the real writer. */
static void write_summaries(const char *dir, const struct pgwt_trace_event *ev,
                            int n)
{
    struct pgwt_summary_writer *w = calloc(1, sizeof(*w));
    if (!w) { printf("  FAIL: calloc writer\n"); tests_failed++; return; }
    if (pgwt_summary_writer_init(w, dir, 24, NULL) != 0) {
        printf("  FAIL: writer init on %s\n", dir);
        tests_failed++; free(w); return;
    }
    for (int i = 0; i < n; i++)
        pgwt_summary_push_event(w, &ev[i]);
    pgwt_summary_flush(w);          /* close the last, still-open second */
    pgwt_summary_close(w);
    pgwt_summary_destroy(w);
    free(w);
}

/* ── reading the records back directly (section 2) ─────────────────────── */

struct rec_ctx {
    int      records;
    uint64_t class_ns[PGWT_NUM_CLASSES];
    uint64_t ev_total_ns[8];            /* indexed by `probe[]` below */
    int      idle_top_wait_sessions;    /* sessions whose top wait is idle */
    int      idle_top_wait_queries;
    uint64_t q_total_ns[2];             /* QID_A, QID_B */
    uint64_t q_class_ns[2][PGWT_NUM_CLASSES];
    uint64_t min_sec, max_sec;
};

static const uint32_t probe[] = {
    EV_LOCK, EV_IO, EV_PGSLEEP, EV_CHECKPOINT_DELAY, EV_VACUUM_DELAY,
    EV_CLIENTREAD, EV_ACTIVITY,
};
#define PROBE_N ((int)(sizeof(probe)/sizeof(probe[0])))

static int rec_visitor(const struct pgwt_summary_accum *rec, void *vctx)
{
    struct rec_ctx *c = vctx;
    c->records++;
    if (c->min_sec == 0 || rec->second_wall_ns < c->min_sec)
        c->min_sec = rec->second_wall_ns;
    if (rec->second_wall_ns > c->max_sec)
        c->max_sec = rec->second_wall_ns;
    for (int i = 0; i < PGWT_NUM_CLASSES; i++)
        c->class_ns[i] += rec->class_ns[i];
    for (int e = 0; e < SUMMARY_MAX_EVENTS; e++) {
        const struct pgwt_summary_event *se = &rec->events[e];
        if (se->event_id == 0 && se->count == 0) continue;
        for (int p = 0; p < PROBE_N; p++)
            if (se->event_id == probe[p]) c->ev_total_ns[p] += se->total_ns;
    }
    for (int s = 0; s < SUMMARY_MAX_SESSIONS; s++) {
        const struct pgwt_summary_session *ss = &rec->sessions[s];
        if (ss->pid == 0) continue;
        if (ss->top_wait_id != 0 && pgwt_is_idle_event(ss->top_wait_id))
            c->idle_top_wait_sessions++;
    }
    for (int q = 0; q < SUMMARY_MAX_QUERIES; q++) {
        const struct pgwt_summary_query *sq = &rec->queries[q];
        if (sq->query_id == 0 && sq->count == 0) continue;
        if (sq->top_wait_id != 0 && pgwt_is_idle_event(sq->top_wait_id))
            c->idle_top_wait_queries++;
        int k = (sq->query_id == QID_A) ? 0 : (sq->query_id == QID_B) ? 1 : -1;
        if (k < 0) continue;
        c->q_total_ns[k] += sq->total_ns;
        for (int i = 0; i < PGWT_NUM_CLASSES; i++)
            c->q_class_ns[k][i] += sq->class_ns[i];
    }
    return 0;
}

/* The WALL window the records actually occupy. Derived from the records, never
 * from a clock read in this file: that is what keeps section 1 from silently
 * comparing an empty summary window against a full raw one. */
static int window_from_records(const char *dir, uint64_t *from, uint64_t *to,
                              int *records)
{
    struct rec_ctx c;
    memset(&c, 0, sizeof(c));
    pgwt_visit_summaries(dir, 0, 0, rec_visitor, &c);
    *records = c.records;
    if (c.records == 0) return 0;
    *from = c.min_sec;
    *to   = c.max_sec + ONE_SEC;
    return 1;
}

/* ── 1. raw vs summary ─────────────────────────────────────────────────── */

static const struct pgwt_tm_row *row_at(const struct pgwt_tm_result *tm,
                                        const char *name, int indent)
{
    for (int i = 0; i < tm->num_rows; i++)
        if (strcmp(tm->rows[i].name, name) == 0 && tm->rows[i].indent == indent)
            return &tm->rows[i];
    return NULL;
}

static const struct pgwt_event_row *ev_row(const struct pgwt_events_result *r,
                                           uint32_t eid)
{
    for (int i = 0; i < r->num_rows; i++)
        if (r->rows[i].event_id == eid) return &r->rows[i];
    return NULL;
}

static double aas_total(const struct pgwt_aas_result *a)
{
    double t = 0;
    for (int b = 0; b < a->num_buckets; b++)
        for (int c = 0; c < PGWT_NUM_CLASSES; c++)
            t += a->buckets[b].class_aas[c];
    return t;
}

/* Compare the time model from both paths under one filter. */
static void cmp_time_model(const char *label, const char *dir,
                           const struct pgwt_trace_event *ev, int n,
                           uint64_t from, uint64_t to,
                           const struct pgwt_filter *f, double wall_ms,
                           double want_db_ms)
{
    struct pgwt_tm_result raw, sum;
    pgwt_compute_time_model(ev, n, f, 0, 0, wall_ms, &raw);
    pgwt_compute_time_model_from_summaries(dir, from, to, f, wall_ms, &sum);

    /* NON-VACUITY: both sides must be non-trivial before "they agree" means
     * anything. want_db_ms is read off the fixture, not off either path. */
    CHECK(want_db_ms > 0, "%s: expected DB Time is non-zero by construction",
          label);
    CHECK(NEAR_MS(raw.db_time_ms, want_db_ms),
          "%s: RAW db_time=%.3f expected %.3f", label, raw.db_time_ms,
          want_db_ms);
    CHECK(NEAR_MS(sum.db_time_ms, want_db_ms),
          "%s: SUMMARY db_time=%.3f expected %.3f — the summary fast path is "
          "what the 900 s demo window takes", label, sum.db_time_ms,
          want_db_ms);
    CHECK(NEAR_MS(raw.db_time_ms, sum.db_time_ms),
          "%s: raw %.3f vs summary %.3f", label, raw.db_time_ms,
          sum.db_time_ms);
    CHECK(NEAR_MS(raw.aas, sum.aas),
          "%s: AAS raw %.6f vs summary %.6f", label, raw.aas, sum.aas);
    CHECK(NEAR_MS(raw.idle_time_ms, sum.idle_time_ms),
          "%s: Idle raw %.3f vs summary %.3f", label, raw.idle_time_ms,
          sum.idle_time_ms);

    /* Class rows, one by one: a matching grand total can hide two classes
     * swapping time. */
    const char *classes[] = {"IO", "Lock", "Timeout", "Client"};
    for (int i = 0; i < 4; i++) {
        const struct pgwt_tm_row *r1 = row_at(&raw, classes[i], 1);
        const struct pgwt_tm_row *r2 = row_at(&sum, classes[i], 1);
        double v1 = r1 ? r1->time_ms : 0.0;
        double v2 = r2 ? r2->time_ms : 0.0;
        CHECK(NEAR_MS(v1, v2), "%s: class %s raw %.3f vs summary %.3f",
              label, classes[i], v1, v2);
    }
    free(raw.rows);
    free(sum.rows);
}

static void test_raw_vs_summary(void)
{
    printf("--- 1. raw vs summary over %d s ---\n", SLICES);
    const char *dir = fresh_dir("agree");
    struct pgwt_trace_event *ev = malloc(sizeof(*ev) * MAX_EV);
    int n = build_stream(ev, mono_origin());
    write_summaries(dir, ev, n);

    uint64_t from = 0, to = 0;
    int records = 0;
    CHECK(window_from_records(dir, &from, &to, &records) == 1,
          "the writer produced summary records to read");
    CHECK(records == SLICES,
          "%d seconds on disk, expected %d — a short read would make every "
          "comparison below compare two smaller numbers", records, SLICES);
    double wall_ms = (double)(to - from) / 1e6;
    CHECK(to - from >= 120ULL * ONE_SEC,
          "the window is >= 120 s, i.e. the shape that actually takes the "
          "summary path in pgwt-server (got %.1f s)",
          (double)(to - from) / 1e9);

    /* 1a. UNFILTERED. */
    struct pgwt_filter f;
    memset(&f, 0, sizeof(f));
    cmp_time_model("unfiltered", dir, ev, n, from, to, &f, wall_ms,
                   PER_SEC_DB_MS * SLICES);

    /* The number this whole change is about, stated explicitly: with the
     * pacing sleeps still counted the summary path would report
     * 9000 + 40*150 + 15*150 = 17250 ms. */
    struct pgwt_tm_result sum;
    pgwt_compute_time_model_from_summaries(dir, from, to, &f, wall_ms, &sum);
    CHECK(!NEAR_MS(sum.db_time_ms, 17250.0),
          "summary db_time must NOT be the old 17250 ms (got %.3f)",
          sum.db_time_ms);
    CHECK(NEAR_MS(sum.idle_time_ms, PER_SEC_IDLE_MS * SLICES),
          "summary Idle = %.1f ms expected %.1f", sum.idle_time_ms,
          PER_SEC_IDLE_MS * SLICES);
    free(sum.rows);

    /* 1b. QUERY FILTER — a different branch in both visitors (per-query
     * class_ns + top_events, not the system-wide tables).
     * QID_A = Lock 20 + IO 10 => 30 ms/s DB Time, plus ClientRead 25 ms/s idle.
     * QID_B = PgSleep 30 => 30 ms/s DB Time, no idle (the pacing sleeps belong
     *         to the checkpointer, which has no query id). */
    memset(&f, 0, sizeof(f));
    f.query_id = QID_A;
    cmp_time_model("query A", dir, ev, n, from, to, &f, wall_ms,
                   30.0 * SLICES);
    memset(&f, 0, sizeof(f));
    f.query_id = QID_B;
    cmp_time_model("query B", dir, ev, n, from, to, &f, wall_ms,
                   30.0 * SLICES);

    /* 1c. TIMEOUT CLASS FILTER — the third branch (per-event table only).
     * Only PgSleep survives the idle rule: 30 ms/s. If the pacing events were
     * still load this would read 85 ms/s. */
    memset(&f, 0, sizeof(f));
    snprintf(f.class_name, sizeof(f.class_name), "timeout");
    cmp_time_model("class=timeout", dir, ev, n, from, to, &f, wall_ms,
                   30.0 * SLICES);

    /* 1d. AAS, all three filters. */
    const struct pgwt_filter aas_filters[3] = {
        { .class_name = "", .event_id = 0, .pid = 0, .query_id = 0 },
        { .class_name = "", .event_id = 0, .pid = 0, .query_id = QID_A },
        { .class_name = "timeout", .event_id = 0, .pid = 0, .query_id = 0 },
    };
    const char *aas_labels[3] = { "unfiltered", "query A", "class=timeout" };
    /* The raw events carry MONOTONIC timestamps; the summaries were converted
     * to WALL by the writer. So the two calls take DIFFERENT windows that cover
     * the same data, and the comparison is of the AGGREGATE AAS (the sum over
     * buckets), which is clock-free. Both totals are asserted non-zero first,
     * so a clock mismatch shows up as "summary total is zero" rather than as
     * 0 == 0 agreement -- which is exactly how the first version of this
     * section passed while comparing nothing. */
    uint64_t raw_from = g_origin;
    uint64_t raw_to   = g_origin + (uint64_t)SLICES * ONE_SEC;
    for (int i = 0; i < 3; i++) {
        struct pgwt_aas_result ra, sa;
        pgwt_compute_aas(ev, n, &aas_filters[i], raw_from, raw_to, SLICES, 0, 0,
                         &ra);
        pgwt_compute_aas_from_summaries(dir, from, to, &aas_filters[i], SLICES,
                                        &sa);
        double t_raw = aas_total(&ra), t_sum = aas_total(&sa);
        CHECK(t_raw > 0.0, "AAS[%s]: raw total is non-zero (%.6f)",
              aas_labels[i], t_raw);
        CHECK(t_sum > 0.0, "AAS[%s]: summary total is non-zero (%.6f)",
              aas_labels[i], t_sum);
        CHECK(NEAR_MS(t_raw, t_sum),
              "AAS[%s]: raw total %.6f vs summary total %.6f", aas_labels[i],
              t_raw, t_sum);
        free(ra.buckets);
        free(sa.buckets);
    }

    /* 1e. VISIBLE EVENT ROWS. The pacing events must still be listed by the
     * summary path, with their full time and the idle %DB sentinel. */
    memset(&f, 0, sizeof(f));
    struct pgwt_events_result re, se;
    pgwt_compute_top_events(ev, n, &f, 0, 0, wall_ms, &re);
    pgwt_compute_top_events_from_summaries(dir, from, to, &f, wall_ms, &se);
    for (int p = 0; p < PROBE_N; p++) {
        const struct pgwt_event_row *a = ev_row(&re, probe[p]);
        const struct pgwt_event_row *b = ev_row(&se, probe[p]);
        if (pgwt_is_hidden_event(probe[p])) {
            CHECK(a == NULL && b == NULL,
                  "hidden event 0x%08x listed by neither path", probe[p]);
            continue;
        }
        CHECK(a != NULL && b != NULL,
              "event 0x%08x listed by BOTH paths (raw=%p summary=%p)",
              probe[p], (const void *)a, (const void *)b);
        if (a && b) {
            CHECK(NEAR_MS(a->total_ms, b->total_ms),
                  "event 0x%08x total: raw %.3f vs summary %.3f", probe[p],
                  a->total_ms, b->total_ms);
            CHECK(PGWT_PCT_DB_IS_IDLE(a->pct_db) ==
                  PGWT_PCT_DB_IS_IDLE(b->pct_db),
                  "event 0x%08x idle-sentinel agreement (raw %.2f summary "
                  "%.2f)", probe[p], a->pct_db, b->pct_db);
            CHECK(PGWT_PCT_DB_IS_IDLE(a->pct_db) ==
                  (pgwt_is_idle_event(probe[p]) != 0),
                  "event 0x%08x sentinel matches the predicate", probe[p]);
        }
    }
    CHECK(NEAR_MS(re.db_time_ms, se.db_time_ms),
          "top_events db_time: raw %.3f vs summary %.3f", re.db_time_ms,
          se.db_time_ms);
    free(re.rows);
    free(se.rows);

    /* 1f. SESSIONS. pid 103 does nothing but pacing sleeps and pid 104 nothing
     * but Activity, so both must show zero DB Time. pid 101 and 102 must
     * agree between the paths. */
    struct pgwt_sessions_result rs, ss;
    pgwt_compute_top_sessions(ev, n, &f, 0, 0, wall_ms, &rs);
    pgwt_compute_top_sessions_from_summaries(dir, from, to, &f, wall_ms, &ss);
    for (uint32_t pid = 101; pid <= 104; pid++) {
        double v1 = 0, v2 = 0;
        for (int i = 0; i < rs.num_rows; i++)
            if (rs.rows[i].pid == pid) v1 = rs.rows[i].db_time_ms;
        for (int i = 0; i < ss.num_rows; i++)
            if (ss.rows[i].pid == pid) v2 = ss.rows[i].db_time_ms;
        CHECK(NEAR_MS(v1, v2), "session pid %u db_time: raw %.3f vs summary "
              "%.3f", pid, v1, v2);
        if (pid == 103 || pid == 104)
            CHECK(NEAR_MS(v2, 0.0),
                  "pid %u does nothing but sleep/park: summary db_time must be "
                  "0, got %.3f", pid, v2);
    }
    CHECK(rs.num_rows > 0 && ss.num_rows > 0,
          "both paths returned sessions (raw %d, summary %d)", rs.num_rows,
          ss.num_rows);
    free(rs.rows);
    free(ss.rows);

    /* 1g. QUERIES. The per-query totals used to include idle on the summary
     * path and exclude it on the raw path, so the SAME query reported a
     * different total depending on which path answered. */
    struct pgwt_queries_result rq, sq;
    pgwt_compute_top_queries(ev, n, &f, 0, 0, wall_ms, &rq);
    pgwt_compute_top_queries_from_summaries(dir, from, to, &f, wall_ms, &sq);
    uint64_t qids[2] = { QID_A, QID_B };
    double want_q[2] = { 30.0 * SLICES, 30.0 * SLICES };
    for (int k = 0; k < 2; k++) {
        double v1 = -1, v2 = -1;
        for (int i = 0; i < rq.num_rows; i++)
            if (rq.rows[i].query_id == qids[k]) v1 = rq.rows[i].total_ms;
        for (int i = 0; i < sq.num_rows; i++)
            if (sq.rows[i].query_id == qids[k]) v2 = sq.rows[i].total_ms;
        CHECK(v1 >= 0 && v2 >= 0, "query %d present in both paths", k);
        CHECK(NEAR_MS(v1, want_q[k]),
              "query %d RAW total %.3f expected %.3f", k, v1, want_q[k]);
        CHECK(NEAR_MS(v2, want_q[k]),
              "query %d SUMMARY total %.3f expected %.3f (idle used to be "
              "included here and not in raw)", k, v2, want_q[k]);
    }
    free(rq.rows);
    free(sq.rows);
    free(ev);
}

/* ── 2. the writer-side exclusion, read off the records ────────────────── */
static void test_writer_excludes_idle(void)
{
    printf("--- 2. v3 writer: class_ns has no idle, events[] has all ---\n");
    const char *dir = fresh_dir("writer");
    struct pgwt_trace_event *ev = malloc(sizeof(*ev) * MAX_EV);
    int n = build_stream(ev, mono_origin());
    write_summaries(dir, ev, n);

    struct rec_ctx c;
    memset(&c, 0, sizeof(c));
    pgwt_visit_summaries(dir, 0, 0, rec_visitor, &c);
    CHECK(c.records == SLICES, "%d records read, expected %d", c.records,
          SLICES);

    /* class_ns: Timeout holds PgSleep ONLY, Client holds nothing (ClientRead
     * was its only Client event), Activity holds nothing. */
    CHECK(c.class_ns[PGWT_CLASS_TIMEOUT] == 30ULL * MS * SLICES,
          "class_ns[TIMEOUT] = %llu ns, expected %llu (PgSleep only; 85 ms/s "
          "would mean the pacing sleeps are still in there)",
          (unsigned long long)c.class_ns[PGWT_CLASS_TIMEOUT],
          (unsigned long long)(30ULL * MS * SLICES));
    CHECK(c.class_ns[PGWT_CLASS_CLIENT] == 0,
          "class_ns[CLIENT] = %llu ns, expected 0 (ClientRead excluded at the "
          "writer)", (unsigned long long)c.class_ns[PGWT_CLASS_CLIENT]);
    CHECK(c.class_ns[PGWT_CLASS_ACTIVITY] == 0,
          "class_ns[ACTIVITY] = %llu ns, expected 0",
          (unsigned long long)c.class_ns[PGWT_CLASS_ACTIVITY]);
    CHECK(c.class_ns[PGWT_CLASS_LOCK] == 20ULL * MS * SLICES,
          "class_ns[LOCK] unchanged at %llu ns",
          (unsigned long long)c.class_ns[PGWT_CLASS_LOCK]);

    /* events[]: EVERY event, including the idle ones, with full time. This is
     * where the idle total and the named Idle sub-rows come from, so if this
     * were pruned the time would vanish instead of moving. */
    const uint64_t want_ev[PROBE_N] = {
        20ULL * MS * SLICES, 10ULL * MS * SLICES, 30ULL * MS * SLICES,
        40ULL * MS * SLICES, 15ULL * MS * SLICES, 25ULL * MS * SLICES,
        50ULL * MS * SLICES,
    };
    for (int p = 0; p < PROBE_N; p++)
        CHECK(c.ev_total_ns[p] == want_ev[p],
              "events[0x%08x] = %llu ns, expected %llu", probe[p],
              (unsigned long long)c.ev_total_ns[p],
              (unsigned long long)want_ev[p]);

    /* TOP WAIT must never be an idle event. pid 103's only waits are pacing
     * sleeps, so a selection that allowed idle would label it
     * "Timeout:CheckpointWriteDelay" — naming the thing that is explicitly not
     * load as the session's biggest problem. */
    CHECK(c.idle_top_wait_sessions == 0,
          "%d session records chose an idle top wait, expected 0",
          c.idle_top_wait_sessions);
    CHECK(c.idle_top_wait_queries == 0,
          "%d query records chose an idle top wait, expected 0",
          c.idle_top_wait_queries);

    /* Per-query totals exclude idle at the writer too. */
    CHECK(c.q_total_ns[0] == 30ULL * MS * SLICES,
          "queries[QID_A].total_ns = %llu, expected %llu (ClientRead's 25 ms/s "
          "excluded)", (unsigned long long)c.q_total_ns[0],
          (unsigned long long)(30ULL * MS * SLICES));
    CHECK(c.q_total_ns[1] == 30ULL * MS * SLICES,
          "queries[QID_B].total_ns = %llu, expected %llu "
          "(CheckpointWriteDelay's 40 ms/s excluded)",
          (unsigned long long)c.q_total_ns[1],
          (unsigned long long)(30ULL * MS * SLICES));
    CHECK(c.q_class_ns[1][PGWT_CLASS_TIMEOUT] == 30ULL * MS * SLICES,
          "queries[QID_B].class_ns[TIMEOUT] = %llu, expected %llu",
          (unsigned long long)c.q_class_ns[1][PGWT_CLASS_TIMEOUT],
          (unsigned long long)(30ULL * MS * SLICES));
    CHECK(c.q_class_ns[0][PGWT_CLASS_CLIENT] == 0,
          "queries[QID_A].class_ns[CLIENT] = %llu, expected 0",
          (unsigned long long)c.q_class_ns[0][PGWT_CLASS_CLIENT]);
    free(ev);
}

/* ── 3. the Idle row on the summary path ───────────────────────────────── */
static void test_summary_idle_rows(void)
{
    printf("--- 3. Idle row + named children on the summary path ---\n");
    const char *dir = fresh_dir("idlerows");
    struct pgwt_trace_event *ev = malloc(sizeof(*ev) * MAX_EV);
    int n = build_stream(ev, mono_origin());
    write_summaries(dir, ev, n);

    uint64_t from = 0, to = 0;
    int records = 0;
    CHECK(window_from_records(dir, &from, &to, &records) == 1 &&
          records == SLICES, "records present (%d)", records);
    double wall_ms = (double)(to - from) / 1e6;

    struct pgwt_filter f;
    memset(&f, 0, sizeof(f));
    struct pgwt_tm_result tm;
    pgwt_compute_time_model_from_summaries(dir, from, to, &f, wall_ms, &tm);

    const struct pgwt_tm_row *idle = row_at(&tm, "Idle", 0);
    CHECK(idle != NULL, "an indent-0 Idle row on the summary path too");
    if (idle)
        CHECK(NEAR_MS(idle->time_ms, PER_SEC_IDLE_MS * SLICES),
              "Idle row = %.1f ms expected %.1f", idle->time_ms,
              PER_SEC_IDLE_MS * SLICES);
    struct { const char *name; double ms; } kids[] = {
        { "Activity:CheckpointerMain",       0.0 },   /* hidden: no row */
        { "Timeout:CheckpointWriteDelay", 40.0 * SLICES },
        { "Client:ClientRead",            25.0 * SLICES },
        { "Timeout:VacuumDelay",          15.0 * SLICES },
    };
    CHECK(row_at(&tm, kids[0].name, 2) == NULL,
          "the hidden Activity event gets no child row");
    for (int i = 1; i < 4; i++) {
        const struct pgwt_tm_row *r = row_at(&tm, kids[i].name, 2);
        CHECK(r != NULL && NEAR_MS(r->time_ms, kids[i].ms),
              "named child %s = %.1f ms expected %.1f", kids[i].name,
              r ? r->time_ms : -1, kids[i].ms);
    }
    /* Same structural guards as the raw path: no indent-1 Idle row (it would
     * break demo_rehearsal_lib.time_model_conservation), rows[0] is DB Time. */
    CHECK(row_at(&tm, "Idle", 1) == NULL, "no indent-1 Idle row");
    CHECK(tm.num_rows > 0 && strcmp(tm.rows[0].name, "DB Time") == 0,
          "rows[0] is still DB Time");
    double class_sum = 0;
    for (int i = 0; i < tm.num_rows; i++)
        if (tm.rows[i].indent == 1) class_sum += tm.rows[i].time_ms;
    CHECK(NEAR_MS(class_sum, tm.db_time_ms),
          "conservation on the summary path: sum(indent==1)=%.3f vs "
          "db_time=%.3f", class_sum, tm.db_time_ms);
    free(tm.rows);

    /* Under a CLASS FILTER the Idle row describes the FILTERED idle, because
     * that is what the raw path does (it applies pgwt_filter_matches before its
     * own idle branch). The two paths must agree, filter or no filter --
     * cross_validate compares them. class=timeout => CheckpointWriteDelay
     * 40 ms/s + VacuumDelay 15 ms/s = 55 ms/s, and NOT the 130 ms/s the
     * unfiltered window reports. */
    memset(&f, 0, sizeof(f));
    snprintf(f.class_name, sizeof(f.class_name), "timeout");
    struct pgwt_tm_result rawtm;
    pgwt_compute_time_model(ev, n, &f, 0, 0, wall_ms, &rawtm);
    pgwt_compute_time_model_from_summaries(dir, from, to, &f, wall_ms, &tm);
    CHECK(NEAR_MS(tm.idle_time_ms, 55.0 * SLICES),
          "class=timeout: summary Idle = %.1f expected %.1f",
          tm.idle_time_ms, 55.0 * SLICES);
    CHECK(NEAR_MS(rawtm.idle_time_ms, tm.idle_time_ms),
          "class=timeout: raw Idle %.3f vs summary Idle %.3f",
          rawtm.idle_time_ms, tm.idle_time_ms);
    CHECK(!NEAR_MS(tm.idle_time_ms, PER_SEC_IDLE_MS * SLICES),
          "...and it is NOT the unfiltered total, so the filter is applied");
    CHECK(row_at(&tm, "Client:ClientRead", 2) == NULL,
          "class=timeout: ClientRead is filtered out of the Idle children");
    free(rawtm.rows);
    free(tm.rows);
    free(ev);
}

/* ── 4. THE VERSION PREFLIGHT ──────────────────────────────────────────── */

/* Rewrite the version byte of every *.summary / current.summary file in `dir`.
 * Returns the number of files touched. */
static int set_version(const char *dir, uint32_t version)
{
    DIR *d = opendir(dir);
    if (!d) return -1;
    struct dirent *e;
    int n = 0;
    while ((e = readdir(d))) {
        size_t len = strlen(e->d_name);
        if (len < 8 || strcmp(e->d_name + len - 8, ".summary") != 0)
            continue;
        char path[700];
        snprintf(path, sizeof(path), "%s/%s", dir, e->d_name);
        FILE *fp = fopen(path, "r+b");
        if (!fp) continue;
        struct pgwt_trace_file_header hdr;
        if (fread(&hdr, sizeof(hdr), 1, fp) == 1 &&
            hdr.magic == PGWT_SUMMARY_MAGIC) {
            hdr.version = version;
            if (fseek(fp, 0, SEEK_SET) == 0 &&
                fwrite(&hdr, sizeof(hdr), 1, fp) == 1)
                n++;
        }
        fclose(fp);
    }
    closedir(d);
    return n;
}

static void test_version_preflight(void)
{
    printf("--- 4. accounting-version preflight ---\n");
    const char *dir = fresh_dir("version");
    struct pgwt_trace_event *ev = malloc(sizeof(*ev) * MAX_EV);
    int n = build_stream(ev, mono_origin());
    write_summaries(dir, ev, n);

    uint64_t from = 0, to = 0;
    int records = 0;
    CHECK(window_from_records(dir, &from, &to, &records) == 1 &&
          records == SLICES, "v3 records present (%d)", records);

    int considered = -1, unusable = -1;
    CHECK(pgwt_summaries_window_current(dir, from, to, &considered,
                                        &unusable) == 1,
          "a freshly written window is current");
    CHECK(considered > 0,
          "...and it CONSIDERED %d file(s) — a gate that looked at nothing "
          "would also have returned 1", considered);
    CHECK(unusable == 0, "no unusable files (got %d)", unusable);

    /* Now make it a v2 window: the pre-2026-10-06 accounting. The preflight
     * must refuse, because mixing v2 seconds with v3 seconds inside one window
     * blends two accounting rules and reports a DB Time that is neither. */
    int touched = set_version(dir, 2);
    CHECK(touched > 0, "rewrote %d header(s) to version 2", touched);
    CHECK(pgwt_summaries_window_current(dir, from, to, &considered,
                                        &unusable) == 0,
          "a v2 window is REFUSED");
    CHECK(unusable == touched,
          "all %d v2 file(s) reported unusable (got %d)", touched, unusable);

    /* AND THE REASON THE PREFLIGHT HAS TO EXIST: the visitor SKIPS files it
     * cannot open, so without the preflight this window reads as a plausible
     * EMPTY answer rather than an error. Pin that shape explicitly, so nobody
     * "simplifies" the preflight away on the grounds that the reader already
     * refuses. */
    struct rec_ctx c;
    memset(&c, 0, sizeof(c));
    pgwt_visit_summaries(dir, 0, 0, rec_visitor, &c);
    CHECK(c.records == 0,
          "the visitor skipped every v2 file and returned %d records — a "
          "plausible-looking EMPTY window, which is why refusing in the reader "
          "alone is not enough", c.records);
    struct pgwt_filter f;
    memset(&f, 0, sizeof(f));
    struct pgwt_tm_result tm;
    pgwt_compute_time_model_from_summaries(dir, from, to, &f, 1000.0, &tm);
    CHECK(tm.db_time_ms == 0.0,
          "...and the summary time model over that window answers 0.000 ms "
          "with no error (got %.3f) — the exact silent-wrong-answer the "
          "preflight prevents by routing to raw", tm.db_time_ms);
    free(tm.rows);

    /* Back to v3: the preflight must APPROVE again, so it is reacting to the
     * version and not permanently stuck. */
    CHECK(set_version(dir, PGWT_SUMMARY_VERSION) == touched,
          "restored headers to v%d", PGWT_SUMMARY_VERSION);
    CHECK(pgwt_summaries_window_current(dir, from, to, &considered,
                                        &unusable) == 1,
          "the restored v3 window is current again");
    memset(&c, 0, sizeof(c));
    pgwt_visit_summaries(dir, 0, 0, rec_visitor, &c);
    CHECK(c.records == SLICES,
          "and the visitor reads all %d seconds again (got %d)", SLICES,
          c.records);
    free(ev);
}

/* ══ 5. FALSE NEGATIVES ═══════════════════════════════════════════════════
 * Section 1's agreement check is satisfied by 0 == 0 and section 4's gate is
 * satisfied by "there were no files". These are the ways each can be made
 * unable to see. */
static void test_bypass_suite(void)
{
    printf("--- 5. bypass suite ---\n");
    struct pgwt_filter f;
    memset(&f, 0, sizeof(f));

    /* 5a. NO DIRECTORY AT ALL. The scan fails, so the preflight must REFUSE —
     * "I could not look" is never "all clear". */
    int considered = -1, unusable = -1;
    char missing[600];
    snprintf(missing, sizeof(missing), "%s/does-not-exist", g_base);
    CHECK(pgwt_summaries_window_current(missing, 1, 2, &considered,
                                        &unusable) == 0,
          "a missing trace dir is REFUSED, not approved");

    /* 5b. AN EMPTY DIRECTORY. Genuinely different from 5a: there is nothing to
     * blend, so approving is correct — but *considered must be 0 so the caller
     * can tell "all good" from "nothing there". Without that counter a gate
     * that silently looked at no files would be indistinguishable from a
     * passing one, which is the defect shape this project keeps finding. */
    const char *empty = fresh_dir("empty");
    considered = -1; unusable = -1;
    CHECK(pgwt_summaries_window_current(empty, 1, 2, &considered,
                                        &unusable) == 1,
          "an empty dir has nothing to blend: approved");
    CHECK(considered == 0 && unusable == 0,
          "...and it reports considered=%d so the caller can see it examined "
          "nothing", considered);

    /* 5c. A WRITER THAT WROTE NOTHING. The files exist (header + footer) but
     * carry no seconds, so an agreement comparison over them is 0 == 0. Pin
     * that the record count is 0, which is what makes section 1's
     * `records == SLICES` assertion load-bearing. */
    const char *nowt = fresh_dir("nowrite");
    struct pgwt_summary_writer *w = calloc(1, sizeof(*w));
    CHECK(w != NULL && pgwt_summary_writer_init(w, nowt, 24, NULL) == 0,
          "writer opens on an empty dir");
    if (w) { pgwt_summary_close(w); pgwt_summary_destroy(w); free(w); }
    struct rec_ctx c;
    memset(&c, 0, sizeof(c));
    pgwt_visit_summaries(nowt, 0, 0, rec_visitor, &c);
    CHECK(c.records == 0,
          "a writer that wrote nothing yields 0 records (got %d) — an "
          "agreement test over this window would be 0 == 0", c.records);
    uint64_t from = 0, to = 0;
    int records = 0;
    CHECK(window_from_records(nowt, &from, &to, &records) == 0,
          "...and window_from_records REFUSES to invent a window for it");

    /* 5d. A TRUNCATED HEADER. The file exists and the name matches, but the
     * header cannot be read. It must count as UNUSABLE, not as absent: the
     * visitor would skip it and answer from the remaining files. */
    const char *trunc = fresh_dir("trunc");
    struct pgwt_trace_event *ev = malloc(sizeof(*ev) * MAX_EV);
    int n = build_stream(ev, mono_origin());
    write_summaries(trunc, ev, n);
    CHECK(window_from_records(trunc, &from, &to, &records) == 1 &&
          records == SLICES, "trunc fixture starts healthy (%d records)",
          records);
    {
        DIR *d = opendir(trunc);
        int cut = 0;
        struct dirent *e;
        while (d && (e = readdir(d))) {
            size_t len = strlen(e->d_name);
            if (len < 8 || strcmp(e->d_name + len - 8, ".summary") != 0)
                continue;
            char path[700];
            snprintf(path, sizeof(path), "%s/%s", trunc, e->d_name);
            if (truncate(path, 8) == 0) cut++;
        }
        if (d) closedir(d);
        CHECK(cut > 0, "truncated %d summary file(s)", cut);
    }
    considered = -1; unusable = -1;
    CHECK(pgwt_summaries_window_current(trunc, from, to, &considered,
                                        &unusable) == 0,
          "a truncated-header file is REFUSED");
    CHECK(unusable > 0, "...and reported as unusable (got %d)", unusable);

    /* 5e. A WINDOW WITH NO OVERLAP. Healthy v3 files, but the requested window
     * is somewhere else entirely. Approving is right (nothing to blend) and
     * considered must be 0 — otherwise the gate would be claiming to have
     * checked files it never opened. */
    const char *ok = fresh_dir("overlap");
    n = build_stream(ev, mono_origin());
    write_summaries(ok, ev, n);
    CHECK(window_from_records(ok, &from, &to, &records) == 1, "overlap fixture");
    considered = -1; unusable = -1;
    /* Two hours before anything was written: the file's hour cannot overlap. */
    uint64_t far_to = (from > 7200ULL * ONE_SEC) ? from - 7200ULL * ONE_SEC : 1;
    CHECK(pgwt_summaries_window_current(ok, far_to - 1000, far_to,
                                        &considered, &unusable) == 1,
          "a window with no overlapping file is approved");
    CHECK(considered == 0,
          "...with considered=%d, so 'approved' cannot be confused with "
          "'checked the data'", considered);

    /* 5f. THE THING BEING CHECKED IS ABSENT. A stream with no Timeout events
     * at all makes every "pacing is excluded" assertion in sections 1-3
     * vacuously true. Pin that such a window is DISTINGUISHABLE: its Timeout
     * class is empty and its Idle total comes only from ClientRead. A test
     * built on this fixture could not detect the bug, which is why section 1
     * asserts its expected DB Time from the fixture rather than comparing the
     * two paths alone. */
    const char *nopace = fresh_dir("nopacing");
    {
        uint64_t origin = mono_origin();
        struct pgwt_summary_writer *ww = calloc(1, sizeof(*ww));
        CHECK(ww != NULL && pgwt_summary_writer_init(ww, nopace, 24, NULL) == 0,
              "writer opens for the no-pacing fixture");
        for (int s = 0; s < SLICES && ww; s++) {
            uint64_t sec = origin + (uint64_t)s * ONE_SEC;
            struct pgwt_trace_event e;
            memset(&e, 0, sizeof(e));
            e.new_event = EV_IO; e.cpu_ns = PGWT_CPU_NS_UNKNOWN;
            e.timestamp_ns = sec + 1 * MS; e.pid = 101;
            e.old_event = EV_LOCK; e.duration_ns = 20 * MS;
            pgwt_summary_push_event(ww, &e);
            e.timestamp_ns = sec + 2 * MS;
            e.old_event = EV_CLIENTREAD; e.duration_ns = 25 * MS;
            pgwt_summary_push_event(ww, &e);
        }
        if (ww) { pgwt_summary_flush(ww); pgwt_summary_close(ww);
                  pgwt_summary_destroy(ww); free(ww); }
    }
    CHECK(window_from_records(nopace, &from, &to, &records) == 1 &&
          records == SLICES, "no-pacing fixture has %d records", records);
    {
        struct pgwt_tm_result tm;
        double wall_ms = (double)(to - from) / 1e6;
        pgwt_compute_time_model_from_summaries(nopace, from, to, &f, wall_ms,
                                               &tm);
        CHECK(row_at(&tm, "Timeout", 1) == NULL,
              "no-pacing fixture has no Timeout class row — it cannot "
              "detect anything about pacing");
        CHECK(NEAR_MS(tm.db_time_ms, 20.0 * SLICES),
              "no-pacing db_time = %.1f expected %.1f", tm.db_time_ms,
              20.0 * SLICES);
        CHECK(NEAR_MS(tm.idle_time_ms, 25.0 * SLICES),
              "no-pacing Idle = %.1f (ClientRead only) expected %.1f",
              tm.idle_time_ms, 25.0 * SLICES);
        CHECK(row_at(&tm, "Timeout:CheckpointWriteDelay", 2) == NULL,
              "and no pacing child row");
        free(tm.rows);
    }

    /* 5g. THE MASK IS EMPTY. If the pacing mask were ever installed as 0 (the
     * PG13 failure mode of a hardcoded id list), the WRITER would put the
     * pacing time back into class_ns and the summary path would silently
     * report the old number. Drive it directly so the failure has a name and a
     * value, then restore and prove the restore worked. */
    uint32_t saved = pgwt_idle_rule_timeout_mask();
    pgwt_idle_rule_set_timeout_mask(0);
    const char *badmask = fresh_dir("badmask");
    n = build_stream(ev, mono_origin());
    write_summaries(badmask, ev, n);
    pgwt_idle_rule_set_timeout_mask(saved);
    CHECK(pgwt_idle_rule_timeout_mask() == saved, "mask restored");
    CHECK(window_from_records(badmask, &from, &to, &records) == 1 &&
          records == SLICES, "bad-mask fixture written (%d records)", records);
    {
        memset(&c, 0, sizeof(c));
        pgwt_visit_summaries(badmask, 0, 0, rec_visitor, &c);
        CHECK(c.class_ns[PGWT_CLASS_TIMEOUT] == 85ULL * MS * SLICES,
              "with an empty mask the writer puts all %llu ns of Timeout time "
              "into class_ns (got %llu) — this is the number the old lumped "
              "writer produced", (unsigned long long)(85ULL * MS * SLICES),
              (unsigned long long)c.class_ns[PGWT_CLASS_TIMEOUT]);
        CHECK(c.class_ns[PGWT_CLASS_TIMEOUT] != 30ULL * MS * SLICES,
              "and it differs from the correct value, so section 2 can go red");
    }
    free(ev);
}

int main(void)
{
    printf("=== test_idle_summary ===\n");
    pgwt_init_event_names(18);
    if (pgwt_idle_rule_timeout_mask() != PGWT_IDLE_TIMEOUT_MASK_PG18) {
        printf("REFUSING: PG18 tables not active (mask 0x%03x) — this file's "
               "event ids would mean something else\n",
               pgwt_idle_rule_timeout_mask());
        return 1;
    }
    snprintf(g_base, sizeof(g_base), "/tmp/pgwt_idle_summary_%d", (int)getpid());
    char cmd[400];
    snprintf(cmd, sizeof(cmd), "rm -rf '%s'", g_base);
    if (system(cmd) != 0) { /* ignore */ }
    if (mkdir(g_base, 0755) != 0) {
        printf("REFUSING: cannot create %s\n", g_base);
        return 1;
    }

    test_raw_vs_summary();
    test_writer_excludes_idle();
    test_summary_idle_rows();
    test_version_preflight();
    test_bypass_suite();

    snprintf(cmd, sizeof(cmd), "rm -rf '%s'", g_base);
    if (system(cmd) != 0) { /* ignore */ }

    printf("\n%d checks, %d failed\n", tests_run, tests_failed);
    return tests_failed == 0 ? 0 : 1;
}
